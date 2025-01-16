#include "ftl.h"

//#define FEMU_DEBUG_FTL

static void *ftl_thread(void *arg);

// 用於正常情況下的垃圾回收(0.75)
static inline bool should_gc(struct ssd *ssd)
{
    return (ssd->lm.free_line_cnt <= ssd->sp.gc_thres_lines);
}

// 用於系統資源更加限制或高負載時，作為更嚴格的條件來觸發垃圾回收(0.95)
static inline bool should_gc_high(struct ssd *ssd)
{
    return (ssd->lm.free_line_cnt <= ssd->sp.gc_thres_lines_high);
}

// 取得資料(logical)的記憶體位置(physical)
static inline struct ppa get_maptbl_ent_subpage(struct ssd *ssd, uint64_t data)
{
    return ssd->maptbl2[data];
}

// 將subblock的原始位置(channel、lun、plane、block)轉換為subblock ID(1 ~ Total Number of Subblock in SSD)
static uint64_t sblk2sblkidx(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    uint64_t sblkidx;

    sblkidx = ppa->g.ch  * spp->blks_per_ch * spp->sblks_per_blk + \
              ppa->g.lun * spp->blks_per_lun * spp->sblks_per_blk+ \
              ppa->g.pl  * spp->blks_per_pl * spp->sblks_per_blk  + \
              ppa->g.blk * spp->sblks_per_blk + \
              ppa->g.sblk;

    return sblkidx;
}
// 將subblock ID(1 ~ Total Number of Subblock in SSD)轉換為subblock的原始位置(channel、lun、plane、block)
static struct ppa sblkidx2sblk(struct ssd *ssd, uint64_t sblkidx)
{
    struct ssdparams *spp = &ssd->sp;
    struct ppa ppa;

    ppa.g.ch = sblkidx / (spp->blks_per_ch * spp->sblks_per_blk);
    sblkidx -= ppa.g.ch * spp->blks_per_ch * spp->sblks_per_blk;
    ppa.g.lun = sblkidx / (spp->blks_per_lun * spp->sblks_per_blk);
    sblkidx -= ppa.g.lun * spp->blks_per_lun * spp->sblks_per_blk;
    ppa.g.pl = sblkidx / (spp->blks_per_pl * spp->sblks_per_blk);
    sblkidx -= ppa.g.pl * spp->blks_per_pl * spp->sblks_per_blk;
    ppa.g.blk = sblkidx / spp->sblks_per_blk;
    sblkidx -= ppa.g.blk * spp->sblks_per_blk;
    ppa.g.sblk = sblkidx;

    return ppa;
}

// 將subpage的原始位置(block、subblock、page)轉換為subpage ID(1 ~ Total Number of Subpage in SSD)
static uint64_t ppa2pgidx(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    uint64_t pgidx;

    pgidx = ppa->g.ch  * spp->secs_per_ch  + \
            ppa->g.lun * spp->secs_per_lun + \
            ppa->g.pl  * spp->secs_per_pl  + \
            ppa->g.blk * spp->secs_per_blk + \
            ppa->g.sblk * spp->secs_per_sblk + \
            ppa->g.pg * spp->secs_per_pg + \
            ppa->g.sec;

    ftl_assert(pgidx < spp->tt_pgs * spp->secs_per_pg);

    return pgidx;
}

// 取得subpage對應的資料
static inline uint64_t get_rmap_ent(struct ssd *ssd, struct ppa *ppa)
{
    uint64_t pgidx = ppa2pgidx(ssd, ppa);

    return ssd->rmap[pgidx];
}

// 設定subpage對應的資料
static inline void set_rmap_ent(struct ssd *ssd, uint64_t lpn, struct ppa *ppa)
{
    uint64_t pgidx = ppa2pgidx(ssd, ppa);
    ssd->rmap[pgidx] = lpn;
}

// 1) 設定資料對應的subpage 2) 設定subpage對應的資料
static inline void set_maptbl_ent_subpage(struct ssd *ssd, struct ppa *ppa, uint64_t data)
{
    ftl_assert(lpn < ssd->sp.tt_pgs); 

    ssd->maptbl2[data] = *ppa; // Index => Logical, value => Physical
    uint64_t ppaidx = ppa2pgidx(ssd, ppa); // struct ppa => physical number
    ssd->rmap[ppaidx] = data; // Index => physical number, value => Logical
}

// 初始化SSD中的指標(記錄目前寫入位置)
static void ssd_init_write_pointer(struct ssd *ssd)
{
    struct write_pointer *wpp = &ssd->wp;
    struct write_pointer *hot_wpp = &ssd->hot_wp;

    ssd->select_time = 0;
    ssd->erase_time = 0;
    ssd->move_time = 0;
    ssd->reallocation_time = 0;
    
    wpp->ch = 0;
    wpp->lun = 0;
    wpp->pg = 0;
    wpp->sec = 0;
    wpp->blk = 0;
    wpp->sblk = 0;
    wpp->pl = 0;
    hot_wpp->ch = 1;
    hot_wpp->lun = 2;
    hot_wpp->pg = 0;
    hot_wpp->sec = 0;
    hot_wpp->blk = 0;
    hot_wpp->sblk = 0;
    hot_wpp->pl = 0;
}

// 檢查記憶體位置是否合法
static inline void check_addr(int a, int max)
{
    ftl_assert(a >= 0 && a < max);
}

// SSD中的指標演算法(遵循寫入規則)
static void ssd_advance_write_pointer(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp; 
    struct write_pointer *wpp = &ssd->wp; 

    check_addr(wpp->sec, spp->secs_per_pg);
    wpp->sec += 2;
    if (wpp->sec >= spp->secs_per_pg){
        wpp->sec = 0;
        check_addr(wpp->pg, spp->pgs_per_sblk);
        wpp->pg++;
        if (wpp->pg % 2) {
            wpp->sec += 1;
        }
        if (wpp->pg == spp->pgs_per_sblk){
            wpp->pg = 0;
            check_addr(wpp->sblk, spp->sblks_per_blk);
            wpp->sblk++;
            if (wpp->sblk == spp->sblks_per_blk){
                wpp->sblk = 0;
                check_addr(wpp->blk, spp->blks_per_pl);
                wpp->blk++;
                if (wpp->blk == spp->blks_per_pl){
                    wpp->blk = 0;
                    check_addr(wpp->pl, spp->pls_per_lun);
                    wpp->pl++;
                    if (wpp->pl == spp->pls_per_lun){
                        wpp->pl = 0;
                        check_addr(wpp->lun, spp->luns_per_ch);
                        wpp->lun++;
                        if (wpp->lun == spp->luns_per_ch){
                            wpp->lun = 0;
                            check_addr(wpp->ch, spp->nchs);
                            wpp->ch++;
                            if (wpp->ch == spp->nchs){
                                wpp->ch = 0;
                            }
                        }
                    }
                }
            }
        }
    }
}

// SSD中的熱數據(較長使用的數據)指標演算法(遵循寫入規則)
static void ssd_advance_write_hotPointer(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp; // System limit
    struct write_pointer *hot_wpp = &ssd->hot_wp; // Current physical address
    // struct line_mgmt *lm = &ssd->lm;

    check_addr(hot_wpp->sec, spp->secs_per_pg);
    hot_wpp->sec += 2;
    if (hot_wpp->sec >= spp->secs_per_pg){
        hot_wpp->sec = 0;
        check_addr(hot_wpp->pg, spp->pgs_per_sblk);
        hot_wpp->pg++;
        if (hot_wpp->pg % 2) {
            hot_wpp->sec += 1;
        }
        if (hot_wpp->pg == spp->pgs_per_sblk){
            hot_wpp->pg = 0;
            check_addr(hot_wpp->sblk, spp->sblks_per_blk);
            hot_wpp->sblk++;
            if (hot_wpp->sblk == spp->sblks_per_blk){
                hot_wpp->sblk = 0;
                check_addr(hot_wpp->blk, spp->blks_per_pl);
                hot_wpp->blk++;
                if (hot_wpp->blk == spp->blks_per_pl){
                    hot_wpp->blk = 0;
                    check_addr(hot_wpp->pl, spp->pls_per_lun);
                    hot_wpp->pl++;
                    if (hot_wpp->pl == spp->pls_per_lun){
                        hot_wpp->pl = 0;
                        check_addr(hot_wpp->lun, spp->luns_per_ch);
                        hot_wpp->lun++;
                        if (hot_wpp->lun == spp->luns_per_ch){
                            hot_wpp->lun = 0;
                            check_addr(hot_wpp->ch, spp->nchs);
                            hot_wpp->ch++;
                            if (hot_wpp->ch == spp->nchs){
                                hot_wpp->ch = 0;
                            }
                        }
                    }
                }
            }
        }
    }
}

// 取得新的subpage
static struct ppa get_new_subpage(struct ssd *ssd)
{
    
    struct write_pointer *wpp = &ssd->wp; 
    struct ppa ppa; 

    ppa.ppa = 1;
    ppa.g.ch = wpp->ch;
    ppa.g.lun = wpp->lun;
    ppa.g.pg = wpp->pg;
    ppa.g.sblk = wpp->sblk;
    ppa.g.blk = wpp->blk;
    ppa.g.pl = wpp->pl;
    ppa.g.sec = wpp->sec;
    ftl_assert(ppa.g.pl == 0);

    return ppa;
}

// 檢查並確保參數符合限制
static void check_params(struct ssdparams *spp)
{
    ftl_assert(is_power_of_2(spp->luns_per_ch));
    ftl_assert(is_power_of_2(spp->nchs));
}

// 初始化參數
static void ssd_init_params(struct ssdparams *spp)
{
    spp->secsz = 512;
    spp->secs_per_pg = 8;
    spp->pgs_per_sblk = 64;
    spp->sblks_per_blk = 4;
    spp->blks_per_pl = 256; /* 16GB */
    spp->pls_per_lun = 2;
    spp->luns_per_ch = 4;
    spp->nchs = 2;

    spp->pg_rd_lat = NAND_READ_LATENCY;
    spp->pg_wr_lat = NAND_PROG_LATENCY;
    spp->blk_er_lat = NAND_ERASE_LATENCY;
    spp->ch_xfer_lat = 0;

    /* calculated values */
    spp->secs_per_sblk = spp->secs_per_pg * spp->pgs_per_sblk;
    spp->secs_per_blk = spp->secs_per_sblk * spp->sblks_per_blk;
    spp->secs_per_pl = spp->secs_per_blk * spp->blks_per_pl;
    spp->secs_per_lun = spp->secs_per_pl * spp->pls_per_lun;
    spp->secs_per_ch = spp->secs_per_lun * spp->luns_per_ch;
    spp->tt_secs = spp->secs_per_ch * spp->nchs;

    spp->pgs_per_pl = spp->pgs_per_sblk * spp->blks_per_pl;
    spp->pgs_per_lun = spp->pgs_per_pl * spp->pls_per_lun;
    spp->pgs_per_ch = spp->pgs_per_lun * spp->luns_per_ch;
    spp->tt_pgs = spp->pgs_per_ch * spp->nchs;

    spp->blks_per_lun = spp->blks_per_pl * spp->pls_per_lun;
    spp->blks_per_ch = spp->blks_per_lun * spp->luns_per_ch;
    spp->tt_blks = spp->blks_per_ch * spp->nchs;

    spp->pls_per_ch =  spp->pls_per_lun * spp->luns_per_ch;
    spp->tt_pls = spp->pls_per_ch * spp->nchs;

    spp->tt_luns = spp->luns_per_ch * spp->nchs;

    /* line is special, put it at the end */
    spp->blks_per_line = spp->tt_luns; /* TODO: to fix under multiplanes */
    spp->pgs_per_line = spp->blks_per_line * spp->pgs_per_sblk;
    spp->secs_per_line = spp->pgs_per_line * spp->secs_per_pg;
    spp->tt_lines = spp->blks_per_lun; /* TODO: to fix under multiplanes */

    spp->gc_thres_pcent = 0.75;
    spp->gc_thres_lines = (int)((1 - spp->gc_thres_pcent) * spp->tt_lines);
    spp->gc_thres_pcent_high = 0.95;
    spp->gc_thres_lines_high = (int)((1 - spp->gc_thres_pcent_high) * spp->tt_lines);
    spp->enable_gc_delay = true;


    check_params(spp);
}

// 初始化page
static void ssd_init_nand_page(struct nand_page *pg, struct ssdparams *spp)
{
    pg->nsecs = spp->secs_per_pg;
    pg->sec = g_malloc0(sizeof(nand_sec_status_t) * pg->nsecs);
    for (int i = 0; i < pg->nsecs; i++) {
        pg->sec[i] = SEC_FREE;
    }
    pg->status = PG_FREE;
    pg->ispc = 0;
    pg->vspc = 0;
}

// 初始化subblock
static void ssd_init_nand_sblk(struct nand_subblock *sblk, struct ssdparams *spp)
{
    sblk->npgs = spp->pgs_per_sblk;
    sblk->pg = g_malloc0(sizeof(struct nand_page) * sblk->npgs);
    for (int i = 0; i < sblk->npgs; i++) {
        ssd_init_nand_page(&sblk->pg[i], spp);
    }
    sblk->ipc = 0;
    sblk->vpc = 0;
    sblk->erase_cnt = 0;
    sblk->wp = 0;
}

// 初始化block
static void ssd_init_nand_blk(struct nand_block *blk, struct ssdparams *spp)
{
    blk->nsblks = spp->sblks_per_blk;
    blk->sblk = g_malloc0(sizeof(struct nand_subblock) * blk->nsblks);
    for (int i = 0; i < blk->nsblks; i++) {
        ssd_init_nand_sblk(&blk->sblk[i], spp);
    }
    blk->isblkc = 0;
    blk->vsblkc = 0;
    blk->erase_cnt = 0;
    blk->wp = 0;
}

// 初始化plane
static void ssd_init_nand_plane(struct nand_plane *pl, struct ssdparams *spp)
{
    pl->nblks = spp->blks_per_pl;
    pl->blk = g_malloc0(sizeof(struct nand_block) * pl->nblks);
    for (int i = 0; i < pl->nblks; i++) {
        ssd_init_nand_blk(&pl->blk[i], spp);
    }
}

// 初始化lun
static void ssd_init_nand_lun(struct nand_lun *lun, struct ssdparams *spp)
{
    lun->npls = spp->pls_per_lun;
    lun->pl = g_malloc0(sizeof(struct nand_plane) * lun->npls);
    for (int i = 0; i < lun->npls; i++) {
        ssd_init_nand_plane(&lun->pl[i], spp);
    }
    lun->next_lun_avail_time = 0;
    lun->busy = false;
}

// 初始化channel
static void ssd_init_ch(struct ssd_channel *ch, struct ssdparams *spp)
{
    ch->nluns = spp->luns_per_ch;
    ch->lun = g_malloc0(sizeof(struct nand_lun) * ch->nluns);
    for (int i = 0; i < ch->nluns; i++) {
        ssd_init_nand_lun(&ch->lun[i], spp);
    }
    ch->next_ch_avail_time = 0;
    ch->busy = 0;
}

//  初始化記錄Subpage Number與資料對應關係的陣列
static void ssd_init_maptbl_subpage(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp; 
    uint64_t size = spp->tt_pgs * spp->secs_per_pg * 10;
    ssd->maptbl = (struct ppa **)g_malloc0(sizeof(struct ppa *) * spp->tt_pgs); // 8bits * 1048576page = 1.0486MB
    ssd->maptbl2 = (struct ppa *)g_malloc0(sizeof(struct ppa) * spp->tt_pgs * spp->secs_per_pg * 10); // maptbl2[data] = ppa; // 18bits * 1048576page  * 8subpage = 18.8744MB

    for (int i = 0 ; i < spp->tt_pgs; i++){
        ssd->maptbl[i] = (struct ppa *)g_malloc0(sizeof(struct ppa) * spp->secs_per_pg);
    }
    for (int i = 0; i < spp->tt_pgs; i++) {
        for (int j = 0 ; j < spp->secs_per_pg; j++)
        {
            ssd->maptbl[i][j].ppa = UNMAPPED_PPA;
        }
    }
    for (int i = 0; i < size; i++){
        ssd->maptbl2[i].ppa = UNMAPPED_PPA;
    }
}

// 初始化記錄Subpage使用情況的陣列
static void ssd_init_usgtbl_subpage(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;
    const u_int64_t tt_spgs = spp->secs_per_pg * spp->tt_pgs;
    ssd->usgtbl = (int *)g_malloc0(sizeof(int) * tt_spgs);
    for (int i = 0 ; i < tt_spgs; i++) {
        ssd->usgtbl[i] = 0;
    }
}

//  初始化記錄資料與Subpage Number對應關係的陣列
static void ssd_init_rmap(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;
    ssd->rmap = g_malloc0(sizeof(uint64_t) * spp->tt_pgs * spp->secs_per_pg * 2);
    for (int i = 0; i < spp->tt_pgs * spp->secs_per_pg * 2; i++) {
        ssd->rmap[i] = INVALID_LPN;
    }
}

// 初始化記錄Subblock使用情況的陣列
static void ssd_init_usgtbl_subblock(struct ssd *ssd){
    struct ssdparams *spp = &ssd->sp;
    const u_int64_t tt_sblks = spp->tt_blks * spp->sblks_per_blk;

    ssd->sblk_usgtbl = (uint64_t *)g_malloc0(sizeof(uint64_t) * tt_sblks); 
    ssd->sblk_movedPg_usgtbl = (uint64_t *)g_malloc0(sizeof(uint64_t) * tt_sblks); 
    ssd->sblk_freePg_usgtbl = (uint64_t *)g_malloc0(sizeof(uint64_t) * tt_sblks); 
    ssd->sblk_ecValue = (uint64_t *)g_malloc0(sizeof(uint64_t) * tt_sblks); 
    ssd->priority_erased_sblk = (uint64_t *)g_malloc0(sizeof(uint64_t) * tt_sblks); 
    
    for (int i = 0 ; i < tt_sblks; i++){
        ssd->sblk_usgtbl[i] = 0;
        ssd->sblk_movedPg_usgtbl[i] = 0;
        ssd->sblk_freePg_usgtbl[i] = 64;
        ssd->sblk_ecValue[i] = 0;
        ssd->priority_erased_sblk[i] = 0;
    }

}

// 初始化記錄Subblock已寫入資料比例的陣列
static void ssd_init_cnttbl_subblock(struct ssd *ssd){
    struct ssdparams *spp = &ssd->sp;
    const u_int64_t tt_sblks = spp->tt_blks * spp->sblks_per_blk;

    ssd->sblk_cnttbl = (uint64_t *)g_malloc0(sizeof(uint64_t) * tt_sblks);
    
    for (int i = 0 ; i < tt_sblks; i++){
        ssd->sblk_cnttbl[i] = 0;
    }

}

// 初始化SSD
void ssd_init(FemuCtrl *n)
{
    struct ssd *ssd = n->ssd;
    struct ssdparams *spp = &ssd->sp;

    ftl_assert(ssd);

    ssd_init_params(spp);

    // 初始化SSD空間
    ssd->ch = g_malloc0(sizeof(struct ssd_channel) * spp->nchs);
    for (int i = 0; i < spp->nchs; i++) {
        ssd_init_ch(&ssd->ch[i], spp);
    }

    // 初始化記錄對應關係的陣列
    ssd_init_maptbl_subpage(ssd); // 19.923MB
    ssd_init_usgtbl_subpage(ssd);
    ssd_init_usgtbl_subblock(ssd); // 0.6555MB
    ssd_init_cnttbl_subblock(ssd); // 0.1311MB
    ssd_init_rmap(ssd); // 2.3593MB -> 19.923MB

    // 初始化記錄SSD指標的陣列
    ssd_init_write_pointer(ssd); 

    qemu_thread_create(&ssd->ftl_thread, "FEMU-FTL-Thread", ftl_thread, n,
                       QEMU_THREAD_JOINABLE);
}

// 檢查位址是否合法(沒有超出範圍)
static inline bool valid_ppa(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    int ch = ppa->g.ch;
    int lun = ppa->g.lun;
    int pl = ppa->g.pl;
    int blk = ppa->g.blk;
    int pg = ppa->g.pg;
    int sec = ppa->g.sec;

    if (ch >= 0 && ch < spp->nchs && lun >= 0 && lun < spp->luns_per_ch && pl >=
        0 && pl < spp->pls_per_lun && blk >= 0 && blk < spp->blks_per_pl && pg
        >= 0 && pg < spp->pgs_per_sblk && sec >= 0 && sec < spp->secs_per_pg)
        return true;

    return false;
}

// 檢查Page Number是否合法(沒有超出範圍)
static inline bool valid_lpn(struct ssd *ssd, uint64_t lpn)
{
    return (lpn < ssd->sp.tt_pgs);
}

// 檢查位址是否已經存入資料
static inline bool mapped_ppa(struct ppa *ppa)
{
    return !(ppa->ppa == UNMAPPED_PPA);
}

// 取得Channel
static inline struct ssd_channel *get_ch(struct ssd *ssd, struct ppa *ppa)
{
    return &(ssd->ch[ppa->g.ch]);
}

// 取得Lun
static inline struct nand_lun *get_lun(struct ssd *ssd, struct ppa *ppa)
{
    struct ssd_channel *ch = get_ch(ssd, ppa);
    return &(ch->lun[ppa->g.lun]);
}

// 取得Plane
static inline struct nand_plane *get_pl(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_lun *lun = get_lun(ssd, ppa);
    return &(lun->pl[ppa->g.pl]);
}

// 取得Block
static inline struct nand_block *get_blk(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_plane *pl = get_pl(ssd, ppa);
    return &(pl->blk[ppa->g.blk]);
}

// 取得Subblock
static inline struct nand_subblock *get_sblk(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_block *blk = get_blk(ssd, ppa);
    return &(blk->sblk[ppa->g.sblk]);
}

// 取得Page
static inline struct nand_page *get_pg(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_subblock *sblk = get_sblk(ssd, ppa);
    return &(sblk->pg[ppa->g.pg]);
}

// 記錄執行時間的演算法
static uint64_t ssd_advance_status(struct ssd *ssd, struct ppa *ppa, struct
        nand_cmd *ncmd)
{
    int c = ncmd->cmd;
    uint64_t cmd_stime = (ncmd->stime == 0) ? \
        qemu_clock_get_ns(QEMU_CLOCK_REALTIME) : ncmd->stime;
    const char* fileName = "output.txt";
    uint64_t nand_stime;
    struct ssdparams *spp = &ssd->sp;
    struct nand_lun *lun = get_lun(ssd, ppa);
    uint64_t lat = 0;

    switch (c) {
    case NAND_READ:
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->pg_rd_lat;
        lat = lun->next_lun_avail_time - cmd_stime;

    case NAND_WRITE:
        /* write: transfer data through channel first */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time; // Choose thr bigger timestamp between next lun available time and cmd stime(Always next lun available time)
        if (ncmd->type == USER_IO) {
            lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;
        } else {
            lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;
        }
        lat = lun->next_lun_avail_time - cmd_stime; // Total time

    case NAND_ERASE:
        /* erase: only need to advance NAND status */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->blk_er_lat;

        lat = lun->next_lun_avail_time - cmd_stime;
        break;

    default:
        ftl_err("Unsupported NAND command: 0x%x\n", c);
    }

    return lat;
}

// 將Page註記為Invalid，代表該Page中的Subpage均已無法寫入資料
static void mark_page_invalid(struct ssd *ssd, struct ppa *ppa)
{
    struct line_mgmt *lm = &ssd->lm;
    struct ssdparams *spp = &ssd->sp;
    struct nand_block *blk = NULL;
    struct nand_page *pg = NULL;
    bool was_full_line = false;
    struct line *line;
    uint64_t sblkidx;

    pg = get_pg(ssd, ppa);
    ftl_assert(pg->status == PG_VALID);
    pg->status = PG_INVALID;

    sblkidx = sblk2sblkidx(ssd, ppa);
    ssd->sblk_usgtbl[sblkidx]++;
    ssd->sblk_movedPg_usgtbl[sblkidx]--;
    ssd->sblk_ecValue[sblkidx] = ssd->sblk_movedPg_usgtbl[sblkidx] * 2 + ssd->sblk_freePg_usgtbl[sblkidx];

    /* update corresponding block status */
    blk = get_blk(ssd, ppa);
    ftl_assert(blk->isblkc >= 0 && blk->isblkc < spp->sblks_per_blk);
    blk->isblkc++;
    ftl_assert(blk->vsblkc > 0 && blk->vsblkc <= spp->sblks_per_blk);
    blk->vsblkc--;
}

// 將Subpage註記為Invalid，代表該Subpage已經無法寫入資料
static void mark_subpage_invalid(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_page *pg = NULL;

    pg = get_pg(ssd, ppa);
    ftl_assert(pg->sec[ppa->g.sec] == SEC_VALID);
    pg->sec[ppa->g.sec] = SEC_INVALID;
    ftl_assert(pg->ispc >= 0 && pg->ispc < ssd->sp.secs_per_pg);
    pg->ispc++;
    ftl_assert(pg->vspc > 0 && pg->vspc <= ssd->sp.secs_per_pg);
    pg->vspc--;
}

// 將Page註記為Valid，代表該Page中的Subpage可以寫入資料
static void mark_page_valid(struct ssd *ssd, struct ppa *ppa) 
{
    struct nand_block *blk = NULL;
    struct nand_page *pg = NULL;
    struct line *line;
    uint64_t sblkidx;

    /* update page status */
    pg = get_pg(ssd, ppa);
    ftl_assert(pg->status == PG_FREE);
    pg->status = PG_VALID;

    /* update corresponding block status */
    blk = get_blk(ssd, ppa);
    ftl_assert(blk->vsblkc >= 0 && blk->vsblkc < ssd->sp.sblks_per_blk);
    blk->vsblkc++;

    sblkidx = sblk2sblkidx(ssd, ppa);
    ssd->sblk_freePg_usgtbl[sblkidx]--;
    ssd->sblk_movedPg_usgtbl[sblkidx]++;
    ssd->sblk_ecValue[sblkidx] = ssd->sblk_movedPg_usgtbl[sblkidx] * 2 + ssd->sblk_freePg_usgtbl[sblkidx];
}

// 將Subpage註記為Valid，代表該Subpage可以寫入資料
static void mark_subpage_valid(struct ssd *ssd, struct ppa *ppa) 
{
    struct nand_page *pg = NULL;

    /* update page status */
    pg = get_pg(ssd, ppa);
    ftl_assert(pg->sec[ppa->g.sec] == SEC_FREE);
    pg->sec[ppa->g.sec] = SEC_VALID;
    pg->vspc++;
}

// 記錄寫入資料的使用頻率，以區別該資料為冷數據還是熱數據
static void add_subpage_usage(struct ssd *ssd, int index) 
{
    ssd->current_written_data_cnt++;

    uint64_t isRecent;
    if (ssd->current_written_data_cnt > ssd->total_written_data_cnt * 0.8){
        isRecent = 1;
    }
    else{
        isRecent = 0;
    }
    ssd->usgtbl[index] += (1 + isRecent);
}

// 將該Subblock中的Page以及Subpage恢復為初始狀態
static void mark_subblock_free(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    struct nand_subblock *sblk = get_sblk(ssd, ppa);
    struct nand_page *pg = NULL;

    for (int i = 0; i < spp->pgs_per_sblk; i++) {
        pg = &sblk->pg[i];
        ftl_assert(pg->nsecs == spp->secs_per_pg);
        pg->status = PG_FREE;
    }

    ftl_assert(sblk->npgs == spp->pgs_per_sblk);
    sblk->ipc = 0;
    sblk->vpc = 0;
    sblk->erase_cnt++;
}

// 取得要被擦除的Subblock(分為 1. 無Valid Data可以直接擦除的Subblock 2.有Valid Data要先存取再擦除的Subblock)
static uint64_t ssd_get_erased_moved_subblock(struct ssd *ssd){
    struct ssdparams *spp = &ssd->sp;
    uint64_t erased_sblk_cnt = 1; // The number of the subblock should be erased
    ssd->erased_sblk_cnt = erased_sblk_cnt;

    ssd->erase_sblk = (uint64_t *)g_malloc0(sizeof(uint64_t) * (erased_sblk_cnt));
    ssd->move_sblk = (uint64_t *)g_malloc0(sizeof(uint64_t) * (erased_sblk_cnt));
    ssd->move_data = (uint64_t *)g_malloc0(sizeof(uint64_t) * (erased_sblk_cnt * 64 * 4)); // Array that store the value will be moved to the other subblock
    ssd->adjust_data = (uint64_t *)g_malloc0(sizeof(uint64_t) * (erased_sblk_cnt * 64 * 4));
    for (int i = 0 ; i < erased_sblk_cnt ; i++){
        ssd->erase_sblk[i] = 0; // Array contians the subblock should be erased directly.
        ssd->move_sblk[i] = 0; // Array contian the subblock should be erased after moving the valid page.
    }

    // Use the reallocation subblock
    for (int i = 0; i < ssd->reallocation_sblk_cnt; i++){
        ssd->erase_sblk[i] = ssd->reallocation_sblk[i];
    }
    ssd->erased_sblk_cnt = 1; 

    uint64_t isBreak = 0;
    uint64_t total_sblk_cnt = 0;
    uint64_t erase_slbk_index = 0;
    uint64_t move_sblk_index = 0;
    uint64_t cnt = 0;
    const u_int64_t tt_sblks = spp->tt_blks * spp->sblks_per_blk;

    // for (int i = 1; i <= 128; i++){
    for (int i = ssd->erased_sblk_finalEC; i <= 128; i++){
    // for (int i = ssd->reallocation_sblk_finalEC; i <= 128; i++){
        for (int j = 0; j < tt_sblks; j++){
            cnt++;
            ssd->select_time += spp->pg_rd_lat;
            if (ssd->sblk_ecValue[j] == 20000){ // The invalid page in a subblock is equal to 64
                ssd->erase_sblk[erase_slbk_index] = j;
                erase_slbk_index++;
                total_sblk_cnt++;
                if (total_sblk_cnt == ssd->erased_sblk_cnt){
                    isBreak = 1;
                    break;
                }           
            }
            if (ssd->sblk_ecValue[j] == i){
                if (ssd->sblk_ecValue[j] == 64 && ssd->sblk_freePg_usgtbl[j] == 64){
                    continue;
                }
                ssd->move_sblk[move_sblk_index] = j;
                move_sblk_index++;
                total_sblk_cnt++;
                if (total_sblk_cnt == ssd->erased_sblk_cnt){
                    ssd->erased_sblk_finalEC = i;
                    isBreak = 1;
                    break;
                }
            }
        }
        if (isBreak == 1){
            break;
        }
    }
    return ssd->erased_sblk_cnt;
}

// 處理有Valid Data要先存取再擦除的Subblock
static void process_moved_subblock(struct ssd* ssd)
{
    struct ssdparams *spp = &ssd->sp;
    struct ppa ppa;
    struct nand_page* pg;
    ssd->moved_data_cnt = 0;

    uint64_t moved_subblock_index;
    uint64_t page_moved_subpage_cnt;

    for (int i = 0; i < 1; i++){
        moved_subblock_index = ssd->move_sblk[i]; // Get the moved subblock
        ppa = sblkidx2sblk(ssd, moved_subblock_index); // Get the physical address of the moved subblock
        for (int j = 0; j < spp->pgs_per_sblk; j++){
            ppa.g.pg = j; // Stored the page
            pg = get_pg(ssd, &ppa);
            if (pg->status == PG_VALID){
                page_moved_subpage_cnt = 0;
                for (int k = 0; k < spp->secs_per_pg; k++){
                    if (pg->sec[k] == SEC_VALID){
                        page_moved_subpage_cnt++;
                        ppa.g.sec = k;
                        uint64_t ppaidx = ppa2pgidx(ssd, &ppa);
                        uint64_t data = ssd->rmap[ppaidx];
                        ssd->move_data[ssd->moved_data_cnt++] = data;
                        pg->sec[k] = SEC_INVALID;
                        pg->vspc--;
                        pg->ispc++;
                    }
                }
                pg->status = PG_INVALID;
                ssd->sblk_movedPg_usgtbl[moved_subblock_index]--;
                ssd->sblk_usgtbl[moved_subblock_index]++;
                ssd->sblk_ecValue[moved_subblock_index] = ssd->sblk_movedPg_usgtbl[moved_subblock_index] * 2 + ssd->sblk_freePg_usgtbl[moved_subblock_index];
            }
        }
    }
}

// 擦除Subblock並初始化(無Valid Data的Subblock)
static void set_erased_subblock_pages_free(struct ssd* ssd)
{
    struct ssdparams *spp = &ssd->sp;
    struct ppa ppa;
    // uint64_t erase_sblk_cnt = ssd->move;
    uint64_t erased_subblock_index;
    for (int i = 0 ; i < 1; i++){
        erased_subblock_index = ssd->move_sblk[i];
        ppa = sblkidx2sblk(ssd, erased_subblock_index);
        for (int j = 0 ; j < spp->pgs_per_sblk; j++){
                ppa.g.pg = j;
                struct nand_page* pg = get_pg(ssd, &ppa); // Get the ppa of the erased page
                pg->status = PG_FREE;
                for (int i = 0 ; i < spp->secs_per_pg; i++){ // All of the subpage status of the page would reset to free 
                    pg->sec[i] = SEC_FREE;
                }
                pg->vspc = 0;
                pg->ispc = 0;
        }
        ssd->erase_time += spp-> blk_er_lat / 4;

        ssd->sblk_movedPg_usgtbl[erased_subblock_index] = 0;
        ssd->sblk_usgtbl[erased_subblock_index] = 0; // The invalid page count of the subblock would reset to 0
        ssd->sblk_freePg_usgtbl[erased_subblock_index] = 64;
        ssd->sblk_ecValue[erased_subblock_index] = ssd->sblk_movedPg_usgtbl[erased_subblock_index] * 2 + ssd->sblk_freePg_usgtbl[erased_subblock_index];
        ssd->sblk_cnttbl[erased_subblock_index]++; // The using time of the subblock
    }
}

// 遷移Valid Data至暫存區
static void write_move_data(struct ssd* ssd)
{
    struct ssdparams *spp = &ssd->sp;
    struct ppa ppa;
    int data;

    for (int i = 0; i < ssd->moved_data_cnt; i++){
        data = ssd->move_data[i];
        ppa = get_maptbl_ent_subpage(ssd, data); // Same logical number got same ppa
        add_subpage_usage(ssd, data); // data hotness(logical)
        ppa = get_new_subpage(ssd);
        struct nand_page* pg = get_pg(ssd, &ppa);
        if (pg->status == PG_FREE){ // Mark the page from free to valid at the 1st time use the page 
            mark_page_valid(ssd, &ppa); // 4
        }
        set_maptbl_ent_subpage(ssd, &ppa, data); // 1 2
        mark_subpage_valid(ssd, &ppa); // 4
        ssd_advance_write_pointer(ssd);

        ssd->move_time += spp->pg_wr_lat / 8;
    }
}

// 擦除Subblock並初始化(有Valid Data的Subblock)
static void set_reallocation_subblock_pages_free(struct ssd* ssd)
{
    struct ssdparams *spp = &ssd->sp;
    struct ppa ppa;
    uint64_t reallocation_sblk_cnt = ssd->reallocation_sblk_cnt;
    uint64_t reallocation_subblock_index;
    for (int i = 0 ; i < reallocation_sblk_cnt; i++){
        reallocation_subblock_index = ssd->reallocation_sblk[i];
        ppa = sblkidx2sblk(ssd, reallocation_subblock_index);
        for (int j = 0 ; j < spp->pgs_per_sblk; j++){
                ppa.g.pg = j;
                struct nand_page* pg = get_pg(ssd, &ppa); // Get the ppa of the erased page
                pg->status = PG_FREE;
                for (int i = 0 ; i < spp->secs_per_pg; i++){ // All of the subpage status of the page would reset to free 
                    pg->sec[i] = SEC_FREE;
                }
                pg->vspc = 0;
                pg->ispc = 0;
        }
        ssd->erase_time += spp-> blk_er_lat / 4;

        ssd->sblk_movedPg_usgtbl[reallocation_subblock_index] = 0;
        ssd->sblk_usgtbl[reallocation_subblock_index] = 0; // The invalid page count of the subblock would reset to 0
        ssd->sblk_freePg_usgtbl[reallocation_subblock_index] = 64;
        ssd->sblk_ecValue[reallocation_subblock_index] = ssd->sblk_movedPg_usgtbl[reallocation_subblock_index] * 2 + ssd->sblk_freePg_usgtbl[reallocation_subblock_index];
        ssd->sblk_cnttbl[reallocation_subblock_index]++; // The using time of the subblock
    }
}

// Garbage Collection
static void do_subblock_gc(struct ssd *ssd)
{
    ssd_get_erased_moved_subblock(ssd);
    process_moved_subblock(ssd);
    set_erased_subblock_pages_free(ssd);
    write_move_data(ssd);
    set_reallocation_subblock_pages_free(ssd);
    ssd->status = 1;
    ssd_init_erased_write_pointer(ssd);
}

// 遷移暫存區的資料至其他Subblock
static void process_data_reallocation(struct ssd* ssd)
{
    struct ssdparams *spp = &ssd->sp;
    struct ppa ppa;
    struct nand_page* pg;
    uint64_t cur = 0;
    uint64_t moved_subblock_index;
    for (int i = 0; i < 5; i++){
        moved_subblock_index = ssd->move_sblk[i];
        ssd->priority_erased_sblk[i] = moved_subblock_index;
        ppa = sblkidx2sblk(ssd, moved_subblock_index);
        for (int j = 0; j < spp->pgs_per_sblk; j++){
            ppa.g.pg = j; // Stored the page
            pg = get_pg(ssd, &ppa);
            if (pg->status == PG_VALID){
                for (int k = 0; k < spp->secs_per_pg; k++){
                    if (pg->sec[k] == SEC_VALID){
                        ppa.g.sec = k;
                        uint64_t ppaidx = ppa2pgidx(ssd, &ppa);
                        uint64_t data = ssd->rmap[ppaidx];
                        ssd->move_data[cur] = data;
                        cur++;
                    }
                }
                ssd->sblk_movedPg_usgtbl[moved_subblock_index]++;
            }
        }
    }
}

// 取得資料的熱度(使用更新頻率)
static void get_subpage_hotness(struct ssd* ssd, int reallocation_subbblock_size){
    struct ssdparams *spp = &ssd->sp;
    struct ppa ppa;
    struct nand_page* pg;
    uint64_t cur = 0;

    uint64_t moved_subblock_index;

    for (int i = 0; i < reallocation_subbblock_size; i++){
        moved_subblock_index = ssd->move_sblk[i]; // Get the moved subblock
        ppa = sblkidx2sblk(ssd, moved_subblock_index); // Get the physical address of the moved subblock
        for (int j = 0; j < spp->pgs_per_sblk; j++){
            ppa.g.pg = j; // Stored the page
            pg = get_pg(ssd, &ppa);
           
            if (pg->status == PG_VALID){
                for (int k = 0; k < spp->secs_per_pg; k++){
                    if (pg->sec[k] == SEC_VALID){
                        ppa.g.sec = k;
                        uint64_t ppaidx = ppa2pgidx(ssd, &ppa);
                        uint64_t data = ssd->rmap[ppaidx];
                        ssd->move_data[cur] = data;
                        cur++;
                    }
                }
                ssd->sblk_movedPg_usgtbl[moved_subblock_index]++;
            }
        }
    }
}

// 取得要被擦除的Subblock中有Valid Data的Subblock
static void get_reallocation_subblock(struct ssd *ssd){

    ssd->reallocation_sblk = (uint64_t *)g_malloc0(sizeof(uint64_t));
    ssd->reallocation_data = (uint64_t *)g_malloc0(sizeof(uint64_t) * (64 * 4)); 
    for (int i = 0 ; i < 1 ; i++){
        ssd->reallocation_sblk[i] = 0; 
        ssd->reallocation_data[i] = 0; /
    }
    for (int i = 1; i < 256; i++){
        ssd->reallocation_data[i] = 0;
    }
    uint64_t isBreak = 0;
    uint64_t reallocation_sblk_index = 0;
    for (int i = 1; i <= 128; i++){
        for (int j = 0; j < 16384 ; j++){
            if (ssd->sblk_ecValue[j] == i){
                ssd->reallocation_sblk[reallocation_sblk_index] = j;
                reallocation_sblk_index++;
                if (reallocation_sblk_index == 1){
                    isBreak = 1;
                    ssd->reallocation_sblk_finalEC = ssd->sblk_ecValue[j];
                    break;
                }
            }
        }
        if (isBreak == 1){
            break;
        }
    }
}

// 檢查熱數據
static uint64_t check_hot_data(struct ssd *ssd){
    struct ssdparams *spp = &ssd->sp;
    struct ppa ppa;
    struct nand_page* pg;
    uint64_t reallocation_subblock_index;
    ssd->reallocation_data_cnt = 0;

    for (int i = 0; i < 1; i++){
        reallocation_subblock_index = ssd->reallocation_sblk[i]; 
        ppa = sblkidx2sblk(ssd, reallocation_subblock_index); 
        for (int j = 0; j < spp->pgs_per_sblk; j++){
            ppa.g.pg = j; // Stored the page
            pg = get_pg(ssd, &ppa);
            if (pg->status == PG_VALID){
                for (int k = 0; k < spp->secs_per_pg; k++){
                    if (pg->sec[k] == SEC_VALID){
                        ppa.g.sec = k;
                        uint64_t ppaidx = ppa2pgidx(ssd, &ppa);
                        uint64_t data = ssd->rmap[ppaidx];
                        ssd->reallocation_data[ssd->reallocation_data_cnt] = data;
                        ssd->reallocation_data_cnt++;
                        pg->sec[k] = SEC_INVALID;
                        pg->ispc++;
                        pg->vspc--;
                    }
                }
                pg->status = PG_INVALID;
                ssd->sblk_usgtbl[reallocation_subblock_index]++;
                ssd->sblk_movedPg_usgtbl[reallocation_subblock_index]--;
                ssd->sblk_ecValue[reallocation_subblock_index] = ssd->sblk_movedPg_usgtbl[reallocation_subblock_index] * 2 + ssd->sblk_freePg_usgtbl[reallocation_subblock_index];
            }
        }
        ssd->sblk_ecValue[reallocation_subblock_index] = -1;
    }
    return ssd->reallocation_data_cnt;
}

// 調整熱數據指標
static void move_hot_data(struct ssd* ssd, uint64_t dataCount){
    for (int i = 0; i < dataCount; i++){
        ssd_advance_write_hotPointer(ssd);
    }
}

// 遷移熱數據
static void do_data_reallocation(struct ssd *ssd){
    get_reallocation_subblock(ssd);
    uint64_t dataCount = check_hot_data(ssd);
    move_hot_data(ssd, dataCount);
    get_subpage_hotness(ssd, len);
}

static int do_gc(struct ssd *ssd, bool force)
{
    struct ssdparams *spp = &ssd->sp;
    struct nand_lun *lunp;
    struct ppa ppa;
    int ch, lun;

    for (ch = 0; ch < spp->nchs; ch++) {
        for (lun = 0; lun < spp->luns_per_ch; lun++) {
            ppa.g.ch = ch;
            ppa.g.lun = lun;
            ppa.g.pl = 0;
            lunp = get_lun(ssd, &ppa);
            mark_subblock_free(ssd, &ppa);

            if (spp->enable_gc_delay) {
                struct nand_cmd gce;
                gce.type = GC_IO;
                gce.cmd = NAND_ERASE;
                gce.stime = 0;
                ssd_advance_status(ssd, &ppa, &gce);
            }

            lunp->gc_endtime = lunp->next_lun_avail_time;
        }
    }
    return 0;
}

// SSD讀取資料
static uint64_t ssd_read(struct ssd *ssd, NvmeRequest *req)
{
    struct ssdparams *spp = &ssd->sp;
    uint64_t lba = req->slba;
    int nsecs = req->nlb;
    lba = 100;
    nsecs = 45;
    struct ppa ppa;
    uint64_t start_lpn = lba / spp->secs_per_pg; // 12
    uint64_t end_lpn = (lba + nsecs - 1) / spp->secs_per_pg; // 18
    uint64_t lpn;
    uint64_t sublat, maxlat = 0;

    openFile();

    if (end_lpn >= spp->tt_pgs) {
        ftl_err("start_lpn=%"PRIu64",tt_pgs=%d\n", start_lpn, ssd->sp.tt_pgs);
    }

    /* normal IO read path */
    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
        ppa = get_maptbl_ent(ssd, lpn);
        ppa = get
        if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
            continue;
        }
        struct nand_cmd srd;
        srd.type = USER_IO;
        srd.cmd = NAND_READ;
        srd.stime = req->stime;
        sublat = ssd_advance_status(ssd, &ppa, &srd);
        maxlat = (sublat > maxlat) ? sublat : maxlat;
    }
    return maxlat;
}

// SSD寫入資料
static uint64_t ssd_write(struct ssd *ssd, NvmeRequest *req)
{
    struct ssdparams *spp = &ssd->sp;

    ssd->total_written_data_cnt = 1000000; // 265219
    ssd->current_written_data_cnt = 0;
    ssd->status = 0;
    int total = 0;
    int tmp = 2048; // 2048
    ssd->is_gc = 0;
    ssd->is_data_reallocation = 0;
    for (int i = 0; i < 25; i++){ // 1025
        if (i < 24){ // 1024
            req -> slba = total;
            req -> nlb = 1024; // 1024
            if (i % 2 == 0){
                tmp -= 4;
            }
            
            total += tmp;
        }

        if (i == 24){
            req -> slba = 0;
            ssd->erased_sblk_finalEC = 1;
            // req -> nlb = 524288; // 256MB
            for (int j = 0; j < 48; j++){
                do_subblock_gc(ssd); // 1個Subblock為0.25MB
            }   
            ssd->is_gc = 1;
            process_data_reallocation(ssd);
        }
        if (i == 20460){
            for (int j = 0; j < 20; j++){
                do_data_reallocation(ssd);
            }
            
            ssd->is_data_reallocation = 1;
        }
        uint64_t lba = req->slba;
        uint64_t len = req->nlb;
        int pos = 0;

        struct ppa ppa;
        uint64_t curlat = 0, maxlat = 0;
        uint64_t move_status;
        uint64_t data_cnt;
        int data;
        
        if (ssd->is_data_reallocation == 1 || ssd->is_gc == 1){
            maxlat = (curlat > maxlat) ? curlat : maxlat;
            move_status = (ssd->is_gc == 1) ? 0 : 1; // Determine gc or reallocation, gc -> 0, reallocation -> 1 
            data_cnt = (move_status == 0) ? ssd->moved_data_cnt : ssd->reallocation_data_cnt; // Get the correct size(gc or reallocation)
            for (int i = 0; i < data_cnt; i++){
                data = (move_status == 0) ? ssd->move_data[i] : ssd->reallocation_data[i];
                ppa = get_maptbl_ent_subpage(ssd, data); // Same logical number got same ppa
                add_subpage_usage(ssd, data); // data hotness(logical)
                ppa = get_new_subpage(ssd);
                struct nand_page* pg = get_pg(ssd, &ppa);
                if (pg->status == PG_FREE){ // Mark the page from free to valid at the 1st time use the page 
                    mark_page_valid(ssd, &ppa); // 4
                }
                set_maptbl_ent_subpage(ssd, &ppa, data); // 1 2
                mark_subpage_valid(ssd, &ppa); // 4
                if (move_status == 0){ // gc mode
                    ssd_advance_write_pointer(ssd);
                }
                else{ // reallocation mode
                    ssd_advance_write_hotPointer(ssd);
                }

                pos++;
                data_cnt = (move_status == 0) ? ssd->moved_data_cnt : ssd->reallocation_data_cnt;
                if (move_status == 0){
                    ssd->move_time += spp->pg_wr_lat / 8;
                }
                else{
                    ssd->reallocation_time += spp->pg_wr_lat / 8;
                }
            }
            ssd->is_gc = 0;
            ssd->is_data_reallocation = 0; 
        }
        
        while (ssd->is_data_reallocation == 0 && pos < len){
            data = lba + pos;
            ppa = get_maptbl_ent_subpage(ssd, data); // Same logical number got same ppa
            add_subpage_usage(ssd, data); // data hotness(logical)***
            if (mapped_ppa(&ppa)) {
                mark_subpage_invalid(ssd, &ppa);// update old page information first
                struct nand_page* pg = get_pg(ssd, &ppa);
                if (pg->ispc == ssd->sp.secs_per_pg / 2){  
                    mark_page_invalid(ssd, &ppa);
                }
                ppa = get_new_subpage(ssd);
                pg = get_pg(ssd, &ppa);
                if (pg->status == PG_FREE){ // Mark the page from free to valid at the 1st time use the page 
                    mark_page_valid(ssd, &ppa); // 4
                }
                set_maptbl_ent_subpage(ssd, &ppa, data); // 1 2
                mark_subpage_valid(ssd, &ppa); // 4
                ssd_advance_write_pointer(ssd);
            }
            else {
                ppa = get_new_subpage(ssd);
                struct nand_page* pg = get_pg(ssd, &ppa);
                if (pg->status == PG_FREE){ // Mark the page from free to valid at the 1st time use the page 
                    mark_page_valid(ssd, &ppa);
                }
                set_maptbl_ent_subpage(ssd, &ppa, data); // 1 2
                mark_subpage_valid(ssd, &ppa);
                ssd_advance_write_pointer(ssd);
            }
            
            pos++;
            struct nand_cmd swr;
            swr.type = USER_IO;
            swr.cmd = NAND_WRITE;
            swr.stime = req->stime;

            /* get latency statistics */
            curlat = ssd_advance_status(ssd, &ppa, &swr);
            maxlat = (curlat > maxlat) ? curlat : maxlat;            
        }
    }  
    return maxlat;
}

// FTL啟動函式
static void *ftl_thread(void *arg)
{
    FemuCtrl *n = (FemuCtrl *)arg;
    struct ssd *ssd = n->ssd;
    NvmeRequest *req = NULL;
    uint64_t lat = 0;
    int rc;
    int i;

    while (!*(ssd->dataplane_started_ptr)) {
        usleep(100000);
    }

    /* FIXME: not safe, to handle ->to_ftl and ->to_poller gracefully */
    ssd->to_ftl = n->to_ftl;
    ssd->to_poller = n->to_poller;
    int count = 0;
    while (count < 1) {
        for (i = 1; i <= n->num_poller; i++) {
            if (!ssd->to_ftl[i] || !femu_ring_count(ssd->to_ftl[i]))
                continue;

            rc = femu_ring_dequeue(ssd->to_ftl[i], (void *)&req, 1);
            if (rc != 1) {
                printf("FEMU: FTL to_ftl dequeue failed\n");
            }

            ftl_assert(req);
            req -> cmd.opcode = 0x01;
            count ++;

            switch (req->cmd.opcode) {
            case NVME_CMD_WRITE:
                lat = ssd_write(ssd, req);
                break;
            case NVME_CMD_READ:
                lat = ssd_read(ssd, req);
                break;
            case NVME_CMD_DSM:
                lat = 0;
                break;
            default:
                ftl_err("FTL received unkown request type, ERROR\n");
            }
            req->reqlat = lat;
            req->expire_time += lat;

            rc = femu_ring_enqueue(ssd->to_poller[i], (void *)&req, 1);
            if (rc != 1) {
                ftl_err("FTL to_poller enqueue failed\n");
            }
        }
    }
    return NULL;
}
# 專案動機與目標

## 1. 動機
SSD 以其高速存取特性，廣泛應用於高性能計算、伺服器及數據庫領域。然而，由於 SSD 使用 NAND Flash 作為存儲介質，每個存儲單元的擦寫次數有限（例如，消費級 SSD 約 3,000 次 P/E 次數，企業級 SSD 可達 10,000 次），頻繁的寫入操作可能導致壽命縮短。

### 挑戰
- **數據量增長**：SSD 需要頻繁進行 GC（Garbage Collection），影響性能和壽命。
- **FTL 效率問題**：現有架構在搜尋可用存儲空間時，效率較低，影響 I/O 性能。
- **擦除成本高**：當 Free Page（可用頁面）較少時，影響存儲效率。

## 2. 解決方案
本專案透過 **預搜尋機制** 降低可用存儲空間的搜尋時間，並提出 **EC Value 指標** 來減少擦除成本，從而提升 SSD 整體效能並延長壽命。

---

# 實驗環境
- **專案環境**：FEMU
- **測試環境**：FIO
- **程式語言**：C Language
- **作業系統**：Linux
- **開發程式碼**：[`ftl.c`](https://github.com/MarkHsieh0603/ssdSimulation/blob/master/FEMU-master/hw/femu/bbssd/ftl.c)、[`ftl.h`](https://github.com/MarkHsieh0603/ssdSimulation/blob/master/FEMU-master/hw/femu/bbssd/ftl.h)
---

# FEMU 介紹

## 1. FEMU (Fast Emulation of Non-Volatile Memory) 簡介
FEMU 是一款開源的非易失性記憶體（NVM）模擬平台，主要用於存儲系統研究與開發。它能夠模擬現代 NAND Flash 設備的行為，例如讀取、寫入、擦除等操作，幫助開發者分析並優化 SSD 及相關技術。

## 2. FEMU 主要特性
| 特性 | 說明 |
|------|------|
| **高速存儲模擬** | 支援 NAND Flash 讀寫擦除操作，模擬 SSD 行為 |
| **靈活配置** | 可自定義儲存設備的 Page、Block 大小及延遲參數 |
| **支持真實負載** | 可模擬並行讀寫、大量 I/O 操作，測試不同策略對 SSD 影響 |

---

# Flash Translation Layer (FTL)

FTL 是 SSD 內部管理數據存取的軟體層，負責提升性能與壽命。其核心功能如下：

### 1. **地址映射（Logical-to-Physical Mapping）**
- 由於 NAND Flash 只能按 Block 擦除，FTL 需要將邏輯地址對應到物理地址，減少不必要的擦除操作。

### 2. **磨損平衡（Wear Leveling）**
- SSD 每個 Block 的擦除次數有限，FTL 會將寫入操作均勻分布，避免某些 Block 先壞掉。

### 3. **垃圾回收（GC, Garbage Collection）**
- 回收無效數據塊，釋放空間並提升效能，避免因 Free Page 不足導致寫入速度下降。

### 4. **錯誤管理**
- 透過 ECC（Error Correction Code）檢測並修正 NAND 錯誤，提高 SSD 穩定性。

### 5. **電源管理**
- 調整 I/O 操作以降低功耗，提升 SSD 續航表現（適用於移動設備）。

---

# 實驗方向

## 1️⃣ 預搜尋機制（Pre-Search）
- 原始架構需要遍歷所有 Block 來搜尋可用存儲空間，導致搜索時間長。
- 本專案提出的 **Subblock 預搜尋機制** 可以更快定位可用空間，減少搜尋時間。

## 2️⃣ EC Value 優化（Erase Cost Value）
- EC Value 衡量擦除成本，數值越大代表擦除開銷越高。
- 透過 Free Page 比例來決定擦除優先順序，降低擦除成本。

---

# 實驗結果

## ✅ 預搜尋優化（Pre-Search）
- 測試寫入 1GB 至 4GB 資料時，原始架構的搜尋時間為預搜尋架構的 **1.06 至 1.19 倍**。
- 當寫入量增加，預搜尋的優勢更明顯。

![預搜尋效能比較](https://github.com/MarkHsieh0603/ssdSimulation/blob/master/FEMU-master/images/PreSearch.PNG)

## ✅ EC Value 優化
- 在相同 Valid Page 和 Free Page 數量下，當 Free Page 比例越高，EC Value 優化的擦除成本降低幅度越大。
- 無論在何種條件下，EC Value 方法皆能有效減少擦除開銷。

![EC Value 優化對擦除成本的影響](https://github.com/MarkHsieh0603/ssdSimulation/blob/master/FEMU-master/images/EC%20Value.PNG)


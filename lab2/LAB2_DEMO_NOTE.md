# NYCU OSC 2026 Spring Lab2 重點整理與 Demo 筆記

這份筆記是依照 `spec/lab2.rst` 與目前 `lab2/` 程式碼整理，目標是讓你在 TA demo 時可以快速、有條理地回答「規格要求是什麼」與「你怎麼實作」。

## 1. Lab2 目標總覽

依規格，Lab2 有四個主軸：

1. Basic 1: UART bootloader，透過 `load` command 收 kernel 並跳轉執行。
2. Basic 2: Devicetree parser，動態從 DTB 取硬體資訊（至少 UART base）。
3. Basic 3: Initial Ramdisk + CPIO parser，實作 `ls`/`cat` 讀 initramfs。
4. Advanced: Bootloader self-relocation，讓 kernel 可載到標準 entry address。

## 2. 你的實作地圖（檔案對應）

- 啟動流程與 shell 指令: `src/start.S`, `src/main.c`
- UART driver 與 DTB 取 UART base: `src/uart.c`, `src/uart.h`
- DTB parser: `src/fdt.c`, `src/fdt.h`
- CPIO parser: `src/cpio.c`, `src/cpio.h`
- 連結位址與映像範圍符號: `src/linker.ld`
- OrangePi FIT 打包（含 initrd）: `src/kernel.its`
- UART 傳輸工具: `send_kernel.py`
- 建置與執行流程: `Makefile`, `deploy.sh`

## 3. 開機到 shell 的實際流程

1. `_start` in `src/start.S`
- 清 `.bss`
- 設定 `sp = _end`
- `tail start_kernel`，保留 RISC-V boot ABI 傳入參數：
  - `a0 = hartid`
  - `a1 = fdt pointer`

2. `start_kernel(hartid, fdt)` in `src/main.c`
- 保存 `g_boot_hartid`, `g_boot_fdt`
- `uart_init_from_dtb(fdt)`：先嘗試從 DTB 取 UART base，失敗才用預設值
- `initrd_from_dtb(fdt)`：從 `/chosen` 取 `linux,initrd-start/end`，交給 `cpio_set_archive`
- 進入命令迴圈，支援 `help/hello/info/ls/cat/load`

## 4. Basic Exercise 1: UART Bootloader

### 4.1 規格重點

- 需設計一個簡單 UART protocol 收 kernel image。
- 在 shell 實作 `load` command。
- 驗證資料完整性（你有做 checksum）。

### 4.2 你的 protocol 設計（對應 `send_kernel.py` + `cmd_load`）

Header 格式（little-endian, 12 bytes）：

1. `magic` (`0x544F4F42`, "BOOT")
2. `size` (payload bytes)
3. `checksum` (payload 所有 byte 加總，mod `2^32`)

Board 端流程（`cmd_load`）：

1. 先做 self-relocation（見第 7 節）
2. 收 `magic/size/checksum`
3. 驗證 magic 與 size
4. 把 payload 收到 `KERNEL_LOAD_ADDR`
5. 驗證 checksum
6. `a0/a1` 回填 boot args，`fence.i` 後 `jr KERNEL_LOAD_ADDR`

### 4.3 你目前載入位址策略

- Board 模式：
  - `KERNEL_LOAD_ADDR = 0x00200000`
  - `RELOC_ADDR = 0x20000000`
- QEMU 模式：
  - `KERNEL_LOAD_ADDR = 0x80200000`
  - `RELOC_ADDR = 0x80300000`

也就是你已經走 advanced 解法：先搬 bootloader，再把 kernel 載到標準 entry。

## 5. Basic Exercise 2: Devicetree

### 5.1 規格重點

- 不可硬編硬體位址，至少要用 DTB 抓 UART base（`/soc/serial` 的 `reg`）。
- 需實作類似：
  - `fdt_path_offset`
  - `fdt_getprop`

### 5.2 你的 parser 核心觀念

1. 檢查 `fdt_header.magic == 0xd00dfeed`
2. 由 header 取 `off_dt_struct`, `off_dt_strings`
3. 走訪 structure block token：
  - `FDT_BEGIN_NODE`
  - `FDT_PROP`
  - `FDT_END_NODE`
  - `FDT_END`
4. node name/property 皆需處理 4-byte 對齊
5. 所有數值是 big-endian，需 `fdt_be32/fdt_be64`

### 5.3 你的 UART DTB 初始化

在 `uart_init_from_dtb`：

1. 依序找 node path：
  - `/soc/serial`
  - `/soc/serial@10000000`
  - `/soc/serial@d4017000`
2. 取 `reg` property
3. `len >= 8` 用 `fdt_be64`，`len >= 4` 用 `fdt_be32`
4. 設定 `uart_base`
5. 依 base 判斷 register layout（QEMU byte register vs OrangePi 32-bit register）

TA 常問重點：
- 為何要 fallback path？
  - 不同平台 dtb 節點命名可能不同。
- 為何要區分 reg32 / byte register？
  - 同是 UART，寄存器對齊與 offset 可能不同，直接共用會讀錯位址。

## 6. Basic Exercise 3: Initrd + CPIO (newc)

### 6.1 規格重點

- 不能硬編 initrd 位址，必須由 DTB `/chosen` 取：
  - `linux,initrd-start`
  - `linux,initrd-end`
- 實作 `ls`、`cat`
- 解析 newc 格式，注意 4-byte padding

### 6.2 你的 initrd 流程

1. `initrd_from_dtb(fdt)` 找 `/chosen`
2. 讀 `initrd-start/end`（支援 32/64 bit property）
3. 驗證 `end > start`
4. `cpio_set_archive(start, end)`

### 6.3 你的 CPIO parser 重點

`cpio_walk_next` 每次解析一個 entry：

1. 檢查 header magic 是 `"070701"`
2. 讀 `c_namesize` 與 `c_filesize`（hex string -> integer）
3. `name` 後對齊到 4 bytes 才是 `data`
4. `data + filesize` 再對齊到下一個 entry
5. 全程做 boundary check，避免越界

`ls`：
- 逐項走訪
- 遇到 `TRAILER!!!` 停止
- 輸出 `filesize + filename`

`cat`：
- 找到檔名後輸出內容
- 支援 `./name` 與 `name` 比對

## 7. Advanced: Bootloader Self-Relocation（你這份最關鍵）

### 7.1 為什麼要 relocation

若 bootloader 仍在 `0x00200000` 執行，直接把新 kernel 寫到 `0x00200000` 會覆蓋自己，可能在接收中就 crash。

### 7.2 你的 relocation 具體做法

`cmd_load` 開頭呼叫：

- `ensure_self_relocated_for_load((unsigned long)&&after_reloc, sp_now, g_boot_fdt);`

核心步驟：

1. 取原映像範圍 `[_start, _end)`
2. 若目前 PC 已不在原映像，代表已搬過，直接成功
3. 檢查 `resume_addr`（`&&after_reloc`）是否位於原映像內
4. 檢查 `RELOC_ADDR` 不可與原映像重疊
5. 檢查 `RELOC_ADDR` 不可撞 FDT / initrd
6. `mem_copy` 把整個 bootloader 複製到 `RELOC_ADDR`
7. 用 offset 計算新 `resume_reloc` 與新 `sp_reloc`
8. `mv sp`, `fence.i`, `jr resume_reloc`

### 7.3 TA 很常追問的兩題

1. 為什麼 `resume_addr` 必須在原映像內？
- 因為要用 `RELOC_ADDR + (resume_addr - image_start)` 做位址平移。
- 若 `resume_addr` 不在原映像，offset 沒有語意，跳轉位址會錯。

2. 為什麼要 `fence.i`？
- 因為程式碼剛被複製到新位址，I-cache 可能仍是舊內容。
- `fence.i` 保證後續取指看到最新記憶體內容。

## 8. 記憶體配置口試版（建議背）

- Linker script (`linker.ld`)：bootloader link at `0x00200000`
- `_start`: 映像起點
- `_end`: 映像尾端 + stack base
- Self-relocation destination: `0x20000000` (board)
- Kernel load address after relocation: `0x00200000` (board)
- FIT 中 ramdisk load: `0x46100000`
- FIT 中 fdt load: `0x31000000`

## 9. Demo 推薦流程（OrangePi）

1. 建置：
```bash
cd /home/osc/osc/NYCU_OSC_2026Spring/lab2
make clean
make all
```

2. 準備 initramfs（若有更新 rootfs）：
```bash
cd rootfs
find . | cpio -o -H newc > ../initramfs.cpio
cd ..
```

3. 打包 FIT（含 kernel/fdt/initrd）：
```bash
make fit
```

4. 上板開機後先做功能展示：
- `help`
- `ls`
- `cat osc.txt`

5. 展示 UART load：
- 板子端先打 `load`
- 主機端送：
```bash
python3 send_kernel.py /dev/ttyUSB0 kernel_boot.bin
```

6. TA 問時說明：
- load 前先 relocation，避免覆蓋自己
- 收包有 magic/size/checksum 驗證
- jump 前有 `fence.i`，並保留 `a0/a1`

## 10. 快速口試 QA（精簡版）

1. 你怎麼拿到 UART base？
- 由 `a1` 傳入的 DTB，走 `fdt_path_offset + fdt_getprop` 讀 `/soc/serial` 的 `reg`。

2. DTB 為何需要 big-endian 轉換？
- FDT 定義 multi-byte 欄位為 big-endian，不轉會讀錯地址/長度。

3. CPIO newc 什麼最容易錯？
- `namesize/filesize` 是 ASCII hex，不是 binary integer。
- name/data 兩段都要 4-byte 對齊。

4. 為何 self-relocation 要檢查 FDT/initrd overlap？
- 搬到這些區域會破壞 boot metadata 或 initramfs，後續 parser 會壞掉。

5. 為什麼 jump kernel 要設 `a0/a1`？
- 這是 RISC-V boot ABI，kernel 依賴它取得 hartid 與 DTB。

## 11. 現在程式與舊筆記差異提醒

`LAB2_IMPLEMENTATION_NOTES.md` 中有些段落是舊進度（例如曾寫 advanced 未完成）。  
以目前 `src/main.c` 實際程式為準，你已經實作 self-relocation 與標準位址載入流程。

## 12. Demo 前最終檢查清單

- `load` 前後不會 crash（表示 relocation 可用）
- `ls` 可列出 `rootfs` 檔案
- `cat osc.txt` 輸出內容正確
- 傳錯 magic/checksum 會被拒絕（可口頭描述）
- 能解釋 `fence.i`、`a0/a1`、DTB big-endian、cpio 4-byte padding


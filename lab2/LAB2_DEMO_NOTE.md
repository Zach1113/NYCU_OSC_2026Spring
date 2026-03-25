# NYCU OSC 2026 Spring Lab2 重點整理與 Demo 筆記（Refactor 版）

這份版本已對齊目前 `lab2/` 的重構架構：

- **Stage 1**: `bootloader` 打包成 `kernel.fit`，從 SD 啟動。
- **Stage 2**: `kernel.bin` 由主機透過 UART 傳給 bootloader（`load`），再 jump 進真正 kernel。

---

## 1. Lab2 目標總覽

依 `spec/lab2.rst`，Lab2 主軸：

1. Basic 1: UART bootloader（`load` 收 kernel 並跳轉）
2. Basic 2: Devicetree parser（至少用 DTB 取得 UART base）
3. Basic 3: Initrd + CPIO parser（`ls` / `cat`）
4. Advanced: Bootloader self-relocation

---

## 2. 目前程式碼地圖（Refactor 後）

- **Bootloader**
  - `src/bootloader/start.S`
  - `src/bootloader/linker.ld`
  - `src/bootloader/main.c`
- **Kernel (actual OS kernel payload)**
  - `src/kernel/start.S`
  - `src/kernel/linker.ld`
  - `src/kernel/main.c`
- **Shared components**
  - `src/uart.c`, `src/uart.h`
  - `src/fdt.c`, `src/fdt.h`
  - `src/cpio.c`, `src/cpio.h`
- **Packaging / tools**
  - `src/kernel.its`
  - `send_kernel.py`
  - `Makefile`
  - `deploy.sh`

---

## 3. 兩階段啟動流程（你現在的 demo 重點）

### Stage 1: SD 啟動 bootloader (`kernel.fit`)

1. U-Boot 依 `boot.cmd` 載入 `kernel.fit`
2. 進入 `src/bootloader/main.c` 的 shell
3. Bootloader shell 指令為：`help`, `load`

### Stage 2: UART 載入實際 kernel (`kernel.bin`)

1. 在 bootloader shell 輸入 `load`
2. 主機送出 `kernel.bin`
3. Bootloader 驗證 magic/size/checksum
4. 設定 `a0/a1`（hartid/fdt），`fence.i` 後跳到 kernel
5. 進入 `src/kernel/main.c` 的 kernel shell
6. Kernel shell 指令為：`help`, `hello`, `info`, `ls`, `cat`

---

## 4. Basic Exercise 1（UART Bootloader）

### 4.1 Protocol

header（little-endian, 12 bytes）:

1. magic = `0x544F4F42` (`"BOOT"`)
2. size
3. checksum（payload byte sum mod `2^32`）

對應：`send_kernel.py` 與 `src/bootloader/main.c::cmd_load`

### 4.2 Self-relocation（Advanced 也一起完成）

Bootloader 在 `load` 先做 self-relocation，避免在 `0x00200000` 接收 payload 時覆蓋自己。

- Board:
  - `KERNEL_LOAD_ADDR = 0x00200000`
  - `RELOC_ADDR = 0x20000000`

關鍵：
- 檢查 relocation 目的地不可 overlap 原映像
- 檢查不可撞到 FDT / initrd 區間
- 搬完後 `mv sp` + `fence.i` + `jr`

---

## 5. Basic Exercise 2（Devicetree）

### 5.1 有做的點

- 實作 `fdt_path_offset` / `fdt_getprop`
- `uart_init_from_dtb(fdt)` 由 DTB 讀 `/soc/serial` 的 `reg`
- 支援常見 fallback path（不同平台節點命名差異）
- 正確處理 big-endian（`fdt_be32` / `fdt_be64`）

### 5.2 TA 常問

- 為何不能硬編 UART base？
  - 可攜性差，換板子就壞。
- 為何要 big-endian 轉換？
  - FDT 規格就是 big-endian，不轉會讀錯。

---

## 6. Basic Exercise 3（Initrd + CPIO）

### 6.1 Initrd 來源

Kernel 端從 `/chosen` 讀：
- `linux,initrd-start`
- `linux,initrd-end`

再呼叫 `cpio_set_archive(start, end)`。

### 6.2 CPIO 功能

- `ls`: 列出 archive 內容
- `cat <file>`: 顯示檔案內容
- 解析 newc 格式，處理 4-byte 對齊與邊界檢查

---

## 7. 映像命名與用途（目前版本）

- `boot.bin`: bootloader binary（from `src/bootloader`）
- `kernel.fit`: SD 開機用 FIT（內含 bootloader + dtb + initramfs）
- `kernel.bin`: 實際 kernel payload（from `src/kernel`，給 UART `load`）

---

## 8. Demo 推薦流程（OrangePi）

1. 建置
```bash
cd /home/osc/osc/NYCU_OSC_2026Spring/lab2
make clean
make build
```

2. 若 rootfs 更新，先重建 initramfs
```bash
cd rootfs
find . | cpio -o -H newc > ../initramfs.cpio
cd ..
```

3. 產生 SD 開機映像
```bash
make fit
```

4. 部署到 SD（會覆蓋 SD 上的 `kernel.fit`）
```bash
./deploy.sh
```

5. 開機後先看到 **bootloader shell**
```text
# help
Available commands:
  load - Receive kernel stream and jump to it
  help - Show this help message
```

6. 在板子端輸入
```text
# load
```

7. 主機送 kernel payload
```bash
make send
# 等價於：python3 send_kernel.py /dev/ttyUSB0 kernel.bin
```

8. 跳進 **kernel shell** 後展示
- `help`
- `hello`
- `info`
- `ls`
- `cat osc.txt`

---

## 9. 快速口試 QA

1. 你怎麼避免 bootloader 被覆蓋？
- `load` 前先 self-relocation 到 `0x20000000`，再收 kernel 到 `0x00200000`。

2. 為什麼 jump 前要 `fence.i`？
- 確保 CPU 取到剛寫入的新指令。

3. jump 為什麼要設 `a0/a1`？
- RISC-V boot ABI：`a0=hartid`, `a1=fdt`。

4. initrd 位址怎麼拿？
- 從 DTB `/chosen` 的 `linux,initrd-start/end`，不是硬編。

5. CPIO 最容易踩雷？
- `namesize/filesize` 是 ASCII hex；name/data 都要 4-byte 對齊。

---

## 10. Demo 前檢查清單

- `make fit` 產生 `kernel.fit`
- `deploy.sh` 已覆蓋 SD 上 `kernel.fit`
- 開機先看到 bootloader（`Simple Bootloader`）
- `load` 後主機送 `kernel.bin` 成功跳轉
- kernel shell 的 `ls/cat` 正常讀出 initramfs

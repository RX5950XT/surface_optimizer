# CONTEXT.md — Surface Pro 7 效能與能耗優化守護程式

## 專案狀態：✅ Windows 服務 `SurfaceOptimizer` 常駐（開機自啟可在系統列勾選）。登入後 `--foreground-watch` 放系統列圖示：左鍵開關優化、右鍵開機自啟動。EPP 等仍改 `surface_optimizer.toml`。

## 架構概覽

以 C++20 原生靜態編譯的 Windows 背景守護程式，針對 Surface Pro 7（Intel i7-1065G7 Ice Lake）設計。

### 這台機器量到的硬體事實（2026-08-28）

- i7-1065G7、4C/8T、**均質、沒有大小核**。CpuSet `EfficiencyClass` 全是 0，沒有「最強那顆核」可綁。
- CPUID.06H EAX=`0x178ff3`：HWP / EPP / Activity Window 都有；**PECI override=yes**（EC 可蓋過 OS 的 Min/Max/EPP）；沒有 Turbo Boost Max 3.0、沒有 hybrid。
- 作用中方案是高效能（`SCHEME_MIN`）。`PERFAUTONOMOUS` AC/DC 都是 1（硬體自己選頻）。
- 出廠／目前 PPM：插電 `PROCTHROTTLEMIN=100%`、`CPMINCORES=100%`（閒置也幾乎不能降頻）；電池 `MIN=30%`、`CPMINCORES=12%`。`SOFTPARKLATENCY=0`，`SMTUNPARKPOLICY=0`。高效能方案插電 **PCIe ASPM=2（高度省電）**，會讓 NVMe 第一次 I/O 頓；daemon 把 AC 改 0、DC 留 2。
- `PowerWrite` + `PowerSetActiveScheme` 讓 PERFEPP 讀回目標：**約 93 ms**（不是 10 ms）。硬體 HWP 選頻本身約 1 ms，瓶頸在方案套用。
- RAPL PL1/PL2/Tau 使用者態讀不到（沒有 MSR 驅動）。DTT/PEP 裝置存在。不裝 WinRing0。

### 模組結構

| 模組 | 檔案 | 功能 |
|------|------|------|
| M1 核心服務 | `core/service`, `config`, `logger`, `utils`, `tray`, `ui_state` | Windows 服務、事件迴圈、CLI；使用者工作階段 `--foreground-watch` 負責前台視窗、LastInput **與系統列圖示**（暫停優化／開機自啟）。全域 IPC 限 `SYSTEM` 與互動使用者，命令值只收 0／1。 |
| M2 CPU 調度 | `optimizer/power_manager`, `optimizer/burst_policy` | EPP + Boost + **Min processor state** + **CPMINCORES 叫醒核** + **GPU 忙碌維持** + **插電 ASPM Off**；前台／輸入／CPU／IO／GPU 忙碌時 InstantBoost，安靜超過 hysteresis 立刻降回 |
| M3 記憶體管理 | `optimizer/memory_manager` | EmptyWorkingSet 背景修剪（75%）；Standby List **預設不清**，僅手動或 enable 且 RAM≥85% |
| M4 進程守護 | `optimizer/process_guardian`, `optimizer/cpu_reserve`, `optimizer/memory_leak_policy` | 前台 **HighQoS**、背景 **EcoQoS**；**非白名單行程親和性避開第一顆實體核**（給 DWM／核心留核）；單次工作集成長 >100 MB 才視為洩漏。EcoQoS 在這顆均質 CPU 上只降頻、不換核 |
| M5 探測 | `telemetry/platform_probe` | 唯讀：CPUID、CpuSet、PPM、NtPower MHz、EPP 套用延遲。`--status` 不再寫入電源方案 |

### 編譯工具鏈

- GCC 14.2.0 (MSYS2 UCRT64)、C++20、`-O3 -flto -s -static`
- 零外部依賴、靜態連結
- 二進位檔：`surface_optimizer.exe`（~1.35 MB）

### CLI 指令

```
--install / --uninstall    Windows 服務安裝/移除
--start / --stop           啟動/停止服務
--daemon                   SCM 後台服務模式
--interactive / -i         前台互動模式（含即時 log）
--status                   唯讀查詢服務狀態 + CPU/HWP/parking 探測
--benchmark                探測 + EPP 套用延遲 + 子系統自檢
--trim-memory / --clean-cache  手動記憶體整理
--version / --help
```

### 驗證結果

- 2026-08-29 發布版：完整編譯成功、`test_burst_policy` 30 項通過、`audit_ipc_security.ps1` 通過；一般使用者 `--status` 顯示 daemon RUNNING，系統列 IPC 可讀寫。部署 SHA-256：`69AE630A…C3F05652C`。
- 2026-08-29：服務 + 系統列 helper 私有記憶體 8.3 MB，工作集 23.8 MB。
- 2026-08-29：30 秒閒置量測服務 1.4892% 單核心、helper 0.1024%；相較修正前服務 2.7913%，下降約 46.7%。
- 前台升頻：WinEvent 仍是事件驅動；EPP 方案套用實測 ~93 ms；背景 EcoQoS 第一次巡查可套上 100+ 個行程
- 插電 PCIe ASPM 已改 0（Off），電池仍 2
- 閒置時把插電 Min processor state 從 100% 放到 5%，頻率才真正降得下去
- 記憶體修剪實測回收 >1 GB
- ProcessGuardian 掃描 77 進程、22 項白名單保護正常
- OS 預留核實測：`dwm`/`csrss`/`explorer` 親和性 `0xff`；一般程式 `0xfc`（第一顆實體核 `0x3` 留給系統）

## 使用方式

沒有設定視窗。改 `surface_optimizer.toml` 後要 `--stop` 再 `--start` 才會整份重載。

```powershell
# 管理員：註冊為開機自啟的 Windows 服務並立刻跑
.\surface_optimizer.exe --install
.\surface_optimizer.exe --start
.\surface_optimizer.exe --status

# 前台互動模式（測試用，與服務互斥）
.\surface_optimizer.exe --interactive
```

本機服務路徑：`"C:\RX5950XT\surface_optimizer\surface_optimizer.exe" --daemon`  
日誌：`C:\ProgramData\surface_optimizer\surface_optimizer.log`

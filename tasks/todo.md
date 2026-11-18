# surface_optimizer todo
最後更新：2026-11-19

## 現況（交接）

- C++20 原生靜態編譯的 Windows 背景守護程式，針對 Surface Pro 7（i7-1065G7 Ice Lake）做效能與能耗優化。Windows 服務 `SurfaceOptimizer` 常駐；登入後 `--foreground-watch` 放系統列圖示（左鍵開關優化、右鍵開機自啟動）。CLI 與使用方式見 README。
- 沒有設定視窗：改 `surface_optimizer.toml` 後要 `--stop` 再 `--start` 才整份重載。
- 本機服務路徑 `"C:\RX5950XT\surface_optimizer\surface_optimizer.exe" --daemon`；日誌 `C:\ProgramData\surface_optimizer\surface_optimizer.log`。
- 工具鏈：GCC 14.2.0（MSYS2 UCRT64）、C++20、`-O3 -flto -s -static`，零外部依賴，執行檔約 1.35 MB。
- 目前部署版（2026-11-13）：SHA-256 `163CE08FA71C460B10D29D332FDF97EB24D1D781FA19C3CF2D857B5E5B503B05`；舊版備份在 `build/backup-20261113/`。

### 模組

| 模組 | 檔案 | 功能 |
|---|---|---|
| M1 核心服務 | `core/service`, `config`, `logger`, `utils`, `tray`, `ui_state` | 服務、事件迴圈、CLI；`--foreground-watch` 負責前台視窗、LastInput 與系統列圖示。全域 IPC 限 `SYSTEM` 與互動使用者，命令值只收 0／1 |
| M2 CPU 調度 | `optimizer/power_manager`, `optimizer/burst_policy` | EPP + Boost + Min processor state + CPMINCORES + GPU 忙碌維持 + 插電 ASPM Off；前台／輸入／CPU／IO／GPU 忙碌時 InstantBoost，安靜超過 hysteresis 降回 |
| M3 記憶體 | `optimizer/memory_manager` | EmptyWorkingSet 背景修剪（75%）；Standby List 預設不清，僅手動或 enable 且 RAM≥85% |
| M4 進程守護 | `optimizer/process_guardian`, `cpu_reserve`, `memory_leak_policy` | 前台 HighQoS、背景 EcoQoS；非白名單行程親和性避開第一顆實體核（`0xfc`，`0x3` 留給系統）；單次工作集成長 >100 MB 才算洩漏 |
| M5 探測 | `telemetry/platform_probe` | 唯讀：CPUID、CpuSet、PPM、NtPower MHz、EPP 套用延遲；`--status` 不寫電源方案 |

### 硬體事實（2026-08-28 實測）

- 4C/8T 均質、沒有大小核（CpuSet `EfficiencyClass` 全 0），EcoQoS 只降頻不換核。
- CPUID.06H EAX=`0x178ff3`：HWP / EPP / Activity Window 都有；PECI override=yes（EC 可蓋過 OS 的 Min/Max/EPP）；無 Turbo Boost Max 3.0。
- 作用中方案高效能；`PERFAUTONOMOUS` AC/DC 都是 1。出廠插電 `PROCTHROTTLEMIN=100%`、`CPMINCORES=100%`（閒置幾乎不能降頻），電池 `MIN=30%`、`CPMINCORES=12%`。
- 高效能方案插電 PCIe ASPM=2 會讓 NVMe 第一次 I/O 頓；daemon 把 AC 改 0、DC 留 2。
- `PowerWrite` + `PowerSetActiveScheme` 讓 EPP 生效約 93 ms（HWP 選頻本身約 1 ms，瓶頸在方案套用）。
- RAPL PL1/PL2/Tau 使用者態讀不到；不裝 WinRing0。

### 關鍵調校與決策

- 電池（2026-11-13）：修正 `GUID_ACDC_POWER_SOURCE` 錯誤 GUID（正確 `{5d3e9a59-e9d5-4b00-a6bd-ff34ff516548}`）；`Service` 啟動／電量事件／背景巡檢用 `GetSystemPowerStatus` 同步 AC 狀態。電池 InstantBoost 用 `dc_boost_mode=0`、`Min=5%`、`Unpark=25%`；`dc_max_processor_percent=85`（低電量 ≤70%）、`dc_epp_idle=90`、`dc_epp_active=70`、`idle_hysteresis_ms=1000`。`PowerManager` 會備份／還原 AC/DC `PROCTHROTTLE_MAX`。
- 插電（2026-09-15）：爆發 Min 5%（`ac_min_active_percent`）、降頻 hysteresis 1000 ms、busy poll 250 ms、巡查 15 s；前台 IO 忙碌門檻每秒 ≥1000 次（`busy_io_ops_per_sec_threshold`，修正前插電全速佔 72.9%）。已因輸入／CPU／IO 維持爆發時不查 PDH GPU；每分鐘 INFO log `Burst hold polls (60s)` 記原因。
- `ProcessGuardian` 每 5 秒掃描，已限制的 PID 不重複開啟。
- 關閉優化不停服務（否則圖示一起消失）；暫停時還原電源方案與親和性。圖示在 Windows 11 可能先落在系統列溢位。

### 驗證與實測

- 測試：`test_burst_policy` 36/36、`audit_ipc_security.ps1` 通過（2026-11-13）。
- 改 header 後要清掉 `build/fix_obj` 再編，否則會 `-Wlto-type-mismatch`。
- 2026-08-29：服務 30 秒閒置 1.4892% 單核（修正前 2.7913%，降約 46.7%），helper 0.1024%；私有記憶體 8.3 MB、工作集 23.8 MB。
- 記憶體修剪實測回收 >1 GB；ProcessGuardian 掃描 77 進程、22 項白名單正常。

## 待辦

- 目前無本專案未完成項目。

## 已知問題／刻意不處理

- 舊 todo 殘留「MCP client 修復計畫」（Codex runtime、FreeCAD `node_repl` 路徑），與本專案無關，未處理：找出有效 Codex runtime 與 FreeCAD 安裝、修正 `node_repl` 與 `freecad` 啟動設定、用 CLI 與 JSON-RPC initialize 驗證。

# 發布整理與推送

- [x] 審查變更與確認排除建置產物
- [x] 將暫存建置產物移入已忽略目錄
- [x] 更新 README 與 CONTEXT 的本次發布資訊
- [x] 執行編譯／測試／服務狀態驗證
- [x] 建立 commit 並推送 `master`

# 效能優化可行性探查

- [x] 建立 `AGENTS.md` 與 `CLAUDE.md` 共用規範
- [x] 追查 CPU 調度、行程守護與記憶體整理的完整資料流
- [x] 量測目前執行檔與服務的基線成本
- [x] 修正誤判記憶體洩漏與重複親和性檢查
- [x] 根據程式與量測結果判定可行優化，補上 Review
- [x] 以系統管理員權限部署並重測新版本

# 系統列圖示：開關優化、開機自啟動

- [x] SharedUiState：暫停／開機自啟指令
- [x] 暫停時還原電源方案與親和性
- [x] `--foreground-watch` 系統列圖示
- [x] 互動模式同樣有圖示
- [x] 編譯並重啟服務（watch PID 已起來）

## MCP client 修復計畫

- [ ] 找出目前有效的 Codex runtime 與 FreeCAD 安裝狀態
- [ ] 修正 `node_repl` 路徑與 `freecad` 啟動設定
- [ ] 用 CLI 與 JSON-RPC initialize 驗證

## Review

- 2026-08-29 基線：服務 30 秒用 0.8438 秒 CPU（單核心 2.7913%）；系統列 helper 為 0%。
- `ProcessGuardian` 每 5 秒掃描全部行程；已限制的 PID 不再重複開啟與讀取親和性。
- 修正後只有單次工作集成長超過 100 MB 才會累積洩漏計數，不會再因數 MB 波動降權。
- `build/security-verify/surface_optimizer.exe` 完整編譯成功；`test_burst_policy` 30 項全過。
- 已以系統管理員權限部署；服務 RUNNING（PID 77128），執行檔 SHA-256=`69AE630A…C3F05652C`，前一版保留於已忽略的 `build/release-verify-20260829/`。
- 部署後 30 秒：服務 0.4531 CPU 秒（1.4892% 單核心），helper 0.0312 CPU 秒（0.1024%）；服務用量下降約 46.7%。
- 部署後日誌沒有新的 `Memory leak suspected`；服務只在啟動時套用 99 個行程的 OS CPU reserve。
- 圖示在使用者工作階段；Windows 11 可能先丟在系統列溢位。
- 關閉優化不會停服務，否則圖示會一起消失。

# 修復電池模式誤判為 AC 與過度升頻耗電（2026-11-13）

- [x] 修正 `GUID_ACDC_POWER_SOURCE` 常數（`service.cpp` 與 `power_manager.cpp`）為 Windows SDK 正確值 `{5d3e9a59-e9d5-4b00-a6bd-ff34ff516548}`
- [x] 在 `Service` 啟動與電量／背景巡檢時透過 `GetSystemPowerStatus` 同步 `m_current_is_ac` 與電量，避免預設 `true` 蓋掉 `PowerManager` 電池狀態
- [x] 修正 `PowerManager::evaluate_and_apply_governor` 電池 `InstantBoost` 分支改用 `dc_boost_mode`、`Min=5%`、`Unpark=25%`
- [x] 編譯 `build/fix/surface_optimizer.exe` 並執行既有測試與安全性／依賴檢查
- [x] 部署新版服務並驗證 `SurfaceOptimizer` 正確進入 DC 省電狀態
- [x] 補上 Review 與更新 `CONTEXT.md`

### Review（2026-11-13）

- 編譯與靜態檢查：`build/fix/surface_optimizer.exe`（`-O3 -flto -s -static`）無新增外部 DLL；`test_burst_policy` 36/36 全過；`audit_ipc_security.ps1` 通過。
- 部署：透過管理員 UAC 停服務、備份舊版至 `build/backup-20261113/surface_optimizer.exe`、替換執行檔並重啟 `SurfaceOptimizer`（PID `5372`，SHA-256 `8DCAD1101C7277A976770F766996A85E6FD57A8B018DEEAF8F01DA1AA0EA5FB3`）。
- 實測日誌與 `--status`：啟動後收到 `Power source transition detected: DC (Battery)`，`PERFEPP DC=50`（爆發）／`80`（閒置）、`PERFBOOSTMODE DC=0`、`PROCTHROTTLEMIN DC=5%`、`CPMINCORES DC=25%`（爆發）／`12%`（閒置）、`PCIE ASPM DC=2`。

# 電池模式降震盪與溫和省電調校（2026-11-13）

- [x] 新增 `dc_max_processor_percent`（預設 `85`，限制 `20`–`100`）並調高 `dc_epp_idle = 90`、`dc_epp_active = 70`、`idle_hysteresis_ms = 1000`
- [x] 在 `PowerManager` 備份／還原 AC/DC `GUID_PROCTHROTTLE_MAX`，並在電池模式套用 `dc_max_processor_percent`（低電量省電進一步降至 ≤70%）
- [x] 編譯 `build/fix/surface_optimizer.exe`、執行測試與安全稽核、透過 UAC 部署重啟服務並實測驗證
- [x] 補上 Review 與更新 `CONTEXT.md`

### Review（2026-11-13 調校）

- 乾淨重編 `build/fix/surface_optimizer.exe`（清掉 `build/fix_obj` 避免 header 變更造成 `-Wlto-type-mismatch`），零警告且無新增外部 DLL。
- `test_burst_policy.exe` 36/36 通過、`audit_ipc_security.ps1` 通過。
- UAC 部署重啟 `SurfaceOptimizer`（`PID 17128`，SHA-256 `163CE08FA71C460B10D29D332FDF97EB24D1D781FA19C3CF2D857B5E5B503B05`），`--status` 與日誌確認套用 `EPP=70%`/`90%`、`Boost=0`、`Min=5%`、`Max=85%`、`Unpark=25%`/`12%`、`idle_hysteresis_ms=1000`。





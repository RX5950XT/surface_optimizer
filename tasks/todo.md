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

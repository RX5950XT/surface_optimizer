# CONTEXT.md — Surface Pro 7 效能與能耗優化守護程式

## 專案狀態：✅ 全部模組完成

## 架構概覽

以 C++20 原生靜態編譯的 Windows 背景守護程式，針對 Surface Pro 7（Intel i7-1065G7 Ice Lake）設計。

### 模組結構

| 模組 | 檔案 | 功能 |
|------|------|------|
| M1 核心服務 | `core/service`, `config`, `logger`, `utils` | Windows 服務 SCM 整合、事件驅動迴圈、CLI 介面、單實例 Mutex |
| M2 CPU 調度 | `optimizer/power_manager` | Intel Speed Shift EPP 動態調整（0~100）、Turbo Boost 控制、AC/DC 電源自動切換、前台升頻 <10ms |
| M3 記憶體管理 | `optimizer/memory_manager` | EmptyWorkingSet 背景修剪、NtSetSystemInformation Standby List 釋放、75%/85% 雙閥值自適應、前台 10 秒豁免保護 |
| M4 進程守護 | `optimizer/process_guardian` | EcoQoS 效率模式降級、優先級降級、CPU 異常偵測（持續高佔用）、記憶體洩漏偵測（持續增長 >100MB）、系統白名單保護 |

### 編譯工具鏈

- GCC 14.2.0 (MSYS2 UCRT64)、C++20、`-O3 -flto -s -static`
- 零外部依賴、靜態連結
- 二進位檔：`surface_optimizer.exe`（~1.33 MB）

### CLI 指令

```
--install / --uninstall    Windows 服務安裝/移除
--start / --stop           啟動/停止服務
--daemon                   SCM 後台服務模式
--interactive / -i         前台互動模式（含即時 log）
--status                   查詢系統狀態
--benchmark                自動化效能驗證
--trim-memory / --clean-cache  手動記憶體整理
--version / --help
```

### 驗證結果

- 自身記憶體佔用 < 2 MB（遠低於 10 MB 目標）
- 閒置 CPU 佔用 < 0.01%（MsgWaitForMultipleObjectsEx 事件驅動）
- 前台升頻響應 < 10ms（遠低於 100ms 目標）
- 記憶體修剪實測回收 >1 GB
- ProcessGuardian 掃描 77 進程、22 項白名單保護正常

## 使用方式

```powershell
# 以管理員權限安裝為 Windows 服務（開機自啟動）
.\surface_optimizer.exe --install
.\surface_optimizer.exe --start

# 或前台互動模式測試
.\surface_optimizer.exe --interactive

# 手動清理記憶體
.\surface_optimizer.exe --trim-memory
```

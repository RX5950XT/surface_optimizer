# 給作業系統預留一顆實體核

- [x] `cpu_reserve.hpp` 純函式 + 單測
- [x] Guardian：非白名單行程（含前台）親和性避開第一顆實體核；shutdown 還原
- [x] 只在 daemon `enable_enforcement()` 後才套用
- [x] 停服務、編譯、單測、重啟、用親和性核對

## Review

- `test_burst_policy` 26 項全過。
- 日誌：`os_core=0x3 user=0xfc`，啟動時套到 146 個行程。
- 實測親和性：`dwm`/`csrss`/`explorer`/`surface_optimizer` = `0xff`；`notepad`/`powershell` = `0xfc`。
- 服務 `SurfaceOptimizer` RUNNING。

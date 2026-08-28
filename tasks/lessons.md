# Lessons

- 行程工作集超過 100 MB 不等於記憶體洩漏；必須以每次取樣的工作集成長量超過門檻判斷，否則會誤降權正常程式。
- 已成功套用 CPU 親和性後，不要每輪再查同一 PID；只在行程消失時移除記錄。若未來要支援外部改親和性，再以建立時間驗證 PID。
- Surface Pro 7 高效能方案把插電 `PROCTHROTTLEMIN` 釘在 100%。只改 EPP 降不了頻率，閒置一定要把 Min state 放下來。
- Session 0 的服務看不到前台視窗，也拿不到使用者的 `GetLastInputInfo`，也畫不出系統列。圖示與 LastInput 都要在使用者工作階段的 `--foreground-watch`。
- 系統列「關閉優化」不能把服務停掉，否則圖示一起死、也沒辦法再開。只能暫停調度並還原電源方案。
- 這顆 Ice Lake 是均質 4C/8T，`EfficiencyClass` 全 0，沒有偏好核。EcoQoS 只降頻、不換核。單行程要把 OS 卡死時，要用親和性留一顆實體核給系統，不能只靠 EcoQoS。
- `--status` 也會 `ProcessGuardian::initialize()`，限制 CPU 只能在 daemon `enable_enforcement()` 之後做，否則查狀態會改到別行程的親和性。
- `PowerSetActiveScheme` 讓 EPP 讀回目標，這台實測約 93 ms；不要再宣稱 <10 ms。
- CPUID.06H bit 16（PECI override）在這台是亮的：EC 可以蓋過 OS 的 HWP 要求，使用者態沒辦法硬搶。
- RAPL PL1/PL2 沒有使用者態 API，不要為了讀 MSR 去裝 WinRing0。
- 高效能方案插電仍可能把 PCIe ASPM 設成高度省電；NVMe 第一次讀會頓，插電要 Off。
- 清 Standby List 會讓下次開同一批程式更慢。16 GB 預設只修剪 Working Set。
- GPU 忙碌時 CPU 可能很閒；爆發條件要看 PDH `GPU Engine(*)`，略過 `engtype_idle`。
- toml 字串若帶引號，讀檔一定要剝掉，否則 log 路徑會變成 `"C:\..."` 寫不進去。
- 發布前要用一般使用者跑 `--status`；Global IPC 的 DACL 除了讀寫，也要允許 `SYNCHRONIZE`，否則服務雖正常但狀態會誤判未啟動。

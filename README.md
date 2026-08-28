# Surface Pro 7 效能與能耗優化守護程式

針對 **Surface Pro 7**（Intel Core i7-1065G7 / Ice Lake）設計的原生系統底層效能調度守護程式。  
以 C++20 靜態編譯，零外部依賴，常駐後台自動調度 CPU 能耗比、釋放記憶體、壓制失控進程。

---

## 核心功能

### ⚡ CPU 動態能耗調度（PowerManager）

- 透過 Intel Speed Shift 調整 **EPP**（0=最高效能 ~ 100=最省電），並在爆發時把 **最低處理器狀態拉到 100%**、叫醒 parked cores
- 這顆 i7-1065G7 **沒有大小核、沒有偏好核**；忙碌時把執行緒攤到不同實體核，不要綁死某一顆
- 前台切換、鍵盤滑鼠還在動、前景行程還在算／讀寫、**或 GPU 還在畫**時維持 InstantBoost；安靜約 250 ms 就降回去（不是死等 3 秒）
- 插電把 PCIe ASPM 關掉，避免 NVMe 睡死後第一次讀檔頓一下；電池維持高度省電
- 插電閒置時把 Min processor state 放到 5%——這台出廠高效能方案把下限釘在 100%，不放的話頻率降不下去
- 電池低於 20% 自動進入 Battery Saver（EPP=100、Turbo 關閉）

### 🧠 智慧記憶體管理（MemoryManager）

- 自動修剪背景程式的 **Working Set**（`EmptyWorkingSet`）
- **Standby List 預設不清**（清了下次開程式更慢）；只有手動 `--trim-memory`，或設定打開且 RAM ≥85% 才清
- **雙閥值自適應**：記憶體佔用 75% 觸發修剪、85% 縮短冷卻
- **前台零卡頓保護**：活躍視窗與最近 10 秒內切換過的程式一律跳過

### 🛡️ 失控進程抑制（ProcessGuardian）

- 前台立刻 **HighQoS**、背景立刻 **EcoQoS**（不必等 30 秒）
- **作業系統預留 1 顆實體核**：一般程式（含前台）不能佔第一顆核；`dwm` / `csrss` / `explorer` 等白名單仍可用全部核
- 偵測背景程式 **持續高 CPU 佔用**（>15%、超過 30 秒）→ 再降優先順序
- 偵測背景程式 **記憶體持續增長**（>100MB）→ 自動降級優先權
- **白名單保護**：系統關鍵進程（csrss、lsass、svchost、dwm、explorer 等）永不降級
- 前台不降 QoS／優先順序；但仍不能佔系統預留的那顆核

### 🔧 服務與設定

- 以 **Windows 服務** `SurfaceOptimizer` 常駐（Session 0），開機自動啟動；掛掉會 5／10／30 秒重開
- 服務會在使用者工作階段另開 `--foreground-watch`（否則看不到前台視窗）
- **沒有 GUI**。設定全在 exe 同目錄的 `surface_optimizer.toml`，改完要重啟服務才生效
- 單實例保護（Global Mutex）

---

## 效能指標

| 指標 | 目標 | 實測 |
|------|------|------|
| 常駐記憶體 | < 10 MB | **~2 MB** |
| 閒置 CPU 佔用 | < 0.1% | **< 0.01%** |
| 前台升頻（方案套用到 EPP 讀回） | < 100 ms | **~93 ms**（本機實測） |
| 二進位檔大小 | < 5 MB | **~1.35 MB** |
| 外部依賴 | 0 | **0** |

---

## 專案結構

```
surface_optimizer/
├── Makefile                          # MSYS2 GCC 編譯設定
├── build.ps1                         # PowerShell 編譯腳本（替代方案）
├── surface_optimizer.toml            # 執行期設定檔（EPP、記憶體閥值、白名單等）
│
├── include/                          # 標頭檔
│   ├── core/
│   │   ├── config.hpp                #   設定載入與結構定義
│   │   ├── logger.hpp                #   分級日誌（Debug/Info/Warn/Error）
│   │   ├── service.hpp               #   Windows 服務 SCM 整合與事件迴圈
│   │   └── utils.hpp                 #   權限管理、字串轉換、錯誤格式化
│   ├── optimizer/
│   │   ├── power_manager.hpp         #   CPU 動態能耗調度（EPP / Min state / unpark / 忙時維持）
│   │   ├── burst_policy.hpp          #   升頻維持／降頻條件（可單測）
│   │   ├── cpu_reserve.hpp           #   系統預留核的親和性計算（可單測）
│   │   ├── memory_manager.hpp        #   記憶體修剪與 Standby List 釋放
│   │   └── process_guardian.hpp      #   EcoQoS／HighQoS／系統預留核
│   └── telemetry/
│       └── platform_probe.hpp        #   唯讀 CPUID / CpuSet / PPM / MHz 探測
│
├── src/                              # 實作檔
│   ├── core/
│   │   ├── config.cpp
│   │   ├── logger.cpp
│   │   ├── service.cpp               #   主事件迴圈（MsgWaitForMultipleObjectsEx）
│   │   └── utils.cpp
│   ├── optimizer/
│   │   ├── power_manager.cpp
│   │   ├── memory_manager.cpp
│   │   └── process_guardian.cpp
│   ├── telemetry/
│   │   └── platform_probe.cpp
│   └── main.cpp                      # CLI 進入點與指令路由
│
├── scripts/                          # 輔助腳本
│   ├── install_service.ps1           #   一鍵安裝服務（自動提升管理員權限）
│   └── uninstall_service.ps1         #   一鍵移除服務
│
└── tests/                            # 測試套件
    ├── e2e_runner.ps1                #   4-Tier 自動化 E2E 測試（145+ 項）
    ├── test_m2_challenger.ps1        #   CPU 調度對抗性測試
    ├── test_m3_stress.ps1            #   記憶體壓力測試
    ├── test_burst_policy.cpp         #   忙時維持／閒置降頻策略單測
    ├── test_ramp_latency.cpp         #   前台升頻延遲精密測量
    ├── forensic_m3_verifier.cpp      #   記憶體修剪鑑識驗證
    ├── test_m3_empirical_challenger.cpp
    ├── audit_imports.py              #   二進位依賴審計
    ├── audit_symbols.ps1             #   符號表審計
    └── synthetic/                    #   合成測試負載
        ├── synth_cpu_hog.cpp         #     模擬 CPU 狂飆進程
        ├── synth_mem_leak.cpp        #     模擬記憶體洩漏進程
        ├── synth_burst_app.cpp       #     模擬突發負載應用
        ├── synth_idle_bloat.cpp      #     模擬閒置肥大進程
        ├── Makefile
        └── build_synthetic.ps1
```

---

## 系統需求

- **作業系統**：Windows 10 / 11（推薦 Windows 11 以完整支援 EcoQoS）
- **硬體**：Surface Pro 7 或其他 Intel 10 代以上處理器裝置
- **權限**：管理員權限（安裝服務與完整記憶體管理功能）
- **編譯環境**（僅開發需要）：MSYS2 UCRT64 + GCC 14.2+

---

## 編譯

```bash
# 在 MSYS2 UCRT64 終端中
make -j4

# 或使用 PowerShell
.\build.ps1
```

產出：`surface_optimizer.exe`（~1.33 MB，靜態連結，零外部 DLL 依賴）

---

## 使用方式

### 常駐後台（Windows 服務，開機自啟）

這台機器上服務名是 `SurfaceOptimizer`，啟動類型 **自動**，開機就跑，沒有系統列圖示。

```powershell
# 方法一：一鍵腳本（會跳 UAC）
.\scripts\install_service.ps1

# 方法二：管理員身分
.\surface_optimizer.exe --install     # 註冊為自動啟動
.\surface_optimizer.exe --start       # 立刻跑
.\surface_optimizer.exe --status      # 看是否 RUNNING
```

改 `surface_optimizer.toml` 之後：

```powershell
.\surface_optimizer.exe --stop
.\surface_optimizer.exe --start
```

### 前台互動模式（測試用）

```powershell
.\surface_optimizer.exe --interactive
```

即時顯示調度日誌，按 Ctrl+C 停止。

### 其他指令

```powershell
.\surface_optimizer.exe --status        # 查看系統狀態（電源、記憶體、進程守護）
.\surface_optimizer.exe --benchmark     # 自動化效能驗證
.\surface_optimizer.exe --trim-memory   # 手動清理背景記憶體
.\surface_optimizer.exe --clean-cache   # 同上（別名）
.\surface_optimizer.exe --stop          # 停止服務
.\surface_optimizer.exe --uninstall     # 移除服務
.\surface_optimizer.exe --version       # 版本資訊
.\surface_optimizer.exe --help          # 完整說明
```

---

## 設定檔

**沒有設定視窗。** 參數寫在 `surface_optimizer.toml`（與 exe 同目錄），改完重啟服務：

| 區段 | 參數 | 預設值 | 說明 |
|------|------|--------|------|
| `[power]` | `ac_epp_idle` | 60 | 插電閒置時的 EPP（0=全效能, 100=全省電） |
| | `ac_epp_active` | 0 | 插電爆發時的 EPP |
| | `dc_epp_idle` | 80 | 電池閒置時的 EPP |
| | `dc_boost_mode` | 0 | 電池閒置時 Turbo Boost（0=關, 2=激進） |
| | `fast_ramp_duration_ms` | 3000 | 讀不到前景行程時的後備維持時間 |
| | `busy_poll_interval_ms` | 100 | 爆發期間檢查間隔 |
| | `idle_hysteresis_ms` | 250 | 安靜多久才降頻 |
| | `busy_gpu_percent_threshold` | 15.0 | GPU 還在畫就維持爆發（%） |
| | `battery_saver_threshold_percent` | 20 | 低電量 Battery Saver 觸發閾值 |
| `[memory]` | `pressure_threshold_percent` | 75 | 記憶體修剪觸發閾值（%） |
| | `trim_interval_seconds` | 60 | 修剪冷卻時間（秒） |
| | `enable_standby_purge` | false | 自動清 Standby（僅 RAM≥85% 才會真的清） |
| `[governor]` | `cpu_hog_threshold_percent` | 15.0 | CPU 異常偵測閾值（%） |
| | `cpu_hog_sustain_seconds` | 30 | 持續超標多久才降級（秒） |
| | `enable_eco_qos` | true | 前台 HighQoS、背景 EcoQoS |
| | `allowlist` | *見檔案* | 白名單進程（永不降級） |
| `[daemon]` | `housekeeping_interval_ac_ms` | 5000 | 插電時巡查間隔（ms） |
| | `housekeeping_interval_dc_ms` | 15000 | 電池時巡查間隔（ms） |
| | `log_level` | INFO | 日誌等級（DEBUG / INFO / WARN / ERROR） |

---

## 測試

```powershell
# 執行完整 4-Tier E2E 測試（需以管理員身分執行）
.\tests\e2e_runner.ps1 -Tier all

# 僅執行特定 Tier
.\tests\e2e_runner.ps1 -Tier 1    # Feature Coverage（22 功能點）
.\tests\e2e_runner.ps1 -Tier 2    # Boundary & Corner Cases
.\tests\e2e_runner.ps1 -Tier 3    # Cross-Feature Interactions
.\tests\e2e_runner.ps1 -Tier 4    # Real-World Scenarios

# 編譯合成測試負載
cd tests\synthetic && .\build_synthetic.ps1

# 忙時維持／閒置降頻策略單測
g++ -std=c++20 -Iinclude tests/test_burst_policy.cpp -o tests/test_burst_policy.exe
.\tests\test_burst_policy.exe
```

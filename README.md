# Surface Pro 7 效能與能耗優化守護程式

針對 Surface Pro 7（Intel i7-1065G7 Ice Lake）設計的原生系統底層效能調度守護程式。

## 功能

- **CPU 動態能耗調度**：依據負載與電源狀態（AC/DC），即時調整 Intel Speed Shift EPP 與 Turbo Boost
- **智慧記憶體釋放**：自適應修剪背景程式工作集與系統待機快取
- **失控進程自動抑制**：偵測背景異常高 CPU 與記憶體洩漏進程，自動套用 EcoQoS 效率模式
- **極致輕量**：零外部依賴、靜態編譯、常駐記憶體 < 2 MB、閒置 CPU < 0.01%

## 系統需求

- Windows 10/11（推薦 Windows 11 以支援 EcoQoS）
- 管理員權限（安裝服務與完整記憶體管理功能）
- MSYS2 UCRT64 GCC 14.2+（編譯用）

## 編譯

```bash
# 在 MSYS2 UCRT64 環境下
make -j4
```

## 使用

```powershell
# 安裝為 Windows 服務（開機自啟動、自動復原）
.\surface_optimizer.exe --install
.\surface_optimizer.exe --start

# 前台互動模式（含即時日誌）
.\surface_optimizer.exe --interactive

# 查看系統狀態
.\surface_optimizer.exe --status

# 手動清理記憶體
.\surface_optimizer.exe --trim-memory

# 效能驗證
.\surface_optimizer.exe --benchmark

# 解除安裝
.\surface_optimizer.exe --uninstall
```

## 授權

私有專案

# Surface Optimizer 開發規範

- 使用繁體中文（臺灣用語）；先理解完整資料流，再做最小且可驗證的修改。
- 程式碼相關工作先遵循 `karpathy-guidelines`；非簡單任務在 `tasks/todo.md` 留下計畫與 Review。
- 每次修改後執行對應測試並回報實際輸出；不碰既有無關變更。
- README、`AGENTS.md`、`CONTEXT.md` 是專案共同規範；架構或規則實質變動時同步更新。
- 不硬編碼 secrets；驗證外部輸入，錯誤不可靜默吞掉。

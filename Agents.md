# Cursor Agent Configuration

设计/Plan 相关任务须遵循 `.cursor/rules/design-docs.mdc` 的命名约定（plan_*.md / todo-*.md / *-design.md）。

## Plan 流程

复杂任务（满足任一即触发）必须**先出 Plan，等用户确认后再改代码**：
- 涉及 3+ 个不同模块
- 新增模块/抽象接口/外部依赖
- 数据结构或接口不兼容改动
- 用户意图含"设计"、"重构"、"优化架构"等关键词
- 目标模糊，需拆解

**Plan 保存至 `design/plan_<简述>.md`**（详见 design-docs 规则），格式：
- **Goal**：要解决的问题
- **Steps**：分步方案，含文件路径
- **Risks & Assumptions**
- **To Confirm**：需用户确认的问题

简单任务（单文件 bugfix、小范围补充）直接执行。

## 完成检查

代码改完后，依次确认：
1. **构建与测试**：`cd build && cmake --build . && ctest --output-on-failure`
2. **文档同步**：涉及模块/配置/依赖/用法/架构变更时，检查并更新：
   - `README.md`（架构图、特性、构建、依赖表、用法、项目结构）
   - `config/example.yaml`（与 `Config` 结构体保持一致）
   - `design/` 下相关 Plan（偏离原 Plan 时补充说明，标记 `> Status: Done`）
   - **`docs/` 下设计/说明文档**（如 `docs/client-standalone-design.md`、`docs/usage.md`）：
     - 若本次改动涉及文档中描述的 API、文件、模块或流程，须同步更新文档中的说明与示例。
     - 若文档内含有**指向源码的链接**（形如 `../src/.../file.h#L行号`），在修改/移动对应代码或增删行后，须**更新这些链接的行号或路径**，保证 Cmd+点击 仍能正确跳转到目标位置。
     - **新增代码**：若该模块有对应设计文档（如 `docs/client-standalone-design.md`），须在相关源文件顶部加 `// Design: docs/xxx.md (§x.x …)` 注释，便于从代码跳回文档。
3. 向用户报告变更文件列表

**失败时**：说明卡点和已完成部分，等待指令；**不擅自降级需求或跳过完成检查项**。

## 编码纪律

- **范围**：以用户意图为边界；超预期的大范围改动提前说明。
- **依赖**：不引入不必要依赖；确需时走 `find_package + FetchContent`。
- **测试**：功能变更必须附带测试；测试放在 `tests/` 下，目录结构镜像 `src/`（如 `src/client/` 对应 `tests/client/`）。
- **透明**：歧义时提问，复杂逻辑加注释。
- **语言**：代码注释英文，设计/使用文档中文。

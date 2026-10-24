# tugmcp designs

本目录是 `tugmcp` 的设计权威。代码实现前先读这里；实现与设计冲突时，先改设计再改代码。

## 索引

| 文档 | 内容 | 读者 |
|---|---|---|
| [`mcp-library-design.md`](./mcp-library-design.md) | 库的分层、API、协议、传输、schema、并发、安全、打包、里程碑 | 实现者/评审者 |
| [`host-adapter.md`](./host-adapter.md) | 宿主应用如何消费本库：进程内、执行器、安全/审批、工具映射 | 宿主适配开发者 |
| [`protocol-decisions.md`](./protocol-decisions.md) | 已定协议/传输/依赖决策（ADR） | 所有人 |
| [`header-only.md`](./header-only.md) | header-only 可行性、取舍与工量评估 | 架构/构建负责人 |
| [`testing.md`](./testing.md) | 测试分层/框架/目录与 target/CI | 实现者/CI 维护者 |

## 建议阅读顺序

1. `mcp-library-design.md`：先看整体分层和 API。
2. `protocol-decisions.md`：确认已定决策，避免重复讨论。
3. `header-only.md`：决定分发形态。
4. `testing.md`：动手写代码前确定测试落点。
5. `host-adapter.md`：如果要做宿主应用集成。

## 文档关系

- 本目录文档自包含，不引用外部仓库。
- `mcp-library-design.md` 是通用库设计；`host-adapter.md` 是宿主适配边界摘要。
- 实现与设计冲突时，先改设计再改代码。

## 变更规则

- 公共 API、协议 wire 行为、传输、依赖、header-only 策略变更：先更新本目录，再改代码。
- 决策变更写入 `protocol-decisions.md`，不要只改代码或聊天记录。
- 设计文档中的 C++ 代码块是接口草图，未编译；不要把它们当作已实现。

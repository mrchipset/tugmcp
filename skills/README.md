# tugmcp HTTP 项目 skills

本目录是 `tugmcp` 的 Streamable HTTP 传输专用 skill 集合，供 agent 在处理
HTTP 开发、调试、测试时加载。每个子目录包含一个 `SKILL.md`。

| Skill | 何时使用 |
|---|---|
| [`tugmcp-http-development`](tugmcp-http-development/SKILL.md) | 修改 `tugmcp::http`、`/mcp` 路由、2026/2025 协议行为、SSE/session/TLS、cpp-httplib 边界或单头产物 |
| [`tugmcp-http-debugging`](tugmcp-http-debugging/SKILL.md) | HTTP 测试失败、conformance/Inspector 报错、4xx/5xx、SSE 挂起、session/TLS 异常 |
| [`tugmcp-http-testing`](tugmcp-http-testing/SKILL.md) | 新增/运行 HTTP 测试、loopback、TLS、legacy、conformance、amalgamation |

## Bundle

`bundle/` 是随 skills 发布的配置快照：根/tests CMake、doctest 与 hash 校验
cmake、conformance `expected_failures.yaml`、单头 `*.sha256`。SKILL.md 中的
`bundle/...` 路径以本目录（`skills/`）为基准；发布时把整个 `skills/` 复制走，
并保持三个 `tugmcp-http-*/` 目录与 `bundle/` 同级。

刷新快照：

```powershell
pwsh -File bundle/refresh.ps1
```

快照布局与刷新见 [`bundle/README.md`](bundle/README.md)；来源、commit 与
SHA256 见 [`bundle/MANIFEST.md`](bundle/MANIFEST.md)。

## 镜像位置

Git 仓库只跟踪 `skills/`。本地工作区另有 `.opencode/skills/` 供 opencode
自动扫描，该目录被 `.gitignore` 忽略；修改后请同步两处。

## 使用方式

opencode 会自动扫描 `.opencode/skills/**/SKILL.md`。新增或修改 skill 后需要
**退出并重启 opencode**，当前会话不会热加载。

也可以直接读取对应 `SKILL.md`，按其中的命令与检查表执行。

## 权威来源

Skill 只做工作流索引，不替代以下文档：

1. `AGENTS.md`：仓库约束与架构不变量。
2. `designs/protocol-decisions.md`：D2/D3/D14/D18/D19/D20 是 HTTP 行为与
   wire 的决策来源；改协议先改这里。
3. `designs/testing.md`：测试分层与 CI 口径（部分 target 是规划，实际以
   `tests/CMakeLists.txt` 为准；发布快照 `bundle/tests/CMakeLists.txt`）。
4. `docs/api.md` §7.3、§11.6：HTTP API 与错误/状态映射。
5. `docs/integration.md` §5–§6、§11–§12：宿主接入、部署边界、故障排查。
6. `docs/building.md`：CMake 开关、target、标签与命令。
7. `docs/conformance-report.md`：外部 Inspector/conformance 证据与已知偏差。
8. `docs/amalgamation.md`：单头产物、哈希与外部验证。

## 基准

- 记录日期：2026-10-27；skills 基线 commit `dbbdfde`。
- 配置快照：commit `6273026`（见 `bundle/MANIFEST.md`）。
- 默认 Debug 构建：267 个 ctest 测试（含 `amalgamation` 3 个）。
- 参考基线（`docs/building.md`，2026-10-25）：默认 267/267；TLS 269/269；
  legacy 275/275；schema 198/198；conformance 2/2；amalgamation 3/3。
- 命令中的 `build/`、`build-tls/`、`build-conformance/` 均为示例目录。

## 维护规则

- HTTP 行为、状态码、header、session、TLS 或测试命令变化时，同步更新对应
  `SKILL.md`，并在 `designs/protocol-decisions.md` 留下决策记录。
- 修改 `CMakeLists.txt`、`tests/CMakeLists.txt`、conformance 账本或单头 hash
  后，运行 `pwsh -File bundle/refresh.ps1`，再同步 `.opencode/skills`。
- 不要在这三个 skill 里复制大段设计正文；引用权威文档，保留可执行命令、
  文件路径、错误 `reason` 与检查表。
- 新增 skill 时保持 `name` 与目录名一致，`description` 同时写“做什么”和
  “何时触发”。

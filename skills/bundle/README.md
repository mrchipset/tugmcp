# tugmcp HTTP skills bundle

本目录是 `tugmcp-http-*` skills 的**配置快照**。目的：skills 被复制到没有
tugmcp 仓库的环境时，仍能读到 target、label、超时、conformance 失败账本和
单头 hash 的原始配置，而不是指向不存在的路径。

## 布局

| Bundle 文件 | 仓库源文件 | 用途 |
|---|---|---|
| `CMakeLists.txt` | `CMakeLists.txt` | 根 target、构建开关、依赖 wiring |
| `cmake/doctest.cmake` | `cmake/doctest.cmake` | doctest 测试框架接入 |
| `cmake/verify_hashes.cmake` | `cmake/verify_hashes.cmake` | `3rd/` 依赖 hash 校验 |
| `cmake/verify_manifest.cmake` | `cmake/verify_manifest.cmake` | amalgamation manifest 校验 |
| `tests/CMakeLists.txt` | `tests/CMakeLists.txt` | 测试 target、label、timeout、注册 |
| `tests/conformance/expected_failures.yaml` | `tests/conformance/expected_failures.yaml` | conformance 已知未计分账本 |
| `tests/amalgamation/CMakeLists.txt` | `tests/amalgamation/CMakeLists.txt` | 单头测试 target 与外部 gating |
| `tests/amalgamation/require_external.cmake` | `tests/amalgamation/require_external.cmake` | 外部 Inspector/conformance 前置条件 |
| `tests/amalgamation/core.sha256` | 同源 | core 单头 hash |
| `tests/amalgamation/http.sha256` | 同源 | HTTP 单头 hash |
| `tests/amalgamation/http_tls.sha256` | 同源 | HTTP+TLS 单头 hash |

`MANIFEST.md` 记录快照来源 commit、生成日期和每个文件的 SHA256。

## 权威性

- **在 tugmcp 仓库内**：以仓库原文件为准；`bundle/` 只是便于对照的快照。
- **在仓库外**：`bundle/` 是唯一随 skills 发布的配置参考；源码、公共头、
  文档、测试 `.cpp`、`tools/`、`3rd/` 仍需从仓库获取。
- `bundle/` **不是可独立 configure 的源码树**。`CMakeLists.txt` 中的
  `src/`、`tests/`、`3rd/` 相对路径在 bundle 内不完整；不要直接拿它构建。

## 刷新

从仓库根：

```powershell
pwsh -File skills/bundle/refresh.ps1
```

或在 bundle 目录：

```powershell
pwsh -File refresh.ps1
```

如果 bundle 已复制到仓库外，显式指定仓库根：

```powershell
pwsh -File refresh.ps1 -RepoRoot <path-to-tugmcp>
```

脚本会按上表复制文件并重新生成 `MANIFEST.md`。修改仓库配置后请重新运行，
保持 bundle 与源文件一致。

## 发布

把整个 `skills/` 目录复制走即可；`tugmcp-http-development/`、
`tugmcp-http-debugging/`、`tugmcp-http-testing/` 必须与 `bundle/` 保持同级，
因为 SKILL.md 中的 `bundle/...` 路径以 `skills/` 根为基准。

Git 只跟踪 `skills/`；`.opencode/skills/` 是本地 opencode 副本，已被
`.gitignore` 忽略。发布前可用 `refresh.ps1` 后比较两处文件 hash。

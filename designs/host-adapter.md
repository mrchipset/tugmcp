# 宿主应用适配边界（tugmcp 消费者）

> 本文说明宿主应用（host application）如何消费 `tugmcp`。库本身不包含任何宿主业务逻辑或设备逻辑；协议与传输归库，业务、设备与安全归宿主。

## 1. 结论

- **进程内嵌**：`tugmcp` 以静态库或单头发布 artifact 方式链接进宿主应用；不新建第二个进程、不创建第二个 UI 主循环实例。split-header `TUGMCP_HEADER_ONLY` 模式未实现。
- **库只做协议/传输**：MCP server/client、stdio/HTTP、JSON-RPC、schema 校验、cancel/progress、auth/approval hooks。
- **宿主做业务与安全**：任务、业务操作、参数、硬件控制、外设、文件、审计、审批、设备联锁留在宿主适配层。
- **不覆盖现有 UI 回调**：宿主的业务事件回调可能是单槽；适配层必须做 fan-out，保留原有 UI 消费者。

## 2. 非协商约束

1. 不启动第二个宿主应用实例；客户端不能通过 stdio 让宿主重新初始化硬件。
2. 所有设备命令走宿主的单一执行入口；库的 HTTP/SSE 线程不得直接调用宿主设备接口。
3. 所有 UI 操作走宿主 UI 调度器（`IUiDispatcher`）；库不 include 任何 UI 工具包头。
4. 单活跃任务由宿主强制（`ITaskManager`）；库只维护 MCP 侧 request/cancel bookkeeping。
5. 硬件联锁/安全条件由宿主与硬件强制；库的 `IApprovalHook`/`IAuthenticator` 只是钩子。
6. `record=false` 是 MCP 触发工作的默认；需要落盘时由 `IRecordingService` 单写者管理。
7. 参数写权威在宿主：`IParameterService` 单写者；UI 与 MCP 通过版本/冲突协调。
8. 不把设备安全、参数范围、任务完成语义放进 `tugmcp` 核心。

## 3. 边界划分

| 能力 | `tugmcp` | 宿主适配层 |
|---|---|---|
| JSON-RPC/MCP 编解码 | 是 | 否 |
| 2026/2025 lifecycle/headers | 是 | 否 |
| stdio/Streamable HTTP 传输 | 是 | 配置/启动 |
| schema 校验 | 是（可插拔） | 提供 schema/validator |
| auth/approval/audit | hook/context | 策略、审批 UX、审计存储 |
| tool/resource/prompt 注册 | 是 | handler 实现 |
| 任务状态/单活跃 | 仅 request/cancel 原语 | `ITaskManager` |
| 设备/参数/硬件控制 | 否 | `IDeviceService`/`IParameterService` |
| 业务事件 fan-out | 否 | 宿主事件适配器 |
| 文件/录制 | 否 | `IDataCatalog`/`IRecordingService` |
| 设备安全/联锁 | 否 | 宿主 + 硬件 |
| UI | 否 | 宿主 |

## 4. 宿主服务到 MCP 工具的映射

工具名以宿主应用的 MCP 设计文档为准；`tugmcp` 只提供注册/分发能力。

| 宿主域 | 典型工具 | 库侧机制 |
|---|---|---|
| 状态/遥测/预检 | `app_status`、`app_telemetry`、`app_preflight` | `IToolHandler` + `RequestContext` |
| 业务执行/任务 | `app_task_start_*`、`app_task_stop`、`app_task_*` | tool + cancellation/progress；任务语义在宿主 |
| 参数 | `app_params_*` | tool；参数权威/冲突在宿主 |
| 硬件控制 | `app_hw_open/close` | tool + `IApprovalHook`；联锁在宿主 |
| 外设 | `app_peripheral_*` | tool；串行化在宿主 |
| 数据流 | `app_streams_*` | resource/tool；数据源在宿主 |
| 文件 | `app_file_*` | tool；路径/单写者在宿主 |
| 资源模板 | `app://tasks/{id}`、`app://files/{id}` | `IResourceProvider::templates()` |
| 资源更新 | 状态/任务/遥测 | `notifyResourceUpdated`/`notifyListChanged` |

## 5. 线程与生命周期

```text
HTTP/SSE thread (tugmcp)
  -> JSON-RPC dispatcher (tugmcp)
    -> IExecutor::post(Priority) (host wrapper)
      -> host single execution thread
        -> host device/business services
    -> IUiDispatcher (host, for UI-only work)
```

- `tugmcp` 的 HTTP worker 只解析/校验/入队；不阻塞、不直接碰设备。
- 宿主注入 `IExecutor` 包装单一执行线程，映射 `Priority{Normal,High,Emergency}`。
- 紧急请求走宿主的 dedicated admission/`/emergency`，绕过普通 rate limit/队列满/owner；库只负责把请求送到。
- 宿主的业务事件回调可能是单槽：安装一个 composite sink，先转发给原有 UI 消费者，再复制业务数据到有界事件队列；不得阻塞业务线程。
- 关闭顺序：停止新请求 → 取消/等待 handler → 关闭 SSE/transport → drain executor → join。

## 6. 安全与审批

- `tugmcp` 不提供设备安全结论；`IAuthenticator`/`IAuthorizer`/`IApprovalHook`/`IAuditHook` 由宿主实现。
- `confirmDangerous` 只是客户端提示，不是审批证明；宿主必须有 server-side approval record（UI 或 MRTR `input_required`）。
- 审批必须有 deadline/cancel/deny reason；不能无限占用 executor。
- 硬件联锁/安全条件由宿主的安全谓词与硬件决定；库不提供 bypass。
- 日志脱敏；不记录 token/Authorization/完整参数。

## 7. 录制与文件

- MCP 触发的工作默认 `record=false`（live-only）。
- `record=true` 时由宿主的 `IRecordingService` 拥有 `FileWrite`；`tugmcp` 不直接写数据文件。
- 文件读取用 `FileRead`-based serializer；单读者串行化；路径 canonicalize/allowlist。
- 不宣称原子保存、CRC、setpoint readback——这些当前不存在，除非宿主后续实现。

## 8. 宿主侧前置条件（启用写控制前）

在启用 MCP 写控制前，宿主必须先关闭这些 P0：

- 工作线程 join/生命周期、队列所有权、共享状态原子化、状态/遥测快照一致性。
- 业务事件 fan-out / 移除 per-start 覆盖、单活跃任务仲裁接入 UI 与 MCP。
- 文件读写防护（空文件/定位失败/stop+flush）、硬件联锁拆分、非模态安全路径。
- 审计/审批、参数写权威、硬件控制与安全联锁验证。

## 9. 集成方式

### 9.1 静态库（推荐起步）

- 宿主链接 `tugmcp::core` + `tugmcp::http`（+ 可选 schema target）。
- `TUGMCP_TLS=ON` 时还需 OpenSSL ≥3.0（`OpenSSL::SSL`/`OpenSSL::Crypto`），Windows 追加 `crypt32`；同一进程只能有一套 OpenSSL。
- 不提供 `install(EXPORT)`，以源码内嵌方式集成。
- 好处：编译隔离、编译时间可控、公共 ABI 用 pimpl。
- 需要固定 `nlohmann/json` 与 `cpp-httplib` 版本/commit。

### 9.2 header-only

- core/protocol/stdio 可 header-only；HTTP 的 cpp-httplib 仅经 `detail/http_httplib.hpp` 引入，建议可选。
- 好处：无库 ABI、集成简单；代价：宿主编译时间/二进制增大。
- 细节见 `header-only.md`。

## 10. 不做的事

- 不在库内 include 宿主的业务/设备头文件。
- 不让 MCP 客户端启动第二个宿主进程。
- 不用 skill 代替 server 安全强制。
- 不把 `app_emergency_stop` 走普通 rate-limited/queue-limited 路径。

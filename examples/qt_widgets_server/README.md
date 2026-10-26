# Qt Widgets MCP server example

A runnable **Qt 5 Widgets** host that embeds a `tugmcp` **Streamable HTTP**
server. It shows how a GUI application can expose tools without ever touching a
`QWidget` from a non-GUI thread.

The server negotiates **2026-07-28** first and falls back to **2025-11-25**
(the library default). The example itself always talks Streamable HTTP; it does
not use stdio or legacy HTTP+SSE.

## What it demonstrates

| Tool | Runs on | What it proves |
|---|---|---|
| `ui_set_text` | MCP executor thread → **GUI thread** via `QMetaObject::invokeMethod(..., Qt::BlockingQueuedConnection)` | Widget mutation is marshalled to the GUI thread; the handler returns only after the GUI applied the change. |
| `ui_get_state` | MCP executor thread → **GUI thread** (same marshalling) | Reads the live `QLabel` text plus `guiThreadId`, `updateCounter`, and a GUI heartbeat counter. |
| `worker_job` | dedicated `std::thread` | Reports progress through `IProgressSink::report`, polls `RequestContext::cancellation`, and returns the handler/worker/GUI thread ids. The executor thread joins the worker; the GUI thread is never blocked. |

The window also has a 100 ms GUI heartbeat timer, so a caller can poll
`ui_get_state` while `worker_job` is running and see the heartbeat advance.

## Build

Requirements:

- CMake ≥ 3.20 and an MSVC toolchain (this example was built with VS 2022).
- Qt 5.15.2 MSVC 2019 64-bit, e.g. `C:/Qt/5.15.2/msvc2019_64`.
- `TUGMCP_WITH_HTTP=ON` (the default) and `TUGMCP_BUILD_EXAMPLES=ON`.

`TUGMCP_BUILD_QT_EXAMPLE` is **OFF** by default. When it is OFF, CMake never
looks for Qt. When it is ON, CMake requires Qt5 Widgets and fails loudly if the
prefix path is wrong.

```powershell
cmake -S . -B build-qt ^
  -DTUGMCP_BUILD_QT_EXAMPLE=ON ^
  -DTUGMCP_BUILD_TESTS=OFF ^
  -DTUGMCP_BUILD_EXAMPLES=ON ^
  -DCMAKE_PREFIX_PATH=C:/Qt/5.15.2/msvc2019_64

cmake --build build-qt --config Debug --parallel
```

Executable (Visual Studio generator):

```text
build-qt\examples\Debug\tugmcp_qt_widgets_server.exe
```

## Run

Put the Qt DLL directory on `PATH` (Debug builds need `Qt5Cored.dll` etc.):

```powershell
$env:PATH = "C:\Qt\5.15.2\msvc2019_64\bin;$env:PATH"
.\build-qt\examples\Debug\tugmcp_qt_widgets_server.exe --port 18090
```

The app prints the actual bound URL to stderr:

```text
tugmcp-qt: listening on http://127.0.0.1:18090/mcp
```

Options:

```text
--bind ADDR          listen address (default 127.0.0.1)
--port N             listen port, 0 = ephemeral (default 0)
--exit-after-ms N    test hook: close and shut down cleanly after N ms
--help
```

Headless launch (no visible window):

```powershell
$env:QT_QPA_PLATFORM = "offscreen"
$env:PATH = "C:\Qt\5.15.2\msvc2019_64\bin;$env:PATH"
.\build-qt\examples\Debug\tugmcp_qt_widgets_server.exe --port 18090 --exit-after-ms 30000
```

Closing the window, pressing **Stop server and quit**, or using
`--exit-after-ms` performs an orderly shutdown (see below).

## Raw 2026-07-28 requests

Every 2026 request needs `MCP-Protocol-Version`, `Mcp-Method`, and a
`params._meta` object with `io.modelcontextprotocol/protocolVersion` and
`io.modelcontextprotocol/clientCapabilities`. `tools/call` additionally needs
`Mcp-Name`.

Discover:

```bash
curl -sS -X POST http://127.0.0.1:18090/mcp \
  -H "Content-Type: application/json" \
  -H "MCP-Protocol-Version: 2026-07-28" \
  -H "Mcp-Method: server/discover" \
  -d '{"jsonrpc":"2.0","id":1,"method":"server/discover","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientCapabilities":{}}}}'
```

List tools:

```bash
curl -sS -X POST http://127.0.0.1:18090/mcp \
  -H "Content-Type: application/json" \
  -H "MCP-Protocol-Version: 2026-07-28" \
  -H "Mcp-Method: tools/list" \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientCapabilities":{}}}}'
```

Set the label, then read it back:

```bash
curl -sS -X POST http://127.0.0.1:18090/mcp \
  -H "Content-Type: application/json" \
  -H "MCP-Protocol-Version: 2026-07-28" \
  -H "Mcp-Method: tools/call" \
  -H "Mcp-Name: ui_set_text" \
  -d '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"ui_set_text","arguments":{"text":"hello from MCP"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientCapabilities":{}}}}'

curl -sS -X POST http://127.0.0.1:18090/mcp \
  -H "Content-Type: application/json" \
  -H "MCP-Protocol-Version: 2026-07-28" \
  -H "Mcp-Method: tools/call" \
  -H "Mcp-Name: ui_get_state" \
  -d '{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"ui_get_state","arguments":{},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientCapabilities":{}}}}'
```

Run a worker job:

```bash
curl -sS -X POST http://127.0.0.1:18090/mcp \
  -H "Content-Type: application/json" \
  -H "MCP-Protocol-Version: 2026-07-28" \
  -H "Mcp-Method: tools/call" \
  -H "Mcp-Name: worker_job" \
  -d '{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"worker_job","arguments":{"iterations":10,"step_ms":50},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientCapabilities":{}}}}'
```

The structured result contains `handlerThreadId`, `workerThreadId`,
`guiThreadId`, `progressReports`, `cancelled`, `completed`, and `elapsedMs`.
`workerThreadId` and `guiThreadId` are different.

### Progress over SSE

Progress notifications are emitted on the request route. To receive them, the
client must ask for an SSE response (`Accept: text/event-stream`) and include a
`params._meta.progressToken`. With a plain JSON POST the transport drops
notifications for that route; `progressReports` in the final result still counts
the reports made by the worker.

### Cancellation

`worker_job` polls `RequestContext::cancellation`. In 2026 stateless mode the
library has no session-scoped cancellation; it still matches a best-effort
`notifications/cancelled` request id across routes and cancels all in-flight
tokens during server shutdown. A 2026 notification POST must carry its own
`_meta` and `Mcp-Method: notifications/cancelled`:

```bash
curl -sS -X POST http://127.0.0.1:18090/mcp \
  -H "Content-Type: application/json" \
  -H "MCP-Protocol-Version: 2026-07-28" \
  -H "Mcp-Method: notifications/cancelled" \
  -d '{"jsonrpc":"2.0","method":"notifications/cancelled","params":{"requestId":5,"reason":"test cancel","_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientCapabilities":{}}}}'
```

The original `worker_job` response should then report `"cancelled": true` and
`"completed": false`.

### MCP Inspector

Inspector 2.12 may negotiate 2025 fallback; use it as an additional check, not
a replacement for the raw 2026 requests above. `--protocol-era auto` lets it
negotiate 2026 when available; the default is `legacy`.

```powershell
npx @modelcontextprotocol/inspector@2.12.0 --cli --server-url http://127.0.0.1:18090/mcp --transport http --protocol-era auto --method tools/list --format json

npx @modelcontextprotocol/inspector@2.12.0 --cli --server-url http://127.0.0.1:18090/mcp --transport http --protocol-era auto --method tools/call --tool-name ui_get_state --tool-args-json "{}" --format json
```

## Thread boundaries and shutdown

```text
GUI thread (QApplication)
  QMainWindow, QLabel, QPlainTextEdit, GuiBridge, heartbeat QTimer
        ^                                   |
        | BlockingQueuedConnection          | QueuedConnection (log only)
        |                                   v
tugmcp executor thread(s)  ---- joins --->  worker std::thread (worker_job)
  IToolHandler::invoke                        loops, report(), cancellation poll
        ^
        | transport callbacks
server std::thread  -- McpServer::run() -->  Streamable HTTP transport
```

- Widgets are only touched on the GUI thread. `ui_set_text` and `ui_get_state`
  use `Qt::BlockingQueuedConnection`; the handler thread waits, the GUI thread
  does not block (it services the queued call).
- `worker_job` never touches Qt. It runs on its own `std::thread`; the MCP
  handler joins it on the executor thread. Progress and cancellation are
  observed on the worker.
- The MCP server runs on a dedicated `std::thread`. Shutdown is started from
  `closeEvent` but executed on a third thread: `McpServer::stop()` cancels
  in-flight tokens, `run()` drains the executor, the server thread is joined,
  and only then is the `McpServer` destroyed. This ordering lets a handler that
  is waiting on a `BlockingQueuedConnection` finish instead of deadlocking
  against the closing GUI.
- After shutdown the window is hidden and the app quits; the bridge and widgets
  stay alive until the server thread has joined, so no tool callback can fire
  against a destroyed `QWidget`.
- `progress.report()` is called from the worker thread. Notifications for a
  plain JSON route are discarded by the transport; use SSE if you need to see
  them in the client.

## Limitations

- The example deliberately sets `ServerOptions::allowUnvalidatedTools = true`
  because no schema-validation backend is linked. Production hosts should
  inject a `SchemaValidator` and keep the fail-closed default.
- The default bind is loopback (`127.0.0.1`); there is no auth, TLS, approval,
  or audit hook in this example. Do not expose it to a network.
- Only one window/UI state exists. Concurrent `ui_set_text` calls are serialized
  on the GUI thread; the last one wins.
- `worker_job` is a synthetic CPU/sleep loop. Real work must keep polling the
  cancellation token to remain responsive to shutdown.
- The inspector command is an additional 2025 fallback check; raw 2026 requests
  are the authoritative validation path.
- Legacy HTTP+SSE (2024-11-05) is not used or demonstrated.

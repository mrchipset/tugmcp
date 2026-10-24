# 澶栭儴 MCP 浜掓搷浣滀笌 conformance 娴嬭瘯璁板綍

> 鏃ユ湡锛?026-10-24
> 浠ｇ爜鍩虹嚎锛歚4c7d150`锛坒eat(examples): add Streamable HTTP mode to echo server锛?> 鐩殑锛氳褰曢娆′娇鐢?*澶栭儴 MCP 瀹㈡埛绔?瀹樻柟 conformance 濂椾欢**鐨勬祴璇曠粨鏋滐紝浣滀负 2026-07-28 鍗忚瀵归綈涓?P4 鐨勮緭鍏ャ€?> 鏈枃鏄祴璇曡瘉鎹紝涓嶆槸璁捐鏉冨▉锛泈ire 鍐崇瓥瑙?`designs/protocol-decisions.md` D18銆?
## 1. 鐜

| 椤?| 鍊?|
|---|---|
| OS | Windows 10.0.26100 x64 |
| 缂栬瘧鍣?| MSVC 14.42锛坄/W4 /permissive- /utf-8`锛?|
| CMake | 4.2.1 |
| Node / npm | v22.13.1 / 11.3.0锛坮egistry: `https://registry.npmmirror.com`锛?|
| MCP Inspector | `@modelcontextprotocol/inspector@2.12.0`锛圕LI 妯″紡锛?|
| MCP Conformance | `@modelcontextprotocol/conformance@0.2.0-alpha.12` |
| 琚祴 server | `build/examples/Debug/tugmcp_echo_server.exe`锛堜粎 `echo` 宸ュ叿锛夛紝stdio 涓?`--http` 涓ょ妯″紡 |
| 鏈簱娴嬭瘯鍩虹嚎 | TLS=OFF 237/237锛汿LS=ON 238/238锛泂chema 191/191 |

Inspector 2.12.0 澹版槑 Node 鈮?2.19锛涙湰鏈?22.13.1 浠?`EBADENGINE` 璀﹀憡锛孋LI 瀹為檯鍙敤銆?
## 2. MCP Inspector锛堝閮ㄥ鎴风锛?
鍏ㄩ儴閫氳繃锛?
| 浼犺緭 | 鎿嶄綔 | 缁撴灉 |
|---|---|---|
| stdio | `initialize` + `tools/list` | 杩斿洖 `echo` 宸ュ叿 |
| stdio | `tools/call`锛坄text=hello`锛?| 杩斿洖 text content + structuredContent |
| HTTP 2025 session | `initialize`锛堟湇鍔＄杩斿洖 `MCP-Session-Id`锛?| 閫氳繃 |
| HTTP 2025 session | `tools/list` | 杩斿洖 `echo` |
| HTTP 2025 session | `tools/call`锛坄text=hello-http`锛?| 杩斿洖 text content + structuredContent |

缁撹锛?025 鐨?stdio 涓?Streamable HTTP 鍩烘湰鎻℃墜銆乻ession銆佸伐鍏疯皟鐢ㄥ彲琚嫭绔嬪鎴风娑堣垂銆?
澶嶇幇锛?
```powershell
$exe = (Resolve-Path 'build\examples\Debug\tugmcp_echo_server.exe').Path
npx -y @modelcontextprotocol/inspector@2.12.0 --cli $exe --method tools/list
npx -y @modelcontextprotocol/inspector@2.12.0 --cli $exe --method tools/call --tool-name echo --tool-arg text=hello

# HTTP锛堝厛鍚姩 --http --port 18081锛?npx -y @modelcontextprotocol/inspector@2.12.0 --cli `
  --server-url http://127.0.0.1:18081/mcp --transport http --method tools/list
```

## 3. 瀹樻柟 conformance锛?026-07-28锛?
鍛戒护锛?
```powershell
$exe = (Resolve-Path 'build\examples\Debug\tugmcp_echo_server.exe').Path
$p = Start-Process -FilePath $exe -ArgumentList '--http','--port','18083' -PassThru `
  -RedirectStandardError "$env:TEMP\tugmcp-conf.err" -RedirectStandardOutput "$env:TEMP\tugmcp-conf.out"
Start-Sleep -Milliseconds 1200
npx -y @modelcontextprotocol/conformance@0.2.0-alpha.12 server `
  --url http://127.0.0.1:18083/mcp --requirements 2026-07-28 --timeout 8000 `
  -o build\conformance-2026-echo
Stop-Process -Id $p.Id -Force
```

缁撴灉锛?*79 passed / 88 failed**锛?0 scenarios锛涘惈 extension/pending 涓嶈鍒嗛」锛夈€傜粨鏋滅洰褰?`build/conformance-2026-echo/`锛堟瀯寤轰骇鐗╋紝涓嶅叆搴擄級銆?
### 3.1 蹇呴』淇殑鐪熷疄鍗忚鍋忓樊

| # | check id | 鏈熸湜 | 瀹為檯 |
|---|---|---|---|
| 1 | `sep-2575-request-meta-invalid-missing-meta` | `-32602` + HTTP 400 | 200锛屾甯哥粨鏋?|
| 2 | `sep-2575-request-meta-invalid-missing-protocol-version` | `-32602` + HTTP 400 | 200锛屾甯哥粨鏋?|
| 3 | `sep-2575-request-meta-invalid-missing-client-capabilities` | `-32602` + HTTP 400 | 200锛屾甯哥粨鏋?|
| 4 | `sep-2575-http-server-header-mismatch-400` | HTTP 400 + `-32020` | 200 |
| 5 | `sep-2575-server-unsupported-version-error` | `-32022`锛宍error.data.supported` 鏁扮粍 + `requested` 鍥炴樉 | 缁撴瀯涓嶅尮閰?|
| 6 | `sep-2575-http-server-error-jsonrpc-id` | 閿欒鍥炴樉璇锋眰 id锛?01锛?| `id:null` |
| 7 | `sep-2575-http-server-method-not-found-404-initialize` | 404 + `-32601` | 200锛屾垚鍔?|
| 8 | `sep-2575-http-server-method-not-found-404-ping` | 404 + `-32601` | 200锛屾垚鍔?|
| 9 | `sep-2575-http-server-method-not-found-404-logging-setlevel` | 404 + `-32601` | 200 + `-32601` |
| 10 | `sep-2575-http-server-method-not-found-404-resources-subscribe` | 404 + `-32601` | 200 + `-32601` |
| 11 | `sep-2575-http-server-method-not-found-404-resources-unsubscribe` | 404 + `-32601` | 200 + `-32601` |
| 12 | `sep-2575-http-server-method-not-found-404`锛堟湭鐭ユ柟娉曪級 | 404 + `-32601` | 200 + `-32601` |
| 13 | `sep-2575-server-identifies-in-result-meta` | result `_meta["io.modelcontextprotocol/serverInfo"]`锛圫HOULD锛?| 浠呴《灞?`serverInfo`锛坵arning锛?|
| 14 | `sep-2575-server-tags-subscription-id` | 閫氱煡 `params._meta["io.modelcontextprotocol/subscriptionId"]` | 椤跺眰 `subscriptionId` |
| 15 | `localhost-host-rebinding-rejected` | 闈炴硶 Host/Origin 鈫?4xx | 200 |
| 16 | `completion-complete` | `{completion:{values:[]}}` | `-32601` |
| 17 | `sep-2575-server-sends-tools-list-changed-on-subscription` | 鍙樻洿鍚?`notifications/tools/list_changed`锛圫HOULD锛?| 鏈彂閫侊紙warning锛?|

鍏抽敭瑙勮寖缁嗚妭锛堟潵鑷?conformance 0.2.0-alpha.12 鐨?schema/scenario锛夛細

- 2026 璇锋眰 `params._meta` **蹇呴』**鍚?`io.modelcontextprotocol/protocolVersion` 涓?`io.modelcontextprotocol/clientCapabilities`锛沗clientInfo` 涓?SHOULD锛沗progressToken` 浠嶆槸鏅€氶敭锛涙棩蹇楃骇鍒敭涓?`io.modelcontextprotocol/logLevel`銆?- 2026 绉婚櫎 `initialize`/`ping`/`logging/setLevel`/`resources/subscribe`/`resources/unsubscribe`锛涙湭鐭ユ柟娉曞悓鏍?404 + `-32601`銆?- 鐗堟湰涓嶆敮鎸侊細`-32022`锛宍data.requested` 鍥炴樉锛宍data.supported` 涓烘敮鎸佺増鏈暟缁勶紱HTTP 400銆?- 璁㈤槄锛氳姹?filter 涓?`params.notifications.{toolsListChanged,promptsListChanged,resourcesListChanged,...}`锛沘ck 棣栧抚锛宍params._meta.subscriptionId` 绛変簬 `subscriptions/listen` 鐨?JSON-RPC id锛涙墍鏈夋祦閫氱煡甯﹀悓涓€ `_meta.subscriptionId`锛涗笉鍐嶄娇鐢ㄩ《灞?`subscriptionId`/`requestId`銆?- `completion/complete` 灞?2026 蹇呴渶鍦烘櫙锛涙渶灏忕┖鏁扮粍鍗冲彲閫氳繃锛屼絾蹇呴』澹版槑 `completions` capability 骞跺彲鍝嶅簲銆?
### 3.2 宸查€氳繃鐨勪俊鍙?
- `tools-list` 3/3锛沗server-sse-multiple-streams` 1/1锛沗sep-2164-resource-not-found` 2/2锛?- `http-header-validation` 14/14锛坧ending 椤癸級锛?- MRTR 缁撴瀯瀛愰泦锛歚missing-input-response`銆乣unsupported-methods`銆乣ignore-extra-params`銆乣validate-input`锛?- `caching` 鐨?`tools/list` hints銆?
### 3.3 Fixture 缂哄彛锛堜笉鏄簱 bug锛?
褰撳墠 echo server 鍙湁 `echo`锛岃€?conformance 鏈熸湜娴嬭瘯璧勪骇锛?
- `test_simple_text`銆乣test_image_content`銆乣test_audio_content`銆乣test_embedded_resource`銆乣test_mixed_content`銆乣test_error`銆乣test_progress`锛?- `test_missing_capability`銆乣test_streaming_elicitation`銆乣test_logging_tool`锛?- `test_trigger_tool_change`銆乣test_trigger_prompt_change`锛?- resources/read锛坱ext/binary锛夈€乺esources/templates銆乸rompts锛坰imple/args/embedded/image锛夈€丮RTR 澶氳疆绛夈€?
鍥犳 `resources-*`銆乣prompts-*`銆乣tools-call-*`銆乣input-required-*` 鐨勫ぇ閲忓け璐ュ睘 fixture 涓嶈冻锛涘繀椤诲湪娴嬭瘯渚ф彁渚涗笓鐢?conformance fixture server锛屼笉鑳界敤鐢熶骇 example 浠ｆ浛銆?
## 4. 闄愬埗涓庡悗缁?
- 瀹樻柟 conformance 鐨?`server` 瀛愬懡浠ゅ彧鎺ュ彈 `--url`锛圚TTP/SSE锛夛紝**涓嶆敮鎸?stdio command**锛泂tdio 澶栭儴楠岃瘉鐩墠鐢?Inspector銆俙designs/testing.md` 搂8.4 鐨?runner 闇€鎸夋鐜板疄淇銆?- `tasks-*` 涓?extension锛宍json-schema-2020-12`/`http-header-validation` 绛変负 pending/draft锛屾湰娆′笉璁″垎銆?- 澶栭儴濂椾欢缁撴灉鍙啓鍏?`build/`锛屼笉姹℃煋婧愮爜鏍戙€?- 涓嬩竴姝ワ細鍏堟寜 D18 淇湡瀹炲亸宸紝鍐嶅缓 conformance fixture server锛岄噸璺?`--requirements 2026-07-28` 涓?`2025-11-25`锛岀洿鍒板繀闇€鍦烘櫙鐪熷疄澶辫触娓呴浂锛涚劧鍚庤繘鍏?P4銆?
## 5. 最终状态（2026-10-25）

首轮问题已全部关闭或明确归入后续 backlog：

- **2026-07-28**：required 37，seen 50，**121 checks，0 failure / 0 unexpected skip**；包含 namespaced `_meta`、removed-method 404、订阅 `_meta.subscriptionId`、completion、Host/Origin。
- **2025-11-25**：required 31，seen 33，**73 checks，0 failure / 0 unexpected skip**；包含 session lifecycle、logging/setLevel、resources subscribe/unsubscribe、sampling/elicitation。
- `expected_failures.yaml` 为空；没有任何必需检查被屏蔽。
- Runner 强制 required⊆seen、已知未计分 ledger、npx 非零即失败、超时清理 fixture。
- `ctest --test-dir build-conformance -C Debug -R conformance` → 2/2。
- 相关设计：D18（2026 wire）、D19（2025 logging/subscriptions）、D20（security hooks）；fixture server 仅测试资产（`tests/conformance/`），不随生产库发布。
- 复现：
  ```powershell
  cmake -S . -B build-conformance -DTUGMCP_CONFORMANCE=ON
  cmake --build build-conformance --config Debug --parallel
  ctest --test-dir build-conformance -C Debug -R conformance --output-on-failure
  ```

# Header-only 鍙鎬т笌宸ラ噺

> 闂锛歚tugmcp` 鏄惁涔熷彲浠ュ仛鎴?header-only锛熸槸鍚﹁澧炲姞寰堝宸ヤ綔閲忥紵
>
> 缁撹锛?*鍙互**銆傞粯璁ゆ瀯寤轰骇鐗╀负闈欐€佸簱锛屽悓鏃舵敮鎸佷互 header-only 鏂瑰紡娑堣垂锛坄TUGMCP_HEADER_ONLY=ON` 鏄惧紡寮€鍚級锛岃€屼笉鏄竴寮€濮嬪氨鍋氫竴涓法澶х殑鍗曞ご鏂囦欢銆傚伐浣滈噺澧炲姞鏈夐檺锛堣璁￠樁娈电害 +15鈥?5%锛夛紝涓昏鎴愭湰鍦ㄧ紪璇戞椂闂淬€丱DR 绾緥鍜?HTTP 渚濊禆鐨勪紶閫掞紝涓嶅湪鍗忚閫昏緫鏈韩銆?
## 1. 涓轰粈涔堝彲琛?
褰撳墠璁捐鐨勪富瑕佷緷璧栭兘鏀寔 header-only锛?
| 渚濊禆 | 褰㈡€?| 褰卞搷 |
|---|---|---|
| nlohmann/json | 鍗曞ご | 宸插ぉ鐒?header-only |
| cpp-httplib | 鍗曞ご | HTTP/SSE 鍙?header-only锛屼絾浼氫紶閫?ws2_32/crypt32锛圱LS锛?Threads 閾炬帴瑕佹眰 |
| jsoncons | 鍗曞ご锛堝彲閫夛級 | schema 2020-12 鍙?header-only |
| Valijson | 鍗曞ご锛堝彲閫夛級 | draft-07 鍙?header-only |
| 鏍囧噯搴?| 鈥?| thread/mutex/condition_variable 闇€瑕侀摼鎺ュ钩鍙扮嚎绋嬪簱锛圕Make 鐢?`Threads::Threads`锛?|

鍗忚/缂栬В鐮?registry/dispatcher/transport 鎶借薄鏈韩鏄櫘閫?C++锛屾病鏈夊繀椤绘斁鍒?`.cpp` 鐨勫ぉ鐒惰竟鐣屻€?
## 2. 鎺ㄨ崘褰㈡€?
**榛樿鏋勫缓浜х墿涓洪潤鎬佸簱锛涘悓鏃舵敮鎸佷互 header-only 鏂瑰紡娑堣垂锛坄TUGMCP_HEADER_ONLY=ON` 鏄惧紡寮€鍚級銆?*

```text
include/tugmcp/
  core.hpp                 // 绫诲瀷/閿欒/JSON-RPC/涓婁笅鏂囷紙杞伙級
  server.hpp
  client.hpp
  transport/stdio.hpp      // 杞伙紝浠?OS pipe/绾跨▼
  transport/http.hpp       // 浠呭０鏄庯細TlsOptions / StreamableHttp*/宸ュ巶锛涗笉鍚?cpp-httplib
  detail/http_httplib.hpp  // header-only 瀹炵幇锛屽敮涓€ include cpp-httplib 鐨勫疄鐜板ご
  protocol/v2026.hpp
  protocol/v2025.hpp
  schema/jsoncons.hpp      // 鍙€?  schema/valijson.hpp      // 鍙€?  schema/null.hpp          // 娴嬭瘯
  tugmcp.hpp               // 鐢熸垚鐨?amalgamation锛堝彂甯冪敤锛屼笉鎵嬪啓锛?```

- 鍏叡澶村彧鍏佽 include nlohmann/json锛坄Json` 鏆撮湶锛変笌瀵瑰簲鍙€?schema target 鐨勭涓夋柟澶达紱`core.hpp`/`server.hpp`/`client.hpp`/`transport/http.hpp` 姘镐笉 include cpp-httplib/GUI 妗嗘灦澶淬€?- `detail/http_httplib.hpp` 鎵嶅紩鍏?cpp-httplib锛涗笉浣跨敤 HTTP 鐨勬秷璐硅€呬笉浠樿繖浠界紪璇戞垚鏈€?- 鍗曞ご `tugmcp.hpp` 鐢辫剼鏈妸闇€瑕佺殑澶存寜椤哄簭鍚堝苟鐢熸垚锛屽苟鍋氬弻 TU/ODR 娴嬭瘯锛涗笉瑕佹墜宸ョ淮鎶や袱浠藉疄鐜般€?- `TUGMCP_HEADER_ONLY` 鏄?**CMake option**锛歄N 鏃剁浉鍏?target 鍙樹负 INTERFACE锛堜笉缂栬瘧 `src/`锛夈€?- `TUGMCP_IMPLEMENTATION` 鏄?**瀹?*锛歨eader-only / amalgamation 鍦烘櫙鐨勫崟 TU 瀹炵幇瀹忋€?
## 3. 闇€瑕侀澶栧仛鐨勫伐浣?
| 宸ヤ綔 | 璇存槑 | 宸ラ噺 |
|---|---|---|
| ODR/inline 绾緥 | 闈炴ā鏉胯嚜鐢卞嚱鏁?`inline`锛涘父閲?`inline constexpr`锛涚姝㈠ご鏂囦欢閲岀殑闈?inline 鍏ㄥ眬瀵硅薄 | 灏忥紝浣嗗繀椤?CI 妫€鏌?|
| 鍙?TU 娴嬭瘯 | 涓や釜 `.cpp` 鍚屾椂 include `tugmcp.hpp`/`core.hpp`锛岄摼鎺ユ棤閲嶅绗﹀彿 | 灏?|
| 缂栬瘧鏃堕棿 | cpp-httplib + nlohmann + 妯℃澘浼氭斁澶ф瘡涓?TU 鐨勭紪璇戞垚鏈?| 涓紱鐢?feature headers/缂栬瘧妯″紡缂撹В |
| 浜岃繘鍒跺ぇ灏?| inline 浠ｇ爜鍦ㄥ TU 閲嶅锛汱TO/`-ffunction-sections` 鍙紦瑙?| 涓?|
| 閾炬帴瑕佹眰 | Windows `ws2_32`锛圱LS 鏃?`crypt32`锛夛紱绾跨▼鐢?`Threads::Threads`锛汿LS 闇€ OpenSSL 鈮?.0锛汣Make target 蹇呴』鍐欐竻 | 灏?|
| 鍙€変緷璧?瀹?| `TUGMCP_WITH_HTTP`銆乣TUGMCP_WITH_JSONCONS`銆乣TUGMCP_WITH_VALIJSON`銆乣TUGMCP_TLS` 鐨勪竴鑷存€?| 灏忊€撲腑 |
| ABI/瀹炵幇闅愯棌 | header-only 鏆撮湶瀹炵幇锛汥LL/鎻掍欢鍦烘櫙涓嶉€傚悎 | 璁捐鍙栬垗 |
| 娴嬭瘯鐭╅樀 | MSVC/GCC/Clang銆佸崟 TU/澶?TU銆乭eader-only/缂栬瘧妯″紡銆乀LS on/off | 涓?|

header-only 涓?`CPPHTTPLIB_OPENSSL_SUPPORT` 鏃犳硶鍦ㄩ摼鎺ユ湡妫€娴嬭法 TU 涓嶄竴鑷达紝鍥犳鍙敮鎸佺粡 `tugmcp::http` target 娑堣垂锛涚紪璇戞ā寮忎笅璇ュ畯涓?PRIVATE锛岀敱 `detail/http_httplib.hpp` 鐨?`#error` 瀹堝崼鍏滃簳銆?
`tugmcp::tests` 鏄?INTERFACE helper 搴擄紙fake transport / conformance helpers / fixture 澶达紝浼犳挱 `tugmcp_3rd_doctest`锛夛紱娴嬭瘯鍙墽琛屾枃浠讹紙`tugmcp_unit_tests` 绛夛級鏄嫭绔?target锛?*濮嬬粓缂栬瘧**锛沗TUGMCP_HEADER_ONLY` 鍙奖鍝嶅簱 target銆?
## 4. 宸ラ噺璇勪及

- **浠庤璁″紑濮嬪氨鎸?header-only 璁捐**锛氱浉瀵圭紪璇戝簱锛岀害 **+15鈥?5%** 宸ヤ綔閲忋€備富瑕佹槸澶存枃浠舵媶鍒嗐€丱DR/CI銆佺紪璇戞椂闂磋皟浼橈紝涓嶆槸閲嶅啓鍗忚銆?- **宸叉湁缂栬瘧搴撳啀鏀?header-only**锛氱害 **+30鈥?0%**锛屽洜涓鸿鎶婂疄鐜扮Щ鍏ュご銆佸鐞嗛潤鎬佺姸鎬?鍗曚緥銆佽皟鏁翠緷璧栧拰娴嬭瘯锛涜繕鍙兘鍑虹幇缂栬瘧鏃堕棿/浠ｇ爜鑶ㄨ儉闂銆?- **瀹屾暣鍗曞ご鍖栵紙鍖呮嫭 HTTP/SSE锛?*锛氬彲琛岋紝浣嗘秷璐硅€呮瘡娆＄紪璇戦兘浠?cpp-httplib/nlohmann 鐨勬垚鏈紱澶ч」鐩彲鑳介毦浠ユ帴鍙椼€傚缓璁崟澶村彧浣滀负鍙戝竷浜х墿锛屼笉浣滀负寮€鍙戜富褰㈡€併€?- **涓嶄細**鍥犱负 header-only 灏辫嚜鍔ㄨ幏寰椻€滈浂渚濊禆鈥濓細ws2_32/crypt32/Threads/OpenSSL锛堣嫢 TLS锛変粛鐒惰閾炬帴銆?
## 5. 浠€涔堟椂鍊欎笉瑕?header-only

- 闇€瑕佺ǔ瀹?ABI 鐨?DLL/鎻掍欢杈圭晫锛?- 娑堣垂鑰呭緢澶氥€佺紪璇戞椂闂存晱鎰燂紱
- 闇€瑕侀殣钘忓疄鐜?鍑忓皯鍏叡澶存毚闇诧紱
- 闇€瑕佸彲閫変緷璧栭殧绂伙紙HTTP/TLS/schema锛夎€屼笉鎯崇敤瀹忔薄鏌撴秷璐硅€咃紱
- 闇€瑕?pimpl 鎺у埗浜岃繘鍒跺吋瀹广€?
杩欎簺鎯呭喌涓嬬敤闈欐€佸簱/缂栬瘧搴撴洿鍚堥€傦紱header-only 鍙綔涓洪檮鍔犲垎鍙戝舰鎬併€?
## 6. 鎺ㄨ崘鍐崇瓥

1. **core/protocol/stdio**锛歨eader-only 鍙嬪ソ锛屼紭鍏堟敮鎸併€?2. **HTTP/SSE**锛氱嫭绔?`transport/http.hpp`锛堜粎澹版槑锛屼笉鍚?cpp-httplib锛夛紝鍙?header-only锛堝疄鐜颁綅浜?`detail/http_httplib.hpp`锛夛紝涔熷彲缂栬瘧鎴?`tugmcp::http`锛涗笉瑕佽 `core.hpp` 浼犻€?cpp-httplib銆俆LS 鏄?`tugmcp::http` 鐨勬瀯寤哄紑鍏筹紙`TUGMCP_TLS`锛夛紝涓嶆柊澧?target銆傜紪璇戞ā寮忎笅 `CPPHTTPLIB_OPENSSL_SUPPORT`/`TUGMCP_TLS_ENABLED` 涓?PRIVATE锛圤penSSL 浜?PRIVATE锛岄潤鎬佸簱缁?`$<LINK_ONLY:>` 浼犻€掞級锛沨eader-only 妯″紡涓嬭繖浜涘畾涔夋墠璧?INTERFACE锛屼笖娑堣垂鑰呭繀椤诲湪鎵€鏈?TU 浣跨敤鍚屼竴瀹忥紝鍙敮鎸佺粡 `tugmcp::http` target 娑堣垂銆傜紪璇戞ā寮?`src/transport/*.cpp` 鐩存帴 include `httplib.h`锛宍detail/http_httplib.hpp` 涓?header-only 涓撶敤锛涘畧鍗惈 `#ifndef TUGMCP_TLS_ENABLED` 妫€鏌ャ€?3. **schema**锛氱嫭绔嬪彲閫夊ご锛坖soncons/Valijson/null锛夛紝鎸夐渶鍖呭惈銆?4. **鍗曞ご**锛氬彂甯冩椂鐢熸垚 `tugmcp.hpp`锛岄檮甯?SHA256 鍜岀敓鎴愯剼鏈紱寮€鍙戞椂缁存姢 split headers銆?5. **CI**锛氬弻 TU ODR 娴嬭瘯 + MSVC x64 / GCC / Clang + header-only/缂栬瘧妯″紡 + TLS on/off 鍚勪竴閬嶃€?
## 7. 楠屾敹鏍囧噯

瀹屾暣娴嬭瘯鍒嗗眰瑙?[`testing.md`](./testing.md)锛涙湰鑺傚彧鍒?header-only 鐗规湁楠屾敹椤广€?
- [ ] 涓や釜 TU 鍚屾椂 include 鍏叡澶达紝閾炬帴鏃犻噸澶嶇鍙枫€?- [ ] `core.hpp` 涓?include cpp-httplib/GUI 妗嗘灦澶达紱`http.hpp` 涓嶈繘鍏?core 缂栬瘧鍗曞厓銆?- [ ] MSVC x64 涓?GCC/Clang 鍚勬瀯寤轰竴涓?echo server/client锛圱LS on/off 鍚勪竴閬嶏級銆?- [ ] header-only 涓庣紪璇戞ā寮忚涓轰竴鑷达紙鍚屼竴娴嬭瘯闆嗭級銆?- [ ] 璁板綍缂栬瘧鏃堕棿/浜岃繘鍒跺ぇ灏忓熀绾匡紱鍗曞ご鐢熸垚鍙鐜般€?- [ ] Windows 閾炬帴 `ws2_32`/`crypt32` 涓?`TUGMCP_TLS` 寮€鍏崇殑璇存槑鍦?README/CMake target 涓彲瑙併€?- [ ] 鍚屼竴杩涚▼鍐呮墍鏈?TU 鐨?`CPPHTTPLIB_OPENSSL_SUPPORT` 瀹氫箟涓€鑷达紙TLS on/off 涓嶆贩鐢級銆?- [ ] `transport/http.hpp` 涓?include 浠讳綍绗笁鏂瑰ご锛汿LS/鏋勫缓妯″紡鍒囨崲涓嶆敼鍙樺叾澹版槑銆?- [ ] `TUGMCP_HEADER_ONLY=ON` + `TUGMCP_ENABLE_LEGACY_SSE=ON` 蹇呴』 configure 澶辫触锛坄FATAL_ERROR`锛夈€?
## 8. 涓庡涓诲簲鐢ㄧ殑鍏崇郴

- 瀹夸富搴旂敤浠ラ潤鎬佸簱鎴?header-only 宓屽叆閮藉彲浠ワ紱header-only 鍙伩鍏?DLL ABI锛屼絾浼氬鍔犲涓荤紪璇戞椂闂淬€?- 濡傛灉瀹夸富鍙敤涓€涓?TU 鍖呭惈 HTTP 澶达紝header-only 鍙锛涘鏋滃涓?TU 閮藉寘鍚紝寤鸿鐢ㄧ紪璇戠増 `tugmcp::http` 鎴栧彧鍦ㄤ竴涓?TU 瀹氫箟瀹炵幇銆?- 鏃犺鍝褰㈡€侊紝瀹夸富鐨勬墽琛屽櫒/UI 璋冨害/瀹夊叏閫昏緫浠嶇暀鍦ㄥ涓汇€?
## 9. 单头 artifact（发布用，与 `TUGMCP_HEADER_ONLY` 分离）

- **源码权威**：`include/` split headers + `src/` 实现；单头只在发布时由 `tools/amalgamate.py` 生成，不手写、不提交为第二份实现。
- **产物矩阵**：
  - `tugmcp.hpp`：core + stdio（nlohmann/json 作为外部依赖；Threads 链接）。
  - `tugmcp_http.hpp`：上面的 + Streamable HTTP/SSE（cpp-httplib 外部依赖；TLS 变体按宏/单独产物；ws2_32/crypt32/OpenSSL）。
  - schema（jsoncons/Valijson）与 legacy **不进入单头**；`TUGMCP_HEADER_ONLY + LEGACY` 继续 FATAL。
- **消费方式**：恰好一个 TU `#define TUGMCP_IMPLEMENTATION` 后 include；其余 TU 只 include。`TUGMCP_HEADER_ONLY` CMake 模式仍不实现，单头 artifact 不依赖它。
- **生成规则**：按公共头依赖排序；去掉项目内 include/`#pragma once`，保留第三方 include；实现段由 `src/**` 合并到 `#ifdef TUGMCP_IMPLEMENTATION`；处理匿名命名空间重名；输出确定性（无时间戳）。
- **验收**：生成两次 SHA256 一致；双 TU ODR/链接；standalone consumer 行为测试；单头构建的 echo/fixture server 通过 MCP Inspector 与官方 conformance（2026/2025）；TLS 变体编译/链接 smoke。
- **发布**：CI 生成到 `build/amalgamation/` 或 release 附件，附 `SHA256SUMS` 与第三方许可证说明；不进入源码树。

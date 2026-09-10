# Clean-room gluapack functional analysis

This note records the evidence used to design HolyLib's `gmoddatapack` luapack feature. It is a functional interoperability study, not a source or byte-for-byte reconstruction. No DRM, licensing, telemetry, opt-out, or proprietary payload was copied.

## Evidence and limits

The requested stripped `gmsv_gluapack_plugin64.so` and 32-bit plugin were not present during the initial study. The earlier operator report was therefore treated as a hypothesis, not evidence. A later follow-up inspected one archived Linux x86-64 incumbent plugin for its observable pre-Lua opt-out protocol; claims requiring the missing architectures or unobserved native detours remain explicitly open below.

Evidence that was available:

- Target Linux x86-64 `server.so`: SHA-256 `7371bc3cbbfb53796651ef80c775c88e5266c1b0d8c3bbe7537a05ceb382d4be`, GNU build ID `f7c8ebd81215316e3fd15fe93db5337043c5c0fc`.
- Target Linux x86 `server.so`: SHA-256 `562bf00dd2b26befa29df5aac88e2366971fb19e88f586520037572943875f64`, GNU build ID `ff7e4cfaa49a08c6be8c0f79d85c7203eb38e788`.
- HolyLib's complete `source/modules/gmoddatapack.cpp`, symbols, module manager, filesystem, string-table, thread-pool, and build integration.
- A publicly committed recovered incumbent bootstrap at `eoan-ermine/urfim_ww2`, commit `2bb21fc383301c125753ef95640078bdf158f7ef`, path `addons/urf_plib/lua/includes/init.lua`. It is used only as behavioral evidence. HolyLib's bootstrap was written independently.
- `danielga/sourcesdk-minimal` public interfaces for signon states, cvar flags, string tables, and GMod client messages.

Later follow-up also inspected an archived Linux x86-64 incumbent plugin (SHA-256 `7ba78ad490d9d554ac1b19d4e88a825889f1acfc8ef1375a24be213d4cf11395`, 5,829,800 bytes). Its pre-Lua opt-out path asks the engine for the client's `tv_nochat` user setting, compares it with the exact value `no_gluapack`, and stores a per-slot boolean. HolyLib independently implements only that observable protocol through `CBaseClient::GetUserSetting`; no incumbent code or payload is reused.

Offsets below are ELF virtual addresses in the two target `server.so` files, not stable signatures.

## Findings 1–12

### 1. `GModDataPack::SendFileToClient` contract — CONFIRMED for the target engine

The exported symbols are `_ZN12GModDataPack16SendFileToClientEii` on both targets. The x86-64 function is at `0x00c398f0` (831 bytes); x86 is at `0x0093f2f0` (965 bytes). Both take `(this, clientIndex, fileID)` and return `void`. They bounds-check the ID, resolve the client-Lua string table and Lua cache, create the ordinary Lua-file payload when needed, enforce the approximately 64 KiB message limit, write message opcode `4`, a 16-bit file ID, and the compressed bytes, then call `GMOD_SendToClient`.

No server-side per-file counter or end-of-list state is mutated in this function. The barrier advances because the requested file ID receives a syntactically normal `LuaFileDownload`. Suppressing the function without a replacement message can therefore strand the client at Requesting Lua.

Current HolyLib decision: the full init file uses the existing async-compressed real-file path so it can carry the bootstrap. A pinned required connection receives a normally framed, SHA-256-prefixed/LZMA-compressed canonical placeholder for each eligible requested ID without waiting for READY; the bounded request scheduler preserves the engine's request allowance and advances the barrier normally. Init, native deltas, opt-out, recovery, and other native lanes retain the native body path.

### 2. Global suppression versus hidden per-client state — OPEN for the incumbent

The incumbent's relevant native detour remains unavailable, so the suspected global `pack active => suppress` branch cannot be truthfully confirmed from a native xref. The recovered bootstrap contains no client-to-server READY acknowledgement and the four manifest cvars are global, which is consistent with the reported deadlock but is not proof of the native decision.

What would settle it: the stripped plugin's `SendFileToClient` detour or a runtime trace of two simultaneous clients with different pack outcomes.

Current HolyLib decision: required-mode eligibility is fixed per connection and advertised through an exact per-client `SendServerInfo` baseline. Required canonical requests do not wait for READY; READY later proves that the exact immutable object mounted successfully. Requests outside the pinned canonical set remain on the identity-aware native path, while malformed batches and all native lanes retain the engine decoder.

### 3. `AddOrUpdateFile` identity and bytes — CONFIRMED for HolyLib and target engine

The exported target symbol `_ZN12GModDataPack15AddOrUpdateFileEP7LuaFileb` is at x86-64 `0x00c39260` (1677 bytes) and x86 `0x0093eb60` (1933 bytes). It consumes `LuaFile::{name,contents}`, finds or adds the name in `client_lua_files`, hashes `contents + NUL` into string userdata, and optionally creates the per-file LZMA payload.

HolyLib already detours that exact symbol and retains `LuaFile::{name,source,contents}` while its worker processes and compresses the normal per-file cache. There was no need for a second detour or new module.

Implementation decision: capture the original registered virtual path, source path, and bytes in the existing hook, then snapshot that registry into an off-thread whole-pack build.

### 4. Stub and resolver — PARTLY CONFIRMED, PARTLY OPEN

The public bootstrap's resolver is named `gluapack`; it hashes `debug.getinfo(2, "S").source` after stripping `@`, compiles the VFS entry, and returns the function. It replaces `CompileFile`. The exact native replacement payload was not available for confirmation; `return gluapack()()` remains consistent with the resolver but is not claimed as binary-verified.

The prior reconstruction is corrected in one respect: the recovered incumbent bootstrap does **not** override `include` or `RunString`.

Current HolyLib decision: use the generation-independent canonical placeholder `return __holypack()()`. Preserve the engine's native `include` and `CompileFile` registration semantics so caller-relative paths remain authoritative; the placeholder resolver uses the registered logical source identity to enter the already-mounted immutable base. Init, late registrations, and current native deltas are never replaced by this placeholder.

### 5. Pack layout, compression, crypto, and MD5 — CONFIRMED client-side; server producer remains OPEN

The recovered parser skips a one-byte version and repeatedly reads three 16-byte keys, a four-byte big-endian length, and source bytes. It hex-encodes the three binary keys for lookup. It RC4-decrypts using a hex-derived 16-byte key, discards the first 256 decrypted bytes, calls `util.Decompress`, then compares `util.MD5(uncompressedContents)` with `gluapack_md5`.

That confirms the client order `decrypt -> discard RC4 prefix -> decompress -> MD5(uncompressed pack)`. It does not independently prove how the unavailable native producer constructs each key.

Implementation decision: retain the compatible simple layout and MD5 point, use one whole-pack Bootil LZMA stream, and omit encryption entirely. First-party transport security belongs at the operator's HTTP/CDN layer; correctness never depends on a secret.

### 6. Key normalization and salt — CONFIRMED from the bootstrap

The incumbent defines `saltedMD5(value) = util.MD5(gluapack_salt .. value)`. Its two local-path forms apply these anchored removals:

1. `^addons/[^/]+/`, `^gamemodes/[^/]+/entities/`, `^gamemodes/`, `^lua/`
2. `^addons/[^/]+/`, `^gamemodes/`, `^lua/`

Implementation decision: emit the source key plus both normalized virtual-path keys. HolyLib persists a non-secret random salt below the configured data directory.

### 7. Client path, resources, and `sv_downloadurl` — CLIENT HALF CONFIRMED

The recovered bootstrap reads `download/data/gluapack/<gluapack_file>.bsp` through the `GAME` search path. Native `AddResource`/`downloadables` registration and the incumbent's `sv_downloadurl` guard could not be recovered without the plugin.

HolyLib already demonstrates the correct `INetworkStringTableContainer` acquisition in `precachefix`, and the `downloadables` table identity in `stringtable`.

Implementation decision: atomically write `garrysmod/data/<packdir>/<md5>.bsp`, then add `data/<packdir>/<md5>.bsp` to `downloadables`. Publication is refused unless the object exists and registration succeeds. `respect` never changes `sv_downloadurl`; `require` needs a non-empty operator value; `lock` restores the captured operator value while active.

### 8. Replicated manifest — CONFIRMED format weakness, timing OPEN

The recovered bootstrap independently creates `gluapack_file`, `gluapack_md5`, `gluapack_key`, and `gluapack_salt` with the combined replicated/protected/dont-record/unlogged/unregistered client flags. These are separate values and therefore can be observed across different publication instants. Native set timing relative to signon remains open.

Source SDK correction: a **server** cvar carrying a manifest cannot use `FCVAR_PROTECTED`; Source replicates a protected value as a boolean rather than its text. `FCVAR_UNREGISTERED` also conflicts with normal server registration. HolyLib therefore uses one registered `FCVAR_REPLICATED | FCVAR_DONTRECORD | FCVAR_UNLOGGED` server cvar, while the client-created mirror requests the additional local flags.

Current HolyLib decision: publish one compact snapshot for the level's single immutable map base. It carries the base ID, pack directory, and salt; the client derives the object path. One `SetValue` remains the publication barrier.

### 9. Generation and autorefresh — MESSAGE FORMAT CONFIRMED; NATIVE PINNING OPEN

The public bootstrap registers `gmsv_gluapack_autorefresh`, reads one path string, and removes the path's normalized keys from its tables. It does not re-include autorun files in that recovered version. The native `Autorefresh detected! Repacking...` flow and absence of hidden connection pinning cannot be proven without the plugin.

Current HolyLib decision: publish exactly one immutable map base per level lifecycle and pin required connections to it. Later registrations and byte changes are native per-path deltas; they do not rotate the manifest, publish another object, or consume another `downloadables` slot. An exact byte-for-byte restoration returns that path to its canonical base identity.

### 10. Bootstrap injection point — FILE POSITION CONFIRMED; NATIVE DETOURS OPEN

The public artifact places the bootstrap at the top of `lua/includes/init.lua`, before the normal GMod init body. The unavailable plugin's `CLuaInterface::Init` / `CLuaManager::Startup` detours and byte signatures could not be inspected. The target x86-64 `CLuaManager::Startup` was located from `CLuaManager::Startup Lua already exsits?` at approximately `0x00c20120`, but that alone does not reveal the plugin injection instruction.

Current HolyLib decision: compose through the existing `AddOrUpdateFile` ownership by prepending a self-contained bootstrap only to `includes/init.lua`; that file is always excluded from stubbing. This avoids adding unverified Lua-manager detours. The production bootstrap harness and isolated/runtime joins confirmed init executes before canonical placeholders are resolved.

### 11. Failure handling and the `cl_downloadfilter=none` limbo — CONFIRMED

The recovered `failed(message, disconnect, openHelp)` installs a no-op resolver when `disconnect` is truthy. Missing-pack and MD5-mismatch sites pass a disconnect reason. At label `::failed::`, however, `RunConsoleCommand("disconnect")` executes only when `cl_downloadfilter ~= "none"`; the code then requires three base modules and returns. Thus the exact downloads-disabled branch does not disconnect and cannot install the real packed Lua state: it is a genuine limbo path.

Current HolyLib decision: the default fail-open lane retains its compatibility behavior. The separately configured required lane marks placeholders explicitly and reports an unresolvable required stub without scheduling client `retry`. By default, one exact failure with revalidated authenticated SteamID64 ownership queues a server-driven engine reconnect whose next initial baseline is wholly native; disabled, unavailable, or exhausted recovery disconnects with the documented `+tv_nochat no_gluapack` opt-out. The failed join never changes lanes in progress. The server also rejects a required join before sending placeholders when its pinned object is not in the engine download queue or the complete Linux required-delivery hook set is unavailable. Active hot changes use matching native hashes and bodies until exact canonical restoration.

### 12. Sigscan validity — CONFIRMED for HolyLib symbols; incumbent comparison OPEN

HolyLib resolves `AddOrUpdateFile` and `SendFileToClient` by the exported Itanium names above on Linux and its existing Windows symbols. Required delivery additionally resolves its per-client `CBaseClient::SendServerInfo` baseline boundary and the Linux request-batch path used to preserve the engine allowance before bounded canonical delivery. The supported Linux x86-64 target and Windows x64 build checks cover the implemented symbol set; Windows required-mode runtime support remains intentionally unavailable.

The incumbent signatures for those functions, `CLuaInterface::Init`, `CLuaManager::Startup`, and `AddResource` cannot be extracted without its plugin, so cross-plugin signature drift remains open.

What would settle it: provide both stripped plugin architectures plus the exact target `lua_shared.so`; record hook setup xrefs and compare patterns byte-for-byte against these target modules.

### 13. Active string-table userdata length — CONFIRMED for the inspected engines

The September 10 controlled-client follow-up found that the direct hash writer incorrectly assumed fixed-size `client_lua_files` userdata. At `c42f26c`, an update with hash prefix `fb bf a8` left the Windows client's entry reporting 49,147 bytes instead of 32. A diagnostic 16-bit length field was also insufficient. This was a receiver-framing failure, separate from reliable-stream capacity and source-cache coherence.

The actual variable-size `CNetworkStringTable::ParseUpdate` branch consumes **19 bits** for the userdata byte count and checks it against `0x80000`. This is visible in Linux `engine.so` (SHA-256 `333dae74032f7b0b29c6694b3440f1131e6361cd576d9be074744cf47f6596cf`) at virtual addresses `0x175965`–`0x1759c2`, and Windows client `engine.dll` (SHA-256 `c537406de4195fdaf6fa116e0d9535077ffc7011c6fe289584fe0bdde6e17731`) at image-relative addresses `0x20027d`–`0x20039d`. Both readers subtract/shift by `0x13`; the Windows branch reaches the `CNetworkStringTableClient::ParseUpdate: message too large` diagnostic. These addresses describe the inspected builds, not signatures to reuse in production.

HolyLib writes the 19-bit value `32` before the hash and includes it in all payload, envelope, and trailing-message capacity calculations. The regression decodes this field explicitly, fails against the old production header, and checks every insufficient combined capacity. Parser inspection and policy tests establish framing agreement; active request, native response, cache contents, and execution remain distinct runtime observations.

### 14. Ignored active rescan opcode — superseded

At `b6d64b6`, `RequestActiveClientLuaFiles` wrote server-to-client GMod opcode `3`. In Windows `client.dll` SHA-256 `e00f3b7513af81ca2b964b684b2004edda864ad8eeea306d656d978912aa881c`, the actual `IBaseClientDLL::GMOD_ReceiveServerMessage` implementation reads the opcode at RVA `0x204250`. Its opcode-3 branch at `0x204401` jumps directly to the return block at `0x2046d9`. It does not call the separate `GMOD_RequestLuaFiles` implementation at RVA `0x204700`.

With the corrected 19-bit hash framing and the engine watcher blocked only for the canary fixture, the controlled client received the exact new 32-byte hash but did not request the changed ID. Calling the client's actual request function as a separate diagnostic control produced one matching server-side request, but no observed file dispatch or native response. Neither transport staging nor that control request proves refresh completion. The ordinary native H1 -> pending H2 -> H1 restoration did replace the pending map entry and the actual client-visible hash without forced recovery; this is selection/advertisement evidence only.

This concrete opcode defect is addressed by the replacement below. The prior request-control no-dispatch result remains unexplained; it is not evidence against or proof of a filename/body transaction that requires no request. The allocation and canonical-restoration observations below are separate from parked-client lifecycle and current build acceptance.

### 15. Native filename/body refresh contract

The inspected Linux x64 `server.so` SHA-256 `7371bc3cbbfb53796651ef80c775c88e5266c1b0d8c3bbe7537a05ceb382d4be` constructs this payload in `GarrysMod::AutoRefresh::HandleChange_Lua`, beginning at VA `0x10af510`: opcode **1**, the registered filename including its terminating NUL, a little-endian **32-bit body byte count**, then **SHA-256(source + NUL)** followed by Bootil LZMA(source + NUL), compression level 9 and dictionary 65,536. The corresponding Linux x86 sender agrees. The surrounding GMod service message carries the payload's bit count in 20 bits.

In the Windows client binary identified above, the opcode-1 branch reaches RVA `0x574d0`. It reads the filename, body byte count, and body; resolves the existing registration through `0x872a0`; and calls the installation routine at `0x872f0` with the download flag false. That routine installs the complete body in the client string table, invalidates the LuaShared cache under the `!`-prefixed registered name (`0x8756b` path), and returns to the filename reload handler at `0x56ca0`. Thus the native refresh carries cache replacement and engine-controlled reload semantics. It is distinct from opcode-4 file download and opcode-3 request handling.

HolyLib now writes that full transaction after the per-client hash. Both records must fit before any write, including all variable-length fields; the former fixed 34-bit reserve and pending-rescan state are removed. Each recipient is captured by connection ownership at publication. Tests decode the full envelope and body, sweep every insufficient capacity including unaligned starts, reject malformed payloads, and reject replacement owners. Those tests still use a modeled writer, so live execution remains a separate observation.

The native watcher already generates this body and normally broadcasts it itself. Its filter overload directly calls `CBaseServer::BroadcastMessage`; it does not call the individual-client overload. A narrow Linux filter detour recognizes only a validated filename/body message inside the shared watcher's exact registered-file scope, copies the completed body into HolyLib-owned storage, and routes active recipients to the bounded queue. Joining recipients retain the original sender. Required admission now also depends on resolving that filter detour. Linux x64 `engine.so` places it at VA `0xA8790`, CVEngineServer slot 119, immediately before the individual-client sender at `0xA5FC0`. The native callback supplies directory, leaf stem, and extension separately. The ingress now constructs and validates that complete source path before both scope lookup and post-handler disk capture; testing only already-complete paths missed this callback boundary. This lets the filter route prevent duplicate execution while preserving server-side reload.


### 16. Engine-owned compression metadata ABI

The retained-allocation control exposed a Linux x64 mismatch that source inspection of the bundled Buffer alone had missed. The bundled class stores 32-bit capacity/cursor/written fields; this engine's Bootil uses `size_t`. In `lua_shared.so` (SHA-256 `d425eaa603a764b8c1cbb6aae00ddf79777c83fc3b456804fee66418c5849c3a`), `GetSize` at VA `0x46900` reads offset `0x10`, `GetPos` at `0x46910` reads `0x18`, and `GetWritten` at `0x46A20` reads `0x20`, all as 64-bit values. Its setters `SetPos(unsigned long)` at `0x46A00` and `SetWritten(unsigned long)` at `0x46A10` use those engine fields. The native sender at `server.so+0xC398F0` tests that engine `GetWritten`, then rebuilds the hash/LZMA body when it is zero.

Calling the bundled setters on that engine object reset the low cursor field instead of the engine written length. The first canary observer made the same layout assumption, so its reported zero written length did not prove native invalidation; its subsequent native recompression assertion failed. Those results are retained as failed/superseded evidence, not acceptance.

LuaPack now resolves the engine's own buffer accessors from `lua_shared`, using the `unsigned int` symbols on Linux x86 and `unsigned long` symbols on Linux x64. Required support and explicit disk refresh remain unavailable if those accessors cannot be resolved. Source changes call the engine metadata setters without copying, clearing, destroying, or freeing its allocation. The corrected runtime observer likewise uses engine getters for pointer, capacity, cursor, and written length. HolyLib-owned compression buffers continue to use the bundled library.

### 17. Packed CompileString filename identity

The controlled Windows client showed that GMod's `CompileString` adds its own `@` source marker. Passing `@lua/...` therefore produced `@@lua/...`; passing `lua/...` produced `@lua/...`. The old bootstrap supplied the first form, and the old test model silently repaired it. On a required baseline with a natural autorun/include parent, the client installed the native changed body but retained the original execution version.

The bootstrap now supplies the resolved logical filename without an explicit marker. The repository test model follows the observed engine behavior, and a regression checks the resulting exact debug source. Restoring only the old prefix in a separate negative control fails that assertion. This preserves the engine's global `include` and `CompileFile` functions. The model does not establish the engine's reload behavior; the controlled transaction records remain the runtime evidence.

## Design traceability

| HolyLib choice | Evidence |
|---|---|
| Normal compressed stub message, never suppression | Findings 1 and 4 |
| Per-client required baseline, mounted-object READY proof, and native recovery | Findings 1, 2, and 11 |
| Existing module/detours and worker infrastructure | Findings 3 and 12 |
| Three salted path keys, versioned entry stream | Findings 5 and 6 |
| No encryption or secret | Finding 5; clean-room/no-DRM requirement |
| Atomic single-map-base manifest | Findings 8 and 9 |
| Init-file bootstrap excluded from stubbing | Finding 10 |
| Missing/corrupt required pack permits one authenticated native next-connection recovery by default; disabled or exhausted recovery kicks | Finding 11 |
| `tv_nochat=no_gluapack` selects a per-slot native lane before Lua | Follow-up native artifact |
| Only HTTP carries the pack body | Findings 1, 7, and 9 |

## Remaining evidence boundary

Earlier candidates have author-run runtime observations of init ordering, canonical hash/body pairing, required cold and cached joins, native recovery, exact opt-out, hot deltas, canonical restoration, bounded request delivery, and connection-flood behavior. Those observations are scoped to their recorded binaries and procedures. This note does not claim binary equivalence with the incumbent.

The September 10 replacement canary used the production changes plus a separately retained observation/control delta on the empty NBreach Linux x64 server and one owned Windows x64 client. A required baseline executed an included fixture naturally from an autorun root. With only that fixture's ordinary watcher blocked, explicit version 2 capture installed and executed version 2 once; ordinary changed capture restoring version 1 installed the pinned canonical body and executed version 1 once. Enabling the ordinary watcher then produced exactly one execution for version 3 and one for canonical version 1 restoration. The recorded execution sequence is `1, 2, 1, 3, 1`, with matching advertised digest, decoded cached body, and LuaShared source at each step. No file request was required. The earlier manual-include, double-watcher, and double-marker failures remain failed/superseded evidence.

On that final canary, a separate fixture-only control called the actual production native response and original engine sender to prime a 587-byte engine allocation with 180 written bytes. Explicit source capture reset the engine written length and cursor to zero while preserving its pointer and capacity. The original sender then recompressed version 2 to 181 written bytes with the expected digest and decoded source; unchanged recovery and another native send retained the same allocation and valid payload. This verifies the observed retained-allocation path, not request-decoder acceptance. The control bypassed the request decoder and is excluded from production.

These are author-run observations for the identified binaries and fixture, not independent external execution or acceptance of every engine lifecycle. Shared-helper tests cover serialization capacity, owner replacement, and parked collection selection; the complete parked opt-out/authentication/promotion/disconnect lifecycle and current CI matrix remain open. The original server module/settings and owned client configuration were restored after testing. The merge hold remains.

If both stripped incumbent plugin architectures are supplied, findings 2, 3 (incumbent half), 4 (exact stub), 5 (producer), 7 (registration), 8 (set timing), 9 (pinning), 10 (detours), and 12 (signature comparison) can be completed before making any binary-equivalence claim. That comparison is not a HolyLib runtime-readiness gate.

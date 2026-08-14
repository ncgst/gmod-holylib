# Bundled clientside Lua over FastDL

HolyLib's `gmoddatapack` module can bundle registered clientside Lua into one immutable LZMA map-base pack and add that object to Source's `downloadables` table. The engine downloads it during the normal HTTP FastDL resource phase. Pack bodies are never sent through the game netchannel.

The feature is experimental and defaults off. The `gmoddatapack` module itself must also be enabled.

## Map-base and native-delta model

- After the map's registered client Lua reaches the build quiescence window, HolyLib publishes exactly one immutable base object for that level lifecycle.
- That base must be in Source's engine download queue. LuaPack refuses to publish an HTTP-only replacement after the level's `downloadables` budget is exhausted.
- Every base file stores a SHA-256 content identity. Files still identical to the base use the canonical placeholder `return __holypack()()`; files changed since the base and files registered after the base are delivered natively with their current per-client hashes.
- A second or later hotfix changes only the current native delta. It does not create G2/G3 pack objects, rotate the manifest, or consume another `downloadables` entry.
- Restoring a file byte-for-byte to its base identity returns it to the canonical placeholder. This also restores that file's per-client canonical hash after an active-client native delta.
- `includes/init.lua` is always real because it installs the resolver. It is never read from the pack.
- Packed files keep their full logical source path, so their later `include` calls retain the engine's caller-relative behavior. Nested `include` and `CompileFile` calls stay on the engine's current registered path: native deltas remain native, while canonical placeholders re-enter the immutable base.

The full base intentionally contains every captured `client_lua_files` registration except init. While LuaPack is enabled, `gmoddatapack` server-branch/comment stripping and `HolyLib:OnTokenizeContent` rewriting are bypassed so registered hashes, native bodies, and captured pack sources retain one byte identity. A native cold join may request fewer paths, but one request trace is not authoritative proof that the other registered paths can never execute. Realm-looking filenames are hints, not eligibility rules.

## Safety model

- A connecting slot is pinned to the immutable map base.
- The client mounts, parses, and validates the downloaded base before sending `READY(base, md5)`.
- Duplicate exact source keys invalidate a pack during both server build validation and client parsing. Divergent local-alias collisions are marked ambiguous instead of silently selecting the last parsed payload; exact registered paths remain authoritative.
- With `required=1`, Source serializes a mixed per-client baseline: unchanged base files have the canonical placeholder SHA-256, while current deltas and init have native SHA-256 values. Lua bodies then follow the same per-file decision.
- A missing pin, missing server object, non-downloadable base, unavailable per-client baseline hook, or initial READY timeout fails the required path. Pre-baseline faults and timeout disconnect; an exact post-admission client failure may use the separately bounded native-recovery path below. Recovery never changes the failed join's lane in progress.
- A client can explicitly choose native delivery for its whole connection by presenting the exact pre-Lua userinfo value `tv_nochat=no_gluapack`, when `allow_optout=1`.
- Every stub is an ordinary reliable `LuaFileDownload` for the requested file ID, so the Requesting Lua barrier advances normally.
- Canonical stub requests are deduplicated per physical slot and paced every 50 ms with 16 KiB per-client and 128 KiB server-wide staging budgets while retaining 64 KiB of reliable-stream headroom. Only one batch per client is transferred at a time into the netchannel's owned reliable-fragment list, so a later scratch reset cannot discard it and owned backlog remains bounded. While those fragments remain, LuaPack emits at most one packet per client per 50 ms independently of Source's one-packet-per-second PRESPAWN cadence. The pump rotates across slots and caps each 50 ms pass at 32 packets, bounding both per-client delivery and a connection flood's synchronous work. A transient staging rejection defers the same request without consuming it; only an invalid payload or required ordered-hash failure is fatal. PRESPAWN waits for queued and owned stub work. A new `SendServerInfo` baseline, physical disconnect, kill switch, or level/module shutdown clears unsent work; late game-layer callbacks do not.
- Publishing writes the immutable object before atomically replacing the replicated manifest.
- Encryption, DRM, licensing, telemetry, and external defaults do not exist. The only optional outbound request is the operator-configured ingest hook.

## Configuration

| Name | Default | Meaning |
|---|---:|---|
| `holylib_enable_gmoddatapack` | module default off | Enable the existing module and detours. |
| `holylib_gmoddatapack_luapack_enable` | `0` | Master switch. `0` preserves stock HolyLib behavior. |
| `holylib_gmoddatapack_luapack_packdir` | `holylib/luapack` | Directory below `garrysmod/data`; use a stable relative path. |
| `holylib_gmoddatapack_luapack_downloadurl_policy` | `respect` | `respect`, `require`, or `lock` as described below. |
| `holylib_gmoddatapack_luapack_ingest_url` | empty | Optional HTTP endpoint receiving the compressed base object. |
| `holylib_gmoddatapack_luapack_ingest_method` | `PUT` | Method used by the optional ingest request. |
| `holylib_gmoddatapack_luapack_downloadable_limit` | `1` | Maximum LuaPack objects allowed in Source's level-lifetime table. Map-base mode uses one. `0`, a pre-exhausted budget, or registration failure leaves required joins fail-closed until a level lifecycle boundary. |
| `holylib_gmoddatapack_luapack_retention_ttl` | `300` | Compatibility floor for object retention; map-base mode does not rotate superseded generations during a level. |
| `holylib_gmoddatapack_luapack_object_retention_ttl` | `604800` | Minimum age before an unreferenced local object may be removed. `0` disables housekeeping. Active `downloadables` remain protected. |
| `holylib_gmoddatapack_luapack_ready_deadline` | `180` | Seconds a silent spawned required slot may remain unacknowledged. The clock starts at client activation, not connect. |
| `holylib_gmoddatapack_luapack_required` | `0` | Fail-closed map-base admission for non-opt-out clients. |
| `holylib_gmoddatapack_luapack_allow_optout` | `1` | Honor exact `tv_nochat=no_gluapack` as a per-connection native opt-out. |
| `holylib_gmoddatapack_luapack_required_recovery` | `1` | Permit one authenticated server-driven engine reconnect whose next initial Lua baseline is wholly native after an exact required-generation failure. It never switches the failed join in progress. |
| `holylib_gmoddatapack_luapack_required_recovery_ttl` | `120` | Seconds the account-owned one-shot recovery latch remains eligible to be claimed. |
| `holylib_gmoddatapack_luapack_manifest` | empty | Internal atomic replicated base snapshot; do not set manually. |

The legacy optimistic-prefix cvars remain accepted for configuration compatibility. Required map-base delivery does not use speculative whole-join fallback.

The client engine truncates replicated convar values to 255 characters, so the manifest carries only the base id (also its content MD5 and object basename), pack directory, and salt. The client derives `data/<packdir>/<id>.bsp`.

While LuaPack is enabled, `holylib_gmoddatapack_removeserverif` and `holylib_gmoddatapack_removecomments` are ignored with a one-time warning. Pack capture, native delivery, and the string-table identity must refer to one byte stream.

`sv_downloadurl` remains operator-owned:

- `respect` never writes it.
- `require` refuses base publication while it is empty.
- `lock` remembers its value while LuaPack is active and restores accidental changes. It does not invent a URL.

The ingest worker is asynchronous and non-fatal. Requests carry the object path, MD5, and retention TTL. This repository's `cpp-httplib` build is not linked to OpenSSL, so built-in ingestion accepts `http://` only and refuses to downgrade `https://`. Operators needing HTTPS can handle `HolyLib:OnLuaPackBuilt(base, resourcePath, md5, compressedSize)` in an existing trusted uploader. Never place credentials in archived cvars or committed configuration.

## Required mode and exact native opt-out

Before Source serializes `client_lua_files`, a required connection receives canonical hashes only for paths whose current SHA-256 still equals the base. Init, hotfixes, and late registrations receive real source hashes. A native body advances that file to its current per-client hash; a later exact restoration sends the canonical hash and placeholder again.

Canonical registration, the per-client `SendServerInfo` baseline hook, and body-selection hook are currently supported as a required-delivery set only on Linux. If any required hook is disabled, unresolved, or fails to install, `required=1` rejects every connection at `ClientConnect` before a cached native baseline can bypass policy. Restore the hooks or set `required=0`; the exact opt-out exception is deliberately not evaluated while this server capability gate is closed. With the hooks active, exact opt-out and `required=0` remain wholly native. Windows x64 checks prove build compatibility, not required-mode runtime support.

An absent, corrupt, unparsable, MD5-invalid, or incomplete client base reports an exact required-generation failure without scheduling client `retry`. With required recovery enabled, the server revalidates authenticated SteamID64 ownership, arms one account-owned latch, and queues one engine `Reconnect()`. The replacement connection claims that latch before its first `SendServerInfo` and receives a wholly native initial baseline. The failed connection never changes lanes and cannot consume its own latch.

A queued connection may reach its first `SendServerInfo` before Source exposes its SteamID64, or may have connected before the immutable map base finished publishing. The first baseline pins the current base when one is now available and proceeds in the required lane without consuming recovery state. Missing identity never rejects an otherwise usable required base; recovery remains unavailable until authenticated ownership can be established.

The latch expires if it is not claimed within its TTL. Once consumed, its tombstone prevents another automatic recovery until the native connection proves success or the level/module/server lifecycle resets. Physical disconnect and slot reuse clear only per-slot reconnect handoffs; they do not erase the account-owned armed latch or consumed tombstone. Disabled recovery, stale or malformed failure reports, authentication mismatch, unavailable handoff, and exhausted recovery disconnect with the manual opt-out instructions.

A player who needs native Lua must set this Garry's Mod launch option and restart before joining:

```text
+tv_nochat no_gluapack
```

Only the exact value `no_gluapack` opts out, and `holylib_gmoddatapack_luapack_allow_optout 0` disables the exception. The lane is fixed before Requesting Lua and a late READY cannot move an opt-out connection onto stubs.

Source's `downloadables` table is global, not per connection. Therefore an opt-out client may still download the globally registered base object during the resource phase even though its Lua delivery is wholly native. Avoiding that unused HTTP transfer would conflict with keeping the base engine-downloadable for required clients.

## FastDL layout

For a base with MD5 `abc...`, HolyLib writes and registers:

```text
garrysmod/data/<packdir>/abc....bsp
downloadables entry: data/<packdir>/abc....bsp
client cache: download/data/<packdir>/abc....bsp
```

Mirror `garrysmod/data/<packdir>/` into the same relative path below the configured FastDL origin. CDN replication, overseas routing, cache invalidation, and `.bz2` generation are operator responsibilities. Local housekeeping never removes an object that remains registered in the current level's `downloadables` table.

## Rollout and verification

1. Deploy to one staging server with both module and feature flags off. Verify ordinary native joins first.
2. Configure reachable `sv_downloadurl`/mirroring and use `downloadurl_policy=require` during validation.
3. Set `holylib_enable_gmoddatapack 1`, keep LuaPack off, and verify the existing native path. A restart is recommended for staging parity; runtime enable defers its all-registration sweep until the engine datapack is bound. The module cannot be removed during a live level because it owns the registered Lua hash/body hooks.
4. Enable LuaPack off-peak. Confirm one immutable map base, one engine `downloadables` entry, a reachable CDN object, and a non-empty manifest.
5. With `required=0`, record a clean native cold join and reconnect baseline.
6. Enable `required=1`. Purge only the isolated test client's relevant cache, then confirm cold entry, Requesting Lua completion, READY, and reconnect. On a registration-heavy server, include a concurrent cold-join burst and confirm placeholder replies remain paced, no reliable overflow buffer is discarded, and every client eventually leaves Requesting Lua.
7. Apply two successive hotfixes without changing the map. Confirm no new pack object/manifest generation appears, then confirm a new required JIP receives both current files natively and all unchanged base files canonically.
8. For an active required client, change one base path and confirm its native hash/body executes. Restore the exact base bytes and confirm the canonical hash/stub executes base content again.
9. With recovery enabled, block, corrupt, and separately remove only the isolated test client's base after Source admission. Each case must produce exactly one server-driven reconnect and one wholly native next baseline in the same client process, with no client `retry`, mid-join lane switch, second recovery, or cross-slot consumption. Then set recovery to `0` and confirm the same failure kicks fail-closed.
10. Restart the isolated client with `+tv_nochat no_gluapack`. Confirm native Lua for the whole connection, that a forged/late READY cannot change lanes, and that an active hot refresh sends a matching native hash/body rather than the canonical placeholder hash.
11. Exercise the master kill switch. Confirm an immediate request repairs its stale registration before send, the next server frame reprocesses every remaining registered path from canonical to native identity, and subsequent requests use matching ordinary native hashes and bodies.
12. Restore server configuration, binary, Lua files, CDN test object state, and client launch/cache state; verify no test players or test artifacts remain.

## Kill switch

Run this from server console/RCON, or as a superadmin player:

```text
holylib_gmoddatapack_luapack_kill
```

The command sets the master switch to `0`. Stub decisions stop immediately and the next frame clears the manifest. Existing and new requests use normal Lua networking without a restart. Disable the feature with this command; the `gmoddatapack` module itself cannot be removed during a live level because it owns the registered Lua hash/body hooks.

## Risks and non-goals

Clientside Lua is in every player's join path, so this has high blast radius. Engine interfaces, exported names, and init ordering can drift between GMod branches; stage every engine update and retain verified rollback inputs.

This feature does not use `sv_allowdownload`, does not add a netchannel pack-body fallback, and does not automatically make Source's global resource list per-client. READY messages are control-plane only. CDN reachability and origin policy remain operator concerns.

See [the clean-room functional analysis](luapack-gluapack-re.md) for evidence and open runtime questions.

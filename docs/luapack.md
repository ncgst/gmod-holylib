# Bundled clientside Lua over FastDL

HolyLib's `gmoddatapack` module can bundle registered clientside Lua into one immutable LZMA pack and put that object in the Source `downloadables` table. The engine downloads it during the normal HTTP FastDL resource phase. The multi-megabyte body is never sent over the game netchannel.

The feature is experimental and defaults off. The `gmoddatapack` module itself must also be enabled.

## Safety model

- A connecting slot is pinned to the current immutable generation.
- The client mounts and validates downloaded packs before sending `READY(generation, md5)`.
- A delayed READY for a superseded pin is accepted, then the active client is immediately handed the latest generation. The handoff retries until acknowledged, so back-to-back hotfixes converge instead of leaving a client ready on an intermediate pack.
- With `required=0`, only a ready slot receives tiny per-file stubs. The full init file first follows HolyLib's existing path so it can carry the bootstrap; after that, an uncertain slot is handed directly to native `GModDataPack::SendFileToClient` unless the separate optimistic mode is enabled.
- With `required=1`, the init file is still real, but every other connecting request is answered immediately with a required-generation stub without waiting for READY. A missing generation, an object unavailable during the engine download phase, a requested path absent from the generation, a client mount/validation failure, or an expired initial READY deadline disconnects that client. Required joins never silently rotate to native delivery.
- A client can explicitly choose native delivery for its whole connection by presenting the exact userinfo value `tv_nochat=no_gluapack`, when `allow_optout=1`. The lane is read and latched on the first Lua request, before client Lua exists.
- Every stub is an ordinary reliable `LuaFileDownload` for the requested file ID, so the Requesting Lua barrier advances.
- Publishing creates the immutable object before replacing the one replicated manifest snapshot. At most `downloadable_limit` LuaPack objects are appended to Source's non-removable, level-lifetime `downloadables` table; later hotfix generations use the same validated HTTP handoff as connected-client autorefresh once a new join has spawned.
- Old generations remain addressable until pins drain and their manifest-retention TTL passes. A separate object TTL prunes only files that are no longer retained **and** no longer present in the active `downloadables` table, so hotfix rebuilds cannot strand current-session joins. New joins are never offered an unbounded history of hotfix packs.
- Encryption, DRM, licensing, telemetry, and external defaults do not exist. The only optional outbound request is the operator-configured ingest hook.

## Configuration

| Name | Default | Meaning |
|---|---:|---|
| `holylib_enable_gmoddatapack` | module default off | Enable the existing module and detours. |
| `holylib_gmoddatapack_luapack_enable` | `0` | Master switch. `0` preserves stock HolyLib behavior. |
| `holylib_gmoddatapack_luapack_packdir` | `holylib/luapack` | Directory below `garrysmod/data`; use a stable relative path. |
| `holylib_gmoddatapack_luapack_downloadurl_policy` | `respect` | `respect`, `require`, or `lock` as described below. |
| `holylib_gmoddatapack_luapack_ingest_url` | empty | Optional HTTP endpoint receiving the compressed object body. |
| `holylib_gmoddatapack_luapack_ingest_method` | `PUT` | Method used by the optional ingest request. |
| `holylib_gmoddatapack_luapack_downloadable_limit` | `1` | Maximum LuaPack objects appended to Source's level-lifetime `downloadables` table. `0` makes every generation post-spawn HTTP-only. Once exhausted, publication and autorefresh continue. Fail-open joins use native Lua during signon and fetch after activation; required joins are rejected until the current generation is engine-downloadable (normally after a level lifecycle boundary, or when the budget is sized to include it). |
| `holylib_gmoddatapack_luapack_retention_ttl` | `300` | Seconds an unpinned superseded manifest entry remains retained. |
| `holylib_gmoddatapack_luapack_object_retention_ttl` | `604800` | Minimum age in seconds before an unreferenced local object may be removed. `0` disables housekeeping. The effective value is never lower than `retention_ttl`; active `downloadables` and retained generations are always protected. Compatible ingest endpoints receive the same value in `X-HolyLib-LuaPack-Retention-Seconds`. |
| `holylib_gmoddatapack_luapack_ready_deadline` | `180` | Seconds a silent **spawned** slot may remain unacknowledged. The clock starts at client activation, not at connect. Fail-open clients release the pin and use native delivery after expiry; required clients are kicked until their first successful READY. |
| `holylib_gmoddatapack_luapack_required` | `0` | GluaPack-style fail-closed join mode. Non-opt-out clients receive required stubs during Requesting Lua without waiting for READY; pack or publication failures kick instead of using native Lua. |
| `holylib_gmoddatapack_luapack_allow_optout` | `1` | Honor the exact pre-Lua userinfo value `tv_nochat=no_gluapack` as a per-connection native-delivery opt-out. |
| `holylib_gmoddatapack_luapack_optimistic` | `0` | Speculatively stub large joins before the READY acknowledgement. See below. |
| `holylib_gmoddatapack_luapack_optimistic_prefix_files` | `256` | Files delivered natively at the start of a join before speculation may begin. |
| `holylib_gmoddatapack_luapack_optimistic_prefix_bytes` | `262144` | Native Lua source bytes delivered at the start of a join before speculation may begin. |
| `holylib_gmoddatapack_luapack_unready_ttl` | `900` | Seconds an account that failed a speculative join keeps receiving native files on new connections. |
| `holylib_gmoddatapack_luapack_manifest` | empty | Internal atomic replicated snapshot; do not set manually. |

The client engine truncates replicated convar values to 255 characters, so the snapshot carries only generation ids (the id doubles as the content MD5 and the FastDL object basename), the pack directory, and the shared per-lifecycle salt; the client derives `data/<packdir>/<id>.bsp` itself. Retained generations that no longer fit are dropped from the snapshot but stay valid server-side for late acknowledgements.

While luapack is enabled, `holylib_gmoddatapack_removeserverif` and `holylib_gmoddatapack_removecomments` are ignored (a one-time warning is logged). Luapack requires a single canonical byte stream per file: the stringtable hash is computed from the processed content, while pack capture and the engine-native fallback path carry the raw file bytes — stripping would make those disagree.

`sv_downloadurl` remains operator-owned:

- `respect` never writes it.
- `require` refuses optimized publication while it is empty.
- `lock` remembers its value while luapack is active and restores accidental changes. It does not invent a URL.

The ingest worker is asynchronous and non-fatal. Requests carry the object path, MD5, and the configured object-retention TTL; a compatible endpoint may use that TTL to prune its own immutable-object store after preserving the newly uploaded object. This repository's `cpp-httplib` build is not linked to OpenSSL, so built-in ingestion accepts `http://` only and refuses to downgrade `https://`. Operators needing HTTPS can handle the pluggable server hook `HolyLib:OnLuaPackBuilt(generation, resourcePath, md5, compressedSize)` in their existing trusted uploader. Do not place credentials in archived cvars or commit them to configuration.

## Required join mode and native opt-out

`holylib_gmoddatapack_luapack_required 1` makes LuaPack canonical for new connections. The server still sends the real `includes/init.lua` so the resolver exists, while every remaining file uses the generation-independent placeholder `return __holypack()()`. Before Source serializes `client_lua_files` for one connection, required clients temporarily receive that placeholder's SHA256 baseline and native opt-out clients receive the real source hashes. Each file response repeats its exact hash as a targeted reliable string-table update before the body. The replicated manifest selects the pinned generation, and the replicated required policy makes an absent, corrupt, unparsable, MD5-invalid, or incomplete pack report failure without scheduling `retry`; the server then kicks with opt-out instructions.

The lane is fixed while the per-client server-info baseline is serialized, before Requesting Lua begins. A player who needs native Lua must set this Garry's Mod launch option and restart before joining:

```text
+tv_nochat no_gluapack
```

Source exposes `tv_nochat` as userinfo, so the server can read it before client Lua starts. Only the exact value `no_gluapack` opts out, and `holylib_gmoddatapack_luapack_allow_optout 0` disables the exception. Opt-out clients use native Lua for their entire connection and cannot switch back by sending a late READY.

Required mode takes precedence over optimistic mode. It also requires the pinned generation to be present in Source's engine download queue during an initial join; the post-spawn HTTP handoff is too late for Requesting Lua. Because `downloadables` entries cannot safely be removed during a level, exhausting `downloadable_limit` or publishing a newer HTTP-only generation deliberately causes required new joins to be kicked instead of falling back. Plan hotfixes around a level change or size the per-level budget for the expected number of generations.

Required mode is deliberately an admission policy, not a reason to kick already-playing users during every live Lua edit. After a connection has successfully acknowledged one generation, the existing autorefresh bridge remains fail-open while the replacement pack is built and fetched: changed paths are delivered natively during that handoff, then required-marked stubs resume after the new READY. An actual required stub that later becomes unresolvable still reports failure and is kicked.

## Optimistic join stubbing

The READY acknowledgement is sent from the client bootstrap, but the engine only flushes
queued client commands after signon completes — so on a real join it arrives after the
Requesting Lua phase already delivered everything natively. The base feature therefore saves
almost nothing on first joins. `holylib_gmoddatapack_luapack_optimistic` closes that gap by
speculating per connection instead of waiting for proof:

- The first `optimistic_prefix_files` requested files (and at least `optimistic_prefix_bytes`
  of Lua source) are always delivered natively. Warm reconnects request few files, never
  cross the prefix, and never speculate; the prefix also progressively re-natives files for
  clients that cached stubs from an earlier session.
- Once a connection crosses both thresholds without having acknowledged, the remaining
  requested files that exist in its pinned generation are answered with generation stubs.
  Files missing from the generation (changed since publication), the init file, fallback
  slots, and every existing fail-open path stay native.
- A generation omitted from `downloadables` because the per-level budget is exhausted is
  never speculatively stubbed. That join stays native until its post-spawn HTTP handoff is
  validated and acknowledged; normal ready-client stubbing and later autorefresh then resume.

Recovery is what makes speculation safe. If a stub executes client-side and no pack can
serve it, the bootstrap first attempts one synchronous mount of the already-downloaded
object (the FastDL fetch may have finished mid-join). If that fails, the session cannot be
repaired in place: the client sends `holylib_luapack_unready`, stops acknowledging, and
issues one `retry`. The server latches that account (by engine network ID) to native
delivery for `unready_ttl` seconds — and it also sets the same latch on its own whenever a
connection that received speculative stubs disconnects without acknowledging, so a recovery
command lost in the reconnect teardown still converges. A matching READY clears the latch.
The worst case per affected account is one wasted stub join plus one full native join;
a server restart between the failure and the retry forgets the latch and costs one more such
cycle. Latches are held in memory only and reset with the level.

Every join logs one summary line at activation (native files/bytes, speculative stubs,
acknowledged stubs, latch and fallback state). Run with speculation off first and use these
lines to validate prefix sizing against real traffic before enabling the flag.

## Kill switch

Run this from the server console/RCON, or as a superadmin player:

```text
holylib_gmoddatapack_luapack_kill
```

The command sets the master switch to `0`. Stub decisions stop immediately; the next frame clears the replicated manifest. Existing and new file requests use normal Lua networking without a restart.

Setting `holylib_gmoddatapack_luapack_required 0` changes the lane selected by future connections; a connection that already selected required or opt-out delivery remains on that lane. Use the master kill switch for an immediate all-client emergency return to native delivery.

Optimistic join stubbing has its own independent switch: setting `holylib_gmoddatapack_luapack_optimistic 0` stops speculation on the next file request while leaving acknowledged-stub delivery and the rest of the feature untouched.

## FastDL layout

For a generation with MD5 `abc...`, HolyLib always writes the immutable object and, while
the per-level budget remains, registers it:

```text
garrysmod/data/<packdir>/abc....bsp
downloadables entry: data/<packdir>/abc....bsp
client cache: download/data/<packdir>/abc....bsp
```

Source string-table entries cannot be removed safely during a level, which is why `downloadable_limit` exists. Once the budget is exhausted, additional generations are not appended: fail-open JIP clients receive the current object through a retried post-spawn HTTP handoff, while required JIP clients are rejected because that handoff occurs after Requesting Lua. Mirror `garrysmod/data/<packdir>/` into the same relative `data/<packdir>/` path below the configured FastDL origin. CDN replication, overseas routing, cache invalidation, and `.bz2` generation are operator responsibilities. Do not configure a pipeline that removes a generation while it can still appear in the retained manifest. Local housekeeping runs after a successful publish and also preserves every object still registered in the current level's `downloadables` table; superseded hotfix objects therefore become removable only after both the TTL and a level lifecycle boundary make them safe.

## Rollout and verification

1. Deploy to one staging server with both module and feature flags off. Verify ordinary joins first.
2. Configure a reachable `sv_downloadurl` and mirror pipeline. Keep `downloadurl_policy=require` during validation.
3. Enable `holylib_enable_gmoddatapack 1`, leave luapack off, and verify the existing async-compression path.
4. Off-peak, set `holylib_gmoddatapack_luapack_enable 1`. Confirm the log reports one immutable generation, the object exists on FastDL, and the manifest is non-empty.
5. Join with downloads enabled. Confirm one `.bsp` HTTP object, a READY log for the pinned generation, and successful completion of Requesting Lua.
6. Start a throttled join on generation G1, modify a registered Lua file to publish G2, and confirm that join still acknowledges/uses G1 while new joins use G2.
7. While `required=0`, join with `cl_downloadfilter none` and corrupt/remove the FastDL object. Confirm both cases still take the legacy native recovery path.
8. Confirm the current generation is marked engine-downloadable, then set `holylib_gmoddatapack_luapack_required 1` with `allow_optout 1`. On a normal clean-cache join, confirm the summary reports the `required` lane and required stubs during Requesting Lua, followed by READY.
9. In required mode, repeat with downloads disabled, a missing object, a corrupt object, and an MD5 mismatch. Each case must kick with the `+tv_nochat no_gluapack` instruction and must not schedule an automatic retry or rotate to native Lua.
10. Restart a test client with launch option `+tv_nochat no_gluapack`. Confirm the summary reports `optout-native`, the join completes using native per-file Lua, and a forged/late READY cannot move that connection onto stubs.
11. Exhaust `downloadable_limit` with a test hotfix and confirm a new required join is rejected rather than native-fallback. Perform the planned level lifecycle boundary and confirm the current generation becomes joinable again.
12. Exercise the master kill switch during a join and confirm subsequent requests are real files, not stubs.
13. Watch netchannel and HTTP byte counts at production population before expanding rollout.

## Risks and non-goals

Clientside Lua is in every player's join path, so this has the highest practical blast radius in the module. Engine interfaces, exported names, and init ordering can drift between GMod branches; stage every engine update and retain the kill switch.

This feature does not change the pack body delivery channel, does not use `sv_allowdownload`, and does not add a netchannel body fallback. Tiny READY and autorefresh metadata messages are control-plane only. CDN reachability, overseas edge behavior, and origin policy remain operator concerns; the ingest TTL header is advisory and only endpoints that explicitly implement it perform remote housekeeping.

See [the clean-room functional analysis](luapack-gluapack-re.md) for evidence and open runtime questions.

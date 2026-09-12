# September 2026 Garry's Mod update readiness

Tracking issue: https://github.com/ncgst/gmod-holylib/issues/20

## Scope and baseline

The official announcement currently targets September 16, 2026 at 15:00 UTC
(23:00 Asia/Shanghai) and says updated clients can still join older servers.
Recheck the announcement and preview on release day:
- https://steamcommunity.com/app/4000/announcements/
- https://wiki.facepunch.com/gmod/Update_Preview_Changelog

The initial baseline was fork main at
`586915c32ea901721f610f77cda79786e858230a`, with 36 enabled runtime jobs passing
in https://github.com/ncgst/gmod-holylib/actions/runs/34586144963.
That was source/CI evidence, not acceptance of production's loaded binaries.

## Automated preparation

The build workflow enables the eight broader `gmod_tests` LuaJIT replacement
combinations: Linux32 public/dev/prerelease and Linux64 x86-64, each with and
without GhostInj. Each has a ten-minute runner limit, independent matrix
results, and retained sanitized console logs. The upstream test suite is pinned
to `f71a7b07406f5655b2b78b106e36646147b4e56f` for comparable results.

The HolyLib test suite additionally checks native and FFI Vector/Angle type
contracts, primitive TypeID results on hot JIT paths, GC, and mixed
`net.WriteTable` serialization. Its existing harness repeats the suite after
a map change. These checks do not send test messages to clients.

Preparation branches build and test without triggering the three testing-area
deployment jobs. Those deployment jobs require `refs/heads/main`.

A failed broader test is a compatibility finding to classify. Do not skip it,
replace its expected result, or turn off a production feature just to make CI
green. Compare with the corresponding stock-runtime job and inspect the exact
engine and module identity.

## Physics coverage: embedded IVP is not deployed Jolt

The old `-holylib_replaceivp` jobs select HolyLib's embedded IVP implementation.
They do not install Avrena/VPhysics-Jolt. `IncludeIVP()` is commented out in
`premake5.lua`, and the replacement flag handler is compiled only with
`CUSTOM_VPHYSICS_BUILD`. Merely enabling those jobs would not establish
replacement-physics coverage.

Those jobs remain disabled with this concrete reason. Jolt acceptance requires
an explicit staging installation of the actual candidate Jolt loader and backend
binaries, with recorded SHA-256 hashes and runtime identity. Select the artifact
from the intended Avrena/VPhysics-Jolt revision; never substitute a latest
upstream or generic Jolt binary. A green HolyLib workflow does not clear this gate.

## Candidate evidence

Capture before and after the update, using the same configuration:

- Steam app/build/depot identity and exact branch; retain `garrysmod.ver` and the
  server's `version` output.
- Engine, server, lua_shared, filesystem/dedicated, HolyLib, GhostInj and Jolt
  loader/backend hashes. Include every loaded native addon.
- HolyLib commit and enabled modules; custom LuaJIT/FFI switches and diagnostics.
- Boot log, hook-resolution/unknown-layout warnings, and proof that required
  modules are active. A successful startup with disabled hooks is insufficient.
- Effective tick interval, map, addon versions and test outcome.
- Real client version, operating system and architecture for client acceptance.

Use `tools/capture-gmod-update-evidence.sh ROOT OUTPUT` on the Linux staging
host to collect a read-only file-identity manifest. It does not prove which
binaries the running process loaded; obtain that independently from the server
process and runtime diagnostics. It intentionally does not copy configuration,
tokens, player identifiers or arbitrary logs.

## Staging sequence

1. Identify the actual production instance and loaded native versions read-only.
   Make an isolated staging copy with its own ports, data, credentials and
   publication settings. Keep the candidate out of public rotation.
2. Preserve a restorable copy of the engine, all native modules and configuration
   as one versioned set. Verify the restore paths and available storage.
3. Install the candidate engine in staging. On Linux64 select the x86-64 candidate;
   Linux32 uses prerelease. Record the actual build identity, since branch labels
   can move. Also test an updated client against the retained old server.
4. Boot with the intended production configuration. Check filesystem search-path
   handling, addon/Workshop mounting, Lua delivery and required hook activation.
5. Run native and FFI type checks, timers, delayed callbacks, join/reconnect,
   Steam/Workshop initialization and at least two map changes.
6. Compare stock physics with the intended Jolt build on the same candidate
   engine. Test physics object destruction, constraints, cleanup and shutdown.
7. Retain one real BRDM through occupied driving, collisions, exit and re-entry.
   Test paired normal/relative teleport triggers with ragdolls separately.
   Keep teleportguard enabled until comparison evidence supports changing it.
8. Test real clients: SCP-939 hearing and voice around join/disconnect, terminal
   fades and RNDX/CEF documents with Escape open, spectator/death/respawn/job
   changes, weapon prediction, and custom model bones/materials.

Bots and CI fixtures cannot replace steps 7 and 8. Record failures with the exact
entity identity and state transition; do not replace the BRDM midway through the
test or attribute every physics failure to the same cause.

## Promotion and rollback

Promote only after the candidate identity, required hooks, enabled production
features, transition checks and client/player gates are recorded as passing.
The issue remains open while any required acceptance gate is pending.

For a native update, stop the target process, back up, replace the complete
verified set, restart, and verify loaded identities and runtime status. Restore
the matching previous engine/native/configuration set if boot, hook activation,
join, transition or gameplay gates fail. Do not overwrite a loaded native binary
or use Steam validation as the rollback plan.

Production mutation requires a freshly verified target and artifact set.
This preparation does not assert that a server has been upgraded.


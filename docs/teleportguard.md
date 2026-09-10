# Teleport recursion guard

The `teleportguard` module prevents a `trigger_teleport` or
`trigger_teleport_relative` from recursively teleporting an entity while that
entity's previous trigger touch is still executing. This can happen when both
ends of a teleporter pair are enabled and a destination overlaps the return
trigger. It can exhaust the native stack without returning to Lua.

The module wraps the native `Touch` functions. An allocation-free thread-local
scope records the entity handle, including its serial number, until the original
touch returns. Recursive touches of that entity are skipped. Other entities and
later touches still use the original engine implementation, including spawnflags,
named filters, landmarks, angles, velocity and player-specific behavior. There is
no timer or persistent cooldown. Nested chains involving different entities have
a separate 64-frame safety limit.

This is stack-recursion protection, not a redesign of map transport logic. A map
that continuously leaves reciprocal endpoints enabled can still move an entity
back on a later physics update. Map authors should use sensible endpoint timing.

## Availability and Lua integration

The module currently supports Linux64 only. It is enabled by default on supported
builds and can be disabled at startup with `-holylib_enable_teleportguard 0`.
Runtime enable/disable is refused, because scripts may already have lifted their
fallback restrictions. No existing `HolyLib` table functions are replaced.

- `teleportguard.IsActive()` returns true only when **both** native detours are
  enabled. If either installation fails, the module removes both detours and
  returns false.
- `teleportguard.GetBlockedCount()` returns the cumulative number of recursive
  or excessive-depth touches suppressed by this process. It does not reset on a
  map change. The counter is diagnostic, not a count of successful teleports.

A server that previously narrowed teleporter spawnflags to players only should
skip that narrowing only when `teleportguard` exists and `IsActive()` returns
true. Keep the fallback on older, disabled or unsupported builds. Deploy the
module and fallback adjustment together while stopped; a fresh map restores its
authored flags. Merely deleting a Lua hook during hot reload neither removes the
registered hook nor restores already-modified entities.

## Function recovery

The Linux64 signatures were verified against a GMod `server.so` with SHA-256
`7371bc3cbbfb53796651ef80c775c88e5266c1b0d8c3bbe7537a05ceb382d4be`.
Both signatures have one match in its executable code:

| Function | RVA | RTTI vtable | Touch slot |
|---|---:|---:|---:|
| `CTriggerTeleport::Touch` | `0xb508d0` | `0x1845610` | 102 |
| `CTriggerTeleportRelative::Touch` | `0xb51370` | `0x1845e70` | 102 |

The vtables were recovered from the corresponding Itanium RTTI names. The
functions call `PassesTriggerFilters` through slot 255, then the entity's
`Teleport` through slot 111. Runtime resolution uses function-entry signatures,
not these RVAs or vtable indices. Revalidate after an engine update.

## Validation

`tests/teleport_guard_tests.cpp` exercises 100,000 recursive pairs, nested
independent entities, serial-number reuse, separate threads, scope/exception
unwinding, and the distinct-entity depth limit. The CI job runs it with ASan and
UBSan. Compile locally with:

```sh
c++ -std=c++17 -Wall -Wextra -Werror -pedantic -pthread \
  -fsanitize=address,undefined -Isource tests/teleport_guard_tests.cpp \
  -o /tmp/teleport_guard_tests
/tmp/teleport_guard_tests
```

Runtime acceptance should include real brush triggers and physics objects,
ragdolls, player transport, existing filters, both endpoints enabled, map cleanup,
and the fallback on a build where the module is unavailable. Run deliberate
overflow negative controls only in disposable servers.

The 2026-09-11 isolated Linux64 engine check used the target server's engine,
physics backend and map with gameplay addons excluded. It passed 59 checks:
props, ragdolls, concurrent entities, a player bot, relative teleport offsets,
named and spawnflag filters, 20 paired-endpoint iterations, refusal to remove an
active guard, and preservation of all 26 authored flags after map cleanup. The
paired sequence suppressed 50 recursive touches. Disabling the module at startup
made the same paired sequence terminate with SIGSEGV (exit 139) after its first
iteration. This synthetic player test is separate from real-client acceptance.

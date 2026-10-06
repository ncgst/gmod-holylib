# Stabilizing networking fastpath for 120 players

`holylib_networking_fastpath` remains experimental and defaults to `0`. This
revision replaces cross-player transmit-list copying with a bounded cache of
`CheckHeadnodeVisible` results. It is a correctness-first starting point for
profiling a 120-player workload, not a demonstrated 120-player capacity increase.

## Why the old reuse boundary was unsafe

The previous path matched a recipient to another player's area or cluster and
copied that player's final entity bitset. Area/cluster identity does not identify
the actual PVS: cameras, spectators and extra visibility origins can differ. The
cache also treated zero-initialized entries as valid matches for area/cluster
zero. A matching recipient returned before the normal entity loop and full-update
handling.

Final transmit lists include recipient-specific `ShouldTransmit`, `SetTransmit`,
prevent-transmit rules, range limits and ownership-dependent attachments. Removing
the source player's own bits does not make the rest of that result reusable. An
entity hidden from the source cannot be recovered by subtracting the next
recipient's prevent-transmit mask. Conversely, an entity accepted by a custom
source-only rule can leak into another recipient's list.

## Reuse only a pure visibility query

All recipients now traverse the normal transmit implementation. The cache sits
inside the headnode branch of `CCServerNetworkProperty::IsInPVS`; it does not
store entities, client slots or final transmit bits.

- Match the complete PVS byte sequence and its length. Hashes locate candidates;
  an exact comparison is required even when hashes match.
- Cache both positive and negative results using the current BSP headnode as the
  key. A moving entity supplies its current headnode after the existing PVS
  recomputation. Slot reuse and entity deletion cannot revive entity results.
- Evaluate area connectivity on every call, outside the cache. A portal changing
  during the tick does not reuse an earlier area-connectivity answer. Skybox,
  distance, parent and force-transmit rules also remain outside the cache.
- Keep ordinary cluster bit tests direct. Resolve a recipient's cache context
  lazily, only when a headnode query is reached. Maps without these queries incur
  no PVS hashing or copying.
- Bound storage to `MAX_PLAYERS` PVS contexts and 256 direct-mapped headnode
  entries per context. Node collisions replace results; context exhaustion and
  unsupported PVS sizes fall back to the original query. No per-query heap
  allocation is needed. There is no scan over player slots.
- Invalidate contexts on every tick change, including rewinds, and reset on
  module initialization, map activation and shutdown. Contexts do not survive
  into a different BSP. Counters accumulate until initialization/map activation.

With 8 KiB PVS buffers and 128 context slots, storage is approximately 1.3 MiB.
The code assumes the existing server-thread transmit execution and a PVS built
before `CheckTransmit` that stays fixed during that invocation. It does not make
the rest of the networking module thread-safe or support native extensions
rewriting that PVS buffer from inside an entity transmit callback.

HLTV/replay retain their separate path. The existing full-update player additions
and the final `g_bWasSeenByPlayer` union still run. This does not establish that
every other networking optimization is equivalent to the stock engine.

In-eye observer handling is shared by both cache settings. The common recipient
path explicitly transmits a valid observed player and its viewmodels and hands,
even if the target was already marked for transmission. Attachment binding
otherwise excludes these entities from ordinary checks, and the character hook
only adds them for their owner. This retains the observer additions previously
performed by the old fastpath cache-hit helper and also repairs their omission
with `fastpath 0`. The real recipient stays unchanged, so an observer does not
qualify for owner-only inventory transmission. A prevented target is not forced;
individual additions respect prevent-transmit bits, and a final mask removes
prevented entities added recursively before the packing union is updated.

`holylib_networking_fastpath_usecluster` remains registered for configuration
compatibility but no longer changes matching. Both values require exact PVS
identity. No engine structure layout or network protocol changes are introduced.

## Measurement and 120-player acceptance

Use `holylib_networking_fastpath_stats` to read context hits, misses and bypasses,
plus headnode hits and misses. Take counter differences over a measurement window.
A context hit means identical PVS data was found; it is not a skipped client
transmit pass. Headnode hit rate is `hits / (hits + misses)` when that denominator
is nonzero. It is not a prediction of total tick-time improvement.

Compare `fastpath 0` and `fastpath 1` on the same build and configuration, leaving
the other networking switches fixed. Separate this comparison from a stock-engine
baseline. Test 30, 60, 90 and 120 real connected clients, with both concentrated
and dispersed visibility. Bots can help exercise entities but do not replace
real client acknowledgments, bandwidth or join/full-update behavior.

For each population and visibility pattern, warm up and alternate several equal
measurement windows. Record median, p95 and p99 tick time and `CheckTransmit`
time, cache hit/miss deltas, bytes per client, choke/loss, snapshot sizes and
full-update frequency. Keep map, tickrate, entity population and client activity
comparable. Do not increase snapshot frequency or entity transmission to obtain
a misleading CPU result.

Correctness acceptance should cover:

1. Different per-player prevent-transmit rules and custom `ShouldTransmit` rules,
   with recipients processed in both orders.
2. Weapons, hands, viewmodels, ownership changes, spectator target changes,
   remote cameras and added PVS origins.
3. Open/closed portals, skybox changes, distance limits, movement across BSP
   regions, parent changes, entity deletion and slot reuse.
4. Joining during load, repeated full updates, reconnects, map changes, HLTV and
   existing pre/post transmit hooks. Check both entity visibility and packing.

Adopt the setting only when these cases remain correct and tail latency improves
without increased bandwidth or full updates. Turn `fastpath` back to `0` when
there is no useful headnode reuse or a regression appears. Do not enable
`holylib_networking_enableunsafe64x` for this experiment: it controls separate
snapshot/ABI-sensitive code. Switching the networking module itself off at
runtime remains unsupported.

## Automated evidence and remaining work

`tests/networking_pvs_cache_tests.cpp` checks exact keys, a deliberately constructed
hash collision, zero-filled PVS, negative results, bounded capacity, node
collisions, tick rewinds, map-style resets and 120 distinct PVS contexts. Its
synthetic shared-PVS workload issues 7,680 headnode requests with 64 underlying
queries. That is a query-count result, not a server timing benchmark.

`tests/networking_fastpath_integration_tests.py` compiles the production
`New_CServerGameEnts_CheckTransmit`, `DoTransmitPVSCheck` and PVS function bodies
against a deterministic engine fixture. It compares complete transmit/always
bitsets, recipient callback counts and the packing union with caching on/off for
120 recipients, including different PVS data, live portal changes, full updates,
recipient attachments, range limits, HLTV, parent visibility and area splitting.
The fixture also compiles the production character transmit hook and attachment
exclusion block. Its in-eye cases use a distinct target and a spectator with no
own attachments, and check both recipient orders, already-marked targets,
multiple viewmodels, owner-only weapon exclusion, recursive prevent-transmit
filtering, the packing union, and target changes. The observer case fails against
the initial cache-only revision with the target transmitted but its viewmodels
and hands absent; comparing cache settings alone does not detect that omission.
It does not load GMod or verify engine ABI, real weapon behavior or network I/O.
The CI regression job runs both tests under ASan and UBSan.

The next optimization should follow measured profiles. If headnode misses are
rare and recipient callbacks dominate, this cache cannot solve that bottleneck.
Consider separately optimizing repeatable callback-independent work, or area
connectivity keyed by a verified portal generation. Do not return to sharing
final transmit bitsets without proving every recipient-dependent input and
invalidation path. Full plugin builds and live-client acceptance remain distinct
from the deterministic regression fixtures.

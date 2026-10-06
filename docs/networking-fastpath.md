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

- Match the complete PVS byte sequence and its length. Lookups compare the stored
  64-bit hashes of the contexts to locate candidates; an exact comparison is
  required even when hashes match.
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
  entries per context. Node collisions replace results. When all contexts are
  taken, the least recently used one is recycled, so every current recipient's
  context stays resident; unsupported PVS sizes fall back to the original query.
  No per-query heap allocation is needed. There is no scan over player slots.
- Keep contexts across ticks. The answer depends only on the BSP and the PVS
  bytes, so a recipient whose PVS stays the same reuses its own answers on later
  ticks. Reset on module initialization, map activation and shutdown, so
  contexts never survive into a different BSP. Counters accumulate until
  initialization/map activation.
- Answer misses from the context's stored copy of the PVS, so every answer is
  stored under the bytes it was computed from.

With 8 KiB PVS buffers and 128 context slots, storage is approximately 1.3 MiB.
The code assumes the existing server-thread transmit execution. If an entity
callback adds to a recipient's PVS buffer during its `CheckTransmit`, headnode
answers for that recipient keep using the copy taken at its first headnode
query while ordinary cluster tests see the live buffer; nothing is stored under
the wrong PVS. This does not make the rest of the networking module thread-safe.

HLTV/replay retain their separate path and never query the cache. The existing
full-update player additions and the final `g_bWasSeenByPlayer` union still run.
This does not establish that every other networking optimization is equivalent
to the stock engine.

## Bound attachments for spectators and HLTV

With `holylib_networking_bind_viewmodels_to_player` and
`holylib_networking_bind_gmodhands_to_player` (both on by default), viewmodels and
hands skip the ordinary entity checks, and the character transmit hook only adds
them for their owner. The engine's `CBaseViewModel::ShouldTransmit` also sends a
player's viewmodels to spectators watching that player in first person and to
HLTV. `TransmitBoundAttachmentsToViewers` adds these two cases on the common
recipient path for both cache settings, after full updates and before the
packing union. The old fastpath cache-hit helper made similar additions, which
were missing with `fastpath 0`.

- An `OBS_MODE_IN_EYE` spectator of a player receives that player's viewmodels
  and hands, even if the player was already marked for transmission. HLTV
  receives every player's.
- Attachments flagged `FL_EDICT_DONTSEND`, such as unused viewmodel slots, and
  attachments prevented for the recipient are not forced.
- The observed player is not forced on its own. As in the engine, `SetTransmit`
  on an attachment also sends its move parent, so the player arrives with a
  visible viewmodel even when its own `ShouldTransmit` returned
  `FL_EDICT_DONTSEND`. A player that is prevented for the spectator keeps all of
  its attachments back, so it cannot return as their move parent.
- The real recipient stays unchanged, so a spectator does not qualify for
  owner-only inventory transmission.
- There is no spectator-only prevent-transmit mask. Entities that reach the
  recipient through `SetTransmit` recursion or full updates follow the same
  rules as for every other recipient. Masking afterwards would remove prevented
  move parents while keeping their children.

The observer mode and target are read from `DT_BasePlayer` `m_iObserverMode`
and `m_hObserverTarget`. The `CBasePlayer::GetObserverMode()` and
`GetObserverTarget()` virtuals cannot be used: the current x86-64 branch server
binaries (Linux32 and Linux64) have one more virtual ahead of them than
sourcesdk-minimal, so the SDK's slots hold `SetObserverMode(int)` and
`ObserverUse(bool)` there.

## Players carrying always-transmitted entities

An `FL_EDICT_ALWAYS` entity forces its move parents for every recipient. The
per-tick precomputation used to set the parents' bits directly, so a player
carrying such an entity, for example a projected texture or a scripted entity
returning `TRANSMIT_ALWAYS`, was skipped by the full-check loop. Its transmit
hook never ran: the owner lost its viewmodels, hands and weapons, and other
recipients lost its active weapon. Such players are now recorded instead and
transmitted through `SetTransmit` for every recipient, which applies the usual
weapon rules and forces the rest of the parent chain.

`holylib_networking_fastpath_usecluster` remains registered for configuration
compatibility but no longer changes matching. Both values require exact PVS
identity. No engine structure layout or network protocol changes are introduced.

## Measurement and 120-player acceptance

Use `holylib_networking_fastpath_stats` to read context hits, misses, evictions
and bypasses, plus headnode hits and misses. Take counter differences over a
measurement window. A context hit means the recipient's PVS data was already
cached, from another recipient or an earlier tick; it is not a skipped client
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
2. Weapons, hands, viewmodels, ownership changes, in-eye spectating and
   spectator target changes, always-transmitted entities parented to players,
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
hash collision, zero-filled PVS, negative results, node collisions, persistence
until a map-style reset, least-recently-used recycling of a full cache and 120
distinct PVS contexts. Its synthetic shared-PVS workload issues 7,680 headnode
requests with 64 underlying queries. A dispersed workload of 120 distinct PVS
sets over 10 ticks issues 76,800 requests with 7,680 underlying queries, all on
the first tick. These are query-count results, not server timing benchmarks.

`tests/networking_fastpath_integration_tests.py` compiles the production
`New_CServerGameEnts_CheckTransmit`, `DoTransmitPVSCheck` and PVS function bodies
against a deterministic engine fixture. It compares complete transmit/always
bitsets, recipient callback counts and the packing union with caching on/off for
120 recipients, including different PVS data, live portal changes, full updates,
recipient attachments, range limits, HLTV, parent visibility and area splitting.
A separate case changes a recipient's PVS buffer after its first headnode query
and checks that another recipient with the original PVS gets the original answer.

The fixture also compiles the production character transmit hook, the attachment
exclusion block and the always-transmit marking. It models the engine's
attachment hierarchy: viewmodels follow their owner and the hands follow the
first viewmodel. Its in-eye cases use a distinct target and a spectator with no
own attachments, and check both recipient orders, already-marked and hidden
targets, multiple viewmodels, `FL_EDICT_DONTSEND` attachments, binding turned
off, owner-only weapon exclusion, prevent-transmit rules that must match a
recipient who is not spectating, HLTV, the packing union, and target changes.
Another case parents an always-transmitted entity to a player and checks what
the owner and other recipients receive. The fixture entity has no observer
accessors, so production code that calls the `CBasePlayer` virtuals does not
compile. The first observer case fails against the initial cache-only revision
with the target transmitted but its viewmodels and hands absent; comparing cache
settings alone does not detect that omission. The newer observer, always-transmit
and PVS-buffer cases fail against the revisions before their fixes.
It does not load GMod or verify engine ABI, real weapon behavior or network I/O.
The CI regression job runs both tests under ASan and UBSan, with UBSan recovery
disabled so that any report fails the job.

The next optimization should follow measured profiles. If headnode misses are
rare and recipient callbacks dominate, this cache cannot solve that bottleneck.
Consider separately optimizing repeatable callback-independent work, or area
connectivity keyed by a verified portal generation. Do not return to sharing
final transmit bitsets without proving every recipient-dependent input and
invalidation path. Full plugin builds and live-client acceptance remain distinct
from the deterministic regression fixtures.

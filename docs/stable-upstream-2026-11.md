# Stable upstream backports, November 2026

This update brings selected upstream correctness fixes onto the fork's existing
engine compatibility baseline. It is a selective backport, not a merge of
upstream development history.

## Reviewed revisions and selection

- Fork base: `bb9c1c82e14e023a880ea5a1301b1449d924b520`.
- Upstream reviewed through: [`15705335c659c8dfc88e2a9a5963b620a7186922`](https://github.com/RaphaelIT7/gmod-holylib/commit/15705335c659c8dfc88e2a9a5963b620a7186922), 7 November 2026.
- Common ancestor: `a6c98df177b9fd168173f0d1a1451ee7ffcf4c57`.
- The histories have 207 fork-only and 129 upstream-only commits at these
  revisions. Those counts include overlapping fixes and do not measure missing
  functionality. Cherry-picking does not make GitHub's ancestry-based behind
  count disappear.

Upstream has no `stable` branch. Its latest non-prerelease tag,
[`Release0.8`](https://github.com/RaphaelIT7/gmod-holylib/releases/tag/Release0.8),
warns about broken filesystem compatibility. Conversely, the
[current development README](https://github.com/RaphaelIT7/gmod-holylib/blob/15705335c659c8dfc88e2a9a5963b620a7186922/README.md#dev-builds)
targets GMod's `dev` branch and warns about use on `public`.

The selection therefore consists of identifiable bug fixes that can be applied
without changing the fork's SDK or LuaJIT ABI, module defaults, queue protocol,
or LuaPack delivery design. Original upstream commits are recorded in the
backport commit messages. Mixed upstream commits are applied only to the
relevant source paths.

## Included fixes

| Upstream source | Change carried into the fork |
| --- | --- |
| [`1ed6959`](https://github.com/RaphaelIT7/gmod-holylib/commit/1ed6959a8ae8e270b8a19340bcabef02ab4da707) | Run IVP constraint iterations for non-zero solver coefficients. |
| [`0e2985d`](https://github.com/RaphaelIT7/gmod-holylib/commit/0e2985dd50d72e1ed5f52d59aa6f259748c893e5) | Place IVP min-list sentinel indices above the supported allocation range. The unrelated README invitation change is omitted. |
| [`3817ce1`](https://github.com/RaphaelIT7/gmod-holylib/commit/3817ce1e7a5ab45b461f489712aa2ef73b0b2649) | Source-level runtime guards: bounded buffer copies, voice lengths, Lua metatable indices and error formatting, valid file-descriptor cleanup, filesystem sorting/read bounds, interface lookup, transmit-list bounds, PVS recipient handling, SourceTV availability and user-info table matching. LuaJIT changes from this commit are excluded. |
| [`e64bf49`](https://github.com/RaphaelIT7/gmod-holylib/commit/e64bf4989f635cc811098e7a6a2dfc1ffbbfe868) | Disable physics/voice detours through the shared bookkeeping wrapper. The fork's Jolt-specific condition remains in place. |
| [`3d28edc`](https://github.com/RaphaelIT7/gmod-holylib/commit/3d28edcd1337ff0d2658a7c8f28a34bfcfbd0aeb) | Avoid holding a vector iterator across asynchronous util callbacks that may enqueue more work. |
| [`068fefa`](https://github.com/RaphaelIT7/gmod-holylib/commit/068fefadd9fad60c39b4c913a4f05f50ee627fe8) | Reject null/non-player entities in both `Get_Player` paths, including a stale valid-looking handle. The upstream Reconnect test depends on a test file absent from this fork; the production-function regression exercises both resolution paths instead. |
| [`f455308`](https://github.com/RaphaelIT7/gmod-holylib/commit/f45530859f9813b3dce2ca7d919b9fa5509598c4) | Deliver level cleanup when loaded as a binary module, before Lua shutdown and after finalisation. The fork's plugin-think lifecycle stays in `plugin.cpp`. |
| [`6c4d0eb`](https://github.com/RaphaelIT7/gmod-holylib/commit/6c4d0eb92763459b6657975fe9e8b14cdd3d8a79) | Handle missing HTTP responses in the version check and failed-download diagnostic. Other changes in this mixed commit are outside this backport. |
| [`b31d10f`](https://github.com/RaphaelIT7/gmod-holylib/commit/b31d10f02345e3191d5932181303fe638952959d) | Clear a departing player's entity bit in every recipient's prevent-transmit set, as well as clearing that player's own set. A subsequent player reusing the slot no longer inherits invisibility. |
| [`57b8979`](https://github.com/RaphaelIT7/gmod-holylib/commit/57b897944490d90d0f23ba56ed25fe14b603501f) | Reject negative or oversized explicit string-table user-data lengths before copying from the supplied string. |

The timer backport takes the `systimer.cpp` fixes from
[`3aaf104`](https://github.com/RaphaelIT7/gmod-holylib/commit/3aaf1042f00352e01991569b579f8be7feeed954),
[`831792c`](https://github.com/RaphaelIT7/gmod-holylib/commit/831792cfa2857d79ed93e0f106fe93b74c69ab4e),
[`30fc301`](https://github.com/RaphaelIT7/gmod-holylib/commit/30fc301c9ac0d1038c2ad60d5a2bffedb1336b9f),
[`5185e87`](https://github.com/RaphaelIT7/gmod-holylib/commit/5185e8745bea09190bacf7befdc0cf800dbf1f5e),
[`f7fbdb3`](https://github.com/RaphaelIT7/gmod-holylib/commit/f7fbdb3db852895e9c83cdfcf113add27f9783ec),
[`00f37e7`](https://github.com/RaphaelIT7/gmod-holylib/commit/00f37e7982990e1a60cfa36f87f816e5c03693ea), and
[`f79eb0d`](https://github.com/RaphaelIT7/gmod-holylib/commit/f79eb0d2e4dc94f77e0ca85e3b9f65a7720a3376).
These correct removal indices, pause/resume/toggle timing, `TimeLeft` units,
deadline adjustment, reused timers, callback mutation and infinite-repeat
underflow. The fork continues to use its own `Util::Reference*` API. Physical
deletion is deferred until dispatch completes, explicitly removed names become
unavailable immediately, and callback-created timers wait until the next pass.
These adaptations prevent skipped siblings and unbounded zero-delay chains.
Adjusting a paused timer preserves its remaining-duration representation. The
new upstream `Think` and `IsPaused` APIs are not required for this update.

The native error-formatting tests also exposed adjacent buffer-ownership
defects: `ErrorNoHalt` used scalar deletion on an array, `ErrorFromLua` leaked
the formatted buffer, and formatting failures leaked the error object. These
paths now release their own allocations correctly. `CLuaError::message` owns
a separate string copy, so freeing the formatting buffer does not invalidate it.

## Deliberately deferred upstream changes

- SDK and engine ABI updates, including
  [`5883a0e`](https://github.com/RaphaelIT7/gmod-holylib/commit/5883a0ee9321092bd8d5fe71b2fd946b3cd35d1a).
  The SDK pins and network-channel ABI documented in [BUILD.md](../BUILD.md)
  require coordinated validation before changing.
- Upstream's removal of public/prerelease runtime coverage in
  [`95ce60d`](https://github.com/RaphaelIT7/gmod-holylib/commit/95ce60de71157042650f8473cbee3bd632dc4baa).
  The existing test matrix remains intact.
- LuaJIT source/library updates, GC and multi-state refactors, and binary
  refreshes. These form a separate compatibility change; updating only source
  while retaining old linked LuaJIT libraries would be incomplete.
- Filesystem rewrites, recent Lua tokenisation/datapack changes, network
  threading experiments, enabling area splitting by default, and removal of
  the network-debugging module. These need individual integration with the
  fork's existing fixes and runtime validation.
- The separate networking changes in fork PRs
  [#24](https://github.com/ncgst/gmod-holylib/pull/24) and
  [#25](https://github.com/ncgst/gmod-holylib/pull/25). This update does not
  incorporate either PR as a whole; the area-list bounds fix also appears in
  #24 because it is independently present in upstream `3817ce1`.

The LuaPack delivery/recovery implementation, parked-client promotion, VProf
userdata ownership, FFI fixes, and validated x64 symbol resolvers remain on
the fork's current code. All production changes are confined to the selected
fixes and necessary integration adjustments.

## Validation scope

Both the pre-update fork and the candidate pass all 14 existing portable
regression groups, including parsing all 90 GLuaTest files and all 15 CI
telemetry unit tests. Both also pass a full Linux x86-64 `ReleaseWithSymbols`
compile and link using GCC 14.2.0, `garrysmod_common` revision
`15301a9bb93928f511ee979a74c09e2a9b0da59a`, and the pinned SDK
`7ad3ac0b89a951a73e2e10c70491ed643f5eb4e8`. The build uses the existing premake
configuration and `make -C projects/linux/gmake -j4
config=releasewithsymbols_x86_64`; no SDK or toolchain compatibility patches
were needed.

The new stable-upstream regression compiles actual production function bodies
with deterministic engine adapters. Its five cases cover player resolution,
player-slot visibility reuse, string-table data bounds, asynchronous callback
queue reallocation, and binary-module shutdown ordering. All five fail on the
pre-update fork and pass after the backports:

The runtime-guard regression adds 77 assertions across seven groups: heap and
stack bit buffers, voice data, metadata bounds, variadic error formatting,
networking area removal, failed asynchronous reads, and file-descriptor
ownership. Each group fails against the old fork. The timer regression compiles
the production module with a deterministic clock and exercises 13 timing,
mutation and lifetime scenarios. Its negative controls reproduce the fork's
removal-index and repeat-count failures; upstream's unadapted index loop also
fails the sibling-preservation scenario after self-removal.

```sh
python3 tests/stable_upstream_regression_tests.py
python3 tests/upstream_runtime_guard_tests.py --sanitize
python3 tests/systimer_regression_tests.py
```

The first test accepts `--source-root /path/to/old-checkout` to reproduce the
old failures. The guard test accepts `--source-ref` and `--case`; the timer test
accepts `--source` and `--case`. The commands above enable AddressSanitizer and
UndefinedBehaviorSanitizer. Local execution in a ptraced environment requires
`ASAN_OPTIONS=detect_leaks=0`; CI retains normal leak checking.

Portable adapter tests and compilation do not exercise a running GMod server.
Map changes, binary-module loading, live player-slot reuse and engine-specific
behaviour still require the normal runtime qualification. Engine ELF fixture
tests also require separately supplied engine binaries.

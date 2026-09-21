# Sources

Garry's Mod common used (only for 64x): https://github.com/RaphaelIT7/garrysmod_common.git<br> 
SourceSDK minimal used: https://github.com/RaphaelIT7/sourcesdk-minimal.git<br>

The fork's dependency revisions are specified in [.github/workflows/compile.yml](.github/workflows/compile.yml).

The Source SDK revisions are pinned to match this fork's `CNetChan` interface:

| SDK lineage | Revision |
| --- | --- |
| `patch-7` (32-bit and the Windows 64-bit main-branch build) | `0784d6fdfe93bdc017098f3d8422268f7bafb87e` |
| `x86-64-patch-3` (64-bit branch builds) | `7ad3ac0b89a951a73e2e10c70491ed643f5eb4e8` |

These are the revisions used by [the successful build of `a46a758`](https://github.com/ncgst/gmod-holylib/actions/runs/35380219107).
Later SDK commits change `INetChannelInfo::GetTime`, `GetTimeConnected`, and
`INetChannel::GetTimeSinceLastReceived` from `float` to `double`, which conflicts
with this fork's implementations. Use the pinned revision for local builds too.
Updating the SDK requires reviewing the matching engine ABI and local channel
implementation, then validating the supported build and runtime matrix. Keep
the pins consistent across plugin, custom, release, and GhostInj workflows.

# How to build HolyLib

1. Clone garrysmod_common & its submodules into the `development` folder.<br>
So the result should be a structure like this:<br>
\- `development`<br>
\- \- `module`<br>
\- \- `garrysmod_common`<br>

2. Clone the sourcesdk-minimal into the `garrysmod_common/sourcesdk-minimal` folder (remove all files before cloning it)<br>
This needs to be done since in the garrysmod_common, the submodule points at the original sourcesdk-minimal and not at our fork.<br>

3. Enter the `development/module` folder<br>
4. Run `premake5.exe vs2022` or for Linux `premake5.exe gmake`<br>
5. Now you can open the generated project file and do your stuff.<br>

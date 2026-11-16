"""Exercise the production allowance guards without loading GMod.

The request test extracts the entry guard before any native decoder or probe;
LuaPack request decoding is covered separately by the existing policy tests.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument("--compiler", default="c++")
parser.add_argument("--msvc", action="store_true")
parser.add_argument("--sanitize", action="store_true")
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
source = (root / "source/modules/gmoddatapack.cpp").read_text(encoding="utf-8")

def extract(signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for pos in range(opening, len(source)):
        depth += (source[pos] == "{") - (source[pos] == "}")
        if depth == 0:
            return source[start:pos + 1]
    raise AssertionError(signature)

guards = "\n".join(extract(signature) for signature in (
    "static bool AllowNativeDataPackClient(",
    "static void hook_CServerGameClients_GMOD_ClientConnected(",
    "static void hook_CServerGameClients_GMOD_SentClientStringTables(",
))
request = extract("static void hook_GModDataPack_OnFilesRequested(")
entry = request[:request.index("\tauto original =")]
assert 'AllowNativeDataPackClient(clientIdx, "OnFilesRequested", warned)' in entry
assert "clientsVTable[19]" in source and "clientsVTable[20]" in source
program = r'''
#include <cassert>
#include <climits>
#include <cstdio>
#define PROJECT_NAME "test"
struct CGlobalVars { int maxClients = 128; } globals;
CGlobalVars* gpGlobals = &globals;
struct IServerGameClients {} g_clients;
struct GModDataPack {};
struct bf_read {};
int warnings = 0, lookups = 0, missing = -1;
void Warning(const char*, const char*, int) { ++warnings; }
namespace Util {
void* server = &g_clients;
void* GetClientBySlot(int slot) {
    ++lookups;
    assert(slot >= 0 && slot < 128 && slot < gpGlobals->maxClients);
    return slot == missing ? nullptr : &g_clients;
}
}
int calls[3] = {}, allowance[128] = {};
int lastSlot = -1;
void native(int path, int slot) {
    assert(slot >= 0 && slot < 128);
    lastSlot = slot;
    ++calls[path];
    if (path == 0) allowance[slot] = 0;
    if (path == 1) ++allowance[slot];
    if (path == 2 && allowance[slot] > 0) --allowance[slot];
}
void connected(IServerGameClients* self, int slot) { assert(self == &g_clients); native(0, slot); }
void tables(IServerGameClients* self, int slot) { assert(self == &g_clients); native(1, slot); }
using NativeDataPackClientCallback = void (*)(IServerGameClients*, int);
struct Hook {
    NativeDataPackClientCallback callback;
    template<class T> T GetTrampoline() { return callback; }
};
Hook detour_CServerGameClients_GMOD_ClientConnected{connected};
Hook detour_CServerGameClients_GMOD_SentClientStringTables{tables};
// GUARDS
// REQUEST
    (void)pDataPack; (void)message; (void)bits;
    native(2, clientIdx);
}
void check(int slot, bool allowed) {
    int before[3] = {calls[0], calls[1], calls[2]};
    hook_CServerGameClients_GMOD_ClientConnected(&g_clients, slot);
    hook_CServerGameClients_GMOD_SentClientStringTables(&g_clients, slot);
    // Even null decoder state must not route an invalid slot to a native fallback.
    hook_GModDataPack_OnFilesRequested(nullptr, slot, nullptr, 0);
    for (int path = 0; path < 3; ++path) assert(calls[path] == before[path] + int(allowed));
    if (allowed) { assert(lastSlot == slot); assert(allowance[slot] == 0); }
}
int main() {
    // Preserve every real slot, including the last physical player (127).
    for (int slot = 0; slot < 128; ++slot) check(slot, true);
    assert(warnings == 0);
    for (int cap : {1, 64, 128, 255}) {
        globals.maxClients = cap;
        for (int slot : {INT_MIN, -1, 0, 1, 63, 64, 127, 128, 132, 133, 184, 254, 255, INT_MAX})
            check(slot, slot >= 0 && slot < cap && slot < 128);
    }
    assert(warnings == 3); // One diagnostic per path, regardless of later drops.
    missing = 0;
    check(0, false);
    missing = -1;
    lookups = 0;
    gpGlobals = nullptr;
    check(0, false);
    gpGlobals = &globals;
    Util::server = nullptr;
    check(0, false);
    assert(lookups == 0);
    Util::server = &g_clients;
    globals.maxClients = 0;
    check(0, false);
    globals.maxClients = 128;
    check(127, true); // A promoted/real slot remains usable after a rejected call.
    assert(warnings == 3);
    std::puts("Native allowance guard boundaries and callback forwarding passed");
}
'''
program = "#include <initializer_list>\n" + program.replace("// GUARDS", guards).replace("// REQUEST", entry)
with tempfile.TemporaryDirectory(prefix="datapack-allowance-") as tmp:
    cpp, binary = Path(tmp) / "test.cpp", Path(tmp) / ("test.exe" if args.msvc else "test")
    cpp.write_text(program, encoding="utf-8")
    if args.msvc:
        command = [args.compiler, "/nologo", "/EHsc", "/std:c++17", "/W4", str(cpp), "/Fe:" + str(binary)]
    else:
        command = [args.compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic", str(cpp), "-o", str(binary)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
    subprocess.run(command, cwd=tmp, check=True)
    subprocess.run([str(binary)], check=True, timeout=20)

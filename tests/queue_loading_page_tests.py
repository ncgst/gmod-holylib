"""Compile the production signon hooks and codecs, then decode their wire output independently."""
import argparse
import hashlib
from pathlib import Path
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SDK = ROOT / "source/sourcesdk"


def segment(text, start, stop):
    first = text.index(start)
    return text[first:text.index(stop, first + len(start))]


def body(text, signature):
    first = text.index(signature + "\n{")
    return text[first:text.index("\n}", first) + 2] + "\n"


def production_fixture():
    header = (SDK / "netmessages.h").read_text(encoding="utf-8")
    codecs = (SDK / "netmessages.cpp").read_text(encoding="utf-8")
    registry = (SDK / "GameEventManager.h").read_text(encoding="utf-8")
    lookup = (SDK / "gameeventmanager.cpp").read_text(encoding="utf-8")
    game = (ROOT / "source/modules/gameserver.cpp").read_text(encoding="utf-8")
    messages = [segment(header, "#define DECLARE_BASE_MESSAGE", "// bidirectional net messages:"),
                segment(header, "class SVC_ServerInfo :", "class SVC_SendTable :")]
    for name in ("SVC_GameEvent", "SVC_GameEventList"):
        first = header.index("class " + name + " :")
        messages.append(header[first:header.index("\n};", first) + 3])
    for name in ("SVC_ServerInfo", "SVC_GameEvent", "SVC_GameEventList"):
        for method, arg in (("WriteToBuffer", "bf_write"), ("ReadFromBuffer", "bf_read")):
            messages.append(body(codecs, f"bool {name}::{method}( {arg} &buffer )"))
    parking = [
        segment(game, "struct QueuePageConnection\n", "// Queue clients live outside"),
        body(game, "static void hook_CGameServer_RemoveClientFromGame(CBaseServer* _this, CBaseClient* pClient)"),
        body(game, "static bool SendReliableToEngineChannel(INetChannel* channel, INetMessage& msg)"),
        segment(game, "static bool IsCurrentParkedReply(", "static Detouring::Hook detour_CGameClient_SetSignonState;"),
        body(game, "static bool hook_CGameClient_SetSignonState(CGameClient* client, int state, int spawncount)"),
    ]
    # Exercise the same resets as promotion/reuse and check the native hook bindings.
    for start, end in [
        ("static CBaseClient* GetFreeQueueClient(CBaseServer* _this, netadr_t& adr)\n{", "static Detouring::Hook detour_CBaseServer_GetFreeClient;"),
        ("static void hook_CGameServer_RemoveClientFromGame(", "static ConVar gameserver_steamlookup_queueclients"),
        ("void CGameServerModule::OnClientDisconnect(", "static ConVar* host_timescale"),
    ]:
        assert "ResetQueuePageConnection(" in segment(game, start, end)
    assert 'gameserver_queue_loadingpage("holylib_gameserver_queue_loadingpage", "0"' in game
    # The Linux sender fixture supplies a function pointer. Also check that
    # the engine resolver which populates it has the i386 entry in slot zero.
    symbols = (ROOT / "source/symbols.cpp").read_text(encoding="utf-8")
    sender = segment(symbols, "const std::vector<Symbol> CNetChan_SendNetMsgSym = {", "\n\t};")
    assert sender.splitlines()[1].strip().startswith('Symbol::FromName("_ZN8CNetChan10SendNetMsgER11INetMessagebb")')
    fixture = (ROOT / "tests/queue_loading_page_fixture.cpp").read_text(encoding="utf-8")
    descriptor = segment(registry, "class CGameEventDescriptor\n", "class CGameEvent :")
    descriptor += segment(game, "// GetEventDescriptor walks", "struct QueuePageConnection")
    fixture = fixture.replace("// PRODUCTION DESCRIPTOR", descriptor)
    fixture = fixture.replace("// PRODUCTION LOOKUP", body(lookup, "CGameEventDescriptor *CGameEventManager::GetEventDescriptor(const char * name)"))
    return fixture.replace("// PRODUCTION MESSAGES", "\n".join(messages)).replace("// PRODUCTION PARKING", "\n".join(parking))


class Bits:
    def __init__(self, data, count):
        self.value = int.from_bytes(data, "little")
        self.pos = 0
        self.count = count

    def take(self, width):
        assert self.pos + width <= self.count
        value = (self.value >> self.pos) & ((1 << width) - 1)
        self.pos += width
        return value

    def string(self):
        data = bytearray()
        while (byte := self.take(8)):
            data.append(byte)
        return data.decode("utf-8")


FIELDS = [("hostname", 1), ("mapname", 1), ("maxplayers", 3), ("loadurl", 1),
          ("gamemode", 1), ("maxplayers_visible", 4)]
URL = "https://queue.invalid/%s/%m"
# Captured from both native Windows type 8 writers with the fixture inputs.
# The six-bit 0x2d sentinel starts at bit 904, just after the visible limit.
NATIVE_SERVERINFO = bytes.fromhex(
    "0806009e158d84ffffffff0101000102030405060708090a0b0c0d0e0f07800000803c6c"
    "6761727279736d6f6400676d5f6669787475726500736b790051756575652046697874757265"
    "0068747470733a2f2f71756575652e696e76616c69642f25732f256d0073616e64626f7800ffff2d")


def parse_pair(data, count):
    bits = Bits(data, count)
    assert bits.take(6) == 30 and bits.take(9) == 1
    end = bits.take(20) + bits.pos
    descriptor_id = bits.take(9)
    assert descriptor_id == 317 and bits.string() == "server_spawn"
    fields = []
    key_positions = {}
    while (kind := bits.take(3)):
        start = bits.pos
        name = bits.string()
        fields.append((name, kind))
        key_positions[name] = start
    assert bits.pos == end and fields == FIELDS
    assert bits.take(6) == 25
    payload = bits.take(11)
    event_id_at = bits.pos
    assert bits.take(9) == descriptor_id
    values = {}
    for name, kind in fields:
        if kind == 1:
            values[name] = bits.string()
        else:
            width = {3: 32, 4: 16}[kind]
            number = bits.take(width)
            values[name] = number - (1 << width) if number >> (width - 1) else number
    assert values == dict(hostname="Queue Fixture", mapname="gm_fixture", maxplayers=128,
                          loadurl=URL, gamemode="sandbox", maxplayers_visible=-1)
    assert bits.pos == event_id_at + payload == count
    return key_positions, event_id_at


def parse_serverinfo(data, count):
    bits = Bits(data, count)
    assert bits.take(6) == 8 and bits.take(16) == 24 and bits.take(32) == 0x12345678
    assert bits.take(1) == 0 and bits.take(1) == 1 and bits.take(32) == 0xffffffff
    assert bits.take(16) == 257 and bytes(bits.take(8) for _ in range(16)) == bytes(range(16))
    assert bits.take(8) == 7 and bits.take(8) == 128
    assert struct.unpack("<f", bits.take(32).to_bytes(4, "little"))[0] == 0.015625
    assert bits.take(8) == ord("l")
    assert [bits.string() for _ in range(4)] == ["garrysmod", "gm_fixture", "sky", "Queue Fixture"]
    tail = bits.pos
    assert bits.string() == URL and bits.string() == "sandbox"
    assert bits.take(16) == 0xffff and bits.take(6) == 0x2d and bits.pos == count
    return tail


def must_reject(parser, data, count):
    try:
        parser(data, count)
    except (AssertionError, ValueError, KeyError):
        return
    raise AssertionError("decoder accepted a negative control")


def verify(output):
    frames = {}
    for line in output.strip().splitlines():
        name, count, data = line.split()
        frames[name] = (bytes.fromhex(data), int(count))
    data, count = frames["pair"]
    # All three native writers produced this stream, including its final partial byte.
    assert count == 1243 and hashlib.sha256(data).hexdigest() == "41f1610b32d026b376aafd5f700a248552bbb2ecc441a5cc2f25b31b549e2302"
    keys, event_id = parse_pair(data, count)
    value = int.from_bytes(data, "little")
    for bit in (keys["loadurl"], event_id, 6 + 9 + 20):
        must_reject(parse_pair, (value ^ (1 << bit)).to_bytes(len(data), "little"), count)
    # Omit gamemode from both frames while keeping their advertised lengths valid.
    desc_start = keys["gamemode"] - 3
    desc_width = 3 + 8 * len(b"gamemode\0")
    value_start = event_id + 9 + 8 * (len(b"Queue Fixture\0gm_fixture\0") + len(URL) + 1) + 32
    value_width = 8 * len(b"sandbox\0")
    def without(integer, first, width):
        return (integer & ((1 << first) - 1)) | ((integer >> (first + width)) << first)
    omitted = without(without(value, value_start, value_width), desc_start, desc_width)
    omitted -= desc_width << (6 + 9)
    omitted -= value_width << (event_id - desc_width - 11)
    omitted_count = count - desc_width - value_width
    must_reject(parse_pair, omitted.to_bytes((omitted_count + 7) // 8, "little"), omitted_count)
    # Insert five bits into the legacy 16-bit length field, as the fork used to do.
    low = (1 << event_id) - 1
    widened = (value & low) | ((value & ~low) << 5)
    must_reject(parse_pair, widened.to_bytes((count + 12) // 8, "little"), count + 5)
    data, count = frames["serverinfo"]
    assert count == 910 and data == NATIVE_SERVERINFO
    tail = parse_serverinfo(data, count)
    swap = b"sandbox\0" + URL.encode() + b"\0"
    width = len(swap) * 8
    value = int.from_bytes(data, "little") & ~(((1 << width) - 1) << tail)
    value |= int.from_bytes(swap, "little") << tail
    must_reject(parse_serverinfo, value.to_bytes(len(data), "little"), count)


def main():
    args = argparse.ArgumentParser()
    args.add_argument("--msvc", action="store_true")
    args.add_argument("--sanitize", action="store_true")
    options = args.parse_args()
    with tempfile.TemporaryDirectory(prefix="holylib-queue-page-") as directory:
        work = Path(directory)
        cpp = work / "fixture.cpp"
        cpp.write_text(production_fixture(), encoding="utf-8")
        if options.msvc:
            command = ["cl", "/nologo", "/std:c++17", "/EHsc", "/utf-8", "/W4", "/WX", "/wd4100", str(cpp)]
            if options.sanitize:
                command += ["/fsanitize=address", "/Zi"]
        else:
            command = ["c++", "-std=c++17", "-O1", "-Wno-invalid-offsetof", str(cpp)]
            if options.sanitize:
                command += ["-fsanitize=address", "-fno-omit-frame-pointer"]
        define = "/D" if options.msvc else "-D"
        for name, sender in (("virtual", []), ("resolved", [define + "SYSTEM_LINUX=1", define + "REPLAY_ENABLED=1"])):
            binary = work / (name + (".exe" if options.msvc else ""))
            output = ["/Fe" + str(binary)] if options.msvc else ["-o", str(binary)]
            subprocess.run(command + sender + output, cwd=work, check=True)
            result = subprocess.run([str(binary)], cwd=work, text=True, capture_output=True)
            if result.returncode:
                raise RuntimeError(result.stderr or f"fixture exited {result.returncode}")
            verify(result.stdout)
    print("Queue lifecycle, reliable event pair, ServerInfo ABI/round trip and negative controls passed.")


if __name__ == "__main__":
    main()

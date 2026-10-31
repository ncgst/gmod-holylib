// The driver inserts the production declarations, serializers and signon hooks.
// Only the bit I/O, engine objects and Lua callbacks are fixture implementations.
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

constexpr int NETMSG_TYPE_BITS = 6, NETMSG_LENGTH_BITS = 16, MAX_EVENT_BITS = 9;
constexpr int svc_ServerInfo = 8, svc_GameEvent = 25, svc_GameEventList = 30;
constexpr int MAX_OSPATH = 260, MD5_DIGEST_LENGTH = 16, PROTOCOL_VERSION_17 = 17;
constexpr int SIGNONSTATE_CONNECTED = 2, ABSOLUTE_PLAYER_LIMIT = 255;
constexpr int SIGNON_TIME_OUT = 300, NET_MAX_PAYLOAD = 1 << 18;
#define Assert assert
#define VPROF(label) ((void)0)
#define PROJECT_NAME "fixture"
using CRC32_t = uint32_t;
struct MD5Value_t { unsigned char bits[16]; };

class bf_write
{
public:
	void StartWriting(void* p, int bytes, int offset = 0, int bits = -1)
	{
		data = static_cast<unsigned char*>(p);
		cursor = offset;
		limit = bits < 0 ? bytes * 8 : bits;
		overflow = false;
	}
	void WriteUBitLong(uint32_t value, int width)
	{
		if (width > GetNumBitsLeft()) { overflow = true; return; }
		for (int i = 0; i < width; ++i, ++cursor)
		{
			const unsigned mask = 1u << (cursor % 8);
			auto& byte = data[cursor / 8];
			byte = static_cast<unsigned char>((byte & ~mask) | (((value >> i) & 1u) ? mask : 0u));
		}
	}
	void WriteOneBit(int v) { WriteUBitLong(v, 1); }
	void WriteByte(int v) { WriteUBitLong(v, 8); }
	void WriteChar(int v) { WriteByte(v); }
	void WriteShort(int v) { WriteUBitLong(v, 16); }
	void WriteWord(int v) { WriteShort(v); }
	void WriteLong(uint32_t v) { WriteUBitLong(v, 32); }
	void WriteFloat(float v) { uint32_t n; std::memcpy(&n, &v, 4); WriteLong(n); }
	bool WriteBits(const void* p, int n)
	{
		auto* src = static_cast<const unsigned char*>(p);
		for (int i = 0; i < n; ++i) WriteOneBit((src[i / 8] >> (i % 8)) & 1);
		return !overflow;
	}
	bool WriteBytes(const void* p, int bytes) { return WriteBits(p, bytes * 8); }
	bool WriteString(const char* p) { return WriteBytes(p, static_cast<int>(std::strlen(p) + 1)); }
	int GetNumBitsLeft() const { return limit - cursor; }
	int GetNumBitsWritten() const { return cursor; }
	unsigned char* GetData() { return data; }
	bool IsOverflowed() const { return overflow; }
private:
	unsigned char* data = nullptr;
	int cursor = 0, limit = 0;
	bool overflow = false;
};

class bf_read
{
public:
	bf_read() = default;
	bf_read(const void* p, int bits) : data(static_cast<const unsigned char*>(p)), limit(bits) {}
	uint32_t ReadUBitLong(int width)
	{
		if (cursor + width > limit) { overflow = true; return 0; }
		uint32_t v = 0;
		for (int i = 0; i < width; ++i, ++cursor)
			v |= uint32_t((data[cursor / 8] >> (cursor % 8)) & 1u) << i;
		return v;
	}
	int ReadOneBit() { return ReadUBitLong(1); }
	int ReadByte() { return ReadUBitLong(8); }
	int ReadChar() { return static_cast<int8_t>(ReadByte()); }
	int ReadShort() { return static_cast<int16_t>(ReadUBitLong(16)); }
	int ReadWord() { return ReadUBitLong(16); }
	int32_t ReadLong() { return static_cast<int32_t>(ReadUBitLong(32)); }
	float ReadFloat() { uint32_t n = ReadUBitLong(32); float v; std::memcpy(&v, &n, 4); return v; }
	bool ReadBytes(void* p, int n)
	{
		for (int i = 0; i < n; ++i) static_cast<unsigned char*>(p)[i] = static_cast<unsigned char>(ReadByte());
		return !overflow;
	}
	bool ReadString(char* dst, size_t capacity)
	{
		size_t n = 0;
		for (int c = ReadByte(); c; c = ReadByte())
			if (n + 1 < capacity) dst[n++] = static_cast<char>(c);
		dst[n] = 0;
		return !overflow;
	}
	bool SeekRelative(int n) { if (n > limit - cursor) { overflow = true; return false; } cursor += n; return true; }
	bool IsOverflowed() const { return overflow; }
	int GetNumBitsRead() const { return cursor; }
private:
	const unsigned char* data = nullptr;
	int cursor = 0, limit = 0;
	bool overflow = false;
};

class INetChannel;
class SVC_ServerInfo;
class SVC_GameEvent;
class SVC_GameEventList;
struct IServerMessageHandler
{
	bool ProcessServerInfo(SVC_ServerInfo*) { return false; }
	bool ProcessGameEvent(SVC_GameEvent*) { return false; }
	bool ProcessGameEventList(SVC_GameEventList*) { return false; }
};
struct INetChannelInfo { enum { GENERIC, SIGNON, EVENTS }; };
struct INetMessage
{
	virtual ~INetMessage() = default;
	virtual bool ReadFromBuffer(bf_read&) = 0;
	virtual bool WriteToBuffer(bf_write&) = 0;
	virtual int GetType() const = 0;
	virtual const char* GetName() const = 0;
	virtual const char* ToString() const = 0;
};
// PRODUCTION MESSAGES

const char* SVC_ServerInfo::ToString() const { return GetName(); }
const char* SVC_GameEvent::ToString() const { return GetName(); }
const char* SVC_GameEventList::ToString() const { return GetName(); }

class INetChannel
{
public:
	std::array<unsigned char, 768> data{};
	bf_write out;
	int calls = 0, setup = 0;
	bool loopback = false;
	bool IsLoopback() const { return loopback; }
	INetChannel() { reset(); }
	void reset(int bits = -1) { data.fill(0); out.StartWriting(data.data(), (int)data.size(), 0, bits); }
	bool SendNetMsg(INetMessage& msg, bool reliable, bool voice)
	{
		assert(reliable && !voice && msg.GetType() == 30);
		++calls;
		return msg.WriteToBuffer(out);
	}
	void SetTimeout(int) { ++setup; }
	void SetFileTransmissionMode(bool) { ++setup; }
	void SetMaxBufferSize(bool, int) { ++setup; }
};
struct CNetChan : INetChannel {};
#if defined(SYSTEM_LINUX)
bool (*g_pEngineCNetChanSendNetMsg)(void*, INetMessage&, bool, bool) =
	[](void* ptr, INetMessage& msg, bool reliable, bool voice) {
		return static_cast<CNetChan*>(ptr)->SendNetMsg(msg, reliable, voice);
	};
#endif
struct IServer
{
	bool dedicated = true;
	bool IsDedicated() const { return dedicated; }
};
struct CBaseServer : IServer
{
	int fills = 0;
	void (*duringFill)() = nullptr;
	const char* url = "https://queue.invalid/%s/%m";
	// Store by the verified engine byte offsets, without using the member layout.
	template<class T> static void field(SVC_ServerInfo& info, size_t offset, T value)
	{
		std::memcpy(reinterpret_cast<unsigned char*>(&info) + offset, &value, sizeof(value));
	}
	void FillServerInfo(SVC_ServerInfo& info)
	{
		++fills;
		const bool x64 = sizeof(void*) == 8;
		const char* strings[] = {"garrysmod", "gm_fixture", "sky", "Queue Fixture", url, "sandbox"};
		for (size_t i = 0; i < 6; ++i) field(info, (x64 ? 0x50 : 0x40) + i * sizeof(void*), strings[i]);
		field(info, x64 ? 0x80 : 0x58, int16_t(-1));
		field(info, x64 ? 0x40 : 0x30, 128);
		if (duringFill) duringFill();
	}
};
struct KeyValues;
struct CGameEventCallback;
constexpr int MAX_EVENT_NAME_LENGTH = 32;
template<class T> struct CUtlVector
{
	T* data = nullptr;
	int allocated = 0, growth = 0, count = 0;
	T* elements = nullptr;
	int Count() const { return count; }
	T& operator[](int i) { return data[i]; }
};
// PRODUCTION DESCRIPTOR
struct CGameEventManager
{
	enum { TYPE_STRING = 1, TYPE_LONG = 3, TYPE_SHORT = 4 };
	CUtlVector<CGameEventDescriptor> m_GameEvents;
	CGameEventDescriptor descriptors[2];
	CGameEventManager()
	{
		std::memcpy(descriptors[0].name, "fixture_event", sizeof("fixture_event"));
		std::memcpy(descriptors[1].name, "server_spawn", sizeof("server_spawn"));
		descriptors[0].eventid = 6;
		descriptors[1].eventid = 317;
		m_GameEvents.data = m_GameEvents.elements = descriptors;
		m_GameEvents.count = 2;
	}
	CGameEventDescriptor* GetEventDescriptor(const char* name);
};
#define Q_strcmp std::strcmp
// PRODUCTION LOOKUP
CBaseServer server;
CGameEventManager fixtureEvents;
namespace Util { IServer* server = &::server; CGameEventManager* gameeventmanager = &fixtureEvents; }
struct CBaseClient
{
	int slot = 128, uid = 93, challenge = 602, m_nSignonState = 2;
	bool m_bSendServerInfo = true, known = true, registered = true, fake = false;
	CNetChan* channel = nullptr;
	int GetPlayerSlot() const { return slot; }
	int GetUserID() const { return uid; }
	int GetClientChallenge() const { return challenge; }
	INetChannel* GetNetChannel() { return channel; }
	bool IsFakeClient() const { return fake; }
	IServer* GetServer() { return &server; }
};
struct CGameClient : CBaseClient {};
bool IsKnownClient(CBaseClient* c) { return c && c->known; }
bool IsParkedQueueClient(CBaseClient* c) { return c->slot >= 128; }
static bool IsRegisteredQueueClient(CGameClient* c) { return c->registered; }
struct Toggle { bool value = false; bool GetBool() const { return value; } } gameserver_queue_loadingpage;
int warnings = 0, engineBase = 0, engineDerived = 0, datapack = 0;
void Warning(const char*, ...) { ++warnings; }
bool GMODDataPack_SetSignOnState(CBaseClient*, int) { ++datapack; return false; }
using SignonFn = bool (*)(CBaseClient*, int, int);
using GameSignonFn = bool (*)(CGameClient*, int, int);
namespace Symbols
{
	using CBaseClient_SetSignonState = SignonFn;
	using CGameClient_SetSignonState = GameSignonFn;
	using CGameServer_RemoveClientFromGame = void (*)(CBaseServer*, CBaseClient*);
}
bool baseSignon(CBaseClient*, int, int) { ++engineBase; return true; }
bool gameSignon(CGameClient*, int, int) { ++engineDerived; return true; }
template<class F> struct Hook
{
	F fn;
	bool enabled = true;
	bool IsEnabled() const { return enabled; }
	template<class T> T GetTrampoline() { return fn; }
};
Hook<SignonFn> detour_CBaseClient_SetSignonState{baseSignon};
Hook<GameSignonFn> detour_CGameClient_SetSignonState{gameSignon};
Hook<Symbols::CGameServer_RemoveClientFromGame> detour_CGameServer_RemoveClientFromGame{
	[](CBaseServer*, CBaseClient*) { assert(false && "parked game-DLL removal"); }
};
struct LuaStub
{
	void (*action)() = nullptr;
	bool veto = false;
	void PushNumber(int) {}
	bool CallFunctionProtected(int, int, bool) { if (action) action(); return true; }
	bool GetBool(int) { return veto; }
	void Pop(int) {}
} lua, *g_Lua = &lua;
namespace Lua { bool PushHook(const char*) { return lua.veto || lua.action; } }
void Push_CBaseClient(LuaStub*, CBaseClient*) {}
#define MODULE_EXISTS_GMODDATAPACK 1
// PRODUCTION PARKING

void dump(const char* name, bf_write& out)
{
	std::printf("%s %d ", name, out.GetNumBitsWritten());
	for (int i = 0; i < (out.GetNumBitsWritten() + 7) / 8; ++i) std::printf("%02x", out.GetData()[i]);
	std::puts("");
}

void testServerInfo()
{
	SVC_ServerInfo source{}, target{};
	server.FillServerInfo(source);
	source.m_nProtocol = 24;
	source.m_nServerCount = 0x12345678;
	source.m_bIsHLTV = false;
	source.m_bIsDedicated = true;
	source.m_cOS = 'l';
	source.m_nMaxClasses = 257;
	for (int i = 0; i < 16; ++i) source.m_nMapMD5.bits[i] = static_cast<unsigned char>(i);
	source.m_nPlayerSlot = 7;
	source.m_fTickInterval = 0.015625f;
	unsigned int storage[512]{};
	bf_write out;
	out.StartWriting(storage, sizeof(storage));
	assert(source.WriteToBuffer(out));
	const int frame = out.GetNumBitsWritten();
	out.WriteUBitLong(0x2d, 6);
	dump("serverinfo", out);

	bf_read input(storage, out.GetNumBitsWritten());
	assert(input.ReadUBitLong(6) == 8 && target.ReadFromBuffer(input));
	assert(input.GetNumBitsRead() == frame && input.ReadUBitLong(6) == 0x2d);
	assert(target.m_nMaxClientsVisible == -1 && !std::strcmp(target.m_szLoadingURL, server.url));
	assert(!std::strcmp(target.m_szGameMode, "sandbox") && target.m_nPlayerSlot == 7);
	const auto base = reinterpret_cast<const char*>(&target);
	const char* strings[] = {target.m_szGameDir, target.m_szMapName, target.m_szSkyName, target.m_szHostName,
		target.m_szLoadingURL, target.m_szGameMode};
	for (int i = 0; i < 6; ++i) assert(strings[i] == base + (sizeof(void*) == 8 ? 0x82 : 0x5a) + i * 260);
	bf_read truncated(storage, frame - 1);
	assert(truncated.ReadUBitLong(6) == 8 && !target.ReadFromBuffer(truncated));

	// The older GMod reader uses a CRC instead of the sixteen-byte MD5.
	unsigned int legacy[512]{};
	bf_write demo;
	demo.StartWriting(legacy, sizeof(legacy));
	for (int bit = 0; bit < frame; ++bit)
	{
		if (bit == 6) { demo.WriteShort(17); bit += 15; continue; }
		if (bit == 104) { demo.WriteLong(0x0088abcd); bit += 127; continue; }
		demo.WriteOneBit((storage[bit / 32] >> (bit % 32)) & 1);
	}
	bf_read old(legacy, demo.GetNumBitsWritten());
	assert(old.ReadUBitLong(6) == 8 && target.ReadFromBuffer(old));
	assert(target.m_nMapCRC == 0x0088abcd && target.m_nMaxClientsVisible == -1);
}

void testPayloadLimits()
{
	SVC_ServerInfo info{};
	server.FillServerInfo(info);
	info.m_szHostName = info.m_szMapName = info.m_szGameMode = "";
	std::string url(244, 'a'); // 248 string bytes including all four NULs
	info.m_szLoadingURL = url.c_str();
	QueueLoadingPageMessage pair;
	assert(!pair.Prepare(-1, info) && !pair.Prepare(512, info));
	assert(pair.Prepare(0, info) && pair.Prepare(511, info));
	unsigned int data[128]{};
	bf_write out;
	out.StartWriting(data, sizeof(data));
	out.WriteUBitLong(5, 3);
	assert(pair.WriteToBuffer(out));
	const int bits = out.GetNumBitsWritten();
	out.WriteUBitLong(45, 6);
	bf_read input(data, out.GetNumBitsWritten());
	SVC_GameEventList list;
	SVC_GameEvent event;
	assert(input.ReadUBitLong(3) == 5 && input.ReadUBitLong(6) == 30 && list.ReadFromBuffer(input));
	assert(list.m_nNumEvents == 1 && list.m_DataIn.ReadUBitLong(9) == 511);
	assert(input.ReadUBitLong(6) == 25 && event.ReadFromBuffer(input));
	assert(event.m_nLength == 2041 && event.m_DataIn.ReadUBitLong(9) == 511);
	assert(input.GetNumBitsRead() == bits && input.ReadUBitLong(6) == 45);
	url.push_back('x');
	info.m_szLoadingURL = url.c_str();
	assert(!pair.Prepare(0, info));
}

CGameClient client;
CNetChan first, second;
void testParking()
{
	client.channel = &first;
	auto reply = [] { return hook_CGameClient_SetSignonState(&client, 2, -1); };
	const int start = server.fills;
	assert(reply() && first.calls == 0 && server.fills == start && !client.m_bSendServerInfo);
	gameserver_queue_loadingpage.value = true;
	lua.veto = true;
	assert(!reply() && first.calls == 0);
	lua.veto = false;
	assert(reply() && reply() && first.calls == 1 && server.fills == start + 1);
	dump("pair", first.out);
	gameserver_queue_loadingpage.value = false;
	assert(reply());
	gameserver_queue_loadingpage.value = true;
	assert(reply() && first.calls == 1);

	// Same shell, same endpoint, channel, uid, and challenge after a genuine restart.
	hook_CGameServer_RemoveClientFromGame(&server, &client);
	assert(reply() && reply() && first.calls == 2);
	++client.uid;
	assert(reply() && first.calls == 3);
	++client.challenge;
	assert(reply() && first.calls == 4);
	client.channel = &second;
	assert(reply() && second.calls == 1);
	CGameClient peer;
	peer.slot = 129;
	CNetChan peerChannel;
	peer.channel = &peerChannel;
	assert(hook_CGameClient_SetSignonState(&peer, 2, -1) && peerChannel.calls == 1 && second.calls == 1);

	// A physical target can inherit the channel but cannot run the parked path.
	client.slot = 7;
	assert(reply() && second.calls == 1 && engineDerived == 1);
	lua.action = [] { client.slot = 128; };
	client.m_bSendServerInfo = true;
	assert(hook_CBaseClient_SetSignonState(&client, 2, -1) && !client.m_bSendServerInfo && second.calls == 1);
	lua.action = nullptr;
	ResetQueuePageConnection(&client);
	client.channel = nullptr;
	assert(!reply());
	client.channel = &second;
	client.registered = false;
	assert(reply() && second.calls == 1);
	client.registered = true;
	lua.action = [] { ResetQueuePageConnection(&client); }; // recycle during Lua
	assert(!reply() && second.calls == 1);
	lua.action = [] { client.channel = nullptr; }; // promote/disconnect during Lua
	assert(!reply() && second.calls == 1);
	lua.action = nullptr;
	client.channel = &second;
	assert(reply() && second.calls == 2);

	ResetQueuePageConnection(&client);
	const int sentBits = second.out.GetNumBitsWritten();
	second.reset(sentBits / 2 - 1); // exactly one bit short of the complete pair
	const auto before = second.data;
	assert(reply() && reply() && warnings == 1 && second.calls == 3);
	assert(second.data == before && second.out.GetNumBitsWritten() == 0 && !second.out.IsOverflowed());
	second.reset();
	ResetQueuePageConnection(&client);
	fixtureEvents.descriptors[1].eventid = -1;
	assert(reply() && reply() && warnings == 2 && second.calls == 3);
	fixtureEvents.descriptors[1].eventid = 317;
	ResetQueuePageConnection(&client);
	std::string tooLong(300, 'q');
	server.url = tooLong.c_str();
	assert(reply() && reply() && warnings == 3 && second.calls == 3 && second.out.GetNumBitsWritten() == 0);
	server.url = "";
	assert(client.m_nSignonState == 2 && !client.m_bSendServerInfo && !engineBase && !datapack);

	ResetQueuePageConnection(&client);
	server.duringFill = [] { ResetQueuePageConnection(&client); };
	assert(reply() && second.calls == 3); // The native metadata read lost the original attempt.
	server.duringFill = nullptr;
	detour_CGameServer_RemoveClientFromGame.enabled = false;
	assert(reply() && reply() && warnings == 4 && second.calls == 3);
	detour_CGameServer_RemoveClientFromGame.enabled = true;
	server.dedicated = false;
	second.loopback = true;
	ResetQueuePageConnection(&client);
	const int fills = server.fills;
	assert(reply() && reply() && warnings == 5 && second.calls == 3 && server.fills == fills);
	second.loopback = false;
	ResetQueuePageConnection(&client);
	assert(reply() && second.calls == 4); // Remote client on that listen server.
}

int main()
{
	testServerInfo();
	testPayloadLimits();
	testParking();
}

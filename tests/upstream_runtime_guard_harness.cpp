// The runner inserts production bodies; these adapters model the surrounding
// contracts only. They deliberately do not reproduce the guard decisions.
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <climits>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <alloca.h>
#include <fcntl.h>
#include <unistd.h>

#define PROJECT_NAME "holylib"
#define DLL_EXPORT
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#define _alloca alloca
#define LuaDebugPrint(...) ((void)0)

static int checks = 0;
static void Check(bool condition, const char* message)
{
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
static void Warning(const char*, ...) {}
static void Msg(const char*, ...) {}
static void DevMsg(const char*, ...) {}

namespace GarrysMod::Lua::Type
{
    enum { Number, Function };
    static const char* Name[] = {"nil", "boolean", "number"};
}
struct FakeLua;
struct bf_read
{
    void* bytes = nullptr;
    size_t length = 0;
    bf_read() = default;
    bf_read(void* data, size_t size) : bytes(data), length(size) {}
    void StartReading(void* data, size_t size) { bytes = data; length = size; }
    void StartWriting(void* data, size_t size) { bytes = data; length = size; }
};
using bf_write = bf_read;
struct LuaUserData
{
    void* data = nullptr;
    void* GetData() { return data; }
    void Init(FakeLua*, int, void* value, bool) { data = value; }
    void Push(FakeLua*);
    void Release(FakeLua*);
};
struct VoiceData
{
    int iPlayerSlot = 0;
    std::string bytes;
    void SetData(const char* data, uint16_t length) { bytes.assign(data, length); }
};
struct FakeLua
{
    bool numeric = false;
    double number = 0;
    int voiceLength = 0;
    std::string input;
    std::vector<std::unique_ptr<LuaUserData>> wrappers;
    std::vector<void*> allocations;
    std::unique_ptr<VoiceData> voice;
    bf_write* stackBuffer = nullptr;
    std::function<void(bf_write*)> callback;
    ~FakeLua() { for (void* allocation : allocations) ::operator delete(allocation); }
    bool IsType(int, int type) { return numeric && type == GarrysMod::Lua::Type::Number; }
    double GetNumber(int) { return number; }
    double CheckNumberOpt(int position, double fallback) { return position == 3 ? voiceLength : fallback; }
    const char* CheckStringOpt(int, const char*) { return input.c_str(); }
    size_t ObjLen(int) { return input.size(); }
    void CheckType(int, int) {}
    [[noreturn]] void ThrowError(const char* text) { throw std::runtime_error(text); }
    void Push(int) {}
    void CallFunctionProtected(int, int, bool) { callback(stackBuffer); }
};
void LuaUserData::Push(FakeLua* lua) { lua->stackBuffer = static_cast<bf_write*>(data); }
void LuaUserData::Release(FakeLua* lua) { lua->stackBuffer = nullptr; }
namespace Util
{
    static const char* CheckLString(FakeLua* lua, int, size_t* size)
    {
        *size = lua->input.size();
        return lua->input.data();
    }
}
namespace Lua
{
    enum { bf_read };
    struct Data { int GetMetaEntry(int) { return 0; } };
    static Data* GetLuaData(FakeLua*) { static Data data; return &data; }
}
static LuaUserData* PushInlined_bf_read(FakeLua* lua, size_t size)
{
    void* allocation = ::operator new(sizeof(bf_read) + size);
    lua->allocations.push_back(allocation);
    lua->wrappers.push_back(std::make_unique<LuaUserData>());
    lua->wrappers.back()->data = new (allocation) bf_read;
    return lua->wrappers.back().get();
}
static LuaUserData* PushInlined_bf_write(FakeLua* lua, size_t size) { return PushInlined_bf_read(lua, size); }
static bool holylib_canstackalloc(int) { return true; }
#define LUA_FUNCTION_STATIC(name) static int name(FakeLua* LUA)

// INSERT_BITBUF

struct GCstr { std::string bytes; };
namespace Lua
{
    static const char* GetGCStrData(GCstr* value) { return value->bytes.data(); }
    static size_t GetGCStrLength(GCstr* value) { return value->bytes.size(); }
}
static void Push_VoiceData(FakeLua* lua, VoiceData* value) { lua->voice.reset(value); }
#define LUA_JIT_WRAPPED_3(name, ...) static void name(LuaUserData* pUD, GCstr* pCompressedData, int iNewLength)
#define LUA_JIT_RAW_2(name, ...) static void name(LuaUserData* pUD, GCstr* pCompressedData)

// INSERT_VOICE

struct lua_State {};
class CLuaInterface;
namespace GarrysMod::Lua
{
    struct ILuaObject
    {
        bool pushed = false;
        void SetFromStack(int, CLuaInterface*) {}
        void Push(CLuaInterface*) { pushed = true; }
    };
    struct ILuaGameCallback
    {
        struct CLuaError
        {
            static inline int alive = 0;
            std::string message;
            CLuaError() { ++alive; }
            ~CLuaError() { --alive; }
        };
        std::string lastError;
        void LuaError(CLuaError* error) { lastError = error->message; }
    };
}
static GarrysMod::Lua::ILuaObject* ToSimpleObject(GarrysMod::Lua::ILuaObject* object) { return object; }
static int luaL_newmetatable_type(lua_State*, const char*, int) { return 1; }
static GarrysMod::Lua::ILuaGameCallback::CLuaError* ReadStackIntoError(lua_State*)
{ return new GarrysMod::Lua::ILuaGameCallback::CLuaError; }
class CLuaInterface
{
public:
    // Same extent as the real declaration; the production accessors are inserted.
    static constexpr int MAX_METATABLES = 255;
    GarrysMod::Lua::ILuaObject* m_pMetaTables[MAX_METATABLES]{};
    std::vector<std::unique_ptr<GarrysMod::Lua::ILuaObject>> objects;
    lua_State* state = nullptr;
    GarrysMod::Lua::ILuaGameCallback callback;
    GarrysMod::Lua::ILuaGameCallback* m_pGameCallback = &callback;
    const char* pendingError = "literal %% percent";
    GarrysMod::Lua::ILuaObject* CreateObject()
    {
        objects.push_back(std::make_unique<GarrysMod::Lua::ILuaObject>());
        return objects.back().get();
    }
    lua_State* GetState() { return state; }
    const char* GetString(int) { return pendingError; }
    void Pop(int) {}
    const char* GetTypeName(int);
    void CreateMetaTableType(const char*, int);
    bool PushMetaTable(int);
    void ErrorNoHalt(const char*, ...);
    void ErrorFromLua(const char*, ...);
};

// INSERT_LUA_INTERFACE

namespace Util { static int func_lua_pcall(lua_State*, int, int, int) { return 1; } }
struct StateData
{
    CLuaInterface* pLua;
    // INSERT_FAST_PCALL
};
static char g_pCurrentSysError[2048]{};
static std::string forwardedError, capturedError;
static void EngineSysError(bool, const char* format, va_list args)
{
    char buffer[2048];
    vsnprintf(buffer, sizeof(buffer), format, args);
    forwardedError = buffer;
    capturedError = g_pCurrentSysError;
}
namespace Symbols { using Sys_Error_Internal = void (*)(bool, const char*, va_list); }
struct ErrorHook
{
    template<class T> T GetTrampoline() { return &EngineSysError; }
} detour_Sys_Error_Internal;
struct SysError_SkipInfo { std::string msg; uint32_t count; };
static std::vector<SysError_SkipInfo> g_pErrorsToIgnore;

// INSERT_SYS_ERROR

struct CBaseEntity { const char* GetClassname() const { return "fixture"; } };
struct edict_t { int m_EdictIndex; };
static constexpr int MAX_MAP_AREAS = 3;
struct AreaCache
{
    int nCount = 0;
    // A separate allocation lets ASan detect writes beyond a completely full area.
    std::unique_ptr<CBaseEntity*[]> pEntities{new CBaseEntity*[512]{}};
};
struct EntityTransmitCache
{
    bool m_bIsActivelyNetworking = true;
    struct Bits { void Clear(int) {} } pAlwaysTransmitBits, pNeverTransmitBits, pPVSTransmitBits, pFullTransmitBits;
    int nFullEdictCount = -1, nPVSEdictCount = -1;
    CBaseEntity* pFullEntityList[2]{};
    CBaseEntity* pPVSEntityList[2]{};
    AreaCache nAreaEntities[MAX_MAP_AREAS - 1];
    // INSERT_ENTITY_REMOVED
};
struct FileAsyncRequest_t
{
    void* pContext = nullptr;
    void* pData = nullptr;
    const char* pszFilename = "fixture";
    const char* pszPathID = "DATA";
};
using FSAsyncStatus_t = int;

// INSERT_ASYNC_READ
// INSERT_FILE_DESCRIPTOR

static void TestBitbuf()
{
    for (size_t size : {size_t(0), size_t(3), size_t(MAX_BUFFER_SIZE), size_t(MAX_BUFFER_SIZE + 8192)})
    {
        for (auto create : {bitbuf_CreateReadBuffer, bitbuf_CreateWriteBuffer})
        {
            FakeLua lua;
            lua.input.assign(size, 'B');
            Check(create(&lua) == 1, "buffer constructor returns its userdata");
            auto* buffer = static_cast<bf_read*>(lua.wrappers.back()->data);
            Check(buffer->length <= size_t(MAX_BUFFER_SIZE), "buffer allocation is capped");
            Check(std::memcmp(buffer->bytes, lua.input.data(), std::min(size, buffer->length)) == 0, "buffer copies the fitting prefix");
        }
        FakeLua lua;
        lua.input.assign(size, 'S');
        bool called = false;
        lua.callback = [&](bf_write* buffer) {
            called = true;
            Check(buffer->length <= size_t(MAX_BUFFER_SIZE), "stack buffer allocation is capped");
            Check(std::memcmp(buffer->bytes, lua.input.data(), std::min(size, buffer->length)) == 0, "stack callback sees the fitting prefix");
        };
        bitbuf_CreateStackWriteBuffer(&lua);
        Check(called && !lua.stackBuffer, "stack userdata is released after its callback");
    }
    FakeLua lua;
    lua.numeric = true;
    lua.number = 8;
    bitbuf_CreateWriteBuffer(&lua);
    Check(static_cast<bf_write*>(lua.wrappers.back()->data)->length == 9, "numeric allocation requires no source bytes");
}

static void TestVoice()
{
    VoiceData value;
    LuaUserData wrapper{&value};
    GCstr shortInput{"voice"};
    for (int length : {-2, -1, 0, 100})
    {
        VoiceData_SetData(&wrapper, &shortInput, length);
        Check(value.bytes == shortInput.bytes, "explicit voice lengths cannot overread the string");
    }
    VoiceData_SetData(&wrapper, &shortInput, 2);
    Check(value.bytes == "vo", "shorter explicit voice length retains its prefix");
    GCstr largeInput{std::string(70000, 'V')};
    VoiceData_SetData_NoLength(&wrapper, &largeInput);
    Check(value.bytes.size() == 65535, "voice size saturates instead of wrapping uint16_t");
    VoiceData_SetData(&wrapper, &largeInput, INT_MAX);
    Check(value.bytes.size() == 65535, "explicit voice size saturates at the transport limit");
    FakeLua lua;
    lua.input = largeInput.bytes;
    lua.voiceLength = -1;
    voicechat_CreateVoiceData(&lua);
    Check(lua.voice->bytes.size() == 65535, "voice constructor shares the safe length rule");
    WavAudioFile wav;
    wav.Resize(4);
    wav.WriteData("test", 4);
    Check(wav.CurrentPos() == 4 && std::memcmp(wav.GetData(), "test", 4) == 0, "WAV writes may exactly fill their allocation");
    wav.WriteData("x", -1);
    wav.WriteData("x", INT_MAX);
    Check(wav.CurrentPos() == 4, "invalid WAV write lengths leave the position unchanged");
}

static void TestMetadata()
{
    CLuaInterface lua;
    Check(std::string(lua.GetTypeName(-1)) == "none", "negative type name remains supported");
    Check(std::string(lua.GetTypeName(2)) == "number", "last valid type name is available");
    Check(std::string(lua.GetTypeName(3)) == "unknown", "type count is outside the name array");
    Check(!lua.PushMetaTable(-1), "negative metatable index is rejected");
    Check(!lua.PushMetaTable(255), "metatable upper bound is excluded");
    lua.CreateMetaTableType("negative", -1);
    lua.CreateMetaTableType("too large", 255);
    Check(lua.objects.empty(), "invalid metatable ids do not touch the native cache");
    lua.CreateMetaTableType("last", 254);
    Check(lua.PushMetaTable(254) && lua.m_pMetaTables[254]->pushed, "last valid metatable remains usable");
}

static void EmitSysError(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    hook_Sys_Error_Internal_ignore_this_function_in_crashes_check_your_logs_(true, format, args);
    va_end(args);
}
static void TestVarargs()
{
    CLuaInterface lua;
    lua.ErrorNoHalt("%s %d %.2f %%", "value", 37, 2.5);
    Check(lua.callback.lastError == "value 37 2.50 %", "ErrorNoHalt preserves arguments after sizing");
    lua.ErrorFromLua("%s %d %.2f %%", "other", 91, 1.25);
    Check(lua.callback.lastError == "other 91 1.25 %", "ErrorFromLua preserves arguments after sizing");
    StateData data{&lua};
    Check(!data.FastPCall(0, 0, true), "failed protected call reports failure");
    Check(lua.callback.lastError == "literal %% percent", "Lua error text is not used as a format string");
    lua.pendingError = nullptr;
    data.FastPCall(0, 0, true);
    Check(lua.callback.lastError == "<lua error>", "non-string Lua errors have a safe fallback");
    Check(GarrysMod::Lua::ILuaGameCallback::CLuaError::alive == 0, "temporary Lua errors are released");
    EmitSysError("%s %d %.2f", "engine", 73, 4.25);
    Check(forwardedError == "engine 73 4.25", "engine receives the original unconsumed va_list");
    Check(capturedError == forwardedError && g_pCurrentSysError[0] == '\0', "captured engine error is correct and cleared on return");
}

static void TestNetworking()
{
    EntityTransmitCache cache;
    CBaseEntity entities[512], missing;
    edict_t edict{1};
    auto& area = cache.nAreaEntities[0];
    area.nCount = 512;
    for (int i = 0; i < 512; ++i) area.pEntities[i] = &entities[i];
    cache.EntityRemoved(&entities[0], &edict);
    Check(area.nCount == 511 && area.pEntities[0] == &entities[1], "removing from a full area compacts its live entries");
    Check(area.pEntities[511] == nullptr, "full-area removal clears the last valid slot");
    cache.EntityRemoved(&entities[511], &edict);
    Check(area.nCount == 510 && area.pEntities[510] == nullptr, "removing the final entry stays in bounds");
    cache.EntityRemoved(&missing, &edict);
    Check(area.nCount == 510, "unrelated entity removal leaves the area unchanged");
    cache.m_bIsActivelyNetworking = false;
    cache.EntityRemoved(&entities[1], &edict);
    Check(area.nCount == 510, "inactive cache removal remains a no-op");
}

static void TestAsyncRead()
{
    char input[] = {'A', '\0', 'B'};
    for (int size : {-1, 0, 3})
    {
        IAsyncFile file{};
        FileAsyncRequest_t request{&file, input};
        AsyncCallback(request, size, 7);
        Check(file.status == 7 && file.nBytesRead == size, "async read preserves the engine status");
        if (size > 0)
            Check(std::memcmp(file.content, input, 3) == 0 && file.content[3] == '\0', "async read copies binary data and terminates it");
        else
            Check(file.content[0] == '\0', "empty or failed async reads have safe empty data");
        asyncCallback.clear();
    }
    IAsyncFile file{};
    FileAsyncRequest_t request{&file, nullptr};
    AsyncCallback(request, 20, 7);
    Check(file.content[0] == '\0', "missing engine data cannot be dereferenced");
    asyncCallback.clear();
}

static void TestFileDescriptor()
{
    int descriptors[2];
    Check(pipe(descriptors) == 0, "pipe fixture opens");
    { ScopedFileDescriptor descriptor(descriptors[0]); }
    Check(fcntl(descriptors[0], F_GETFD) == -1 && errno == EBADF, "owned file descriptor closes at scope exit");
    close(descriptors[1]);
    const int stderrFlags = fcntl(STDERR_FILENO, F_GETFD);
    { ScopedFileDescriptor descriptor(STDERR_FILENO); }
    Check(fcntl(STDERR_FILENO, F_GETFD) == stderrFlags, "borrowed stderr stays open");
}

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    try
    {
        const std::string name = argv[1];
        if (name == "bitbuf") TestBitbuf();
        else if (name == "voice") TestVoice();
        else if (name == "metadata") TestMetadata();
        else if (name == "varargs") TestVarargs();
        else if (name == "networking") TestNetworking();
        else if (name == "async-read") TestAsyncRead();
        else if (name == "file-descriptor") TestFileDescriptor();
        else return 2;
        std::printf("runtime guards: %s passed (%d checks)\n", name.c_str(), checks);
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}

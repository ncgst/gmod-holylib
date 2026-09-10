// The runner inserts the actual production request, worker, shutdown, and
// initialization methods. Engine/thread adapters below make their interleaving
// deterministic; compression bytes and live engine registration are not modeled.
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

struct BatchGate
{
	std::mutex mutex;
	std::condition_variable changed;
	bool armed = false, claimed = false, released = false;
	void Enter()
	{
		std::unique_lock<std::mutex> lock(mutex);
		if (!armed) return;
		claimed = true;
		changed.notify_all();
		assert(changed.wait_for(lock, std::chrono::seconds(3), [&] { return released; }));
		armed = false;
	}
} batchGate;

namespace Bootil { struct AutoBuffer {}; }
namespace GarrysMod::Lua { struct LuaFile { std::string contents; }; }
namespace Lua
{
	struct ScopedThreadAccess { ScopedThreadAccess() { batchGate.Enter(); } };
	struct StateAccess { explicit StateAccess(void*) {} };
	struct Shared { GarrysMod::Lua::LuaFile* GetCache(const std::string&) { return nullptr; } };
	static Shared* GetShared() { static Shared shared; return &shared; }
}
namespace HolyLib::LuaPack { static void CaptureFileContents(const std::string&, const std::string&) {} }
struct Table { int GetNumStrings() { return 0; } const char* GetString(int) { return ""; } };
struct DataPack { Table* m_pClientLuaFiles = nullptr; };
static DataPack* g_pDataPack = nullptr;
struct Module { int InDebug() { return 0; } } g_pGModDataPackModule;
static void Msg(const char*, ...) {}
#define PROJECT_NAME "test"
#define SIMPLETHREAD_RETURNVALUE unsigned int
using ThreadFunc_t = unsigned int (*)(void*);
using ThreadHandle_t = std::thread*;
static ThreadHandle_t CreateSimpleThread(ThreadFunc_t function, void* data)
{ return new std::thread([=] { function(data); }); }
static void ReleaseThreadHandle(ThreadHandle_t handle) { handle->join(); delete handle; }
static void ThreadSleep(int) { std::this_thread::yield(); }
enum class ThreadState { STATE_NOTRUNNING, STATE_RUNNING, STATE_SHOULD_SHUTDOWN };

class LuaDataPack
{
public:
	struct LuaPackEntry
	{
		std::shared_mutex mutex;
		std::string sourceContent = "unchanged source";
		bool activeRefreshRequested = false, activeRefreshFailed = false;
		bool activeHashRefreshPending = false, forceActiveHashRefreshPending = false;
		std::shared_ptr<const Bootil::AutoBuffer> activeRefreshCompressed;
		bool IsContentReady() const { return true; }
		bool IsReady() const { return true; }
	};
	std::array<LuaPackEntry, 2> m_pLuaFileCache;
	std::vector<int> m_pCompressQueue, m_pActiveHashRefreshQueue;
	std::mutex m_pCompressQueueMutex, m_pActiveHashRefreshQueueMutex;
	std::unordered_map<int, int> m_pActiveHashRefreshTargets, m_pForcedActiveHashRefreshes,
		m_pActiveHashRefreshNextSlots, m_pActiveHashRefreshFailedSlots, m_pActiveHashRefreshRetryAfter;
	std::atomic<ThreadState> m_pWorkerThreadState{ThreadState::STATE_NOTRUNNING};
	ThreadHandle_t m_pWorkerThread = nullptr;
	struct Interface { void InvalidateInterface() {} void* GetLua() { return nullptr; } } pInterface;
	LuaPackEntry* GetPackEntry(int id) { return &m_pLuaFileCache.at(id); }
	void ProcessContent(LuaPackEntry*, int) { assert(false && "ready source must be reused"); }
	void EnsureSourceHash(LuaPackEntry*) {}
	void CompressFile(LuaPackEntry*, int) { assert(false && "ready ordinary payload must be reused"); }
	void AddFileContents(const std::string&, const std::string&) { assert(false); }
	void CompressActiveRefreshPayload(LuaPackEntry* entry)
	{
		std::lock_guard<std::shared_mutex> lock(entry->mutex);
		assert(entry->activeRefreshRequested);
		entry->activeRefreshCompressed = std::make_shared<const Bootil::AutoBuffer>();
		entry->activeRefreshRequested = false;
	}
	std::shared_ptr<const Bootil::AutoBuffer> ActiveRefreshPayload(int fileID, bool& failed);
	void Initialize();
	void Shutdown();
};
static LuaDataPack g_pLuaDataPack;

// INSERT_PRODUCTION_METHODS

template <typename Predicate> static void WaitFor(Predicate predicate)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (!predicate())
	{
		assert(std::chrono::steady_clock::now() < deadline && "unchanged recovery must schedule and finish");
		std::this_thread::yield();
	}
}

int main()
{
	auto& pack = g_pLuaDataPack;
	auto& entry = pack.m_pLuaFileCache[0];
	auto completed = std::make_shared<const Bootil::AutoBuffer>();
	pack.m_pLuaFileCache[1].activeRefreshCompressed = completed;
	pack.Initialize();
	{
		std::lock_guard<std::mutex> lock(batchGate.mutex);
		batchGate.armed = true;
	}
	bool failed = false;
	assert(!pack.ActiveRefreshPayload(0, failed) && !failed);
	{
		std::unique_lock<std::mutex> lock(batchGate.mutex);
		assert(batchGate.changed.wait_for(lock, std::chrono::seconds(3), [] { return batchGate.claimed; }));
	}
	{
		std::lock_guard<std::mutex> lock(pack.m_pCompressQueueMutex);
		assert(pack.m_pCompressQueue.empty()); // ID is now solely in the real worker's private batch.
	}
	std::thread shutdown([&] { pack.Shutdown(); });
	WaitFor([&] { return pack.m_pWorkerThreadState.load() == ThreadState::STATE_SHOULD_SHUTDOWN; });
	{
		std::lock_guard<std::mutex> lock(batchGate.mutex);
		batchGate.released = true;
		batchGate.changed.notify_all();
	}
	shutdown.join();
	assert(!pack.m_pWorkerThread && !entry.activeRefreshCompressed);
	assert(entry.sourceContent == "unchanged source" && entry.IsContentReady() && entry.IsReady());
	assert(pack.m_pLuaFileCache[1].activeRefreshCompressed == completed);

	// Reinitialize without a source/mode invalidation. The production request
	// method must enqueue again despite the abandoned task having left no payload.
	pack.Initialize();
	assert(!pack.ActiveRefreshPayload(0, failed) && !failed);
	WaitFor([&] { return static_cast<bool>(pack.ActiveRefreshPayload(0, failed)); });
	assert(!failed && pack.ActiveRefreshPayload(1, failed) == completed);
	pack.Shutdown();
	assert(!entry.activeRefreshRequested && entry.activeRefreshCompressed);
	assert(pack.m_pLuaFileCache[1].activeRefreshCompressed == completed);
	std::puts("luapack worker lifecycle tests passed");
}

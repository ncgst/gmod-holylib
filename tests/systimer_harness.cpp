// Minimal engine/Lua adapters around the unmodified production timer functions.
// These tests do not emulate the complete Lua VM or require a GMod installation.
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
    std::abort(); \
} } while (false)

namespace TestClock { double microseconds = 100000000.0; }

template <typename T> class CUtlVector {
    std::vector<T> entries;
public:
    int Count() const { return static_cast<int>(entries.size()); }
    void AddToTail(T value) { entries.push_back(value); }
    T& operator[](int index) {
        CHECK(index >= 0 && index < Count());
        return entries[static_cast<size_t>(index)];
    }
    void FastRemove(int index) {
        CHECK(index >= 0 && index < Count());
        entries[static_cast<size_t>(index)] = entries.back();
        entries.pop_back();
    }
    void RemoveAll() { entries.clear(); }
    void Compact() { entries.shrink_to_fit(); }
    auto begin() { return entries.begin(); }
    auto end() { return entries.end(); }
};

namespace Lua {
struct ModuleData { virtual ~ModuleData() = default; };
struct StateData {
    std::map<int, std::unique_ptr<ModuleData>> modules;
    void SetModuleData(int id, ModuleData* data) { modules[id].reset(data); }
    ModuleData* GetModuleData(int id) { return modules.at(id).get(); }
};
}

namespace GarrysMod::Lua {
namespace Type { enum { Nil, Bool, Number, String, Function }; }
struct Value {
    int type = Type::Nil;
    bool boolean = false;
    double number = 0;
    std::shared_ptr<const std::string> string;
    std::function<void()> function;
    Value() = default;
    Value(bool value) : type(Type::Bool), boolean(value) {}
    Value(double value) : type(Type::Number), number(value) {}
    Value(const char* value) : type(Type::String), string(std::make_shared<std::string>(value)) {}
    Value(std::function<void()> value) : type(Type::Function), function(std::move(value)) {}
};

struct ILuaInterface {
    ::Lua::StateData data;
    std::vector<Value> stack;
    std::map<int, Value> references;
    int nextReference = 1;

    const Value& At(int index) const {
        int offset = index > 0 ? index - 1 : static_cast<int>(stack.size()) + index;
        CHECK(offset >= 0 && static_cast<size_t>(offset) < stack.size());
        return stack[static_cast<size_t>(offset)];
    }
    bool IsType(int index, int type) const {
        if (index > 0 && static_cast<size_t>(index) > stack.size())
            return type == Type::Nil;
        return At(index).type == type;
    }
    void CheckType(int index, int type) const { CHECK(IsType(index, type)); }
    const char* CheckString(int index) const {
        CheckType(index, Type::String);
        return At(index).string->c_str();
    }
    double CheckNumber(int index) const {
        CheckType(index, Type::Number);
        return At(index).number;
    }
    double GetNumber(int index) const { return CheckNumber(index); }
    void Push(int index) { Value copy = At(index); stack.push_back(copy); }
    void PushNil() { stack.emplace_back(); }
    void PushBool(bool value) { stack.emplace_back(value); }
    void PushNumber(double value) { stack.emplace_back(value); }
    void CallFunctionProtected(int arguments, int results, bool showError) {
        CheckType(-1, Type::Function);
        // A currently executing Lua closure stays alive after ReferenceFree.
        auto callback = At(-1).function;
        stack.pop_back();
        auto savedStack = std::move(stack);
        stack.clear();
        callback();
        stack = std::move(savedStack);
    }
};
}

using GarrysMod::Lua::ILuaInterface;
using GarrysMod::Lua::Value;
using LuaFunction = int (*)(ILuaInterface*);

namespace Lua {
StateData* GetLuaData(ILuaInterface* lua) { return &lua->data; }
}
namespace Util {
int ReferenceCreate(ILuaInterface* lua, const char* reason) {
    CHECK(!lua->stack.empty());
    int reference = lua->nextReference++;
    lua->references.emplace(reference, lua->stack.back());
    lua->stack.pop_back();
    return reference;
}
void ReferenceFree(ILuaInterface* lua, int reference, const char* reason) {
    CHECK(lua->references.erase(reference) == 1);
}
void ReferencePush(ILuaInterface* lua, int reference) {
    CHECK(lua->references.count(reference) == 1);
    lua->stack.push_back(lua->references.at(reference));
}
void StartTable(ILuaInterface*) {}
void AddFunc(ILuaInterface*, LuaFunction, const char*) {}
void FinishTable(ILuaInterface*, const char*) {}
void NukeTable(ILuaInterface*, const char*) {}
}

class IModule {
public:
    int m_pID = 1;
    virtual ~IModule() = default;
    virtual void LuaInit(ILuaInterface*, bool) {}
    virtual void LuaShutdown(ILuaInterface*) {}
    virtual void LuaThink(ILuaInterface*) {}
    virtual const char* Name() { return "test"; }
    virtual int Compatibility() { return 0; }
    virtual bool SupportsMultipleLuaStates() { return false; }
    bool InDebug() const { return false; }
};
constexpr int LINUX32 = 1, LINUX64 = 2, WINDOWS32 = 4, WINDOWS64 = 8;
void Msg(const char*, ...) {}
#define VPROF_BUDGET(...) ((void)0)
#define FOR_EACH_VEC(vector, index) for (int index = 0; index < (vector).Count(); ++index)
#define LUA_FUNCTION_STATIC(name) static int name(ILuaInterface* LUA)
#define LUA_GetModuleData(type, module, name) \
    static type* Get##name##LuaData(ILuaInterface* lua) { \
        return static_cast<type*>(Lua::GetLuaData(lua)->GetModuleData(module.m_pID)); \
    }

#include "systimer_production.inc"

static Value Call(ILuaInterface& lua, LuaFunction function, std::vector<Value> args) {
    auto savedStack = std::move(lua.stack);
    lua.stack = std::move(args);
    int count = function(&lua);
    CHECK(count >= 0 && count <= 1);
    Value result = count == 1 ? lua.stack.back() : Value{};
    lua.stack = std::move(savedStack);
    return result;
}

struct Fixture {
    ILuaInterface lua;
    Fixture() { g_pSysTimerModule.LuaInit(&lua, false); }
    ~Fixture() {
        g_pSysTimerModule.LuaShutdown(&lua);
        CHECK(lua.references.empty());
    }
    void Create(const char* name, double delay, double repetitions, std::function<void()> callback) {
        Call(lua, timer_Create, {name, delay, repetitions, Value(std::move(callback))});
    }
    void Simple(double delay, std::function<void()> callback) {
        Call(lua, timer_Simple, {delay, Value(std::move(callback))});
    }
    void Remove(const char* name) { Call(lua, timer_Remove, {name}); }
    bool Exists(const char* name) { return Call(lua, timer_Exists, {name}).boolean; }
    bool Pause(const char* name) { return Call(lua, timer_Pause, {name}).boolean; }
    bool UnPause(const char* name) { return Call(lua, timer_UnPause, {name}).boolean; }
    bool Toggle(const char* name) { return Call(lua, timer_Toggle, {name}).boolean; }
    double TimeLeft(const char* name) { return Call(lua, timer_TimeLeft, {name}).number; }
    double RepsLeft(const char* name) { return Call(lua, timer_RepsLeft, {name}).number; }
    bool Adjust(const char* name, double delay) { return Call(lua, timer_Adjust, {name, delay}).boolean; }
    void Think() { g_pSysTimerModule.LuaThink(&lua); }
    void Advance(double seconds) { TestClock::microseconds += seconds * 1000000.0; }
    void ExpectTime(const char* name, double seconds) { CHECK(std::abs(TimeLeft(name) - seconds) < 0.000001); }
};

static void TestRemovalIndex() {
    Fixture f;
    f.Create("first", 10, 1, [] {});
    f.Create("second", 10, 1, [] {});
    f.Remove("first");
    CHECK(!f.Exists("first") && f.Exists("second"));
    CHECK(f.lua.references.size() == 2);
    f.Remove("second");
    CHECK(f.lua.references.empty());
}

static void TestPauseResume() {
    Fixture f;
    int calls = 0;
    f.Create("paused", 10, 1, [&] { ++calls; });
    f.Advance(3);
    CHECK(f.Pause("paused"));
    CHECK(!f.Pause("paused"));
    f.ExpectTime("paused", 7);
    f.Advance(20);
    f.Think();
    CHECK(calls == 0);
    f.ExpectTime("paused", 7);
    CHECK(f.UnPause("paused"));
    f.Advance(6);
    CHECK(!f.UnPause("paused")); // Must not postpone an active timer.
    f.ExpectTime("paused", 1);
    f.Think();
    CHECK(calls == 0);
    f.Advance(1);
    f.Think();
    CHECK(calls == 1 && !f.Exists("paused"));
}

static void TestToggle() {
    Fixture f;
    int calls = 0;
    f.Create("toggle", 5, 1, [&] { ++calls; });
    f.Advance(2);
    CHECK(!f.Toggle("toggle"));
    f.ExpectTime("toggle", 3);
    f.Advance(20);
    CHECK(f.Toggle("toggle"));
    f.ExpectTime("toggle", 3);
    f.Advance(2);
    f.Think();
    CHECK(calls == 0);
    f.Advance(1);
    f.Think();
    CHECK(calls == 1);
}

static void TestAdjust() {
    Fixture f;
    int calls = 0;
    f.Create("adjust", 10, 1, [&] { ++calls; });
    f.Advance(2);
    CHECK(f.Adjust("adjust", 3));
    f.ExpectTime("adjust", 3);
    f.Advance(2);
    f.Think();
    CHECK(calls == 0);
    f.Advance(1);
    f.Think();
    CHECK(calls == 1);
    CHECK(!f.Adjust("missing", 3));
}

static void TestPausedAdjust() {
    Fixture f;
    int calls = 0;
    f.Create("adjust", 10, 1, [&] { ++calls; });
    CHECK(f.Pause("adjust"));
    CHECK(f.Adjust("adjust", 4));
    f.ExpectTime("adjust", 4);
    f.Advance(20);
    f.Think();
    CHECK(calls == 0);
    CHECK(f.UnPause("adjust"));
    f.ExpectTime("adjust", 4);
    f.Advance(4);
    f.Think();
    CHECK(calls == 1);
}

static void TestRecreatePaused() {
    Fixture f;
    int oldCalls = 0, newCalls = 0;
    f.Create("reused", 10, 1, [&] { ++oldCalls; });
    CHECK(f.Pause("reused"));
    f.Create("reused", 2, 1, [&] { ++newCalls; });
    CHECK(f.lua.references.size() == 2);
    f.Advance(2);
    f.Think();
    CHECK(oldCalls == 0 && newCalls == 1);
    CHECK(!f.Exists("reused"));
}

static void TestInfiniteRepetitions() {
    Fixture f;
    int calls = 0;
    f.Create("forever", 1, 0, [&] { ++calls; });
    for (int i = 0; i < 3; ++i) {
        f.Advance(1);
        f.Think();
        CHECK(f.RepsLeft("forever") == 0);
    }
    CHECK(calls == 3 && f.Exists("forever"));
}

static void TestSelfRemoval() {
    Fixture f;
    int self = 0, second = 0, third = 0;
    f.Create("self", 1, 0, [&] {
        ++self;
        f.Remove("self");
        CHECK(!f.Exists("self"));
    });
    f.Create("second", 1, 0, [&] { ++second; });
    f.Create("third", 1, 0, [&] { ++third; });
    f.Advance(1);
    f.Think();
    CHECK(self == 1 && second == 1 && third == 1);
    f.Advance(1);
    f.Think();
    CHECK(self == 1 && second == 2 && third == 2);
}

static void TestRemoveOthers() {
    Fixture f;
    int earlier = 0, remover = 0, removed = 0, tail = 0;
    f.Create("earlier", 1, 0, [&] { ++earlier; });
    f.Create("remover", 1, 0, [&] {
        ++remover;
        f.Remove("earlier");
        f.Remove("removed");
        CHECK(!f.Exists("earlier") && !f.Exists("removed"));
    });
    f.Create("removed", 1, 0, [&] { ++removed; });
    f.Create("tail", 1, 0, [&] { ++tail; });
    f.Advance(1);
    f.Think();
    CHECK(earlier == 1 && remover == 1 && removed == 0 && tail == 1);
}

static void TestRecreateDuringCallback() {
    Fixture f;
    int first = 0, replacement = 0;
    f.Create("one", 1, 1, [&] {
        ++first;
        f.Create("one", 1, 1, [&] { ++replacement; });
    });
    f.Advance(1);
    f.Think();
    CHECK(first == 1 && replacement == 0 && f.Exists("one"));
    CHECK(f.lua.references.size() == 2);
    f.Advance(1);
    f.Think();
    CHECK(first == 1 && replacement == 1 && !f.Exists("one"));
}

static void TestRemoveThenRecreate() {
    Fixture f;
    int original = 0, replacement = 0, sibling = 0;
    f.Create("one", 1, 1, [&] {
        ++original;
        f.Remove("one");
        CHECK(!f.Exists("one"));
        f.Create("one", 0, 1, [&] { ++replacement; });
        CHECK(f.Exists("one"));
    });
    f.Create("sibling", 1, 1, [&] { ++sibling; });
    f.Advance(1);
    f.Think();
    CHECK(original == 1 && replacement == 0 && sibling == 1);
    CHECK(f.Exists("one"));
    f.Think();
    CHECK(replacement == 1 && !f.Exists("one"));
}

static void TestCallbackGrowth() {
    Fixture f;
    int children = 0, sibling = 0;
    f.Simple(0, [&] {
        for (int i = 0; i < 128; ++i)
            f.Simple(0, [&] { ++children; });
    });
    f.Simple(0, [&] { ++sibling; });
    f.Think();
    CHECK(sibling == 1 && children == 0);
    f.Think();
    CHECK(sibling == 1 && children == 128);
    CHECK(f.lua.references.empty());
}

static void TestStateIsolation() {
    Fixture first, second;
    int firstCalls = 0, secondCalls = 0;
    first.Create("same", 0, 1, [&] {
        ++firstCalls;
        first.Remove("same");
        second.Think();
    });
    second.Create("same", 0, 1, [&] { ++secondCalls; });
    first.Think();
    CHECK(firstCalls == 1 && secondCalls == 1);
    CHECK(!first.Exists("same") && !second.Exists("same"));
}

int main(int argc, char** argv) {
    const std::map<std::string, void (*)()> tests = {
        {"removal_index", TestRemovalIndex}, {"pause_resume", TestPauseResume},
        {"toggle", TestToggle}, {"adjust", TestAdjust},
        {"paused_adjust", TestPausedAdjust}, {"recreate_paused", TestRecreatePaused},
        {"infinite_repetitions", TestInfiniteRepetitions}, {"self_removal", TestSelfRemoval},
        {"remove_others", TestRemoveOthers}, {"recreate_callback", TestRecreateDuringCallback},
        {"remove_recreate", TestRemoveThenRecreate}, {"callback_growth", TestCallbackGrowth},
        {"state_isolation", TestStateIsolation},
    };
    std::vector<std::string> selected;
    for (int i = 1; i < argc; ++i) {
        CHECK(tests.count(argv[i]) == 1);
        selected.emplace_back(argv[i]);
    }
    if (selected.empty())
        for (const auto& test : tests)
            selected.push_back(test.first);
    for (const auto& name : selected) {
        TestClock::microseconds = 100000000.0;
        tests.at(name)();
        std::printf("PASS %s\n", name.c_str());
    }
    std::printf("Passed %zu systimer production regressions\n", selected.size());
}

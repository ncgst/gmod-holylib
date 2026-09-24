// Deterministic ABI model of the GMod Linux x86-64 ICvar factory iterator.
//
// The engine iterator is a local mock whose vtable uses the layout verified in
// both deployed libvstdlib.so builds:
//   slot 0 = complete destructor (D1), slot 1 = deleting destructor (D0),
//   slot 2 = SetFirst, slot 3 = Next, slot 4 = IsValid, slot 5 = Get.
//
// The baseline side models the pinned sourcesdk-minimal ICvar::Iterator inline
// methods, which call the vtable in SDK declaration order and therefore hit
// slots 0..3 (destructors included) and delete the iterator object themselves.
// The patched side exercises the real production CX64CVarIterator. No engine
// instruction is executed and no LuaJIT/GC behaviour is modelled.
#include <cstdint>
#include <cstring>
#include <cstdio>

// INSERT_PRODUCTION_ITERATOR

// Completes the production forward declaration at the same scope.
class ConCommandBase
{
public:
	explicit ConCommandBase(int nId) : nId(nId) {}
	int nId;
};

namespace
{
	constexpr int kSlotD1 = 0;
	constexpr int kSlotD0 = 1;
	constexpr int kSlotSetFirst = 2;
	constexpr int kSlotNext = 3;
	constexpr int kSlotIsValid = 4;
	constexpr int kSlotGet = 5;

	struct MockIterator
	{
		void* vptr;
		int nIndex;
		int nSetFirstCalls;
		int nNextCalls;
		int nIsValidCalls;
		int nGetCalls;
		int nD1Calls;
		int nD0Calls;
		int nFrees;
		bool bDestroyed;
		bool bUseAfterDestroy;
	};

	struct MockICvar
	{
		void* vptr;
	};

	int g_factoryCalls = 0;
	MockIterator g_iterator = {};
	MockICvar g_icvar = {};
	void* g_iteratorVTable[7] = {};
	void* g_icvarVTable[43] = {};
	constexpr int kEntryCount = 3;
	ConCommandBase g_entries[2] = { ConCommandBase(1), ConCommandBase(2) };
	void* g_slots[kEntryCount] = {};

	void ResetMock()
	{
		g_factoryCalls = 0;
		memset(&g_iterator, 0, sizeof(g_iterator));
		memset(&g_icvar, 0, sizeof(g_icvar));
		memset(g_iteratorVTable, 0, sizeof(g_iteratorVTable));
		memset(g_icvarVTable, 0, sizeof(g_icvarVTable));
		g_icvar.vptr = g_icvarVTable;
		g_iterator.vptr = g_iteratorVTable;
		g_slots[0] = &g_entries[0];
		g_slots[1] = nullptr; // The engine can report IsValid() with a null Get().
		g_slots[2] = &g_entries[1];
		g_iteratorVTable[kSlotD1] = (void*)+[](void* p) {
			((MockIterator*)p)->nD1Calls++;
			((MockIterator*)p)->bDestroyed = true;
		};
		g_iteratorVTable[kSlotD0] = (void*)+[](void* p) {
			((MockIterator*)p)->nD0Calls++;
			((MockIterator*)p)->nFrees++;
			((MockIterator*)p)->bDestroyed = true;
		};
		g_iteratorVTable[kSlotSetFirst] = (void*)+[](void* p) {
			MockIterator* it = (MockIterator*)p;
			if (it->bDestroyed) it->bUseAfterDestroy = true;
			it->nSetFirstCalls++;
			it->nIndex = 0;
			while (it->nIndex < kEntryCount && !g_slots[it->nIndex]) it->nIndex++;
		};
		g_iteratorVTable[kSlotNext] = (void*)+[](void* p) {
			MockIterator* it = (MockIterator*)p;
			if (it->bDestroyed) it->bUseAfterDestroy = true;
			it->nNextCalls++;
			// The engine advances one entry at a time and can expose a null entry
			// through Get() while IsValid() stays true.
			it->nIndex++;
		};
		g_iteratorVTable[kSlotIsValid] = (void*)+[](void* p) -> bool {
			MockIterator* it = (MockIterator*)p;
			if (it->bDestroyed) it->bUseAfterDestroy = true;
			it->nIsValidCalls++;
			return it->nIndex < kEntryCount;
		};
		g_iteratorVTable[kSlotGet] = (void*)+[](void* p) -> ConCommandBase* {
			MockIterator* it = (MockIterator*)p;
			if (it->bDestroyed) it->bUseAfterDestroy = true;
			it->nGetCalls++;
			return (ConCommandBase*)g_slots[it->nIndex];
		};
		g_icvarVTable[42] = (void*)+[](ICvar*) -> void* { ++g_factoryCalls; return &g_iterator; };
	}

	int g_checks = 0;
	int g_failures = 0;
	void Check(bool bCondition, const char* pName)
	{
		g_checks++;
		if (!bCondition)
		{
			g_failures++;
			fprintf(stderr, "FAIL: %s\n", pName);
		}
	}

	// Slot-only model of the pinned SDK's ICvar::Iterator. The engine methods have
	// different signatures than the SDK assumes, so the model dispatches the engine
	// entries that each SDK inline call would reach and asserts the resulting state.
	void TestBaselineMapping()
	{
		// SDK inline method -> engine vtable slot it actually calls:
		//   SetFirst() -> slot 0 (complete destructor)
		//   IsValid()  -> slot 2 (SetFirst)
		//   Get()      -> slot 3 (Next)
		//   Next()     -> slot 1 (deleting destructor)
		//   ~Iterator()-> delete m_pIter
		ResetMock();
		void* pD1 = g_iteratorVTable[kSlotD1];
		void* pD0 = g_iteratorVTable[kSlotD0];
		void* pSetFirst = g_iteratorVTable[kSlotSetFirst];
		void* pNext = g_iteratorVTable[kSlotNext];

		((void (*)(void*))pD1)(&g_iterator); // SDK SetFirst()
		Check(g_iterator.nD1Calls == 1 && g_iterator.bDestroyed,
			"baseline SetFirst reaches the complete destructor and destroys the iterator");

		((void (*)(void*))pSetFirst)(&g_iterator); // SDK IsValid()
		Check(g_iterator.nSetFirstCalls == 1 && g_iterator.nIsValidCalls == 0,
			"baseline IsValid reaches SetFirst instead of IsValid");

		((void (*)(void*))pNext)(&g_iterator); // SDK Get()
		Check(g_iterator.nNextCalls == 1 && g_iterator.nGetCalls == 0,
			"baseline Get reaches Next instead of Get");

		((void (*)(void*))pD0)(&g_iterator); // SDK Next()
		Check(g_iterator.nD0Calls == 1 && g_iterator.nFrees == 1,
			"baseline Next reaches the deleting destructor and frees the iterator mid-loop");

		((void (*)(void*))pSetFirst)(&g_iterator); // loop condition after Next()
		Check(g_iterator.bUseAfterDestroy, "baseline keeps using the iterator after destruction");

		g_iterator.nFrees++; // SDK ~Iterator(): delete m_pIter
		Check(g_iterator.nFrees == 2, "baseline double frees the iterator (deleting destructor plus delete)");
		Check(g_iterator.nGetCalls == 0 && g_iterator.nIsValidCalls == 0,
			"baseline never reaches the real Get or IsValid");
	}

	void TestPatchedIteration()
	{
		ResetMock();
		int nProcessed = 0;
		{
			CX64CVarIterator iter((ICvar*)&g_icvar, (void*)1);
			Check(iter.IsAvailable(), "patched iterator is available on the verified ABI");
			for (iter.SetFirst(); iter.IsValid(); iter.Next())
			{
				ConCommandBase* pCommand = iter.Get();
				if (!pCommand)
					continue; // Documented engine behaviour: IsValid() with a null entry.
				Check(pCommand == &g_entries[0] || pCommand == &g_entries[1], "patched iteration returns a real entry");
				nProcessed++;
			}
			Check(g_iterator.nFrees == 0, "iterator object is still alive during iteration");
		}
		Check(nProcessed == 2, "patched iteration visits both non-null entries");
		Check(g_iterator.nGetCalls == 3, "Get is called for every IsValid() entry");
		Check(g_iterator.nD1Calls == 0, "complete destructor is never called");
		Check(g_iterator.nD0Calls == 1 && g_iterator.nFrees == 1, "deleting destructor runs exactly once at scope exit");
		Check(!g_iterator.bUseAfterDestroy, "no use after destruction");
		Check(g_iterator.nSetFirstCalls == 1 && g_iterator.nNextCalls == 3, "SetFirst/Next call counts match the engine loop");
	}

	void TestUnavailablePaths()
	{
		ResetMock();
		{
			CX64CVarIterator iter((ICvar*)nullptr, (void*)1);
			Check(!iter.IsAvailable(), "null ICvar reports unavailable");
			iter.SetFirst();
			Check(iter.Get() == nullptr, "unavailable iterator returns null");
		}
		Check(g_iterator.nD0Calls == 0, "null ICvar does not free anything");

		ResetMock();
		g_icvarVTable[42] = (void*)+[](ICvar*) -> void* { return nullptr; };
		{
			CX64CVarIterator iter((ICvar*)&g_icvar, (void*)1);
			Check(!iter.IsAvailable(), "null factory result reports unavailable");
		}
		Check(g_iterator.nD0Calls == 0, "null factory result is not freed blindly");

		ResetMock();
		g_iteratorVTable[kSlotGet] = nullptr;
		{
			CX64CVarIterator iter((ICvar*)&g_icvar, (void*)1);
			Check(!iter.IsAvailable(), "incomplete method table reports unavailable");
		}
		Check(g_iterator.nD0Calls == 0 && g_iterator.nD1Calls == 0, "incomplete method table is never called");
		Check(g_factoryCalls == 0, "invalid iterator layout is rejected before allocation");
		ResetMock();
		g_icvarVTable[42] = nullptr;
		{
			CX64CVarIterator iter((ICvar*)&g_icvar, (void*)1);
			Check(!iter.IsAvailable(), "missing factory rejected before dispatch");
		}
		Check(g_factoryCalls == 0, "missing factory never called");
		ResetMock();
		{
			CX64CVarIterator iter((ICvar*)&g_icvar, nullptr);
			Check(!iter.IsAvailable(), "unrecognized module unavailable");
		}
		Check(g_factoryCalls == 0, "unrecognized module never calls factory");

	}
}

// The ABI dispatch test injects a layout-validation adapter. Separate ELF
// fixtures exercise the real validator, without executing engine instructions.
bool Symbols::ResolveCVarIteratorLayout(void* module, const void* vtable, void**& methods)
{
	methods = nullptr;
	if (module != (void*)1 || vtable != g_icvarVTable || !g_icvarVTable[42]) return false;
	for (size_t i = 0; i < 6; ++i)
		if (!g_iteratorVTable[i]) return false;
	methods = g_iteratorVTable;
	return true;
}

int main()
{
	TestBaselineMapping();
	TestPatchedIteration();
	TestUnavailablePaths();
	printf("cvar iterator ABI: %d checks, %d failures\n", g_checks, g_failures);
	return g_failures ? 1 : 0;
}

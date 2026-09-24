#pragma once

/*
 * Linux x86-64 ICvar factory iterator with the verified GMod engine ABI.
 *
 * The pinned sourcesdk-minimal declares ICvar::ICVarIteratorInternal without the
 * virtual destructor that the shipped engine class actually has, so the SDK's
 * inline ICvar::Iterator calls every iterator method two vtable slots early
 * (SetFirst -> complete destructor, Next -> deleting destructor, IsValid -> SetFirst,
 * Get -> Next) and its own destructor frees the iterator object a second time.
 * gameserver.cpp documents the same mismatch and already drives this ABI directly
 * for the queue sign-on guard (CX64CVarIterator260709).
 *
 * The layout resolver first checks the loaded libvstdlib build ID, CCvar and
 * iterator RTTI, exact vtables and executable method addresses. No factory or
 * iterator method is called on an unrecognized layout. Supported builds use
 * factory slot 42, iterator methods 2..5 and deleting destructor slot 1.
 * Unknown builds omit the diagnostic ConVar list until their ABI is verified.
 */
#if defined(SYSTEM_LINUX) && defined(ARCHITECTURE_X86_64)

#include <cstddef>
#include <cstdint>

class ICvar;
class ConCommandBase;

namespace Symbols
{
	bool ResolveCVarIteratorLayout(void* pModule, const void* pCVarVTable, void**& pIteratorVTable);
}

class CX64CVarIterator
{
public:
	CX64CVarIterator(ICvar* pCVar, void* pModule)
	{
		if (!pCVar)
			return;

		void** pVTable = *reinterpret_cast<void***>(pCVar);
		if (!Symbols::ResolveCVarIteratorLayout(pModule, pVTable, m_pMethods))
			return;

		m_pIterator = reinterpret_cast<void* (*)(ICvar*)>(pVTable[42])(pCVar);
		if (!m_pIterator)
			return;

		// Only the factory's verified class is accepted. If an unexpected object is
		// returned, do not dispatch through its unknown vtable, even to destroy it.
		if (*reinterpret_cast<void***>(m_pIterator) != m_pMethods)
			m_pIterator = nullptr;

	}

	~CX64CVarIterator()
	{
		if (!m_pIterator)
			return;

		void** pVTable = m_pMethods;
		reinterpret_cast<void (*)(void*)>(pVTable[1])(m_pIterator); // deleting destructor
	}

	CX64CVarIterator(const CX64CVarIterator&) = delete;
	CX64CVarIterator& operator=(const CX64CVarIterator&) = delete;

	bool IsAvailable() const { return m_pIterator != nullptr; }
	void SetFirst() { CallVoid(2); }
	void Next() { CallVoid(3); }
	bool IsValid() { return Call<bool (*)(void*), bool>(4, false); }
	ConCommandBase* Get() { return Call<ConCommandBase* (*)(void*), ConCommandBase*>(5, nullptr); }

private:
	void CallVoid(size_t nSlot)
	{
		if (!m_pIterator)
			return;

		void** pVTable = m_pMethods;
		reinterpret_cast<void (*)(void*)>(pVTable[nSlot])(m_pIterator);
	}

	template<typename TFn, typename TResult>
	TResult Call(size_t nSlot, TResult pFallback)
	{
		if (!m_pIterator)
			return pFallback;

		void** pVTable = m_pMethods;
		return reinterpret_cast<TFn>(pVTable[nSlot])(m_pIterator);
	}

	void* m_pIterator = nullptr;
	void** m_pMethods = nullptr;
};

#endif

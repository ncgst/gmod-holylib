#pragma once

#include <cstddef>
#include <cstdint>

namespace HolyLib::TeleportGuard
{
// A teleport synchronously updates spatial partitions and can call another
// trigger's Touch before the first Touch returns. Track the entity, not the
// trigger: guarding each endpoint independently would still allow A -> B -> A.
// No allocation, persistent entity references, tick delay or global busy flag.
class Scope
{
public:
	explicit Scope(std::uint32_t entityHandle)
	{
		if (s_Depth >= MaxDepth)
			return;
		for (const Scope* scope = s_Top; scope; scope = scope->m_Previous)
			if (scope->m_EntityHandle == entityHandle)
				return;

		m_EntityHandle = entityHandle;
		m_Previous = s_Top;
		m_Entered = true;
		s_Top = this;
		++s_Depth;
	}

	~Scope()
	{
		if (m_Entered)
		{
			s_Top = m_Previous;
			--s_Depth;
		}
	}

	Scope(const Scope&) = delete;
	Scope& operator=(const Scope&) = delete;
	bool Entered() const { return m_Entered; }
	static constexpr std::size_t MaxDepth = 64;

private:
	inline static thread_local const Scope* s_Top = nullptr;
	inline static thread_local std::size_t s_Depth = 0;
	const Scope* m_Previous = nullptr;
	std::uint32_t m_EntityHandle = 0;
	bool m_Entered = false;
};
}

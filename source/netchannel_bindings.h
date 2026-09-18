#pragma once

#include "Platform.hpp"
#include "sourcesdk/net_chan.h"
#include <cstddef>
#include <cstdint>

struct NetChannelBindings
{
	INetChannelHandler** handler = nullptr;
	CUtlVector<INetMessage*>* messages = nullptr;
};

// Engine-owned channels need not have the layout of HolyLib's own CNetChan.
// Linux x64 gained a fourteenth message-statistics group in September 2026:
// m_Name moved by four bytes, and pointer alignment moved these bindings by
// eight. The packet-history frame size did not change. Keep the custom channel
// implementation's layout intact and select only the two verified engine ABIs.
inline bool TryGetNetChannelBindings(INetChannel* channel, NetChannelBindings& bindings)
{
	bindings = {};
	if (!channel)
		return false;

	const uintptr_t base = reinterpret_cast<uintptr_t>(channel);
	const uintptr_t name = reinterpret_cast<uintptr_t>(channel->GetName());
	size_t shift = 0;
	if (name != base + offsetof(CNetChan, m_Name))
	{
#if defined(SYSTEM_LINUX) && defined(ARCHITECTURE_X86_64)
		static_assert(__builtin_offsetof(CNetChan, m_Name) == 0x2740, "Recheck supported CNetChan layouts");
		static_assert(__builtin_offsetof(CNetChan, m_MessageHandler) == 0x2768, "Recheck supported CNetChan layouts");
		static_assert(__builtin_offsetof(CNetChan, m_NetMessages) == 0x2770, "Recheck supported CNetChan layouts");
		if (name != base + 0x2744)
			return false;
		shift = 8;
#else
		return false;
#endif
	}

	auto** handler = reinterpret_cast<INetChannelHandler**>(base + offsetof(CNetChan, m_MessageHandler) + shift);
	if (*handler != channel->GetMsgHandler())
		return false;

	auto* messages = reinterpret_cast<CUtlVector<INetMessage*>*>(base + offsetof(CNetChan, m_NetMessages) + shift);
	const int count = messages->Count();
	// A type occupies NETMSG_TYPE_BITS, and registration rejects duplicate types.
	// Check the container before following any message pointer or modifying it.
	if (count < 0 || count > (1 << NETMSG_TYPE_BITS) ||
		messages->NumAllocated() < count || messages->NumAllocated() > 1024 ||
		(count && (!messages->Base() || reinterpret_cast<uintptr_t>(messages->Base()) % alignof(void*))))
	{
		return false;
	}

	bindings.handler = handler;
	bindings.messages = messages;
	return true;
}

#pragma once

namespace Util
{
	template <typename Client, typename Server>
	Client* FindPhysicalClientBySlot(int slot, Server* physicalServer)
	{
		if (!physicalServer || slot < 0 || slot >= physicalServer->GetClientCount())
			return nullptr;
		Client* client = static_cast<Client*>(physicalServer->GetClient(slot));
		return client && client->m_nClientSlot == slot ? client : nullptr;
	}

	// Parked clients are a separate collection whose positions are not client slots.
	template <typename Client, typename Server, typename Queue>
	Client* FindClientBySlot(int slot, Server* physicalServer, const Queue& queuedClients)
	{
		if (slot < 0)
			return nullptr;
		if (Client* client = FindPhysicalClientBySlot<Client>(slot, physicalServer))
			return client;
		for (auto* client : queuedClients)
		{
			if (client && client->m_nClientSlot == slot)
				return static_cast<Client*>(client);
		}
		return nullptr;
	}
}

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace Networking
{
// Only caches the pure CheckHeadnodeVisible(headnode, PVS, size) query. Its answer depends only on the map's BSP
// and these bytes, so results stay valid across ticks until Reset(), which must run whenever a map is activated.
// Area connectivity and all entity/recipient decisions must remain outside the cache.
// The networking module calls this from the server thread. No engine, entity or client pointers are retained.
template <std::size_t MaxPVSBytes, std::size_t MaxContexts, std::size_t NodesPerContext = 256>
class PVSCache
{
	static_assert(MaxPVSBytes > 0 && MaxContexts > 0 && NodesPerContext > 0, "Empty PVS cache");

	struct Node
	{
		int headnode = 0;
		// Zero is uncomputed; false and true are cached separately.
		unsigned char state = 0;
	};

public:
	struct Context
	{
		int size = 0;
		std::array<unsigned char, MaxPVSBytes> pvs{};
		std::array<Node, NodesPerContext> nodes{};
	};

	struct Stats
	{
		std::uint64_t contextHits = 0;
		std::uint64_t contextMisses = 0;
		std::uint64_t contextEvictions = 0;
		std::uint64_t contextBypasses = 0;
		std::uint64_t nodeHits = 0;
		std::uint64_t nodeMisses = 0;
	};

	void Reset()
	{
		m_contextCount = 0;
		m_clock = 0;
		m_stats = {};
	}

	// Returns the context holding exactly this PVS. A new PVS takes a free context, or recycles the least recently
	// used one once all are taken. The pointer stays valid until the next FindContext() or Reset() call.
	Context* FindContext(const unsigned char* pvs, int size)
	{
		if (!pvs || size <= 0 || static_cast<std::size_t>(size) > MaxPVSBytes)
		{
			++m_stats.contextBypasses;
			return nullptr;
		}

		const std::uint64_t hash = HashPVS(pvs, size);
		for (std::size_t index = 0; index < m_contextCount; ++index)
		{
			Context& context = m_contexts[index];
			// Hashes only locate candidates. A collision must never share visibility.
			if (m_hashes[index] == hash && context.size == size &&
				std::memcmp(context.pvs.data(), pvs, size) == 0)
			{
				m_lastUse[index] = ++m_clock;
				++m_stats.contextHits;
				return &context;
			}
		}

		std::size_t index = m_contextCount;
		if (index < MaxContexts)
		{
			++m_contextCount;
		}
		else
		{
			index = 0;
			for (std::size_t candidate = 1; candidate < MaxContexts; ++candidate)
			{
				if (m_lastUse[candidate] < m_lastUse[index])
					index = candidate;
			}
			++m_stats.contextEvictions;
		}

		Context& context = m_contexts[index];
		m_hashes[index] = hash;
		m_lastUse[index] = ++m_clock;
		context.size = size;
		std::memcpy(context.pvs.data(), pvs, size);
		for (Node& node : context.nodes)
			node.state = 0;
		++m_stats.contextMisses;
		return &context;
	}

	template <typename Query>
	bool CheckHeadnode(Context* context, int headnode, Query&& query)
	{
		if (!context)
			return query();

		// Direct mapping bounds memory and work even for adversarial node sets.
		// Collisions evict an answer; the complete node id is always checked.
		Node& node = context->nodes[static_cast<std::uint32_t>(headnode) % NodesPerContext];
		if (node.state != 0 && node.headnode == headnode)
		{
			++m_stats.nodeHits;
			return node.state == 2;
		}

		const bool visible = query();
		node.headnode = headnode;
		node.state = visible ? 2 : 1;
		++m_stats.nodeMisses;
		return visible;
	}

	const Stats& GetStats() const { return m_stats; }

	static std::uint64_t HashPVS(const unsigned char* pvs, int size)
	{
		std::uint64_t hash = 14695981039346656037ULL;
		// memcpy permits unaligned buffers and avoids aliasing assumptions.
		while (size >= static_cast<int>(sizeof(std::uint64_t)))
		{
			std::uint64_t word;
			std::memcpy(&word, pvs, sizeof(word));
			hash = (hash ^ word) * 1099511628211ULL;
			pvs += sizeof(word);
			size -= sizeof(word);
		}
		while (size-- > 0)
			hash = (hash ^ *pvs++) * 1099511628211ULL;
		return hash;
	}

private:
	std::size_t m_contextCount = 0;
	std::uint64_t m_clock = 0;
	// Kept outside the contexts, so lookups and evictions scan small arrays instead of the large contexts.
	std::array<std::uint64_t, MaxContexts> m_hashes{};
	std::array<std::uint64_t, MaxContexts> m_lastUse{};
	std::array<Context, MaxContexts> m_contexts{};
	Stats m_stats;
};
}

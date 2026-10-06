#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace Networking
{
// Only caches the pure CheckHeadnodeVisible(headnode, PVS, size) query. Area
// connectivity and all entity/recipient decisions must remain outside the cache.
// The networking module calls this from the server thread, once visibility has
// been built for a recipient. No engine, entity or client pointers are retained.
template <std::size_t MaxPVSBytes, std::size_t MaxContexts, std::size_t NodesPerContext = 256>
class PVSCache
{
	static_assert(MaxPVSBytes > 0 && MaxContexts > 0 && NodesPerContext > 0, "Empty PVS cache");
	static constexpr std::size_t BucketCount = MaxContexts * 2;

	struct Node
	{
		int headnode = 0;
		// Zero is uncomputed; false and true are cached separately.
		unsigned char state = 0;
	};

public:
	struct Context
	{
		std::uint64_t hash = 0;
		int size = 0;
		std::array<unsigned char, MaxPVSBytes> pvs{};
		std::array<Node, NodesPerContext> nodes{};
	};

	struct Stats
	{
		std::uint64_t contextHits = 0;
		std::uint64_t contextMisses = 0;
		std::uint64_t contextBypasses = 0;
		std::uint64_t nodeHits = 0;
		std::uint64_t nodeMisses = 0;
	};

	void Reset()
	{
		m_hasTick = false;
		m_contextCount = 0;
		m_buckets.fill(0);
		m_stats = {};
	}

	void BeginTick(int tick)
	{
		if (m_hasTick && m_tick == tick)
			return;

		m_hasTick = true;
		m_tick = tick;
		m_contextCount = 0;
		m_buckets.fill(0);
	}

	// Call once per recipient, after SetupVisibility/PreCheckTransmit. The PVS
	// remains fixed throughout that recipient's CheckTransmit invocation.
	Context* FindContext(const unsigned char* pvs, int size)
	{
		if (!m_hasTick || !pvs || size <= 0 || static_cast<std::size_t>(size) > MaxPVSBytes)
		{
			++m_stats.contextBypasses;
			return nullptr;
		}

		const auto hash = HashPVS(pvs, size);
		std::size_t bucket = hash % BucketCount;
		for (std::size_t probe = 0; probe < BucketCount; ++probe)
		{
			const std::size_t index = m_buckets[bucket];
			if (index == 0)
			{
				if (m_contextCount == MaxContexts)
					break;

				Context& context = m_contexts[m_contextCount];
				context.hash = hash;
				context.size = size;
				std::memcpy(context.pvs.data(), pvs, size);
				for (Node& node : context.nodes)
					node.state = 0;
				m_buckets[bucket] = ++m_contextCount;
				++m_stats.contextMisses;
				return &context;
			}

			Context& context = m_contexts[index - 1];
			// Hashes only locate candidates. A collision must never share visibility.
			if (context.hash == hash && context.size == size &&
				std::memcmp(context.pvs.data(), pvs, size) == 0)
			{
				++m_stats.contextHits;
				return &context;
			}
			bucket = (bucket + 1) % BucketCount;
		}

		++m_stats.contextBypasses;
		return nullptr;
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

private:
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

	bool m_hasTick = false;
	int m_tick = 0;
	std::size_t m_contextCount = 0;
	std::array<std::size_t, BucketCount> m_buckets{};
	std::array<Context, MaxContexts> m_contexts{};
	Stats m_stats;
};
}

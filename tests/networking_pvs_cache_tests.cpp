#include "networking_pvs_cache.h"
#include <cassert>
#include <climits>
#include <iostream>

using Cache = Networking::PVSCache<8192, 128>;
static Cache cache; // Production-sized storage must not consume the test stack.

int main()
{
	std::array<unsigned char, 8192> pvs{};
	assert(cache.FindContext(pvs.data(), 8192) == nullptr);
	cache.BeginTick(0);
	assert(cache.FindContext(nullptr, 1) == nullptr);
	assert(cache.FindContext(pvs.data(), 0) == nullptr);
	assert(cache.FindContext(pvs.data(), -1) == nullptr);
	assert(cache.FindContext(pvs.data(), 8193) == nullptr);
	auto* zero = cache.FindContext(pvs.data(), 8192);
	assert(zero); // An all-zero PVS is data, not an uninitialized cache entry.
	int queries = 0;
	for (int n = 0; n < 120; ++n)
		assert(!cache.CheckHeadnode(zero, 0, [&] { ++queries; return false; }));
	assert(queries == 1); // Negative results are shared too.
	assert(cache.CheckHeadnode(zero, 256, [&] { ++queries; return true; }));
	assert(!cache.CheckHeadnode(zero, 0, [&] { ++queries; return false; }));
	assert(queries == 3); // Direct-map collisions cannot return another node's result.
	assert(cache.CheckHeadnode(zero, INT_MIN, [] { return true; }));
	assert(!cache.CheckHeadnode(zero, INT_MAX, [] { return false; }));

	pvs[8191] = 1;
	auto* changed = cache.FindContext(pvs.data(), 8192);
	assert(changed && changed != zero);
	pvs[8191] = 0;
	assert(cache.FindContext(pvs.data(), 8192) == zero); // Owns a copy, not the caller's buffer.
	assert(cache.FindContext(pvs.data(), 8191) != zero);
	cache.BeginTick(0);
	assert(cache.FindContext(pvs.data(), 8192) == zero);
	cache.BeginTick(1);
	zero = cache.FindContext(pvs.data(), 8192);
	assert(cache.CheckHeadnode(zero, 0, [] { return true; }));
	cache.BeginTick(-1); // Tick rewinds cannot revive results.
	zero = cache.FindContext(pvs.data(), 8192);
	assert(!cache.CheckHeadnode(zero, 0, [] { return false; }));
	cache.Reset(); // Map activation can reuse the same tick number.
	assert(cache.GetStats().nodeHits == 0);
	cache.BeginTick(-1);
	zero = cache.FindContext(pvs.data(), 8192);
	assert(cache.CheckHeadnode(zero, 0, [] { return true; }));

	Networking::PVSCache<16, 2, 1> small;
	small.BeginTick(10);
	unsigned char first[] = {0}, second[] = {1}, third[] = {2};
	auto* firstContext = small.FindContext(first, 1);
	auto* secondContext = small.FindContext(second, 1);
	assert(firstContext && secondContext && firstContext != secondContext);
	assert(!small.FindContext(third, 1));
	assert(small.FindContext(first, 1) == firstContext); // Full caches still find existing keys.
	assert(small.CheckHeadnode(nullptr, 7, [] { return true; }));
	assert(!small.CheckHeadnode(nullptr, 7, [] { return false; }));

	// Construct two distinct 16-byte inputs with the same word-wise hash. The
	// exact comparison, not probabilistic hash uniqueness, protects recipients.
	std::array<std::uint64_t, 2> a{0, 0}, b{1, 0};
	constexpr std::uint64_t seed = 14695981039346656037ULL, prime = 1099511628211ULL;
	b[1] = seed * prime ^ (seed ^ b[0]) * prime;
	small.Reset();
	small.BeginTick(11);
	auto* aContext = small.FindContext(reinterpret_cast<const unsigned char*>(a.data()), 16);
	auto* bContext = small.FindContext(reinterpret_cast<const unsigned char*>(b.data()), 16);
	assert(aContext && bContext && aContext != bContext && aContext->hash == bContext->hash);
	assert(small.CheckHeadnode(aContext, 7, [] { return true; }));
	assert(!small.CheckHeadnode(bContext, 7, [] { return false; }));

	cache.Reset();
	cache.BeginTick(100);
	queries = 0;
	for (int recipient = 0; recipient < 120; ++recipient)
	{
		auto* context = cache.FindContext(pvs.data(), 8192);
		for (int headnode = 0; headnode < 64; ++headnode)
		{
			const bool visible = cache.CheckHeadnode(context, headnode, [&] {
				++queries;
				return headnode % 3 == 0;
			});
			assert(visible == (headnode % 3 == 0));
		}
	}
	assert(queries == 64);
	assert(cache.GetStats().contextHits == 119 && cache.GetStats().contextMisses == 1);
	assert(cache.GetStats().nodeHits == 120 * 64 - 64);

	// 120 different visibility sets must remain independent, including when
	// their headnode queries disagree and the direct-map entries collide.
	cache.BeginTick(101);
	for (int recipient = 0; recipient < 120; ++recipient)
	{
		pvs[0] = static_cast<unsigned char>(recipient);
		auto* context = cache.FindContext(pvs.data(), 8192);
		assert(context);
		for (int node : {0, 256, 0, 511, 255})
			assert(cache.CheckHeadnode(context, node, [&] { return (recipient + node) % 2 == 0; }) ==
				((recipient + node) % 2 == 0));
	}
	std::cout << "PVS cache: exact keys, forced hash collisions, limits, resets and 120 recipients passed\n"
		<< "Synthetic shared-PVS workload: 7680 headnode requests, 64 oracle calls\n";
}

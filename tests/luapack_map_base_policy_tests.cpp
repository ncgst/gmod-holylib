#include "modules/gmoddatapack_luapack_policy.h"

#include <cassert>
#include <array>
#include <iostream>
#include <string>
#include <unordered_map>
#include <unordered_set>

using HolyLib::LuaPack::Policy::Action;
using HolyLib::LuaPack::Policy::BaseAvailability;
using HolyLib::LuaPack::Policy::Lane;

int main()
{
	using namespace HolyLib::LuaPack::Policy;

	// Required base-plus-delta: unchanged files are canonical, current deltas are native.
	assert(SelectBaseline(Lane::Required, true, BaseAvailability::Ready) == Action::CanonicalStub);
	assert(SelectFile(Lane::Required, BaseAvailability::Ready, false, false) == Action::CanonicalStub);
	assert(SelectFile(Lane::Required, BaseAvailability::Ready, false, true) == Action::Native);
	assert(SelectFile(Lane::Required, BaseAvailability::Ready, true, false) == Action::Native);
	assert(ShouldBuildMapBase(false, true));
	assert(!ShouldBuildMapBase(true, true));
	assert(!ShouldBuildMapBase(true, false));

	// A changed path becomes native and exact restoration returns it to the canonical base.
	const std::string baseIdentity = "base-sha256";
	assert(IsNativeDelta(true, baseIdentity, "hotfix-sha256"));
	assert(!IsNativeDelta(true, baseIdentity, baseIdentity));
	assert(IsNativeDelta(false, "", "late-registration-sha256"));

	// A JIP delta hash is already present in its mixed server-info baseline. Do not
	// send the same hash again before the body (that can stall signon), but do send
	// after a global canonical update or when a later hotfix changes the identity.
	using TestHash = std::array<unsigned char, 4>;
	const TestHash hotfixOne{1, 2, 3, 4};
	const TestHash hotfixTwo{4, 3, 2, 1};
	std::unordered_map<int, TestHash> clientNativeHashes;
	RememberNativeHash(clientNativeHashes, 42, hotfixOne);
	assert(NativeHashMatches(clientNativeHashes, 42, hotfixOne));
	assert(!NativeHashMatches(clientNativeHashes, 42, hotfixTwo));
	assert(RestoreCanonicalHash(clientNativeHashes, 42));
	assert(!NativeHashMatches(clientNativeHashes, 42, hotfixOne));
	assert(!RestoreCanonicalHash(clientNativeHashes, 42));
	RememberNativeHash(clientNativeHashes, 42, hotfixTwo);
	assert(NativeHashMatches(clientNativeHashes, 42, hotfixTwo));

	// Exact-key duplicates are rejected by the same registry used by pack validation.
	std::unordered_set<std::string> exactKeys;
	assert(RegisterExactKey(exactKeys, "0123456789abcdef"));
	assert(RegisterExactKey(exactKeys, "fedcba9876543210"));
	assert(!RegisterExactKey(exactKeys, "0123456789abcdef"));

	// Opt-out is exact and remains native for the whole connection.
	assert(ResolveLane(true, true, "no_gluapack") == Lane::NativeOptOut);
	assert(ResolveLane(true, true, "NO_GLUAPACK") == Lane::Required);
	assert(ResolveLane(true, true, "no_gluapack ") == Lane::Required);
	assert(ResolveLane(true, false, "no_gluapack") == Lane::Required);
	assert(SelectBaseline(Lane::NativeOptOut, false, BaseAvailability::Missing) == Action::Native);
	assert(SelectFile(Lane::NativeOptOut, BaseAvailability::Missing, false, true) == Action::Native);

	// Required mode fails closed when the base is absent, invalid, or cannot be canonicalized.
	assert(SelectBaseline(Lane::Required, true, BaseAvailability::Missing) == Action::Reject);
	assert(SelectBaseline(Lane::Required, true, BaseAvailability::Unusable) == Action::Reject);
	assert(SelectBaseline(Lane::Required, false, BaseAvailability::Ready) == Action::Reject);
	assert(SelectFile(Lane::Required, BaseAvailability::Missing, false, true) == Action::Reject);

	std::cout << "luapack map-base policy tests passed\n";
	return 0;
}

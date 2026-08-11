#pragma once

#include <cstring>
#include <string>
#include <unordered_set>

namespace HolyLib::LuaPack::Policy
{
	enum class Lane
	{
		NativeRescue,
		NativeOptOut,
		Required,
	};

	enum class BaseAvailability
	{
		Missing,
		Unusable,
		Ready,
	};

	enum class Action
	{
		Native,
		CanonicalStub,
		Reject,
	};

	inline Lane ResolveLane(bool required, bool allowOptOut, const char* tvNoChat)
	{
		const bool optedOut = allowOptOut && tvNoChat && std::strcmp(tvNoChat, "no_gluapack") == 0;
		return optedOut ? Lane::NativeOptOut : (required ? Lane::Required : Lane::NativeRescue);
	}

	constexpr Action SelectBaseline(Lane lane, bool canonicalRegistration,
		BaseAvailability base)
	{
		return lane != Lane::Required ? Action::Native :
			(!canonicalRegistration || base != BaseAvailability::Ready ? Action::Reject : Action::CanonicalStub);
	}

	constexpr Action SelectFile(Lane lane, BaseAvailability base, bool bootstrap,
		bool nativeDelta)
	{
		return lane != Lane::Required ? Action::Native :
			(base != BaseAvailability::Ready ? Action::Reject :
				(bootstrap || nativeDelta ? Action::Native : Action::CanonicalStub));
	}

	constexpr bool ShouldBuildMapBase(bool hasMapBase, bool buildRequested)
	{
		return !hasMapBase && buildRequested;
	}

	inline bool IsNativeDelta(bool baseContainsPath, const std::string& baseIdentity,
		const std::string& currentIdentity)
	{
		return !baseContainsPath || baseIdentity != currentIdentity;
	}

	inline bool RegisterExactKey(std::unordered_set<std::string>& keys, const std::string& key)
	{
		return keys.insert(key).second;
	}

	static_assert(SelectBaseline(Lane::Required, true, BaseAvailability::Ready) == Action::CanonicalStub,
		"a usable required base must select mixed canonical/native delivery");
	static_assert(SelectBaseline(Lane::Required, true, BaseAvailability::Missing) == Action::Reject,
		"a missing required base must fail closed");
	static_assert(SelectBaseline(Lane::Required, true, BaseAvailability::Unusable) == Action::Reject,
		"an unusable required base must fail closed");
	static_assert(SelectBaseline(Lane::NativeOptOut, true, BaseAvailability::Ready) == Action::Native,
		"the explicit opt-out lane must remain wholly native");
	static_assert(SelectFile(Lane::Required, BaseAvailability::Ready, false, false) == Action::CanonicalStub,
		"an unchanged base file must use the canonical stub");
	static_assert(SelectFile(Lane::Required, BaseAvailability::Ready, false, true) == Action::Native,
		"a file changed from the base must use native delivery");
	static_assert(SelectFile(Lane::Required, BaseAvailability::Ready, true, false) == Action::Native,
		"the bootstrap must always use native delivery");
	static_assert(SelectFile(Lane::Required, BaseAvailability::Missing, false, true) == Action::Reject,
		"a native delta must never turn a missing required base into whole-join fallback");
	static_assert(ShouldBuildMapBase(false, true), "the initial map base must be buildable");
	static_assert(!ShouldBuildMapBase(true, true), "hotfixes must not rotate the immutable map base");
}

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
	constexpr std::uint64_t accountA = 101ULL;
	constexpr std::uint64_t accountB = 202ULL;

	// An authenticated required failure arms only the next connection. The failure
	// connection cannot consume its own latch, and consumption is wholly native.
	{
		RequiredRecoveryTracker recovery;
		assert(recovery.Arm(accountA, 100, 10.0, 30.0) == RecoveryArmResult::Armed);
		assert(recovery.Consume(accountA, 100, 11.0) == RecoveryConsumeResult::SameConnection);
		assert(recovery.Consume(accountA, 101, 12.0) == RecoveryConsumeResult::Native);
		assert(SelectBaseline(Lane::NativeRecovery, true, BaseAvailability::Ready) == Action::Native);
		assert(SelectFile(Lane::NativeRecovery, BaseAvailability::Missing, false, false) == Action::Native);
		assert(recovery.Consume(accountA, 102, 13.0) == RecoveryConsumeResult::RetryExhausted);
	}

	// Same-process native restoration is independent of the stale canonical cache
	// object. Every native hash is remembered from the initial baseline, so the body
	// request does not receive a duplicate pre-body hash update.
	{
		const bool staleCanonicalCacheObjectPresent = true;
		assert(staleCanonicalCacheObjectPresent);
		assert(NeedsPerClientNativeHashes(Lane::NativeRecovery));
		std::unordered_map<int, std::array<unsigned char, 4>> recoveryHashes;
		const std::array<unsigned char, 4> nativeBody{9, 8, 7, 6};
		RememberNativeHash(recoveryHashes, 73, nativeBody);
		assert(NativeHashMatches(recoveryHashes, 73, nativeBody));
		assert(SelectBaseline(Lane::NativeRecovery, true, BaseAvailability::Ready) == Action::Native);
	}

	// The ticket identity must resolve before a recoverable required baseline, while
	// full authentication may complete later in signon. Arming still requires both,
	// and a reused slot cannot consume account A's latch for account B.
	{
		assert(!RequiredBaselineIdentityReady(true, false));
		assert(RequiredBaselineIdentityReady(true, true));
		assert(RequiredBaselineIdentityReady(false, false));
		assert(!RequiredFailureIdentityReady(true, true, false));
		assert(RequiredFailureIdentityReady(true, true, true));
		assert(!RequiredFailureIdentityReady(true, false, true));
		RequiredRecoveryTracker recovery;
		assert(recovery.Arm(0, 200, 0.0, 30.0) == RecoveryArmResult::Invalid);
		assert(recovery.Arm(accountA, 200, 0.0, 30.0) == RecoveryArmResult::Armed);
		assert(recovery.Consume(accountB, 201, 1.0) == RecoveryConsumeResult::None);
		assert(recovery.Consume(accountA, 202, 1.0) == RecoveryConsumeResult::Native);
	}

	// Armed state expires at the TTL boundary. Once consumed, it becomes a tombstone
	// until a proven successful lifecycle boundary or server reset prevents a loop.
	{
		RequiredRecoveryTracker recovery;
		assert(recovery.Arm(accountA, 300, 50.0, 5.0) == RecoveryArmResult::Armed);
		assert(recovery.Consume(accountA, 301, 55.0) == RecoveryConsumeResult::Expired);
		assert(recovery.Size() == 0);
		assert(recovery.Arm(accountA, 302, 60.0, 5.0) == RecoveryArmResult::Armed);
		assert(recovery.Consume(accountA, 303, 61.0) == RecoveryConsumeResult::Native);
		recovery.Prune(1000.0);
		assert(recovery.Size() == 1);
		assert(!recovery.Complete(accountA, 304));
		assert(recovery.Complete(accountA, 303));
		assert(recovery.Size() == 0);
	}

	// A successful recovery clears the account for a future independent incident.
	{
		RequiredRecoveryTracker recovery;
		assert(recovery.Arm(accountA, 400, 0.0, 30.0) == RecoveryArmResult::Armed);
		assert(recovery.Consume(accountA, 401, 1.0) == RecoveryConsumeResult::Native);
		assert(recovery.Complete(accountA, 401));
		assert(recovery.Arm(accountA, 402, 2.0, 30.0) == RecoveryArmResult::Armed);
	}

	// A failed native recovery cannot re-arm or trigger an automatic reconnect loop.
	// The process-local client guard mirrors ShouldArmAutomaticRetry.
	{
		RequiredRecoveryTracker recovery;
		assert(ShouldArmAutomaticRetry(true, false));
		assert(!ShouldArmAutomaticRetry(true, true));
		assert(!ShouldArmAutomaticRetry(false, false));
		assert(ShouldEnforceReadyDeadline(false));
		assert(!ShouldEnforceReadyDeadline(true));
		assert(recovery.Arm(accountA, 500, 0.0, 30.0) == RecoveryArmResult::Armed);
		assert(recovery.Consume(accountA, 501, 1.0) == RecoveryConsumeResult::Native);
		assert(recovery.Arm(accountA, 502, 2.0, 30.0) == RecoveryArmResult::RetryExhausted);
		assert(recovery.Consume(accountA, 503, 3.0) == RecoveryConsumeResult::RetryExhausted);
	}

	// Server restart/level shutdown is an explicit boundary for all transient state.
	{
		RequiredRecoveryTracker recovery;
		assert(recovery.Arm(accountA, 600, 0.0, 30.0) == RecoveryArmResult::Armed);
		recovery.Reset();
		assert(recovery.Size() == 0);
		assert(recovery.Consume(accountA, 601, 1.0) == RecoveryConsumeResult::None);
	}

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

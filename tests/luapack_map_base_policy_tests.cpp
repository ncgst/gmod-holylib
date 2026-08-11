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
		RequiredRecoveryHandoff handoff;
		assert(recovery.Arm(accountA, 100, 10.0, 30.0) == RecoveryArmResult::Armed);
		assert(handoff.Queue(accountA, 100, 10.0, 15.0));
		assert(!handoff.Queue(accountA, 100, 10.0, 15.0));
		assert(handoff.TryDispatchServerInfo(accountA, true, true, true, 10.5) ==
			RecoveryHandoffResult::Invalid);
		// Queuing and invoking the engine primitive do not consume the latch or change
		// the failed connection's lane. Consumption happens only at new ServerInfo.
		assert(recovery.Consume(accountA, 100, 11.0) == RecoveryConsumeResult::SameConnection);
		assert(handoff.BeginBaseline(accountA, true, 11.0) == RecoveryHandoffResult::Invalid);
		assert(handoff.TryInvoke(accountA, 100, true, true, 11.0) == RecoveryHandoffResult::Invoke);
		assert(handoff.TryInvoke(accountA, 100, true, true, 11.5) == RecoveryHandoffResult::AlreadyInvoked);
		assert(handoff.TryDispatchServerInfo(accountA, true, true, false, 11.6) ==
			RecoveryHandoffResult::NotReady);
		assert(handoff.TryDispatchServerInfo(0, false, false, true, 11.7) ==
			RecoveryHandoffResult::NotReady);
		assert(handoff.TryDispatchServerInfo(accountB, true, true, true, 11.8) ==
			RecoveryHandoffResult::NotReady);
		// Source may clear the old slot or assign a new connection serial before
		// ServerInfo. Once invoked, neither transition discards or reinvokes the handoff.
		assert(handoff.TryInvoke(0, 0, false, false, 11.75) == RecoveryHandoffResult::AlreadyInvoked);
		assert(handoff.TryInvoke(accountB, 999, true, true, 11.9) == RecoveryHandoffResult::AlreadyInvoked);
		assert(handoff.TryDispatchServerInfo(accountA, true, true, true, 11.95) ==
			RecoveryHandoffResult::DispatchServerInfo);
		assert(handoff.TryDispatchServerInfo(accountA, true, true, true, 11.96) ==
			RecoveryHandoffResult::AlreadyInvoked);
		assert(handoff.BeginBaseline(accountA, true, 12.0) == RecoveryHandoffResult::BeginBaseline);
		assert(!handoff.Pending());
		assert(recovery.Consume(accountA, 101, 12.0) == RecoveryConsumeResult::Native);
		assert(recovery.OwnsConsumed(accountA, 101));
		assert(!recovery.OwnsConsumed(accountA, 100));
		assert(SelectBaseline(Lane::NativeRecovery, true, BaseAvailability::Ready) == Action::Native);
		assert(SelectFile(Lane::NativeRecovery, BaseAvailability::Missing, false, false) == Action::Native);
		assert(recovery.Consume(accountA, 102, 13.0) == RecoveryConsumeResult::RetryExhausted);
	}

	// CGameClient::Reconnect can begin the native ServerInfo baseline before the old
	// game-layer disconnect/connect callbacks finish. Those late callbacks and commands
	// must preserve the consumed native epoch; the lower-level physical disconnect still
	// wins, and no different account/connection can inherit the fence.
	{
		RequiredRecoveryTracker recovery;
		assert(recovery.Arm(accountA, 110, 0.0, 30.0) == RecoveryArmResult::Armed);
		assert(recovery.Consume(accountA, 111, 1.0) == RecoveryConsumeResult::Native);
		const bool ownsRecovery = recovery.OwnsConsumed(accountA, 111);
		assert(ShouldPreserveRecoveryLifecycle(true, true, false, false, true, ownsRecovery));
		assert(!ShouldPreserveRecoveryLifecycle(false, true, false, false, true, ownsRecovery));
		assert(!ShouldPreserveRecoveryLifecycle(true, true, true, false, true, ownsRecovery));
		assert(!ShouldPreserveRecoveryLifecycle(true, true, false, true, true, ownsRecovery));
		assert(!ShouldPreserveRecoveryLifecycle(true, true, false, false, false, ownsRecovery));
		assert(!ShouldPreserveRecoveryLifecycle(true, true, false, false, true,
			recovery.OwnsConsumed(accountB, 111)));
		assert(ShouldKeepInvokedRecoveryHandoff(true, true));
		assert(!ShouldKeepInvokedRecoveryHandoff(false, true));
		assert(!ShouldKeepInvokedRecoveryHandoff(true, false));
		assert(ShouldIgnoreLateRecoveryFailure(true, ownsRecovery, false));
		assert(!ShouldIgnoreLateRecoveryFailure(false, ownsRecovery, false));
		assert(!ShouldIgnoreLateRecoveryFailure(true,
			recovery.OwnsConsumed(accountA, 112), false));
		assert(ShouldIgnoreLateRecoveryFailure(true, false, true));
		RequiredRecoveryHandoff physicalHandoff;
		assert(physicalHandoff.Queue(accountA, 112, 2.0, 15.0));
		assert(!physicalHandoff.ResetIfOwnedBy(accountB));
		assert(physicalHandoff.Pending());
		assert(physicalHandoff.ResetIfOwnedBy(accountA));
		assert(!physicalHandoff.Pending());
		// Physical disconnect removes only the reconnect handoff. The consumed account
		// tombstone survives so another failed join cannot schedule a retry loop.
		assert(recovery.OwnsConsumed(accountA, 111));
	}

	// A stale success callback from the connection that armed recovery cannot clear
	// its own latch. A distinct authenticated success remains a safe clear boundary.
	{
		RequiredRecoveryTracker recovery;
		assert(recovery.Arm(accountA, 125, 0.0, 30.0) == RecoveryArmResult::Armed);
		assert(recovery.ClearAfterSuccessfulConnection(accountA, 125) ==
			RecoveryClearResult::SameFailedConnection);
		assert(recovery.Size() == 1);
		assert(recovery.ClearAfterSuccessfulConnection(accountA, 126) ==
			RecoveryClearResult::Cleared);
		assert(recovery.Size() == 0);
	}

	// A physical connection can supersede an invoked slot handoff without losing the
	// Armed account latch. Its distinct connection epoch consumes the one-shot native lane.
	{
		RequiredRecoveryTracker recovery;
		RequiredRecoveryHandoff handoff;
		assert(recovery.Arm(accountA, 150, 0.0, 30.0) == RecoveryArmResult::Armed);
		assert(handoff.Queue(accountA, 150, 0.0, 15.0));
		assert(handoff.TryInvoke(accountA, 150, true, true, 1.0) == RecoveryHandoffResult::Invoke);
		assert(handoff.ResetIfOwnedBy(accountA));
		assert(!handoff.Pending());
		assert(recovery.Consume(accountA, 151, 2.0) == RecoveryConsumeResult::Native);
		assert(recovery.Complete(accountA, 151));
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
		RequiredRecoveryHandoff handoff;
		assert(recovery.Arm(0, 200, 0.0, 30.0) == RecoveryArmResult::Invalid);
		assert(recovery.Arm(accountA, 200, 0.0, 30.0) == RecoveryArmResult::Armed);
		assert(handoff.Queue(accountA, 200, 0.0, 10.0));
		assert(handoff.TryInvoke(accountA, 200, false, true, 1.0) == RecoveryHandoffResult::Invalid);
		assert(handoff.TryInvoke(accountB, 200, true, true, 1.0) == RecoveryHandoffResult::OwnershipMismatch);
		assert(handoff.TryInvoke(accountA, 201, true, true, 1.0) == RecoveryHandoffResult::OwnershipMismatch);
		assert(handoff.TryInvoke(accountA, 200, true, true, 1.0) == RecoveryHandoffResult::Invoke);
		assert(handoff.BeginBaseline(accountB, true, 2.0) == RecoveryHandoffResult::OwnershipMismatch);
		assert(handoff.BeginBaseline(accountA, true, 2.0) == RecoveryHandoffResult::BeginBaseline);
		assert(recovery.Consume(accountB, 201, 1.0) == RecoveryConsumeResult::None);
		assert(recovery.Consume(accountA, 202, 1.0) == RecoveryConsumeResult::Native);
	}

	// Armed state expires at the TTL boundary. Once consumed, it becomes a tombstone
	// until a proven successful lifecycle boundary or server reset prevents a loop.
	{
		RequiredRecoveryTracker recovery;
		RequiredRecoveryHandoff handoff;
		assert(recovery.Arm(accountA, 300, 50.0, 5.0) == RecoveryArmResult::Armed);
		assert(handoff.Queue(accountA, 300, 50.0, 2.5));
		assert(handoff.TryInvoke(accountA, 300, true, true, 52.5) == RecoveryHandoffResult::Expired);
		assert(!handoff.Pending());
		assert(recovery.Consume(accountA, 301, 55.0) == RecoveryConsumeResult::Expired);
		assert(recovery.Size() == 0);
		assert(handoff.Queue(accountA, 301, 60.0, 2.0));
		assert(handoff.TryInvoke(accountA, 301, true, true, 60.5) == RecoveryHandoffResult::Invoke);
		assert(handoff.TryDispatchServerInfo(accountA, true, true, true, 62.0) ==
			RecoveryHandoffResult::Expired);
		assert(!handoff.Pending());
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
	// The per-slot handoff invokes the engine primitive once, while the consumed
	// account tombstone prevents another failure from scheduling a second attempt.
	{
		RequiredRecoveryTracker recovery;
		RequiredRecoveryHandoff handoff;
		assert(RecoveryHandoffWindow(120.0) == 30.0);
		assert(RecoveryHandoffWindow(40.0) == 20.0);
		assert(RecoveryHandoffWindow(5.0) == 2.5);
		assert(RecoveryHandoffWindow(0.0) == 0.0);
		assert(recovery.Arm(accountA, 500, 0.0, 30.0) == RecoveryArmResult::Armed);
		assert(handoff.Queue(accountA, 500, 0.0, 15.0));
		assert(handoff.TryInvoke(accountA, 500, true, true, 1.0) == RecoveryHandoffResult::Invoke);
		assert(handoff.TryInvoke(accountA, 500, true, true, 2.0) == RecoveryHandoffResult::AlreadyInvoked);
		assert(handoff.TryDispatchServerInfo(accountA, true, true, true, 2.0) ==
			RecoveryHandoffResult::DispatchServerInfo);
		assert(handoff.TryDispatchServerInfo(accountA, true, true, true, 2.1) ==
			RecoveryHandoffResult::AlreadyInvoked);
		assert(handoff.BeginBaseline(accountA, true, 2.0) == RecoveryHandoffResult::BeginBaseline);
		assert(recovery.Consume(accountA, 501, 1.0) == RecoveryConsumeResult::Native);
		assert(recovery.Arm(accountA, 502, 2.0, 30.0) == RecoveryArmResult::RetryExhausted);
		assert(recovery.Consume(accountA, 503, 3.0) == RecoveryConsumeResult::RetryExhausted);
	}

	// Server restart/level shutdown is an explicit boundary for all transient state.
	{
		RequiredRecoveryTracker recovery;
		RequiredRecoveryHandoff handoff;
		assert(recovery.Arm(accountA, 600, 0.0, 30.0) == RecoveryArmResult::Armed);
		assert(handoff.Queue(accountA, 600, 0.0, 15.0));
		recovery.Reset();
		handoff.Reset();
		assert(recovery.Size() == 0);
		assert(!handoff.Pending());
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

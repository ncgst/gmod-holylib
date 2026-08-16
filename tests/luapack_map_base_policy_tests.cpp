#include "modules/gmoddatapack_luapack_policy.h"

#include <cassert>
#include <array>
#include <iostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using HolyLib::LuaPack::Policy::Action;
using HolyLib::LuaPack::Policy::BaseAvailability;
using HolyLib::LuaPack::Policy::Lane;

class TestBitWriter
{
public:
	explicit TestBitWriter(std::size_t maximumBits) : maximumBits(maximumBits) {}

	int GetNumBitsWritten() const { return static_cast<int>(bits.size()); }
	int GetNumBitsLeft() const
	{
		return maximumBits >= bits.size()
			? static_cast<int>(maximumBits - bits.size()) : 0;
	}
	bool IsOverflowed() const { return overflowed; }

	void WriteUBitLong(std::uint32_t value, int bitCount)
	{
		for (int bit = 0; bit < bitCount; ++bit)
			WriteBit((value >> bit) & 1u);
	}

	void WriteBytes(const void* data, int byteCount)
	{
		const auto* bytes = static_cast<const unsigned char*>(data);
		for (int byte = 0; byte < byteCount; ++byte)
			WriteUBitLong(bytes[byte], 8);
	}

	std::uint32_t ReadUBitLong(std::size_t start, int bitCount) const
	{
		std::uint32_t value = 0;
		for (int bit = 0; bit < bitCount; ++bit)
			value |= static_cast<std::uint32_t>(bits[start + bit]) << bit;
		return value;
	}

	std::vector<bool> bits;

private:
	void WriteBit(bool value)
	{
		if (bits.size() >= maximumBits)
		{
			overflowed = true;
			return;
		}
		bits.push_back(value);
	}

	std::size_t maximumBits;
	bool overflowed = false;
};

class TestBitReader
{
public:
	explicit TestBitReader(std::vector<std::uint16_t> values,
		int availableBits = -1) : values(std::move(values)),
		availableBits(availableBits >= 0 ? availableBits :
			static_cast<int>(this->values.size() * 16)) {}

	int GetNumBitsLeft() const { return availableBits - bitsRead; }
	bool IsOverflowed() const { return overflowed; }

	std::uint32_t ReadUBitLong(int bitCount)
	{
		if (bitCount != 16 || bitsRead + bitCount > availableBits ||
			position >= values.size())
		{
			overflowed = true;
			return 0;
		}
		bitsRead += bitCount;
		return values[position++];
	}

private:
	std::vector<std::uint16_t> values;
	std::size_t position = 0;
	int availableBits = 0;
	int bitsRead = 0;
	bool overflowed = false;
};

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
		assert(SelectFile(Lane::NativeRecovery, true, BaseAvailability::Missing, false, false) == Action::Native);
		assert(SelectFile(Lane::NativeRecovery, false, BaseAvailability::Missing, false, false) == Action::Native);
		assert(recovery.Consume(accountA, 102, 13.0) == RecoveryConsumeResult::RetryExhausted);
	}

	// A late authenticated reconnect gets one full but finite scheduler window after
	// its exact ServerInfo request is claimed; it cannot become an immortal slot token.
	{
		RequiredRecoveryHandoff delayed;
		assert(delayed.Queue(accountA, 103, 0.0, 15.0));
		assert(delayed.TryInvoke(accountA, 103, true, true, 1.0) ==
			RecoveryHandoffResult::Invoke);
		assert(delayed.TryDispatchServerInfo(accountA, true, true, true, 14.0) ==
			RecoveryHandoffResult::DispatchServerInfo);
		assert(delayed.BeginBaseline(accountA, true, 28.9) ==
			RecoveryHandoffResult::BeginBaseline);

		RequiredRecoveryHandoff expired;
		assert(expired.Queue(accountA, 104, 0.0, 15.0));
		assert(expired.TryInvoke(accountA, 104, true, true, 1.0) ==
			RecoveryHandoffResult::Invoke);
		assert(expired.TryDispatchServerInfo(accountA, true, true, true, 14.0) ==
			RecoveryHandoffResult::DispatchServerInfo);
		assert(expired.BeginBaseline(accountA, true, 29.0) ==
			RecoveryHandoffResult::Expired);
		assert(!expired.Pending());
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

	// Queue clients may reach ServerInfo before Source exposes their SteamID64 or before
	// a map base existed at their earlier connect callback. They still receive the
	// current required base at their first baseline, but cannot consume an account-owned
	// recovery latch until identity resolves. Later authenticated failure ownership may
	// bind for the first time, while a previously-bound account must remain unchanged.
	{
		const Lane queuedRequiredLane = ResolveLane(true, true, nullptr);
		assert(queuedRequiredLane == Lane::Required);
		assert(!CanConsumeRequiredRecovery(true, false));
		assert(CanConsumeRequiredRecovery(true, true));
		assert(!CanConsumeRequiredRecovery(false, true));
		assert(NeedsConnectionEpochAtBaseline(0));
		assert(!NeedsConnectionEpochAtBaseline(1));
		assert(ShouldPinCurrentBaseForBaseline(Lane::Required, false, true));
		assert(!ShouldPinCurrentBaseForBaseline(Lane::Required, true, true));
		assert(!ShouldPinCurrentBaseForBaseline(Lane::Required, false, false));
		assert(!ShouldPinCurrentBaseForBaseline(Lane::NativeRecovery, false, true));
		assert(!ShouldPinCurrentBaseForBaseline(Lane::NativeOptOut, false, true));
		assert(!ShouldPinCurrentBaseForBaseline(Lane::NativeRescue, false, true));
		bool queueClientHasPinnedBase = false;
		if (ShouldPinCurrentBaseForBaseline(queuedRequiredLane,
			queueClientHasPinnedBase, true))
			queueClientHasPinnedBase = true;
		assert(queueClientHasPinnedBase);
		assert(SelectBaseline(queuedRequiredLane, true, BaseAvailability::Ready) ==
			Action::CanonicalStub);
		assert(!RequiredFailureIdentityReady(true, true, false));
		assert(RequiredFailureIdentityReady(true, true, true));
		assert(!RequiredFailureIdentityReady(true, false, true));
		assert(RequiredFailureIdentityMatches(false, 0, accountA));
		assert(RequiredFailureIdentityMatches(true, accountA, accountA));
		assert(!RequiredFailureIdentityMatches(true, accountA, accountB));
		assert(!RequiredFailureIdentityMatches(false, 0, 0));
		assert(RejectUnavailableRequiredAdmission(true, true, false));
		assert(!RejectUnavailableRequiredAdmission(true, true, true));
		assert(!RejectUnavailableRequiredAdmission(true, false, false));
		assert(!RejectUnavailableRequiredAdmission(false, true, false));
		assert(UsesCanonicalRegistration(true, true));
		assert(!UsesCanonicalRegistration(false, true));
		assert(!UsesCanonicalRegistration(true, false));
		assert(UsesPassthroughProcessing(true));
		assert(!UsesPassthroughProcessing(false));
		assert(RegistrationModeMatches(true, true, true, true));
		assert(RegistrationModeMatches(false, false, false, true));
		assert(RegistrationModeMatches(false, true, true, false));
		assert(!RegistrationModeMatches(true, true, false, true));
		assert(!RegistrationModeMatches(false, false, true, true));
		assert(!RegistrationModeMatches(false, false, true, false));
		assert(RegistrationNeedsHashPublication(true, true, true, true, true, false, true));
		assert(RegistrationNeedsHashPublication(false, false, true, true, true, true, true));
		assert(RegistrationNeedsHashPublication(true, true, true, false, false, true, true));
		assert(RegistrationNeedsHashPublication(true, true, true, true, false, true, true));
		assert(RegistrationNeedsHashPublication(false, false, false, true, true, false, true));
		assert(!RegistrationNeedsHashPublication(true, true, true, true, true, true, true));
		assert(!RegistrationNeedsHashPublication(false, false, true, true, true, false, true));
		assert(NeedsOrderedCanonicalHash(true, false, false));
		assert(NeedsOrderedCanonicalHash(false, true, false));
		assert(NeedsOrderedCanonicalHash(false, false, true));
		assert(!NeedsOrderedCanonicalHash(false, false, false));

		// A required connection may reuse the exact per-file decision made while its
		// string-table baseline was serialized. File/source transitions invalidate only
		// the affected ID, and the kill switch, hook loss, or incomplete payload always
		// leaves the slower identity-aware path in control.
		PinnedCanonicalFileSet<8> pinned;
		assert(!pinned.Contains(3));
		pinned.Mark(3);
		pinned.Mark(-1);
		pinned.Mark(8);
		assert(pinned.Contains(3));
		assert(!pinned.Contains(-1));
		assert(!pinned.Contains(8));
		assert(CanUsePinnedRequiredStub(true, true, pinned.Contains(3), true, 73));
		assert(!CanUsePinnedRequiredStub(false, true, pinned.Contains(3), true, 73));
		assert(!CanUsePinnedRequiredStub(true, false, pinned.Contains(3), true, 73));
		assert(!CanUsePinnedRequiredStub(true, true, false, true, 73));
		assert(!CanUsePinnedRequiredStub(true, true, pinned.Contains(3), false, 73));
		assert(!CanUsePinnedRequiredStub(true, true, pinned.Contains(3), true, 31));
		assert(CanDecodePinnedRequiredRequestBatch(true, true, true, 73,
			16 * 7, 16 * 7, 8, 8));
		assert(!CanDecodePinnedRequiredRequestBatch(false, true, true, 73,
			16 * 7, 16 * 7, 8, 8));
		assert(!CanDecodePinnedRequiredRequestBatch(true, false, true, 73,
			16 * 7, 16 * 7, 8, 8));
		assert(!CanDecodePinnedRequiredRequestBatch(true, true, false, 73,
			16 * 7, 16 * 7, 8, 8));
		assert(!CanDecodePinnedRequiredRequestBatch(true, true, true, 31,
			16 * 7, 16 * 7, 8, 8));
		assert(!CanDecodePinnedRequiredRequestBatch(true, true, true, 73,
			0, 0, 8, 8));
		assert(!CanDecodePinnedRequiredRequestBatch(true, true, true, 73,
			17, 17, 8, 8));
		assert(!CanDecodePinnedRequiredRequestBatch(true, true, true, 73,
			16 * 7, 16 * 6, 8, 8));
		assert(!CanDecodePinnedRequiredRequestBatch(true, true, true, 73,
			16 * 9, 16 * 9, 8, 8));
		assert(!CanDecodePinnedRequiredRequestBatch(true, true, true, 73,
			16 * 7, 16 * 7, 9, 8));

		TestBitReader requestedIds({5, 2, 5, 0, 7, 8});
		PinnedCanonicalFileSet<8> decodedRequests;
		std::size_t uniqueRequests = 0;
		assert(DecodeRequiredRequestIds(requestedIds, 16 * 6, 8,
			decodedRequests, uniqueRequests));
		assert(uniqueRequests == 3);
		std::vector<int> orderedRequests;
		for (int fileID = 1; fileID < 8; ++fileID)
		{
			if (decodedRequests.Contains(fileID))
				orderedRequests.push_back(fileID);
		}
		assert((orderedRequests == std::vector<int>{2, 5, 7}));
		TestBitReader shortRequest({1}, 15);
		PinnedCanonicalFileSet<8> rejectedRequests;
		assert(!DecodeRequiredRequestIds(shortRequest, 16, 8,
			rejectedRequests, uniqueRequests));
		assert(uniqueRequests == 0);
		pinned.Invalidate(3);
		assert(!pinned.Contains(3));
		pinned.Mark(4);
		pinned.Reset();
		assert(!pinned.Contains(4));

		std::array<PinnedCanonicalFileSet<8>, 2> clientPlans;
		clientPlans[0].Mark(2);
		clientPlans[0].Mark(5);
		clientPlans[1].Mark(2);
		for (auto& plan : clientPlans)
			plan.Invalidate(2);
		assert(!clientPlans[0].Contains(2));
		assert(!clientPlans[1].Contains(2));
		assert(clientPlans[0].Contains(5));

		RequiredStubQueue<4> requiredQueue;
		assert(requiredQueue.Empty());
		assert(requiredQueue.Enqueue(2) == RequiredStubEnqueueResult::Queued);
		assert(requiredQueue.Enqueue(2) == RequiredStubEnqueueResult::AlreadyQueued);
		assert(requiredQueue.Enqueue(1) == RequiredStubEnqueueResult::Queued);
		assert(requiredQueue.Enqueue(-1) == RequiredStubEnqueueResult::OutOfRange);
		assert(requiredQueue.Enqueue(4) == RequiredStubEnqueueResult::OutOfRange);
		assert(requiredQueue.Size() == 2);
		assert(requiredQueue.Front() == 2);
		requiredQueue.Pop();
		assert(requiredQueue.Size() == 1);
		assert(requiredQueue.Front() == 1);
		assert(requiredQueue.Enqueue(2) == RequiredStubEnqueueResult::Queued);
		requiredQueue.Pop();
		assert(requiredQueue.Front() == 2);
		requiredQueue.Reset();
		assert(requiredQueue.Empty());
		assert(requiredQueue.Front() == -1);

		RequiredStubScheduler<4> requiredScheduler;
		assert(requiredScheduler.Empty());
		assert(requiredScheduler.Schedule(1));
		assert(requiredScheduler.Schedule(2));
		assert(!requiredScheduler.Schedule(1));
		assert(!requiredScheduler.Schedule(-1));
		assert(!requiredScheduler.Schedule(4));
		assert(requiredScheduler.Size() == 2);
		assert(requiredScheduler.TakeNext() == 1);
		assert(!requiredScheduler.IsScheduled(1));
		assert(requiredScheduler.Schedule(1));
		assert(requiredScheduler.TakeNext() == 2);
		assert(requiredScheduler.TakeNext() == 1);
		assert(requiredScheduler.Empty());
		assert(requiredScheduler.Schedule(3));
		requiredScheduler.Unschedule(3);
		assert(requiredScheduler.Empty());
		requiredScheduler.Schedule(0);
		requiredScheduler.Reset();
		assert(requiredScheduler.Empty());

		// SendServerInfo admission uses the same fixed-capacity FIFO but one global
		// budget across physical and parked slots. Repeated polls never move an older
		// request, and finite mixed floods drain without starvation.
		RoundRobinSlotScheduler<256> serverInfoScheduler;
		for (int slot = 0; slot < 60; ++slot)
			assert(serverInfoScheduler.Schedule(slot));
		for (int slot = 128; slot < 188; ++slot)
			assert(serverInfoScheduler.Schedule(slot));
		assert(!serverInfoScheduler.Schedule(0));
		std::array<bool, 256> serviced{};
		unsigned int frames = 0;
		while (!serverInfoScheduler.Empty())
		{
			unsigned int servicedThisFrame = 0;
			while (servicedThisFrame < 1 && !serverInfoScheduler.Empty())
			{
				const int slot = serverInfoScheduler.TakeNext();
				assert(slot >= 0 && slot < 256);
				assert(!serviced[static_cast<std::size_t>(slot)]);
				serviced[static_cast<std::size_t>(slot)] = true;
				++servicedThisFrame;
			}
			assert(servicedThisFrame <= 1);
			++frames;
		}
		assert(frames == 120);
		for (int slot = 0; slot < 60; ++slot)
			assert(serviced[static_cast<std::size_t>(slot)]);
		for (int slot = 128; slot < 188; ++slot)
			assert(serviced[static_cast<std::size_t>(slot)]);

		// A stale head can be discarded without consuming the execution budget; the
		// next exact token remains serviceable in that same frame.
		assert(serverInfoScheduler.Schedule(7));
		assert(serverInfoScheduler.Schedule(9));
		serverInfoScheduler.Unschedule(7);
		assert(serverInfoScheduler.Schedule(7));
		assert(serverInfoScheduler.TakeNext() == 9);
		assert(serverInfoScheduler.TakeNext() == 7);
		assert(serverInfoScheduler.Schedule(7));
		assert(serverInfoScheduler.Schedule(9));
		assert(serverInfoScheduler.TakeNext() == 7);
		assert(serverInfoScheduler.TakeNext() == 9);
		assert(serverInfoScheduler.Empty());
		assert(serverInfoScheduler.Schedule(7));
		assert(serverInfoScheduler.TakeNext() == 7);
		serverInfoScheduler.Reset();
		assert(serverInfoScheduler.Empty());

		assert(ShouldScheduleLuaPackServerInfo(true, true, true, true, true, true));
		assert(!ShouldScheduleLuaPackServerInfo(false, true, true, true, true, true));
		assert(!ShouldScheduleLuaPackServerInfo(true, false, true, true, true, true));
		assert(!ShouldScheduleLuaPackServerInfo(true, true, false, true, true, true));
		assert(!ShouldScheduleLuaPackServerInfo(true, true, true, false, true, true));
		assert(!ShouldScheduleLuaPackServerInfo(true, true, true, true, false, true));
		assert(!ShouldScheduleLuaPackServerInfo(true, true, true, true, true, false));
		assert(QueuedServerInfoIdentityMatches(true, true, true, true, true, true, true));
		assert(!QueuedServerInfoIdentityMatches(false, true, true, true, true, true, true));
		assert(!QueuedServerInfoIdentityMatches(true, false, true, true, true, true, true));
		assert(!QueuedServerInfoIdentityMatches(true, true, false, true, true, true, true));
		assert(!QueuedServerInfoIdentityMatches(true, true, true, false, true, true, true));
		assert(!QueuedServerInfoIdentityMatches(true, true, true, true, false, true, true));
		assert(!QueuedServerInfoIdentityMatches(true, true, true, true, true, false, true));
		assert(!QueuedServerInfoIdentityMatches(true, true, true, true, true, true, false));

		// A global budget advances clients in round-robin order instead of letting the
		// first cold join monopolize a frame.
		std::array<RequiredStubQueue<8>, 2> fairQueues;
		for (int fileID : {1, 2})
			assert(fairQueues[0].Enqueue(fileID) == RequiredStubEnqueueResult::Queued);
		for (int fileID : {3, 4})
			assert(fairQueues[1].Enqueue(fileID) == RequiredStubEnqueueResult::Queued);
		RequiredStubScheduler<2> fairScheduler;
		assert(fairScheduler.Schedule(0));
		assert(fairScheduler.Schedule(1));
		std::array<int, 3> fairOrder{};
		for (std::size_t sent = 0; sent < fairOrder.size(); ++sent)
		{
			const int slot = fairScheduler.TakeNext();
			assert(slot >= 0);
			fairOrder[sent] = slot;
			fairQueues[slot].Pop();
			if (!fairQueues[slot].Empty())
				assert(fairScheduler.Schedule(slot));
		}
		assert((fairOrder == std::array<int, 3>{0, 1, 0}));
		assert(fairQueues[0].Empty());
		assert(fairQueues[1].Size() == 1);
		assert(fairScheduler.TakeNext() == 1);

		assert(RequiredStubPayloadBits(73) == 608);
		assert(RequiredStubWireBits(73) == 634);
		assert(CanAppendRequiredStub(false, 634, 73));
		assert(!CanAppendRequiredStub(false, 633, 73));
		assert(!CanAppendRequiredStub(true, 634, 73));
		assert(CanAppendRequiredStub(false,
			(std::numeric_limits<std::size_t>::max)(), 131068));
		assert(!CanAppendRequiredStub(false,
			(std::numeric_limits<std::size_t>::max)(), 131069));
		assert(!CanAppendRequiredStub(false,
			(std::numeric_limits<std::size_t>::max)(), 1u << 17u));
		assert(SelectRequiredStubDrainAction(true, true, true, true, 73,
			true, false, 634) == RequiredStubDrainAction::Append);
		assert(SelectRequiredStubDrainAction(true, true, true, true, 73,
			true, false, 633) == RequiredStubDrainAction::WaitForReliableSpace);
		assert(SelectRequiredStubDrainAction(false, true, true, true, 73,
			true, false, 634) == RequiredStubDrainAction::Reject);
		assert(SelectRequiredStubDrainAction(true, false, true, true, 73,
			true, false, 634) == RequiredStubDrainAction::Reject);
		assert(SelectRequiredStubDrainAction(true, true, false, true, 73,
			true, false, 634) == RequiredStubDrainAction::Reject);
		assert(SelectRequiredStubDrainAction(true, true, true, false, 73,
			true, false, 634) == RequiredStubDrainAction::Reject);
		assert(SelectRequiredStubDrainAction(true, true, true, true, 31,
			true, false, 634) == RequiredStubDrainAction::Reject);
		assert(SelectRequiredStubDrainAction(true, true, true, true, 73,
			false, false, 634) == RequiredStubDrainAction::Reject);
		assert(SelectRequiredStubDrainAction(true, true, true, true, 73,
			true, true, 634) == RequiredStubDrainAction::Reject);

		const std::array<unsigned char, 3> compressed = {0x00, 0x7f, 0xff};
		const std::size_t wireBits = RequiredStubWireBits(compressed.size());
		TestBitWriter wire(3 + wireBits);
		wire.WriteUBitLong(5, 3); // Exercise the production writer from an unaligned offset.
		std::size_t start = wire.bits.size();
		assert(AppendRequiredStubWire(wire, 33, 7, 0x1234,
			compressed.data(), compressed.size()));
		assert(wire.bits.size() - start == wireBits);
		assert(wire.ReadUBitLong(start, RequiredStubServiceTypeBits) == 33);
		start += RequiredStubServiceTypeBits;
		assert(wire.ReadUBitLong(start, RequiredStubPayloadLengthBits) ==
			RequiredStubPayloadBits(compressed.size()));
		start += RequiredStubPayloadLengthBits;
		assert(wire.ReadUBitLong(start, RequiredStubMessageTypeBits) == 7);
		start += RequiredStubMessageTypeBits;
		assert(wire.ReadUBitLong(start, RequiredStubFileIdBits) == 0x1234);
		start += RequiredStubFileIdBits;
		assert(wire.ReadUBitLong(start, 8) == compressed[0]);
		assert(wire.ReadUBitLong(start + 8, 8) == compressed[1]);
		assert(wire.ReadUBitLong(start + 16, 8) == compressed[2]);

		TestBitWriter shortWire(wireBits - 1);
		assert(!AppendRequiredStubWire(shortWire, 33, 7, 1,
			compressed.data(), compressed.size()));
		assert(shortWire.bits.empty());
		assert(!shortWire.IsOverflowed());
		TestBitWriter invalidWire(wireBits);
		assert(!AppendRequiredStubWire(invalidWire, 64, 7, 1,
			compressed.data(), compressed.size()));
		assert(!AppendRequiredStubWire(invalidWire, 33, 256, 1,
			compressed.data(), compressed.size()));
		assert(!AppendRequiredStubWire(invalidWire, 33, 7, 65536,
			compressed.data(), compressed.size()));
		assert(!AppendRequiredStubWire(invalidWire, 33, 7, 1, nullptr, 1));
		assert(invalidWire.bits.empty());

		constexpr std::size_t scprpFiles = 3922;
		const std::array<unsigned char, 73> canonicalStub{};
		const std::size_t scprpCapacity = RequiredStubReliableCapacityBytes(
			scprpFiles, canonicalStub.size());
		TestBitWriter scprpBurst(scprpCapacity * 8);
		for (std::size_t fileID = 1; fileID <= scprpFiles; ++fileID)
		{
			assert(AppendRequiredStubWire(scprpBurst, 33, 7,
				static_cast<std::uint32_t>(fileID), canonicalStub.data(),
				canonicalStub.size()));
		}
		assert(scprpBurst.bits.size() ==
			scprpFiles * RequiredStubWireBits(canonicalStub.size()));
		assert(scprpBurst.GetNumBitsLeft() >=
			static_cast<int>(RequiredStubReliableReserveBytes * 8));
		assert(RequiredStubReliableCapacityBytes(795, 73) == 128540);
		assert(RequiredStubReliableCapacityBytes(32, 73) == 68072);
		assert(RequiredStubReliableCapacityBytes(3922, 73) == 376355);
		assert(RequiredStubReliableCapacityBytes(3922, 73, 262144) == 572963);
		assert(RequiredStubReliableCapacityBytes(5048, 73) == 465590);
		assert(RequiredStubReliableCapacityBytes(5048, 73, 262144) == 662198);
		assert(RequiredStubReliableCapacityBytes(795, 73) <= 262144);
		assert(RequiredStubReliableCapacityBytes(3922, 73) > 262144);
		assert(RequiredStubReliableCapacityBytes(3922, 73, 262144) <= 576000);
		assert(RequiredStubReliableCapacityBytes(5048, 73) <= 576000);
		assert(RequiredStubReliableCapacityBytes(5048, 73, 262144) > 576000);
		assert(RequiredStubWireBits((std::numeric_limits<std::size_t>::max)()) ==
			(std::numeric_limits<std::size_t>::max)());
		assert(RequiredStubReliableCapacityBytes(
			(std::numeric_limits<std::size_t>::max)(), 73) ==
			(std::numeric_limits<std::size_t>::max)());
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
	assert(SelectFile(Lane::Required, true, BaseAvailability::Ready, false, false) == Action::CanonicalStub);
	assert(SelectFile(Lane::Required, true, BaseAvailability::Ready, false, true) == Action::Native);
	assert(SelectFile(Lane::Required, true, BaseAvailability::Ready, true, false) == Action::Native);
	assert(SelectFile(Lane::Required, false, BaseAvailability::Ready, true, false) == Action::Reject);
	assert(ShouldBuildMapBase(false, true));
	assert(!ShouldBuildMapBase(true, true));
	assert(!ShouldBuildMapBase(true, false));

	// A changed path becomes native and exact restoration returns it to the canonical base.
	const std::string baseIdentity = "base-sha256";
	assert(IsNativeDelta(true, baseIdentity, "hotfix-sha256"));
	assert(!IsNativeDelta(true, baseIdentity, baseIdentity));
	assert(IsNativeDelta(false, "", "late-registration-sha256"));

	std::unordered_set<std::string> nativeDeltaIndex;
	UpdateNativeDeltaIndex(nativeDeltaIndex, std::string("lua/hot.lua"),
		IsNativeDelta(true, baseIdentity, "hotfix-sha256"));
	assert(nativeDeltaIndex.count("lua/hot.lua") == 1);
	UpdateNativeDeltaIndex(nativeDeltaIndex, std::string("lua/hot.lua"),
		IsNativeDelta(true, baseIdentity, baseIdentity));
	assert(nativeDeltaIndex.empty());
	UpdateNativeDeltaIndex(nativeDeltaIndex, std::string("lua/late.lua"),
		IsNativeDelta(false, "", "late-registration-sha256"));
	assert(nativeDeltaIndex.count("lua/late.lua") == 1);

	// Disabling capture invalidates every immutable-base identity. Re-enable then
	// proves each path exact or native as the registration sweep recaptures it.
	std::unordered_map<std::string, std::string> preservedBase{
		{"lua/a.lua", "base-a"},
		{"lua/b.lua", "base-b"},
	};
	MarkBasePathsNative(nativeDeltaIndex, preservedBase);
	assert(nativeDeltaIndex.count("lua/a.lua") == 1);
	assert(nativeDeltaIndex.count("lua/b.lua") == 1);
	UpdateNativeDeltaIndex(nativeDeltaIndex, std::string("lua/a.lua"), false);
	UpdateNativeDeltaIndex(nativeDeltaIndex, std::string("lua/b.lua"), true);
	assert(nativeDeltaIndex.count("lua/a.lua") == 0);
	assert(nativeDeltaIndex.count("lua/b.lua") == 1);

	assert(SelectBaselineHashDisposition(true, true) ==
		BaselineHashDisposition::ReusePublished);
	assert(SelectBaselineHashDisposition(true, false) ==
		BaselineHashDisposition::ReusePublished);
	assert(SelectBaselineHashDisposition(false, true) ==
		BaselineHashDisposition::OverrideCached);
	assert(SelectBaselineHashDisposition(false, false) ==
		BaselineHashDisposition::OverrideComputed);

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

	// Linux globally broadcasts the canonical registration hash before every active
	// hot refresh. All four lanes can subsequently select a native body, so each must
	// restore that body's native identity rather than pair it with the canonical hash.
	assert(NeedsPerClientNativeHashes(Lane::Required));
	assert(NeedsPerClientNativeHashes(Lane::NativeRecovery));
	assert(NeedsPerClientNativeHashes(Lane::NativeOptOut));
	assert(NeedsPerClientNativeHashes(Lane::NativeRescue));
	std::unordered_map<int, TestHash> nativeLaneHashes;
	RememberNativeHash(nativeLaneHashes, 84, hotfixOne);
	assert(NativeHashMatches(nativeLaneHashes, 84, hotfixOne));
	assert(RestoreCanonicalHash(nativeLaneHashes, 84));
	assert(!NativeHashMatches(nativeLaneHashes, 84, hotfixTwo));
	RememberNativeHash(nativeLaneHashes, 84, hotfixTwo);
	assert(NativeHashMatches(nativeLaneHashes, 84, hotfixTwo));

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
	assert(SelectFile(Lane::NativeOptOut, true, BaseAvailability::Missing, false, true) == Action::Native);
	assert(SelectFile(Lane::NativeOptOut, false, BaseAvailability::Missing, false, true) == Action::Native);
	assert(ResolveLane(false, false, nullptr) == Lane::NativeRescue);
	assert(SelectBaseline(Lane::NativeRescue, false, BaseAvailability::Missing) == Action::Native);
	assert(SelectFile(Lane::NativeRescue, true, BaseAvailability::Missing, false, true) == Action::Native);
	assert(SelectFile(Lane::NativeRescue, false, BaseAvailability::Missing, false, true) == Action::Native);

	// Required mode fails closed when the base is absent, invalid, or cannot be canonicalized.
	assert(SelectBaseline(Lane::Required, true, BaseAvailability::Missing) == Action::Reject);
	assert(SelectBaseline(Lane::Required, true, BaseAvailability::Unusable) == Action::Reject);
	assert(SelectBaseline(Lane::Required, false, BaseAvailability::Ready) == Action::Reject);
	assert(SelectFile(Lane::Required, true, BaseAvailability::Missing, false, true) == Action::Reject);

	std::cout << "luapack map-base policy tests passed\n";
	return 0;
}

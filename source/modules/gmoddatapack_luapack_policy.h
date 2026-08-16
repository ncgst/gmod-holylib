#pragma once

#include <algorithm>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace HolyLib::LuaPack::Policy
{
	template <std::size_t Capacity>
	class PinnedCanonicalFileSet
	{
	public:
		void Reset()
		{
			files.reset();
		}

		void Mark(int fileID)
		{
			if (fileID >= 0 && static_cast<std::size_t>(fileID) < Capacity)
				files.set(static_cast<std::size_t>(fileID));
		}

		void Invalidate(int fileID)
		{
			if (fileID >= 0 && static_cast<std::size_t>(fileID) < Capacity)
				files.reset(static_cast<std::size_t>(fileID));
		}

		bool Contains(int fileID) const
		{
			return fileID >= 0 && static_cast<std::size_t>(fileID) < Capacity &&
				files.test(static_cast<std::size_t>(fileID));
		}

	private:
		std::bitset<Capacity> files;
	};

	enum class RequiredStubEnqueueResult
	{
		Queued,
		AlreadyQueued,
		OutOfRange,
	};

	// GMod can request the complete missing Lua baseline in one ProcessMessages call.
	// Retain one response per unique file ID without allocating an unbounded queue or
	// allowing a repeated request to amplify the reliable response burst.
	template <std::size_t Capacity>
	class RequiredStubQueue
	{
	public:
		RequiredStubEnqueueResult Enqueue(int fileID)
		{
			if (fileID < 0 || static_cast<std::size_t>(fileID) >= Capacity)
				return RequiredStubEnqueueResult::OutOfRange;
			if (queued.Contains(fileID))
				return RequiredStubEnqueueResult::AlreadyQueued;

			queued.Mark(fileID);
			files.push_back(fileID);
			return RequiredStubEnqueueResult::Queued;
		}

		bool Empty() const
		{
			return files.empty();
		}

		std::size_t Size() const
		{
			return files.size();
		}

		int Front() const
		{
			return files.empty() ? -1 : files.front();
		}

		void Pop()
		{
			if (files.empty())
				return;

			queued.Invalidate(files.front());
			files.pop_front();
		}

		void Reset()
		{
			files.clear();
			queued.Reset();
		}

	private:
		std::deque<int> files;
		PinnedCanonicalFileSet<Capacity> queued;
	};

	template <std::size_t SlotCount>
	class RequiredStubScheduler
	{
	public:
		bool Schedule(int slot)
		{
			if (slot < 0 || static_cast<std::size_t>(slot) >= SlotCount ||
			scheduled.test(static_cast<std::size_t>(slot)))
			{
				return false;
			}

			scheduled.set(static_cast<std::size_t>(slot));
			active.push_back(slot);
			return true;
		}

		int TakeNext()
		{
			if (active.empty())
				return -1;

			const int slot = active.front();
			active.pop_front();
			scheduled.reset(static_cast<std::size_t>(slot));
			return slot;
		}

		void Unschedule(int slot)
		{
			if (slot < 0 || static_cast<std::size_t>(slot) >= SlotCount)
				return;

			scheduled.reset(static_cast<std::size_t>(slot));
			active.erase(std::remove(active.begin(), active.end(), slot), active.end());
		}

		bool IsScheduled(int slot) const
		{
			return slot >= 0 && static_cast<std::size_t>(slot) < SlotCount &&
				scheduled.test(static_cast<std::size_t>(slot));
		}

		bool Empty() const
		{
			return active.empty();
		}

		std::size_t Size() const
		{
			return active.size();
		}

		void Reset()
		{
			active.clear();
			scheduled.reset();
		}

	private:
		std::deque<int> active;
		std::bitset<SlotCount> scheduled;
	};

	constexpr bool CanUsePinnedRequiredStub(bool enabled, bool canonicalRegistration,
		bool filePinned, bool payloadAvailable, std::size_t compressedBytes)
	{
		return enabled && canonicalRegistration && filePinned && payloadAvailable &&
			compressedBytes >= 32u;
	}

	template <typename Index, typename Key>
	void UpdateNativeDeltaIndex(Index& nativeDeltas, const Key& key, bool nativeDelta)
	{
		if (nativeDelta)
			nativeDeltas.insert(key);
		else
			nativeDeltas.erase(key);
	}

	template <typename Index, typename Files>
	void MarkBasePathsNative(Index& nativeDeltas, const Files& baseFiles)
	{
		for (const auto& baseFile : baseFiles)
			nativeDeltas.insert(baseFile.first);
	}

	enum class BaselineHashDisposition
	{
		ReusePublished,
		OverrideCached,
		OverrideComputed,
	};

	constexpr BaselineHashDisposition SelectBaselineHashDisposition(
		bool publishedMatches, bool cachedIdentity)
	{
		if (publishedMatches)
			return BaselineHashDisposition::ReusePublished;
		return cachedIdentity
			? BaselineHashDisposition::OverrideCached
			: BaselineHashDisposition::OverrideComputed;
	}

	// GMod encodes one requested Lua file ID in each 16-bit word. Required delivery
	// may decode that bounded batch directly only while the exact pinned baseline and
	// payload remain usable. Empty or malformed messages retain the engine parser.
	constexpr bool CanDecodePinnedRequiredRequestBatch(bool enabled,
		bool canonicalRegistration, bool payloadAvailable,
		std::size_t compressedBytes, int messageBits, int availableBits,
		int registeredFiles, std::size_t trackedCapacity)
	{
		return CanUsePinnedRequiredStub(enabled, canonicalRegistration, true,
			payloadAvailable, compressedBytes) &&
			messageBits > 0 && availableBits >= messageBits &&
			(messageBits % 16) == 0 && registeredFiles > 1 &&
			static_cast<std::size_t>(registeredFiles) <= trackedCapacity &&
			(messageBits / 16) <= registeredFiles;
	}

	template <std::size_t Capacity, typename BitReader>
	bool DecodeRequiredRequestIds(BitReader& message, int messageBits,
		int registeredFiles, PinnedCanonicalFileSet<Capacity>& requested,
		std::size_t& uniqueRequests)
	{
		uniqueRequests = 0;
		if (messageBits <= 0 || message.GetNumBitsLeft() < messageBits ||
			(messageBits % 16) != 0 || registeredFiles <= 1 ||
			static_cast<std::size_t>(registeredFiles) > Capacity ||
			(messageBits / 16) > registeredFiles)
		{
			return false;
		}

		const int requestCount = messageBits / 16;
		for (int request = 0; request < requestCount; ++request)
		{
			const int fileID = static_cast<int>(message.ReadUBitLong(16));
			if (fileID <= 0 || fileID >= registeredFiles || requested.Contains(fileID))
				continue;
			requested.Mark(fileID);
			++uniqueRequests;
		}
		return !message.IsOverflowed();
	}

	enum class Lane
	{
		NativeRescue,
		NativeOptOut,
		NativeRecovery,
		Required,
	};

	enum class RecoveryPhase
	{
		Armed,
		Consumed,
	};

	enum class RecoveryArmResult
	{
		Armed,
		Invalid,
		RetryExhausted,
	};

	enum class RecoveryConsumeResult
	{
		None,
		Native,
		SameConnection,
		RetryExhausted,
		Expired,
	};

	enum class RecoveryClearResult
	{
		None,
		Cleared,
		SameFailedConnection,
	};

	struct RecoveryEntry
	{
		RecoveryPhase phase = RecoveryPhase::Armed;
		std::uint64_t failedConnection = 0;
		std::uint64_t recoveryConnection = 0;
		double expiresAt = 0.0;
	};

	// Main-thread state machine for required-pack recovery. The account key prevents
	// slot reuse from inheriting another player's lane, while connection serials make
	// it impossible to consume a latch on the connection that armed it.
	class RequiredRecoveryTracker
	{
	public:
		RecoveryArmResult Arm(std::uint64_t account, std::uint64_t connection,
			double now, double ttl)
		{
			if (account == 0 || connection == 0 || ttl <= 0.0)
				return RecoveryArmResult::Invalid;

			EraseExpired(account, now);
			if (entries.find(account) != entries.end())
				return RecoveryArmResult::RetryExhausted;

			entries.emplace(account, RecoveryEntry{
				RecoveryPhase::Armed, connection, 0, now + ttl});
			return RecoveryArmResult::Armed;
		}

		RecoveryConsumeResult Consume(std::uint64_t account, std::uint64_t connection,
			double now)
		{
			if (account == 0 || connection == 0)
				return RecoveryConsumeResult::None;

			auto entry = entries.find(account);
			if (entry == entries.end())
				return RecoveryConsumeResult::None;
			if (entry->second.phase == RecoveryPhase::Armed && now >= entry->second.expiresAt)
			{
				entries.erase(entry);
				return RecoveryConsumeResult::Expired;
			}
			if (entry->second.phase != RecoveryPhase::Armed)
				return RecoveryConsumeResult::RetryExhausted;
			if (entry->second.failedConnection == connection)
				return RecoveryConsumeResult::SameConnection;

			entry->second.phase = RecoveryPhase::Consumed;
			entry->second.recoveryConnection = connection;
			return RecoveryConsumeResult::Native;
		}

		bool Complete(std::uint64_t account, std::uint64_t connection)
		{
			auto entry = entries.find(account);
			if (entry == entries.end() || entry->second.phase != RecoveryPhase::Consumed ||
				entry->second.recoveryConnection != connection)
				return false;
			entries.erase(entry);
			return true;
		}

		bool OwnsConsumed(std::uint64_t account, std::uint64_t connection) const
		{
			auto entry = entries.find(account);
			return account != 0 && connection != 0 && entry != entries.end() &&
				entry->second.phase == RecoveryPhase::Consumed &&
				entry->second.recoveryConnection == connection;
		}

		bool Clear(std::uint64_t account)
		{
			return account != 0 && entries.erase(account) != 0;
		}

		RecoveryClearResult ClearAfterSuccessfulConnection(std::uint64_t account,
			std::uint64_t connection)
		{
			if (account == 0 || connection == 0)
				return RecoveryClearResult::None;

			auto entry = entries.find(account);
			if (entry == entries.end())
				return RecoveryClearResult::None;
			// A late READY/active callback from the connection that armed recovery is
			// not a safe boundary. Only a distinct successful connection can retire an
			// armed latch without consuming its native baseline.
			if (entry->second.phase == RecoveryPhase::Armed &&
				entry->second.failedConnection == connection)
				return RecoveryClearResult::SameFailedConnection;

			entries.erase(entry);
			return RecoveryClearResult::Cleared;
		}

		void Prune(double now)
		{
			for (auto entry = entries.begin(); entry != entries.end();)
			{
				if (entry->second.phase == RecoveryPhase::Armed && now >= entry->second.expiresAt)
					entry = entries.erase(entry);
				else
					++entry;
			}
		}

		void Reset()
		{
			entries.clear();
		}

		std::size_t Size() const
		{
			return entries.size();
		}

	private:
		void EraseExpired(std::uint64_t account, double now)
		{
			auto entry = entries.find(account);
			if (entry != entries.end() && entry->second.phase == RecoveryPhase::Armed &&
				now >= entry->second.expiresAt)
				entries.erase(entry);
		}

		std::unordered_map<std::uint64_t, RecoveryEntry> entries;
	};

	enum class RecoveryHandoffPhase
	{
		Empty,
		Queued,
		Invoked,
		ServerInfoClaimed,
	};

	enum class RecoveryHandoffResult
	{
		None,
		Invoke,
		AlreadyInvoked,
		NotReady,
		DispatchServerInfo,
		BeginBaseline,
		OwnershipMismatch,
		Invalid,
		Expired,
	};

	// Per-slot gate around the engine's server-side Reconnect call. It deliberately
	// keeps the failed connection's lane unchanged until a new SendServerInfo baseline
	// begins, and guarantees that the engine reconnect primitive is invoked at most once.
	class RequiredRecoveryHandoff
	{
	public:
		bool Queue(std::uint64_t inputAccount, std::uint64_t inputConnection,
			double now, double window)
		{
			if (phase != RecoveryHandoffPhase::Empty || inputAccount == 0 ||
				inputConnection == 0 || window <= 0.0)
				return false;
			account = inputAccount;
			failedConnection = inputConnection;
			expiresAt = now + window;
			phase = RecoveryHandoffPhase::Queued;
			return true;
		}

		RecoveryHandoffResult TryInvoke(std::uint64_t currentAccount,
			std::uint64_t currentConnection, bool authenticated, bool channelReady,
			double now)
		{
			if (phase == RecoveryHandoffPhase::Empty)
				return RecoveryHandoffResult::None;
			if (now >= expiresAt)
			{
				Reset();
				return RecoveryHandoffResult::Expired;
			}
			// Reconnect may temporarily clear or replace the slot before its next
			// ServerInfo. Once invoked, ownership is checked at that baseline instead;
			// this gate must neither invoke twice nor discard the account handoff.
			if (phase == RecoveryHandoffPhase::Invoked ||
				phase == RecoveryHandoffPhase::ServerInfoClaimed)
				return RecoveryHandoffResult::AlreadyInvoked;
			if (currentAccount != account || currentConnection != failedConnection)
				return RecoveryHandoffResult::OwnershipMismatch;
			if (!authenticated)
				return RecoveryHandoffResult::Invalid;
			if (!channelReady)
				return RecoveryHandoffResult::Invalid;
			phase = RecoveryHandoffPhase::Invoked;
			return RecoveryHandoffResult::Invoke;
		}

		RecoveryHandoffResult TryDispatchServerInfo(std::uint64_t currentAccount,
			bool authenticated, bool channelReady, bool signonRestarted, double now)
		{
			if (phase == RecoveryHandoffPhase::Empty)
				return RecoveryHandoffResult::None;
			if (now >= expiresAt)
			{
				Reset();
				return RecoveryHandoffResult::Expired;
			}
			if (phase == RecoveryHandoffPhase::Queued)
				return RecoveryHandoffResult::Invalid;
			if (phase == RecoveryHandoffPhase::ServerInfoClaimed)
				return RecoveryHandoffResult::AlreadyInvoked;
			// Reconnect can temporarily clear the slot, and the replacement connection can
			// be admitted in another slot. Never dispatch through an unidentified or reused
			// slot; the account-owned handoff remains available to that connection's normal
			// ServerInfo hook until the bounded expiry.
			if (currentAccount == 0 || !authenticated || !channelReady)
				return RecoveryHandoffResult::NotReady;
			if (currentAccount != account)
				return RecoveryHandoffResult::NotReady;
			if (!signonRestarted)
				return RecoveryHandoffResult::NotReady;
			phase = RecoveryHandoffPhase::ServerInfoClaimed;
			return RecoveryHandoffResult::DispatchServerInfo;
		}

		RecoveryHandoffResult BeginBaseline(std::uint64_t currentAccount,
			bool authenticated, double now)
		{
			if (phase == RecoveryHandoffPhase::Empty)
				return RecoveryHandoffResult::None;
			if (now >= expiresAt)
			{
				Reset();
				return RecoveryHandoffResult::Expired;
			}
			if (currentAccount != account)
				return RecoveryHandoffResult::OwnershipMismatch;
			if (!authenticated)
				return RecoveryHandoffResult::Invalid;
			if (phase != RecoveryHandoffPhase::Invoked &&
				phase != RecoveryHandoffPhase::ServerInfoClaimed)
				return RecoveryHandoffResult::Invalid;
			Reset();
			return RecoveryHandoffResult::BeginBaseline;
		}

		void Reset()
		{
			phase = RecoveryHandoffPhase::Empty;
			account = 0;
			failedConnection = 0;
			expiresAt = 0.0;
		}

		bool Pending() const
		{
			return phase != RecoveryHandoffPhase::Empty;
		}

		bool Invoked() const
		{
			return phase == RecoveryHandoffPhase::Invoked ||
				phase == RecoveryHandoffPhase::ServerInfoClaimed;
		}

		bool ResetIfOwnedBy(std::uint64_t inputAccount)
		{
			if (inputAccount == 0 || phase == RecoveryHandoffPhase::Empty ||
				account != inputAccount)
				return false;
			Reset();
			return true;
		}

		std::uint64_t Account() const
		{
			return account;
		}

		std::uint64_t FailedConnection() const
		{
			return failedConnection;
		}

	private:
		RecoveryHandoffPhase phase = RecoveryHandoffPhase::Empty;
		std::uint64_t account = 0;
		std::uint64_t failedConnection = 0;
		double expiresAt = 0.0;
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

	constexpr bool CanConsumeRequiredRecovery(bool recoveryEnabled,
		bool resolvedIdentity)
	{
		return recoveryEnabled && resolvedIdentity;
	}

	constexpr bool NeedsConnectionEpochAtBaseline(std::uint64_t connectionSerial)
	{
		return connectionSerial == 0;
	}

	constexpr bool ShouldPinCurrentBaseForBaseline(Lane lane,
		bool hasPinnedBase, bool currentBaseAvailable)
	{
		return lane == Lane::Required && !hasPinnedBase && currentBaseAvailable;
	}

	constexpr bool RequiredFailureIdentityReady(bool recoveryEnabled,
		bool resolvedIdentity, bool authenticatedIdentity)
	{
		return !recoveryEnabled || (resolvedIdentity && authenticatedIdentity);
	}

	constexpr bool RequiredFailureIdentityMatches(bool previouslyResolved,
		std::uint64_t expectedAccount, std::uint64_t authenticatedAccount)
	{
		return authenticatedAccount != 0 &&
			(!previouslyResolved || expectedAccount == authenticatedAccount);
	}

	constexpr bool RejectUnavailableRequiredAdmission(bool enabled, bool required,
		bool canonicalRegistration)
	{
		return enabled && required && !canonicalRegistration;
	}

	constexpr bool UsesCanonicalRegistration(bool enabled, bool canonicalRegistration)
	{
		return enabled && canonicalRegistration;
	}

	constexpr bool UsesPassthroughProcessing(bool enabled)
	{
		return enabled;
	}

	constexpr bool RegistrationModeMatches(bool cachedCanonical, bool cachedPassthrough,
		bool enabled, bool canonicalRegistration)
	{
		return cachedCanonical == UsesCanonicalRegistration(enabled, canonicalRegistration) &&
			cachedPassthrough == UsesPassthroughProcessing(enabled);
	}

	constexpr bool RegistrationNeedsHashPublication(bool cachedCanonical, bool cachedPassthrough,
		bool hasSource, bool contentReady, bool hashPublished,
		bool enabled, bool canonicalRegistration)
	{
		return !hasSource || !contentReady || !hashPublished ||
			!RegistrationModeMatches(cachedCanonical, cachedPassthrough,
				enabled, canonicalRegistration);
	}

	constexpr bool NeedsOrderedCanonicalHash(bool clientActive, bool nativeHashKnown,
		bool publishedHashMatchesCanonical)
	{
		return clientActive || nativeHashKnown || publishedHashMatchesCanonical;
	}

	// GMod asks for every missing Lua ID in one request. Required delivery retains that
	// ownership boundary but stages a bounded number of canonical placeholders per frame.
	// This math reserves reliable scratch space for one bounded batch plus unrelated
	// sign-on traffic. The outer wire envelope is the svc type (6) plus payload
	// length (20); the payload is the GMod message type (8), Lua file ID (16), and
	// compressed placeholder bytes.
	constexpr std::size_t RequiredStubReliableReserveBytes = 64u * 1024u;
	constexpr int RequiredStubServiceTypeBits = 6;
	constexpr int RequiredStubPayloadLengthBits = 20;
	constexpr int RequiredStubMessageTypeBits = 8;
	constexpr int RequiredStubFileIdBits = 16;
	constexpr std::size_t RequiredStubOuterEnvelopeBits =
		RequiredStubServiceTypeBits + RequiredStubPayloadLengthBits;
	constexpr std::size_t RequiredStubPayloadEnvelopeBits =
		RequiredStubMessageTypeBits + RequiredStubFileIdBits;
	constexpr std::size_t RequiredStubMaximumPayloadBits =
		(1u << RequiredStubPayloadLengthBits) - 1u;

	constexpr std::size_t RequiredStubPayloadBits(std::size_t compressedBytes)
	{
		const std::size_t maximum = (std::numeric_limits<std::size_t>::max)();
		return compressedBytes > (maximum - RequiredStubPayloadEnvelopeBits) / 8u
			? maximum
			: RequiredStubPayloadEnvelopeBits + compressedBytes * 8u;
	}

	constexpr std::size_t RequiredStubWireBits(std::size_t compressedBytes)
	{
		const std::size_t maximum = (std::numeric_limits<std::size_t>::max)();
		const std::size_t payloadBits = RequiredStubPayloadBits(compressedBytes);
		return payloadBits == maximum || payloadBits > maximum - RequiredStubOuterEnvelopeBits
			? maximum
			: RequiredStubOuterEnvelopeBits + payloadBits;
	}

	constexpr bool CanAppendRequiredStub(bool reliableOverflowed,
		std::size_t reliableBitsLeft, std::size_t compressedBytes)
	{
		const std::size_t payloadBits = RequiredStubPayloadBits(compressedBytes);
		const std::size_t wireBits = RequiredStubWireBits(compressedBytes);
		return !reliableOverflowed && payloadBits <= RequiredStubMaximumPayloadBits &&
			wireBits != (std::numeric_limits<std::size_t>::max)() &&
			wireBits <= reliableBitsLeft;
	}

	enum class RequiredStubDrainAction
	{
		Append,
		WaitForReliableSpace,
		Reject,
	};

	constexpr RequiredStubDrainAction SelectRequiredStubDrainAction(
		bool enabled, bool canonicalRegistration, bool filePinned,
		bool payloadAvailable, std::size_t compressedBytes,
		bool clientConnected, bool reliableOverflowed,
		std::size_t reliableBitsLeft)
	{
		if (!CanUsePinnedRequiredStub(enabled, canonicalRegistration,
			filePinned, payloadAvailable, compressedBytes) ||
			!clientConnected || reliableOverflowed)
		{
			return RequiredStubDrainAction::Reject;
		}

		const std::size_t payloadBits = RequiredStubPayloadBits(compressedBytes);
		const std::size_t wireBits = RequiredStubWireBits(compressedBytes);
		if (payloadBits > RequiredStubMaximumPayloadBits ||
			wireBits == (std::numeric_limits<std::size_t>::max)())
		{
			return RequiredStubDrainAction::Reject;
		}

		return wireBits <= reliableBitsLeft
			? RequiredStubDrainAction::Append
			: RequiredStubDrainAction::WaitForReliableSpace;
	}

	template <typename BitWriter>
	bool AppendRequiredStubWire(BitWriter& reliable, std::uint32_t serviceType,
		std::uint32_t messageType, std::uint32_t fileID,
		const void* compressed, std::size_t compressedBytes)
	{
		if (serviceType >= (1u << RequiredStubServiceTypeBits) ||
			messageType >= (1u << RequiredStubMessageTypeBits) ||
			fileID >= (1u << RequiredStubFileIdBits) ||
			(!compressed && compressedBytes != 0))
		{
			return false;
		}

		const int bitsBefore = reliable.GetNumBitsWritten();
		const int bitsLeft = reliable.GetNumBitsLeft();
		const std::size_t payloadBits = RequiredStubPayloadBits(compressedBytes);
		const std::size_t wireBits = RequiredStubWireBits(compressedBytes);
		if (bitsBefore < 0 || bitsLeft < 0 ||
			!CanAppendRequiredStub(reliable.IsOverflowed(),
				static_cast<std::size_t>(bitsLeft), compressedBytes) ||
			payloadBits > static_cast<std::size_t>((std::numeric_limits<int>::max)()) ||
			wireBits > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
		{
			return false;
		}

		reliable.WriteUBitLong(serviceType, RequiredStubServiceTypeBits);
		reliable.WriteUBitLong(static_cast<std::uint32_t>(payloadBits),
			RequiredStubPayloadLengthBits);
		reliable.WriteUBitLong(messageType, RequiredStubMessageTypeBits);
		reliable.WriteUBitLong(fileID, RequiredStubFileIdBits);
		reliable.WriteBytes(compressed, static_cast<int>(compressedBytes));

		return !reliable.IsOverflowed() && reliable.GetNumBitsWritten() - bitsBefore ==
			static_cast<int>(wireBits);
	}

	constexpr std::size_t RequiredStubReliableCapacityBytes(std::size_t stubCount,
		std::size_t compressedBytes,
		std::size_t reserveBytes = RequiredStubReliableReserveBytes)
	{
		const std::size_t maximum = (std::numeric_limits<std::size_t>::max)();
		const std::size_t wireBits = RequiredStubWireBits(compressedBytes);
		if (wireBits == maximum || (stubCount != 0 && wireBits > (maximum - 7u) / stubCount))
			return maximum;

		const std::size_t burstBytes = (wireBits * stubCount + 7u) / 8u;
		return reserveBytes > maximum - burstBytes ? maximum : burstBytes + reserveBytes;
	}

	constexpr bool NeedsPerClientNativeHashes(Lane lane)
	{
		return lane == Lane::Required || lane == Lane::NativeRecovery ||
			lane == Lane::NativeOptOut || lane == Lane::NativeRescue;
	}

	constexpr bool ShouldPreserveRecoveryLifecycle(bool gameLayerCallback,
		bool nativeRecovery, bool active, bool recoveryStateCleared,
		bool identityMatches, bool ownsConsumedRecovery)
	{
		return gameLayerCallback && nativeRecovery && !active &&
			!recoveryStateCleared && identityMatches && ownsConsumedRecovery;
	}

	constexpr bool ShouldKeepInvokedRecoveryHandoff(bool gameLayerCallback,
		bool invoked)
	{
		return gameLayerCallback && invoked;
	}

	constexpr bool ShouldIgnoreLateRecoveryFailure(bool nativeRecovery,
		bool ownsConsumedRecovery, bool recoveryStateCleared)
	{
		// A native recovery baseline contains no stubs. A required/unready command
		// observed during its consumed or successfully-completed epoch can only belong
		// to the failed baseline.
		return nativeRecovery && (ownsConsumedRecovery || recoveryStateCleared);
	}

	constexpr double RecoveryHandoffWindow(double recoveryTtlSeconds)
	{
		if (recoveryTtlSeconds <= 0.0)
			return 0.0;
		const double halfTtl = recoveryTtlSeconds * 0.5;
		return halfTtl < 30.0 ? halfTtl : 30.0;
	}

	constexpr Action SelectBaseline(Lane lane, bool canonicalRegistration,
		BaseAvailability base)
	{
		return lane != Lane::Required ? Action::Native :
			(!canonicalRegistration || base != BaseAvailability::Ready ? Action::Reject : Action::CanonicalStub);
	}

	constexpr Action SelectFile(Lane lane, bool canonicalRegistration,
		BaseAvailability base, bool bootstrap, bool nativeDelta)
	{
		return lane != Lane::Required ? Action::Native :
			(!canonicalRegistration || base != BaseAvailability::Ready ? Action::Reject :
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

	template <typename Hash>
	inline bool NativeHashMatches(const std::unordered_map<int, Hash>& hashes,
		int fileID, const Hash& current)
	{
		auto known = hashes.find(fileID);
		return known != hashes.end() && known->second == current;
	}

	template <typename Hash>
	inline void RememberNativeHash(std::unordered_map<int, Hash>& hashes,
		int fileID, const Hash& current)
	{
		hashes[fileID] = current;
	}

	template <typename Hash>
	inline bool RestoreCanonicalHash(std::unordered_map<int, Hash>& hashes, int fileID)
	{
		return hashes.erase(fileID) != 0;
	}

	static_assert(SelectBaseline(Lane::Required, true, BaseAvailability::Ready) == Action::CanonicalStub,
		"a usable required base must select mixed canonical/native delivery");
	static_assert(SelectBaseline(Lane::Required, true, BaseAvailability::Missing) == Action::Reject,
		"a missing required base must fail closed");
	static_assert(SelectBaseline(Lane::Required, true, BaseAvailability::Unusable) == Action::Reject,
		"an unusable required base must fail closed");
	static_assert(SelectBaseline(Lane::NativeOptOut, true, BaseAvailability::Ready) == Action::Native,
		"the explicit opt-out lane must remain wholly native");
	static_assert(SelectBaseline(Lane::NativeRecovery, true, BaseAvailability::Ready) == Action::Native,
		"a consumed recovery latch must be wholly native from its baseline");
	static_assert(RejectUnavailableRequiredAdmission(true, true, false),
		"required mode must reject before a cached client can bypass an unavailable baseline hook");
	static_assert(!RejectUnavailableRequiredAdmission(true, true, true),
		"required admission is available when every canonical delivery hook is active");
	static_assert(!RejectUnavailableRequiredAdmission(true, false, false),
		"native rescue remains available when required mode is disabled");
	static_assert(UsesCanonicalRegistration(true, true),
		"enabled supported registration must use canonical LuaPack contents");
	static_assert(!UsesCanonicalRegistration(false, true),
		"the kill switch must restore native registration");
	static_assert(!UsesCanonicalRegistration(true, false),
		"unsupported registration must remain native");
	static_assert(UsesPassthroughProcessing(true),
		"LuaPack processing must preserve the same raw bytes captured for native and packed bodies");
	static_assert(!UsesPassthroughProcessing(false),
		"stock gmoddatapack token processing must resume when LuaPack is disabled");
	static_assert(!RegistrationModeMatches(true, true, false, true),
		"disabling LuaPack must invalidate a cached canonical body");
	static_assert(!RegistrationModeMatches(false, false, true, true),
		"enabling supported LuaPack must invalidate a cached native body");
	static_assert(!RegistrationModeMatches(false, false, true, false),
		"enabling native rescue must invalidate a token-processed native registration");
	static_assert(RegistrationNeedsHashPublication(true, true, true, true, true, false, true),
		"a stale canonical body must publish native identity before the kill switch can send it");
	static_assert(RegistrationNeedsHashPublication(true, true, true, false, false, true, true),
		"a matching mode is not observable until its replacement hash is published");
	static_assert(RegistrationNeedsHashPublication(true, true, true, true, false, true, true),
		"worker processing is not publication until the main thread updates the string table");
	static_assert(RegistrationNeedsHashPublication(false, false, false, true, true, false, true),
		"an uninitialized native cache entry must capture source before publishing or sending");
	static_assert(!RegistrationNeedsHashPublication(true, true, true, true, true, true, true),
		"a ready canonical body with a matching mode needs no transition repair");
	static_assert(NeedsOrderedCanonicalHash(true, false, false),
		"an active client must receive canonical identity before its restored stub body");
	static_assert(!NeedsOrderedCanonicalHash(false, false, false),
		"a joining client already received canonical identity in its ServerInfo baseline");
	static_assert(NeedsPerClientNativeHashes(Lane::NativeRecovery),
		"required recovery still needs per-client native hash identity");
	static_assert(NeedsPerClientNativeHashes(Lane::NativeOptOut),
		"native opt-out hot refreshes need per-client native hash identity");
	static_assert(NeedsPerClientNativeHashes(Lane::NativeRescue),
		"native rescue hot refreshes need per-client native hash identity");
	static_assert(SelectFile(Lane::Required, true, BaseAvailability::Ready, false, false) == Action::CanonicalStub,
		"an unchanged base file must use the canonical stub");
	static_assert(SelectFile(Lane::Required, true, BaseAvailability::Ready, false, true) == Action::Native,
		"a file changed from the base must use native delivery");
	static_assert(SelectFile(Lane::Required, true, BaseAvailability::Ready, true, false) == Action::Native,
		"the bootstrap must always use native delivery");
	static_assert(SelectFile(Lane::Required, true, BaseAvailability::Missing, false, true) == Action::Reject,
		"a native delta must never turn a missing required base into whole-join fallback");
	static_assert(SelectFile(Lane::Required, false, BaseAvailability::Ready, true, false) == Action::Reject,
		"required delivery must fail closed without the per-client baseline hook");
	static_assert(ShouldBuildMapBase(false, true), "the initial map base must be buildable");
	static_assert(!ShouldBuildMapBase(true, true), "hotfixes must not rotate the immutable map base");
}

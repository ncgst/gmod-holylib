#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace HolyLib::LuaPack::Policy
{
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

	// Canonical LuaPack placeholders are small, but thousands can be requested in one
	// frame. Keep a large part of the engine's reliable stream available to unrelated
	// signon traffic and pace both each client and the server as a whole. The envelope
	// allowance covers the GMod custom-message frame and an optional one-entry string-
	// table update; it is deliberately larger than their encoded wire size.
	constexpr std::size_t ReliableStubStreamReserveBytes = 64u * 1024u;
	constexpr std::size_t ReliableStubClientBudgetBytes = 16u * 1024u;
	constexpr std::size_t ReliableStubGlobalBudgetBytes = 128u * 1024u;
	constexpr std::size_t ReliableStubGlobalPacketBudget = 32u;
	constexpr std::size_t ReliableStubEnvelopeBytes = 32u;
	constexpr std::size_t ReliableStubOrderedHashBytes = 96u;
	constexpr double ReliableStubTransferTimeoutSeconds = 60.0;

	// The engine must own and finish one reliable batch before another batch uses
	// the same connection. Do not populate CNetChan's private fragment list through
	// the SDK mirror: that non-virtual layout is not an engine ownership boundary.
	constexpr bool CanBeginReliableStubBatch(bool channelUsable,
		bool streamOverflowed, bool transferPending, bool engineReliablePending)
	{
		return channelUsable && !streamOverflowed && !transferPending &&
			!engineReliablePending;
	}

	// HasPendingReliableData is virtual and therefore reports the engine's actual
	// scratch/fragment ownership. Once it clears, the previously committed batch is
	// acknowledged and the next bounded batch may be staged.
	constexpr bool ReliableStubEngineTransferComplete(bool transferPending,
		bool engineReliablePending)
	{
		return transferPending && !engineReliablePending;
	}

	constexpr bool CanPumpReliableStubEngineTransfer(bool channelUsable,
		bool streamOverflowed, bool transferPending, bool engineReliablePending,
		std::size_t globalPacketBudget)
	{
		return channelUsable && !streamOverflowed && transferPending &&
			engineReliablePending && globalPacketBudget != 0;
	}

	constexpr bool ReliableStubEngineTransferTimedOut(bool transferPending,
		double startedAt, double currentTime)
	{
		return transferPending && startedAt >= 0.0 && currentTime >=
			(startedAt + ReliableStubTransferTimeoutSeconds);
	}

	constexpr std::size_t ReliableStubStagingBytes(std::size_t compressedBytes,
		bool orderedCanonicalHash)
	{
		const std::size_t overhead = ReliableStubEnvelopeBytes +
			(orderedCanonicalHash ? ReliableStubOrderedHashBytes : 0u);
		return compressedBytes > (std::numeric_limits<std::size_t>::max)() - overhead
			? (std::numeric_limits<std::size_t>::max)()
			: compressedBytes + overhead;
	}

	constexpr bool CanStageReliableStub(bool streamOverflowed,
		std::size_t streamBytesLeft, std::size_t clientBudgetBytes,
		std::size_t globalBudgetBytes, std::size_t compressedBytes,
		bool orderedCanonicalHash)
	{
		if (streamOverflowed || streamBytesLeft < ReliableStubStreamReserveBytes)
			return false;
		const std::size_t stagedBytes = ReliableStubStagingBytes(
			compressedBytes, orderedCanonicalHash);
		return stagedBytes <= clientBudgetBytes && stagedBytes <= globalBudgetBytes &&
			stagedBytes <= streamBytesLeft - ReliableStubStreamReserveBytes;
	}

	constexpr bool MustDeferReliableStubForGlobalBudget(
		std::size_t globalBudgetBytes, std::size_t stagedBytes)
	{
		return stagedBytes > globalBudgetBytes;
	}

	constexpr bool CommitReliableStubBatch(bool wroteBatch,
		bool streamOverflowed, bool engineOwned)
	{
		return wroteBatch && !streamOverflowed && engineOwned;
	}

	constexpr bool HoldPreSpawnForLuaDelivery(bool legacyQueuePending,
		std::size_t canonicalStubQueueSize)
	{
		return legacyQueuePending || canonicalStubQueueSize != 0;
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

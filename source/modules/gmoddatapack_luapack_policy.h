#pragma once

#include <cstdint>
#include <cstring>
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

	constexpr bool RequiredBaselineIdentityReady(bool recoveryEnabled,
		bool resolvedIdentity)
	{
		return !recoveryEnabled || resolvedIdentity;
	}

	constexpr bool RequiredFailureIdentityReady(bool recoveryEnabled,
		bool resolvedIdentity, bool authenticatedIdentity)
	{
		return !recoveryEnabled || (resolvedIdentity && authenticatedIdentity);
	}

	constexpr bool NeedsPerClientNativeHashes(Lane lane)
	{
		return lane == Lane::Required || lane == Lane::NativeRecovery;
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
	static_assert(NeedsPerClientNativeHashes(Lane::NativeRecovery),
		"required recovery still needs per-client native hash identity");
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

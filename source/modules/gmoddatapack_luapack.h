#pragma once

#include "interface.h"
#include "public/imodule.h"

#include <string>

class CBaseClient;
class CCommand;
struct edict_t;

namespace GarrysMod::Lua
{
	class ILuaInterface;
	struct LuaFile;
}

namespace Bootil
{
	class AutoBuffer;
}

namespace HolyLib::LuaPack
{
	struct Config
	{
		bool enabled = false;
		std::string packDirectory;
		std::string downloadUrlPolicy;
		std::string ingestUrl;
		std::string ingestMethod;
		unsigned int downloadableLimit = 1;
		double generationRetentionSeconds = 300.0;
		double objectRetentionSeconds = 604800.0;
		double readyDeadlineSeconds = 30.0;
		bool requiredStubbing = false;
		bool allowOptOut = true;
		bool requiredRecovery = true;
		double requiredRecoveryTtlSeconds = 120.0;
		bool optimisticStubbing = false;
		unsigned int optimisticPrefixFiles = 256;
		unsigned long long optimisticPrefixBytes = 262144;
		double unreadyTtlSeconds = 900.0;
	};

	enum class DeliveryAction
	{
		Native,
		Stub,
		Reject,
	};

	enum class BaselineAction
	{
		Unchanged,
		BasePlusDelta,
		CanonicalStub,
		NativeSource,
		Reject,
	};

	struct BaselineDecision
	{
		BaselineDecision(BaselineAction baselineAction = BaselineAction::Unchanged,
			const char* rejectionFailure = nullptr)
			: action(baselineAction), failure(rejectionFailure) {}

		BaselineAction action;
		const char* failure;
	};

	struct DeliveryDecision
	{
		DeliveryDecision(DeliveryAction deliveryAction = DeliveryAction::Native,
			const Bootil::AutoBuffer* payload = nullptr, const char* rejectionFailure = nullptr)
			: action(deliveryAction), compressed(payload), failure(rejectionFailure) {}

		DeliveryAction action;
		const Bootil::AutoBuffer* compressed;
		const char* failure;
	};

	const Config& GetConfig();
	bool IsEnabled();
	bool SupportsCanonicalRegistration();
	bool IsInitFile(const std::string& virtualPath);

	void Init(CreateInterfaceFn* appfn);
	void Shutdown();
	void LevelShutdown();
	void Think();
	void LuaInit(GarrysMod::Lua::ILuaInterface* pLua, bool bServerInit);

	void CaptureFile(const GarrysMod::Lua::LuaFile* file);
	std::string PrepareVanillaFile(const std::string& virtualPath, const std::string& contents);
	bool ConsumeBootstrapRefresh();
	BaselineDecision DecideBaselineForClient(int slot);
	BaselineDecision DecideFileBaselineForClient(int slot, const std::string& virtualPath);
	bool NeedsNativeHashUpdate(int slot);
	DeliveryDecision DecideDeliveryForClient(int slot, const std::string& virtualPath, size_t nativeSourceBytes);
	void DisconnectRequiredClient(int slot, const char* failure);
	bool ClientConnect(int slot);
	void ClientActive(int slot);
	bool ClientDisconnect(int slot, bool gameLayerCallback = true);
	MODULE_RESULT ClientCommand(int slot, const CCommand* args);
}

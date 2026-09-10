#pragma once

#include <string>

namespace HolyLib::AutoRefresh
{
	// Shared by the optional autorefresh module and the LuaPack datapack owner so
	// only one detour ever owns GarrysMod::AutoRefresh::HandleChange_Lua.
	bool RunPreLuaChange(const std::string* fileRelPath,
		const std::string* fileName, const std::string* fileExt);
	void RunPostLuaChange(const std::string* fileRelPath,
		const std::string* fileName, const std::string* fileExt);
}

#if defined(MODULE_EXISTS_GMODDATAPACK)
namespace HolyLib::GModDataPack
{
	void InstallLuaAutoRefreshDetour();
}
#endif

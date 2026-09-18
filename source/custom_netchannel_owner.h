#pragma once

class CNetChan;

// These entry points belong only to HolyLib's local CNetChan implementation.
// No field or vtable is added to the engine's CNetChan layout.
bool GameServer_InterceptOwnedNetChannelShutdown(CNetChan* channel, const char* reason);
const char* GameServer_BeginOwnedNetChannelDestruction(CNetChan* channel);
bool GameServer_IsOwnedNetChannelDestructing(CNetChan* channel);
void GameServer_EndOwnedNetChannelDestruction(CNetChan* channel);

// Kept outside enabled-module dispatch: a callback may disable gameserver while
// its channel close is waiting for the native packet/message frame to return.
void GameServer_DrainNetChannelRetirements();

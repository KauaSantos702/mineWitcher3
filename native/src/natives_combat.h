#pragma once

namespace mw3
{
	// Natives da v6: combate (MW3_Actor*, MW3_PollHit/MW3_Hit*, MW3_SendPlayerHurt, MW3_CombatLinked).
	// Chamado por RegisterNatives (natives.cpp). Script: mw3_combat.ws.
	bool RegisterCombatNatives();
} // namespace mw3

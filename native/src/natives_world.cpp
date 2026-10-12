// Natives da v4: colisao Stage A (mw3_collision.ws) e fantoche (mw3_puppet.ws).
// Os imports correspondentes ficam em mod/modMineWitcher3/content/scripts/local/mw3_native.ws.
#include "natives_world.h"

#include "collision.h"
#include "log.h"
#include "session.h"

#include <WitcherNativeBridgeApi.h>

#include <string>

namespace mw3
{
	namespace
	{
		// ---- colisao ---------------------------------------------------------------------------
		// import function MW3_ColBegin( x : float, y : float, z : float ) : bool;  pes do Geralt (W3)
		void ColBeginNative(void*, void* frame, void* result)
		{
			const float x = WNB_ReadFloatParameter(frame);
			const float y = WNB_ReadFloatParameter(frame);
			const float z = WNB_ReadFloatParameter(frame);
			WNB_ReturnBool(frame, result, collision::Begin(x, y, z));
		}

		// import function MW3_ColNext() : int;  0 nada, 1 coluna pronta, 2 coluna + nivel da agua
		void ColNextNative(void*, void* frame, void* result)
		{
			WNB_ReturnInt(frame, result, collision::Next());
		}

		void ColXNative(void*, void* frame, void* result)
		{
			WNB_ReturnFloat(frame, result, collision::PendingX());
		}

		void ColYNative(void*, void* frame, void* result)
		{
			WNB_ReturnFloat(frame, result, collision::PendingY());
		}

		void ColTopNative(void*, void* frame, void* result)
		{
			WNB_ReturnFloat(frame, result, collision::PendingTop());
		}

		void ColBottomNative(void*, void* frame, void* result)
		{
			WNB_ReturnFloat(frame, result, collision::PendingBottom());
		}

		// import function MW3_ColHitDown( z : float );
		void ColHitDownNative(void*, void* frame, void*)
		{
			collision::HitDown(WNB_ReadFloatParameter(frame));
			WNB_ReturnVoid(frame);
		}

		// import function MW3_ColHitUp( z : float );
		void ColHitUpNative(void*, void* frame, void*)
		{
			collision::HitUp(WNB_ReadFloatParameter(frame));
			WNB_ReturnVoid(frame);
		}

		// import function MW3_ColCommit( water : float );  < -1000000 = sem agua
		void ColCommitNative(void*, void* frame, void*)
		{
			collision::Commit(WNB_ReadFloatParameter(frame));
			WNB_ReturnVoid(frame);
		}

		// import function MW3_ColBudget() : int;
		void ColBudgetNative(void*, void* frame, void* result)
		{
			WNB_ReturnInt(frame, result, collision::TraceBudget());
		}

		// ---- fantoche (depois de MW3_LinkConnected no mesmo frame) ------------------------------
		// import function MW3_PuppetActive() : bool;
		void PuppetActiveNative(void*, void* frame, void* result)
		{
			const auto& s = session::CurrentSnapshot();
			WNB_ReturnBool(frame, result, s.linked && s.puppet);
		}

		void McFeetXNative(void*, void* frame, void* result)
		{
			WNB_ReturnFloat(frame, result, static_cast<float>(session::CurrentSnapshot().feetX));
		}

		void McFeetYNative(void*, void* frame, void* result)
		{
			WNB_ReturnFloat(frame, result, static_cast<float>(session::CurrentSnapshot().feetY));
		}

		void McFeetZNative(void*, void* frame, void* result)
		{
			WNB_ReturnFloat(frame, result, static_cast<float>(session::CurrentSnapshot().feetZ));
		}

		void McVelXNative(void*, void* frame, void* result)
		{
			WNB_ReturnFloat(frame, result, static_cast<float>(session::CurrentSnapshot().velX));
		}

		void McVelYNative(void*, void* frame, void* result)
		{
			WNB_ReturnFloat(frame, result, static_cast<float>(session::CurrentSnapshot().velY));
		}

		void McVelZNative(void*, void* frame, void* result)
		{
			WNB_ReturnFloat(frame, result, static_cast<float>(session::CurrentSnapshot().velZ));
		}

		// import function MW3_McFlags() : int;  4 no chao, 8 agachado, 16 correndo, 32 morto, 64 nadando, 128 voando
		void McFlagsNative(void*, void* frame, void* result)
		{
			WNB_ReturnInt(frame, result, static_cast<int>(session::CurrentSnapshot().mcFlags));
		}

		// import function MW3_PuppetMoved( x : float, y : float, z : float );  onde o script pos o Geralt
		void PuppetMovedNative(void*, void* frame, void*)
		{
			const float x = WNB_ReadFloatParameter(frame);
			const float y = WNB_ReadFloatParameter(frame);
			const float z = WNB_ReadFloatParameter(frame);
			session::NotePuppetTarget(x, y, z);
			WNB_ReturnVoid(frame);
		}

		// ---- v5 ---------------------------------------------------------------------------------
		// import function MW3_PollMcDeath() : bool;  true uma vez por morte do jogador do MC (kEvPlayerDied)
		void PollMcDeathNative(void*, void* frame, void* result)
		{
			WNB_ReturnBool(frame, result, session::ConsumeMcDeath());
		}

		// import function MW3_RequestResync();  fantoche bloqueado: teleporta o MC para o Geralt
		void RequestResyncNative(void*, void* frame, void*)
		{
			session::RequestResync();
			WNB_ReturnVoid(frame);
		}

		struct Registration
		{
			const char*              name;
			WNB_NativeImplementation impl;
		};
	} // namespace

	bool RegisterWorldNatives()
	{
		const Registration registrations[] = {
		    { "MW3_ColBegin", &ColBeginNative },
		    { "MW3_ColNext", &ColNextNative },
		    { "MW3_ColX", &ColXNative },
		    { "MW3_ColY", &ColYNative },
		    { "MW3_ColTop", &ColTopNative },
		    { "MW3_ColBottom", &ColBottomNative },
		    { "MW3_ColHitDown", &ColHitDownNative },
		    { "MW3_ColHitUp", &ColHitUpNative },
		    { "MW3_ColCommit", &ColCommitNative },
		    { "MW3_ColBudget", &ColBudgetNative },
		    { "MW3_PuppetActive", &PuppetActiveNative },
		    { "MW3_McFeetX", &McFeetXNative },
		    { "MW3_McFeetY", &McFeetYNative },
		    { "MW3_McFeetZ", &McFeetZNative },
		    { "MW3_McVelX", &McVelXNative },
		    { "MW3_McVelY", &McVelYNative },
		    { "MW3_McVelZ", &McVelZNative },
		    { "MW3_McFlags", &McFlagsNative },
		    { "MW3_PuppetMoved", &PuppetMovedNative },
		    { "MW3_PollMcDeath", &PollMcDeathNative },
		    { "MW3_RequestResync", &RequestResyncNative },
		};

		bool ok = true;
		for (const Registration& r : registrations)
		{
			if (WNB_RegisterNative(r.name, r.impl))
			{
				Log(std::string("native enfileirada: ") + r.name);
			}
			else
			{
				Log(std::string("ERRO ao enfileirar native: ") + r.name);
				ok = false;
			}
		}
		return ok;
	}
} // namespace mw3

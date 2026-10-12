// Natives da v6: combate W3 <-> MC (mw3_combat.ws). Logica em combat.cpp; os imports ficam em
// mod/modMineWitcher3/content/scripts/local/mw3_native.ws.
#include "natives_combat.h"

#include "combat.h"
#include "log.h"
#include "session.h"

#include <WitcherNativeBridgeApi.h>
#include <windows.h>

#include <climits>
#include <string>

namespace mw3
{
	namespace
	{
		std::string ReadUtf8(void* frame)
		{
			WNB_String input{};
			if (WNB_ReadStringParameter(frame, &input) <= 0 || !input.data || input.size <= 1 || input.size > INT_MAX)
				return {};
			return std::string(reinterpret_cast<const char*>(input.data), input.size - 1);
		}

		// Ultimo golpe tirado por MW3_PollHit (os getters MW3_Hit* devolvem os campos dele).
		combat::Hit g_hit;

		// import function MW3_CombatLinked() : bool;  MC vivo e no mundo (sem isso o combate fica inerte)
		void CombatLinkedNative(void*, void* frame, void* result)
		{
			WNB_ReturnBool(frame, result, session::Linked());
		}

		// import function MW3_ActorsBegin();
		void ActorsBeginNative(void*, void* frame, void*)
		{
			combat::BeginActors();
			WNB_ReturnVoid(frame);
		}

		// import function MW3_ActorAdd( id : int, x : float, y : float, z : float, yaw : float, radius : float,
		//     height : float, health : float, maxHealth : float, flags : int, level : int, displayName : string );
		void ActorAddNative(void*, void* frame, void*)
		{
			combat::ActorIn in;
			in.id = static_cast<std::uint32_t>(WNB_ReadIntParameter(frame));
			in.x = WNB_ReadFloatParameter(frame);
			in.y = WNB_ReadFloatParameter(frame);
			in.z = WNB_ReadFloatParameter(frame);
			in.w3Yaw = WNB_ReadFloatParameter(frame);
			in.radius = WNB_ReadFloatParameter(frame);
			in.height = WNB_ReadFloatParameter(frame);
			in.health = WNB_ReadFloatParameter(frame);
			in.maxHealth = WNB_ReadFloatParameter(frame);
			in.flags = static_cast<std::uint32_t>(WNB_ReadIntParameter(frame));
			in.level = WNB_ReadIntParameter(frame);
			in.name = ReadUtf8(frame);
			combat::AddActor(in);
			WNB_ReturnVoid(frame);
		}

		// import function MW3_ActorsCommit() : int;  escreve a tabela (so com o MC linkado; senao esvazia)
		void ActorsCommitNative(void*, void* frame, void* result)
		{
			int n = 0;
			if (session::Linked())
				n = static_cast<int>(combat::CommitActors(Link::Get()));
			else
				combat::ClearActors(Link::Get());
			WNB_ReturnInt(frame, result, n);
		}

		// import function MW3_ActorsClear();
		void ActorsClearNative(void*, void* frame, void*)
		{
			combat::ClearActors(Link::Get());
			WNB_ReturnVoid(frame);
		}

		// import function MW3_PollHit() : int;  actorId do proximo golpe do MC (0 = nenhum); depois MW3_Hit*
		void PollHitNative(void*, void* frame, void* result)
		{
			int id = 0;
			if (!session::Linked())
			{
				combat::ClearHits();
			}
			else if (combat::PopHit(g_hit, GetTickCount64()))
			{
				id = static_cast<int>(g_hit.actorId);
			}
			WNB_ReturnInt(frame, result, id);
		}

		void HitDamageNative(void*, void* frame, void* result)
		{
			WNB_ReturnFloat(frame, result, g_hit.mcDamage);
		}

		void HitFlagsNative(void*, void* frame, void* result)
		{
			WNB_ReturnInt(frame, result, static_cast<int>(g_hit.flags));
		}

		void HitWeaponNative(void*, void* frame, void* result)
		{
			WNB_ReturnInt(frame, result, static_cast<int>(g_hit.weapon));
		}

		void HitPushNative(void*, void* frame, void* result)
		{
			WNB_ReturnFloat(frame, result, g_hit.push);
		}

		void HitPushXNative(void*, void* frame, void* result)
		{
			WNB_ReturnFloat(frame, result, g_hit.pushX);
		}

		void HitPushYNative(void*, void* frame, void* result)
		{
			WNB_ReturnFloat(frame, result, g_hit.pushY);
		}

		// import function MW3_SendPlayerHurt( kind : int, mcDamage : float, attackerId : int, flags : int ) : bool;
		// mcDamage em pontos de vida do MC (2 = um coracao). false: MC nao linkado, nada enviado.
		void SendPlayerHurtNative(void*, void* frame, void* result)
		{
			const int   kind = WNB_ReadIntParameter(frame);
			const float damage = WNB_ReadFloatParameter(frame);
			const int   attacker = WNB_ReadIntParameter(frame);
			const int   flags = WNB_ReadIntParameter(frame);
			bool        sent = false;
			if (session::Linked())
				sent = combat::SendPlayerHurt(Link::Get(), static_cast<std::uint32_t>(kind < 0 ? 0 : kind), damage, static_cast<std::uint32_t>(attacker),
				                              static_cast<std::uint32_t>(flags));
			WNB_ReturnBool(frame, result, sent);
		}

		struct Registration
		{
			const char*              name;
			WNB_NativeImplementation impl;
		};
	} // namespace

	bool RegisterCombatNatives()
	{
		const Registration registrations[] = {
		    { "MW3_CombatLinked", &CombatLinkedNative },
		    { "MW3_ActorsBegin", &ActorsBeginNative },
		    { "MW3_ActorAdd", &ActorAddNative },
		    { "MW3_ActorsCommit", &ActorsCommitNative },
		    { "MW3_ActorsClear", &ActorsClearNative },
		    { "MW3_PollHit", &PollHitNative },
		    { "MW3_HitDamage", &HitDamageNative },
		    { "MW3_HitFlags", &HitFlagsNative },
		    { "MW3_HitWeapon", &HitWeaponNative },
		    { "MW3_HitPush", &HitPushNative },
		    { "MW3_HitPushX", &HitPushXNative },
		    { "MW3_HitPushY", &HitPushYNative },
		    { "MW3_SendPlayerHurt", &SendPlayerHurtNative },
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

#include "natives.h"

#include "input_hook.h"
#include "log.h"
#include "natives_combat.h"
#include "natives_world.h"
#include "session.h"

#include <WitcherNativeBridgeApi.h>
#include <windows.h>

#include <climits>
#include <string>

namespace mw3
{
	namespace
	{
		// Strings do script chegam em UTF-8 (size inclui o NUL final), ver o exemplo Remastered do WNB.
		std::string ReadUtf8String(void* frame)
		{
			WNB_String input{};
			if (WNB_ReadStringParameter(frame, &input) <= 0 || !input.data || input.size <= 1 || input.size > INT_MAX)
				return {};

			return std::string(reinterpret_cast<const char*>(input.data), input.size - 1);
		}

		// import function MW3_Ping() : int;
		void PingNative(void*, void* frame, void* result)
		{
			static int calls = 0;
			++calls;
			Log("MW3_Ping chamado pelo script (" + std::to_string(calls) + "x)");
			WNB_ReturnInt(frame, result, kNativeVersion);
		}

		// import function MW3_Log(msg : string);
		void LogNative(void*, void* frame, void*)
		{
			const std::string msg = ReadUtf8String(frame);
			Log("[script] " + msg);
			WNB_ReturnVoid(frame);
		}

		// import function MW3_PollCameraKey() : int;
		// F5 e engolido pela WndProc (o motor usa F5 fixo para quicksave); o script consome aqui.
		void PollCameraKeyNative(void*, void* frame, void* result)
		{
			WNB_ReturnInt(frame, result, ConsumeCameraKeyPresses());
		}

		// ---- link com o Minecraft (v3) ------------------------------------------------------------
		// O script chama MW3_LinkConnected() uma vez por frame ANTES dos getters MW3_Mc*: ela le o
		// McState e congela o snapshot que os getters devolvem (todos do mesmo frame do MC).

		// import function MW3_LinkConnected() : bool;
		void LinkConnectedNative(void*, void* frame, void* result)
		{
			WNB_ReturnBool(frame, result, session::RefreshSnapshot().linked);
		}

		// import function MW3_InputRouted() : bool;   teclado/mouse indo para o MC agora (o W3 nao os ve)
		void InputRoutedNative(void*, void* frame, void* result)
		{
			WNB_ReturnBool(frame, result, session::RouteInput());
		}

		// import function MW3_McEyeX() : float;  (e Y, Z) olho do jogador do MC, ja em espaco W3 (metros, Z para cima)
		void McEyeXNative(void*, void* frame, void* result)
		{
			WNB_ReturnFloat(frame, result, static_cast<float>(session::CurrentSnapshot().eyeX));
		}

		void McEyeYNative(void*, void* frame, void* result)
		{
			WNB_ReturnFloat(frame, result, static_cast<float>(session::CurrentSnapshot().eyeY));
		}

		void McEyeZNative(void*, void* frame, void* result)
		{
			WNB_ReturnFloat(frame, result, static_cast<float>(session::CurrentSnapshot().eyeZ));
		}

		// import function MW3_McYaw() : float;    EulerAngles.Yaw do W3 (ver McYawToW3Yaw em link.h: sinal a confirmar)
		void McYawNative(void*, void* frame, void* result)
		{
			WNB_ReturnFloat(frame, result, session::CurrentSnapshot().yaw);
		}

		// import function MW3_McPitch() : float;  EulerAngles.Pitch do W3 (positivo = para cima, a confirmar)
		void McPitchNative(void*, void* frame, void* result)
		{
			WNB_ReturnFloat(frame, result, session::CurrentSnapshot().pitch);
		}

		// import function MW3_McCameraMode() : int;  0 1a pessoa, 1 3a por tras, 2 3a de frente (F5 do MC)
		void McCameraModeNative(void*, void* frame, void* result)
		{
			WNB_ReturnInt(frame, result, session::CurrentSnapshot().cameraMode);
		}

		// import function MW3_McCameraDistance() : float;  distancia olho -> camera do MC (ja com o zoom colidido)
		void McCameraDistanceNative(void*, void* frame, void* result)
		{
			WNB_ReturnFloat(frame, result, session::CurrentSnapshot().cameraDistance);
		}

		// import function MW3_McFov() : float;  FOV vertical efetivo do MC (graus)
		void McFovNative(void*, void* frame, void* result)
		{
			WNB_ReturnFloat(frame, result, session::CurrentSnapshot().fov);
		}

		// import function MW3_SendGameState( inGame : bool, menuOpen : bool, loading : bool, gameHour : float );
		// Chamar antes de MW3_SendPlayerState no mesmo frame.
		void SendGameStateNative(void*, void* frame, void*)
		{
			const bool  inGame = WNB_ReadBoolParameter(frame);
			const bool  menuOpen = WNB_ReadBoolParameter(frame);
			const bool  loading = WNB_ReadBoolParameter(frame);
			const float gameHour = WNB_ReadFloatParameter(frame);
			session::SetGameState(inGame, menuOpen, loading, gameHour);
			WNB_ReturnVoid(frame);
		}

		// import function MW3_SendWorld( depotPath : string );  theGame.GetWorld().GetDepotPath()
		void SendWorldNative(void*, void* frame, void*)
		{
			const std::string path = ReadUtf8String(frame);
			session::SetWorld(path.c_str());
			WNB_ReturnVoid(frame);
		}

		// import function MW3_SendPlayerState( x : float, y : float, z : float, yaw : float );
		// Pes do Geralt em espaco W3 + heading (EulerAngles.Yaw). Uma vez por frame: escreve o GameState
		// (o MC faz pacing pelo seq dele).
		void SendPlayerStateNative(void*, void* frame, void*)
		{
			const float x = WNB_ReadFloatParameter(frame);
			const float y = WNB_ReadFloatParameter(frame);
			const float z = WNB_ReadFloatParameter(frame);
			const float yaw = WNB_ReadFloatParameter(frame);
			session::SetPlayerState(x, y, z, yaw);
			WNB_ReturnVoid(frame);
		}

		struct Registration
		{
			const char* name;
			WNB_NativeImplementation impl;
		};
	} // namespace

	bool RegisterNatives()
	{
		const uint32_t api = WNB_GetApiVersion();
		Log("WitcherNativeBridge API v" + std::to_string(api));
		if (api < WNB_API_VERSION)
		{
			Log("ERRO: WitcherNativeBridge antigo demais (precisa v" + std::to_string(WNB_API_VERSION) + ")");
			return false;
		}

		const Registration registrations[] = {
		    {"MW3_Ping", &PingNative},
		    {"MW3_Log", &LogNative},
		    {"MW3_PollCameraKey", &PollCameraKeyNative},
		    {"MW3_LinkConnected", &LinkConnectedNative},
		    {"MW3_InputRouted", &InputRoutedNative},
		    {"MW3_McEyeX", &McEyeXNative},
		    {"MW3_McEyeY", &McEyeYNative},
		    {"MW3_McEyeZ", &McEyeZNative},
		    {"MW3_McYaw", &McYawNative},
		    {"MW3_McPitch", &McPitchNative},
		    {"MW3_McCameraMode", &McCameraModeNative},
		    {"MW3_McCameraDistance", &McCameraDistanceNative},
		    {"MW3_McFov", &McFovNative},
		    {"MW3_SendGameState", &SendGameStateNative},
		    {"MW3_SendWorld", &SendWorldNative},
		    {"MW3_SendPlayerState", &SendPlayerStateNative},
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
		// v4: colisao Stage A e fantoche (natives_world.cpp).
		ok = RegisterWorldNatives() && ok;
		// v6: combate (natives_combat.cpp).
		ok = RegisterCombatNatives() && ok;
		return ok;
	}
} // namespace mw3

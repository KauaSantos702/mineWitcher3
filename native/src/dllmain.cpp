#include "input_hook.h"
#include "launcher.h"
#include "log.h"
#include "natives.h"
#include "overlay.h"
#include "session.h"

#include <windows.h>

namespace
{
	DWORD WINAPI Initialize(LPVOID)
	{
		mw3::InitLog();
		mw3::Log("mw3.asi carregado (native v" + std::to_string(mw3::kNativeVersion) + ")");

		if (!mw3::RegisterNatives())
			mw3::Log("ERRO: uma ou mais natives falharam; os scripts do mod nao vao compilar");

		// Memoria compartilhada + thread do link antes do hook de input (a WndProc consulta o link).
		mw3::session::Start();
		mw3::StartInputHook();
		mw3::launcher::StartMinecraft();
		// Mao/HUD/telas do MC sobre o back buffer (hook de Present; espera a janela do jogo numa thread).
		mw3::overlay::StartForGame();

		return 0;
	}
} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		DisableThreadLibraryCalls(module);

		// Mesmo padrao do exemplo do WitcherNativeBridge: nada pesado sob o loader lock.
		HANDLE thread = CreateThread(nullptr, 0, Initialize, nullptr, 0, nullptr);
		if (thread)
			CloseHandle(thread);
	}

	return TRUE;
}

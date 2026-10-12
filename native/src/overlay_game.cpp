// mw3.asi side of the overlay: reads [Overlay] from mw3.ini, gates drawing on the link state and
// installs the Present hooks once the game window exists. Kept apart from overlay.cpp so the
// standalone test (test/overlaytest.cpp) links the renderer without the session/input code.
#include "config.h"
#include "log.h"
#include "overlay.h"
#include "session.h"

#include <windows.h>

namespace mw3::overlay
{
	namespace
	{
		bool g_forwardDisabled = false;

		// MC vivo e no mundo, e (com bForwardInput=1) o W3 em gameplay: sem menu, cena ou loading do
		// W3 por cima (o mesmo criterio que manda o input ao MC). O frame do MC na tela de titulo ou
		// num mundo carregando nao aparece.
		bool ShouldDraw()
		{
			return session::Linked() && (g_forwardDisabled || session::RouteInput());
		}

		BOOL CALLBACK FindGameWindow(HWND hwnd, LPARAM param)
		{
			DWORD pid = 0;
			GetWindowThreadProcessId(hwnd, &pid);
			RECT rc{};
			if (pid == GetCurrentProcessId() && IsWindowVisible(hwnd) && GetClientRect(hwnd, &rc) && rc.right > 0 && rc.bottom > 0)
			{
				*reinterpret_cast<bool*>(param) = true;
				return FALSE;
			}
			return TRUE;
		}

		DWORD WINAPI InstallThread(LPVOID)
		{
			// Espera a janela do jogo: a essa altura o jogo ja criou o device dele (sem um device nosso
			// existindo durante D3D12EnableExperimentalFeatures/debug layer/slInit). As vtables valem
			// para swapchains ja criadas, entao instalar depois nao perde nada (fora um SetColorSpace1
			// anterior, que ChooseColorMode cobre pelo monitor).
			for (int i = 0; i < 1800; ++i)  // ate ~3 min
			{
				bool found = false;
				EnumWindows(FindGameWindow, reinterpret_cast<LPARAM>(&found));
				if (found)
					break;
				Sleep(100);
			}
			Options options;
			options.shouldDraw = ShouldDraw;
			options.hdrPaperWhiteNits = static_cast<float>(config::OverlayPaperWhiteNits());
			Install(options);
			return 0;
		}
	} // namespace

	void StartForGame()
	{
		if (!config::OverlayEnabled())
		{
			Log("mw3.ini: [Overlay] bEnable=0, sem overlay (mao/HUD/telas do MC) e heartbeat so pela thread do link");
			return;
		}
		g_forwardDisabled = config::GetInt(L"Input", L"bForwardInput", 1) == 0;
		HANDLE thread = CreateThread(nullptr, 0, InstallThread, nullptr, 0, nullptr);
		if (thread)
			CloseHandle(thread);
	}
} // namespace mw3::overlay

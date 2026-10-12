// Derived from SkyCraft (https://github.com/chasmlol/skycraft), Copyright (c) 2026 chasmlol, MIT License.
// The DirectInput -> SDL scancode table and the key routing (which keys stay with the game, O opens
// Minecraft's menu, everything goes to Minecraft while one of its screens is open) come from
// skse/src/Input.cpp (upstream commit bfcaf17). See THIRD-PARTY-NOTICES.md.
#include "input_hook.h"

#include "link.h"
#include "log.h"
#include "session.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <string>

namespace mw3
{
	namespace
	{
		constexpr wchar_t kViewportClass[] = L"W2ViewportClass";
		constexpr UINT    kCameraKey = VK_F5;

		// Codigo "DIK" = scancode set 1 do Windows, com 0x80 somado nas teclas estendidas (prefixo E0).
		// E exatamente o que o DirectInput reporta, entao a tabela do SkyCraft vale sem mudanca.
		// DIK -> scancode SDL / uso USB HID (o que o MC 26.3 usa, SDL3).
		constexpr auto kDikToSdl = [] {
			std::array<std::uint16_t, 256> t{};
			t[0x01] = 41;  // Esc
			for (int i = 0; i < 9; ++i)
				t[0x02 + i] = static_cast<std::uint16_t>(30 + i);  // 1-9
			t[0x0B] = 39;                                         // 0
			t[0x0C] = 45, t[0x0D] = 46, t[0x0E] = 42, t[0x0F] = 43;  // - = Backspace Tab
			t[0x10] = 20, t[0x11] = 26, t[0x12] = 8, t[0x13] = 21, t[0x14] = 23;  // Q W E R T
			t[0x15] = 28, t[0x16] = 24, t[0x17] = 12, t[0x18] = 18, t[0x19] = 19;  // Y U I O P
			t[0x1A] = 47, t[0x1B] = 48, t[0x1C] = 40, t[0x1D] = 224;               // [ ] Enter LCtrl
			t[0x1E] = 4, t[0x1F] = 22, t[0x20] = 7, t[0x21] = 9, t[0x22] = 10;     // A S D F G
			t[0x23] = 11, t[0x24] = 13, t[0x25] = 14, t[0x26] = 15;                // H J K L
			t[0x27] = 51, t[0x28] = 52, t[0x29] = 53, t[0x2A] = 225, t[0x2B] = 49;  // ; ' ` LShift backslash
			t[0x2C] = 29, t[0x2D] = 27, t[0x2E] = 6, t[0x2F] = 25, t[0x30] = 5;    // Z X C V B
			t[0x31] = 17, t[0x32] = 16, t[0x33] = 54, t[0x34] = 55, t[0x35] = 56;  // N M , . /
			t[0x36] = 229, t[0x37] = 85, t[0x38] = 226, t[0x39] = 44, t[0x3A] = 57;  // RShift KP* LAlt Space Caps
			for (int i = 0; i < 10; ++i)
				t[0x3B + i] = static_cast<std::uint16_t>(58 + i);  // F1-F10
			t[0x45] = 83, t[0x46] = 71;                             // NumLock ScrollLock
			t[0x47] = 95, t[0x48] = 96, t[0x49] = 97, t[0x4A] = 86;  // KP7 KP8 KP9 KP-
			t[0x4B] = 92, t[0x4C] = 93, t[0x4D] = 94, t[0x4E] = 87;  // KP4 KP5 KP6 KP+
			t[0x4F] = 89, t[0x50] = 90, t[0x51] = 91, t[0x52] = 98, t[0x53] = 99;  // KP1 KP2 KP3 KP0 KP.
			t[0x56] = 100, t[0x57] = 68, t[0x58] = 69;                             // OEM102 F11 F12
			t[0x9C] = 88, t[0x9D] = 228, t[0xB5] = 84, t[0xB7] = 70, t[0xB8] = 230;  // KPEnter RCtrl KP/ PrtSc RAlt
			t[0xC5] = 72, t[0xC7] = 74, t[0xC8] = 82, t[0xC9] = 75, t[0xCB] = 80;  // Pause Home Up PgUp Left
			t[0xCD] = 79, t[0xCF] = 77, t[0xD0] = 81, t[0xD1] = 78, t[0xD2] = 73;  // Right End Down PgDn Insert
			t[0xD3] = 76, t[0xDB] = 227, t[0xDC] = 231, t[0xDD] = 101;             // Delete LWin RWin Menu
			return t;
		}();
		static_assert(kDikToSdl[0x3F] == 62, "F5 precisa virar SDL_SCANCODE_F5");

		// Teclas que o W3 mantem enquanto o MC controla (fora de telas do MC). O resto e do MC.
		constexpr std::uint32_t kDikEscape = 0x01;   // menu do W3 (ou fecha uma tela do MC)
		constexpr std::uint32_t kDikConsole = 0x29;  // console do W3 (`/~)
		constexpr std::uint32_t kDikI = 0x17;        // inventario do W3 (o do MC e o E)
		constexpr std::uint32_t kDikJ = 0x24;        // diario
		constexpr std::uint32_t kDikK = 0x25;        // personagem
		constexpr std::uint32_t kDikM = 0x32;        // mapa
		constexpr std::uint32_t kDikO = 0x18;        // menu de pausa/opcoes do MC (o Esc e do W3)
		constexpr std::uint32_t kDikF4 = 0x3E;       // Alt+F4 continua fechando o jogo
		constexpr std::uint32_t kDikF5 = 0x3F;       // camera: sempre engolido (quicksave fixo do motor)
		constexpr std::uint32_t kDikF6 = 0x40;       // F6-F9: atalhos do MW3 / quickload
		constexpr std::uint32_t kDikF9 = 0x43;

		bool IsGameMenuKey(std::uint32_t dik)
		{
			return dik == kDikEscape || dik == kDikConsole || dik == kDikI || dik == kDikJ || dik == kDikK || dik == kDikM ||
			       (dik >= kDikF6 && dik <= kDikF9);
		}

		bool AltDown()
		{
			return (GetKeyState(VK_MENU) & 0x8000) != 0;
		}

		std::atomic<HWND> g_window{ nullptr };
		WNDPROC           g_originalProc = nullptr;
		std::atomic<int>  g_cameraPresses{ 0 };
		bool              g_openMenuHeld = false;
		wchar_t           g_highSurrogate = 0;

		bool ReadRaw(LPARAM lParam, RAWINPUT& raw)
		{
			UINT size = sizeof(raw);
			return GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) != static_cast<UINT>(-1);
		}

		std::uint32_t RawToDik(const RAWKEYBOARD& kb)
		{
			if (kb.MakeCode == KEYBOARD_OVERRUN_MAKE_CODE || kb.VKey == 0xFF)
				return 0;
			if (kb.Flags & RI_KEY_E1)
				return kb.VKey == VK_PAUSE ? 0xC5u : 0u;
			return (kb.MakeCode & 0x7Fu) | ((kb.Flags & RI_KEY_E0) ? 0x80u : 0u);
		}

		std::uint32_t LegacyToDik(LPARAM lParam)
		{
			const auto scan = static_cast<std::uint32_t>((lParam >> 16) & 0x7F);
			const bool extended = (lParam & (1LL << 24)) != 0;
			return scan | (extended ? 0x80u : 0u);
		}

		void PushKey(std::uint32_t dik, bool down)
		{
			if (const auto sdl = kDikToSdl[dik & 0xFF])
				Link::Get().PushInput(proto::kInKey, sdl, down ? 1 : 0);
		}

		// WM_INPUT de teclado com o input indo para o MC. true = engolir (o W3 nao ve).
		bool RouteRawKey(const RAWKEYBOARD& kb)
		{
			const std::uint32_t dik = RawToDik(kb);
			if (!dik)
				return true;
			const bool down = (kb.Flags & RI_KEY_BREAK) == 0;

			// Com uma tela do MC aberta (chat, inventario, opcoes) toda tecla e do MC: digitar funciona e
			// o Esc fecha a tela.
			if (!session::McScreenOpen())
			{
				if (IsGameMenuKey(dik) || (dik == kDikF4 && AltDown()))
					return false;  // so o W3
				if (dik == kDikO)
				{
					if (down && !g_openMenuHeld)
					{
						Link::Get().PushInput(proto::kInReleaseAll, 0);
						Link::Get().PushInput(proto::kInOpenMenu, 0);
					}
					g_openMenuHeld = down;
					return true;
				}
			}
			PushKey(dik, down);
			// Soltar tecla vai tambem para o W3: se ela foi apertada antes do MC assumir, o W3 nao fica
			// com ela presa (ex.: W segurado ao sair de um menu).
			return down;
		}

		void RouteRawMouse(const RAWMOUSE& m)
		{
			auto& link = Link::Get();
			if (!(m.usFlags & MOUSE_MOVE_ABSOLUTE) && (m.lLastX != 0 || m.lLastY != 0))
			{
				if (session::McScreenOpen())
					session::MoveCursor(m.lLastX, m.lLastY);
				else
					session::AddLook(m.lLastX, m.lLastY);
			}

			struct ButtonBits
			{
				USHORT        down, up;
				std::uint16_t sdl;  // SDL: 1 L, 2 M, 3 R, 4 X1, 5 X2
			};
			static constexpr ButtonBits kButtons[] = {
			    { RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_LEFT_BUTTON_UP, 1 },
			    { RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_UP, 3 },
			    { RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP, 2 },
			    { RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_4_UP, 4 },
			    { RI_MOUSE_BUTTON_5_DOWN, RI_MOUSE_BUTTON_5_UP, 5 },
			};
			const USHORT flags = m.usButtonFlags;
			for (const auto& b : kButtons)
			{
				if (flags & b.down)
					link.PushInput(proto::kInMouseButton, b.sdl, 1);
				if (flags & b.up)
					link.PushInput(proto::kInMouseButton, b.sdl, 0);
			}
			if (flags & RI_MOUSE_WHEEL)
				link.PushInput(proto::kInScroll, 0, static_cast<SHORT>(m.usButtonData));  // 120 por "dente", positivo = para cima
		}

		void PushText(wchar_t ch)
		{
			std::uint32_t cp = ch;
			if (ch >= 0xD800 && ch <= 0xDBFF)
			{
				g_highSurrogate = ch;
				return;
			}
			if (ch >= 0xDC00 && ch <= 0xDFFF)
			{
				if (!g_highSurrogate)
					return;
				cp = 0x10000u + ((static_cast<std::uint32_t>(g_highSurrogate) - 0xD800u) << 10) + (static_cast<std::uint32_t>(ch) - 0xDC00u);
			}
			g_highSurrogate = 0;
			if (cp < 0x20 || cp == 0x7F)
				return;  // Enter, Backspace, Tab... chegam como teclas
			Link::Get().PushInput(proto::kInText, 0, static_cast<std::int32_t>(cp));
		}

		LRESULT CALLBACK HookedProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
		{
			switch (msg)
			{
			case WM_INPUT:
			{
				// O jogo le teclado/mouse por Raw Input (WM_INPUT, sem RIDEV_NOLEGACY), entao tudo chega duas
				// vezes: como WM_INPUT e como mensagem legada. Encaminha ao MC so pelo WM_INPUT e engole
				// ambos. Engolir WM_INPUT = DefWindowProc (libera o buffer) sem repassar ao jogo.
				RAWINPUT raw{};
				if (!ReadRaw(lParam, raw))
					break;
				const bool route = session::RouteInput();
				if (raw.header.dwType == RIM_TYPEKEYBOARD)
				{
					const RAWKEYBOARD& kb = raw.data.keyboard;
					if (kb.VKey == kCameraKey || RawToDik(kb) == kDikF5)
					{
						// Com o MC no controle o F5 e dele (a camera F5 do MC cicla); senao conta no WM_KEYDOWN.
						if (route)
							PushKey(kDikF5, (kb.Flags & RI_KEY_BREAK) == 0);
						return DefWindowProcW(hwnd, msg, wParam, lParam);
					}
					if (route && RouteRawKey(kb))
						return DefWindowProcW(hwnd, msg, wParam, lParam);
				}
				else if (raw.header.dwType == RIM_TYPEMOUSE && route)
				{
					RouteRawMouse(raw.data.mouse);
					return DefWindowProcW(hwnd, msg, wParam, lParam);
				}
				break;
			}

			case WM_KEYDOWN:
			case WM_SYSKEYDOWN:
				if (wParam == kCameraKey)
				{
					const bool isRepeat = (lParam & (1LL << 30)) != 0;
					if (!isRepeat && !session::Linked())
						g_cameraPresses.fetch_add(1);
					return 0;
				}
				if (session::RouteInput())
				{
					const std::uint32_t dik = LegacyToDik(lParam);
					if (!session::McScreenOpen() && (IsGameMenuKey(dik) || (wParam == VK_F4 && AltDown())))
						break;
					return 0;
				}
				break;

			case WM_KEYUP:
			case WM_SYSKEYUP:
				if (wParam == kCameraKey)
					return 0;
				break;  // solturas sempre chegam ao W3

			case WM_CHAR:
			case WM_SYSCHAR:
				if (session::RouteInput())
				{
					if (session::McScreenOpen())
					{
						if (msg == WM_CHAR)
							PushText(static_cast<wchar_t>(wParam));
						return 0;
					}
					if (wParam == VK_ESCAPE)
						break;
					return 0;
				}
				break;

			case WM_MOUSEMOVE:
			case WM_LBUTTONDOWN:
			case WM_LBUTTONDBLCLK:
			case WM_RBUTTONDOWN:
			case WM_RBUTTONDBLCLK:
			case WM_MBUTTONDOWN:
			case WM_MBUTTONDBLCLK:
			case WM_XBUTTONDOWN:
			case WM_XBUTTONDBLCLK:
			case WM_MOUSEWHEEL:
			case WM_MOUSEHWHEEL:
				if (session::RouteInput())
					return msg == WM_XBUTTONDOWN || msg == WM_XBUTTONDBLCLK ? TRUE : 0;
				break;

			case WM_KILLFOCUS:
			case WM_ACTIVATEAPP:
				// O W3 perdeu o foco: o MC solta o que estava apertado.
				if (session::Linked() && (msg == WM_KILLFOCUS || wParam == FALSE))
					Link::Get().PushInput(proto::kInReleaseAll, 0);
				break;
			}

			return CallWindowProcW(g_originalProc, hwnd, msg, wParam, lParam);
		}

		BOOL CALLBACK FindViewport(HWND hwnd, LPARAM out)
		{
			DWORD pid = 0;
			GetWindowThreadProcessId(hwnd, &pid);
			if (pid != GetCurrentProcessId())
				return TRUE;

			wchar_t cls[64]{};
			GetClassNameW(hwnd, cls, 64);
			if (wcscmp(cls, kViewportClass) != 0)
				return TRUE;

			*reinterpret_cast<HWND*>(out) = hwnd;
			return FALSE;
		}

		DWORD WINAPI HookThread(LPVOID)
		{
			// A janela aparece segundos depois do carregamento das DLLs; espera ate 5 minutos.
			HWND hwnd = nullptr;
			for (int i = 0; i < 3000 && !hwnd; ++i)
			{
				EnumWindows(FindViewport, reinterpret_cast<LPARAM>(&hwnd));
				if (!hwnd)
					Sleep(100);
			}

			if (!hwnd)
			{
				Log("ERRO: janela W2ViewportClass nao encontrada; F5 nao sera interceptado. Janelas do processo:");
				EnumWindows(
				    [](HWND w, LPARAM) -> BOOL {
					    DWORD pid = 0;
					    GetWindowThreadProcessId(w, &pid);
					    if (pid == GetCurrentProcessId())
					    {
						    char cls[128]{};
						    GetClassNameA(w, cls, 128);
						    Log(std::string("  classe=") + cls);
					    }
					    return TRUE;
				    },
				    0);
				return 0;
			}

			// A WndProc roda na thread da janela, nao nesta: guarda a original antes de trocar, para a
			// HookedProc nunca chamar CallWindowProcW(nullptr) se uma mensagem chegar no meio.
			g_originalProc = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(hwnd, GWLP_WNDPROC));
			const auto previous = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&HookedProc)));
			if (previous)
				g_originalProc = previous;
			if (!previous)
			{
				Log("ERRO: SetWindowLongPtrW falhou (" + std::to_string(GetLastError()) + ")");
				return 0;
			}
			g_window = hwnd;

			Log("WndProc da janela do jogo interceptada; F5 agora troca a camera (input vai ao MC quando ele estiver linkado)");
			return 0;
		}
	} // namespace

	void StartInputHook()
	{
		HANDLE thread = CreateThread(nullptr, 0, HookThread, nullptr, 0, nullptr);
		if (thread)
			CloseHandle(thread);
	}

	int ConsumeCameraKeyPresses()
	{
		return g_cameraPresses.exchange(0);
	}

	bool GetViewportSize(int& width, int& height)
	{
		HWND hwnd = g_window.load();
		RECT rc{};
		if (!hwnd || !GetClientRect(hwnd, &rc))
			return false;
		width = rc.right - rc.left;
		height = rc.bottom - rc.top;
		return width > 0 && height > 0;
	}
} // namespace mw3

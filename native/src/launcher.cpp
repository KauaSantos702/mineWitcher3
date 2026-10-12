// Derived from SkyCraft (https://github.com/chasmlol/skycraft), Copyright (c) 2026 chasmlol, MIT License.
// Port of skse/src/Launcher.cpp (upstream commit bfcaf17). Differences: no bundle unpacking (the
// portable Prism instance is set up by a script in tools/), config in mw3.ini next to witcher3.exe
// (read with GetPrivateProfile* instead of SimpleIni), paths and names for MineWitcher3.
// See THIRD-PARTY-NOTICES.md.
// As interfaces do shell (ExDisp.h/ShlDisp.h) precisam de OLE completo, que o WIN32_LEAN_AND_MEAN corta.
#undef WIN32_LEAN_AND_MEAN

#include "launcher.h"

#include "../../protocol/mw3_protocol.h"
#include "config.h"
#include "log.h"

#include <windows.h>

#include <ExDisp.h>
#include <ShlDisp.h>
#include <ShlObj.h>
#include <servprov.h>
#include <wrl/client.h>

#include <atomic>
#include <filesystem>
#include <string>
#include <utility>

// O Minecraft inicia com o jogo. O mod Fabric do MineWitcher3 espera na tela de titulo ate o mundo do
// W3 estar de pe, esconde a propria janela, abre o mundo MineWitcher3 sozinho e fecha quando este
// witcher3.exe fecha. O que iniciar vem do mw3.ini; por padrao a instancia "MineWitcher3" de um Prism
// Launcher portatil em %LOCALAPPDATA%\MineWitcher3\Prism (o Prism faz o login da conta Microsoft; o
// launcher oficial nao pode ser iniciado num perfil de fora).
namespace mw3::launcher
{
	namespace
	{
		constexpr wchar_t kDefaultLauncher[] = L"%LOCALAPPDATA%\\MineWitcher3\\Prism\\prismlauncher.exe";
		constexpr wchar_t kDefaultArguments[] = L"--launch MineWitcher3";

		std::atomic<Status> g_status{ Status::kOff };

		// Roda um programa como um duplo clique faria: iniciado pelo Explorer do desktop, nao pelo jogo.
		// Assim o Minecraft nao vira filho do witcher3.exe (nem herda o ambiente/overlay da Steam dele).
		bool OpenFromDesktop(const std::wstring& file, const std::wstring& args, const std::wstring& dir, int show)
		{
			using Microsoft::WRL::ComPtr;
			ComPtr<IShellWindows> windows;
			if (FAILED(::CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&windows))))
				return false;
			VARIANT location{};
			location.vt = VT_I4;
			location.lVal = CSIDL_DESKTOP;
			VARIANT           empty{};
			long              hwnd = 0;
			ComPtr<IDispatch> desktop;
			if (FAILED(windows->FindWindowSW(&location, &empty, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH, &desktop)) || !desktop)
				return false;
			ComPtr<IServiceProvider>     services;
			ComPtr<IShellBrowser>        browser;
			ComPtr<IShellView>           view;
			ComPtr<IDispatch>            background;
			ComPtr<IShellFolderViewDual> folderView;
			ComPtr<IDispatch>            application;
			ComPtr<IShellDispatch2>      shell;
			if (FAILED(desktop.As(&services)) || FAILED(services->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser))) ||
			    FAILED(browser->QueryActiveShellView(&view)) || FAILED(view->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(&background))) ||
			    FAILED(background.As(&folderView)) || FAILED(folderView->get_Application(&application)) || FAILED(application.As(&shell)))
				return false;
			BSTR    bfile = ::SysAllocString(file.c_str());
			VARIANT vargs{}, vdir{}, operation{}, vshow{};
			vargs.vt = vdir.vt = operation.vt = VT_BSTR;
			vargs.bstrVal = ::SysAllocString(args.c_str());
			vdir.bstrVal = ::SysAllocString(dir.c_str());
			operation.bstrVal = ::SysAllocString(L"open");
			vshow.vt = VT_I4;
			vshow.lVal = show;
			const HRESULT hr = shell->ShellExecute(bfile, vargs, vdir, operation, vshow);
			::SysFreeString(bfile);
			::VariantClear(&vargs);
			::VariantClear(&vdir);
			::VariantClear(&operation);
			return SUCCEEDED(hr);
		}

		// Pelo desktop (ver OpenFromDesktop) ou, sem shell para pedir, direto. .bat/.cmd vao pelo cmd, sem console.
		bool Start(const std::filesystem::path& program, const std::wstring& args, const std::string& shown)
		{
			const auto         ext = program.extension().wstring();
			const bool         script = _wcsicmp(ext.c_str(), L".bat") == 0 || _wcsicmp(ext.c_str(), L".cmd") == 0;
			const std::wstring dir = program.parent_path().wstring();
			const HRESULT      com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
			const bool         viaDesktop = OpenFromDesktop(program.wstring(), args, dir, script ? SW_HIDE : SW_SHOWNORMAL);
			if (SUCCEEDED(com))
				::CoUninitialize();
			if (viaDesktop)
			{
				Log("Minecraft: iniciado " + shown);
				return true;
			}
			std::wstring command = script ? L"cmd.exe /c \"\"" + program.wstring() + L"\" " + args + L"\"" : L"\"" + program.wstring() + L"\" " + args;
			STARTUPINFOW        si{ sizeof(si) };
			PROCESS_INFORMATION pi{};
			if (!::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, script ? CREATE_NO_WINDOW : 0, nullptr, dir.c_str(), &si, &pi))
			{
				Log("Minecraft: ERRO ao iniciar " + shown + " (erro " + std::to_string(::GetLastError()) + ")");
				return false;
			}
			::CloseHandle(pi.hThread);
			::CloseHandle(pi.hProcess);
			Log("Minecraft: iniciado " + shown + " (direto)");
			return true;
		}

		DWORD WINAPI StartThread(LPVOID param)
		{
			auto* job = static_cast<std::pair<std::filesystem::path, std::wstring>*>(param);
			const std::string shown = config::Narrow(job->first.wstring() + L" " + job->second);
			g_status = Start(job->first, job->second, shown) ? Status::kStarting : Status::kFailed;
			delete job;
			return 0;
		}

		// O MC que estava rodando pode ser o da execucao anterior do jogo saindo (ele fecha alguns
		// segundos depois do W3). Se ele sumir no proximo minuto, inicia um para este jogo.
		DWORD WINAPI WatchOldMinecraft(LPVOID)
		{
			for (int i = 0; i < 60; ++i)
			{
				Sleep(1000);
				if (!MinecraftRunning())
				{
					Log("Minecraft: o que estava rodando fechou; iniciando outro");
					Sleep(3000);  // deixa o launcher dele fechar tambem
					StartMinecraft();
					return 0;
				}
			}
			return 0;
		}
	} // namespace

	Status GetStatus()
	{
		return g_status.load();
	}

	bool MinecraftRunning()
	{
		HANDLE mutex = ::OpenMutexW(SYNCHRONIZE, FALSE, proto::kRunningMutexName);
		if (mutex)
		{
			::CloseHandle(mutex);
			return true;
		}
		return false;
	}

	void StartMinecraft()
	{
		const bool haveIni = config::IniExists();
		if (config::GetInt(L"Minecraft", L"bStartMinecraft", 1) == 0)
		{
			Log("Minecraft: nao iniciado com o jogo (bStartMinecraft = 0 em mw3.ini)");
			g_status = Status::kOff;
			return;
		}
		if (MinecraftRunning())
		{
			Log("Minecraft: ja esta rodando");
			g_status = Status::kRunning;
			HANDLE watch = CreateThread(nullptr, 0, WatchOldMinecraft, nullptr, 0, nullptr);
			if (watch)
				CloseHandle(watch);
			return;
		}

		std::wstring chosen = config::GetString(L"Minecraft", L"sLauncher", L"");
		const bool   custom = !chosen.empty();
		if (!custom)
			chosen = kDefaultLauncher;
		const std::filesystem::path program = config::ExpandEnv(chosen);
		const std::wstring          args = config::GetString(L"Minecraft", L"sArguments", kDefaultArguments);

		std::error_code ec;
		if (!std::filesystem::exists(program, ec))
		{
			if (custom)
				Log("Minecraft: NAO iniciado: " + config::Narrow(program.wstring()) + " nao existe (sLauncher em " + config::Narrow(config::IniPath()) + ")");
			else
				Log("Minecraft: NAO iniciado: Prism Launcher portatil nao encontrado em " + config::Narrow(program.wstring()) +
				    ". Rode o script de setup da instancia Prism em tools\\ ou defina sLauncher em " + config::Narrow(config::IniPath()) +
				    (haveIni ? "" : " (arquivo ainda nao existe)") + ". O jogo continua normal, sem Minecraft.");
			g_status = Status::kNoLauncher;
			return;
		}

		g_status = Status::kStarting;
		// Fora da thread de init: falar com o Explorer leva um instante.
		auto*  job = new std::pair<std::filesystem::path, std::wstring>(program, args);
		HANDLE thread = CreateThread(nullptr, 0, StartThread, job, 0, nullptr);
		if (thread)
			CloseHandle(thread);
		else
		{
			delete job;
			g_status = Status::kFailed;
			Log("Minecraft: ERRO ao criar a thread de inicializacao");
		}
	}
} // namespace mw3::launcher

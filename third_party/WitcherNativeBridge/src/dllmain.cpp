#include "GameModule.h"
#include "NativeBridge.h"
#include "NativeImports.h"

#include <windows.h>

#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace
{
	bool PinBridgeModule()
	{
		HMODULE pinnedModule = nullptr;
		return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&PinBridgeModule), &pinnedModule) != FALSE;
	}

	std::string BuildLogPath()
	{
		char executable[MAX_PATH]{};
		if (GetModuleFileNameA(nullptr, executable, MAX_PATH) == 0)
			return "WitcherNativeBridge.log";

		fs::path path(executable);
		return (path.parent_path() / "WitcherNativeBridge.log").string();
	}

	DWORD WINAPI InitThreadProc(LPVOID)
	{
		witcher_native_bridge::InitLog(BuildLogPath());
		witcher_native_bridge::DebugLog("startup");

		if (!PinBridgeModule())
			witcher_native_bridge::DebugLog("warning: failed to pin bridge module");

		if (witcher_native_bridge::GameModule::Resolve())
		{
			witcher_native_bridge::DebugLog("host=" + witcher_native_bridge::GameModule::HostName() + " version=" + witcher_native_bridge::GameModule::Version());
		}
		else
		{
			witcher_native_bridge::DebugLog("failed to resolve host module");
			return 0;
		}

		if (!witcher_native_bridge::ResolveScriptApi())
		{
			witcher_native_bridge::DebugLog("script API signature resolution failed");
			return 0;
		}

		witcher_native_bridge::DebugLog("script API resolved");

		if (witcher_native_bridge::CanMarshalStrings())
		{
			witcher_native_bridge::DebugLog("script string marshalling resolution OK");
			if (!witcher_native_bridge::imports::RegisterAll())
				witcher_native_bridge::DebugLog("failed to queue one or more built-in imports");
		}
		else
		{
			witcher_native_bridge::DebugLog("script string marshalling resolution unavailable; StringToName not queued");
		}

		if (!witcher_native_bridge::InstallRegistrationHook())
		{
			witcher_native_bridge::DebugLog("registration hook failed: " + witcher_native_bridge::RegistrationError());
			return 0;
		}

		witcher_native_bridge::DebugLog("registration hook installed OK");
		return 0;
	}
} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
	switch (reason)
	{
	case DLL_PROCESS_ATTACH: {
		DisableThreadLibraryCalls(module);
		HANDLE thread = CreateThread(nullptr, 0, InitThreadProc, nullptr, 0, nullptr);
		if (thread)
			CloseHandle(thread);
		break;
	}

	case DLL_PROCESS_DETACH:
		break;
	}

	return TRUE;
}

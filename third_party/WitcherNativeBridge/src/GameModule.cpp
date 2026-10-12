#include "GameModule.h"

#include <windows.h>
#include <psapi.h>
#include <winver.h>

#include <string>
#include <vector>

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "version.lib")

namespace witcher_native_bridge
{
	ModuleRegion GameModule::image_{};
	ModuleRegion GameModule::text_{};
	ModuleRegion GameModule::self_{};
	std::string GameModule::version_ = "unknown";
	std::string GameModule::hostName_ = "unknown";
	bool GameModule::selfHosted_ = false;

	namespace
	{
		bool QueryRegion(HMODULE module, ModuleRegion& out)
		{
			if (!module)
				return false;

			MODULEINFO info{};
			if (!GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info)))
				return false;

			out.base = static_cast<uint8_t*>(info.lpBaseOfDll);
			out.size = static_cast<size_t>(info.SizeOfImage);
			return out.IsValid();
		}
	} // namespace

	bool GameModule::Resolve()
	{
		if (IsResolved())
			return true;

		if (!ResolveImage())
			return false;

		ResolveSelf();
		ResolveVersion();
		selfHosted_ = self_.IsValid() && self_.base == image_.base;

		if (!ResolveTextSection())
			text_ = image_;

		return true;
	}

	bool GameModule::ResolveImage()
	{
		if (!QueryRegion(GetModuleHandleW(nullptr), image_))
			return false;

		char path[MAX_PATH]{};
		if (GetModuleFileNameA(nullptr, path, MAX_PATH) != 0)
		{
			const std::string fullPath(path);
			const size_t slash = fullPath.find_last_of("\\/");
			hostName_ = (slash == std::string::npos) ? fullPath : fullPath.substr(slash + 1);
		}

		return true;
	}

	bool GameModule::ResolveSelf()
	{
		HMODULE module = nullptr;
		const BOOL ok = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&GameModule::ResolveSelf), &module);

		return ok != FALSE && QueryRegion(module, self_);
	}

	bool GameModule::IsInsideSelf(const uint8_t* address)
	{
		if (!self_.IsValid() || !address)
			return false;

		return address >= self_.base && address < (self_.base + self_.size);
	}

	bool GameModule::ResolveTextSection()
	{
		if (!image_.IsValid())
			return false;

		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image_.base);
		if (dos->e_magic != IMAGE_DOS_SIGNATURE)
			return false;

		const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(image_.base + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE)
			return false;

		const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
		for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section)
		{
			if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0)
				continue;

			text_.base = image_.base + section->VirtualAddress;
			text_.size = static_cast<size_t>(section->Misc.VirtualSize);
			return text_.IsValid();
		}

		return false;
	}

	void GameModule::ResolveVersion()
	{
		wchar_t path[MAX_PATH]{};
		if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0)
			return;

		DWORD ignored = 0;
		const DWORD bytes = GetFileVersionInfoSizeW(path, &ignored);
		if (bytes == 0)
			return;

		std::vector<uint8_t> versionData(bytes);
		if (!GetFileVersionInfoW(path, ignored, bytes, versionData.data()))
			return;

		VS_FIXEDFILEINFO* info = nullptr;
		UINT infoSize = 0;
		if (!VerQueryValueW(versionData.data(), L"\\", reinterpret_cast<LPVOID*>(&info), &infoSize) || !info)
			return;

		version_ = std::to_string(HIWORD(info->dwFileVersionMS)) + "." + std::to_string(LOWORD(info->dwFileVersionMS)) + "." + std::to_string(HIWORD(info->dwFileVersionLS)) + "." + std::to_string(LOWORD(info->dwFileVersionLS));
	}
} // namespace witcher_native_bridge

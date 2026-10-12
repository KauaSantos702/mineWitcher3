#include "config.h"

#include <windows.h>

#include <iterator>

namespace mw3::config
{
	std::wstring IniPath()
	{
		wchar_t     path[MAX_PATH]{};
		const DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
		if (len == 0 || len >= MAX_PATH)
			return L"mw3.ini";
		std::wstring p(path, len);
		const auto   slash = p.find_last_of(L"\\/");
		return (slash == std::wstring::npos ? std::wstring() : p.substr(0, slash + 1)) + L"mw3.ini";
	}

	bool IniExists()
	{
		return GetFileAttributesW(IniPath().c_str()) != INVALID_FILE_ATTRIBUTES;
	}

	int GetInt(const wchar_t* section, const wchar_t* key, int fallback)
	{
		return static_cast<int>(GetPrivateProfileIntW(section, key, fallback, IniPath().c_str()));
	}

	std::wstring GetString(const wchar_t* section, const wchar_t* key, const wchar_t* fallback)
	{
		wchar_t     buf[1024]{};
		const DWORD n = GetPrivateProfileStringW(section, key, fallback, buf, static_cast<DWORD>(std::size(buf)), IniPath().c_str());
		return std::wstring(buf, n);
	}

	bool OverlayEnabled()
	{
		return GetInt(L"Overlay", L"bEnable", 0) != 0;
	}

	int OverlayPaperWhiteNits()
	{
		const int nits = GetInt(L"Overlay", L"iHdrPaperWhiteNits", 200);
		return nits < 80 ? 80 : (nits > 1000 ? 1000 : nits);
	}

	std::string Narrow(const std::wstring& text)
	{
		if (text.empty())
			return {};
		const int   n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
		std::string out(static_cast<size_t>(n), '\0');
		WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n, nullptr, nullptr);
		return out;
	}

	std::wstring ExpandEnv(const std::wstring& path)
	{
		wchar_t     buf[MAX_PATH * 2];
		const DWORD n = ExpandEnvironmentStringsW(path.c_str(), buf, static_cast<DWORD>(std::size(buf)));
		return n > 0 && n <= std::size(buf) ? std::wstring(buf) : path;
	}
} // namespace mw3::config

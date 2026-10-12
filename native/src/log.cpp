#include "log.h"

#include <windows.h>

#include <share.h>

#include <cstdio>
#include <mutex>

namespace mw3
{
	namespace
	{
		std::mutex g_lock;
		FILE* g_file = nullptr;
	} // namespace

	void InitLog()
	{
		std::lock_guard<std::mutex> guard(g_lock);
		if (g_file)
			return;

		wchar_t path[MAX_PATH]{};
		const DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
		if (len == 0 || len >= MAX_PATH)
			return;

		wchar_t* slash = wcsrchr(path, L'\\');
		if (!slash)
			return;
		wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"mw3.log");

		// Sobrescreve a cada execucao do jogo; _SH_DENYWR deixa outros processos lerem com o jogo aberto.
		g_file = _wfsopen(path, L"w", _SH_DENYWR);
	}

	void Log(const std::string& text)
	{
		SYSTEMTIME t{};
		GetLocalTime(&t);

		std::lock_guard<std::mutex> guard(g_lock);
		if (!g_file)
			return;

		std::fprintf(g_file, "[%02u:%02u:%02u.%03u] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, text.c_str());
		std::fflush(g_file);
	}
} // namespace mw3

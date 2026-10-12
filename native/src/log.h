#pragma once

#include <string>

namespace mw3
{
	// Arquivo mw3.log ao lado do witcher3.exe. Seguro para chamar de qualquer thread.
	void InitLog();
	void Log(const std::string& text);
} // namespace mw3

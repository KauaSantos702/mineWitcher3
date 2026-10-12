#pragma once

namespace mw3
{
	// Versao da ponte; o script compara com MW3_EXPECTED_NATIVE_VERSION.
	inline constexpr int kNativeVersion = 6;

	// Enfileira as natives no WitcherNativeBridge. Precisa rodar antes do motor montar o RTTI.
	bool RegisterNatives();
} // namespace mw3

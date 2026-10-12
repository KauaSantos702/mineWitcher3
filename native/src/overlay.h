// Derived from SkyCraft (https://github.com/chasmlol/skycraft), Copyright (c) 2026 chasmlol, MIT License.
// D3D12 port of skse/src/Overlay.cpp (Minecraft's hand/HUD/screens drawn over the game at Present).
// See THIRD-PARTY-NOTICES.md.
#pragma once

#include <cstdint>
#include <vector>

namespace mw3::overlay
{
	struct Options
	{
		// Desenhar agora? (MC linkado, sem menu do W3...). nullptr = sempre que houver frame.
		bool (*shouldDraw)() = nullptr;
		// Cursor das telas do MC em pixels do overlay; true = desenhar. nullptr = sem cursor.
		bool (*cursor)(float& x, float& y) = nullptr;
		// Branco do overlay em nits quando o back buffer e HDR (scRGB ou HDR10/PQ).
		float hdrPaperWhiteNits = 200.0f;
		// Link::Heartbeat() a cada Present (roda ate em menus/loading, como no SkyCraft).
		bool heartbeat = true;
		// So para o teste: copia o back buffer depois do desenho (formatos de 4 bytes) para DebugReadback.
		bool debugReadback = false;
		// So para o teste: HMODULE cujo RTTI tem a classe sl::interposer::DXGISwapChain (nullptr =
		// sl.interposer.dll, o do jogo).
		void* proxyModule = nullptr;
	};

	// Instala os hooks na vtable do proxy de swapchain do Streamline (sl::interposer::DXGISwapChain,
	// achada pelo RTTI no sl.interposer.dll): a camada que o jogo chama, acima do Streamline/frame
	// generation e dos hooks nativos (overlay da Steam, RTSS). A vtable nativa do dxgi nunca e escrita;
	// a do D3D12Core so recebe um hook transitorio de ExecuteCommandLists ate a fila ser conhecida.
	// Sincrono. false = falhou (logado em mw3.log); o jogo segue sem overlay. Nunca lanca.
	bool Install(const Options& options);

	// mw3.asi: le [Overlay] do mw3.ini e, numa thread propria, espera a janela do jogo e chama Install.
	void StartForGame();

	struct Stats
	{
		bool          hooked = false;
		bool          disabled = false;            // falha grave: overlay desligado e slots restaurados
		bool          eclHookActive = false;       // hook nativo de ExecuteCommandLists ainda no slot
		int           proxySlotsActive = 0;        // slots do proxy com o nosso hook (5 instalado, 0 restaurado)
		bool          queueFromSwapchain = false;  // fila veio de IDXGISwapChain::GetDevice
		bool          queueFromEcl = false;        // fila veio do hook de ExecuteCommandLists
		std::uint64_t presents = 0;
		std::uint64_t draws = 0;
		std::uint64_t uploads = 0;
		std::uint64_t eclCalls = 0;
		std::uint64_t reentries = 0;  // Present/Present1 aninhados na mesma thread (so repassados)
		std::uint32_t backBufferFormat = 0;  // DXGI_FORMAT
		std::uint32_t backBufferW = 0, backBufferH = 0;
		int           colorMode = 0;  // 0 SDR, 1 scRGB, 2 HDR10
	};
	Stats GetStats();

	// Ultima copia do back buffer (Options::debugReadback). Pixels crus de 32 bits, linha a linha.
	bool DebugReadback(std::uint32_t& width, std::uint32_t& height, std::vector<std::uint32_t>& pixels);
} // namespace mw3::overlay

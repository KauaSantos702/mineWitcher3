// Teste do overlay D3D12 (nao vai para o jogo). Cria janela + device + swapchain D3D12 proprios e
// reproduz as camadas do jogo (research/01-render.md, "Overlay da Steam"):
//   app -> proxy sl::interposer::DXGISwapChain (imitacao do Streamline, mesmo nome de RTTI, que o
//          overlay acha e hookeia como no jogo) -> vtable nativa do dxgi -> "Steam" simulada
//          (desvio inline no Present nativo, um trampolim global, que chama Present de volta pela
//          vtable e refaz o desvio a cada swapchain criada, como o gameoverlayrenderer64.dll).
// Controle negativo: um hook no slot nativo (o desenho antigo) + um passe da "Steam" entra em laco
// (o crash de 2026-10-11); a "Steam" simulada corta em 32 niveis em vez de estourar a pilha.
// Faz o papel do Minecraft no triple buffer do overlay (xadrez premultiplicado com alfa, linhas
// bottom-up como o FrameExporter) e confere o back buffer por readback depois do desenho. Fases:
// RGBA8 640x360, ResizeBuffers 800x450, gate desligado, R10G10B10A2 + SetColorSpace1(HDR10),
// R16G16B16A16_FLOAT, excecao no desenho (desliga e restaura os slots).
//
//   mw3_overlaytest.exe     roda tudo (~5 s) e sai (0 = PASS). Log em mw3.log ao lado do exe.
#include "../src/link.h"
#include "../src/log.h"
#include "../src/overlay.h"

#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{
	using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);

	PresentFn VtablePresent(IUnknown* object)
	{
		return reinterpret_cast<PresentFn>((*reinterpret_cast<void***>(object))[8]);
	}

	bool g_inPresent = false;             // o app esta dentro de Present (o desenho do overlay roda ai)
	bool g_throwInDrawGetBuffer = false;  // fase 6: GetBuffer do proxy lanca SEH dentro do desenho
	int  g_proxyReentries = 0;
} // namespace

// Imitacao do proxy de swapchain do Streamline: o MESMO nome de classe (RTTI
// ".?AUDXGISwapChain@interposer@sl@@") e a mesma vtable estatica de IDXGISwapChain4 (41 slots) do
// sl.interposer.dll; encaminha tudo para a swapchain nativa. O Present chama Present de novo pela
// propria vtable (com DXGI_PRESENT_TEST) para exercitar a reentrada no hook do overlay.
namespace sl::interposer
{
	struct DXGISwapChain : public IDXGISwapChain4
	{
		explicit DXGISwapChain(IDXGISwapChain4* native) : m_native(native) {}

		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
		{
			if (!ppv)
				return E_POINTER;
			if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) || riid == __uuidof(IDXGIDeviceSubObject) ||
			    riid == __uuidof(IDXGISwapChain) || riid == __uuidof(IDXGISwapChain1) || riid == __uuidof(IDXGISwapChain2) ||
			    riid == __uuidof(IDXGISwapChain3) || riid == __uuidof(IDXGISwapChain4))
			{
				*ppv = static_cast<IDXGISwapChain4*>(this);
				AddRef();
				return S_OK;
			}
			return m_native->QueryInterface(riid, ppv);
		}
		ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&m_ref)); }
		ULONG STDMETHODCALLTYPE Release() override
		{
			const LONG n = InterlockedDecrement(&m_ref);
			if (n == 0)
				delete this;
			return static_cast<ULONG>(n);
		}
		HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID name, UINT size, const void* data) override { return m_native->SetPrivateData(name, size, data); }
		HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID name, const IUnknown* unknown) override { return m_native->SetPrivateDataInterface(name, unknown); }
		HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID name, UINT* size, void* data) override { return m_native->GetPrivateData(name, size, data); }
		HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** parent) override { return m_native->GetParent(riid, parent); }
		HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** device) override { return m_native->GetDevice(riid, device); }
		HRESULT STDMETHODCALLTYPE Present(UINT sync, UINT flags) override
		{
			if (!(flags & DXGI_PRESENT_TEST))
			{
				++g_proxyReentries;
				VtablePresent(this)(this, 0, DXGI_PRESENT_TEST);  // de volta pela vtable do proxy (= hook do overlay)
			}
			return m_native->Present(sync, flags);
		}
		HRESULT STDMETHODCALLTYPE GetBuffer(UINT buffer, REFIID riid, void** surface) override
		{
			if (g_throwInDrawGetBuffer && g_inPresent)
			{
				g_throwInDrawGetBuffer = false;
				RaiseException(0xE0D3D012u, 0, 0, nullptr);  // "falha do driver/proxy" dentro do desenho
			}
			return m_native->GetBuffer(buffer, riid, surface);
		}
		HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL fullscreen, IDXGIOutput* target) override { return m_native->SetFullscreenState(fullscreen, target); }
		HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* fullscreen, IDXGIOutput** target) override { return m_native->GetFullscreenState(fullscreen, target); }
		HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* desc) override { return m_native->GetDesc(desc); }
		HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT count, UINT w, UINT h, DXGI_FORMAT format, UINT flags) override { return m_native->ResizeBuffers(count, w, h, format, flags); }
		HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC* params) override { return m_native->ResizeTarget(params); }
		HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** output) override { return m_native->GetContainingOutput(output); }
		HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* stats) override { return m_native->GetFrameStatistics(stats); }
		HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* count) override { return m_native->GetLastPresentCount(count); }
		HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* desc) override { return m_native->GetDesc1(desc); }
		HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* desc) override { return m_native->GetFullscreenDesc(desc); }
		HRESULT STDMETHODCALLTYPE GetHwnd(HWND* hwnd) override { return m_native->GetHwnd(hwnd); }
		HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID riid, void** unk) override { return m_native->GetCoreWindow(riid, unk); }
		HRESULT STDMETHODCALLTYPE Present1(UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* params) override { return m_native->Present1(sync, flags, params); }
		BOOL STDMETHODCALLTYPE    IsTemporaryMonoSupported() override { return m_native->IsTemporaryMonoSupported(); }
		HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput** output) override { return m_native->GetRestrictToOutput(output); }
		HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA* color) override { return m_native->SetBackgroundColor(color); }
		HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA* color) override { return m_native->GetBackgroundColor(color); }
		HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION rotation) override { return m_native->SetRotation(rotation); }
		HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION* rotation) override { return m_native->GetRotation(rotation); }
		HRESULT STDMETHODCALLTYPE SetSourceSize(UINT w, UINT h) override { return m_native->SetSourceSize(w, h); }
		HRESULT STDMETHODCALLTYPE GetSourceSize(UINT* w, UINT* h) override { return m_native->GetSourceSize(w, h); }
		HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT latency) override { return m_native->SetMaximumFrameLatency(latency); }
		HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* latency) override { return m_native->GetMaximumFrameLatency(latency); }
		HANDLE STDMETHODCALLTYPE  GetFrameLatencyWaitableObject() override { return m_native->GetFrameLatencyWaitableObject(); }
		HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F* matrix) override { return m_native->SetMatrixTransform(matrix); }
		HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F* matrix) override { return m_native->GetMatrixTransform(matrix); }
		UINT STDMETHODCALLTYPE    GetCurrentBackBufferIndex() override { return m_native->GetCurrentBackBufferIndex(); }
		HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE cs, UINT* support) override { return m_native->CheckColorSpaceSupport(cs, support); }
		HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE cs) override { return m_native->SetColorSpace1(cs); }
		HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT count, UINT w, UINT h, DXGI_FORMAT format, UINT flags, const UINT* masks, IUnknown* const* queues) override
		{
			return m_native->ResizeBuffers1(count, w, h, format, flags, masks, queues);
		}
		HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE type, UINT size, void* data) override { return m_native->SetHDRMetaData(type, size, data); }

	private:
		~DXGISwapChain() = default;
		ComPtr<IDXGISwapChain4> m_native;
		LONG                    m_ref = 1;
	};
} // namespace sl::interposer

// "Steam" simulada: o que o gameoverlayrenderer64.dll faz (dumps de 2026-10-11, funcao +0x94660
// "Hooking vtable for swap chain"): a cada swapchain criada le o slot Present da vtable NATIVA e
// desvia (inline) a funcao para onde ele aponta, se ainda nao for desvio dela, guardando o
// "original" num UNICO global (gameoverlayrenderer64+0x167340). O handler chama Present de volta
// pela vtable do objeto (consulta) e depois o original. Desvio: jmp [rip] absoluto de 14 bytes;
// "chamar o original" = repor os bytes, chamar, refazer o desvio (thread unica no teste).
namespace simsteam
{
	constexpr std::size_t kJmpBytes = 14;
	constexpr int         kCutDepth = 32;  // a Steam real nao corta: estoura a pilha

	struct Detour
	{
		std::uint8_t* target = nullptr;
		std::uint8_t  saved[kJmpBytes]{};
	};
	Detour           g_detours[4];
	int              g_detourCount = 0;
	Detour*          g_original = nullptr;  // o trampolim global unico
	thread_local int t_depth = 0;
	int              g_maxDepth = 0;
	bool             g_loop = false;
	std::uint64_t    g_calls = 0;
	int              g_hooked = 0;

	HRESULT STDMETHODCALLTYPE Hook(IDXGISwapChain* sc, UINT sync, UINT flags);

	void Jmp(std::uint8_t (&out)[kJmpBytes], const void* dest)
	{
		out[0] = 0xFF;
		out[1] = 0x25;
		std::memset(out + 2, 0, 4);
		std::memcpy(out + 6, &dest, sizeof(dest));
	}

	void WriteCode(std::uint8_t* dst, const std::uint8_t* src)
	{
		DWORD old = 0;
		VirtualProtect(dst, kJmpBytes, PAGE_EXECUTE_READWRITE, &old);
		std::memcpy(dst, src, kJmpBytes);
		VirtualProtect(dst, kJmpBytes, old, &old);
		FlushInstructionCache(GetCurrentProcess(), dst, kJmpBytes);
	}

	bool IsDetoured(const void* target)
	{
		std::uint8_t j[kJmpBytes];
		Jmp(j, reinterpret_cast<const void*>(&Hook));
		return std::memcmp(target, j, kJmpBytes) == 0;
	}

	// Um passe de "Hooking vtable for swap chain" sobre o objeto nativo.
	void Pass(IUnknown* nativeSwapchain)
	{
		auto* target = static_cast<std::uint8_t*>((*reinterpret_cast<void***>(nativeSwapchain))[8]);
		if (IsDetoured(target) || g_detourCount == 4)
			return;  // "ja desviado por nos"
		Detour& d = g_detours[g_detourCount++];
		d.target = target;
		std::memcpy(d.saved, target, kJmpBytes);
		std::uint8_t j[kJmpBytes];
		Jmp(j, reinterpret_cast<const void*>(&Hook));
		WriteCode(target, j);
		g_original = &d;  // sobrescreve o global, como a Steam
		++g_hooked;
	}

	HRESULT CallOriginal(IDXGISwapChain* sc, UINT sync, UINT flags)
	{
		Detour* d = g_original;
		WriteCode(d->target, d->saved);
		const HRESULT hr = reinterpret_cast<PresentFn>(reinterpret_cast<void*>(d->target))(sc, sync, flags);
		std::uint8_t j[kJmpBytes];
		Jmp(j, reinterpret_cast<const void*>(&Hook));
		WriteCode(d->target, j);
		return hr;
	}

	HRESULT STDMETHODCALLTYPE Hook(IDXGISwapChain* sc, UINT sync, UINT flags)
	{
		++g_calls;
		if (t_depth >= kCutDepth)
		{
			g_loop = true;
			return E_FAIL;
		}
		++t_depth;
		g_maxDepth = std::max(g_maxDepth, t_depth);
		if (t_depth == 1 && !(flags & DXGI_PRESENT_TEST))
			VtablePresent(sc)(sc, 0, DXGI_PRESENT_TEST);  // chamada de volta PELA VTABLE do objeto nativo
		const HRESULT hr = CallOriginal(sc, sync, flags);
		--t_depth;
		return hr;
	}
} // namespace simsteam

namespace
{
	int               g_failures = 0;
	std::atomic<bool> g_gate{ true };

	void Check(bool ok, const char* what)
	{
		std::printf("  %s %s\n", ok ? "ok " : "BAD", what);
		if (!ok)
			++g_failures;
	}

	bool Gate()
	{
		return g_gate.load();
	}

	// ---- papel do Minecraft no triple buffer ---------------------------------------------------
	std::uint8_t* g_mc = nullptr;
	std::uint32_t g_mcBack = 1;  // estado inicial 0: meio = 0, leitor = 2, escritor = 1
	std::uint64_t g_frameId = 1;

	// Padrao em orientacao de imagem (y = 0 em cima): 32 linhas de cima verde opaco; resto xadrez de
	// 32 px com celulas vermelho 50% premultiplicado e celulas transparentes.
	std::uint32_t Pattern(std::uint32_t x, std::uint32_t y)
	{
		if (y < 32)
			return 0xFF00FF00u;  // RGBA bytes 00 FF 00 FF
		return ((x / 32 + y / 32) & 1) == 0 ? 0x80000080u : 0u;  // (128,0,0,128) ou (0,0,0,0)
	}

	void PublishFrame(std::uint32_t w, std::uint32_t h)
	{
		auto* px = reinterpret_cast<std::uint32_t*>(g_mc + mw3::proto::kOffOverlayPixels + mw3::proto::kOverlaySlotBytes * g_mcBack);
		for (std::uint32_t m = 0; m < h; ++m)  // bottom-up: linha m da memoria = linha h-1-m da imagem
			for (std::uint32_t x = 0; x < w; ++x)
				px[std::size_t(m) * w + x] = Pattern(x, h - 1 - m);
		auto* hdr = reinterpret_cast<mw3::proto::OverlaySlotHdr*>(g_mc + mw3::proto::kOffOverlaySlotHdr + sizeof(mw3::proto::OverlaySlotHdr) * g_mcBack);
		hdr->width = w;
		hdr->height = h;
		hdr->flags = 1;
		hdr->frameId = g_frameId++;
		auto* ctl = reinterpret_cast<mw3::proto::OverlayCtl*>(g_mc + mw3::proto::kOffOverlayCtl);
		const LONG old = InterlockedExchange(reinterpret_cast<volatile LONG*>(&ctl->state), static_cast<LONG>(g_mcBack | mw3::proto::kOverlayDirty));
		g_mcBack = static_cast<std::uint32_t>(old) & 3;
		++ctl->framesPublished;
	}

	// ---- app D3D12 minima --------------------------------------------------------------------
	HWND                         g_hwnd = nullptr;
	ComPtr<ID3D12Device>         g_device;
	ComPtr<ID3D12CommandQueue>   g_queue;
	ComPtr<IDXGIFactory4>        g_factory;
	ComPtr<IDXGISwapChain3>      g_sc;        // o proxy (o que o "jogo" chama)
	ComPtr<IDXGISwapChain4>      g_native;    // a swapchain nativa por baixo dele
	ComPtr<ID3D12CommandAllocator>    g_alloc;
	ComPtr<ID3D12GraphicsCommandList> g_list;
	ComPtr<ID3D12DescriptorHeap> g_rtvHeap;
	ComPtr<ID3D12Fence>          g_fence;
	UINT64                       g_fenceValue = 0;
	HANDLE                       g_event = nullptr;
	DXGI_FORMAT                  g_format = DXGI_FORMAT_R8G8B8A8_UNORM;

	LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
	{
		return DefWindowProcW(hwnd, msg, wp, lp);
	}

	void WaitGpu()
	{
		g_queue->Signal(g_fence.Get(), ++g_fenceValue);
		if (g_fence->GetCompletedValue() < g_fenceValue)
		{
			g_fence->SetEventOnCompletion(g_fenceValue, g_event);
			WaitForSingleObject(g_event, 5000);
		}
	}

	bool CreateSwapchain(DXGI_FORMAT format, UINT w, UINT h)
	{
		g_sc.Reset();
		g_native.Reset();
		g_format = format;
		DXGI_SWAP_CHAIN_DESC1 sd{};
		sd.Width = w;
		sd.Height = h;
		sd.Format = format;
		sd.SampleDesc.Count = 1;
		sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		sd.BufferCount = 3;
		sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		ComPtr<IDXGISwapChain1> sc1;
		if (FAILED(g_factory->CreateSwapChainForHwnd(g_queue.Get(), g_hwnd, &sd, nullptr, nullptr, &sc1)) || FAILED(sc1.As(&g_native)))
			return false;
		// O Streamline embrulha a nativa no proxy; a Steam roda o passe dela na nativa a cada swapchain.
		g_sc.Attach(new sl::interposer::DXGISwapChain(g_native.Get()));
		simsteam::Pass(g_native.Get());
		return true;
	}

	// Slots da vtable nativa que o desenho antigo escrevia (Present, ResizeBuffers, Present1,
	// SetColorSpace1, ResizeBuffers1): o novo nunca os toca.
	constexpr int kNativeSlots[] = { 8, 13, 22, 38, 39 };
	struct NativeSnapshot
	{
		void* slots[5]{};
	};
	NativeSnapshot SnapshotNative()
	{
		NativeSnapshot s;
		void** vt = *reinterpret_cast<void***>(g_native.Get());
		for (int i = 0; i < 5; ++i)
			s.slots[i] = vt[kNativeSlots[i]];
		return s;
	}
	bool SameNative(const NativeSnapshot& a)
	{
		const NativeSnapshot b = SnapshotNative();
		return std::memcmp(a.slots, b.slots, sizeof(a.slots)) == 0;
	}

	void WriteDataSlot(void** slot, void* value)
	{
		DWORD old = 0;
		VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old);
		*slot = value;
		VirtualProtect(slot, sizeof(void*), old, &old);
	}

	// ---- controle negativo: o desenho antigo --------------------------------------------------
	// HookPresent antigo: hook no slot 8 da vtable nativa que encaminha (tail call) para o valor que o
	// slot tinha. Fica atras de um thunk "mov rax, imm64; jmp rax" de 32 bytes (alvo do desvio de 14).
	std::atomic<PresentFn> g_legacyNext{ nullptr };
	std::uint64_t          g_legacyCalls = 0;
	HRESULT STDMETHODCALLTYPE LegacyHookPresent(IDXGISwapChain* sc, UINT sync, UINT flags)
	{
		++g_legacyCalls;
		return g_legacyNext.load()(sc, sync, flags);
	}

	bool RunLegacyControl()
	{
		auto* thunk = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
		if (!thunk)
			return false;
		std::memset(thunk, 0xCC, 32);
		thunk[0] = 0x48;
		thunk[1] = 0xB8;
		const void* body = reinterpret_cast<const void*>(&LegacyHookPresent);
		std::memcpy(thunk + 2, &body, sizeof(body));
		thunk[10] = 0xFF;
		thunk[11] = 0xE0;
		FlushInstructionCache(GetCurrentProcess(), thunk, 32);

		void** vt = *reinterpret_cast<void***>(g_native.Get());
		void*  before = vt[8];
		simsteam::Detour* keep = simsteam::g_original;
		const int         keepCount = simsteam::g_detourCount;
		g_legacyNext = reinterpret_cast<PresentFn>(before);  // o "original" = dxgi Present (ja desviado pela Steam)
		WriteDataSlot(&vt[8], thunk);                         // hook antigo no slot nativo
		simsteam::Pass(g_native.Get());                       // nova swapchain: a Steam desvia o NOSSO hook
		const bool detouredOurs = simsteam::g_detourCount == keepCount + 1;
		simsteam::g_loop = false;
		simsteam::g_maxDepth = 0;
		g_inPresent = true;
		const HRESULT hr = g_sc->Present(0, 0);  // pelo proxy, como o jogo
		g_inPresent = false;
		const bool loop = simsteam::g_loop;
		std::printf("    desenho antigo: Steam desviou o hook do slot nativo: %s; Present -> 0x%08X, Steam aninhada %d niveis, "
		            "hook antigo chamado %llu vezes%s\n",
		    detouredOurs ? "sim" : "nao", static_cast<unsigned>(hr), simsteam::g_maxDepth, static_cast<unsigned long long>(g_legacyCalls),
		    loop ? " -> LACO (o crash: Steam -> hook -> dxgi(jmp Steam) -> Steam ...)" : "");

		// Desfaz: slot nativo, desvio do thunk, trampolim global da Steam de volta ao dxgi.
		WriteDataSlot(&vt[8], before);
		if (detouredOurs)
		{
			simsteam::WriteCode(simsteam::g_detours[keepCount].target, simsteam::g_detours[keepCount].saved);
			simsteam::g_detourCount = keepCount;
			--simsteam::g_hooked;
		}
		simsteam::g_original = keep;
		simsteam::g_loop = false;
		simsteam::g_maxDepth = 0;
		VirtualFree(thunk, 0, MEM_RELEASE);
		return detouredOurs && loop;
	}

	void Pump()
	{
		MSG msg;
		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
		{
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
	}

	// Limpa o back buffer de azul e apresenta (o hook desenha o overlay antes do Present original).
	void RenderFrames(int count)
	{
		for (int i = 0; i < count; ++i)
		{
			Pump();
			ComPtr<ID3D12Resource> bb;
			g_sc->GetBuffer(g_sc->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&bb));
			auto rtv = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
			g_device->CreateRenderTargetView(bb.Get(), nullptr, rtv);
			g_alloc->Reset();
			g_list->Reset(g_alloc.Get(), nullptr);
			D3D12_RESOURCE_BARRIER b{};
			b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			b.Transition.pResource = bb.Get();
			b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
			b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
			g_list->ResourceBarrier(1, &b);
			const float blue[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
			g_list->ClearRenderTargetView(rtv, blue, 0, nullptr);
			std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
			g_list->ResourceBarrier(1, &b);
			g_list->Close();
			ID3D12CommandList* lists[] = { g_list.Get() };
			g_queue->ExecuteCommandLists(1, lists);
			g_inPresent = true;
			g_sc->Present(1, 0);
			g_inPresent = false;
			WaitGpu();  // simples: uma alocadora so
		}
	}

	bool Near(int a, int b, int tol = 2)
	{
		return std::abs(a - b) <= tol;
	}

	// Confere o readback RGBA8 contra o padrao composto sobre azul.
	bool CheckComposite(std::uint32_t expectW, std::uint32_t expectH)
	{
		std::uint32_t              w = 0, h = 0;
		std::vector<std::uint32_t> px;
		if (!mw3::overlay::DebugReadback(w, h, px) || w != expectW || h != expectH)
		{
			std::printf("    readback %ux%u (esperado %ux%u)\n", w, h, expectW, expectH);
			return false;
		}
		bool ok = true;
		const std::uint32_t pts[][2] = { { 16, 10 }, { 300, 20 }, { 16, 48 }, { 48, 48 }, { 80, 112 }, { 112, 112 }, { w - 20, h - 20 }, { w / 2 + 5, h / 2 + 5 } };
		for (const auto& p : pts)
		{
			const std::uint32_t v = px[std::size_t(p[1]) * w + p[0]];
			const int r = v & 0xFF, g = (v >> 8) & 0xFF, bl = (v >> 16) & 0xFF, a = (v >> 24) & 0xFF;
			const std::uint32_t pat = Pattern(p[0], p[1]);
			int er, eg, eb;
			if (pat == 0xFF00FF00u)
				er = 0, eg = 255, eb = 0;
			else if (pat == 0x80000080u)
				er = 128, eg = 0, eb = 127;
			else
				er = 0, eg = 0, eb = 255;
			const bool good = Near(r, er) && Near(g, eg) && Near(bl, eb) && Near(a, 255);
			std::printf("    (%3u,%3u) = %3d %3d %3d %3d  esperado %3d %3d %3d 255  %s\n", p[0], p[1], r, g, bl, a, er, eg, eb, good ? "" : "<-- BAD");
			ok = ok && good;
		}
		return ok;
	}
} // namespace

int main()
{
	using namespace mw3;
	InitLog();

	auto& link = Link::Get();
	Check(link.Create(), "Link::Create");
	HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, proto::kMappingName);
	g_mc = mapping ? static_cast<std::uint8_t*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0)) : nullptr;
	Check(g_mc != nullptr, "view do mapeamento como o MC");
	if (!g_mc)
		return 1;

	WNDCLASSEXW wc{};
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = WndProc;
	wc.hInstance = GetModuleHandleW(nullptr);
	wc.lpszClassName = L"mw3_overlaytest";
	RegisterClassExW(&wc);
	RECT rc{ 0, 0, 640, 360 };
	AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
	g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"mw3_overlaytest", WS_OVERLAPPEDWINDOW, 100, 100, rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, wc.hInstance, nullptr);
	ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);

	Check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&g_factory))), "CreateDXGIFactory1");
	Check(SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_device))), "D3D12CreateDevice");
	if (!g_device)
		return 1;
	D3D12_COMMAND_QUEUE_DESC qd{};
	qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	g_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_queue));
	g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_alloc));
	g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc.Get(), nullptr, IID_PPV_ARGS(&g_list));
	g_list->Close();
	D3D12_DESCRIPTOR_HEAP_DESC hd{};
	hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	hd.NumDescriptors = 1;
	g_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_rtvHeap));
	g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence));
	g_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	Check(CreateSwapchain(DXGI_FORMAT_R8G8B8A8_UNORM, 640, 360), "swapchain RGBA8 640x360");

	// Alguns frames sem hook (como o jogo antes de o mw3.asi instalar), ja com a "Steam".
	RenderFrames(5);
	Check(simsteam::g_hooked == 1 && simsteam::IsDetoured(reinterpret_cast<const void*>(VtablePresent(g_native.Get()))), "\"Steam\" desviou o Present nativo do dxgi");
	Check(!simsteam::g_loop && simsteam::g_maxDepth == 2, "\"Steam\" sozinha: chamada de volta pela vtable aninha 2 niveis, sem laco");

	std::printf("controle negativo: hook no slot nativo (desenho antigo) + passe da Steam\n");
	Check(RunLegacyControl(), "desenho antigo reproduz o laco infinito do crash");
	Check(!simsteam::g_loop && simsteam::IsDetoured(reinterpret_cast<const void*>(VtablePresent(g_native.Get()))), "controle desfeito");
	RenderFrames(2);

	const NativeSnapshot native0 = SnapshotNative();
	overlay::Options options;
	options.shouldDraw = Gate;
	options.debugReadback = true;
	options.proxyModule = GetModuleHandleW(nullptr);  // o "sl::interposer::DXGISwapChain" deste exe
	Check(overlay::Install(options), "overlay::Install (vtable do proxy achada pelo RTTI)");
	Check(SameNative(native0), "vtable nativa da swapchain intacta depois do Install");
	Check(overlay::GetStats().proxySlotsActive == 5, "5 slots do proxy com hook");

	const auto beat0 = *reinterpret_cast<volatile std::uint64_t*>(g_mc + 0x10);
	Sleep(20);

	// ---- fase 1: RGBA8 640x360 -----------------------------------------------------------------
	std::printf("fase 1: RGBA8 640x360\n");
	const auto steamCalls0 = simsteam::g_calls;
	const int  reent0 = g_proxyReentries;
	auto       s0 = overlay::GetStats();
	RenderFrames(10);
	auto s = overlay::GetStats();
	std::printf("    presents +%llu reentradas +%llu (proxy reentrou %d) steam: chamadas +%llu, aninhamento max %d, laco %s\n",
	    (unsigned long long)(s.presents - s0.presents), (unsigned long long)(s.reentries - s0.reentries), g_proxyReentries - reent0,
	    (unsigned long long)(simsteam::g_calls - steamCalls0), simsteam::g_maxDepth, simsteam::g_loop ? "SIM" : "nao");
	Check(s.presents - s0.presents == 10, "hook de Present disparou 1x por frame na swapchain criada antes do hook");
	Check(s.reentries - s0.reentries == 10, "reentrada pela vtable do proxy so repassada (sem desenho, sem contar Present)");
	Check(simsteam::g_calls - steamCalls0 >= 20 && !simsteam::g_loop && simsteam::g_maxDepth == 2,
	    "\"Steam\" na cadeia (chamada de volta pela vtable nativa) e sem recursao");
	Check(s.draws == 0, "sem frame do MC: nada desenhado");
	PublishFrame(640, 360);
	RenderFrames(30);
	s = overlay::GetStats();
	std::printf("    presents %llu draws %llu uploads %llu ecl %llu fila %s formato %u modo %d\n", (unsigned long long)s.presents,
	    (unsigned long long)s.draws, (unsigned long long)s.uploads, (unsigned long long)s.eclCalls,
	    s.queueFromSwapchain ? "swapchain" : (s.queueFromEcl ? "ECL" : "?"), s.backBufferFormat, s.colorMode);
	Check(s.draws >= 30 && s.uploads == 1, "30 draws, 1 upload");
	std::printf("    fila via %s\n", s.queueFromSwapchain ? "IDXGISwapChain::GetDevice" : "hook de ExecuteCommandLists (GetDevice da swapchain nao da a fila)");
	Check(s.queueFromSwapchain || s.queueFromEcl, "fila do jogo capturada");
	Check(s.eclCalls > 0, "hook de ExecuteCommandLists disparou");
	Check(!s.eclHookActive, "hook nativo de ExecuteCommandLists retirado depois de achar a fila");
	Check(CheckComposite(640, 360), "composicao premultiplicada correta (e flip bottom-up)");
	const auto beat1 = *reinterpret_cast<volatile std::uint64_t*>(g_mc + 0x10);
	Check(beat1 > beat0, "heartbeat do jogo batido pelo Present");

	// ---- fase 2: ResizeBuffers 800x450 ---------------------------------------------------------
	std::printf("fase 2: ResizeBuffers 800x450\n");
	RECT rc2{ 0, 0, 800, 450 };
	AdjustWindowRect(&rc2, WS_OVERLAPPEDWINDOW, FALSE);
	SetWindowPos(g_hwnd, nullptr, 0, 0, rc2.right - rc2.left, rc2.bottom - rc2.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
	WaitGpu();
	Check(SUCCEEDED(g_sc->ResizeBuffers(0, 800, 450, DXGI_FORMAT_UNKNOWN, 0)), "ResizeBuffers com o overlay ativo");
	RenderFrames(5);  // frame 640x360 esticado ate chegar um 800x450
	PublishFrame(800, 450);
	RenderFrames(20);
	Check(CheckComposite(800, 450), "composicao apos resize");

	// ---- fase 3: gate desligado (MC fora do mundo / menu do W3) ----------------------------------
	std::printf("fase 3: gate desligado\n");
	g_gate = false;
	const auto drawsBefore = overlay::GetStats().draws;
	RenderFrames(10);
	Check(overlay::GetStats().draws == drawsBefore, "gate falso: nenhum draw");
	g_gate = true;
	RenderFrames(3);
	Check(overlay::GetStats().draws == drawsBefore, "gate de volta sem frame novo: nao reaparece frame velho");
	PublishFrame(800, 450);
	RenderFrames(5);
	Check(overlay::GetStats().draws > drawsBefore, "gate de volta + frame novo: desenha");

	// ---- fase 4: R10G10B10A2 + HDR10 -------------------------------------------------------------
	std::printf("fase 4: R10G10B10A2 + SetColorSpace1(G2084_P2020)\n");
	WaitGpu();
	Check(CreateSwapchain(DXGI_FORMAT_R10G10B10A2_UNORM, 800, 450), "swapchain R10G10B10A2");
	UINT support = 0;
	g_sc->CheckColorSpaceSupport(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020, &support);
	const bool csOk = SUCCEEDED(g_sc->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020));
	std::printf("    SetColorSpace1(HDR10) %s (support 0x%X)\n", csOk ? "aceito" : "recusado (monitor sem HDR)", support);
	auto d0 = overlay::GetStats().draws;
	PublishFrame(800, 450);
	RenderFrames(15);
	s = overlay::GetStats();
	std::printf("    draws +%llu formato %u modo %d\n", (unsigned long long)(s.draws - d0), s.backBufferFormat, s.colorMode);
	Check(s.draws > d0 && s.backBufferFormat == DXGI_FORMAT_R10G10B10A2_UNORM, "desenha em R10G10B10A2");
	Check(s.colorMode == (csOk ? 2 : 0), "modo de cor segue o SetColorSpace1");
	{
		std::uint32_t              w = 0, h = 0;
		std::vector<std::uint32_t> px;
		if (overlay::DebugReadback(w, h, px) && w == 800)
		{
			auto unpack = [](std::uint32_t v, int& r, int& g, int& b) { r = v & 0x3FF, g = (v >> 10) & 0x3FF, b = (v >> 20) & 0x3FF; };
			int r, g, b;
			unpack(px[10 * w + 16], r, g, b);
			std::printf("    verde (16,10) = %d %d %d\n", r, g, b);
			Check(g > r && g > b, "verde no topo (10 bits)");
			unpack(px[48 * w + 16], r, g, b);
			std::printf("    transparente (16,48) = %d %d %d\n", r, g, b);
			Check(r == 0 && g == 0 && b == 1023, "celula transparente mantem o azul do jogo");
		}
		else
			Check(false, "readback 10 bits");
	}

	// ---- fase 5: FP16 (scRGB) ------------------------------------------------------------------
	std::printf("fase 5: R16G16B16A16_FLOAT\n");
	WaitGpu();
	Check(CreateSwapchain(DXGI_FORMAT_R16G16B16A16_FLOAT, 800, 450), "swapchain FP16");
	d0 = overlay::GetStats().draws;
	PublishFrame(800, 450);
	RenderFrames(15);
	s = overlay::GetStats();
	Check(s.draws > d0 && s.backBufferFormat == DXGI_FORMAT_R16G16B16A16_FLOAT && s.colorMode == 1, "desenha em FP16, modo scRGB");
	Check(!s.disabled, "overlay nao se desligou");
	Check(simsteam::g_hooked == 1 && !simsteam::g_loop && simsteam::g_maxDepth == 2, "\"Steam\": 3 swapchains novas, nada novo desviado, sem laco");
	Check(SameNative(native0), "vtable nativa intacta no fim");

	// ---- fase 6: excecao estrutural no desenho -> desliga E restaura os slots ---------------------
	std::printf("fase 6: excecao no desenho\n");
	PublishFrame(800, 450);
	g_throwInDrawGetBuffer = true;
	RenderFrames(3);
	s = overlay::GetStats();
	std::printf("    desligado %s, slots do proxy com hook %d, ECL %s\n", s.disabled ? "sim" : "nao", s.proxySlotsActive, s.eclHookActive ? "ativo" : "fora");
	Check(!g_throwInDrawGetBuffer && s.disabled, "excecao pega: overlay desligado");
	Check(s.proxySlotsActive == 0 && !s.eclHookActive, "todos os slots restaurados");
	const auto presentsOff = s.presents;
	const auto steamOff = simsteam::g_calls;
	RenderFrames(5);
	s = overlay::GetStats();
	Check(s.presents == presentsOff && simsteam::g_calls > steamOff && !simsteam::g_loop, "depois de desligado: jogo apresenta direto pelo proxy, sem o hook");
	Check(SameNative(native0), "vtable nativa intacta");

	WaitGpu();
	g_sc.Reset();
	DestroyWindow(g_hwnd);
	std::printf("%s (%d falha(s))\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
	return g_failures == 0 ? 0 : 1;
}

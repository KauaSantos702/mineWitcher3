// Derived from SkyCraft (https://github.com/chasmlol/skycraft), Copyright (c) 2026 chasmlol, MIT License.
// D3D12 port of skse/src/Overlay.cpp: Present hook, upload of Minecraft's premultiplied RGBA frame
// (triple buffer in shared memory), fullscreen triangle with premultiplied-alpha blending over the
// back buffer, heartbeat from Present. See THIRD-PARTY-NOTICES.md.
//
// Hooks (research/01-render.md, "Overlay da Steam"): the game presents through the NVIDIA Streamline
// swapchain proxy (class sl::interposer::DXGISwapChain, static vtable in sl.interposer.dll .rdata,
// found through its MSVC RTTI). We patch the slots of THAT vtable, i.e. the layer the game calls,
// above Streamline, DLSS frame generation and every native-level hook (Steam overlay, RTSS):
//   game -> [proxy vtable] mw3 hook -> SL proxy Present -> SL plugins (dlss_g) -> native dxgi vtable
//        -> Steam inline detour -> dxgi.dll
// The native dxgi swapchain vtable is never written: the Steam overlay re-runs "Hooking vtable for
// swap chain" on every new swapchain and inline-detours whatever function each native slot points
// to, keeping ONE global trampoline per method. A native-slot hook of ours got detoured too, the
// global was overwritten and Present looped Steam -> mw3 -> dxgi(jmp Steam) -> Steam ... until the
// stack overflowed (crash dumps of 2026-10-11). Steam skips vtables outside dxgi.dll, so it never
// touches the proxy vtable or our functions.
// Each hook forwards to the saved slot value (the SL function), never through a vtable, and keeps a
// thread_local nesting depth across the forwarded call: a nested call only forwards, and a runaway
// nesting (someone else's loop) disables the overlay, restores the slots and cuts the loop.
// The only native write is a transient ExecuteCommandLists capture on the D3D12Core queue vtable,
// used just until the presenting queue is known and then removed.
#include "overlay.h"

#include "link.h"
#include "log.h"

#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <malloc.h>  // _resetstkoflw

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

// Gerados pelo fxc no build (native/CMakeLists.txt): const BYTE kOverlayVS[] / kOverlayPS[].
#include "overlay_ps.h"
#include "overlay_vs.h"

using Microsoft::WRL::ComPtr;

namespace mw3::overlay
{
	namespace
	{
		// Indices nas vtables (ordem de declaracao em dxgi1_4.h / d3d12.h).
		constexpr int kSlotPresent = 8;          // IDXGISwapChain::Present
		constexpr int kSlotResizeBuffers = 13;   // IDXGISwapChain::ResizeBuffers
		constexpr int kSlotPresent1 = 22;        // IDXGISwapChain1::Present1
		constexpr int kSlotSetColorSpace1 = 38;  // IDXGISwapChain3::SetColorSpace1
		constexpr int kSlotResizeBuffers1 = 39;  // IDXGISwapChain3::ResizeBuffers1
		constexpr int kSlotExecuteCommandLists = 10;  // ID3D12CommandQueue::ExecuteCommandLists
		constexpr int kProxyCheckedSlots = 40;        // QueryInterface..ResizeBuffers1 conferidos no proxy

		// Classe do proxy de swapchain do Streamline (nome decorado do RTTI do MSVC) e o modulo dela.
		constexpr char    kProxyClassRtti[] = ".?AUDXGISwapChain@interposer@sl@@";
		constexpr wchar_t kProxyModuleName[] = L"sl.interposer.dll";

		constexpr UINT      kFrameContexts = 3;
		constexpr DWORD     kFenceTimeoutMs = 2000;
		constexpr int       kSwapchainSwitchStreak = 3;  // Presents seguidos de outra swapchain para trocar
		constexpr int       kMaxQueueCandidates = 4;
		// Aninhamento de Present/Resize na mesma thread a partir do qual so pode ser um laco de hooks
		// de terceiros (o legitimo e 0 ou 1): desliga o overlay, restaura os slots e corta o laco.
		constexpr int kMaxNesting = 4;

		using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
		using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
		using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
		using ResizeBuffers1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT*, IUnknown* const*);
		using SetColorSpace1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, DXGI_COLOR_SPACE_TYPE);
		using ExecuteCommandListsFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
		using CreateFactory1Fn = HRESULT(WINAPI*)(REFIID, void**);

		std::atomic<PresentFn>             g_origPresent{ nullptr };
		std::atomic<Present1Fn>            g_origPresent1{ nullptr };
		std::atomic<ResizeBuffersFn>       g_origResizeBuffers{ nullptr };
		std::atomic<ResizeBuffers1Fn>      g_origResizeBuffers1{ nullptr };
		std::atomic<SetColorSpace1Fn>      g_origSetColorSpace1{ nullptr };
		std::atomic<ExecuteCommandListsFn> g_origExecuteCommandLists{ nullptr };

		PFN_D3D12_SERIALIZE_ROOT_SIGNATURE g_serializeRootSignature = nullptr;

		Options           g_options;  // escrito antes dos hooks; so leitura depois
		std::mutex        g_installLock;
		std::atomic<bool> g_installed{ false };
		std::atomic<bool> g_disabled{ false };

		std::atomic<std::uint64_t> g_presents{ 0 };
		std::atomic<std::uint64_t> g_draws{ 0 };
		std::atomic<std::uint64_t> g_uploads{ 0 };
		std::atomic<std::uint64_t> g_eclCalls{ 0 };
		std::atomic<std::uint64_t> g_reentries{ 0 };

		// Um slot de vtable que escrevemos: valor original (para onde o hook encaminha e o que a
		// restauracao devolve) e o nosso hook. So restauramos se o slot ainda for o nosso.
		struct SlotPatch
		{
			void**      slot = nullptr;
			void*       hook = nullptr;
			void*       original = nullptr;
			const char* name = "";
			bool        active = false;
		};
		constexpr int     kMaxProxyPatches = 5;
		std::mutex        g_patchLock;  // ordem: g_lock -> g_patchLock (nunca o contrario)
		SlotPatch         g_proxyPatches[kMaxProxyPatches];
		int               g_proxyPatchCount = 0;
		SlotPatch         g_eclPatch;               // vtable nativa da fila (D3D12Core): transitorio
		std::atomic<bool> g_eclCapture{ false };    // o hook de ECL guarda filas DIRECT

		// Espaco de cor do SetColorSpace1, guardado como private data na propria swapchain nativa (morre
		// com ela: uma swapchain recriada no mesmo endereco nao herda o valor da anterior).
		// {5B0C7A4E-3D1F-4E8B-9C2A-6F1D2E3B4A51}
		constexpr GUID kColorSpaceGuid = { 0x5b0c7a4e, 0x3d1f, 0x4e8b, { 0x9c, 0x2a, 0x6f, 0x1d, 0x2e, 0x3b, 0x4a, 0x51 } };

		// Filas DIRECT vistas no ExecuteCommandLists: plano B quando GetDevice da swapchain falha.
		std::mutex                 g_queueLock;
		ComPtr<ID3D12CommandQueue> g_queues[kMaxQueueCandidates];
		int                        g_queueNext = 0;

		thread_local int                 t_depth = 0;  // hooks de swapchain aninhados nesta thread
		thread_local ID3D12CommandQueue* t_lastDirectQueue = nullptr;  // so comparacao (sem AddRef)

		std::string Hex(std::uint64_t v)
		{
			char buf[32];
			std::snprintf(buf, sizeof(buf), "0x%llX", static_cast<unsigned long long>(v));
			return buf;
		}

		std::string Hr(HRESULT hr)
		{
			return Hex(static_cast<std::uint32_t>(hr));
		}

		std::string ModuleOf(const void* address)
		{
			HMODULE module = nullptr;
			if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			        static_cast<LPCWSTR>(address), &module))
				return "?";
			char path[MAX_PATH]{};
			GetModuleFileNameA(module, path, MAX_PATH);
			const char* slash = std::strrchr(path, '\\');
			return std::string(slash ? slash + 1 : path) + "+" +
			       Hex(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(address) - reinterpret_cast<std::uintptr_t>(module)));
		}

		bool WriteSlot(void** slot, void* value)
		{
			DWORD oldProtect = 0;
			if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect))
				return false;
			InterlockedExchangePointer(slot, value);
			VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);
			return true;
		}

		// g_patchLock tomado. Devolve o valor original ao slot se ele ainda tiver o nosso hook. Se outro
		// modulo escreveu por cima (encadeou em nos), deixa como esta: o nosso hook segue como
		// repasse puro para o original guardado e nunca volta por uma vtable.
		void UnpatchLocked(SlotPatch& p, bool quiet)
		{
			if (!p.active)
				return;
			p.active = false;
			void* now = *p.slot;
			if (now == p.hook)
			{
				const bool ok = WriteSlot(p.slot, p.original);
				if (!quiet || !ok)
					Log(std::string("overlay: ") + p.name + (ok ? " restaurado (" + ModuleOf(p.original) + ")" : " NAO restaurado (VirtualProtect)"));
			}
			else
			{
				Log(std::string("overlay: ") + p.name + " foi reescrito por outro modulo (" + ModuleOf(now) +
				    "); o hook fica como repasse puro");
			}
		}

		void RestoreAll()
		{
			std::lock_guard<std::mutex> guard(g_patchLock);
			g_eclCapture = false;
			for (int i = 0; i < g_proxyPatchCount; ++i)
				UnpatchLocked(g_proxyPatches[i], false);
			UnpatchLocked(g_eclPatch, false);
		}

		// Desligar = parar de desenhar E tirar os hooks das vtables (os hooks continuam repassando para
		// quem ainda os tiver guardado).
		void Disable(const std::string& why)
		{
			if (!g_disabled.exchange(true))
			{
				Log("overlay: DESLIGADO ate o jogo fechar: " + why);
				RestoreAll();
			}
		}

		// Captura de filas pelo ECL so enquanto nao temos a fila: liga/desliga o slot nativo.
		void ArmEclCapture()
		{
			std::lock_guard<std::mutex> guard(g_patchLock);
			if (g_disabled.load() || !g_eclPatch.slot)
				return;
			if (!g_eclPatch.active)
			{
				if (*g_eclPatch.slot != g_eclPatch.original || !WriteSlot(g_eclPatch.slot, g_eclPatch.hook))
					return;  // alguem mexeu no slot depois de nos: nao reinstala
				g_eclPatch.active = true;
				Log("overlay: hook de ExecuteCommandLists religado (procurando a fila)");
			}
			g_eclCapture = true;
		}

		void DisarmEclCapture()
		{
			std::lock_guard<std::mutex> guard(g_patchLock);
			g_eclCapture = false;
			if (g_eclPatch.active)
			{
				UnpatchLocked(g_eclPatch, true);
				Log("overlay: hook de ExecuteCommandLists retirado (fila conhecida)");
			}
		}

		void LogOnce(bool& flag, const std::string& text)
		{
			if (!flag)
			{
				flag = true;
				Log("overlay: " + text);
			}
		}

		struct alignas(16) Params
		{
			float cursor[2];
			float viewport[2];
			float cursorOn;
			float flipY;
			float mode;
			float paperWhite;
		};
		static_assert(sizeof(Params) == 8 * 4);

		struct FrameContext
		{
			ComPtr<ID3D12CommandAllocator> allocator;
			ComPtr<ID3D12Resource>         upload;
			UINT64                         uploadBytes = 0;
			std::uint8_t*                  uploadPtr = nullptr;
			UINT64                         fenceValue = 0;
		};

		// Tudo sob g_lock (Present e ResizeBuffers podem vir de threads diferentes).
		struct Renderer
		{
			IDXGISwapChain* swapchain = nullptr;  // so identidade (sem AddRef)
			int             otherStreak = 0;

			ComPtr<ID3D12CommandQueue> queue;
			bool                       queueFromSwapchain = false;
			ComPtr<ID3D12Device>       device;

			ComPtr<ID3D12RootSignature>                                  rootSignature;
			std::vector<std::pair<DXGI_FORMAT, ComPtr<ID3D12PipelineState>>> psos;
			ComPtr<ID3D12DescriptorHeap>                                  rtvHeap;
			UINT                                                          rtvCount = 0;
			UINT                                                          rtvStride = 0;
			ComPtr<ID3D12DescriptorHeap>                                  srvHeap;
			ComPtr<ID3D12GraphicsCommandList>                             list;
			ComPtr<ID3D12Fence>                                           fence;
			HANDLE                                                        fenceEvent = nullptr;
			UINT64                                                        fenceValue = 0;
			FrameContext                                                  frames[kFrameContexts];
			UINT                                                          frameIndex = 0;

			ComPtr<ID3D12Resource> texture;
			UINT                   texW = 0, texH = 0;
			D3D12_RESOURCE_STATES  texState = D3D12_RESOURCE_STATE_COPY_DEST;
			bool                   haveFrame = false;
			bool                   pendingUpload = false;
			bool                   flipY = true;

			DXGI_FORMAT lastFormat = DXGI_FORMAT_UNKNOWN;
			int         lastMode = -1;
			bool        outputHdr = false;
			UINT        lastW = 0, lastH = 0;

			ComPtr<ID3D12Resource> readback;
			UINT64                 readbackBytes = 0;

			// log-once
			bool loggedNoQueue = false, loggedGetBuffer = false, loggedRemoved = false, loggedFormat = false;
			bool loggedBadFrame = false, loggedFirstDraw = false, loggedNo3 = false;
		};

		std::mutex                 g_lock;
		Renderer                   g_r;
		std::uint32_t              g_readbackW = 0, g_readbackH = 0;
		std::vector<std::uint32_t> g_readbackPixels;

		D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
		{
			D3D12_RESOURCE_BARRIER b{};
			b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			b.Transition.pResource = resource;
			b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			b.Transition.StateBefore = before;
			b.Transition.StateAfter = after;
			return b;
		}

		bool WaitFence(UINT64 value)
		{
			auto& r = g_r;
			if (!r.fence || value == 0 || r.fence->GetCompletedValue() >= value)
				return true;
			if (FAILED(r.fence->SetEventOnCompletion(value, r.fenceEvent)))
				return false;
			return WaitForSingleObject(r.fenceEvent, kFenceTimeoutMs) == WAIT_OBJECT_0;
		}

		bool WaitIdle()
		{
			return WaitFence(g_r.fenceValue);
		}

		void ReleaseDeviceObjects()
		{
			auto& r = g_r;
			if (r.fence && !WaitIdle())
				Log("overlay: GPU nao terminou o trabalho do overlay a tempo ao liberar recursos");
			for (auto& f : r.frames)
			{
				if (f.upload && f.uploadPtr)
					f.upload->Unmap(0, nullptr);
				f = FrameContext{};
			}
			r.texture.Reset();
			r.texW = r.texH = 0;
			r.haveFrame = false;
			r.readback.Reset();
			r.readbackBytes = 0;
			r.list.Reset();
			r.fence.Reset();
			r.fenceValue = 0;
			r.srvHeap.Reset();
			r.rtvHeap.Reset();
			r.rtvCount = 0;
			r.psos.clear();
			r.rootSignature.Reset();
			r.queue.Reset();
			r.device.Reset();
		}

		bool CreateDeviceObjects()
		{
			auto& r = g_r;
			auto* dev = r.device.Get();

			D3D12_ROOT_PARAMETER params[2]{};
			params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
			params[0].Constants.ShaderRegister = 0;
			params[0].Constants.Num32BitValues = sizeof(Params) / 4;
			params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
			D3D12_DESCRIPTOR_RANGE range{};
			range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
			range.NumDescriptors = 1;
			range.BaseShaderRegister = 0;
			range.OffsetInDescriptorsFromTableStart = 0;
			params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			params[1].DescriptorTable.NumDescriptorRanges = 1;
			params[1].DescriptorTable.pDescriptorRanges = &range;
			params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
			D3D12_STATIC_SAMPLER_DESC sampler{};
			sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
			sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			sampler.MaxLOD = D3D12_FLOAT32_MAX;
			sampler.ShaderRegister = 0;
			sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
			D3D12_ROOT_SIGNATURE_DESC rsDesc{};
			rsDesc.NumParameters = 2;
			rsDesc.pParameters = params;
			rsDesc.NumStaticSamplers = 1;
			rsDesc.pStaticSamplers = &sampler;
			rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS | D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
			               D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS | D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;
			ComPtr<ID3DBlob> blob, errors;
			HRESULT          hr = g_serializeRootSignature ? g_serializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors) : E_NOINTERFACE;
			if (FAILED(hr))
			{
				Log("overlay: D3D12SerializeRootSignature falhou " + Hr(hr) +
				    (errors ? std::string(": ") + static_cast<const char*>(errors->GetBufferPointer()) : std::string()));
				return false;
			}
			hr = dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&r.rootSignature));
			if (FAILED(hr))
			{
				Log("overlay: CreateRootSignature falhou " + Hr(hr));
				return false;
			}

			D3D12_DESCRIPTOR_HEAP_DESC srv{};
			srv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
			srv.NumDescriptors = 1;
			srv.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
			hr = dev->CreateDescriptorHeap(&srv, IID_PPV_ARGS(&r.srvHeap));
			if (FAILED(hr))
			{
				Log("overlay: heap SRV falhou " + Hr(hr));
				return false;
			}
			r.rtvStride = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

			hr = dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&r.fence));
			if (FAILED(hr))
			{
				Log("overlay: CreateFence falhou " + Hr(hr));
				return false;
			}
			if (!r.fenceEvent)
				r.fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			if (!r.fenceEvent)
				return false;

			for (auto& f : r.frames)
			{
				hr = dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.allocator));
				if (FAILED(hr))
				{
					Log("overlay: CreateCommandAllocator falhou " + Hr(hr));
					return false;
				}
			}
			hr = dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, r.frames[0].allocator.Get(), nullptr, IID_PPV_ARGS(&r.list));
			if (FAILED(hr))
			{
				Log("overlay: CreateCommandList falhou " + Hr(hr));
				return false;
			}
			r.list->Close();
			Log("overlay: recursos D3D12 criados");
			return true;
		}

		ID3D12PipelineState* GetPso(DXGI_FORMAT format)
		{
			auto& r = g_r;
			for (auto& [f, pso] : r.psos)
				if (f == format)
					return pso.Get();

			D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
			d.pRootSignature = r.rootSignature.Get();
			d.VS = { kOverlayVS, sizeof(kOverlayVS) };
			d.PS = { kOverlayPS, sizeof(kOverlayPS) };
			auto& rt = d.BlendState.RenderTarget[0];
			rt.BlendEnable = TRUE;
			rt.SrcBlend = D3D12_BLEND_ONE;
			rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
			rt.BlendOp = D3D12_BLEND_OP_ADD;
			rt.SrcBlendAlpha = D3D12_BLEND_ONE;
			rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
			rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
			rt.LogicOp = D3D12_LOGIC_OP_NOOP;
			rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
			d.SampleMask = UINT_MAX;
			d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
			d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
			d.RasterizerState.DepthClipEnable = TRUE;
			d.DepthStencilState.DepthEnable = FALSE;
			d.DepthStencilState.StencilEnable = FALSE;
			d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			d.NumRenderTargets = 1;
			d.RTVFormats[0] = format;
			d.SampleDesc.Count = 1;
			ComPtr<ID3D12PipelineState> pso;
			const HRESULT               hr = r.device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso));
			if (FAILED(hr))
			{
				Disable("CreateGraphicsPipelineState (formato " + std::to_string(static_cast<int>(format)) + ") falhou " + Hr(hr));
				return nullptr;
			}
			r.psos.emplace_back(format, pso);
			return pso.Get();
		}

		bool EnsureRtvHeap(UINT count)
		{
			auto& r = g_r;
			if (r.rtvHeap && r.rtvCount >= count)
				return true;
			r.rtvHeap.Reset();  // so descritores de CPU: nada na GPU depende dele
			D3D12_DESCRIPTOR_HEAP_DESC d{};
			d.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
			d.NumDescriptors = std::max<UINT>(count, 4);
			const HRESULT hr = r.device->CreateDescriptorHeap(&d, IID_PPV_ARGS(&r.rtvHeap));
			if (FAILED(hr))
			{
				Disable("heap RTV falhou " + Hr(hr));
				return false;
			}
			r.rtvCount = d.NumDescriptors;
			return true;
		}

		bool EnsureTexture(UINT w, UINT h)
		{
			auto& r = g_r;
			if (r.texture && r.texW == w && r.texH == h)
				return true;
			if (!WaitIdle())
				return false;
			r.texture.Reset();
			r.haveFrame = false;

			D3D12_HEAP_PROPERTIES heap{};
			heap.Type = D3D12_HEAP_TYPE_DEFAULT;
			D3D12_RESOURCE_DESC d{};
			d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			d.Width = w;
			d.Height = h;
			d.DepthOrArraySize = 1;
			d.MipLevels = 1;
			d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			d.SampleDesc.Count = 1;
			d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
			const HRESULT hr = r.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&r.texture));
			if (FAILED(hr))
			{
				Log("overlay: textura " + std::to_string(w) + "x" + std::to_string(h) + " falhou " + Hr(hr));
				return false;
			}
			r.texState = D3D12_RESOURCE_STATE_COPY_DEST;
			D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
			srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srv.Texture2D.MipLevels = 1;
			r.device->CreateShaderResourceView(r.texture.Get(), &srv, r.srvHeap->GetCPUDescriptorHandleForHeapStart());
			r.texW = w;
			r.texH = h;
			Log("overlay: textura " + std::to_string(w) + "x" + std::to_string(h));
			return true;
		}

		bool EnsureBuffer(ComPtr<ID3D12Resource>& buffer, UINT64& have, UINT64 bytes, D3D12_HEAP_TYPE type)
		{
			if (buffer && have >= bytes)
				return true;
			buffer.Reset();
			have = 0;
			D3D12_HEAP_PROPERTIES heap{};
			heap.Type = type;
			D3D12_RESOURCE_DESC d{};
			d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			d.Width = bytes;
			d.Height = 1;
			d.DepthOrArraySize = 1;
			d.MipLevels = 1;
			d.Format = DXGI_FORMAT_UNKNOWN;
			d.SampleDesc.Count = 1;
			d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			const auto    state = type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST;
			const HRESULT hr = g_r.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&buffer));
			if (FAILED(hr))
			{
				Log("overlay: buffer de " + std::to_string(bytes) + " bytes falhou " + Hr(hr));
				return false;
			}
			have = bytes;
			return true;
		}

		bool EnsureUpload(FrameContext& f, UINT64 bytes)
		{
			if (f.upload && f.uploadBytes >= bytes)
				return true;
			if (f.upload && f.uploadPtr)
				f.upload->Unmap(0, nullptr);
			f.uploadPtr = nullptr;
			if (!EnsureBuffer(f.upload, f.uploadBytes, bytes, D3D12_HEAP_TYPE_UPLOAD))
				return false;
			void*             p = nullptr;
			const D3D12_RANGE none{ 0, 0 };
			if (FAILED(f.upload->Map(0, &none, &p)) || !p)
			{
				f.upload.Reset();
				f.uploadBytes = 0;
				return false;
			}
			f.uploadPtr = static_cast<std::uint8_t*>(p);
			return true;
		}

		// Plano B: fila DIRECT vista no ExecuteCommandLists, do mesmo device do back buffer.
		ComPtr<ID3D12CommandQueue> FallbackQueue(IDXGISwapChain* sc)
		{
			ComPtr<ID3D12Resource> bb;
			ComPtr<ID3D12Device>   bbDevice;
			if (FAILED(sc->GetBuffer(0, IID_PPV_ARGS(&bb))) || FAILED(bb->GetDevice(IID_PPV_ARGS(&bbDevice))))
				return {};
			// Preferencia: a fila DIRECT em que esta mesma thread (a que apresenta) submeteu por ultimo; depois
			// a que ja usamos (o hook de ECL sai depois da primeira escolha); depois a mais recente do mesmo device.
			std::lock_guard<std::mutex> guard(g_queueLock);
			auto sameDevice = [&](const ComPtr<ID3D12CommandQueue>& q) {
				ComPtr<ID3D12Device> qDevice;
				return q && SUCCEEDED(q->GetDevice(IID_PPV_ARGS(&qDevice))) && qDevice.Get() == bbDevice.Get();
			};
			for (const auto& q : g_queues)
				if (q && q.Get() == t_lastDirectQueue && sameDevice(q))
					return q;
			if (sameDevice(g_r.queue))
				return g_r.queue;
			for (int i = 0; i < kMaxQueueCandidates; ++i)
			{
				const auto& q = g_queues[(g_queueNext - 1 - i + 2 * kMaxQueueCandidates) % kMaxQueueCandidates];  // mais recente primeiro
				if (sameDevice(q))
					return q;
			}
			return {};
		}

		bool Bind(IDXGISwapChain* sc)
		{
			auto& r = g_r;
			ComPtr<ID3D12CommandQueue> queue;
			// Numa swapchain D3D12, GetDevice devolve a fila com que ela foi criada (a que o Present usa).
			bool fromSwapchain = SUCCEEDED(sc->GetDevice(IID_PPV_ARGS(&queue))) && queue;
			if (!fromSwapchain)
				queue = FallbackQueue(sc);
			if (!queue)
			{
				ArmEclCapture();  // (re)liga a captura ate ver uma fila DIRECT do mesmo device
				LogOnce(r.loggedNoQueue, "sem fila de comandos para a swapchain (GetDevice falhou e nenhuma fila DIRECT do mesmo device foi vista)");
				return false;
			}
			if (queue.Get() == r.queue.Get())
			{
				if (g_eclCapture.load(std::memory_order_relaxed))
					DisarmEclCapture();
				return true;
			}

			if (queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
			{
				LogOnce(r.loggedNoQueue, "a fila da swapchain nao e DIRECT");
				return false;
			}
			ComPtr<ID3D12Device> device;
			const HRESULT        hr = queue->GetDevice(IID_PPV_ARGS(&device));
			if (FAILED(hr))
			{
				LogOnce(r.loggedNoQueue, "ID3D12CommandQueue::GetDevice falhou " + Hr(hr));
				return false;
			}
			if (device.Get() != r.device.Get())
			{
				ReleaseDeviceObjects();
				r.device = device;
				if (!CreateDeviceObjects())
				{
					ReleaseDeviceObjects();
					Disable("nao deu para criar os recursos D3D12");
					return false;
				}
			}
			else if (!WaitIdle())  // mesma GPU, outra fila: o que ja foi submetido termina antes
			{
				return false;
			}
			r.queue = queue;
			r.queueFromSwapchain = fromSwapchain;
			// Fila conhecida: tira o hook nativo de ECL (minimo de tempo numa vtable nativa) e solta as
			// filas do jogo que ele guardou. Volta a ligar se a fila sumir (device recriado).
			DisarmEclCapture();
			{
				std::lock_guard<std::mutex> guard(g_queueLock);
				for (auto& q : g_queues)
					q.Reset();
			}
			void** qv = *reinterpret_cast<void***>(queue.Get());
			Log(std::string("overlay: fila ") + Hex(reinterpret_cast<std::uintptr_t>(queue.Get())) +
			    (fromSwapchain ? " da swapchain (GetDevice)" : " do ExecuteCommandLists") + ", vtable " + ModuleOf(qv) + ", ECL original em " +
			    ModuleOf(reinterpret_cast<const void*>(g_origExecuteCommandLists.load())));
			return true;
		}

		int ChooseColorMode(IDXGISwapChain* sc, DXGI_FORMAT format)
		{
			auto&     r = g_r;
			int  cs = -1;
			UINT size = sizeof(cs);
			if (FAILED(sc->GetPrivateData(kColorSpaceGuid, &size, &cs)) || size != sizeof(cs))
				cs = -1;
			if (format == DXGI_FORMAT_R16G16B16A16_FLOAT)
				return cs == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ? 2 : 1;
			if (format == DXGI_FORMAT_R10G10B10A2_UNORM)
			{
				if (cs >= 0)
					return cs == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ? 2 : 0;
				return r.outputHdr ? 2 : 0;  // SetColorSpace1 antes do hook: segue o monitor
			}
			return 0;
		}

		bool OutputIsHdr(IDXGISwapChain* sc)
		{
			ComPtr<IDXGIOutput>  output;
			ComPtr<IDXGIOutput6> output6;
			DXGI_OUTPUT_DESC1    d{};
			return SUCCEEDED(sc->GetContainingOutput(&output)) && SUCCEEDED(output.As(&output6)) && SUCCEEDED(output6->GetDesc1(&d)) &&
			       d.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
		}

		void DrawFrame(IDXGISwapChain* sc)
		{
			auto& r = g_r;
			// Outra swapchain apresentando intercalada com a nossa (ex.: interna de um plugin): fica com a
			// atual. So troca quando a outra apresenta varias vezes seguidas (a atual morreu/foi recriada).
			if (r.swapchain && r.swapchain != sc && ++r.otherStreak < kSwapchainSwitchStreak)
				return;
			r.otherStreak = 0;
			if (r.swapchain != sc)
			{
				Log("overlay: swapchain " + Hex(reinterpret_cast<std::uintptr_t>(sc)) + " (Present em " +
				    ModuleOf(g_origPresent.load() ? reinterpret_cast<const void*>(g_origPresent.load()) : nullptr) + ")");
				r.swapchain = sc;
				r.lastFormat = DXGI_FORMAT_UNKNOWN;
			}

			if (!Bind(sc))
				return;
			const HRESULT removed = r.device->GetDeviceRemovedReason();
			if (removed != S_OK)
			{
				LogOnce(r.loggedRemoved, "device removido (" + Hr(removed) + "); sem overlay ate o jogo recriar o device");
				return;
			}

			// Conteudo: so com o MC linkado (o gate do mw3.asi) e com frames no triple buffer.
			auto&      link = Link::Get();
			const bool want = link.Valid() && (!g_options.shouldDraw || g_options.shouldDraw());
			if (!want)
			{
				r.haveFrame = false;  // nao reaparecer com um frame velho
				r.pendingUpload = false;
				return;
			}
			if (link.AcquireOverlayFrame())
				r.pendingUpload = true;
			UINT frameW = 0, frameH = 0;
			bool frameFlip = true;
			if (r.pendingUpload)
			{
				const auto* hdr = link.FrontHeader();
				frameW = hdr->width;
				frameH = hdr->height;
				frameFlip = (hdr->flags & 1) != 0;
				if (frameW == 0 || frameH == 0 || frameW > proto::kMaxOverlayW || frameH > proto::kMaxOverlayH)
				{
					LogOnce(r.loggedBadFrame, "frame do overlay com tamanho invalido " + std::to_string(frameW) + "x" + std::to_string(frameH));
					r.pendingUpload = false;
				}
			}
			if (!r.pendingUpload && !r.haveFrame)
				return;

			ComPtr<IDXGISwapChain3> sc3;
			if (FAILED(sc->QueryInterface(IID_PPV_ARGS(&sc3))))
			{
				LogOnce(r.loggedNo3, "swapchain sem IDXGISwapChain3");
				return;
			}
			DXGI_SWAP_CHAIN_DESC1 desc{};
			if (FAILED(sc3->GetDesc1(&desc)))
				return;
			const UINT             bbIndex = sc3->GetCurrentBackBufferIndex();
			ComPtr<ID3D12Resource> bb;
			HRESULT                hr = sc3->GetBuffer(bbIndex, IID_PPV_ARGS(&bb));
			if (FAILED(hr))
			{
				LogOnce(r.loggedGetBuffer, "GetBuffer(" + std::to_string(bbIndex) + ") falhou " + Hr(hr));
				return;
			}
			const D3D12_RESOURCE_DESC bbDesc = bb->GetDesc();
			const DXGI_FORMAT         format = bbDesc.Format;
			const UINT                bbW = static_cast<UINT>(bbDesc.Width), bbH = bbDesc.Height;
			if (format != r.lastFormat || bbW != r.lastW || bbH != r.lastH)
			{
				r.outputHdr = OutputIsHdr(sc);
				r.lastFormat = format;
				r.lastW = bbW;
				r.lastH = bbH;
				r.lastMode = -1;
			}
			const int mode = ChooseColorMode(sc, format);
			if (mode != r.lastMode)
			{
				static const char* const kModes[] = { "SDR", "scRGB", "HDR10/PQ" };
				Log("overlay: back buffer " + std::to_string(bbW) + "x" + std::to_string(bbH) + " formato " + std::to_string(static_cast<int>(format)) +
				    " x" + std::to_string(desc.BufferCount) + ", modo " + kModes[mode]);
				r.lastMode = mode;
			}

			ID3D12PipelineState* pso = GetPso(format);
			if (!pso || !EnsureRtvHeap(std::max(desc.BufferCount, bbIndex + 1)))
				return;

			FrameContext& f = r.frames[r.frameIndex];
			if (!WaitFence(f.fenceValue))
			{
				Disable("timeout esperando a GPU (fence do overlay)");
				return;
			}

			UINT uploadPitch = 0;
			if (r.pendingUpload)
			{
				uploadPitch = (frameW * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
				if (!EnsureTexture(frameW, frameH) || !EnsureUpload(f, UINT64(uploadPitch) * frameH))
				{
					r.pendingUpload = false;
					if (!r.haveFrame)
						return;
				}
				else
				{
					const std::uint8_t* src = link.FrontPixels();
					const std::size_t   rowBytes = std::size_t(frameW) * 4;
					for (UINT y = 0; y < frameH; ++y)
						std::memcpy(f.uploadPtr + std::size_t(y) * uploadPitch, src + y * rowBytes, rowBytes);
				}
			}

			if (FAILED(hr = f.allocator->Reset()) || FAILED(hr = r.list->Reset(f.allocator.Get(), pso)))
			{
				Disable("Reset do command list falhou " + Hr(hr));
				return;
			}
			auto* list = r.list.Get();

			if (r.pendingUpload)
			{
				if (r.texState != D3D12_RESOURCE_STATE_COPY_DEST)
				{
					const auto b = Transition(r.texture.Get(), r.texState, D3D12_RESOURCE_STATE_COPY_DEST);
					list->ResourceBarrier(1, &b);
				}
				D3D12_TEXTURE_COPY_LOCATION dst{};
				dst.pResource = r.texture.Get();
				dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
				dst.SubresourceIndex = 0;
				D3D12_TEXTURE_COPY_LOCATION src{};
				src.pResource = f.upload.Get();
				src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
				src.PlacedFootprint.Offset = 0;
				src.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, frameW, frameH, 1, uploadPitch };
				list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
				const auto b = Transition(r.texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
				list->ResourceBarrier(1, &b);
				r.texState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
				r.flipY = frameFlip;
				r.haveFrame = true;
				r.pendingUpload = false;
				++g_uploads;
			}

			D3D12_CPU_DESCRIPTOR_HANDLE rtv = r.rtvHeap->GetCPUDescriptorHandleForHeapStart();
			rtv.ptr += std::size_t(bbIndex) * r.rtvStride;
			r.device->CreateRenderTargetView(bb.Get(), nullptr, rtv);

			{
				const auto b = Transition(bb.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
				list->ResourceBarrier(1, &b);
			}

			Params p{};
			p.viewport[0] = float(bbW);
			p.viewport[1] = float(bbH);
			p.flipY = r.flipY ? 1.0f : 0.0f;
			p.mode = float(mode);
			p.paperWhite = g_options.hdrPaperWhiteNits;
			float cx = 0, cy = 0;
			if (g_options.cursor && g_options.cursor(cx, cy))
			{
				// O cursor vem em pixels do overlay; escala se overlay e back buffer diferem.
				p.cursor[0] = r.texW ? cx * float(bbW) / float(r.texW) : cx;
				p.cursor[1] = r.texH ? cy * float(bbH) / float(r.texH) : cy;
				p.cursorOn = 1.0f;
			}

			ID3D12DescriptorHeap* heaps[] = { r.srvHeap.Get() };
			list->SetGraphicsRootSignature(r.rootSignature.Get());
			list->SetDescriptorHeaps(1, heaps);
			list->SetGraphicsRoot32BitConstants(0, sizeof(Params) / 4, &p, 0);
			list->SetGraphicsRootDescriptorTable(1, r.srvHeap->GetGPUDescriptorHandleForHeapStart());
			const D3D12_VIEWPORT vp{ 0.0f, 0.0f, float(bbW), float(bbH), 0.0f, 1.0f };
			const D3D12_RECT     scissor{ 0, 0, static_cast<LONG>(bbW), static_cast<LONG>(bbH) };
			list->RSSetViewports(1, &vp);
			list->RSSetScissorRects(1, &scissor);
			list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
			list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			list->DrawInstanced(3, 1, 0, 0);

			// Teste: copia o back buffer (formatos de 4 bytes) para um buffer de readback.
			D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
			bool                               readback = false;
			if (g_options.debugReadback && (format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_R10G10B10A2_UNORM))
			{
				UINT64 total = 0;
				r.device->GetCopyableFootprints(&bbDesc, 0, 1, 0, &footprint, nullptr, nullptr, &total);
				readback = EnsureBuffer(r.readback, r.readbackBytes, total, D3D12_HEAP_TYPE_READBACK);
			}
			if (readback)
			{
				auto b = Transition(bb.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
				list->ResourceBarrier(1, &b);
				D3D12_TEXTURE_COPY_LOCATION dst{};
				dst.pResource = r.readback.Get();
				dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
				dst.PlacedFootprint = footprint;
				D3D12_TEXTURE_COPY_LOCATION src{};
				src.pResource = bb.Get();
				src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
				src.SubresourceIndex = 0;
				list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
				b = Transition(bb.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
				list->ResourceBarrier(1, &b);
			}
			else
			{
				const auto b = Transition(bb.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
				list->ResourceBarrier(1, &b);
			}

			if (FAILED(hr = list->Close()))
			{
				Disable("Close do command list falhou " + Hr(hr));
				return;
			}
			ID3D12CommandList* lists[] = { list };
			r.queue->ExecuteCommandLists(1, lists);
			if (FAILED(hr = r.queue->Signal(r.fence.Get(), ++r.fenceValue)))
			{
				Disable("Signal falhou " + Hr(hr));
				return;
			}
			f.fenceValue = r.fenceValue;
			r.frameIndex = (r.frameIndex + 1) % kFrameContexts;
			++g_draws;
			if (!r.loggedFirstDraw)
			{
				r.loggedFirstDraw = true;
				Log("overlay: primeiro frame do Minecraft desenhado (" + std::to_string(r.texW) + "x" + std::to_string(r.texH) + " -> " +
				    std::to_string(bbW) + "x" + std::to_string(bbH) + ")");
			}

			if (readback && WaitFence(r.fenceValue))
			{
				void*             mapped = nullptr;
				const D3D12_RANGE all{ 0, static_cast<SIZE_T>(r.readbackBytes) };
				if (SUCCEEDED(r.readback->Map(0, &all, &mapped)) && mapped)
				{
					g_readbackW = bbW;
					g_readbackH = bbH;
					g_readbackPixels.resize(std::size_t(bbW) * bbH);
					for (UINT y = 0; y < bbH; ++y)
						std::memcpy(g_readbackPixels.data() + std::size_t(y) * bbW,
						    static_cast<const std::uint8_t*>(mapped) + footprint.Offset + std::size_t(y) * footprint.Footprint.RowPitch, std::size_t(bbW) * 4);
					const D3D12_RANGE none{ 0, 0 };
					r.readback->Unmap(0, &none);
				}
			}
		}

		// SEH aqui (sem objetos com destrutor neste frame): uma falha de driver/proxy desliga o overlay
		// em vez de derrubar o jogo. Um stack overflow pego aqui deixa a pilha sem guard page:
		// _resetstkoflw a recoloca (senao o proximo overflow vira access violation sem aviso).
		bool SafeDraw(IDXGISwapChain* sc, DWORD& code)
		{
			__try
			{
				DrawFrame(sc);
				return true;
			}
			__except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
			{
				if (code == static_cast<DWORD>(EXCEPTION_STACK_OVERFLOW))
					_resetstkoflw();
				return false;
			}
		}

		bool SafeWaitIdle()
		{
			__try
			{
				return WaitIdle();
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		// So na chamada de fora (aninhamento 0) de Present/Present1.
		void OnPresent(IDXGISwapChain* sc, UINT flags)
		{
			++g_presents;
			// O Present roda ate em menus e loading: e aqui que o jogo diz ao MC que esta vivo
			// (a thread do link continua batendo tambem, como reserva).
			if (g_options.heartbeat)
				Link::Get().Heartbeat();
			if (!(flags & (DXGI_PRESENT_TEST | DXGI_PRESENT_DO_NOT_SEQUENCE)) && !g_disabled.load(std::memory_order_relaxed))
			{
				std::lock_guard<std::mutex> guard(g_lock);
				DWORD code = 0;
				if (!SafeDraw(sc, code))
					Disable("excecao estrutural " + Hex(code) + " no desenho");
			}
		}

		void BeforeResize(IDXGISwapChain* sc)
		{
			std::lock_guard<std::mutex> guard(g_lock);
			// Nao guardamos referencias aos back buffers (GetBuffer a cada frame); so garante que o nosso
			// trabalho na fila acabou antes de os buffers sumirem.
			if (g_r.swapchain == sc && g_r.fence && !SafeWaitIdle())
				Log("overlay: ResizeBuffers sem esperar o overlay (timeout)");
		}

		// Profundidade de aninhamento nesta thread, valida durante TODA a chamada encaminhada (o
		// destrutor roda depois dela: o compilador nao pode transformar o encaminhamento em tail call,
		// que zerava a profundidade antes de descer, como no hook antigo).
		struct Nest
		{
			const bool outer;
			Nest() noexcept : outer(t_depth++ == 0) {}
			~Nest() { --t_depth; }
			Nest(const Nest&) = delete;
			Nest& operator=(const Nest&) = delete;
		};

		bool Runaway()
		{
			return t_depth >= kMaxNesting;
		}

		// Laco de terceiros passando por nos (so possivel se alguem guardou o nosso hook como "original"
		// e o chama de volta): corta esta volta sem descer mais, desliga o overlay e restaura os slots.
		HRESULT BreakLoop(const char* method, HRESULT hr)
		{
			Disable(std::string(method) + " reentrou " + std::to_string(t_depth) + " vezes na mesma thread (laco de hooks de terceiros)");
			return hr;
		}

		// Os hooks: chamada de fora desenha; aninhada so repassa. Sempre para o valor original do slot
		// (a funcao do proxy do Streamline), nunca de volta pela vtable.
		HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* sc, UINT sync, UINT flags)
		{
			const PresentFn next = g_origPresent.load();
			if (Runaway())
				return BreakLoop("Present", S_OK);
			Nest nest;
			if (nest.outer)
				OnPresent(sc, flags);
			else
				++g_reentries;
			return next(sc, sync, flags);
		}

		HRESULT STDMETHODCALLTYPE HookPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* params)
		{
			const Present1Fn next = g_origPresent1.load();
			if (Runaway())
				return BreakLoop("Present1", S_OK);
			Nest nest;
			if (nest.outer)
				OnPresent(sc, flags);
			else
				++g_reentries;
			return next(sc, sync, flags, params);
		}

		HRESULT STDMETHODCALLTYPE HookResizeBuffers(IDXGISwapChain* sc, UINT count, UINT w, UINT h, DXGI_FORMAT format, UINT flags)
		{
			const ResizeBuffersFn next = g_origResizeBuffers.load();
			if (Runaway())
				return BreakLoop("ResizeBuffers", DXGI_ERROR_INVALID_CALL);
			Nest nest;
			if (nest.outer)
				BeforeResize(sc);
			return next(sc, count, w, h, format, flags);
		}

		HRESULT STDMETHODCALLTYPE HookResizeBuffers1(IDXGISwapChain3* sc, UINT count, UINT w, UINT h, DXGI_FORMAT format, UINT flags,
		    const UINT* nodeMasks, IUnknown* const* queues)
		{
			const ResizeBuffers1Fn next = g_origResizeBuffers1.load();
			if (Runaway())
				return BreakLoop("ResizeBuffers1", DXGI_ERROR_INVALID_CALL);
			Nest nest;
			if (nest.outer)
				BeforeResize(sc);
			return next(sc, count, w, h, format, flags, nodeMasks, queues);
		}

		HRESULT STDMETHODCALLTYPE HookSetColorSpace1(IDXGISwapChain3* sc, DXGI_COLOR_SPACE_TYPE colorSpace)
		{
			const SetColorSpace1Fn next = g_origSetColorSpace1.load();
			if (Runaway())
				return BreakLoop("SetColorSpace1", DXGI_ERROR_INVALID_CALL);
			Nest          nest;
			const HRESULT hr = next(sc, colorSpace);
			if (nest.outer && SUCCEEDED(hr))
			{
				const int cs = static_cast<int>(colorSpace);
				sc->SetPrivateData(kColorSpaceGuid, sizeof(cs), &cs);
				Log("overlay: SetColorSpace1(" + std::to_string(cs) + ")");
			}
			return hr;
		}

		void RememberQueue(ID3D12CommandQueue* queue)
		{
			std::unique_lock<std::mutex> guard(g_queueLock, std::try_to_lock);
			if (!guard.owns_lock())
				return;
			for (auto& q : g_queues)
				if (q.Get() == queue)
				{
					t_lastDirectQueue = queue;
					return;
				}
			if (queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
				return;
			t_lastDirectQueue = queue;
			g_queues[g_queueNext] = queue;
			g_queueNext = (g_queueNext + 1) % kMaxQueueCandidates;
		}

		// Repasse puro (so anota a fila enquanto a captura esta ligada).
		void STDMETHODCALLTYPE HookExecuteCommandLists(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
		{
			++g_eclCalls;
			if (g_eclCapture.load(std::memory_order_relaxed) && queue)
				RememberQueue(queue);
			g_origExecuteCommandLists.load()(queue, count, lists);
		}

		template <class Fn>
		bool PatchSlot(SlotPatch& p, void** vtable, int index, Fn hook, std::atomic<Fn>& original, const char* name)
		{
			void* current = vtable[index];
			if (current == reinterpret_cast<void*>(hook))
			{
				Log(std::string("overlay: ") + name + " ja tinha o nosso hook");
				return false;
			}
			original = reinterpret_cast<Fn>(current);  // antes do slot: quem ja entrar no hook acha o original
			std::lock_guard<std::mutex> guard(g_patchLock);
			p = SlotPatch{ &vtable[index], reinterpret_cast<void*>(hook), current, name, false };
			if (!WriteSlot(p.slot, p.hook))
			{
				Log(std::string("overlay: VirtualProtect falhou para ") + name + " (" + std::to_string(GetLastError()) + ")");
				return false;
			}
			p.active = true;
			Log(std::string("overlay: hook ") + name + " (slot " + std::to_string(index) + " em " + ModuleOf(&vtable[index]) + ", original em " +
			    ModuleOf(current) + ")");
			return true;
		}

		// ---- vtable do proxy do Streamline pelo RTTI do MSVC (x64) --------------------------------------
		// TypeDescriptor {pVFTable, spare, name} no .data; CompleteObjectLocator {sig 1, offset, cdOffset,
		// RVA do TypeDescriptor, RVA do ClassHierarchyDescriptor, RVA de si mesmo} no .rdata; a vtable vem
		// logo depois de um ponteiro absoluto para o COL. So leitura dentro da imagem do modulo.
		struct RttiCol
		{
			DWORD signature, offset, cdOffset, typeDescriptor, classDescriptor, self;
		};

		struct ImageSections
		{
			const std::uint8_t*         base = nullptr;
			const IMAGE_SECTION_HEADER* sections = nullptr;
			WORD                        count = 0;
		};

		bool ReadImage(HMODULE module, ImageSections& img)
		{
			img.base = reinterpret_cast<const std::uint8_t*>(module);
			const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(img.base);
			if (dos->e_magic != IMAGE_DOS_SIGNATURE)
				return false;
			const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(img.base + dos->e_lfanew);
			if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
				return false;
			img.sections = IMAGE_FIRST_SECTION(nt);
			img.count = nt->FileHeader.NumberOfSections;
			return true;
		}

		bool IsDataSection(const IMAGE_SECTION_HEADER& s)
		{
			return (s.Characteristics & IMAGE_SCN_MEM_READ) && !(s.Characteristics & (IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_DISCARDABLE));
		}

		bool InCode(const ImageSections& img, const void* p)
		{
			const auto a = reinterpret_cast<std::uintptr_t>(p);
			for (WORD i = 0; i < img.count; ++i)
			{
				const auto& s = img.sections[i];
				const auto  lo = reinterpret_cast<std::uintptr_t>(img.base) + s.VirtualAddress;
				if ((s.Characteristics & IMAGE_SCN_MEM_EXECUTE) && a >= lo && a < lo + s.Misc.VirtualSize)
					return true;
			}
			return false;
		}

		void** FindVtableForTypeDescriptor(const ImageSections& img, DWORD tdRva)
		{
			for (WORD i = 0; i < img.count; ++i)
			{
				const auto& s = img.sections[i];
				if (!IsDataSection(s) || s.Misc.VirtualSize < sizeof(RttiCol))
					continue;
				for (DWORD off = 0; off + sizeof(RttiCol) <= s.Misc.VirtualSize; off += 4)
				{
					const DWORD colRva = s.VirtualAddress + off;
					const auto* col = reinterpret_cast<const RttiCol*>(img.base + colRva);
					if (col->signature != 1 || col->offset != 0 || col->typeDescriptor != tdRva || col->self != colRva)
						continue;
					// Ponteiro absoluto para o COL logo antes da vtable.
					const auto colAbs = reinterpret_cast<std::uintptr_t>(img.base) + colRva;
					for (WORD j = 0; j < img.count; ++j)
					{
						const auto& t = img.sections[j];
						if (!IsDataSection(t))
							continue;
						const auto* q = reinterpret_cast<const std::uintptr_t*>(img.base + t.VirtualAddress);
						const DWORD n = t.Misc.VirtualSize / sizeof(std::uintptr_t);
						for (DWORD k = 0; k + 1 < n; ++k)
							if (q[k] == colAbs && InCode(img, reinterpret_cast<const void*>(q[k + 1])))
								return reinterpret_cast<void**>(const_cast<std::uintptr_t*>(&q[k + 1]));
					}
				}
			}
			return nullptr;
		}

		void** FindRttiVtable(HMODULE module, const char* decoratedName)
		{
			ImageSections img;
			if (!ReadImage(module, img))
				return nullptr;
			const std::size_t len = std::strlen(decoratedName) + 1;  // com o '\0'
			for (WORD i = 0; i < img.count; ++i)
			{
				const auto& s = img.sections[i];
				if (!IsDataSection(s) || s.Misc.VirtualSize < len)
					continue;
				const std::uint8_t* p = img.base + s.VirtualAddress;
				for (DWORD off = 2 * sizeof(void*); off + len <= s.Misc.VirtualSize; ++off)
				{
					if (p[off] != '.' || std::memcmp(p + off, decoratedName, len) != 0)
						continue;
					// Cada ocorrencia do nome e candidata (a primeira pode ser so um literal de string).
					if (void** vt = FindVtableForTypeDescriptor(img, static_cast<DWORD>(s.VirtualAddress + off - 2 * sizeof(void*))))
						return vt;
				}
			}
			return nullptr;
		}

		bool SlotsLookSane(HMODULE module, void** vtable, int count)
		{
			ImageSections img;
			if (!ReadImage(module, img))
				return false;
			for (int i = 0; i < count; ++i)
				if (!InCode(img, vtable[i]))
				{
					Log("overlay: slot " + std::to_string(i) + " do proxy aponta para " + ModuleOf(vtable[i]) + " (fora do " +
					    ModuleOf(module) + "); sem overlay");
					return false;
				}
			return true;
		}

		// Vtable nativa da fila DIRECT (D3D12Core), de um device/fila dummy pelo d3d12.dll do System32.
		// D3D12 devolve o device ja existente do mesmo adaptador; a fila dummy e liberada aqui.
		void** NativeQueueVtable(PFN_D3D12_CREATE_DEVICE createDevice)
		{
			ComPtr<ID3D12Device>       device;
			ComPtr<ID3D12CommandQueue> queue;
			HRESULT                    hr = createDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
			if (FAILED(hr))
			{
				Log("overlay: D3D12CreateDevice (dummy) falhou " + Hr(hr));
				return nullptr;
			}
			D3D12_COMMAND_QUEUE_DESC qd{};
			qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
			if (FAILED(hr = device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))))
			{
				Log("overlay: CreateCommandQueue (dummy) falhou " + Hr(hr));
				return nullptr;
			}
			return *reinterpret_cast<void***>(queue.Get());  // vtable vive no D3D12Core (nunca descarregado)
		}

		bool InstallLocked()
		{
			// 1) O proxy de swapchain do Streamline: e por ele que o jogo apresenta.
			HMODULE proxyModule = g_options.proxyModule ? static_cast<HMODULE>(g_options.proxyModule) : GetModuleHandleW(kProxyModuleName);
			if (!proxyModule)
			{
				Log("overlay: sl.interposer.dll nao esta carregado; o overlay so se pendura no proxy do Streamline (nunca na "
				    "vtable nativa do dxgi, por causa do overlay da Steam)");
				return false;
			}
			void** scVtbl = FindRttiVtable(proxyModule, kProxyClassRtti);
			if (!scVtbl)
			{
				Log(std::string("overlay: vtable de ") + kProxyClassRtti + " nao achada pelo RTTI em " + ModuleOf(proxyModule));
				return false;
			}
			Log(std::string("overlay: vtable do proxy ") + kProxyClassRtti + " em " + ModuleOf(scVtbl));
			if (!SlotsLookSane(proxyModule, scVtbl, kProxyCheckedSlots))
				return false;

			// 2) d3d12.dll do System32 (o mesmo D3D12Core do jogo, Agility SDK incluido): root signature e
			// a vtable nativa da fila para a captura transitoria de ExecuteCommandLists.
			wchar_t    sys[MAX_PATH]{};
			const UINT n = GetSystemDirectoryW(sys, MAX_PATH);
			if (n == 0 || n >= MAX_PATH)
				return false;
			HMODULE d3d12 = LoadLibraryW((std::wstring(sys) + L"\\d3d12.dll").c_str());
			if (!d3d12)
			{
				Log("overlay: nao carregou d3d12.dll do System32");
				return false;
			}
			auto createDevice = reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(reinterpret_cast<void*>(GetProcAddress(d3d12, "D3D12CreateDevice")));
			g_serializeRootSignature =
			    reinterpret_cast<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>(reinterpret_cast<void*>(GetProcAddress(d3d12, "D3D12SerializeRootSignature")));
			if (!createDevice || !g_serializeRootSignature)
			{
				Log("overlay: exports do d3d12 ausentes");
				return false;
			}
			if (void** qVtbl = NativeQueueVtable(createDevice))
			{
				if (PatchSlot(g_eclPatch, qVtbl, kSlotExecuteCommandLists, &HookExecuteCommandLists, g_origExecuteCommandLists,
				        "ID3D12CommandQueue::ExecuteCommandLists (nativo, transitorio)"))
					g_eclCapture = true;
			}
			if (!g_eclPatch.active)
				Log("overlay: sem captura de fila pelo ECL; so IDXGISwapChain::GetDevice");

			// 3) Slots do proxy; Present por ultimo.
			int& c = g_proxyPatchCount;
			bool ok = PatchSlot(g_proxyPatches[c++], scVtbl, kSlotResizeBuffers, &HookResizeBuffers, g_origResizeBuffers, "proxy IDXGISwapChain::ResizeBuffers");
			ok = ok && PatchSlot(g_proxyPatches[c++], scVtbl, kSlotResizeBuffers1, &HookResizeBuffers1, g_origResizeBuffers1, "proxy IDXGISwapChain3::ResizeBuffers1");
			ok = ok && PatchSlot(g_proxyPatches[c++], scVtbl, kSlotSetColorSpace1, &HookSetColorSpace1, g_origSetColorSpace1, "proxy IDXGISwapChain3::SetColorSpace1");
			ok = ok && PatchSlot(g_proxyPatches[c++], scVtbl, kSlotPresent1, &HookPresent1, g_origPresent1, "proxy IDXGISwapChain1::Present1");
			ok = ok && PatchSlot(g_proxyPatches[c++], scVtbl, kSlotPresent, &HookPresent, g_origPresent, "proxy IDXGISwapChain::Present");
			if (!ok)
				RestoreAll();
			return ok;
		}

		bool SafeInstall()
		{
			__try
			{
				return InstallLocked();
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}
	} // namespace

	bool Install(const Options& options)
	{
		std::lock_guard<std::mutex> guard(g_installLock);
		if (g_installed)
			return true;
		g_options = options;
		const bool ok = SafeInstall();
		if (!ok)
			RestoreAll();  // excecao no meio da instalacao: nada fica pendurado
		g_installed = ok;
		Log(ok ? "overlay: hooks instalados (vtable do proxy de swapchain do Streamline)" : "overlay: hooks NAO instalados; jogo segue sem overlay");
		return ok;
	}

	Stats GetStats()
	{
		Stats s;
		s.hooked = g_installed.load();
		s.disabled = g_disabled.load();
		s.presents = g_presents.load();
		s.draws = g_draws.load();
		s.uploads = g_uploads.load();
		s.eclCalls = g_eclCalls.load();
		s.reentries = g_reentries.load();
		{
			std::lock_guard<std::mutex> patchGuard(g_patchLock);
			s.eclHookActive = g_eclPatch.active;
			for (int i = 0; i < g_proxyPatchCount; ++i)
				s.proxySlotsActive += g_proxyPatches[i].active ? 1 : 0;
		}
		std::lock_guard<std::mutex> guard(g_lock);
		s.queueFromSwapchain = g_r.queue && g_r.queueFromSwapchain;
		s.queueFromEcl = g_r.queue && !g_r.queueFromSwapchain;
		s.backBufferFormat = static_cast<std::uint32_t>(g_r.lastFormat);
		s.backBufferW = g_r.lastW;
		s.backBufferH = g_r.lastH;
		s.colorMode = g_r.lastMode;
		return s;
	}

	bool DebugReadback(std::uint32_t& width, std::uint32_t& height, std::vector<std::uint32_t>& pixels)
	{
		std::lock_guard<std::mutex> guard(g_lock);
		if (g_readbackPixels.empty())
			return false;
		width = g_readbackW;
		height = g_readbackH;
		pixels = g_readbackPixels;
		return true;
	}
} // namespace mw3::overlay

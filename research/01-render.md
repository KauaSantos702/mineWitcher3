# 01 — Pipeline de render e ponto de hook (witcher3.exe next-gen, DX12)

Alvo: `bin\x64_dx12\witcher3.exe` 5.0.0.1048522 (SHA-256 `2DF76539…FB101`), ImageBase `0x140000000`, link timestamp `0x6AC555A3`.
Todos os endereços abaixo são **RVAs** (somar `0x140000000` para VA sem ASLR).
Análise só estática (pefile + capstone, sem Ghidra/IDA). Nada foi executado nem injetado, e nenhum arquivo do jogo foi alterado.

Scripts (em `tools/scripts/render/`):

| script | função |
|---|---|
| `pe_overview.py` | seções, entropia, imports, exports, TLS |
| `strings_scan.py` | strings ASCII/UTF-16 por regex, com RVA |
| `strings_range.py` | todas as strings de um intervalo de RVA |
| `xref.py` | referências RIP-relative (lea/mov/call [rip]) a um RVA, com a função de origem via `.pdata` |
| `callers.py` | quem faz `call`/`jmp rel32` para um RVA |
| `primary.py` | resolve fragmentos encadeados do `.pdata` até a função primária |
| `funcmap.py` | lista as chamadas de uma função, com todos os seus fragmentos |
| `dis.py` | desassembla um intervalo |
| `dataref.py` | procura ponteiros absolutos de 64 bits |

Legenda: **[O]** observado no binário · **[I]** inferido · **[?]** desconhecido.

---

## 1. Como D3D12/DXGI são obtidos

### 1.1 Não há import de `d3d12.dll` nem de `dxgi.dll`. Tudo passa pelo NVIDIA Streamline
- **[O]** A tabela de imports do exe **não tem** `d3d12.dll` nem `dxgi.dll`. As funções de criação vêm do **`sl.interposer.dll`** (Streamline **2.14.1.0**, "SL.INTERPOSER MODULE - PRODUCTION", assinado pela NVIDIA). Endereços na IAT:
  - `CreateDXGIFactory2` @IAT `0x298dc98`, `CreateDXGIFactory1` `0x298dca8`, `CreateDXGIFactory` `0x298dcd0`
  - `D3D12CreateDevice` `0x298dca0`, `D3D12SerializeRootSignature` `0x298dcd8`, `D3D12SerializeVersionedRootSignature` `0x298dce8`
  - `slInit` `0x298dd08`, `slSetD3DDevice` `0x298dcb8`, `slUpgradeInterface` `0x298dce0`, `slSetTagForFrame` `0x298dd20`, `slGetNewFrameToken` `0x298dcf8`, `slEvaluateFeature` `0x298dcf0`, `slSetConstants` `0x298dcc8`, entre outros.
- **[O]** Onde são chamadas:
  - `CreateDXGIFactory2` em `0x1ec6959`/`0x1ec6984`, dentro da função `0x1ec6910`.
  - `D3D12CreateDevice` em `0x1ec6b13`/`0x1ec6b4c`, dentro de `0x1ec6a79` (criação do device), e também em `0x1b789ea`, `0x1ec797e` e `0x1ec843a`, que parecem sondagens de suporte.
  - `slUpgradeInterface` em `0x1f2735f`.
- **[O]** O `sl.interposer.dll` exporta `CreateDXGIFactory*` e `D3D12CreateDevice`, e importa `GetSystemDirectoryW`, `LoadLibraryW`/`LoadLibraryExW` e `GetProcAddress`. Ele contém as strings "Upgrading IDXGISwapChain to use SL proxy ...", "Injecting hooks into DXGI proxy virtual table", "IDXGISwapChain::Present before hook for feature '%s'..." e "RenderDoc has been detected. As RenderDoc disables NVAPI, any plugins which require NVAPI will be disabled."
- **[I]** O jogo recebe **objetos proxy do Streamline** (device, fila, swapchain e possivelmente command lists). O interposer carrega o `dxgi.dll`/`d3d12.dll` reais pelo caminho do System32 (`GetSystemDirectoryW`). **Consequência:** um `dxgi.dll` proxy na pasta do jogo provavelmente **não** é carregado pelo caminho de criação de device do jogo. Ele só seria carregado se outro módulo fizesse `LoadLibrary("dxgi.dll")` pela ordem de busca normal. Isso precisa ser confirmado em runtime.
- **[O]** Agility SDK: o exe exporta `D3D12SDKVersion = 619` (RVA `0x32109b8`) e `D3D12SDKPath = ".\D3D12_0\"`. A pasta `D3D12_0\` traz `D3D12Core.dll`, `d3d12SDKLayers.dll` e `D3D12StateObjectCompiler.*`.
- **[O]** Há outros imports estáticos ligados a GPU:
  - `amd_ags_x64.dll`, com `agsDriverExtensionsDX12_CreateDevice`: no AMD, o device pode ser criado pelo AGS.
  - `GFSDK_Aftermath_Lib.x64.dll`, `GFSDK_SSAO_D3D12.win64.dll` e `dxcompiler.dll` (`DxcCreateInstance`).
  - Upscalers e frame generation:
    - `amd_fidelityfx_loader_dx12.dll`: `ffxCreateContext`, `ffxConfigure`, `ffxQuery`, `ffxDispatch` (IAT `0x298da10`), `ffxDestroyContext`
    - `libxess.dll`: `xessD3D12Execute` (IAT `0x298db78`), entre outras
    - `libxess_fg.dll`: `xefgSwapChain*`
    - `libxell.dll`: Intel XeLL
  - **[O]** O DLSS SR, RR e FG chega **só via Streamline** (`slEvaluateFeature`). Não há import de `nvngx`.

### 1.2 Anti-tamper e packing
- **[O]** As seções são as de um link MSVC padrão: `.text`, `.rdata`, `.data`, `.pdata`, `_RDATA`, `.fptable`, `.rsrc`, `.reloc`.
  - Entropias: `.text` 6.54, `.rdata` 5.75, `.data` 0.80, `.pdata` 7.03 (normal para `.pdata`).
  - O entry point (`0x1a59bb0`) fica dentro de `.text`.
  - Não há `.bind` (stub Steam DRM), nem seções de Denuvo/VMProtect/Themida, nem seções com entropia alta ou nomes estranhos.
- **[O]** O exe é assinado por "CD PROJEKT RED S.A." (Authenticode válido). Existe um diretório TLS (callbacks em VA `0x1429c3fd8`, normal para o CRT). Importa `IsDebuggerPresent`, `NtQueryInformationProcess`, `MiniDumpWriteDump` e `StackWalk64`.
- **[I]** O conjunto acima indica um crash reporter (pasta `crashreporter\`) e **não** anti-tamper. Não há packing. O `.text` é estático e pode ser desassemblado direto do arquivo.
- **[O]** Há exports de símbolos C++ (397 no total): Wwise (`AK::`), Scaleform (`Scaleform::System`), `NvHairWorks_Create`, APEX. Junto com o exe vem `witcher3map.json`, um mapa oficial com 123 símbolos `0001:offset`. Ele cobre só RTTI e scripting (`CClass`, `CRTTISystem`, `CScriptingSystem`, `CFunction`, `CDebugConsole`, `CInputManager`, `CNamesPool`) e **não tem nada de render**. Mesmo assim, é útil para outros escopos do projeto, como chamar funções de script.

---

## 2. Estratégia de hook para o composite

### 2.1 Evidências da estrutura do renderer
- **[O]** Classes RTTI (strings `.?AV…@@`):
  - Interface e frame: `CRenderInterface`, `CRenderThread`, `CRenderViewport`, `CRenderSurfaces`, `CRenderSceneEx`, `CRenderCollector`, `CRenderStateManager`, `CRenderFrameInfo` (em lambdas), `CRenderCommand_ResizeRenderSurfaces`, `CRenderGameplayRenderTarget`.
  - Post-processing de gameplay: `CSurfacePostFX`, `CFocusModeEffectPostFX`, `CSepiaEffectPostFX`, `CCatViewEffectPostFX`, `CDrunkEffectPostFX`, `CRenderCommand_*PostFx`.
  - Abstração de GPU, no namespace **`GpuApi`**: `GpuApi::ClearDepthTarget`, `GpuApi::HintResourceState`, `CopyQueue@GpuApi`, `GPUVendorApi_NvApi`/`_AMDApi`/`_Intel`, `DxRayTracing@Rt@GpuApi` e `PoolD3D12Allocator@GpuApi`.
  - Scaleform (HUD): `CRenderScaleform`, `CRenderScaleformHAL`, `HAL@Render@Scaleform`, `ShaderDesc@D3D12@Render@Scaleform`, `CRenderScaleformDepthStencilSurface`, `CGuiRenderCommand_*`.
- **[O]** Nomes de render targets em uma tabela de enum, nas strings de `0x2b74be8` a `0x2b756c0`, referenciadas por `0x1bf01d8`…`0x1bf04a8`:
  - **Depth:** `RTN_GBuffer3Depth`, `RTN_GBufferDepthPrevious`, mais `depthBuffer`, `depthBufferHalf`, `depthBufferMSAA` e `depthBuffer4K`.
  - **Cor:** `RTN_Color`, `RTN_Color2`, `RTN_Color3`, `RTN_TemporalAAColor`, **`RTN_FinalColor`** e **`RTN_FinalColorUI`**. A cor da UI é separada.
  - **Motion vectors e upscaler:** `RTN_MotionVectors`, `RTN_FSR2_*` (FSR2 embutido no engine), `RTN_XeSS_RCAS`.
  - **Streamline:** **`RTN_SL_HUDLessColor`**, **`RTN_SL_UIAlpha`**.
- **[O]** Shaders de post-processing, pelos nomes `.fx`:
  - AA: `postfx_temporalAA_cs.fx` (TAA, `TAA_UPSAMPLE`), `taa*.hlsl`, `ffx_fsr2_*_pass.hlsl`, `FSR_Pass.hlsl`.
  - Bloom, tonemap e exposição: `postfx_bloom*`, `postfx_toneMappingApply.fx`/`Adapt`, `CEnvToneMappingAgxParameters`.
  - Outros efeitos: `postfx_dof*`, `postfx_motionBlur.fx`, `postfx_contrastAdaptiveSharpening_cs.fx`.
  - Final e cópias: `postfx_finalize.fx`, `postfx_present.fx`, `postfx_copyDepth.fx`, `postfx_deprojectDepth.fx`.
  - Contextos de desenho: `DRAWCONTEXT_Scaleform2D`, `DRAWCONTEXT_Overlay`, `DRAWCONTEXT_Transparency`, `DRAWCONTEXT_TAATransparency`, entre outros.
  - Marcadores de frame: `RenderFrame`, `RenderScene`, `RenderFinal`, `NormalFrameEpilogue`, `PrepareRender`.
- **[O]** Opções do FSR2: existe a string `FFX_FSR2_OPTION_HDR_COLOR_INPUT`, e também `FFX_FSR2_OPTION_INVERTED_DEPTH`.
  - **[I]** O upscaler recebe cor **HDR linear**, ou seja, roda **antes do tonemap**.
  - **[I]** O depth é **reverse-Z**. Isso bate com a comunidade do ReShade, que usa `RESHADE_DEPTH_INPUT_IS_REVERSED=1` neste jogo.

### 2.2 Ponto de AA/upscale (o "depois da cena, antes do post-processing e do HUD")
- **[O]** A função **`0x1c091c0`** é o seletor de AA/upscaler.
  - Faz um `switch` em `[rcx+0x74]` (valores 0..8, jump table em `0x1c0a070`).
  - Cada caso chama o caminho de uma técnica:
    - `0x1c665b0` → FSR3/4 via `ffxDispatch` (o contexto vem de `[global+0xd0]`)
    - `0x1b7e720` → DLSS SR (dentro dela, `0x1ed8f40` → `slEvaluateFeature`)
    - `0x1b7eba0` + `0x1b7e0a0` → DLSS Ray Reconstruction (`0x1ed95f0` → `slEvaluateFeature`)
    - `0x1c745d0` → XeSS (`0x1efd670` → `xessD3D12Execute`, com o marcador "XessSubmit")
    - `0x1d1dda0`/`0x1d1e060` e `0x347d10` → caminhos internos (TAA, FSR2 embutido ou checkerboard; não identifiquei qual é qual)
  - Retorna `bool`.
- **[O]** O único caller direto de `0x1c091c0` é `0x1c0ef30`, dentro da função **`0x1c0e420`**. Ela é a cadeia de post-processing (fragmentos até `0x1c10670`) e chama muitas sub-passadas depois do upscale. Os callers de `0x1c0e420` são `0x1d5f48e` (em `0x1d5efc0`) e `0x1d6057e` (em `0x1d601a3`).
  - **[I]** Esses dois callers são o frame normal e algum caminho alternativo (photomode ou screenshot).
- **[O]** Tags do Streamline, montadas pelo wrapper `0x1ed8c20`. Ele chama `slGetNewFrameToken`, monta um `sl::Resource` a partir da tabela de texturas do GpuApi em `[ctx+0x7e008 + idx*0x140]` e chama `slSetTagForFrame` com a command list `[dev+0x2100a8]`. Os tipos de buffer (`kBufferType*`) observados nos callers:
  - Em `0x1ed8f40` (DLSS SR): **0 = Depth, 1 = MotionVectors, 3 = ScalingInputColor, 4 = ScalingOutputColor**.
  - Em `0x1ed95f0` (DLSS RR): 0, 1, 3, 4, 7, 8, 0xE, 0x28, 0x2A, 0x3A e 0x44, ou seja, guias de albedo, normal/roughness, specular etc. (o mapeamento exato dos números altos é **[I]**, pelo `sl_consts.h`).
  - Em `0x1b81430`/`0x1b80230` (DLSS Frame Generation): **0 = Depth, 1 = MVec, 2 = HUDLessColor, 0x45 = UIColorAndAlpha [I]**. Essa marcação é chamada a partir de `0x1c1a210`, que também chama o frame generation do FSR (`0x1c64ed0` → `ffxDispatch`) com a command list de `[r9+0x2100a8]`.
  - **[I]** `0x1c1a210` é o epílogo do frame: tudo o que vem depois do HUD e antes do Present.

### 2.3 Conclusão: estratégia recomendada (em camadas)

**Recomendação principal [I, confiança média-alta]: um hook inline no engine, na função `0x1c091c0` (seletor AA/upscale), ou logo depois da chamada em `0x1c0ef30`.**
- Por que esse ponto:
  - Ele cobre **todos** os modos (TAA nativo, FSR, DLSS, XeSS). Hookear só `ffxDispatch`, `slEvaluateFeature` ou `xessD3D12Execute` deixaria de fora quem usa TAA ou FSR2 embutido.
  - Fica **depois da cena e da transparência** e **antes** de bloom, tonemap, finalize e HUD (Scaleform).
  - O composite entra em HDR linear e recebe o tonemap e o color grading do jogo, que é o efeito desejado.
- Como recuperar os recursos:
  - Pegar a cor de saída e o depth (`RTN_GBuffer3Depth`) a partir dos argumentos (`rcx`/`rdx`/`r8d`/`r9d` em `0x1c091c0`) ou do objeto `CRenderSurfaces`, convertendo os handles do GpuApi em `ID3D12Resource*` pela tabela `[ctx+0x7e008 + idx*0x140]` (o mesmo caminho usado pelo wrapper `0x1ed8c20`).
  - **[?]** Os layouts exatos ainda precisam de Ghidra.
- Como gravar o composite: usar a command list corrente do engine (`[dev+0x2100a8]`, **[I]**), ou uma command list própria submetida na mesma fila antes do resto do post-processing.
- Riscos:
  - A função é específica desta versão, então é preciso fazer signature scan ou checar o hash do exe (a versão está fixada acima).
  - O depth tem resolução de render enquanto a saída tem resolução de display quando há upscaler, então o depth test precisa reamostrar o depth.
  - A cor é HDR, então os texels do Minecraft precisam ser convertidos para linear e na escala certa de exposição.

**Alternativa A, quando um upscaler estiver ativo [O/I]:** interceptar as APIs dos upscalers dentro do processo, com hook na IAT do exe ou nos exports das DLLs:
- `slSetTagForFrame` (IAT `0x298dd20`) entrega **Depth, MVec, ScalingInput e ScalingOutput** como `sl::Resource` (contendo `ID3D12Resource*`), mais a command list e o frame token.
- `ffxDispatch` (IAT `0x298da10`) entrega o `ffxDispatchDescUpscale` com color, depth, motionVectors e output.
- `xessD3D12Execute` (IAT `0x298db78`) entrega `xess_d3d12_execute_params_t` com color, velocity, depth e output.
- Vantagens: APIs públicas e estáveis entre patches, e não exige reverse dos structs do engine.
- Desvantagem: não funciona com TAA nativo ou FSR2 interno.
- **Avaliação:** é útil como fonte de recursos e para validar o método principal. Hookear a IAT do exe (sem patch nas DLLs assinadas) basta e é mais seguro que patch inline em DLL de terceiros.

**Alternativa B, a mais simples e "estilo skycraft" [O/I]:** um hook em `ID3D12CommandQueue::ExecuteCommandLists` mais `IDXGISwapChain::Present`.
- Atenção: o jogo usa proxies do Streamline.
  - O hook deve ir na **vtable nativa** (de um device ou swapchain dummy criados pelo `d3d12`/`dxgi` do sistema).
  - Outra opção é hookear o próprio proxy depois que ele for criado.
  - O Streamline faz o seu hook de Present antes ou depois dos seus plugins de FG.
- Problema principal: no Present o HUD **já está desenhado** (existe `RTN_FinalColorUI`), e com DLSS-G, FSR-FG ou XeFG ligado o Present recebe também frames gerados.
- Também não dá acesso direto ao depth. Seria preciso adivinhar qual depth heurístico usar, como faz o ReShade.
- **Avaliação:** só serve como bootstrap (capturar device e fila, criar recursos compartilhados com o Minecraft), não como ponto de composite.

**Frame generation:**
- **[I]** Fazer o composite antes do HUD garante que o conteúdo também entre no `HUDLessColor` que vai para o FG. Mas os motion vectors do jogo não cobrem os blocos, o que gera artefatos de interpolação.
- **Recomendação:** desligar FG no MVP ou também escrever MVs.

### 2.4 Captura com RenderDoc
- **[O]** O Streamline detecta o RenderDoc e desliga os plugins NVAPI. A captura funciona, mas fica sem DLSS: é preciso usar TAA ou FSR.

---

## 3. Injeção: convenção de loader e DLLs proxy

- **[O]** Na pasta `x64_dx12\` hoje **não há** `dxgi.dll`, `version.dll`, `winmm.dll`, `dinput8.dll` nem `dsound.dll`, ou seja, nenhum loader instalado.
- **[O]** DLLs que o exe importa estaticamente e que **não** estão em KnownDLLs (lido do registro desta máquina), e que portanto seriam carregadas da pasta do exe se houvesse uma cópia lá:
  - **`WINMM.dll`** (6 funções: `timeGetTime`, `timeBeginPeriod`, …)
  - **`DINPUT8.dll`** (`DirectInput8Create`)
  - **`DSOUND.dll`** (ordinal #11, `DirectSoundCreate8`)
  - `XINPUT9_1_0.dll`, `dbghelp.dll`, `POWRPROF.dll`, `HID.DLL`, `WININET.dll`, `WINHTTP.dll`
  - Também as DLLs de terceiros na própria pasta.
  - `SETUPAPI`, `SHLWAPI`, `WS2_32`, `ole32`, `OLEAUT32`, `GDI32`, `USER32`, `ADVAPI32` e `SHELL32` estão em KnownDLLs.
  - Observação: o `Wldap32.dll` e o `normaliz.dll` que vêm na pasta (assinados pela CDPR, cópias da Microsoft) também estão em KnownDLLs e por isso são **ignorados**.
- **[O, web]** Convenção da comunidade:
  - O **Ultimate ASI Loader** (ThirteenAG) é usado neste jogo, colocado em `bin\x64_dx12`, como `dinput8.dll` ou como **`winmm.dll`** (há um pacote "Ultimate ASI Loader - WINMM" no Nexus, mod 13671).
  - Mods como W3LoadFix usam `dsound.dll`.
  - ReShade, OptiScaler e DLSS Enabler disputam `dxgi.dll`/`version.dll`, e as comunidades recomendam evitar conflitos nesses nomes.
- **Recomendação [I]:**
  1. Nossa DLL deve ser um **plugin `.asi`** carregado pelo Ultimate ASI Loader (`winmm.dll` ou `dinput8.dll`). Isso convive com outros mods e não exige proxy próprio.
  2. Plano B sem dependência: um proxy próprio de **`winmm.dll`** (só 6 exports, trivial de encaminhar ao System32). `version.dll` **não** é importado pelo exe; só valeria se alguma DLL dependente o importasse, o que não verifiquei.
  3. **Não** usar `dxgi.dll` nem `d3d12.dll` como proxy: o Streamline carrega os originais pelo System32 **[I]**, e esses nomes conflitam com ReShade e OptiScaler.
  4. Nunca substituir ou modificar `sl.*.dll`, `amd_fidelityfx_*` ou `libxess*`: são assinados, e o Streamline valida os seus plugins.
- O carregamento acontece cedo (um import do exe), antes do `slInit`. Isso permite instalar hooks na IAT do exe (`slSetTagForFrame`, `ffxDispatch`, `xessD3D12Execute`, `D3D12CreateDevice`) antes da criação do device.

---

## 4. Resumo de confiança e lacunas

| Item | Status | Confiança |
|---|---|---|
| D3D12/DXGI vêm do `sl.interposer` (import estático), não de `d3d12`/`dxgi` | O | alta |
| Sem packing, DRM ou anti-tamper no exe | O | alta |
| `0x1c091c0` é o seletor AA/upscale (TAA/FSR/DLSS/RR/XeSS) | O (estrutura) / I (semântica de cada caso) | média-alta |
| `0x1c0e420` é a cadeia de post-processing; o upscale acontece antes do tonemap e do HUD | I | média |
| Upscaler recebe cor HDR e depth reverse-Z | I (strings FSR2 + comunidade) | média |
| `0x1c1a210` é o epílogo do frame (tags DLSS-G, FSR-FG) | I | média |
| Hook na IAT de `slSetTagForFrame`/`ffxDispatch`/`xessD3D12Execute` dá color, depth e MV | O (imports) / I (uso) | alta, só com upscaler ligado |
| `winmm.dll` ou `dinput8.dll` (ASI loader) como vetor de injeção | O + web | alta |

**Lacunas (precisam de Ghidra ou RenderDoc):**
1. Assinatura e semântica dos argumentos de `0x1c091c0`: qual argumento é a cor de entrada, a de saída, o depth e o contexto, e o significado de `[rcx+0x74]` (enum de AA, 9 casos).
2. Layout de `CRenderSurfaces` e da tabela de texturas do GpuApi (`+0x7e008`, stride `0x140`): como obter `ID3D12Resource*` e o estado (`D3D12_RESOURCE_STATES`) de `RTN_GBuffer3Depth`, e se o depth está em `DEPTH_WRITE` ou `SRV` no ponto do hook.
3. Ordem real das passadas dentro de `0x1c0e420` (bloom, DOF, tonemap e motion blur antes ou depois do upscale). Uma captura do RenderDoc com TAA/FSR mostraria a ordem dos dispatches, o formato do depth (provavelmente D32_S8 ou R32, reverse-Z) e o formato da cor (provavelmente R11G11B10 ou RGBA16F).
4. Qual command list e qual fila estão ativas no ponto do hook (`[dev+0x2100a8]` parece ser a command list corrente do GpuApi), e se o engine grava passadas em paralelo (`ParallelRenderCommands`, `AsyncSubmit`).
5. Se o Streamline faz proxy de `ID3D12GraphicsCommandList` (a string "Command list state tracking disabled…" sugere que pode não fazer). Isso afeta onde fazer hook de `ExecuteCommandLists`.
6. Projeção e câmera (view/proj com jitter) para alinhar o Minecraft ao W3: fora deste escopo, mas `CRenderFrameInfo` e as constantes de `slSetConstants` (IAT `0x298dcc8`, chamada em `0x1b80e55`) são candidatos óbvios.

Fontes web:
- [Ultimate ASI Loader - WINMM (Nexus W3 #13671)](https://www.nexusmods.com/witcher3/mods/13671)
- [Ultimate-ASI-Loader releases](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases)
- [W3LoadFix (dsound.dll)](https://www.nexusmods.com/witcher3/mods/13881)
- [Gervant First Person (ASI/dinput8 FAQ)](https://www.nexusmods.com/witcher3/mods/5706?tab=posts)
- [ReShade forum: W3 NG + DLSS3 FG](https://reshade.me/forum/troubleshooting/8484-the-witcher-3-next-gen-dlss-3-frame-interpolation-crash)
- [Less Stutter Shader Cache (conflitos dxgi/version/winmm)](https://www.nexusmods.com/witcher3/mods/13168)
- [Witcher3 DLSS5-inspired ReShade (depth reversed)](https://www.nexusmods.com/witcher3/mods/12846)

---

## 5. Overlay da Steam e o hook de Present (crash de 2026-10-11)

Substitui a recomendação "o hook deve ir na vtable nativa" da Alternativa B (§2.3).

**Sintoma [O]:** dois crashes (`witcher3.exe.4900.dmp`, `witcher3.exe.21016.dmp`) por stack overflow, ~0,15 s depois do primeiro Present visto pelo overlay. O mw3.log mostra "excecao estrutural no desenho" logo antes, e o crash vem a seguir, mesmo com o overlay desligado.

**Evidência nos dumps e nos binários [O]** (gameoverlayrenderer64.dll com timestamp `0x6A98CE0D`, igual no disco e no dump; sl.interposer 2.14.1, timestamp `0x6A9AFD22`):
- A pilha do overflow tem 26.012 quadros de `gameoverlayrenderer64+0x93E50`, cada um de exatamente 0x50 bytes: 5 pushes, `sub rsp,0x20` e o endereço de retorno `+0x93F6F`. O `+0x167370` repetido é o `r12` salvo, um `CRITICAL_SECTION`. Não há quadro de outro módulo entre os níveis.
- `+0x93E50` é o hook da Steam para `DXGISwapChain_Present`. Ele faz `EnterCriticalSection` e depois `call [+0x167340]`, um **único global** com o "original".
- `r15` em cada quadro é `0x1FF6039A080`, a mesma swapchain que o mw3.log registrou ("overlay: swapchain 0x1FF6039A080"). Na base da pilha estão `sl.dlss_g+0x487DC`, `sl.interposer+0x2A843` e `witcher3.exe`.
- `+0x94660` é a função que loga "Hooking vtable for swap chain". Ela confere se a vtable fica no `dxgi.dll` (se não, loga "VTable for swap chain in unexpected module" e sai). Depois lê os slots 2, 8, 10 e 13 (e 22, 38 e 39 via QI) da vtable **nativa** e, se a função para onde cada slot aponta ainda não for desvio dela (`+0x8E980`), faz um desvio inline (`+0x8DAB0`) e grava o trampolim no global do método. Isso roda **a cada swapchain criada**, a partir dos hooks de `CreateSwapChain*` da factory nativa. Proxies em outro módulo são ignorados ("points to another module, skipping hooks").
- Os trampolins de Release e SetFullscreenState, que não hookeávamos, ficam no pool perto do dxgi (`0x7FF85D17xxxx`). Os de Present, ResizeBuffers, Present1, SetColorSpace1 e ResizeBuffers1, exatamente os slots que o mw3 escrevia, ficam no pool perto de mw3.asi, D3D12Core e sl.interposer (`0x7FFF971Axxxx`). Ou seja, a Steam desviou as **nossas** funções.
- `HookPresent` no mw3.asi antigo era `call OnPresent; mov rax,[g_origPresent]; jmp rax`, um tail call. O `t_depth` do `OnPresent` já tinha voltado a 0 antes de descer, então **cada nível da recursão desenhava de novo**. A exceção "estrutural" do desenho era o próprio `EXCEPTION_STACK_OVERFLOW`. No 21016, os últimos quadros têm `mw3+0x84FC` (FallbackQueue) e os IIDs `ID3D12Resource`/`ID3D12Device` (mw3 `.rdata` `+0x436F8`/`+0x43758`) como argumentos de GetBuffer/GetDevice. O `__except` engoliu o overflow sem `_resetstkoflw`, e o crash final foi access violation sem guard page.
- **Limitação:** os dumps não incluem páginas de código, então não dá para ler os bytes em `dxgi+0x19530` diretamente. O primeiro passe da Steam é deduzido: a cadeia só fecha se o Present nativo também começar com o desvio dela.

**Mecanismo [O/I]:**
1. A Steam desvia `dxgi!Present` (`+0x19530`) na primeira swapchain.
2. O mw3 escreve o slot 8 nativo, guardando "original = dxgi+0x19530", que agora começa com o `jmp` da Steam.
3. Nova swapchain (por exemplo, quando o FG ou o jogo recriam a swapchain): a Steam vê que o slot 8 aponta para `mw3!HookPresent`, desvia o hook e **sobrescreve** o global `+0x167340` com o trampolim do HookPresent.
4. Daí em diante: `dlss_g` → slot 8 → (jmp) Steam → trampolim → corpo do HookPresent → `jmp dxgi+0x19530` → (jmp) Steam → … e a recursão nunca termina.

Um guarda de reentrada no hook nativo não resolveria. A "próxima camada verdadeira" de um hook nativo seria o trampolim antigo da Steam, que se perdeu quando o global foi sobrescrito.

**Desenho adotado [O, mw3_overlaytest PASS]:**
- **Hook na vtable do proxy de swapchain do Streamline**, a camada que o jogo chama.
  - A classe é `sl::interposer::DXGISwapChain`, com vtable estática de 41 slots em `sl.interposer` RVA `0x794A8`. Slot 8 = `0x2A830`, a função que aparece na pilha do crash (`+0x2A843`).
  - O proxy é criado em `setupSwapchainProxy` (`0x29C50`, construtor `0x2A030`).
  - A vtable é achada pelo RTTI do MSVC (TypeDescriptor `.?AUDXGISwapChain@interposer@sl@@` → CompleteObjectLocator → ponteiro para o COL logo antes da vtable), sem offset fixo. Os 40 primeiros slots precisam apontar para o código do próprio sl.interposer.
  - A Steam não toca essa vtable (fica fora do dxgi.dll) nem as nossas funções. RTSS e Steam continuam desviando o Present nativo, por baixo do Streamline.
- O Streamline hookeia `GetBuffer` e `GetCurrentBackBufferIndex` (lista de hooks do plugin manager). Desenhar pelo proxy usa os mesmos buffers em que o jogo desenhou, inclusive com DLSS-G ligado.
- A vtable nativa do dxgi **nunca** é escrita. O único slot nativo que escrevemos é `ExecuteCommandLists` da fila do D3D12Core, para achar a fila. É transitório: sai assim que a fila é conhecida e só volta se ela sumir. A Steam não hookeia vtable de fila (não há string para isso).
- Cada hook encaminha para o valor guardado do slot (a função do SL), nunca pela vtable. A profundidade `thread_local` vale durante toda a chamada encaminhada: com o destrutor pendente, não há tail call. Uma chamada aninhada só repassa. Com 4 níveis ou mais na mesma thread, o hook trata como laço de terceiros: desliga, restaura os slots e corta.
- Ao desligar por exceção, todos os slots são restaurados quando ainda são nossos. Se outro módulo encadeou por cima, o hook fica como repasse puro. Um stack overflow pego chama `_resetstkoflw`.

**Riscos que continuam [I]:**
- Com frame generation, o overlay é desenhado no frame real antes da interpolação e não entra no `UIAlpha`/`HUDLessColor`. O HUD do MC pode tremer ou ter ghosting nos frames gerados.
- Se o Streamline mudar o nome da classe, ou não usar proxy ("IDXGISwapChain proxy not required, skipping"), o overlay não instala (loga e segue sem overlay). Ele não cai para a vtable nativa de propósito.
- Outra ferramenta que desvie o slot do proxy com a mesma lógica da Steam recriaria o problema. O corte em 4 níveis evita o crash, mas o frame cortado não é apresentado.

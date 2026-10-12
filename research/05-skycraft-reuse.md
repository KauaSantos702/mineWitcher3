# 05 — SkyCraft: o que reaproveitar para o MineWitcher3

Fonte estudada: https://github.com/chasmlol/skycraft, commit `bfcaf178524b92c2cdeb88e4ce0f13ef9ded6f32`
(2026-10-01), clonado só para leitura (`git clone --depth 1`) em
um clone local do repositorio. Nada foi compilado nem executado (gradle, cmake,
scripts `.py`/`.ps1`/`.bat` não foram rodados).

---

## 0. Licença (ler primeiro)

- **`LICENSE`: MIT License, "Copyright (c) 2026 chasmlol".** O texto é o MIT padrão: permite usar, copiar,
  modificar, mesclar, publicar, distribuir, sublicenciar e vender, **com uma condição: "The above copyright
  notice and this permission notice shall be included in all copies or substantial portions of the
  Software."** Sem garantia.
- `fabric.mod.json` também declara `"license": "MIT"`.
- `THIRD-PARTY-NOTICES.md` lista as dependências: CommonLibSSE-NG (MIT), spdlog (MIT), {fmt} (MIT), xbyak
  (BSD-3-Clause), SimpleIni (MIT) no plugin; Prism Launcher (**GPL-3.0**, só redistribuído sem alteração,
  com a licença junto), Fabric API (Apache-2.0), e4mc (MIT) no pacote do Minecraft. Minecraft, Java,
  Fabric Loader, Skyrim, SKSE e Address Library **não** vêm junto.

**O que isso significa para nós:** podemos copiar e adaptar código do SkyCraft livremente, desde que:

1. cada arquivo copiado/derivado mantenha (ou ganhe) um cabeçalho com `Copyright (c) 2026 chasmlol` + aviso MIT;
2. o repositório tenha um `THIRD-PARTY-NOTICES.md` com o texto completo da licença MIT do SkyCraft;
3. se um dia empacotarmos o Prism Launcher, ele vai sem modificação e com a licença GPL-3.0 dele (o nosso
   código não fica GPL por isso: o Prism é só um programa à parte que a gente inicia).

CommonLibSSE-NG é específico do Skyrim e não nos serve. Para o W3 o equivalente seria um hook manual
(MinHook/safetyhook) ou o framework que escolhermos para a DLL nativa.

---

## 1. Diferença importante: o DESIGN.md x o código real

O `docs/DESIGN.md` (rascunho v0.1) descreve um plano que **o código não seguiu** em vários pontos. Para
copiar decisões, valem as do código:

| Assunto | DESIGN.md (plano) | Código real (v0.1.2, protocolo v11) |
|---|---|---|
| Blocos no mundo | MC renderiza offscreen uma camada "World" com cor+profundidade, composta com depth test | **MC não renderiza o mundo** (`LevelRendererMixin` cancela `LevelRenderer.render`). O MC **exporta malhas** de cada seção 16³ (feitas pelo `ModelBlockRenderer` do próprio MC: modelo, tint, AO, luz) + o atlas de texturas por um *render ring*, e o **Skyrim desenha os triângulos no próprio frame** (`WorldRender.cpp`, D3D11, usa o depth buffer do Skyrim, sol, sombras, neblina, luzes do jogo) |
| Mão + HUD + GUI | Camadas Hand/GUI por interop de GPU | **Um único overlay RGBA8** (o `mainRenderTarget` inteiro do MC: mão, hotbar, telas) lido de volta para a CPU de forma assíncrona e copiado para um **triple buffer na memória compartilhada**; o Skyrim sobe para uma textura D3D11 e desenha um triângulo de tela cheia no `Present` |
| Interop GPU (WGL_NV_DX_interop / Vulkan external memory) | Planejado | **Não existe.** Só o caminho por CPU ("fallback") |
| Eventos nomeados (named events) | Planejado | **Não usados.** Tudo é polling: seqlock + spin-wait. Só existe um **mutex nomeado** `Local\SkyCraft_v1_minecraft` ("um MC já está rodando") |
| Câmera | MC envia view+projection matrix | MC envia **olho, pés, FOV vertical, bob, modo F5 e distância**; o Skyrim monta a própria câmera. O **yaw/pitch é integrado no lado do jogo** (fórmula de sensibilidade do MC) e enviado ao MC (latência zero na câmera) |
| Colisão | AABBs por seção, 1/8 bloco | Dois formatos: **voxels 8×8×8 por bloco** (máscara de 512 bits, para mobs/fluidos/projéteis, injetados em `BlockCollisions`) **e triângulos exatos** (para o jogador local, colisor próprio `TriCollider` chamado após `Entity.collide`) |
| Lockstep de frame | "BeginFrame N"/"FrameReady N" com fence | MC espera o `SkyState.seq` mudar (o jogo incrementa a cada frame), timeout de 25 ms; overlay chega ~1 frame atrasado |

---

## 2. protocol/

Um único arquivo: `protocol/skycraft_protocol.h` (568 linhas). **Não há gerador nem schema**: o header
C++ é a fonte da verdade e o Java espelha à mão em `fabric/src/main/java/dev/skycraft/link/Proto.java`
(constantes de offset). Testes de layout só via `static_assert` no C++ e o stand-in `tools/fake_skyrim.py`.

### 2.1 Mapeamento `Local\SkyCraft_v1`

- Criado pelo **jogo** (`CreateFileMappingW`, page file, ~176 MB), aberto pelo MC (`OpenFileMappingW` +
  `MapViewOfFile` via **Java FFM/Panama**, sem JNA/JNI; precisa `--enable-native-access=ALL-UNNAMED`).
- DACL explícita (SDDL `D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;<SID do usuário>)S:(ML;;NW;;;ME)`) para um jogo
  rodando como admin não bloquear o MC não elevado. **Copiar isto.**
- Little-endian, structs de tamanho fixo, `kMagic = 'SKYC'`, `kVersion = 11`. Escala `kUnitsPerBlock = 70`.
  **Coordenadas no protocolo são sempre espaço do Minecraft** (blocos, Y para cima, Z para o sul); a conversão
  fica no lado do jogo.

| Offset | Região | Direção | Mecanismo |
|---|---|---|---|
| `0x0` | `Header` {magic, version, skyrimPid, mcPid, heartbeats `GetTickCount64`} | ambos | campos atômicos |
| `0x100` | `SkyState` (0x40): flags InGame/MenuOpen/Loading, worldId, collisionEpoch, pos, yaw/pitch, teleportSeq, viewport W/H, gameHour | jogo→MC | **seqlock** (seq ímpar = escrevendo; +2 por frame do jogo → também é o "clock" de pacing) |
| `0x200` | `McState` (0xC8): flags (onGround, sneak, sprint, dead, swim, fly, screenOpen), pés interpolados, yaw/pitch, eyeHeight, sensibilidade, teleportAck, guiScale, FOV, bob, olho, **ticks crus de 20 Hz com timestamp QPC** (prev/cur pés, eye, walkDist, bob, tickMs), cameraMode (F5) e cameraDistance | MC→jogo | seqlock |
| `0x300` | `OverlayCtl` + 3× `OverlaySlotHdr` | MC→jogo | **triple buffer lock-free** por `xchg` (bits 0-1 índice do meio, bit 2 "dirty") |
| `0x400` | `WaterGrid` 16×16 alturas de água | jogo→MC | seqlock |
| `0x1000` | anel de input, 4096 × `InputEvent{u16 type,u16 code,i32 a,b,c}` | jogo→MC | **SPSC** (head u64 em +0, tail u64 em +0x40, dados em +0x80, linhas de cache separadas) |
| `0x12000` | `ActorTable` 256 × `ActorRecord` (64 B: formId, flags, pos, yaw, width, height, healthFrac, level, nome) | jogo→MC | seqlock |
| `0x17000` | anel de eventos 512 × `McEvent` (32 B: HitActor, PlayerDied, Explosion, ArrowStuck, SkillUse) | MC→jogo | SPSC |
| `0x1C000` | `WorldEntities` 160 × 96 B (flechas, itens no chão, rachaduras, sombras) + caixa de seleção | MC→jogo | seqlock |
| `0x20000` | anel de colisão, 32 MB, mensagens `{u32 type,u32 bytes}` alinhadas a 8 (`ColClear`, `ColRegion`+`ColBlock[]`, `ColTris`+`ColTri[]`, `ColPad`=volta ao início) | jogo→MC | anel de bytes SPSC |
| depois | 3 slots de overlay de 3840×2160×4 | MC→jogo | triple buffer |
| depois | anel de render, 64 MB (`RenAtlas`, `RenSection`, `RenTexture`, `RenAvatar`, `RenScene`, `RenLights`, `RenSolids`, `RenDug`, `RenRagdoll`…) com `RenVertex` de 32 B | MC→jogo | anel de bytes SPSC |

Detalhes que valem copiar:
- `ColBlock`: {x,y,z,pad, u64 bits[8]} = 80 B, `bits[y]` bit `(z*8+x)` = sub-voxel de 1/8 de bloco.
  Regiões de 8×8×8 blocos (`REGION_SIZE = 8`, tem que bater nos dois lados). `ColRegion` **substitui** tudo
  dentro da caixa, então o produtor manda regiões inteiras, sem deltas finos.
- `ColTri` = 9 floats + flags (StairHelper, Diggable+material, Ghost, Terrain).
- `collisionEpoch`: mudou de mundo/célula → MC joga fora toda colisão.
- `teleportSeq` / `teleportAck`: o jogo manda o MC teleportar (load, porta, fast travel); o MC só confirma depois
  que o chão chegou (`holdUntilReady`).
- Heartbeat: o jogo bate no `Present` (roda mesmo em menus); timeout no MC 8 s (telas de loading), no jogo 3 s.
- `InputType`: `kInKey` (código = **scancode SDL/USB HID**), `kInMouseButton` (botões SDL 1..5), `kInScroll`
  (×120), `kInCursor` (posição absoluta em pixels do overlay), `kInText` (code point), `kInReleaseAll`,
  `kInHurt` (dano do jogo no jogador), `kInOpenMenu`.

**Classificação**
- (a) **as-is**: o mecanismo inteiro (header, seqlock, SPSC de entradas fixas, anel de bytes com `Pad`,
  triple buffer do overlay, mutex "running", SDDL), `InputEvent`, `McState`, `OverlayCtl/SlotHdr`,
  `ColBlock/ColRegion/ColTri`, `WorldEntities`, todo o render ring (`Ren*`).
- (b) **com mudanças**: renomear (`Local\MineWitcher3_v1`, magic novo, versão própria); `kUnitsPerBlock = 1.0`
  (W3 já é metro); `SkyState` → `GameState` (trocar `worldId` FormID por um hash do nome do mundo do W3,
  ex. `novigrad`, `skellige`, `kaer_morhen`; `gameHour` vem do `GameTime` do W3); `ActorRecord.formId` →
  id estável de entidade do W3 (ex. `EntityHandle`/GUID truncado), `level` igual; `DigMaterial` remapeado para
  materiais do W3; `SkillUse` (Smithing/Block/armaduras do Skyrim) → algo do W3 ou remover.
- (c) **Skyrim-only**: `kHurtBlockedInSkyrim`/`kHurtPowerAttack` (podem virar flags genéricas), `RenRagdoll`
  (o ragdoll é do Havok do Skyrim, mas a ideia serve), `kSkillUse` com ActorValues.

---

## 3. fabric/ (Java, MC 26.3, Fabric Loader 0.19.5) — o lado mais reaproveitável

Build: `fabric-loom 1.18-SNAPSHOT`, `minecraft_version=26.3`, `loader_version=0.19.5`,
`fabric_api_version=0.161.0+26.3`, Java 25 (`options.release = 25`), `splitEnvironmentSourceSets()`
(src/main + src/client), mixins `compatibilityLevel: JAVA_25`, `injectors.defaultRequire: 1` (Mixin que não
casa = crash no load, bom para detectar quebra de versão). Usa MixinExtras (`@WrapOperation`, embutido no
Fabric Loader). Dimension type próprio `skycraft:mirror` (`min_y -1024`, `height 2048`) + world preset flat
void (`minecraft:the_void`, sem layers).

Observações sobre o MC 26.3 que o código revela (úteis para nós):
- Janela/entrada são **SDL3** (`org.lwjgl.sdl.SDLVideo.SDL_HideWindow`, `SDLKeyboard.SDL_GetKeyFromScancode`),
  não GLFW. Teclas são scancodes SDL.
- Render: `com.mojang.renderpearl.api.*` (`GpuBuffer`, `GpuBufferSlice`, `GpuTexture`) +
  `RenderSystem.getDevice().createCommandEncoder().copyTextureToBuffer(...)` com callback de conclusão.
- Pipeline de render-state: `LevelExtractor`, `CameraRenderState`, `SubmitNodeCollector`,
  `EntityRenderDispatcher.submit(state, cameraState, x, y, z, poseStack, collector)`.
- `net.minecraft.resources.Identifier` (antigo ResourceLocation).

### 3.1 Lista completa de Mixins (alvos exatos no MC 26.3)

**Client (`skycraft.client.mixins.json`, pacote `dev.skycraft.client.mixin`)**

| Mixin | Classe alvo | Método / ponto | Função | Classe p/ W3 |
|---|---|---|---|---|
| `WindowMixin` | `com.mojang.blaze3d.platform.Window` | `isFocused` HEAD → true enquanto linkado; `isIconified` HEAD → false | janela escondida mas "focada" | (a) |
| `InputConstantsMixin` | `com.mojang.blaze3d.platform.InputConstants` | `isKeyDown(int)` HEAD → teclado virtual; `grabMouse(Window,double,double)` / `releaseMouse(...)` HEAD cancel | MC nunca toca no mouse real | (a) |
| `MinecraftMixin` | `net.minecraft.client.Minecraft` | `runTick(boolean)` HEAD → `beginFrame()`; `renderFrame(boolean)` INVOKE `GameRenderer.render()V` AFTER → `afterRender()`; `renderFrame` TAIL → `paceFrame()` | laço por frame: ler estado/input, exportar, pacing | (a) |
| `FramerateLimitTrackerMixin` | `com.mojang.blaze3d.platform.FramerateLimitTracker` | `getFramerateLimit` → 260 | sem limitador próprio | (a) |
| `LevelRendererMixin` | `net.minecraft.client.renderer.LevelRenderer` | `render(GraphicsResourceAllocator, boolean, CameraRenderState, GpuBufferSlice, Vector4f, boolean, boolean)V` HEAD cancel | MC não desenha céu/terreno/neblina: overlay = só mão+HUD com fundo transparente | (a) |
| `WaitingForPlayerChunkMixin` | `net.minecraft.client.multiplayer.LevelLoadTracker$WaitingForPlayerChunk` | `isReady` → true | não esperar compilar chunk que nunca é desenhado | (a) |
| `ScreenMixin` | `net.minecraft.client.gui.screens.Screen` | `isPauseScreen` → false | o mundo MC não pausa com inventário aberto | (a) |
| `EntityCollideMixin` | `net.minecraft.world.entity.Entity` | `collide(Vec3)` RETURN → `SkyCollider.collide` (só `LocalPlayer`) | colisão suave com triângulos do jogo | (a) |
| `CameraZoomMixin` | `net.minecraft.client.Camera` | `getMaxZoom`: wrap `Level.clip(ClipContext)` | câmera F5 encolhe contra paredes do jogo | (a) |
| `LevelExtractorMixin` | `net.minecraft.client.renderer.extract.LevelExtractor` | `setSectionDirty(IIIZ)V` HEAD | marca seção para re-malhar e enviar ao jogo | (a) se mantivermos "o jogo desenha os blocos" |
| `ClientLevelDigMixin` | `net.minecraft.client.multiplayer.ClientLevel` | `setBlocksDirty(BlockPos,BlockState,BlockState)` HEAD | escavação do terreno | (b) (só se fizermos "cavar o W3") |
| `MinecraftDigMixin` | `net.minecraft.client.Minecraft` | `startAttack` HEAD cancelável; `continueAttack(boolean)` HEAD | escavação | (b) |
| `ChatComponentMixin` | `net.minecraft.client.gui.components.ChatComponent` | `addMessage` `@ModifyVariable` | Discord/e4mc | dispensável |
| `IntegratedServerMixin` | `net.minecraft.client.server.IntegratedServer` | `getMaxPlayers` → 100 | multiplayer | dispensável |
| `WorldOpenFlowsMixin` | `net.minecraft.client.gui.screens.worldselection.WorldOpenFlows` | `openWorldCheckWorldStemCompatibility`: wrap `askForBackup(LevelStorageAccess, boolean, Runnable, Runnable)` | pula o aviso de "experimental" do dimension type custom | (a) |
| Accessors | `ClientLevel.destroyingBlocks`, `RenderSetup.textures`, `RenderType.state/name`, `RenderSetup$TextureBinding.location`, `SpriteContents.originalImage`, `TextureAtlas.texturesByName/width/height`, `TextureManager.byPath` | — | exportar atlas/texturas/skins | (a) |

**Common (`skycraft.mixins.json`, pacote `dev.skycraft.mixin`)**

| Mixin | Classe alvo | Método / ponto | Função | Classe p/ W3 |
|---|---|---|---|---|
| `BlockCollisionsMixin` | `net.minecraft.world.level.BlockCollisions` | `computeNext`: wrap `CollisionContext.getCollisionShape(BlockState, CollisionGetter, BlockPos)` → `Shapes.or(blockShape, SkyCollision.shapeAt(pos))` (pula entidades com colisor suave) | **injeção de colisão** do jogo em toda query de blocos; step-up, onGround, dano de queda vanilla | (a) |
| `PlayerEdgeMixin` | `net.minecraft.world.entity.player.Player` | `maybeBackOffFromEdge(Vec3, MoverType)` → delta | agachar não trava em terreno que não é bloco | (a) |
| `EntityPickMixin` | `Entity` | `pick`: wrap `Level.clip` → `SkyClip.refine` | mira acerta superfícies do jogo (colocar bloco no terreno) | (a) |
| `ItemMixin` | `net.minecraft.world.item.Item` | `getPlayerPOVHitResult`: wrap `Level.clip` | baldes/barcos no terreno | (a) |
| `ProjectileUtilMixin` | `net.minecraft.world.entity.projectile.ProjectileUtil` | `getHitResult`, `getHitEntitiesAlong`: wrap `Level.clipIncludingBorder` | projéteis | (a) |
| `AbstractArrowMixin` | `...projectile.arrow.AbstractArrow` | `tick`: wrap `clipIncludingBorder`; `onHitEntity` HEAD + wrap `LivingEntity.setArrowCount(I)V` | flecha crava no terreno / no NPC | (a)/(b) |
| `ProjectileMixin` | `...projectile.Projectile` | `hitTargetOrDeflectSelf` HEAD | acerto em "ar" que é geometria do jogo | (a) |
| `BlockStateBaseMixin` | `BlockBehaviour.BlockStateBase` | `isFaceSturdy(BlockGetter, BlockPos, Direction, SupportType)Z` | tocha/trilho apoiados no terreno | (a) |
| `FallingBlockMixin` | `net.minecraft.world.level.block.FallingBlock` | `tick(BlockState, ServerLevel, BlockPos, RandomSource)` | areia para no chão do jogo | (a) |
| `FlowingFluidMixin` | `net.minecraft.world.level.material.FlowingFluid` | `canPassThroughWall(...)` static | água/lava escorrem sobre o terreno | (a) |
| `EntityFluidInteractionMixin` | `net.minecraft.world.entity.EntityFluidInteraction` | `update`: wrap `hasFluidAndLoaded(Level,IIIIII)`, `BlockGetter.getFluidState`, `FluidState.getHeight`, `getHeightForCamera` | nadar na água do jogo | (a) |
| `EntitySwimMixin` | `Entity` | `updateSwimming`: wrap `Level.getFluidState` | sprint-swim | (a) |
| `PressurePlateMixin` | `PressurePlateBlock`, `WeightedPressurePlateBlock` | `getSignalStrength`: wrap `AABB.move(BlockPos)` | placa no terreno irregular | (a) |
| `ServerExplosionMixin` | `net.minecraft.world.level.ServerExplosion` | `explode` HEAD/RETURN/`@ModifyVariable STORE`; `calculateExplodedPositions`: wrap `ExplosionDamageCalculator.getBlockExplosionResistance(...)` | explosões cavam o terreno e avisam o jogo | (b) |
| `ServerPlayerMixin` | `net.minecraft.server.level.ServerPlayer` | `crit(Entity)` HEAD; `die(DamageSource)` HEAD | crítico no NPC, morte no MC = morte no jogo | (a) |
| `MinecraftServerFlightMixin` | `net.minecraft.server.MinecraftServer` | `allowFlight` → true | anti-kick | (a) |
| `SmithingMixin` | `net.minecraft.world.item.ItemStack` | `onCraftedBy(Player, int)` | treina Smithing do Skyrim | (c) |

### 3.2 Como a janela fica escondida mas focada

1. A instância Prism passa `-Dskycraft.startHidden=true` (e `--enable-native-access=ALL-UNNAMED`).
2. `SkyClient.beginFrame()` → `SDLVideo.SDL_HideWindow(minecraft.getWindow().handle())` uma vez, zera música.
3. `WindowMixin` diz "focado" e "não minimizado"; `InputConstantsMixin` cancela `grabMouse/releaseMouse` e
   responde `isKeyDown` com o teclado virtual do `InputBridge`.
4. `applyLinkedOptions()`: `pauseOnLostFocus=false`, vsync off, `framerateLimit 260`, render/simulation
   distance 8, autoJump off, tutorial off, música 0.
5. `applyViewportSize()`: `getWindow().setWindowed(w,h)` com o tamanho do backbuffer do jogo
   (`SkyState.viewportW/H`), assim o render target do MC tem o tamanho exato da tela do jogo.

→ **(a) as-is.** Flag `-D...showWindow=true` para depurar.

### 3.3 Como o overlay (cor) é produzido e compartilhado

- `FrameExporter.capture()` depois de `GameRenderer.render()`: pega
  `minecraft.gameRenderer.mainRenderTarget().getColorTexture()`, cria até 3 `GpuBuffer` de staging
  (`createBuffer(label, 9, w*h*4)`), chama `copyTextureToBuffer(color, buffer, 0, callback, 0)`; quando o
  callback marca READY, no frame seguinte faz `buffer.map(true,false)` e `MemorySegment.copy` para o slot
  "back" da memória compartilhada; publica com `xchg` no `OverlayCtl`. Linhas bottom-up (flag bit0).
- Alfa **pré-multiplicado** (fundo transparente porque o `LevelRenderer` foi cancelado).
- **Não há profundidade** no overlay: a mão vai por cima de tudo, como HUD. Os blocos do mundo vão por outro
  caminho (3.5).
- Custo: um memcpy de ~33 MB/frame em 4K, latência ~1 frame. É o "PBO fallback" do design.

→ **(a) as-is no lado Java.** No W3, o lado nativo precisa de upload D3D12 (ver 4.2). Interop GPU real
(OpenGL ↔ D3D12 por `WGL_NV_DX_interop2` não existe para D3D12; seria via D3D11On12 ou textura compartilhada
NT handle + `glImportMemoryWin32HandleEXT` do `GL_EXT_memory_object_win32`) é otimização para depois.

### 3.4 Entrada (input)

- O jogo captura teclado/mouse, converte **DirectInput scan code → scancode SDL/HID** (tabela em
  `skse/src/Input.cpp`, linhas 7-33; genérica, só depende de DIK) e empurra no anel.
- `InputBridge.drain()` (no início de `runTick`) despacha para os handlers reais do MC:
  `minecraft.keyboardHandler.keyPress(handle, action, new KeyEvent(scancode, keycode, modifiers))`
  (keycode via `SDL_GetKeyFromScancode`; action 1/0/-1 repeat), `mouseHandler.onButton(handle, new
  MouseButtonInfo(button, mods), 0|1)`, `mouseHandler.onScroll(handle, 0, notches)`,
  `mouseHandler.onMove(handle, x, y, dx, dy)` (cursor absoluto, só para GUIs),
  `keyboardHandler.textInput(handle, String)`.
- **Olhar (yaw/pitch) não passa pelo MC**: o jogo integra o delta do mouse com a fórmula do MC
  (`s = sens*0.6+0.2; f = s³*8*0.15`) e manda em `SkyState.yaw/pitch`; `SkyClient.beginFrame` faz
  `player.setYRot/setXRot` (+`yRotO/xRotO`). Câmera do jogo sem latência.
- Lista de teclas reservadas ao jogo (Esc, G ativar, J, M, H, F9, console); `O` abre o PauseScreen do MC
  (`kInOpenMenu`).

→ `InputBridge.java`: **(a)**. Tabela DIK→SDL: **(a)** se o W3 nos der DIK/VK (precisaremos de VK→SDL se
capturarmos via `WM_INPUT`/`WndProc`). Hooks `MenuControls`/`PlayerControls`/`BSInputDeviceManager`: **(c)**.

### 3.5 Blocos, entidades e skin do jogador (F5)

- `WorldExporter`: re-malha seções sujas com `new ModelBlockRenderer(ao, true, blockColors)` + `FluidRenderer`
  num `VertexConsumer` próprio, remove o sombreamento fixo por face do MC (o jogo reilumina por normal),
  manda `RenSection` (triângulos `RenVertex` relativos à seção), `RenLights` (blocos emissores),
  `RenSolids` (para NPCs colidirem), itens/flechas/rachaduras/seleção como `WorldEntities`.
- `SkyAtlas`: atlas de blocos + itens empilhados num RGBA, com sprites animados mandados como `RenAtlasRegion`.
- `AvatarExporter implements SubmitNodeCollector`: chama
  `minecraft.getEntityRenderDispatcher().submit(state, cameraState, 0,0,0, new PoseStack(), this)` para o
  `LocalPlayer` e grava `submitModel/submitItem/submitBlockModel/submitQuadParticleGroup` como triângulos
  (pose e animação do próprio MC, skin, armadura, item na mão, capa) relativos aos pés → `RenAvatar` só em
  terceira pessoa. Também block entities (`getBlockEntityRenderDispatcher().submit`) e partículas →
  `RenScene`. Texturas (skin baixada, armadura) vão como `RenTexture` lidas por `TextureManagerAccessor`.
- Resultado: o jogo desenha **geometria** do MC com a própria iluminação/sombra/neblina e o próprio depth
  buffer. Nada de composição de profundidade entre dois framebuffers.

→ Java: **(a) as-is** (é todo game-agnóstico). Lado nativo: reescrever o renderer (D3D12, achar o depth buffer
e as constantes de luz do W3) é o **maior trabalho** do port. A skin do jogador entra
naturalmente pelo `AvatarExporter` (é a skin da conta do MC).

### 3.6 Câmera exportada

`SkyClient.afterRender()` escreve: pés interpolados (`player.getPosition(partial)`), olho
(`camera.position()` ou `getEyePosition(partial)` em F5), `camera.getFov()` (vertical, com sprint/fluido),
`options.getCameraType().ordinal()` (0 1ª pessoa, 1 atrás, 2 frente), `cameraDistance` (já com o zoom
colidido de `Camera.getMaxZoom`), bob de `gameRenderState().levelRenderState.cameraRenderState.entityRenderState`
(`backwardsInterpolatedWalkDistance`, `bob`). `publishTick()` (fim de cada tick) manda os ticks crus de 20 Hz
com `QueryPerformanceCounter` corrigido pelo partial tick; o jogo interpola no próprio relógio (evita
judder entre os dois frame rates). O jogo converte FOV vertical do MC para o FOV dele.

→ **(a)** no Java. Lado W3: (b) aplicar no `CCustomCamera`/câmera do jogo (nativo ou WitcherScript).

### 3.7 Colisão

- `SkyCollision` (main): consumidor do anel de colisão numa thread própria; guarda `VoxelShape` por bloco
  (das máscaras 8³), triângulos por região, `KNOWN_REGIONS`, `FILL` (fração/altura do chão por bloco para
  fluidos). `shapeAt`, `isKnown`, `hasSolidBelow`, `groundTop`, `trianglesNear`.
- `TriCollider` + `SkyTri` + `SkyCollider`: colisor de cilindro contra triângulos (anda em rampa ≤ ~45°,
  gruda descendo, desliza em paredes, teto), depois re-colide com blocos do MC via
  `Entity.collideBoundingBox`. Testado por `TriColliderTest`/`SkyRayTest` (JUnit).
- `SkyClip`/`SkyRay`: raycast contra triângulos para mira/projéteis.
- `SkyClient.holdUntilReady`: congela o jogador após teleporte até a colisão em volta chegar (com timeout de
  6 s) e o "levanta" para fora do chão.
- `SkyWater`: grade 16×16 de alturas → `FluidState` de água.

→ **(a) as-is.** Só depende do formato do anel. Toda a extração (Havok, `bhkWorld`, decodificação de shapes
em `Collision.cpp`) é **(c)**; no W3 é PhysX (ver `tools/scripts/collision/physx_probe.py` nosso). O
algoritmo de voxelização (SAT triângulo×caixa de Akenine-Möller, "paredes íngremes 50-84° viram colunas
de bloco inteiro", envio por regiões 8³ numa worker thread) é **(b)** reaproveitável em C++.

### 3.8 Combate e resto

- `SkyrimActorEntity extends LivingEntity` (invisível, `NoopRenderer`) + `SkyCombat` + `ProxySync`: proxies
  hitáveis por `ActorRecord`; o dano vanilla é coletado por tick e enviado como `kEvHitActor`; o dano do jogo
  volta como `kInHurt` → `player.hurt` com damage types `skycraft:skyrim_*`. → **(b)** (renomear para
  `WitcherActorEntity`; escala de dano diferente: W3 tem HP na casa de milhares).
- `MirrorWorld`: abre/cria o mundo void sozinho quando linkado. → (a).
- `SkyDig*`, `DigWalls`, `DestructionToggle`, `BlockLightColors`: cavar o terreno do jogo. → (b), fase tardia.
- `DiscordPresence`, `SkyNet` (multiplayer/e4mc), `/join` `/leave`: → opcional.
- `SkyClient.quitWithSkyrim`: MC fecha 5 s depois que o PID do jogo morre; fecha também se ficou 10 min
  escondido sem conectar. → (a).

---

## 4. skse/ (C++ / CommonLibSSE-NG, D3D11)

| Arquivo | O que faz | Classe p/ W3 |
|---|---|---|
| `Link.cpp/.h` | dono do mapeamento: criação com SDDL, reset das regiões, seqlock write/read, SPSC push/pop, anel de bytes, triple buffer consumidor, `SkyToMc/McToSky`, `HeadingToMcYaw` | **(a)** núcleo IPC; trocar só a conversão de coordenadas e tirar `RE::NiPoint3` |
| `Launcher.cpp` | inicia o MC: descompacta o bundle Prism portátil em `%LOCALAPPDATA%\SkyCraft` (preserva conta/mundo), lê `.ini` (`sLauncher`, `sArguments = --launch SkyCraft`), inicia **via shell do Explorer** (`IShellDispatch2::ShellExecute`, fora da VFS do MO2), checa o mutex `Local\SkyCraft_v1_minecraft` para não abrir dois, roda numa thread | **(b)**: trocar caminhos (`Data/SKSE/Plugins` → pasta do nosso mod/bin do W3), nome da instância |
| `Overlay.cpp` | hook de `IDXGISwapChain::Present` por **vtable[8]**; sobe o slot do triple buffer em textura `D3D11_USAGE_DYNAMIC` (Map WRITE_DISCARD), desenha triângulo fullscreen (SV_VertexID) com blend pré-multiplicado `ONE / INV_SRC_ALPHA`, flipY, cursor desenhado no shader, segunda passada com blend "invert" para a mira do MC; salva/restaura o estado D3D11 | **(b)**: reescrever em **D3D12** (ver 4.2). O HLSL serve quase igual |
| `WorldRender.cpp` (3372 linhas) | desenha seções/atlas/avatar/cena do MC dentro do frame do Skyrim: lê o depth buffer (detecta convenção normal/reversa com 16 sondas), sol/lua, ambiente direcional (DALC), 16 luzes pontuais, neblina, color grading, cascatas de sombra do Skyrim copiadas, shadow map próprio dos blocos, AO de contato | **(b)/(c)**: a ideia e os shaders servem de referência; todo acesso a dados do renderer é Skyrim-only |
| `Collision.cpp/.h`, `Clip.h` | extrai Havok perto do jogador, voxeliza numa worker | algoritmo (b), extração (c) |
| `Game.cpp/.h` | puppet do PlayerCharacter, câmera (1ª/3ª pessoa), interpolação de ticks, FOV, HUD, sneak, takeover por mobília/cavalo/cena, morte | lógica de interpolação/câmera (b); APIs (c) |
| `Input.cpp` | `BSTEventSink<InputEvent*>`, hooks `MenuControls`/`PlayerControls`, tabela DIK→SDL, roteamento de teclas | tabela (a), hooks (c) |
| `Combat.cpp`, `NpcBlocks.cpp`, `PathAvoid.cpp`, `Dig*.cpp`, `BlockLights.cpp`, `CrashLog.cpp` | dano no NPC, NPCs colidindo com blocos, navmesh, buracos no terreno, luzes MC no Skyrim | (c) (ideias reaproveitáveis) |
| `tools/fake_skyrim.py`, `fake_guest.py` | **stand-in do jogo** em Python: cria o mapeamento, manda chão plano + escada, segura W/Espaço, imprime o `McState` e salva o overlay em PNG | **(a)** muito útil: testar o Fabric sem abrir o W3 (só mudar nomes/offsets) |
| `tools/minecraft-bundle/` | `instance.cfg` (`JvmArgs=--enable-native-access=ALL-UNNAMED -Dskycraft.startHidden=true`, `MaxMemAlloc=4096`), `mmc-pack.json` (MC 26.3, LWJGL 3.4.3, Fabric 0.19.5), `prismlauncher.cfg` (`CloseAfterLaunch`, `QuitAfterGameStop`, `LowMemWarning=false`) | **(a)** |

### 4.1 Coordenadas: Skyrim → W3

SkyCraft: `mc.x = sky.x/70`, `mc.y = sky.z/70`, `mc.z = -sky.y/70`; yaw MC = heading(rad)·57.2958 + 180.

Para o W3 (metros, Z para cima, +Y norte): **`mc.x = w.x`, `mc.y = w.z`, `mc.z = -w.y`**, inverso
`w = (mc.x, -mc.z, mc.y)`, escala 1. Yaw do W3 (`EulerAngles.Yaw` em graus) precisa ser fixado num teste
(sinal e offset; no W3 o yaw cresce no sentido anti-horário visto de cima, a confirmar). Alturas do W3 cabem
no dimension type `min_y -1024 / height 2048`. Escala 1:1 deixa o jogador do MC (1,8 m) bem perto do Geralt
(~1,9 m) — melhor que no Skyrim. Coordenadas X/Y do W3 chegam a alguns milhares de metros: `double` no
protocolo e floats relativos à seção/câmera (como o SkyCraft faz) evitam perda de precisão.

### 4.2 D3D11 → D3D12 (W3 next-gen)

- Hook de `Present` continua por vtable de `IDXGISwapChain` (índice 8; `Present1` é 22), mas em D3D12
  precisamos também da **`ID3D12CommandQueue`** do jogo (hook em `ExecuteCommandLists`, vtable índice 10 da
  queue, para capturar a queue DIRECT que apresenta).
- Overlay: um `ID3D12Resource` de upload (persistentemente mapeado, 3 cópias em ring por frame-in-flight),
  `CopyTextureRegion` para uma textura DEFAULT, barrier do backbuffer `PRESENT → RENDER_TARGET → PRESENT`,
  RTV heap próprio, root signature com 1 SRV + 1 CBV + sampler estático, PSO com o mesmo blend pré-multiplicado.
  Fence própria para não sobrescrever o upload que a GPU ainda lê.
- Alternativa rápida: **D3D11On12** sobre a queue do jogo e portar `Overlay.cpp` quase sem mudanças (custo
  de desempenho maior; bom para o primeiro protótipo).
- Blocos no frame do jogo (estilo `WorldRender`): precisa achar o depth buffer do W3 e o estado dele
  (provavelmente reverse-Z), e as matrizes de câmera; com RenderDoc/PIX. Plano B para MVP: renderizar os
  blocos com a câmera do jogo **no próprio MC** e compor como overlay (perde oclusão por paredes do W3), ou
  continuar com entidades WitcherScript (o que nosso `mod/` já faz com caixotes).

---

## 5. Decisões de design a copiar

1. **Nenhum dos dois jogos é reescrito**: o MC roda a física/inventário/combate vanilla; o jogo só traduz.
   Se estivermos reimplementando mecânica do MC em C++, está errado.
2. **O jogo cria a memória compartilhada; o MC abre** (com polling de 1 s) e valida magic/versão. PID nos dois
   lados; mudou o PID → reinicia tudo (`generation++`, reenviar atlas/colisão).
3. **Coordenadas do protocolo em espaço MC**; conversão só no lado nativo.
4. **Seqlock para "último valor"**, **SPSC com cabeça/cauda em linhas de cache diferentes** para eventos,
   **anéis de bytes com mensagem `Pad`** para streams grandes, **triple buffer por `xchg`** para o overlay.
   Sem named events: pacing pelo `seq` do estado do jogo (+2 por frame) com timeout de 25 ms.
5. **Heartbeat no `Present`** (roda até em menus/loading); timeouts generosos (8 s) por causa das telas de loading.
6. **MC escondido mas "focado"** por Mixins (`Window.isFocused`, `InputConstants.*`) + `SDL_HideWindow`; o
   tamanho da janela MC = backbuffer do jogo.
7. **MC não desenha o próprio mundo** (`LevelRenderer.render` cancelado): overlay = só mão + HUD + telas,
   alfa pré-multiplicado.
8. **Olhar integrado no lado do jogo** (fórmula de sensibilidade do MC) para câmera sem latência; posição vem
   dos ticks de 20 Hz do MC com QPC, interpolada no relógio do jogo.
9. **Colisão injetada em `BlockCollisions.computeNext`** como `VoxelShape` extra (não são blocos: dá para
   colocar blocos encostados), regiões 8³ com máscaras 1/8; jogador local usa triângulos exatos após
   `Entity.collide`. Paredes íngremes viram colunas para o step-up do MC recusar. `epoch` por mundo.
10. **Teleporte com ack + "segurar até o chão chegar"**.
11. **Atores do jogo = `LivingEntity` invisível** no MC; dano vanilla vai para o jogo; dano do jogo volta como
    `player.hurt` (armadura/escudo/totem do MC funcionam).
12. **Lançamento via Prism portátil** com `-D...startHidden=true`, mutex nomeado para não abrir dois, start pelo
    shell do Explorer, MC sai sozinho quando o PID do jogo morre.
13. **Stand-in Python do jogo** (`fake_skyrim.py`) para desenvolver o Fabric sem o jogo.

---

## 6. Layout proposto para `C:\codes\rea\mineWitcher3`

```
mineWitcher3/
  README.md
  LICENSE                       # a nossa
  THIRD-PARTY-NOTICES.md        # MIT do SkyCraft (texto completo) + deps
  install.ps1                   # existente
  docs/
    DESIGN.md                   # nosso design (derivado do SkyCraft, com W3/D3D12)
  research/                     # existente (este arquivo é o 05)
  protocol/
    mw3_protocol.h              # fonte da verdade (fork do skycraft_protocol.h, MIT header)
    README.md                   # tabela de offsets
    test/                       # static_asserts + teste que compara offsets com o Proto.java
  fabric/                       # mod Fabric (Gradle/Loom, MC 26.3, Loader 0.19.5, Java 25)
    build.gradle  gradle.properties  settings.gradle
    src/main/java/dev/minewitcher3/
      MineWitcher3.java         # ModInitializer (dimension mirror, entity type do proxy)
      link/     Proto.java  WitcherLink.java        # <- SkyLink.java (FFM)
      world/    W3Collision.java TriCollider.java W3Tri.java W3Clip.java W3Ray.java W3Water.java
      combat/   WitcherActorEntity.java  W3Combat.java
      mixin/    BlockCollisionsMixin ... (lista 3.1, sem os Skyrim-only)
    src/main/resources/ fabric.mod.json  minewitcher3.mixins.json
      data/minewitcher3/dimension_type/mirror.json
      data/minewitcher3/worldgen/world_preset/mirror.json
    src/client/java/dev/minewitcher3/client/
      W3Client.java  InputBridge.java  FrameExporter.java  MirrorWorld.java  ProxySync.java  W3Collider.java
      render/  WorldExporter.java  AvatarExporter.java  W3Atlas.java  BlockLightColors.java
      mixin/   WindowMixin  InputConstantsMixin  MinecraftMixin  LevelRendererMixin ...
    src/client/resources/ minewitcher3.client.mixins.json
    src/test/java/...           # TriColliderTest, SkyRayTest portados
  native/                       # DLL do W3 (C++23, CMake + vcpkg)
    CMakeLists.txt  CMakePresets.json  vcpkg.json   # spdlog, fmt, minhook/safetyhook, directx-headers, simpleini
    MineWitcher3.ini
    src/
      main.cpp                  # DllMain / carregador (asi loader ou proxy dll)
      Link.cpp/.h               # <- Link.cpp do SkyCraft (sem RE::), conversão W3<->MC
      Launcher.cpp              # <- Launcher.cpp (caminhos/instância próprios)
      d3d12/ Hooks.cpp  Overlay.cpp  WorldRender.cpp  shaders/*.hlsl
      game/  Player.cpp  Camera.cpp  Actors.cpp  Collision.cpp (PhysX)  Input.cpp
  mod/                          # existente: WitcherScript (modMineWitcher3)
    modMineWitcher3/content/scripts/local/...       # ponte script<->DLL (estado do jogo, ativar, menus)
  tools/
    scripts/                    # existente (RE do W3)
    fake_witcher.py             # <- fake_skyrim.py (stand-in do jogo)
    minecraft-bundle/Prism/instances/MineWitcher3/{instance.cfg, mmc-pack.json}
    package.ps1
  skin/                         # existente
  input/                        # existente
```

Regra prática: arquivos derivados do SkyCraft mantêm no topo
`// Derived from SkyCraft (https://github.com/chasmlol/skycraft), Copyright (c) 2026 chasmlol, MIT License.`

---

## 7. Resumo da classificação

- **(a) reusar como está** (com renomear pacote/nomes): quase todo `fabric/` — `SkyLink` (FFM), `Proto`,
  `SkyClient` (laço por frame, hide/focus, pacing, teleporte/hold, publicação de ticks/câmera),
  `InputBridge`, `FrameExporter`, `WorldExporter`, `AvatarExporter`, `SkyAtlas`, `SkyCollision`,
  `TriCollider`/`SkyCollider`/`SkyClip`/`SkyRay`/`SkyWater`, `MirrorWorld`, `ProxySync`, quase todos os
  Mixins; no nativo, o núcleo IPC de `Link.cpp`, a tabela DIK→SDL, o shader HLSL do overlay, o bundle Prism
  e o `fake_skyrim.py`.
- **(b) reusar com mudanças**: protocolo (nomes, escala 1.0, ids de mundo/ator do W3, materiais);
  `Launcher.cpp` (caminhos); `Overlay.cpp` (D3D11 → D3D12 ou D3D11On12); conceitos do `WorldRender.cpp`;
  algoritmo de voxelização do `Collision.cpp`; interpolação de ticks/câmera do `Game.cpp`; combate
  (`SkyrimActorEntity`, escala de dano); escavação.
- **(c) só Skyrim**: CommonLibSSE-NG, Address Library, tudo que toca `RE::*` (PlayerCharacter, PlayerCamera,
  bhkWorld/Havok, BSInputDeviceManager, MenuControls/PlayerControls, HUD Scaleform, DALC/ImageSpace,
  shadow cascades do Skyrim), `SmithingMixin` e skill-use, ragdoll Havok, hooks por ID de AE.

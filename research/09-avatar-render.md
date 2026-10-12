# 09 — Avatar do MC em terceira pessoa (F5) no lugar do Geralt

Problema: no F5 (modos 1 e 2 do MC) aparece o Geralt e não o skin do jogador.
Análise só leitura. SkyCraft num clone local (`git pull`: already up to date). As classes do MC 26.3 foram conferidas com `javap` no jar deobf (`~/.gradle/caches/fabric-loom/minecraftMaven/.../minecraft-clientonly-deobf-26.3.jar`).

Legenda: **[O]** observado · **[I]** inferido · **[?]** a confirmar.

---

## 1. Como o SkyCraft faz

### 1.1 Lado MC: `AvatarExporter` (o nosso é uma cópia 1:1)
- **[O]** O `AvatarExporter` implementa `SubmitNodeCollector` (`skycraft/.../render/AvatarExporter.java:81`). Os renderers do MC "submetem" para ele em vez de mandar para a GPU, e ele grava os vértices já posados e animados.
- **[O]** É chamado **todo frame**: `WorldExporter.frame()` chama `AvatarExporter.frame()` (o nosso: `fabric/.../render/WorldExporter.java:119`, a partir de `W3Client.afterRender()` `W3Client.java:395`). O `frame()` (`AvatarExporter.java:129-137`) faz três capturas:
  - `exportAvatar` (`:141-162`): só quando `camera.isDetached()`. Faz `dispatcher.extractEntity(player, partialTick)` e depois `dispatcher.submit(state, cameraState, 0,0,0, new PoseStack(), this)`. As posições saem **relativas aos pés** do jogador. O resultado vai como `REN_AVATAR` com `tryWriteRender` (`:161`, `:418`).
  - `exportScene` (`:280-328`): as outras entidades, block entities e partículas (`REN_SCENE`).
  - `exportRagdoll` (`:169-213`): uma vez por segundo, o corpo em pose neutra, separado em 6 partes (`REN_RAGDOLL`).
- **[O]** Não há skinning. A geometria chega como **triângulos já posados em CPU**, num `RenVertex` de 32 B (`protocol/mw3_protocol.h:529-538`): pos3f, uv2f, cor RGBA8, light (block/sky 0-15) e flags (cutout/translucent/normal da face). Cada quad vira 2 triângulos em `writeQuad` (`AvatarExporter.java:500-509` no nosso). Os lotes vêm como `RenBatch{texture, first, count, flags}` (`mw3_protocol.h:510-516`), e o header é `RenAvatar{batchCount, vertexCount}` (`:504-508`).
- **[O]** Texturas: na primeira vez que uma textura aparece, `textureId()` lê os pixels (do resource pack, ou da `DynamicTexture` no caso do skin baixado) e manda `REN_TEXTURE` (`RenTexture{id,w,h}` + RGBA8) com `writeRender` **bloqueante** (nosso `AvatarExporter.java:640`). Isso é o log "sent texture minecraft:skins/e8fb... (id 2)" (`research/field/mc-20261012-0910.log:116`). O id 1 foi o `zuri.png` padrão (`:113`), antes de o skin terminar de baixar. Também foram armadura iron e capa (`:117-119`). Os UVs de bloco e item são remapeados para o atlas combinado (`Batch.add`).

### 1.2 Lado Skyrim: `skse/src/WorldRender.cpp`
- **[O]** Consumo do ring: `Draw()` → `DrainMessages()` (`WorldRender.cpp:3206`) → `case kRenAvatar: OnMesh(..., avatar, ...)` (`:1109-1110`). `OnMesh` (`:881-958`) copia os vértices para um VB dinâmico D3D11 (`MAP_WRITE_DISCARD`) e guarda os batches. As texturas viram `entityTextures[id]` (SRV).
- **[O]** Desenho: `DrawEntities()` (`:1475-1490`) chama `DrawMesh(avatar, feet = State().feetX/Y/Z, ...)`, com os pés interpolados que a câmera do Skyrim segue (`Game.cpp:833`). Assim o corpo não "descola" da câmera. `DrawMesh` (`:1397-1420`) converte a origem MC→Skyrim, põe o offset relativo à câmera num cbuffer e desenha cada batch com a sua textura. São dois passes: sólido e depois blended.
- **[O]** Shader: o VS converte eixos e escala `float3(x,-z,y)*70` (`:122`) e multiplica por `viewProj`. A `viewProj` é **a matriz da própria câmera do Skyrim** (`worldToCam` re-baseada na posição da câmera, `:2584-2592`), e não é reconstruída a partir do MC. A iluminação usa o sol, a ambient cube, as sombras (de blocos e do Skyrim), as luzes pontuais e o fog do Skyrim (cbuffer `:37-67`, `Lighting()` `:200+`).
- **[O]** Ponto de hook: um `write_call` logo **depois de `Main::RenderWorld`** (`REL::ID(36559)+0x85E`, `:3291-3294`). `RenderWorldHook::thunk` chama o original e depois `DrawInFrame()` (`:3023-3031`). Nesse momento a cena HDR está completa, e tonemap, bloom, DoF e grading ainda não rodaram.
- **[O]** Depth: `DrawInFrame` (`:2895-3020`) pega `kMAIN` (scene HDR), a depth `kMAIN` (DSV + SRV) e os motion vectors. Exige formato HDR e tamanhos iguais (`:2965-2970`). Copia o depth (`CopyResource(depthCopyTex, depthTex)`), desenha com o DSV do Skyrim (depth test e write de verdade, com estados normal ou reverse, `:2797/2822`) e também escreve nas cópias de depth do Skyrim (`:2994-3016`), MVs e normais (para o TAA). Salva e restaura todo o estado D3D11 (`StateBackup`).
- **[O]** Fallback: sem hook in-frame, `Draw()` no Present desenha por cima do back buffer com um depth próprio (`:3245-3270`).
- **[O]** Como o corpo do Skyrim some no F5: o Skyrim fica **sempre em primeira pessoa** (`camera->ForceFirstPerson()`, `Game.cpp:938-939`), com a câmera movida para onde a do MC está e com o FOV do MC (`ApplyMcFov`, `:941`). Assim o corpo de 3ª pessoa nunca é desenhado. As malhas de 1ª pessoa (braços, armas) são escondidas com `SetAppCulled(true)` enquanto o MC dirige (`HideFirstPersonMeshes`, `Game.cpp:125-148`, chamada em `:879` e desfeita em `:961`). Na morte, o corpo do Skyrim é escondido do mesmo jeito e o ragdoll MC é pendurado nos ossos (`WorldRender.cpp:3078-3140`).

### 1.3 O que temos hoje
- **[O]** O MC exporta o avatar todo frame, mas **ninguém lê o ring de render**: `Link::DrainRender` existe (`native/src/link.cpp:356`) e não tem nenhum chamador em `native/src`.
- **[O]** O W3 só esconde o Geralt em 1ª pessoa: `SetPlayerHidden( mcMode == 0 )` (`mod/.../mw3_camera.ws:178`). Em `SetPlayerHidden` (`:285-307`) isso vira `SetHideInGame` no player e nos itens montados.
- **[O]** O `MW3_McFov()` está importado (`mw3_native.ws:20`, `natives.cpp:109`), mas **não é usado** em nenhum `.ws`. A câmera do W3 não usa o FOV do MC.
- **[O]** O overlay é o `mainRenderTarget` inteiro do MC (hand + HUD), RGBA8 premultiplicado. O `FrameExporter.capture` faz readback assíncrono e ele é composto no Present (`native/src/overlay.cpp`). O `LevelRendererMixin` cancela `LevelRenderer.render` inteiro quando há link (`LevelRendererMixin.java:16-25`).
- **[O] Problema extra (afeta os dois planos):** como o ring de 64 MB nunca é drenado, depois que ele enche os `writeRender` bloqueantes (`REN_TEXTURE`, `REN_SECTION`, `REN_LIGHTS`, `REN_SOLIDS`, `REN_DUG`, `REN_ATLAS`) tentam 500×2 ms, ou seja, **até ~1 s de travada no render thread do MC por mensagem** (`WitcherLink.java:558-584`). Os `tryWriteRender` falham em silêncio. Até existir um consumidor: drenar e descartar no nativo (por ex. `DrainRender` com callback vazio a cada Present), ou não chamar `WorldExporter.frame` sem consumidor.

---

## 2. Plano interino (rápido): o MC desenha o próprio jogador no overlay

Ideia: a câmera do W3 já é a câmera do MC (olho, yaw/pitch, distância do F5: `mw3_camera.ws:187-203`, `W3Client.java:368-376`). Se o MC desenhar **só a entidade do jogador local** no `mainRenderTarget`, ela sai no overlay e cai no lugar certo da tela. Fica faltando oclusão pelo mundo do W3 e luz do W3, o que é aceitável. O Geralt é escondido sempre que o MC estiver no controle.

### 2.1 Fabric: nova classe `dev.minewitcher3.client.render.PlayerOverlay`
O molde é o próprio MC: `GameRenderer.renderItemInHand` monta um `SubmitNodeStorage` → `featureRenderDispatcher.prepareFrame(storage)` → `createRenderPass(main color, depth)` → `RenderSystem.bindDefaultUniforms(pass)` → `FeatureRenderDispatcher.renderAllFeatures(pass, frame)` (bytecode de `GameRenderer.renderItemInHand`). O main pass do `LevelRenderer` faz o mesmo, mais `RenderSystem.setShaderFog(fog)` e `gameRenderer.lighting().setupFor(Lighting.Entry.LEVEL)` (`lambda$addMainPass$0`).

APIs confirmadas no jar 26.3:
- `EntityRenderDispatcher.prepare(Camera, Entity)`, `extractEntity(E, float)` e `submit(S, CameraRenderState, double, double, double, PoseStack, SubmitNodeCollector)`
- `SubmitNodeStorage()` (construtor público) e `setUseImprovedTransparency(boolean)`
- `FeatureRenderDispatcher.prepareFrame(SubmitNodeStorage)` → `PreparedFrame` (é `AutoCloseable`), e `static renderAllFeatures(RenderPass, PreparedFrame)`
- `GameRenderer.featureRenderDispatcher()`, `mainRenderTarget()`, `lighting()`
- `RenderSystem.getModelViewStack()`, `setShaderFog(GpuBufferSlice)`, `setProjectionMatrix(GpuBufferSlice, ProjectionType)`, `backupProjectionMatrix()`/`restoreProjectionMatrix()` e `bindDefaultUniforms(RenderPass)`
- `ProjectionMatrixBuffer(String)` com `getBuffer(Matrix4f)`
- `CameraRenderState.pos`, `projectionMatrix` e `viewRotationMatrix`
- `EntityRenderState.x/y/z`, `shadowPieces`, `shadowRadius` e `nameTag`

Esboço (pseudocódigo; nada foi aplicado):
```java
public static void render(Minecraft mc, CameraRenderState cam, GpuBufferSlice fog) {
    var player = mc.player; Camera camera = mc.gameRenderer.mainCamera();
    if (player == null || !camera.isDetached() || player.isSpectator()) return;
    var d = mc.getEntityRenderDispatcher();
    d.prepare(camera, mc.crosshairPickEntity);
    EntityRenderState s = d.extractEntity(player, cam.cameraEntityPartialTicks /* ou partialTick */);
    s.shadowPieces.clear(); s.shadowRadius = 0; s.nameTag = null;   // sombra sobre blocos MC: fora
    STORAGE /* new SubmitNodeStorage(), reutilizado */ ;
    d.submit(s, cam, s.x - cam.pos.x, s.y - cam.pos.y, s.z - cam.pos.z, new PoseStack(), STORAGE);
    Matrix4fStack mv = RenderSystem.getModelViewStack();
    mv.pushMatrix(); mv.mul(cam.viewRotationMatrix);           // igual ao LevelRenderer.render
    RenderSystem.backupProjectionMatrix();                      // sem o bob (o W3 não aplica bob)
    RenderSystem.setProjectionMatrix(PROJ.getBuffer(cam.projectionMatrix), ProjectionType.PERSPECTIVE);
    RenderSystem.setShaderFog(fog);
    mc.gameRenderer.lighting().setupFor(Lighting.Entry.LEVEL);
    RenderTarget t = mc.gameRenderer.mainRenderTarget();
    try (var frame = mc.gameRenderer.featureRenderDispatcher().prepareFrame(STORAGE);
         RenderPass pass = RenderSystem.getDevice().createCommandEncoder().createRenderPass(
             () -> "MineWitcher3 player", t.getColorTextureView(), Optional.empty(),
             t.getDepthTextureView(), OptionalDouble.empty())) {
        RenderSystem.bindDefaultUniforms(pass);
        FeatureRenderDispatcher.renderAllFeatures(pass, frame);
    } finally { RenderSystem.restoreProjectionMatrix(); mv.popMatrix(); }
}
```
Notas:
- O `GameRenderer.render` já limpa a cor e o depth do `mainRenderTarget` no começo do frame (`clearColorAndDepthTextures`). O render pass acima não limpa nada (`Optional.empty()`), e o depth do MC só resolve a auto-oclusão do modelo.
- Em F5 o MC não desenha a mão (`renderItemInHand` testa `cameraType.isFirstPerson()`), então o overlay fica com jogador + HUD.
- `STORAGE` com `setUseImprovedTransparency(false)`: o `renderAllFeatures` vai pelo caminho de transparência clássico, igual ao da mão.

### 2.2 Fabric: `LevelRendererMixin.mw3$skipLevel`
Hoje: `@Inject(method="render(...)V", at=HEAD, cancellable=true)`, que só cancela. Mudança: receber os argumentos do método e desenhar o jogador antes de cancelar.
```java
private void mw3$skipLevel(GraphicsResourceAllocator alloc, boolean b0, CameraRenderState cam,
                           GpuBufferSlice fog, Vector4f fogColor, boolean b1, boolean b2, CallbackInfo ci) {
    if (W3Client.linked()) { PlayerOverlay.render(Minecraft.getInstance(), cam, fog); ci.cancel(); }
}
```
- É o ponto certo porque o `GameRenderer.renderLevel` já setou a projeção do nível e o fog. O `cam` é o `levelRenderState.cameraRenderState` do frame (o `renderLevel` passa esse argumento para `LevelRenderer.render`, e o próprio `render` usa `cam.viewRotationMatrix`).
- O `FrameExporter.capture` roda depois, em `afterRender` (`W3Client.java:401`), e captura o modelo junto. Não muda nada nele.
- Opcional: não chamar `AvatarExporter.frame` (ou pular só `exportAvatar`) enquanto não houver consumidor. É CPU gasto à toa, e ainda enche o ring (ver 1.3).

### 2.3 Script do W3 (`mw3_camera.ws`)
1. Esconder o Geralt em **todos** os modos com link:
   - `:178` `SetPlayerHidden( mcMode == 0 )` → `SetPlayerHidden( true )`. Pode chamar só uma vez, quando `linkedActive` liga (`:167-172`); o `Refresh()` (`:310-316`) já reaplica a cada 1 s nas espadas sacadas.
   - O caminho de desconexão (`:117-121`) já devolve o estado do modo local.
2. **Casar o FOV** (sem isso o boneco desalinha): o MC manda o FOV **vertical** em `mc.fov = camera.getFov()` (`W3Client.java:371`), e o W3 hoje ignora esse valor. No `Apply()` com link, fazer `theGame.GetGameCamera().SetFov( F( MW3_McFov() ) )` (`research/03-camera-input.md:91`). **[?]** Não se sabe se o fov do W3 é horizontal ou vertical. Se for horizontal: `h = 2·atan(tan(v/2)·aspect)`. Calibrar com uma screenshot (contorno do boneco contra um poste próximo).
3. A aspect ratio do MC deve ser a do back buffer do W3. Hoje o overlay é 1920×1080 (`mc log:114`) e é esticado para o back buffer.

### 2.4 O que se perde / riscos do interino
- **Oclusão:** grama, árvores, paredes e NPCs do W3 entre a câmera e o jogador **não** tampam o boneco, porque o overlay fica por cima de tudo, inclusive da água. O zoom do F5 já colide contra a geometria do W3 (`CameraZoomMixin` + `W3Clip`), o que reduz o caso.
- **Luz:** o boneco usa o lightmap e o `Lighting.LEVEL` do MC (dia/noite e luz do bloco do mundo MC). Pode ficar claro demais à noite no W3. Não tem a sombra do W3 nem uma sombra própria no chão do W3. Com o Geralt escondido, `SetHideInGame` provavelmente tira a sombra dele também **[?]**.
- **Latência:** o readback é assíncrono (~1 frame) e o W3 compõe no Present seguinte, então o boneco fica 1–2 frames atrasado em relação ao mundo. Isso aparece como "arrasto" lateral ao girar a câmera. O boneco fica perto do centro, então o efeito é pequeno.
- **Alinhamento:**
  - O FOV (2.3.2) e o bob: o MC aplica o bob também no F5, e o W3 não. Usar `cam.projectionMatrix` (sem bob) resolve.
  - A interpolação dos pés pode divergir: o W3 mostra a câmera a partir do snapshot do MC, então o alinhamento sai praticamente exato.
  - A distância da câmera: se o `pivotDistanceController` do W3 colidir ainda mais que o MC, o boneco fica maior ou menor. Conferir.
- **Premultiplied alpha:** a camada externa translúcida do skin e a capa podem sair com a borda escura ou clara. É o mesmo pipeline do HUD, então o risco é baixo.
- **Ring cheio:** ver 1.3. Resolver junto.

---

## 3. Plano completo: avatar dentro do frame D3D12 do W3

### 3.1 Componentes (nativo, novo `native/src/world_render.cpp`, port parcial do `WorldRender.cpp`)
1. **Consumo do ring:** chamar `Link::DrainRender` todo frame, no Present ou no início do hook de render, e tratar:
   - `kRenTexture`: criar `ID3D12Resource` RGBA8 DEFAULT, subir via upload heap e criar um SRV num heap shader-visible **próprio** (`id → índice`).
   - `kRenAvatar`: copiar os vértices para um upload buffer em anel (3 frames × ~1 MB; o avatar tem poucos milhares de vértices). Sem index buffer, porque vem como lista de triângulos. Guardar os batches.
   - `kRenClearAll`: soltar tudo.
   - O resto (seções, scene...) pode ser descartado até existir o mundo de blocos.
2. **Pipeline:** um root signature com CBV (viewProj, offset dos pés, exposição) e uma SRV table (textura) + sampler point. Dois PSOs, sólido (alpha-test por `flags&1`) e translúcido (blend). VS com a mesma conversão de eixos da nossa ponte: `w = (mc.x, -mc.z, mc.y)` em metros (`native/src/link.h:71-76`). Sem o `*70` do Skyrim.
3. **Matrizes:**
   - O ideal é ler a viewProj real do W3 do frame. O `CCameraDirector` tem pos/rot/fov em `+0x70/+0x80/+0xA0` (`research/03-camera-input.md:31,115`). A view matrix final ainda não foi localizada **[?]**.
   - Interino: reconstruir a partir do estado do MC. Olho = `mc.eye`; câmera = olho − forward·`cameraDistance` (no modo 2, rotação `Flip`); yaw e pitch pelas funções de `link.cpp:100-115`; FOV e aspect iguais aos aplicados ao W3; near igual ao do W3 **[?]**.
   - Posição dos pés: usar a mesma interpolação que o W3 usa para o snapshot (`session::CurrentSnapshot()`), não o pacote cru.
4. **Ponto de hook:** detour inline em `witcher3.exe+0x1C091C0` (seletor AA/upscaler, `research/01-render.md:85-110`). O único caller é `0x1c0ef30`, dentro da cadeia de post `0x1c0e420`.
   - **Depois** do original: desenhar na **saída** do AA/upscale. Ela está em resolução de display e sem jitter, então não precisa jitterizar a projeção nem gerar MVs. Ainda é HDR linear e antes do tonemap e do HUD, então o tonemap e o grading do W3 se aplicam.
   - **Antes** do original: desenhar na entrada, para o TAA/DLSS suavizar. Isso exige o jitter na projeção e MVs, senão o TAA borra ou faz ghosting. Fica para depois.
   - Não temos lib de detour em `third_party/` (só UltimateASILoader e WitcherNativeBridge). Seria preciso adicionar MinHook, ou fazer um patch manual no prólogo com checagem de bytes e do hash do exe (versão fixada em `01-render.md`).
5. **Recursos do engine:**
   - Cor de saída e depth (`RTN_GBuffer3Depth`): pelos argumentos de `0x1C091C0` (`rcx` = contexto; `[rcx+0x74]` = modo AA) ou pela tabela do GpuApi `[ctx+0x7e008 + idx*0x140]` → `ID3D12Resource*` (mesmo caminho do wrapper `0x1ed8c20`). **Precisa de Ghidra/RenderDoc**, porque os layouts estão em aberto (`01-render.md:182`).
   - Atalho com upscaler ativo: hook de IAT em `slSetTagForFrame` (`0x298dd20`), `ffxDispatch` (`0x298da10`) ou `xessD3D12Execute` (`0x298db78`). Eles entregam Depth e Scaling Input/Output Color como `ID3D12Resource*` mais a command list (`01-render.md:120-127`). Dá para validar o resto por esse caminho antes do reverse dos structs.
6. **Depth test:** o depth é reverse-Z **[I]** (`FFX_FSR2_OPTION_INVERTED_DEPTH`, ReShade). Está em resolução de render, então não dá para usar como DSV na saída em resolução de display. Fazer o teste no pixel shader: amostrar o depth do W3 com UV escalado (`renderSize/displaySize`), linearizar e descartar se a cena estiver mais perto (o mesmo `SceneViewDepth` do SkyCraft, `WorldRender.cpp:135-139`). Para a auto-oclusão do modelo, usar um DSV próprio em resolução de display (limpo a 0 com reverse-Z ou 1 sem).
7. **Command list (o ponto mais delicado):** os comandos precisam rodar **entre** a cena e o post, no mesmo stream. Uma command list nossa submetida à parte executaria fora de ordem. As opções:
   - (a) Gravar na command list corrente do engine (`[dev+0x2100a8]` **[I]**), com barriers próprias (RT/SRV ↔ estado que o GpuApi espera) e **restauração de estado**. O GpuApi provavelmente cacheia PSO, root signature, heaps e RTs, e não vai re-setar o que achar que já está bound. É preciso rastrear `SetDescriptorHeaps`, `Set*RootSignature`, `SetPipelineState`, `OMSetRenderTargets`, `RS*` e `IA*` (hook na vtable da command list nativa, estilo ReShade) e reaplicar depois, ou achar e chamar a função de "invalidate state cache" do GpuApi.
   - (b) Usar o ReShade 6 com um addon (D3D12, faz a própria state tracking e a detecção de depth genérica). O desenho seria feito num evento antes do post (`bind_render_targets`/`dispatch` de uma passagem identificada). É mais trabalho de integração com o Streamline, mas não precisa reinventar a restauração de estado.
8. **Luz/cor:** a saída é HDR linear antes do tonemap. Converter os texels sRGB→linear e multiplicar por uma exposição calibrada, ou ler o sol e o ambiente do W3 por natives de environment (fase 2). A cópia do SkyCraft (sol, ambient cube, sombras) só vale depois de achar esses buffers no W3.
9. **Esconder o Geralt:** o mesmo da seção 2.3.1 (`SetHideInGame` em todos os modos com link). O SkyCraft resolve forçando 1ª pessoa no Skyrim; no W3, a câmera custom de terceira pessoa com o Geralt escondido é o equivalente.
10. **Frame generation (DLSS-G/FSR-FG):** desenhar antes do HUD faz o avatar entrar no `HUDLessColor`, mas sem MVs ele interpola mal. No MVP: FG desligado.

### 3.2 Riscos do plano completo
- O RVA `0x1C091C0` e os offsets do GpuApi são específicos desta build (5.0.0.1048522). Qualquer patch quebra o hook, então é preciso checar o hash e ter um fallback (Present/overlay).
- O layout dos argumentos e de `CRenderSurfaces` é desconhecido (precisa de Ghidra/RenderDoc). Com o RenderDoc ligado o DLSS cai (`01-render.md:2.4`).
- Restaurar o estado da command list do engine (7a): se errar, vêm artefatos ou TDR (device removed).
- Resource states e barriers: transicionar a RT ou o depth para um estado que o engine não espera leva a erro de validação ou corrupção.
- Depth em resolução diferente + jitter: um halo de ~1 px nas bordas da oclusão. Aceitável.
- Casamento da câmera (FOV horizontal/vertical, near, aspect) reconstruída do MC versus a matriz real do W3: o avatar "flutua" alguns cm se não bater. Ler a matriz real resolve.
- Exposição HDR: o boneco sai estourado ou escuro sem calibração.
- Proxies do Streamline: a command list ou a fila podem ser proxies. Usar só o objeto que o jogo passa e nunca escrever vtable nativa compartilhada com o overlay da Steam (a lição de `overlay.cpp:12-22`).

---

## 4. Ordem sugerida
1. Drenar e descartar o ring no nativo (ou cortar o export) para eliminar as travadas de até 1 s.
2. Interino (seção 2): `PlayerOverlay` + mixin + `SetPlayerHidden(true)` + `SetFov`. Validar o alinhamento com uma screenshot em F5 parado e girando.
3. Plano completo: começar pelo hook de IAT `slSetTagForFrame` (com DLSS ligado) para obter o depth e a cor reais e testar o draw. Depois migrar para o detour em `0x1C091C0`, para cobrir TAA/FSR/XeSS.

# 03 — Câmera e interceptação de input (witcher3.exe 5.0.0.1048522, DX12)

Escopo: (1) controlar a câmera do W3 a partir do Minecraft (1ª pessoa, F5 atrás, F5 frente), (2) mandar teclado/mouse para o Minecraft sem perder Esc/menus do W3, (3) mover o Geralt "fantoche" sem brigar com a locomoção.

Legenda: **[O]** observado (arquivo:linha ou bytes do exe), **[I]** inferido, **[?]** desconhecido / precisa de runtime.
Scripts vanilla: `...\The Witcher 3\content\content0\scripts\` (abreviado `scripts\`). RVAs relativos a ImageBase.
Nada foi executado, injetado ou alterado. Análise estática pura.

Ferramentas novas em `C:\codes\rea\mineWitcher3\tools\scripts\camera\` (rodar com `tools\.venv\Scripts\python.exe -I`):

| script | o que faz |
|---|---|
| `exe_input_camera_scan.py` | imports de input/janela, RTTI `.?AV*Camera*`/input, strings de câmera |
| `iat_callsites.py` | acha `call [rip+IAT]` para APIs (ex.: `RegisterRawInputDevices`) |
| `disasm_func.py` | desmonta a função (via `.pdata`) que contém um RVA |
| `rtti_vtables.py` | TypeDescriptor → CompleteObjectLocator → vtable |
| `script_native.py` | acha o registro de uma native de script (`lea rax,fn` + `lea rdx,"Nome"`) e desmonta a função |
| `find_strings.py`, `str_xrefs.py`, `src_paths.py` | busca de strings e xrefs |
| `bundle_paths.py` | lista caminhos de recursos nos TOCs dos `.bundle` (ex.: templates de câmera) |

---

## 0. Resumo executivo

1. **Script consegue controlar a câmera por frame, com posição e rotação absolutas.** [O, mas sem teste em runtime] Há três caminhos vanilla. Recomendo o **A2 (câmera do photomode)** como principal e o **A1 (moveData no `OnGameCameraPostTick`)** como alternativa:
   - **A2 — `theGame.GetPhotomodeCamera()`** (um `CCustomCamera`): `Activate()`, depois por frame `SetCameraWorldPosition` + `SetCameraWorldRotation` + `UpdateWithoutInput(true)` + `SetFov` + `SetNoclip`. É exatamente o que o photomode vanilla faz por tick (`photomodeManager.ws:108-111`, `photomodeMenu.ws:3613-3615`, `OnTick` em `photomodeMenu.ws:4050`).
   - **A1 — câmera de gameplay** (`theGame.GetGameCamera()`): sobrescrever `SCameraMovementData` (pivot/rotação/distância/offset) em `CR4Player.OnGameCameraPostTick` via `@wrapMethod`. Os estados do player já fazem isso (`r4Player.ws:4237-4300`, `meditationBase.ws:35-60`).
   - **A3 — `CStaticCamera` spawnada** de um template vanilla (`poster.ws:98-103`), movida com `TeleportWithRotation`. É o padrão do mod de 1ª pessoa do SkacikPL.
2. **Os valores externos chegam por natives registradas pela DLL** (WitcherNativeBridge, ver `02-script-natives.md`). O script chama `MW3_Cam*()` a cada frame. Não existe canal vanilla DLL → script que preste para isso.
3. **Esconder o Geralt:** `thePlayer.SetHideInGame(true)` (precedente vanilla em `poster.ws:96/128`). Complementos: `CDrawableComponent.SetVisible` / `SetCastingShadows` (`engine\components.ws:473,479`) e `CItemEntity.SetHideInGame` nas armas montadas.
4. **Câmera nativa** (plano B, se o script falhar): `CCameraDirector` guarda o resultado em `+0x70` (posição), `+0x80` (EulerAngles Roll/Pitch/Yaw) e `+0xA0` (fov) [O, nos getters nativos]. `CCustomCamera` guarda a posição pedida em `+0x1D0` e a rotação em `+0x1F0` [O]. Ainda não está localizado o ponto de escrita da view matrix [?].
5. **Input:** o exe usa **Raw Input** para teclado e mouse, com `dwFlags=0` (sem `RIDEV_NOLEGACY` e sem `INPUTSINK`) [O]. Também importa DirectInput8, XInput9_1_0, HID e GameInput (dinâmico). A WndProc é a da classe `W2ViewportClass` (`0x1C67740` → handler `0x1C679C0`, que chama `GetRawInputData` em `0x1C67B04`) [O]. **Recomendação:** subclass da WndProc (`SetWindowLongPtrW(GWLP_WNDPROC)`) que descarta `WM_INPUT`/`WM_KEY*`/`WM_MOUSE*`/`WM_CHAR` no modo gameplay e deixa passar Esc (e o que mais for "de menu"). Do lado do script, usar um **contexto de input próprio** (como o `EMPTY_CONTEXT` vanilla, mas com Esc) para o Geralt não reagir a WASD/mouse mesmo que algo vaze.
6. **Mover o fantoche:** o caminho melhor [I] é injetar o "stick virtual" que a locomoção lê (`GI_AxisLeftX/Y`) com `@wrapMethod` nos pontos de leitura (`player.ws:588-599`, `locomotionDirectController.ws:643-657`, `r4Player.ws:1435`). Assim o Geralt anda com animação, colisão e navmesh. Para correção fina use `CMovementAdjustor`. `TeleportWithRotation` por frame fica só para snap ou correção de drift grande.

---

## 1. Câmera em script

### 1.1 Classes e APIs relevantes [O]

| Classe / API | Onde | Notas |
|---|---|---|
| `CCamera extends CEntity` | `engine\camera.ws:36-...` | `SetActive(blendTime)`, `SetFov`, `GetFov`, `SetZoom`, `Rotate`, `Follow`, `LookAt*`, `GetCameraMatrixWorldSpace`. Base de `CStaticCamera`. |
| `CStaticCamera extends CCamera` | `engine\staticCamera.ws:20-32` | `Run()`, `Stop()`, `IsRunning()`, `RunAndWait()`, vars `activationDuration`, `deactivationDuration`, `timeout`, `fadeStartDuration`, `fadeEndDuration`. |
| `CCameraComponent` | `engine\CCameraComponent.ws:11-18` | `import var fov`, `nearPlane`, `farPlane`, `customClippingPlanes`, `aspect`. Tem os vars, mas nenhuma função vanilla para mudar a near plane. |
| `CCustomCamera extends CEntity` | `game\cameras\combat\customCamera.ws:66-152` | `Activate(blend, resetPosition)` (l.71), `Change/BlendToPivot*Controller(name)` (l.77-81), `EnableManualControl` (l.97), `SetCollisionOffset` (l.106), **`SetNoclip`** (l.108), **`GetCameraWorldPosition/Rotation/Offset`** e **`SetCameraWorldPosition/Rotation`** (l.110-114), **`UpdateWithoutInput(force)`** (l.115), `SetFov(val)` = `fov = val` (l.124-127, `import var fov` na l.69), `SetAllowAutoRotation` (l.151). |
| `CCameraDirector` / `CR4CameraDirector` | `engine\cameraDirector.ws`, `game\cameras\r4CameraDirector.ws:6` | Só leitura: `GetCameraPosition/Rotation/Forward/Right/Up/Heading/Direction`, `GetFov`, `GetTopmostCamera()`, `ViewCoordsToWorldVector`, `WorldVectorToViewCoords`. `theCamera` é o director. |
| Controllers de pivot | `game\cameras\combat\customCameraControllers.ws:6-147` | Nativos: `ICustomCameraPivotPosition/Rotation/DistanceController` com `Update(out ...)`, `SetDesiredPosition/Heading/Pitch/Distance`, `offsetZ`, `minPitch/maxPitch`, `minDist/maxDist`. Scriptáveis: `ICustomCameraScriptedPivot{Position,Rotation,Distance}Controller` com `ControllerUpdate(out current..., out velocity, dt)` (l.62-108), e `ICustomCameraScriptedPositionController.ControllerUpdate(out moveData, dt)` (l.128-142). |
| `SCameraMovementData` | `customCameraControllers.ws:113-126` | `pivotPosition/Rotation/DistanceController`, `pivotPositionValue/Velocity`, `pivotRotationValue/Velocity`, `pivotDistanceValue/Velocity`, `cameraLocalSpaceOffset(Vel)`. |
| Eventos por frame da câmera de gameplay | `r4Player.ws:4237` (`OnGameCameraTick`), `r4Player.ws:4375` (`OnGameCameraPostTick`) | São chamados pelo engine (strings `OnGameCameraTick` @0x31FD5F8 e `OnGameCameraPostTick` @0x31FD448 no exe). Os estados sobrescrevem e quase todos chamam `parent/super` (ex.: `extendedMovable.ws:142`, `exploration.ws:766`, `combat.ws:804`). |
| Photomode | `game\photomodeManager.ws:76-146`, `r4Game.ws:303` (`GetPhotomodeCamera() : CCustomCamera`), `engine\game.ws:313-316` (`Get/SetPhotomodeEnabled`, `SetPhotomodeCameraCanMove`) | Ver 1.3. |
| Free camera de debug | `engine\game.ws:158-167` (`IsFreeCameraEnabled`, `EnableFreeCamera`, `GetFreeCameraPosition`, **`MoveFreeCamera(pos, rot)`**) | RTTI `CGameFreeCamera` (vtable 0x39C7628). A native `EnableFreeCamera` (0x2277BE0) testa `[CGame+0x128]` antes de chamar a vfunc `+0x2E0` [O]. Pode ficar desligada em build final [?]. Teclas em `bin\config\base\freecamera.ini`. |

Templates de câmera nos bundles [O, `bundle_paths.py`]: `startup.bundle`: `gameplay\camera\camera.w2ent`, `new_camera.w2ent`, `horse_camera.w2ent`, `photomode_camera.w2ent`, `scene_camera.w2ent`; `gameplay\poster\poster_camera.w2ent` (bob.bundle) e dezenas de `poster_camera_*.w2ent`. Os nomes de controllers aceitos por `ChangePivot*Controller(name)` estão no template da câmera. Só dá para trocar entre os que existem lá ('Default', 'Exploration', 'KeepRelativePositionController', 'Jump', 'Swimming', 'Debug_AR_Test_Cam_Rot' ...).

### 1.2 Caminho A1 — câmera de gameplay via `moveData` [O padrão vanilla / I para o nosso uso]

O engine chama `OnGameCameraTick(out moveData, dt)` e `OnGameCameraPostTick(out moveData, dt)` no player a cada update da `CCustomCamera` de gameplay. O script vanilla já monta a câmera inteira ali: troca controllers e chama `SetDesiredPosition/Pitch/Distance` e `DampVectorSpring(cameraLocalSpaceOffset...)` (`r4Player.ws:4271-4300`). Os pais finais são `CR4Player.OnGameCameraTick/PostTick`, então um `@wrapMethod(CR4Player)` neles vale para todos os estados que encadeiam (a grande maioria).

Esboço (1ª pessoa / F5 atrás / F5 frente):

```ws
@wrapMethod(CR4Player)
function OnGameCameraPostTick( out moveData : SCameraMovementData, dt : float ) : bool
{
    var r : bool = wrappedMethod( moveData, dt );
    if( MW3_CamActive() )              // native da DLL (WNB)
    {
        moveData.pivotPositionValue    = Vector( MW3_CamX(), MW3_CamY(), MW3_CamZ() ); // olho/cabeça
        moveData.pivotRotationValue.Yaw   = MW3_CamYaw();
        moveData.pivotRotationValue.Pitch = MW3_CamPitch();
        moveData.pivotRotationValue.Roll  = 0;
        moveData.pivotDistanceValue    = MW3_CamDist();     // 0 = 1a pessoa, ~4 = F5
        moveData.cameraLocalSpaceOffset = Vector(0,0,0);
        moveData.pivotPositionVelocity = Vector(0,0,0);
        moveData.pivotRotationVelocity = EulerAngles(0,0,0);
        moveData.pivotDistanceVelocity = 0;
        moveData.pivotPositionController.offsetZ = 0;
        // manter os controllers "desejando" o mesmo alvo para nao puxarem de volta:
        moveData.pivotPositionController.SetDesiredPosition( moveData.pivotPositionValue, 1000 );
        moveData.pivotRotationController.SetDesiredHeading( MW3_CamYaw(), 1000 );
        moveData.pivotRotationController.SetDesiredPitch( MW3_CamPitch(), 1000 );
        moveData.pivotDistanceController.SetDesiredDistance( MW3_CamDist(), 1000 );
        return true;
    }
    return r;
}
```

- F5 frente: `Yaw + 180` e `-Pitch` (a fórmula do Minecraft), com a mesma distância.
- FOV: `theGame.GetGameCamera().SetFov(f)` (só escreve o var `fov`, `customCamera.ws:124-127`).
- Os limites de pitch vêm de `ICustomCameraPivotRotationController.minPitch/maxPitch` (`customCameraControllers.ws:25-26`). Abra para ±89 (o vanilla altera `maxPitch` em `r4Player.ws:4285`).
- Riscos [?]: (a) não sei se a composição nativa recalcula depois do PostTick ou se os valores `*Value` gravados no script entram direto. Pelo uso vanilla (`isStartingFistFightMinigame` grava `pivotRotationValue` direto, `r4Player.ws:4250`), entram. (b) O collision controller (`CCustomCameraOutdoorCollisionController`) empurra a câmera; com distância 0 o efeito deve ser mínimo. (c) Estados que não encadeiam o PostTick (ex.: `unconscious.ws:57`) e cutscenes/diálogos, que usam outra câmera. (d) Não sei se `@wrapMethod` aceita `event` [I: a comunidade relata que sim, declarando como `function`]. Testar.

### 1.3 Caminho A2 — câmera do photomode, posição e rotação absolutas ★ recomendado [O padrão vanilla / I fora do photomode]

O photomode vanilla faz, por frame (no `OnTick` do menu, `photomodeMenu.ws:4050-4065` → `BlendCameraTransform`, l.3608-3627):

```ws
theGame.GetPhotomodeCamera().SetCameraWorldPosition(newCameraPosition);
theGame.GetPhotomodeCamera().SetCameraWorldRotation(newCameraRotation);
theGame.GetPhotomodeCamera().UpdateWithoutInput( true );
```

e, ao entrar, `Activate()` + as mesmas três chamadas (`photomodeManager.ws:108-111`). Ao sair: `m_lastActiveCam.Activate(0.0f, false)` (l.137).

Natives confirmadas no exe [O, `script_native.py`]:

| native | RVA | efeito observado |
|---|---|---|
| `CCustomCamera::SetCameraWorldPosition` | `0x1E0D0D0` | grava o Vector em `this+0x1D0` |
| `CCustomCamera::SetCameraWorldRotation` | `0x1E0D050` | grava a EulerAngles em `this+0x1F0..0x1F8` |
| `CCustomCamera::UpdateWithoutInput` | `0x1E0CFD0` | zera `this+0x344` e chama `0x1E0F420(this, k, force)` (update interno) |
| `CGame::SetPhotomodeCameraCanMove` | `0x2272A00` | grava o bool em `CGame+0xFD3B` (trava o PPC do photomode ao input) |
| `CCameraDirector::GetCameraPosition/Rotation/Heading/GetFov` | `0x2382890` / `0x2382850` / `0x2382740` / `0x23826D0` | leem `director+0x70` (Vector), `+0x80/+0x84/+0x88` (EulerAngles nativa = Roll, Pitch, Yaw; heading = `+0x88`) e `+0xA0` (fov) |

RTTI [O]: `CCustomCameraPhotomodePPC` (vtable 0x380D8E0), `CCustomCameraPhotomodePRC`, `CCustomCameraPhotomodePDC`, `CCustomCameraPhotomodePosititonController` [sic], `CCustomCameraPhotomodeCollisionController`. A câmera do photomode tem controllers próprios, sem dependência do estado do player.

Uso proposto (sem abrir o menu do photomode, sem `SetPhotomodeEnabled(true)`):

```ws
// entrar
cam = theGame.GetPhotomodeCamera();
theGame.SetPhotomodeCameraCanMove( false );   // o PPC nao le PMC_Move*
cam.Activate( 0.0, false );
cam.SetNoclip( true );                         // ou false para F5 colidir

// por frame (OnGameCameraPostTick wrap, ou timer 0s no player)
cam.SetCameraWorldPosition( p );               // p ja final (olho, ou olho - fwd*dist)
cam.SetCameraWorldRotation( r );
cam.SetFov( fov );
cam.UpdateWithoutInput( true );

// sair
theGame.GetGameCamera().Activate( 0.0, false );
```

Por que é o melhor caminho: a posição e a rotação são absolutas, não há interação com os estados do player (combate, nado, cavalo) e o noclip é controlável. Também é o mesmo mecanismo que o jogo usa para mexer a câmera de forma arbitrária.
Riscos [?]: (a) não sei se a câmera do photomode continua sendo tickada como topmost sem o photomode habilitado. `UpdateWithoutInput(true)` força o update, o que mitiga. (b) Lógica que usa `theGame.GetGameCamera()` (shake, presets) deixa de afetar a imagem. Para nós isso é desejável. (c) Talvez o `CCustomCameraPhotomodePPC` limite a distância ao player (o photomode tem raio máximo) [?]. (d) Na F5 com colisão, o W3 colide com a geometria do W3, não com os blocos do MC. Ver 1.6.

### 1.4 Caminho A3 — `CStaticCamera` spawnada [O vanilla + prior art]

- Vanilla: `poster.ws:94-103` faz `theInput.StoreContext('EMPTY_CONTEXT')`, `thePlayer.SetHideInGame(true)` e depois `spawnedCamera = (CStaticCamera)theGame.CreateEntity( camera, pos, rot ); spawnedCamera.activationDuration = ...; spawnedCamera.Run();`. `camera` é um `editable var : CEntityTemplate` (`poster.ws:14`). Os `poster_camera*.w2ent` são candidatos a template [I]. Confirme a classe abrindo o CR2W (`tools\scripts\collision\cr2w_props.py`).
- Prior art: **First Person Camera (SkacikPL, 2017)**, reaproveitado no repo `ExtremeDotneting/Witcher-3-FIRST-PERSON-CAMERA-REWORKED` (`mods/modFirstPersonCameraReworked/content/scripts/local/FirstPersonCamera.ws`). Faz `statemachine class FirstPersonCamera extends CStaticCamera` com `Run()`, `AddTimer('CorrectFPCamera', 0, true)` (correção por frame), `CreateAttachmentAtBoneWS(thePlayer, 'Root', ...)` e `BreakAttachment()`. A cabeça é escondida com `CAppearanceComponent.Include/ExcludeAppearanceTemplate(...)` (templates próprios em `dlc\fpcamfix`). FOV e near plane usam funções que **não existem no vanilla** (`setfov`, `setnearz`, `setclippingplanes`), vindas de templates/natives próprios ou ASI [I]. O mod também desregistra listeners do `thePlayer.GetInputHandler()` (`UnregisterListener(..., 'Focus')`, etc.), mesma técnica da seção 3.4.
- Desvantagens: precisa de template de entidade e o blend de `Run()`/`Stop()` tem fades. A câmera é uma entidade comum (teleporte por frame funciona, mas com latência de 1 frame [?]). Fica como plano C.

### 1.5 Head bone, 1ª pessoa e FOV [O]

- Cabeça: `thePlayer.GetHeadBoneIndex()` (`actor.ws:711`) e `MatrixGetTranslation(thePlayer.GetBoneWorldMatrixByIndex(idx))` (`entity.ws:182`). Uso vanilla em `useVehicle.ws:1690-1694`. Também existe `GetBoneIndex('head')` (`entity.ws:188`).
- Para o MC mandar a câmera é melhor que o **Minecraft seja a fonte da verdade** (posição do olho = posição do player MC + 1.62). Assim a câmera não balança com a animação do Geralt. O head bone serve só para "colar" no fantoche, se desejado.
- Near plane: não há setter vanilla. O corpo vai estar escondido, então isso importa pouco. Se for preciso, o caminho é uma native via DLL [?].

### 1.6 Terceira pessoa (F5) [I]

- O Minecraft calcula a câmera de 3ª pessoa recuando 4 blocos e fazendo raycast contra os blocos do MC. Como o mundo visível é o do W3, há duas opções: (a) a DLL manda o olho + yaw/pitch + modo + distância desejada e o W3 faz o clip contra a própria geometria (`theGame.GetWorld().StaticTrace(from, to, hit, normal)`, já usado em `mw3_main.ws:127`). Depois usa A2 com a posição final, ou A1 com `pivotDistanceValue`. Isso deixa o collision controller do W3 trabalhar. (b) A DLL manda a posição final já calculada pelo MC (consistente com a lógica do MC, mas atravessa paredes do W3). Recomendo **(a)**.
- Front (F5 duas vezes): yaw+180 e pitch invertido. No A1 basta mudar a rotação do pivot.

### 1.7 Convenção de eixos (a validar) [?]

- W3: Z para cima, unidades em metros, EulerAngles {Pitch, Yaw, Roll} em graus no script (nativo: Roll, Pitch, Yaw, ver offsets acima). `VecFromHeading`, `VecHeading` e `RotForward` são natives (`core\math.ws:380-462`), sem fonte. Não verifiquei o sinal do yaw nem qual eixo corresponde ao heading 0.
- Minecraft: Y para cima, yaw 0 = +Z (sul) e cresce no sentido horário visto de cima; pitch positivo = olhar para baixo.
- Sugestão: um `exec function mw3camtest()` que loga `RotForward(EulerAngles(0,0,0))`, `RotForward(EulerAngles(0,90,0))` e `RotForward(EulerAngles(30,0,0))`. Com isso a conversão fica fechada em 1 minuto de runtime.

### 1.8 Esconder o player [O]

| método | onde | observação |
|---|---|---|
| `CEntity.SetHideInGame(hide, optional immediate)` | `engine\entity.ws:241`, native `0x22558F0` | precedente vanilla: `poster.ws:96/128`. Esconde a entidade inteira [I: provavelmente inclui a sombra]. |
| `CActor.SetVisibility(bool)` / `GetVisibility` | `game\actor.ws:1207-1208` | alternativa no nível do ator |
| `CDrawableComponent.SetVisible(bool)`, `SetCastingShadows(bool)` | `engine\components.ws:473,479` | por componente: `thePlayer.GetComponentsByClassName('CDrawableComponent')` (`entity.ws:252`). Reaplicar quando a aparência ou o equipamento mudar. Útil para "corpo invisível, sombra visível". |
| `CAppearanceComponent.ApplyAppearance / Include/ExcludeAppearanceTemplate` | `engine\appearanceComponent.ws:12`, mod SkacikPL | precisa de templates próprios |
| Itens montados (espadas, besta) | `inventoryComponent.ws:211` (`GetAllItems`), `:1144` (`GetItemEntityUnsafe`), `:1157` (`IsItemMounted`) | `GetItemEntityUnsafe(id).SetHideInGame(true)`. O próprio `poster.ws:78-87` esconde o item da mão esquerda à parte, então os itens anexados talvez não herdem o hide [I]. |

Recomendação: `SetHideInGame(true)` no player e nas item entities montadas, reaplicado a cada ~1 s ou no evento de equipar. Para F5 o modelo visível será o skin do MC desenhado pela DLL (ver `01-render.md`).

---

## 2. Câmera nativa (fallback)

### 2.1 RTTI e vtables [O, `rtti_vtables.py`]

| classe | TypeDescriptor | vtable(s) (offset do subobjeto) |
|---|---|---|
| `CCameraDirector` | 0x51BC418 | 0x3989EA8 (0), 0x398A078 (16) |
| `CR4CameraDirector` | 0x51BC380 | 0x3810308 (0), 0x3810470 (16) |
| `ICamera` | 0x51BC360 | 0x38101E0 |
| `CCamera` | 0x522D498 | 0x39AF930 (0), 0x39AF850 (16), 0x39AF808 (368 = ICamera) |
| `CStaticCamera` | 0x5231D00 | 0x39C1658 (0), 0x39C1BF0 (16), 0x39C1C20 (368) |
| `CCustomCamera` | 0x51BC338 | 0x380FC40 (0), 0x3810168 (16), 0x3810198 (368 = ICamera) |
| `CGameFreeCamera` | 0x52331E8 | 0x39C7628 |
| `CSimpleFreeCamera` | 0x52230E8 | 0x398A030 |
| `CCameraComponent` | 0x522FF80 | 0x39B9E30, 0x39BA388, 0x39B9DE8 |
| `CCustomCameraPlayerPPC` / `PhotomodePPC` | — | 0x38095B0 / 0x380D8E0 |
| `CRenderViewport` / `CViewportWindow` / `CWin32GameViewport` | — | 0x36EC7F8 / 0x36ECE30 / 0x38268C0 |

- **Não existe RTTI `CRenderCamera`** [O]: é struct/classe sem vtable, então não aparece. Strings úteis: `CameraCB` (cbuffer de shader, 0x32194B5), `UseCameraForStreamingOverride`, `CreateCamera`, `OnCameraBlendToGameplay`.
- `ICamera` aparece como interface no offset 368 de `CCamera` e de `CCustomCamera`. O director provavelmente mantém uma pilha de `ICamera` e faz blend dos dados de cada uma (posição, rotação, fov) para o cache em `director+0x70..0xA0` [I, alta]. Os getters nativos do director leem exatamente esse cache (tabela 1.3).

### 2.2 Onde a DLL poderia sobrescrever [I]

Por ordem de preferência:
1. **Não sobrescrever nada nativo e usar A2 via natives WNB.** É o mais estável entre patches.
2. **Hook da vfunc de update de `ICamera` em `CCustomCamera`** (vtable 0x3810198) ou da função interna `0x1E0F420` (chamada por `UpdateWithoutInput`), gravando depois `this+0x1D0/0x1F0` e o fov. Confiança média: falta identificar qual slot é o `Update`/`GetData`.
3. **Pós-update do `CCameraDirector`**: achar quem escreve `director+0x70` e `+0x80..+0x88` (procurar `vmovups [reg+0x70]` em funções que referenciam a vtable 0x3989EA8/0x3810308) e sobrescrever depois. A view matrix enviada ao render deve sair dali (render frame info) [I, média]. Não está localizado.
4. Para **ler** a câmera final (alinhar o render do MC), o ponto já identificado em `01-render.md` serve: `slSetConstants` (IAT 0x298DCC8, chamada em 0x1B80E55), cujas constantes Streamline trazem `cameraViewToClip`, `clipToCameraView`, `cameraPos/Up/Right/Fwd` e o jitter [I, alta]. Só leitura.

Prior art nativo: as ferramentas do Otis_Inf (IGCS, "The Witcher 3 photomode mod", builds para Classic/NextGen/Remastered) fazem AOB/inline hooks no código que escreve a câmera, mas o código não é público para essas versões. Fica só como confirmação de que o hook nativo é viável.

---

## 3. Input

### 3.1 APIs importadas [O, `exe_input_camera_scan.py`]

`USER32.dll` (RVAs de IAT):

| API | IAT | call sites |
|---|---|---|
| `RegisterRawInputDevices` | 0x298D630 | 0x1E6D861, 0x1E6DB2E (teclado), 0x1E6EA21, 0x1E6EA7B (mouse) |
| `GetRawInputData` | 0x298D738 | 0x1C67B04 (WndProc do viewport), 0x1E67737 (device manager) |
| `GetAsyncKeyState` | 0x298D620 | 0x1E6D49B, 0x2253E7B/E8F/EA3/EB9 (4 seguidas, provavelmente modificadores) |
| `SetWindowLongPtrA/W`, `CallWindowProcW` | 0x298D798 / 0x298D748 / 0x298D758 | ver 3.2 |
| `RegisterClassExA`, `CreateWindowExA` | 0x298D598 / 0x298D5A0 | 0x1C694AE / 0x1C6952C |
| `PeekMessageA`, `GetMessageA`, `TranslateMessage`, `DispatchMessageA`, `DefWindowProcA`, `MapVirtualKeyA` | — | loop de mensagens ANSI |
| `ClipCursor`, `SetCursorPos`, `GetCursorPos`, `SetCapture`, `ReleaseCapture`, `ShowCursor`, `SetCursor` | — | clip e recentralização do cursor (0x1C181BE, 0x1C182AD..., 0x1C672A5, 0x1C68471) |

Outras DLLs: `DINPUT8.dll!DirectInput8Create` (IAT 0x298C030, call 0x1E673D1), `XINPUT9_1_0.dll!XInputGetState` (0x298D9A0, call 0x1E6C50A) e `HID.DLL` (HidD_*/HidP_*, pads Sony). **GameInput** é carregado dinamicamente (strings `GameInput.dll`, `GameInputRedist.dll`, `GameInputCreate` @0x2B372C0, `SOFTWARE\Microsoft\GameInput`). Também há `XInput1_4.dll`/`XInput1_3.dll` em UTF-16.

RTTI de input [O]: `CInputManager` (vtable 0x399CC60), `CInputDeviceManagerWin32` (0x38264A0), `CInputDeviceKeyboardRawInput` (0x3826740), `CInputDeviceMouseRawInput` (0x3826818), `CInputDeviceKeyboardDInput` (0x3826708), `CInputDeviceMouseDInput` (0x3826798), `CInputDeviceGamepadXInput`, `CInputDeviceGamepadGameInput`, `CInputDeviceGamepadSonyForPC`, `CInputDeviceTouchscreenWin32`, `IRawInputListener` (0x36ECE10), `IInputRawInput`.

### 3.2 Como o raw input é registrado [O]

As quatro funções em 0x1E6D830, 0x1E6DAF0, 0x1E6E9F0 e 0x1E6EA40 montam um `RAWINPUTDEVICE` e chamam `RegisterRawInputDevices(&rid, 1, 0x10)`:

```
mov word [rid+0], 1        ; usUsagePage = Generic Desktop
mov word [rid+2], 6 / 2    ; usUsage = Keyboard (6) / Mouse (2)
mov dword [rid+4], 0       ; dwFlags = 0  -> sem RIDEV_NOLEGACY, sem RIDEV_INPUTSINK
mov [rid+8], rdx           ; hwndTarget = janela do viewport
```

Consequências: (a) o W3 recebe `WM_INPUT` **e** as mensagens legadas (`WM_KEYDOWN`, `WM_CHAR`, `WM_MOUSEMOVE`...); (b) sem `INPUTSINK`, só recebe input com a janela em foreground.

Janela e WndProc [O]: `RegisterClassExA` em 0x1C694AE com `lpszClassName = "W2ViewportClass"` (0x2E88248) e `lpfnWndProc = 0x1C67740`. Esse stub pega `GWLP_USERDATA` (−21) e despacha para o handler-membro `0x1C679C0`, que trata `WM_INPUT` (chama `GetRawInputData` em 0x1C67B04 e repassa aos `IRawInputListener`), botões do mouse (0x201...), `WM_SIZE`/clip etc.
Existe ainda um subclass do próprio jogo em 0xF6C700 (`SetWindowLongPtrW(hwnd, GWLP_WNDPROC=-4, 0xF6C860)`). Ele só intercepta `WM_DEVICECHANGE` (0x219, wParam 0x8000/0x8004) e repassa via `CallWindowProcW` (hotplug de controle).

### 3.3 Método de interceptação recomendado (DLL) [I, alta]

**Subclass da WndProc + filtro**, instalado depois que a janela `W2ViewportClass` existir (`FindWindowA("W2ViewportClass", NULL)` ou hook em `CreateWindowExA`):

```cpp
LRESULT CALLBACK MW3_WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (g_mw3Gameplay && !W3MenuOpen()) {           // flag vinda do script via native
    switch (m) {
      case WM_INPUT: {
        // ler com GetRawInputData (copia; o buffer continua valido para DefWindowProc)
        // encaminhar teclado/mouse ao Minecraft (shared memory/pipe)
        if (!IsPassthroughRawKey(...))            // Esc, F-keys de menu do W3...
          return DefWindowProcW(h, m, w, l);      // obrigatorio p/ WM_INPUT; W3 nao ve
        break;
      }
      case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP: case WM_CHAR:
        if (w != VK_ESCAPE /* + whitelist */) return 0;
        break;
      case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONDOWN:
      case WM_RBUTTONUP: case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MOUSEWHEEL:
      case WM_XBUTTONDOWN: case WM_XBUTTONUP:
        return 0;
    }
  }
  return CallWindowProcW(g_origWndProc, h, m, w, l);
}
```

Pontos de atenção:
- **`WM_INPUT` é a fonte principal do W3** (`CInputDeviceKeyboardRawInput`/`MouseRawInput`). Filtrar só `WM_KEYDOWN` não basta. Ao "engolir" `WM_INPUT`, chamar `DefWindowProc` para o sistema liberar o handle.
- **Esc e menus:** deixar passar Esc (raw `VKey==VK_ESCAPE` e `WM_KEYDOWN VK_ESCAPE`). Quando o script avisar que um menu está aberto (`theGame.GetGuiManager().IsAnyMenu()`, usado em `photomodeManager.ws:89`), a DLL desliga o filtro e o W3 recebe tudo (navegação nos menus Scaleform).
- **Mouse look:** o Minecraft precisa de deltas. Pegar `RAWMOUSE.lLastX/lLastY` do `WM_INPUT` e repassar. O W3 continua fazendo `ClipCursor`/`SetCursorPos` (0x1C181BE...), o que ajuda a manter o cursor preso.
- **Fallbacks que vazam:** `GetAsyncKeyState` (0x1E6D49B, 0x2253E7B..EB9) lê o estado físico e ignora o filtro de mensagens. Pelo padrão de 4 chamadas seguidas, parecem modificadores (Shift/Ctrl/Alt) [I]. Se incomodar, fazer um IAT hook de `GetAsyncKeyState` (IAT 0x298D620) que retorna 0 no modo gameplay, exceto para Esc. DirectInput8 só é usado se o device DInput for o escolhido [?]. Se ativo, hook de `IDirectInputDevice8::GetDeviceState/GetDeviceData` (vtable via `DirectInput8Create`, IAT 0x298C030).
- **Gamepad:** XInput/GameInput/HID não passam pela WndProc. Fora de escopo (o MC é teclado e mouse). Se precisar, IAT hook de `XInputGetState` (0x298D9A0).
- **Foco:** o Minecraft roda escondido e não deve roubar o foco. A DLL encaminha o input por IPC, então a janela do W3 continua em foreground e recebe `WM_INPUT` normalmente.
- Alternativa mais "cirúrgica" e frágil: hook nas vfuncs de `CInputDeviceKeyboardRawInput`/`MouseRawInput` (vtables 0x3826740/0x3826818). Não recomendo como primeira opção.

### 3.4 Lado do script: impedir o Geralt de reagir [O]

APIs (`game\inputManager.ws`): `SetContext(name)` (l.42), `GetContext()` (l.43), `StoreContext(newContext)` / `RestoreContext(stored, couldChange)` (l.47/51), `RegisterListener`/`UnregisterListener` (l.39-40), `GetActionValue` (l.29), `SuppressPropagatingEventAfterAction` (l.136), `ForceDeactivateAction` (l.114), `IsInputIgnored`/`ClearIgnoredInput` (l.34-36). A native `IgnoreGameInput` existe no exe (string 0x399CE78, junto de `ClearIgnoredInput`), mas o import foi removido do `.ws`; não sei a assinatura [?].

Contextos ficam em `Documentos\The Witcher 3\input.settings` (ex.: `%USERPROFILE%\OneDrive\Documentos\The Witcher 3\input.settings`). Seções observadas: `[EMPTY_CONTEXT]` (linha 1555, **vazia**), `[Exploration]` (1557), `[Combat]` (723), `[Photomode]` (2506, com `IK_Escape=(Action=DisablePhotoMode)`), `[Scene]`, `[ScriptedAction]`... O Esc do gameplay é `IK_Escape=(Action=IngameMenu)` (ex.: linhas 555, 695, 822).

Opções, da mais forte para a mais fraca:
1. **Contexto próprio** `[MW3_Puppet]` no `input.settings` (o `install.ps1` já edita esse arquivo), contendo só `IK_Escape=(Action=IngameMenu)` e os atalhos do MW3 (F6..F9). Ativar com `theInput.StoreContext('MW3_Puppet')` e sair com `RestoreContext('MW3_Puppet', true)`, mesmo padrão de `poster.ws:94/129`, `ingameMenu.ws:406/652` e `commonIngameMenu.ws:61/80`. Assim `GI_AxisLeftX/Y` e `GI_MouseDampX/Y` ficam 0 e nenhum ataque ou interação dispara.
   - Problema: o jogo troca de contexto sozinho (combate, nado, cavalo, cutscene): `explorationStateManager.ws:1453-1462`, `combatSword.ws:19`, `combatFists.ws:24`, `aimThrow.ws:35`, `r4Player.ws:13833`... Solução: `@wrapMethod(CR4Player) GetExplorationInputContext()` e `GetCombatInputContext()` (`r4Player.ws:15164-15172`) retornando `'MW3_Puppet'` no modo fantoche, mais um "watchdog" por frame (`if theInput.GetContext() != 'MW3_Puppet' && !menu ...`).
   - Quem abre o menu restaura o contexto por conta própria (`ingameMenu.ws`), então o Esc continua funcionando.
2. **`thePlayer.BlockAction(EIAB_*, 'MW3')` / `BlockAllActions('MW3', true, exceptions)`** (`player.ws:314,406`, `playerInput.ws:216,378`; enum `playerTypes.ws:1050-1104`: `EIAB_Movement`, `EIAB_Jump`, `EIAB_Roll`, `EIAB_Dodge`, `EIAB_LightAttacks`, `EIAB_CameraLock`...). Bloqueia ações de gameplay, mas o movimento e a câmera continuam lendo `GI_*`. Use como camada extra contra ataques e interações, mantendo `exceptions` vazias para o que o fantoche precisa disparar via script.
3. **Câmera:** `thePlayer.EnableManualCameraControl(false, 'MW3')` (`r4Player.ws:5199-5217` → `GetGameCamera().EnableManualControl(false)`). No A2 não importa (a câmera do photomode é outra), mas evita a câmera de gameplay girar com o mouse se algo vazar.
4. Desregistrar listeners do handler do player (`theInput.UnregisterListener(thePlayer.GetInputHandler(), 'Focus')`, como o mod SkacikPL faz). É granular, mas precisa re-registrar ao sair.

Recomendação: **DLL filtra no WndProc (primeira barreira) + contexto `MW3_Puppet` com Esc (segunda barreira) + `BlockAction` de combate e interação (terceira).**

---

## 4. Mover o fantoche

### 4.1 APIs [O]

| API | onde |
|---|---|
| `CEntity.Teleport(pos)`, `TeleportWithRotation(pos, rot)`, `TeleportToNode` | `engine\entity.ws:44-50` (uso por frame vanilla em props do photomode: `photomodeMenu.ws:3598`) |
| `CEntity.SetBehaviorVariable(name, float, optional allInstances)`, `RaiseForceEvent(name)` | `entity.ws:226, 61` |
| `CActor.ActionMoveTo*Async(target, moveType, speed, radius)`, `ActionCancelAll()` | `actor.ws:765-789, 678` (navmesh + locomoção) |
| `CActor.EnablePhysicalMovement`, `EnableCharacterCollisions`, `EnableCollisions`, `SetRotationTarget` | `actor.ws:1019, 1029, 997, 1055` |
| `CMovingAgentComponent.SetMoveType`, `SetGameplayRelativeMoveSpeed`, `SetAnimatedMovement`, `TeleportBehindCamera` | `game\components\movingAgentComponent.ws:15, 93, 213, 26` |
| `CMovementAdjustor`: `CreateNewRequest`, `SlideTo`, `SlideTowards`, `RotateTo`, `MaxLocationAdjustmentSpeed`, `MaxRotationAdjustmentSpeed`, `AdjustmentDuration`, `KeepActiveFor`, `DontEnd`, `ReplaceTranslation`, `ReplaceRotation`, `AdjustLocationVertically`, `Cancel*` | `game\movement\movementAdjustor.ws:19-129` (uso: `r4Player.ws:5195`) |
| `CPlayer.SetManualControl(movement, camera)` / `SetIsMovable` / `GetIsMovable` | `player.ws:1461-1479` |

### 4.2 Onde a locomoção do player lê o input [O]

- `CPlayer.HandleMovement`: `rawLeftJoyVec = GI_AxisLeftX/Y` e `rawRightJoyVec = GI_MouseDampX/Y` ou `GI_AxisRightX/Y` (`player.ws:574-600`). Define `lAxisLength` no behavior (`player.ws:625`) e calcula `rawPlayerSpeed` e o heading.
- `CR4Player.WouldLikeToMove` (`r4Player.ws:1429-1440`) e `CR4Player.HandleMovement` (`r4Player.ws:1442`).
- Exploração moderna: `locomotionDirectController.ws:639-726` (`GetInputHeading`, `GetInputInPlayerSpace`, ...) e `l.1101` (stick direito).
- Outros: `movable.ws:114`, `traverseExploration.ws:186`, `approachInteraction.ws:329`.

### 4.3 Recomendação [I]

1. **"Stick virtual" (preferido):** com `@wrapMethod`/`@replaceMethod` nas funções acima, trocar `theInput.GetActionValue('GI_AxisLeftX/Y')` por `MW3_MoveX()/MW3_MoveY()` (natives WNB) no modo fantoche. O input já chega relativo à câmera, porque a locomoção usa o heading da câmera (`locomotionDirectController.ws:636`). Como a câmera ativa é a nossa, isso bate com o yaw do MC [I]. O Geralt anda e corre com animação, colisão, degraus e navmesh. Sprint e pulo podem ser disparados pelos mesmos caminhos dos listeners (`Sprint`, `Jump`), chamando os handlers diretamente.
   - Detalhe: o contexto `MW3_Puppet` zera os `GI_*` reais, então só os nossos valores entram.
2. **Correção de posição (drift MC × W3):** `CMovementAdjustor`, com `ticket = adj.CreateNewRequest('MW3Sync'); adj.SlideTo(ticket, alvo); adj.MaxLocationAdjustmentSpeed(ticket, v); adj.RotateTo(ticket, yaw); adj.KeepActiveFor(ticket, 0.1)`, reemitido por frame. Ele soma correção ao movimento animado sem desligar a locomoção.
3. **`TeleportWithRotation` por frame:** evitar como mecanismo principal. Ele "pula" o motion extraction e o behavior graph fica sem velocidade coerente (desliza e anima idle) [I]. Usar só quando o erro passar de ~1-2 m ou para respawn/teleporte do MC.
4. **AI (`ActionMoveToAsync`)**: serve se o MC mandar "vá para X" em vez de input contínuo. Tem latência e pathfinding próprio. Útil para o modo "IA" mencionado no escopo.
5. Rotação do corpo: no modo fantoche, `SetBehaviorVariable`/`SetRotationTarget` ou `adj.RotateTo` para alinhar o heading ao yaw do MC quando parado.

---

## 5. Transporte dos valores DLL → script (por frame)

- Não existe canal vanilla adequado (ver `02-script-natives.md` §3). Usar **WitcherNativeBridge** (MIT; tem build "Remastered v5.00+"; API `WNB_RegisterNative`, `WNB_Read{Int,Bool,Float,String,Name}Parameter`, `WNB_Return*`). Funções sugeridas, todas globais e com tipos primitivos:
  - `import function MW3_CamActive() : bool;`
  - `import function MW3_CamGet(i : int) : float;` (0..2 = olho xyz já em coordenadas do W3, 3 = yaw, 4 = pitch, 5 = fov, 6 = distância, 7 = modo 0/1/2)
  - `import function MW3_MoveGet(i : int) : float;` (stick virtual, sprint, pulo...)
  - `import function MW3_SetUiState(menuOpen : bool);` (script → DLL, liga/desliga o filtro do WndProc)
- Onde ler: no wrap de `OnGameCameraPostTick` (sincronizado com o update da câmera) e/ou num timer de 0 s no player (`AddTimer('MW3_Tick', 0, true)`, mesmo padrão do mod SkacikPL e de `r4Player.ws:13628`). O `OnPlayerTickTimer` existe (`player.ws:533-540`), mas não roda a cada frame [I].

---

## 6. Confiança e lacunas

| item | confiança |
|---|---|
| APIs de script de câmera, photomode, hide, input context e locomoção | **alta [O]** (arquivo:linha) |
| A2 (photomode cam) funcionar fora do photomode e por frame | **média [I]**: o padrão é idêntico ao vanilla, mas não testei sem o menu e o flag de photomode |
| A1 (moveData) dar controle total | **média [I]**: o vanilla grava `*Value` direto, mas o collision controller e os estados sem encadeamento podem interferir |
| `@wrapMethod` em `event` e em métodos de estados | **[?]**: testar. Para estados, embrulhar o pai em `CR4Player` (os estados chamam `parent`) |
| Offsets `CCameraDirector+0x70/0x80/0xA0` e `CCustomCamera+0x1D0/0x1F0` | **alta [O]** (getters e setters nativos desmontados) |
| Ponto exato da view matrix (escrita) | **[?]** não localizado. Leitura via `slSetConstants` (ver 01-render) |
| Raw input `dwFlags=0`, WndProc `W2ViewportClass` 0x1C67740 / handler 0x1C679C0 | **alta [O]** |
| Uso real de DInput vs RawInput para teclado e mouse | **[?]** os dois devices existem (RTTI). O registro raw está presente. Confirmar em runtime qual é instanciado |
| Assinatura de `IgnoreGameInput` | **[?]** |
| Convenção de yaw/pitch W3 ↔ MC | **[?]**: `exec` de teste (1.7) |
| Itens montados herdarem `SetHideInGame` | **[?]** |

### Fontes externas
- [ExtremeDotneting/Witcher-3-FIRST-PERSON-CAMERA-REWORKED](https://github.com/ExtremeDotneting/Witcher-3-FIRST-PERSON-CAMERA-REWORKED): `FirstPersonCamera.ws` (SkacikPL), lido em scratchpad.
- Mods de 1ª pessoa no Nexus: [Gervant First Person](https://www.nexusmods.com/witcher3/mods/5706) (usa ASI), [Remastered First Person View Toggle](https://www.nexusmods.com/witcher3/mods/13072), [First Person Witcher Senses](https://www.nexusmods.com/witcher3/mods/13157).
- [WitcherNativeBridge](https://github.com/rejuvenate7/WitcherNativeBridge) (MIT), [TW3SE](https://www.nexusmods.com/witcher3/mods/13837) (página deu 403).
- [Otis_Inf — Witcher 3 photomode tools](https://opm.fransbouma.com/Cameras/witcher3.htm), [InjectableGenericCameraSystem](https://github.com/FransBouma/InjectableGenericCameraSystem/releases).

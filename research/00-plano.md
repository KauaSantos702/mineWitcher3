# MineWitcher3: resumo da fase 1 e plano da fase 2

Alvo: `witcher3.exe` 5.0.0.1048522 (DX12), SHA-256 `2DF76539…1FB101`. Minecraft 26.3 + Fabric 0.19.5.
Fase 1 = análise estática somente leitura. **Nada foi testado em runtime.**

## Arquitetura decidida (derivada do SkyCraft, MIT, ver 05)

```
Minecraft 26.3 (oculto, Fabric)  <── shared memory Local\MineWitcher3_v1 ──>  witcher3.exe
  física, blocos, inventário,                                             mw3.asi (C++): hooks D3D12,
  câmera F5, skin do jogador                                              input, launcher, ponte de natives
  exporta malhas/atlas/avatar/overlay                                          │ natives novas
                                                                          WitcherScript (mod/): Geralt fantoche,
                                                                          câmera, dano, raycasts de colisão
```

## Respostas da fase 1

| Pergunta | Resposta | Confiança | Doc |
|---|---|---|---|
| Como injetar a DLL | Ultimate ASI Loader como `winmm.dll` ou `dinput8.dll` em `bin\x64_dx12` (nenhum dos dois existe hoje). Sem DRM nem anti-tamper | alta | 01, 02 |
| D3D12 | Vem do NVIDIA Streamline (`sl.interposer.dll`), não do d3d12/dxgi direto. Hooks precisam mirar os objetos nativos por baixo dos proxies | alta | 01 |
| Onde compor os blocos | Hook no seletor AA/upscaler `0x1C091C0` (depois da cena, antes do tonemap e do HUD). Profundidade provavelmente reverse-Z. Overlay (mão/HUD do MC) no `Present` | média-alta | 01 |
| Natives novas para o script | WitcherNativeBridge (MIT, build v5.00+). Os padrões de bytes batem com o nosso exe. Plano B: hook próprio em `RegisterGlobalFunction` `0x26CE2D0` | alta / média | 02 |
| Câmera (F5) | Pelo script: câmera do photomode (`CCustomCamera`), posição e rotação absolutas por frame. Plano B nativo: `CCameraDirector+0x70/0x80/0xA0` | média | 03 |
| Esconder o Geralt | `thePlayer.SetHideInGame(true)` + armas montadas | alta | 03 |
| Input | Raw Input sem NOLEGACY. Subclass da WndProc `W2ViewportClass` + contexto de input próprio no script | alta | 03 |
| Mover o fantoche | Injetar o stick virtual (`GI_AxisLeftX/Y`) para manter animação, colisão e navmesh. `TeleportWithRotation` só para correções | média | 03 |
| Física | PhysX 3.3 embutido no exe | alta | 04 |
| Colisão para o MC | Estágio A em script: grade de raycasts com `StaticTrace` em volta do jogador | média | 04 |
| Colisão dos blocos no W3 | Templates próprios de cubo de 1 m (WolvenKit), no padrão das invisible walls vanilla | média | 04 |
| NPCs e dano | `FindGameplayEntitiesInSphere` + `W3DamageAction`/`ProcessAction` com o player como atacante (XP, loot e quests funcionam) | alta | 04 |
| Coordenadas | `mc = (w.x, w.z, -w.y)`. O sinal do yaw ainda precisa ser confirmado num teste | média | 04, 05 |

## Fase 2: ordem proposta (cada passo testável sozinho)

1. **Prova de vida nativa:** ASI Loader + `mw3.asi` mínimo que escreve um log. Depois, uma native `MW3_Ping()` registrada via WitcherNativeBridge e chamada pelo script.
2. **Protocolo + `fake_witcher.py`:** portar o `skycraft_protocol.h` para `mw3_protocol.h` (escala 1.0, eixos W3). Isso permite desenvolver o lado Fabric sem abrir o W3.
3. **Fabric:** portar o mod do SkyCraft (`dev.minewitcher3`): link, janela oculta, exportadores, colisão, input.
4. **Câmera + fantoche:** natives `MW3_Cam*`, câmera photomode controlada pelo MC, Geralt escondido, F5.
5. **Overlay:** mão, HUD e telas do MC no `Present` (D3D12, cópia via CPU como no SkyCraft).
6. **Mundo:** blocos desenhados no hook do upscaler, com o depth do jogo (precisa de RenderDoc/Ghidra para os argumentos).
7. **Colisão e combate:** raycasts do W3 → MC; blocos do MC → cubos com colisão no W3; NPCs como proxies no MC → dano no W3.

Lacunas que pedem Ghidra ou RenderDoc: argumentos de `0x1C091C0`, layout do `CScriptStackFrame`, formato do depth, ponto de escrita da view matrix.

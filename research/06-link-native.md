# 06 — Marco "link": lado nativo (mw3.asi native v3)

Estado: compila sem warnings, `mw3_linktest.exe` PASS, leitura cruzada com o layout do
`tools/fake_witcher.py` OK. **Nada testado no jogo ainda.**

## Arquivos

| Arquivo | Origem | O que faz |
|---|---|---|
| `native/src/link.*` | SkyCraft `Link.cpp/.h` | mapeamento `Local\MineWitcher3_v1` (SDDL do SkyCraft), header/magic/PIDs, heartbeat, seqlock GameState/McState/WaterGrid, anel de input (com mutex: 2 produtores), colisão, atores, eventos, world entities, render ring, overlay. Conversão de ângulos MC<->W3 |
| `native/src/session.*` | SkyCraft `Game.cpp` (PerFrame) | thread do link ~64 Hz (heartbeat + McState; vai para o Present depois), teleportSeq, olhar integrado com a fórmula do MC, estado do script, snapshot para as natives |
| `native/src/launcher.*` | SkyCraft `Launcher.cpp` | inicia o Prism portátil pelo shell do Explorer; mutex `Local\MineWitcher3_v1_minecraft`; sem Prism só loga |
| `native/src/config.*` | novo | `mw3.ini` ao lado do `witcher3.exe` (`native/mw3.ini` é o modelo) |
| `native/src/input_hook.cpp` | tabela/roteamento de SkyCraft `Input.cpp` | WndProc: F5 sempre engolido; com o MC linkado, teclado/mouse vão para o anel |
| `native/test/linktest.cpp` | novo | teste de console (não instalado) |

## Roteamento de input (`session::RouteInput`)

Liga quando: `bForwardInput=1` **e** MC vivo (heartbeat < 3 s) com `kMcInWorld` **e** o script mandou
estado há < 300 ms **e** o script não marcou menu/loading (`IsAnyMenu() || IsDialogOrCutscenePlaying()`,
`IsLoadingScreenVideoPlaying() || IsBlackscreenOrFading()`). Quando o jogo pausa num menu o
`OnGameCameraPostTick` para, o estado fica velho e o input volta todo ao W3 sozinho.

Com o roteamento ligado:
- Teclado: `WM_INPUT` → código DIK (set 1 + 0x80 se E0) → scancode SDL (tabela do SkyCraft) → `kInKey`.
  Ficam com o W3 (não vão ao MC): Esc, I, M, J, K, F6–F9, console (`), Alt+F4. `O` → `kInOpenMenu`.
  Com uma tela do MC aberta tudo vai ao MC (Esc fecha a tela) e `WM_CHAR` vira `kInText`.
- Soltar tecla/botão também chega ao W3 (evita tecla presa do lado do W3).
- Mouse: delta cru (`lLastX/Y`) → yaw/pitch integrados aqui (`s = sens*0.6+0.2; f = s³·8·0.15`) e
  mandados no `GameState`; com tela do MC aberta vira cursor (`kInCursor`). Botões SDL 1/3/2/4/5,
  roda `kInScroll` (±120).
- F5: sempre engolido. Linkado e roteando → `kInKey` F5 (o MC cicla a câmera dele). Sem MC → contado
  para o `MW3_PollCameraKey` (comportamento antigo).
- Saiu do roteamento / perdeu o foco → `kInReleaseAll`.

## Teleporte / posição

Sem fantoche ainda: o MC é teleportado para os pés do Geralt quando conecta, depois de loading, troca
de mundo (`GetDepotPath` → stem → FNV-1a) ou quando o Geralt se afasta > 2 m do último teleporte. Como
não mandamos colisão, o `holdUntilReady` do MC nunca solta: o jogador do MC fica parado ali (bom para
este marco: a câmera fica na cabeça do Geralt e o olhar funciona).

## Convenções assumidas (TODO confirmar no jogo)

- `w3Yaw = 180 - mcYaw`, `w3Pitch = -mcPitch` (`McYawToW3Yaw`/`McPitchToW3Pitch` em `link.cpp`),
  supondo `EulerAngles.Yaw` 0 = +Y (norte), crescendo anti-horário, Pitch positivo = para cima.
  Teste rápido: no jogo, olhar para o norte do mapa com o MC linkado; se a câmera olhar para o sul,
  troque o sinal só nessas funções.
- `MW3_McYaw/Pitch` devolvem o olhar integrado do lado do jogo (o que o MC aplica no jogador), não o
  eco do `McState.yaw` (latência zero, como o SkyCraft).

## v4: colisão Stage A + fantoche (não testado no jogo)

**Colisão** (`collision.*`, `collision_mesh.*`, `mw3_collision.ws`). A DLL escolhe as colunas e o
script faz os traces. As colunas ficam a cada 0,5 m num raio de 32 blocos, das mais próximas do
jogador para as mais distantes. Cada coluna recebe raios `StaticTrace` para baixo e para cima, com os
grupos de 04 §2.2 e até 6 superfícies por sentido. O orçamento é `[Collision] iTracesPerFrame=200`, e
uma coluna começada sempre termina. O nível da água é lido uma vez por bloco.
- Amostra → intervalos sólidos `[fundo, topo]`. Um fundo "aberto" significa sólido até o fim do scan.
  O intervalo mais baixo é sempre aberto e vãos < 1,0 m (`kMinGap`) viram sólido (ver v4.1 abaixo).
- Triângulos (`kColTris`, o colisor suave do jogador): a face de cima de cada célula passa
  **exatamente** pelo hit no centro da célula (2 triângulos se uma diagonal já passa a ≤ 1 cm dele,
  senão um leque de 4) e pelos cantos, que são a média dos topos vizinhos que formam uma superfície
  só (nunca dois topos da mesma célula). Vizinhos se ligam com diferença ≤ 0,2 (`kJoin`); até 0,55
  (`kSmooth`) só numa rampa, quando a mesma subida continua uma célula adiante de um dos lados (por
  isso a margem do `ColumnInput` é de 2 amostras). Um degrau entre dois planos (meio-fio, escada,
  piso elevado) vira espelho vertical + topos planos, e o step-up do MC (0,6) sobe. Células
  coplanares (±2,5 cm) viram um retângulo só (greedy), faces de baixo também; paredes colineares
  com o mesmo fundo/topo são unidas. Lajes ganham uma face de baixo (teto). Sem vizinho conhecido,
  não há parede.
- Voxels (`kColRegion`, 1/8 de bloco): uma casca de 1 bloco sob cada topo, que desce até o chão do
  vizinho mais baixo para as paredes ficarem sólidas. Servem para mobs, `isKnown` e `hasSolidBelow` do
  `holdUntilReady`.
- Uma coluna de região 8×8 só vai ao MC quando as suas 256 amostras existem. O worker manda tris e
  depois voxels para cada `ry` do intervalo escaneado. Perto do jogador a amostra é refeita a cada 3 s,
  e em qualquer lugar depois de 90 s. Quando um rescan não muda nada, nada é reenviado.
- Época: é nova na primeira conexão, quando o MC reconecta (pid) e quando o mundo troca. Nesses casos
  a DLL manda `kColClear`. Colunas a mais de 56 blocos são esvaziadas no MC (evicção).
- `WaterGrid` 16×16 em volta dos pés. A água dentro de um intervalo sólido não conta, mas um rio sob
  uma ponte conta.
- Teste: `mw3_coltest.exe` usa um mundo sintético e o anel real. Ele lê o anel com um port do
  `W3Collision`, do `TriCollider` e do `holdUntilReady`. O hold solta no frame ~11.

### v4.1: jogador do MC 0,65–0,72 m abaixo do chão em Toussaint (corrigido, não testado no jogo)

Dados de campo: pés do Geralt em z=35,3357 (piso Static), terreno em 35,297 (só 4 cm abaixo),
estrutura Static em 46,63 por cima. O MC assentava em 34,61–34,68 em vários pontos e "prendia no
chão" com movimento aos trancos.

Causa (reproduzida no `coltest`, cena "cidade sintética"):
1. O `StaticTrace` ignora faces de costas. O 2º raio para baixo começa 5 cm sob o piso, já abaixo do
   terreno (+4 cm), e não vê nada. O raio para cima vê o **fundo enterrado** do piso (~0,69 m abaixo
   do topo). O intervalo ficava fechado `[34,65; 35,34]`, com uma face de baixo "andável" em 34,65
   (o `TriCollider` usa |ny|).
2. O hold do MC soltava com voxels de 3×3 blocos (~1,5 s, poucas colunas), não com triângulos. Ao
   andar para uma região cujos triângulos ainda não tinham chegado, o jogador caía alguns ticks.
   Quando a região chegava, o topo estava acima do alcance (0,3 no ar, 0,6 no chão) e ele pousava no
   fundo enterrado, 0,69 abaixo. Com o código antigo, o teste "jogador 0,44 abaixo do piso" termina
   em 63,35 = topo − 0,69. Debaixo da superfície, os topos suavizados (inclinados) empurravam o
   cilindro de lado, o que dava os trancos.
3. Além disso, a média por canto com diferença até 0,55 tirava o topo até 0,30 m do hit
   (cena: 141 de 1848 amostras com erro > 0,06), e degraus de 0,5–0,55 viravam rampas de 45–48°,
   no limite do andável.

Correção:
- `BuildIntervals`: o intervalo mais baixo é sempre aberto (nada tracado abaixo = enterrado); vãos
  < `kMinGap` (1,0 m) são preenchidos.
- Malha: centro exato, ligação só com dz ≤ 0,2 ou numa rampa, merge greedy (ver acima).
- Java `W3Collider.collide`: o jogador não anda (nem cai) para uma região cujos triângulos ainda não
  chegaram (`W3Collision.trisKnownAround`, margem 0,8); espera na borda. Se os pés afundaram na casca
  de voxels (`insideSolid` a +0,5), ele sobe para o `groundAt` (até 2 m). Voando: sem guarda.
- Java `holdUntilReady`: solta quando há triângulos até 2 blocos em volta **e** chão a ≤ 0,5 dos pés
  (o jogador é posto exatamente nele). No ar: 6 s com os triângulos lá. Sem nada: 30 s (log).

Números (`mw3_coltest.exe`): cidade sintética, erro máximo do topo nas amostras 0,30 → 0,026 m
(médio 0,017 → 0,004). Superfícies de 5 cm a 1,9 m acima de um topo tracado sem face de baixo real
(quase todas o próprio topo suavizado flutuando acima do hit): 114 → 0. Triângulos: cidade 8321 → 2007 (−76%), mundo plano
do teste do anel 6561 → 300 (−95%). Escada 0,1/0,15/0,2/0,3/0,45/0,55 sobe sem travar e para no
degrau de 0,7. Meio-fio de 0,15 e rua de 14° sem trancos.

**Captura**: `mw3.ini [Collision] bCapture=1` grava cada coluna em `bin\x64_dx12\mw3_colcapture.bin`
(formato em `collision.h`, até 50 MB). Para analisar: `mw3_coltest.exe --replay <arquivo>`. Ele refaz
os intervalos e as malhas das colunas de região completas e relata o erro do topo (máximo e médio),
os tetos falsos e as caminhadas de 2 m (travou/afundou), com as coordenadas W3 dos piores pontos.

**Fantoche** (`mw3_puppet.ws`, `MW3_Puppet*`/`MW3_McFeet*`/`MW3_McVel*`/`MW3_McFlags`):
- Liga só quando o MC confirma o último teleporte (`teleportAck == teleportSeq`, depois do hold), e
  fica desligado em menu, loading, cena, montado ou morto.
- Posição: `TeleportWithRotation` por frame para os pés do MC, que são os mesmos pés de onde sai o
  olho da câmera. O corpo vira para a direção do movimento.
- Animação: "stick virtual". `CalculateInputVector` (locomoção de exploração), `WouldLikeToMove` e
  `HandleMovement` recebem a velocidade do MC no espaço da câmera, com 4,3 m/s = analógico cheio.
  `exec mw3anim 0` desliga o stick e deixa só o teleporte. `ApplyFallingDamage` do player devolve 0
  com o fantoche, porque o dano de queda é calculado pelo MC.
- `MW3_PuppetMoved` passa a ser a referência do resync de 2 m da sessão. Se o W3 mover o Geralt por
  conta própria (cutscene, porta, fast travel), o MC é teleportado para lá.

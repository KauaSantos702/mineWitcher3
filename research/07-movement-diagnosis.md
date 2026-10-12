# 07 - Diagnostico do movimento ("I can barely walk")

Run analisado: `research/field/mw3-20261012-0910.log` (DLL/script) e `research/field/mc-20261012-0910.log` (Fabric).
SkyCraft de referencia: clone local, `git pull` -> "Already up to date", commit **bfcaf17** (`bfcaf178524b92c2cdeb88e4ce0f13ef9ded6f32`, 2026-10-01, "SkyCraft 0.1.2").
Nada de codigo foi alterado; este documento e so diagnostico.

Convencoes: `SC:` = arquivo do SkyCraft (raiz do clone), `MW3:` = `C:\codes\rea\mineWitcher3`.
Conversao de eixos (MW3: `protocol/mw3_protocol.h:45-46`): MC = (W3.x, W3.z, -W3.y).

---

## 0. Resumo executivo

O "mal consigo andar" vem de uma cadeia de cinco bugs. Nenhum deles e de conversao de coordenadas.

1. **O timer do hold fica velho** (`W3Client.java:275-287`). O `holdSince` e gravado quando o hold comeca, inclusive no hold "estacionado" do menu/loading, e nao e zerado no `requestTeleport`. O primeiro teleporte (09:01:24) ja nasceu com 300 s de hold, entao bateu na hora o timeout de 30 s. O MC foi solto sem colisao nenhuma, caiu e morreu (09:01:26).
2. **A morte do MC nao chega ao W3, e o fantoche segue o MC ate o ponto de respawn.** Ninguem chama `Link::PopEvent` (o `kEvPlayerDied` nunca e consumido), entao o Geralt continua "vivo" e o fantoche continua elegivel. Com `IMMEDIATE_RESPAWN`, o MC renasce no spawn do mundo void (x~8.5, z~8.5, y perto do `min_y=-1024`). Num frame inteiro o `afterRender` publica esses pes com `teleportAck == seq`, e o fantoche teleporta o Geralt ~830 m para o vazio sob Toussaint. **Daqui sai o -1033**: e a posicao real do Geralt (W3 = (8.63, -8.47, -1033.9)), caindo no vazio, e o MC foi teleportado para ela no fim do loading.
3. **O fantoche nao tem nenhuma checagem de sanidade.** `IsBelowGround` devolve `false` (= "seguro") quando o trace nao acha nada (`mw3_puppet.ws:170-171`). Assim, pes do MC sem chao do W3 embaixo sao aceitos.
4. **A eviction de colisao marca regioes como "conhecidas e vazias".** `EvictFar` manda payload vazio de `kColTris`/`kColRegion` (`collision.cpp:548-573`, `collision_mesh.h:95-97`). O MC passa a achar que conhece aquelas regioes, e que elas nao tem chao. Ao voltar para -298 (reload das 09:08), `trisKnownAround` ja da `true`, o hold solta "mid-air" e o MC cai de novo: o loop de mortes por queda.
5. **O fantoche bloqueado nunca pede resync.** Quando os pes do MC afundam no chao do W3, o Geralt para e nao chama `MW3_PuppetMoved`. A referencia de resync da DLL continua igual a posicao do Geralt, entao o gap fica em ~0 e o MC continua caindo ate morrer (09:08:30 -> 09:08:37).

As **100 regioes / 6349 triangulos** sao sintoma, nao causa. Nao existe cap nem anel cheio: o Geralt ficou parado 6 min no vazio, onde toda coluna e vazia, e amostras identicas nao sujam regiao nenhuma.

---

## 1. Pipeline de movimento no SkyCraft (bfcaf17)

### 1.1 Quem manda na posicao
- **O MC manda.** A fisica do `LocalPlayer` do MC e a verdade, e o jogador do Skyrim e um fantoche (`SC: docs/DESIGN.md:108-112`).
- **O Skyrim manda em teleportes**: loading, portas, fast travel e load de save.
  - O Skyrim incrementa `teleportSeq` (`SC: skse/src/Game.cpp:576-593`).
  - O MC teleporta, segura e confirma (ack) quando solta.

### 1.2 Como o fantoche e movido a cada frame
O hook e `PlayerCharacter::Update`, via `PlayerUpdateHook` (`SC: Game.cpp:1035-1040`), e o corpo e `PerFrame`. Ver `Game.cpp:788-815`.
- `a_player->SetPosition(McToSky(feet), true)`.
- `controller->SetLinearVelocityImpl(0)`, `fallStartHeight = pos.z` e `fallTime = 0`. O estado `kOnGround`/`kInAir` vem de `kMcOnGround`.
- A gravidade fica ligada para manter o contato com o chao (comentario em `Game.cpp:792-794`).
- `lastSetPos = pos` e a referencia para detectar "o Skyrim moveu o jogador sozinho".
- A condicao do fantoche fica em `Game.cpp:639`:
  `puppet = haveMc && mcInWorld && mc.teleportAck == teleportSeq && !loading && !a_player->IsDead() && !takeover`.
  `takeover` cobre mobilia, montaria, killmove, cena scriptada e cameras especiais (`Game.cpp:384-413`).

### 1.3 Quando o MC e teleportado para o jogador do Skyrim, e com quais guardas
| Gatilho | Onde |
|---|---|
| MC (re)conectou ou e outro pid: `++epoch`, `Collision::Reset`, `teleportPending`, `settleTimer=1.5 s` | `Game.cpp:525-540` |
| Mudou o mundo (worldspace ou cell interior): o mesmo pacote | `Game.cpp:561-574` |
| `loading` (`!cell \|\| !Is3DLoaded \|\| LoadingMenu`): `teleportPending`, `haveLastSet=false`, settle | `Game.cpp:554, 578-581` |
| O Skyrim moveu o jogador **> 300 unidades (~4.3 blocos)** de `lastSetPos` | `Game.cpp:19, 582-585` |
| O Skyrim devolveu o jogador depois de um takeover | `Game.cpp:615-621` |
| O MC esta segurando **> 8 blocos** do jogador do Skyrim por **> 1 s**: teleporta de novo | `Game.cpp:624-636` |
| O seq so e incrementado **fora** de loading | `Game.cpp:587-593` |

Menus que pausam (`kPausesGame`/`kUsesCursor`) so devolvem o input (`Game.cpp:259-266, 556-559`). Eles nao teleportam.
O `settleTimer` adia a colheita de colisao por 1.5 s depois de load ou teleporte, para nao mandar regioes vazias enquanto o Havok ainda carrega (`Game.cpp:93-96, 996-1000`).

### 1.4 Protocolo de teleporte, ack e hold (lado Fabric)
1. `beginFrame` faz o teleporte se `player != lastPlayer` (join/respawn) ou se `sky.teleportSeq` mudou, e so com `inGame && !loading` (`SC: SkyClient.java:122-136`). Em seguida grava `teleportAck = seq` e `holdPos`.
2. `holdUntilReady` (`SkyClient.java:257-299`):
   - Em menu ou loading, estaciona o jogador (`holdPos = position`, `teleportPending=true`).
   - Solta quando os **voxels** sao conhecidos em y-1, y e y-8 **e** existe solido ate 12 blocos abaixo, ou depois de 6 s.
   - **Nao ha timeout de 30 s.**
   - Ao soltar, sobe o jogador ate o chao exato (`liftOutOfGeometry`, ate 2.5 blocos).
3. `afterRender` publica `mc.teleportAck = holdPos == null ? teleportAck : teleportAck - 1` (`SkyClient.java:385`). O Skyrim so liga o fantoche quando o ack bate.
4. Enquanto isso o lado Skyrim esta em "arriving" (`Game.cpp:624, 640`):
   - O input vai para o MC (`minecraftOwnsPlayer`, `Input.cpp:242`).
   - O jogador do Skyrim fica sob a fisica do proprio Skyrim, parado no chao real.

Observacao: o SkyCraft tem o mesmo `holdSince` velho (`SkyClient.java:269-275`). La isso so faz soltar assim que a regiao fica `known`, porque nao existe timeout de 30 s e as regioes conhecidas sempre tem geometria real.

### 1.5 Interpolacao dos ticks de 20 Hz
- O Fabric publica, a cada tick, `prev/cur` e o `tickQpc` com o resto do partial tick descontado (`SkyClient.java:220-254`).
- O Skyrim guarda um historico de 8 ticks e trava o ritmo em `tickMs` (filtrando o ruido dos stamps).
- Ele renderiza `renderDelayMs` (de 4 a 30 ms, adaptativo) no passado, so interpolando e nunca extrapolando. Quando falta um tick, emenda o fim de um no inicio do proximo (`Game.cpp:39-57, 662-786`).

### 1.6 Morte e respawn
- `ServerPlayerMixin.die` empurra `kEvPlayerDied`.
- `Combat::PerFrame` sempre drena o anel de eventos, mesmo sem fantoche, e chama `KillPlayer` (`SC: skse/src/Combat.cpp:848-866, 885-887, 400-413`).
- O jogador do Skyrim morre, entao `IsDead()` desliga o fantoche (`Game.cpp:639`) antes do respawn imediato do MC. A posicao de respawn do MC (spawn do mundo void) **nunca** chega ao corpo do Skyrim.
- Em seguida vem a camera de morte e o reload do Skyrim -> `loading` -> `teleportPending` -> o MC vai para o save.

## 2. Primeiro spawn sem colisao (pergunta 3)
- **Antes do load:** com `sky.inGame()==false` o MC ja esta estacionado (`holdUntilReady`, ramo do menu).
- **Quando o Skyrim entra no mundo:**
  - `teleportPending` ja e `true` desde o inicio (`Game.cpp:32`), e o `teleportSeq` comeca num valor aleatorio por execucao (`Game.cpp:24-30`).
  - O seq so incrementa com `!loading`.
  - A colheita de colisao espera o `settleTimer` de 1.5 s.
- **O MC** teleporta para o jogador do Skyrim e segura, publicando ack-1, ate os voxels serem conhecidos e houver solido ate 12 blocos abaixo (ou 6 s).
- **O fantoche no meio tempo:**
  - Ele **nao** se mexe (`puppet` falso porque o ack nao bate).
  - O jogador do Skyrim fica sob a propria fisica, de pe no chao real.
  - O input ja vai para o MC, que esta congelado.
  - Se o MC estiver segurando longe do jogador (mais de 8 blocos por 1 s), o teleporte e reenviado.

---

## 3. Onde o nosso port diverge (com a linha do tempo dos logs)

### 3.1 Primeiro hold: timeout instantaneo (causa da 1a queda)
- **08:56:23**: o MC entra no mundo. O W3 ainda esta no menu e o `GameState.flags` esta zerado, entao `holdUntilReady` estaciona: `holdPos = position` e `holdSince = 08:56:23` (`W3Client.java:264-277`).
- **09:01:24**:
  - O MC loga "teleported to -298.06 26.04 867.68" e, no mesmo segundo, "released after 30 s without the game's collision" e **"hold released after 300777 ms (timeout)"**.
  - O `requestTeleport` (`W3Client.java:134-139`) troca o `holdPos` mas **nao zera o `holdSince`**, e `held > 30000` (`W3Client.java:287`) ja era verdade.
  - Do lado W3, a colisao acabou de ligar ("nova epoca 1", 0 colunas).
- **Resultado:**
  - 09:01:24.809: "pes do MC (z=23.8) abaixo do chao".
  - 09:01:26: "player died (fall)".
- **Por que o SkyCraft nao sofre isso:** ele nao tem timeout de 30 s e exige `known` (`SkyClient.java:276-280`).

O mesmo `holdSince` velho explica os outros tempos:
- "6042 ms (mid-air)" as 09:01:34, so 2 s depois do teleporte das 09:01:32: o hold comecou as ~09:01:28.4, no inicio do loading, junto com "Fantoche: desligado".
- "10352 ms" as 09:08:18, depois do reload das 09:08:06.

### 3.2 A origem do -1033 (fantoche seguindo o respawn do MC)
Fatos:
- `mirror.json`: `min_y=-1024`, `height=2048`. O void mata em -1088.
- `MineWitcher3.java:48`: `IMMEDIATE_RESPAWN=true`. Sem cama, o respawn cai no spawn do mundo, perto do centro do chunk (8.5, ?, 8.5).
- As coordenadas teleportadas (MC 8.63 / -1033.9 / 8.47, depois -1061.6 e -1064.06) estao todas nessa faixa x/z~8.5 e entre min_y e o plano de morte. Sao a posicao do **Geralt** (o `GameState` vem de `thePlayer.GetWorldPosition()`, `mw3_main.ws:154-156`), ja caindo no vazio do W3.

Cadeia:
1. **09:01:26 - morte do MC.**
   - `ServerPlayerMixin` faz `pushEvent(EV_PLAYER_DIED)` (`ServerPlayerMixin.java:39-41`), mas **nenhum codigo nativo chama `Link::PopEvent`** (`link.cpp:314`, sem nenhum chamador em `native/src`).
   - O Geralt nao morre, e `thePlayer.IsAlive()` continua `true` (`mw3_puppet.ws:81`).
2. **Corrida do respawn.**
   - O pacote de respawn e processado dentro do `runTick`, **depois** do `beginFrame` (`MinecraftMixin.java:13-23`).
   - No `afterRender` do mesmo frame, o novo `LocalPlayer` esta no spawn do void, `holdPos == null` e `mc.teleportAck == teleportAck` (`W3Client.java:388`).
   - A DLL ve o ack batendo (`session.cpp:349-350`) e `MW3_PuppetActive` da `true`.
   - Nenhum guarda olha `kMcDead` ou "jogador novo": `MW3_McFlags` existe, mas o fantoche ignora.
3. **O fantoche aceita os pes do spawn.**
   - `IsBelowGround` traca de `max(feet.Z, here.Z)+2` ate `feet.Z-50` em (8.6, -8.5). Nao acha nada (fora do terreno ou nao carregado) e devolve **false** (`mw3_puppet.ws:170-171`), ou seja, "seguro".
   - `TeleportWithRotation` leva o Geralt ~830 m para o vazio (`mw3_puppet.ws:127-134`).
   - No frame seguinte o MC ja se teleportou de volta para -298 (`sky.x` ainda era a posicao velha): "teleported -298... released 0 ms (ground)" as 09:01:27.
4. **09:01:28.4 - loading.**
   - O W3 entra em streaming/blackscreen pelo teleporte gigante: `loading` vira `true` (`IsBlackscreenOrFading`, `mw3_main.ws:149`), e o fantoche desliga ("Fantoche: desligado").
   - `SetPlayerState` marca `g_teleportPending` (`session.cpp:270-274`), e o MC estaciona (`W3Client.java:264-270`) com `holdSince` comecando aqui.
   - O Geralt **cai no vazio** com a fisica do W3 (o fantoche esta desligado e nada o segura).
5. **09:01:32 - fim do loading.**
   - `++g_teleportSeq` com a posicao atual do Geralt, e o MC loga "teleported to 8.63 -1033.90 8.47".
   - O "Camera: Minecraft desconectado" das 09:01:32.554 e um frame em que `RefreshSnapshot` deu `linked=false`. Pode ser leitura rasgada: `ReadMcState` falha depois de 64 tentativas, o MC escreve o `McState` duas vezes por frame (`publishTick` + `afterRender`), e `RefreshSnapshot`, ao contrario da LinkThread (`session.cpp:117-118`), nao segura o ultimo valor (`session.cpp:317-318`). Isso desliga/religa camera e fantoche, mas **nao** e o que produz a coordenada.
6. **09:01:34 - "hold released 6042 ms (mid-air)".**
   - O scan em volta do Geralt no vazio so devolve colunas vazias, e as regioes vazias ficam "conhecidas" (`TRIS.put(region, [])`, `W3Collision.java:424`). Entao `trisKnownAround` da `true`, nao ha chao e, passados 6 s (contando do inicio do loading), o hold solta no ar.
7. **09:01:36 - "Geralt se afastou 2.44 m" (duas vezes).**
   - O Geralt continua caindo no W3 enquanto o MC segura (ack-1, fantoche desligado).
   - O resync de 2 m (`session.cpp:29, 284-297`) teleporta o MC para -1061.6 e depois para -1064.06.
8. **09:01:42 a 09:07:50 - preso no vazio.**
   - O fantoche fica ligado, mas a contagem de colunas mostra o jogador **parado**: +1323 colunas a cada 10 s, que e so o refresh "near" de 3 s, e +13 735 a cada 90 s, o `kMaxAgeMs` (09:03:04 -> 09:03:14, 09:04:34 -> 09:04:44, 09:06:04 -> 09:06:14).
   - O server repete "standing on air" a cada 10 s.
   - Em resumo, o jogador ficou parado no vazio sem chao de verdade. E o "mal consigo andar".

Confianca: **alta** para "o -1033 e a posicao do Geralt no vazio, levado ate la pelo fantoche depois da morte/respawn do MC" (as coordenadas batem com spawn/min_y, o PopEvent nao tem chamador e nao ha guarda de morte). **Media** para a ordem exata entre o frame do respawn e o loading (o log do MC tem resolucao de 1 s).

Descartado:
- **Bug de conversao:** a coordenada pareada (x 8.63 = W3.x, z 8.47 = -W3.y) e coerente com `W3ToMc`.
- **`SendPlayerState` antes do mundo:** antes de `inGame` o pos nao gera teleporte (`session.cpp:268-274`, `W3Client.java:134`).
- **Resync contra zero/stale:** `g_haveLastTeleport` impede comparar com zero, e o gap de 2.44 m e real (queda do Geralt).

### 3.3 Por que as regioes param em 100 / 6349 triangulos
- Isso **nao** e cap, dedupe com bug nem anel cheio: a fila esta em 0 e nao aparece "anel ficou cheio".
- **38 -> 100 regioes com os triangulos constantes em 6349:** as 62 colunas de regiao novas foram mandadas **vazias**. E o Geralt no vazio, com o scan centrado nos pes dele (`mw3_collision.ws:91-92`, faixa `py-32..py+24`, `collision.cpp:42-43, 674-675`).
- Raio de 32 blocos -> ~100 colunas de regiao 8x8. Com o jogador parado, tudo ja foi mandado uma vez.
- Os re-traces dao amostras identicas (`SameSample` -> sem `MarkDirty`, `collision.cpp:734-740`), entao nada mais e reenviado. As "colunas tracadas" sobem e as "regioes enviadas" nao.
- Depois do reload (09:08), o Geralt volta para -298 e as regioes voltam a crescer (129 -> 257, 20 682 -> 89 489 triangulos). Confirmado.

**Efeito colateral grave: a eviction.**
- No vazio, as 38 regioes reais em volta de -298 ficaram a mais de 56 blocos e foram "esquecidas" por `EvictFar`, que manda `BuildEmptyPayload` como `kColTris` + `kColRegion` (`collision.cpp:124-133, 548-573`).
- O proprio comentario admite: *"Minecraft drops what it had there (eviction) and still counts the region as known"* (`collision_mesh.h:95-97`).
- No reload, sem epoca nova (mesmo mundo `bob`, mesmo pid; `session.cpp:247-248`, `collision.cpp:618-623`), o MC tem -298 como "conhecido e vazio". `trisKnownAround` da `true`, `groundAt` da NaN, e depois de 6 s vem o "mid-air" (09:08:18, com a fila ainda em 27 jobs as 09:08:17) e a queda.
- O mesmo "conhecido e vazio" engana o `W3Collider.collide` (`W3Collider.java:35-40`), que deixa andar e cair para dentro dessas regioes.
- O SkyCraft nao manda eviction ao MC: so apaga a propria contabilidade (`SC: Collision.cpp:284-285, 323-325`), e as regioes conhecidas no MC sempre tem a geometria real.

### 3.4 O loop de mortes (09:08)
1. **09:08:18** - "hold released 10352 ms (mid-air)": regioes stale-vazias + `holdSince` velho (3.1/3.3). O MC cai: "pes do MC z=23.15 abaixo do chao; Geralt parado".
2. **09:08:23** - "died (fall)". O respawn passa pela mesma corrida de 3.2. Desta vez o teleporte de volta pega chao: "placed 0.013 / released 0 ms".
3. **09:08:29** - teleporte para -294.67, 25.16: o loading/fade de novo estacionou o MC (`W3Client.java:264-270`). O hold solta "6069 ms mid-air" e o MC afunda: "pes do MC z=22.50 abaixo do chao", as 09:08:30.68.
4. **Fantoche bloqueado e sem resync.** `IsBelowGround` sai antes de `MW3_PuppetMoved` (`mw3_puppet.ws:116-122`). O `g_lastTeleport` da DLL fica igual a posicao do Geralt e o gap fica em ~0 (`session.cpp:284-288`). **Nada teleporta o MC de volta**, e ele cai ate morrer as 09:08:37. O "afastou 2.0 m" das 09:08:37.6 so aparece depois da morte.
5. **09:08:45 e 09:08:53** - as mesmas mortes, ate o W3 fechar.

O SkyCraft cobre o equivalente com `holdMismatch` (`Game.cpp:624-636`), e la o fantoche nunca fica "bloqueado".

### 3.5 Outras divergencias relevantes (secundarias)
| Tema | SkyCraft | Nos | Efeito |
|---|---|---|---|
| Limiar de resync | 300 unidades (~4.3 blocos) contra o `SetPosition` sincrono (`Game.cpp:19, 582`) | 2.0 m contra o alvo do `TeleportWithRotation` e a queda pela gravidade do W3 (`session.cpp:29`) | Resyncs falsos ("Geralt-drift"), cada um vira um hold novo |
| Ramo silencioso | O resync sempre loga | `else if (gap > 2)` sem MC linkado ainda incrementa o seq em silencio (`session.cpp:293-296`) | Teleportes sem log |
| Velocidade/queda do fantoche | Zera velocidade, `fallStartHeight`, estado on-ground (`Game.cpp:791-808`) | Teleporte puro; com o fantoche desligado ou bloqueado o Geralt cai com a fisica do W3 | Drift e queda no vazio |
| Settle depois de load/teleporte | 1.5 s sem colher colisao (`Game.cpp:93-96, 997`) | O scan comeca no primeiro frame sem loading (`mw3_main.ws:113-114`) | Colunas tracadas antes do streaming saem vazias e viram "conhecidas" |
| Criterio de soltar o hold | Voxels known em y-1, y, y-8 + solido ate 12 blocos ou 6 s; sem timeout | Triangulos known + chao a 0.5 bloco, ou 6 s mid-air, ou **timeout de 30 s** (`W3Client.java:283-288`) | Solta no ar com facilidade |
| Morte | `kEvPlayerDied` -> `KillPlayer`; `IsDead` desliga o fantoche | O evento nao e consumido; o fantoche nao olha `kMcDead` | 3.2 |
| Interpolacao | Historico de ticks + render delay (`Game.cpp:662-786`) | Pes do frame do MC com partial (`W3Client.java:340-341`, `session.cpp:329-332`) | So suavidade, nao e a causa |
| Leitura rasgada | `haveMc=false` desliga o fantoche num frame (igual) | `RefreshSnapshot` derruba `linked` e loga "desconectado" | Flapping de camera/fantoche |

---

## 4. Correcoes, por prioridade

1. **Zerar o timer do hold a cada hold novo e repensar o timeout.**
   - **Onde:** `fabric/.../client/W3Client.java`.
   - **O que mudar:**
     - Em `beginFrame`, no bloco do `requestTeleport` (l.134-139), fazer `holdSince = System.currentTimeMillis()`.
     - Ao estacionar em menu/loading (l.264-270), tambem reiniciar o `holdSince` quando sair do loading (ou contar so com `inGame && !loading`).
     - Tirar o `timedOut` de 30 s (l.287), ou faze-lo soltar so se o W3 informar que nao ha chao sob o Geralt. Melhor ainda: reenviar o teleporte em vez de soltar.
   - **Confianca:** alta. Explica 300777 ms, 6042 ms e 10352 ms.
2. **Ligar a morte do MC ao W3 e fechar a janela do respawn.**
   - **Nativo:** drenar o anel de eventos em todo frame, como `SC: Combat.cpp:848-866`. Por exemplo, uma native `MW3_PopEvent` chamada em `TickLinked`/`SendLinkState`, ou drenar em `session::RefreshSnapshot`. Ao chegar `kEvPlayerDied`, matar o Geralt (`thePlayer.Kill(...)`) ou ao menos travar o fantoche ate o proximo ack.
   - **Script** (`mw3_puppet.ws:81`): desligar o fantoche quando `MW3_McFlags() & 32` (`kMcDead`).
   - **Fabric** (`W3Client.java:388`): publicar `teleportAck - 1` enquanto `player != lastPlayer` ou `teleportPending`. Isso cobre o frame em que o pacote de respawn chega depois do `beginFrame`.
   - **Opcional:** fixar o spawn do mundo e do respawn na ultima posicao segura, para o respawn nunca cair no vazio.
   - **Confianca:** alta.
3. **Sanidade no fantoche** (`mw3_puppet.ws:112-136, 159-173`).
   - **O que mudar:**
     - Rejeitar pes do MC a mais de ~4-8 m do Geralt num unico frame (salto impossivel a pe). Nesse caso, nao mexer no Geralt e pedir resync.
     - Em `IsBelowGround`, tratar "o trace nao achou nada" como **inseguro** (bloquear), e nao como livre. Usar o mesmo conjunto de grupos do scan (`mw3_collision.ws:23-31`) em vez de so Terrain/Static.
   - **Confianca:** alta. Foi o que deixou o Geralt ir para (8.6, -8.5).
4. **Eviction sem "conhecido e vazio".**
   - **O que mudar:**
     - Nativo: `collision.cpp:548-573` (e `RunJob` em l.124-133). Ou fazer como o SkyCraft (so apagar `st.sent`, sem mandar nada ao MC), ou criar `kColForget` em `protocol/mw3_protocol.h`.
     - Java: com `kColForget`, `W3Collision.java` passa a remover `TRIS`, `SHAPES`, `FILL`, `KNOWN_REGIONS` e `GHOSTS` da regiao.
     - Atualizar o comentario em `collision_mesh.h:95-97`.
   - **Confianca:** alta. Explica o "mid-air" das 09:08:18 com a fila cheia.
5. **Resync quando o fantoche esta bloqueado e mismatch de hold (como o SkyCraft).**
   - **O que mudar:**
     - Com o fantoche em `blocked` por mais de ~0.5 s, pedir teleporte do MC para o Geralt. Pode ser uma native nova `MW3_RequestResync()` que seta `g_teleportPending`, ou continuar chamando `MW3_PuppetMoved` com a posicao **do MC**, para o gap aparecer.
     - Na DLL, portar o `holdMismatch`: se `teleportAck != seq` e os pes do MC estiverem a mais de 8 blocos do Geralt por mais de 1 s, reenviar o teleporte (`SC: Game.cpp:624-636`, encaixando em `session::RefreshSnapshot`/`SetPlayerState`).
   - **Confianca:** alta para o loop 09:08:30-37.
6. **Settle depois de loading e teleporte grande.**
   - **O que mudar:** em `collision::Begin` ou no script, nao tracar por ~1.5 s depois de loading ou de um teleporte maior que ~50 m (`SC: Game.cpp:93-96, 997`). Colunas sem hit nos primeiros frames depois de streaming deviam ser re-tracadas antes de virar "conhecidas".
   - **Confianca:** media.
7. **Limiar e ramo silencioso do resync** (`session.cpp:29, 284-297`).
   - **O que mudar:**
     - Subir o limiar para ~4.3 m (o equivalente do SkyCraft).
     - Nao contar gap enquanto o MC segura (ack != seq) nem enquanto o fantoche esta bloqueado: o Geralt caindo nao e "o W3 moveu".
     - Logar tambem o ramo `!Linked()`, ou nao incrementar o seq nele.
     - No loading, zerar `g_haveLastTeleport` como o SkyCraft (`Game.cpp:580`).
   - **Confianca:** media.
8. **Leitura rasgada nao derruba o link** (`session.cpp:313-344`).
   - **O que mudar:** se `ReadMcState` falhar com `McAlive()` verdadeiro, manter o ultimo snapshot e o `linked` (histerese de alguns frames). Opcionalmente, o Fabric pode publicar so uma vez por frame.
   - **Confianca:** media. Explica o "desconectado" de 1 frame.
9. **Prender o Geralt quando o fantoche nao dirige mas o MC segura.**
   - **O que mudar:** o SkyCraft deixa a fisica do Skyrim segurar o corpo, e no chao real isso basta. No nosso caso, o Geralt caiu porque estava no vazio, entao isso vira irrelevante depois do item 3. Mesmo assim vale investigar zerar velocidade e queda no W3 (ex.: `ApplyFallingDamage` ja esta embrulhado) e manter o Geralt no ultimo ponto valido enquanto `ack != seq`.
   - **Confianca:** baixa/media.
10. **Criterio de soltar mais proximo do upstream** (`W3Client.java:283-286`).
    - **O que mudar:** exigir regiao conhecida em y-1, y e y-8 e "algum chao ate 12 blocos" (`SC: SkyClient.java:276-280`) antes do mid-air de 6 s.
    - **Confianca:** media.
11. **Interpolacao dos ticks como o SkyCraft** (`SC: Game.cpp:662-786`). So suavidade.
    - **Confianca:** baixa como causa.

## 5. Confianca geral
- **Alta:** timer velho do hold; fantoche seguindo o respawn por falta de acoplamento de morte e de sanidade; eviction "conhecida-vazia"; fantoche bloqueado sem resync.
- **Media:** ordem exata dos frames entre 09:01:26 e 09:01:32; causa do frame "desconectado".
- **Descartados:** bug de conversao de coordenadas; cap/anel cheio no envio de colisao.

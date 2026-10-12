# fabric/ — mod Fabric do MineWitcher3 (Minecraft 26.3, Fabric Loader 0.19.5)

Lado Minecraft do MineWitcher3: um cliente Minecraft escondido que roda junto com o The Witcher 3 e
conversa com o `mw3.asi` pela memória compartilhada `Local\MineWitcher3_v1` (layout em
`../protocol/mw3_protocol.h`). O MC faz física, blocos, inventário, combate e câmera F5; o jogo
desenha tudo e manda input, posição e colisão.

**Quase todo o código vem do SkyCraft** (https://github.com/chasmlol/skycraft, commit `bfcaf17`,
MIT, Copyright (c) 2026 chasmlol). Cada arquivo Java tem o cabeçalho
`// Derived from SkyCraft (...) MIT License.` e o texto completo da licença está em
`../THIRD-PARTY-NOTICES.md` (também vai dentro do `.jar`).

## Build

Precisa de **JDK 25** (Loom 1.18 e o MC 26.x exigem). Se o seu `%USERPROFILE%\.gradle\gradle.properties`
fixar o Gradle em outro JDK (`org.gradle.java.home`), use o atalho, que força o JDK 25 só para este build:

```bat
cd fabric
.\gradle25.cmd build
```

Ele acha o JDK 25 em `C:\Program Files\Eclipse Adoptium\jdk-25*` ou
`%LOCALAPPDATA%\Programs\Temurin\jdk-25*` (ou `MW3_JDK=<pasta>`) e passa
`-Dorg.gradle.java.home` para o wrapper (Gradle 9.7.1). Saída: `build/libs/minewitcher3-0.1.0.jar`.
Os testes JUnit do colisor (`TriColliderTest`, `W3RayTest`, 21 testes) rodam no `build`.

Não rode `runClient` apontando para a instalação real do `.minecraft`. Para testar sem o Witcher 3,
use o stand-in `python ../tools/fake_witcher.py` (cria a memória compartilhada e controla o MC) e
`python ../tools/fake_witcher.py --check` (confere magic/versão/offsets entre o header, o `Proto.java`
e o script).

Propriedades da JVM (o launcher do `mw3.asi` deve passar as duas primeiras):

| Propriedade | Efeito |
|---|---|
| `--enable-native-access=ALL-UNNAMED` | obrigatório: o link usa Java FFM (`OpenFileMappingW`/`MapViewOfFile`) |
| `-Dminewitcher3.startHidden=true` | esconde a janela e a música desde o primeiro frame |
| `-Dminewitcher3.showWindow=true` | depuração: não esconde a janela |
| `-Dminewitcher3.quitWithGame=false` | não fecha o MC quando o `witcher3.exe` fecha |
| `-Dminewitcher3.link=Local\MineWitcher3_guest` | outro nome de mapeamento (segundo cliente no mesmo PC) |
| `-Dminewitcher3.damageScale=N` | dano do W3 ÷ N = dano no MC (padrão 100, chute) |

## O que foi portado

Port mecânico feito por `tools/port_from_skycraft.py` (reexecutável: rode numa pasta vazia e faça
diff com esta para trazer mudanças do upstream) + poucas edições à mão.

- Pacote `dev.skycraft` → `dev.minewitcher3`, mod id `skycraft` → `minewitcher3`, configs
  `minewitcher3.mixins.json` / `minewitcher3.client.mixins.json`, recursos `data/minewitcher3/...`
  (dimension type `minewitcher3:mirror`, preset de mundo vazio), prefixo de Mixin `skycraft$` → `mw3$`.
- Classes renomeadas: `SkyCraft`→`MineWitcher3`, `SkyCraftClient`→`MineWitcher3Client`,
  `SkyLink`→`WitcherLink`, `SkyClient`→`W3Client`, `SkyCollider`→`W3Collider`,
  `SkyCombat`→`W3Combat`, `SkyrimActorEntity`→`WitcherActorEntity` (entidade `minewitcher3:witcher_actor`),
  `SkyCollision`/`SkyClip`/`SkyRay`/`SkyTri`/`SkyWater`/`SkyDig`/`SkyDigBlast`/`SkyDigClient`/`SkyAtlas`/`SkyNet`
  → `W3*`. `TriCollider`, `InputBridge`, `FrameExporter`, `MirrorWorld`, `ProxySync`, `WorldExporter`,
  `AvatarExporter`, `BlockLightColors`, `DigWalls`, `DestructionToggle` mantêm o nome.
- Protocolo: `Proto.java` espelha `mw3_protocol.h` (magic `MW3C` = `0x4333574D`, versão 1,
  `UNITS_PER_BLOCK = 1.0`). Layout de bytes **idêntico** ao SkyCraft v11; só nomes mudaram:
  `SkyState`→`GameState` (`OFF_GAME_STATE`, `GS_*`, `GAME_IN_GAME/MENU_OPEN/LOADING`),
  `H_SKYRIM_PID/HEARTBEAT`→`H_GAME_PID/HEARTBEAT`, `formId`→`actorId`,
  `HURT_BLOCKED_IN_SKYRIM`→`HURT_BLOCKED_IN_GAME`, `HURT_POWER_ATTACK`→`HURT_HEAVY_ATTACK`.
- Todos os Mixins genéricos (lista em `research/05-skycraft-reuse.md` §3.1): 16 comuns e 14 de
  cliente, mais os 7 accessors, com os mesmos alvos e pontos de injeção do upstream.
- Comentários e logs: "Skyrim" virou "the game" de forma mecânica.

## O que foi removido

| Removido | Por quê |
|---|---|
| `SmithingMixin` | treinava a skill Smithing do Skyrim |
| `trainDefence` em `W3Combat` + `EV_SKILL_USE` / `SKILL_*` | skills Block/Light/Heavy Armor do Skyrim; o evento 5 fica reservado no protocolo |
| `DiscordPresence` + `ChatComponentMixin` | usavam o Application ID do SkyCraft no Discord (apareceria como "SkyCraft"); o mixin só alimentava isso |
| `tools/fake_guest.py` (não portado) | stand-in do segundo cliente para testar multiplayer |

Mantidos mesmo sendo opcionais: multiplayer (`W3Net`, `/join`, `/leave`, e4mc só no `runClient`),
escavação (`W3Dig*`, `DigWalls`, `DestructionToggle`, ligada por padrão em
`config/minewitcher3.properties`).

## O que ainda tem forma de Skyrim e precisa de adaptação ao W3

1. **Escala de dano** (`W3Combat.GAME_TO_MC_DAMAGE`): Skyrim usava ÷5; o W3 tem HP na casa dos
   milhares. O padrão 100 é chute; calibrar no jogo. `HURT_MAGIC` deve virar sinais/monstros mágicos;
   `HURT_HEAVY_ATTACK` precisa ser preenchido pelo lado nativo (ataque forte do W3).
2. **IDs de ator**: `ActorRecord.actorId` precisa ser um id estável de entidade do W3 (o Skyrim
   usava FormID). `level` vem do nível do NPC no W3. Nome em UTF-8 (24 bytes).
3. **`worldId` e `collisionEpoch`**: hash FNV-1a do nome do mundo do W3 (`WorldIdFromName`). O W3
   não tem "células" de interior como o Skyrim; decidir quando trocar o epoch (troca de mundo, fast
   travel, talvez entrar em interiores grandes). A escavação (`W3Dig`) guarda os buracos por `worldId`.
4. **Escavação do terreno** (`W3Dig*`, `DigWalls`, `RenDug`, `ColTri` com `kTriDiggable`/material):
   pressupõe que o lado nativo consegue esconder a geometria do jogo nos blocos cavados (o SkyCraft
   recortava o terreno do Skyrim). No REDengine o terreno é heightmap/clipmap: talvez inviável. Se
   não der, desligar (`destruction=false`) ou remover.
5. **`DigMaterial`**: valores (grama, neve, cinza, troncos…) são do lado MC e genéricos, mas o
   mapeamento a partir dos materiais físicos do W3 (PhysX) ainda não existe.
6. **Ragdoll** (`REN_RAGDOLL`, `AvatarExporter.exportRagdoll`): o Skyrim pendurava as partes no
   ragdoll Havok quando o jogador morria. No W3 o lado nativo pode ignorar a mensagem.
7. **Água** (`W3Water`, `WaterGrid` 16×16): o lado nativo precisa amostrar o nível de água do W3.
8. **Colisão**: o formato (voxels 8³ + triângulos) é genérico, mas os comentários falam de Havok,
   "stair helpers" e rampas invisíveis do Skyrim. No W3 a fonte é PhysX/`StaticTrace` (ver
   `research/04`). `REGION_SIZE = 8` tem que bater com o lado nativo.
9. **Câmera e yaw**: o MC recebe yaw/pitch já integrados pelo jogo. O sinal/offset do yaw do W3
   ainda não foi confirmado (ver `mw3_protocol.h`, seção "coordinates"). Conversão de FOV vertical do
   MC para o W3 fica no lado nativo.
10. **Input**: o jogo deve mandar scancodes SDL (o Skyrim convertia DirectInput → SDL). No W3 a
    captura é por Raw Input/WndProc, então falta uma tabela VK/scan → SDL no `mw3.asi`.
11. **Tempos**: heartbeat 8 s, `holdUntilReady` 6 s e fechar o MC 5 s depois do PID do jogo sumir
    foram calibrados para o Skyrim; revisar com as telas de loading do W3.
12. **Kit inicial** (`MineWitcher3.giveStarterKit/giveBuilderKit`) e `dressTestGuest`: iguais ao upstream.

## Pendências conhecidas

- **Nada foi executado em runtime** (nem `runClient`, nem com o W3). Os Mixins usam
  `defaultRequire: 1`: se algum alvo não existir no 26.3 o MC fecha na carga. Os alvos são os mesmos
  do SkyCraft 0.1.2, que mira exatamente o 26.3.
- A instalação do usuário é `iris-fabric-loader-0.19.5-26.3` (Iris/Sodium). O SkyCraft roda numa
  instância limpa (Fabric API + e4mc). Sodium/Iris substituem partes do `LevelRenderer` e da
  compilação de seções, o que pode conflitar com `LevelRendererMixin`/`LevelExtractorMixin` e com o
  `WorldExporter`. Recomendado: instância separada (Prism, como o SkyCraft) sem Iris/Sodium.
- O launcher do lado nativo ainda não existe (`native/` é de outro agente).

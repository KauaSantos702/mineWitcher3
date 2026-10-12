# 08 - Lag do servidor integrado do MC ("Can't keep up! ... 20285ms")

Análise read-only. Fontes: `research/field/mc-20261012-0910.log` (abreviado **MC**), `research/field/mw3-20261012-0910.log` (**MW3**), logs antigos da instância (`Prism/instances/MineWitcher3/.minecraft/logs/2026-10-11-{1..4}.log.gz`), código em `fabric/src`, bytecode do MC 26.3 (`minecraft-common-deobf-26.3.jar`, lido com `javap`) e SkyCraft (`git pull`: já atualizado, HEAD `bfcaf17`).

## TL;DR

1. **O servidor não fica lento o tempo todo. Ele trava em picos de 14 a 20 s, e cada pico começa logo depois de uma morte ou respawn do jogador.** No respawn sem cama, o MC 26.3 chama `ServerPlayer.adjustSpawnLocation`. Esse método roda o `PlayerSpawnFinder.findSpawn` e **bloqueia a server thread** (`MinecraftServer.managedBlock(future::isDone)` + `join()`) enquanto procura um lugar seguro: até `(2*spawnRadius+1)^2 = 441` candidatos, um por vez, cada um com um ticket de chunk e uma volta pelo executor do servidor. Num mundo void nenhum candidato serve, então os 441 são testados e no fim ainda vem um `fixupSpawnHeight`, que percorre a coluna inteira de 2048 blocos com `noCollision`. Isso bate com os 20 s e os 14 s do log.
2. As mortes acontecem por falha nossa: o hold solta o jogador depois de 30 s sem colisão do W3, ele cai e morre de "fall". Depois do respawn ele aparece no spawn do mundo void (8.5, ~-1024, 8.5), o W3 copia essa posição (o Geralt é puppet do MC) e manda de volta um teleporte para Y -1033 a -1064, abaixo do `min_y`. A partir daí o ciclo se repete.
3. O "mal consigo andar" tem três causas que se somam: (a) depois de cada morte, o player do cliente fica parado até o servidor terminar o respawn (`LocalPlayer.tick` só roda quando `ClientPacketListener.hasClientLoaded()` é verdadeiro); (b) cada teleporte pedido pelo W3 (distância > 2 m) gera um hold de **6 s** quando o jogador não está a 0.5 bloco do chão; (c) os `teleportTo` feitos pelo servidor chegam atrasados quando ele está travado e puxam o jogador de volta.
4. O que foi **descartado** com base nas evidências: GC, autosave, volume de colisão, spawn de mobs, FrameExporter e falta de CPU contínua (ver §1.3).

---

## 1. Por que o servidor fica 20 s atrasado

### 1.1 Linha do tempo (o log do servidor serve de relógio de ticks)

`ServerGamePacketListenerImpl.forceSendPlayerSupportBlocks()` escreve "Player X standing on air - force-sending blocks below" no máximo uma vez a cada 200 ticks do listener (bytecode: `tickCount - lastSupportedBlocksSend`). Com o servidor a 20 TPS, a mensagem aparece **a cada 10 s exatos**. Um intervalo maior que 10 s significa que o servidor parou.

| Trecho (MC log) | Evento | Intervalo entre "standing on air" | Leitura |
|---|---|---|---|
| L122-123 09:01:24 | `released after 30 s without the game's collision`, hold de 300 s terminou por timeout | | jogador solto sem colisão |
| L124-126 09:01:26 | `player died (fall)` | | **morte, depois respawn imediato (IMMEDIATE_RESPAWN)** |
| L131-134 09:01:32-36 | `teleported to 8.63 -1033.9 8.47` e depois -1061, -1064 | | posição de spawn do mundo (chunk 0,0) **abaixo do min_y -1024**, copiada pelo W3 |
| L136 09:01:47 | `Can't keep up! ... 20285ms` | 09:01:26 a 09:01:56 = **30 s** | servidor ~20 s parado depois da morte |
| L137-171 09:01:56 a 09:07:36 | "standing on air" a cada 10 s | **10 s fixos** | **20 TPS estáveis por 6 minutos** |
| L177 09:08:23 | `player died (fall)` | | morte e respawn |
| L185-188 09:08:37 | outra morte, depois `Can't keep up! ... 14409ms` | | ~14 s parado depois da morte das 09:08:23 |
| logs antigos `-3.log.gz` L127-130 | `2102ms` logo depois do 1o teleporte longo após o join | | carga de chunks no destino (pico menor) |

Durante o travamento de 09:01:26 a 09:01:47, a **Render thread continuou escrevendo no log** (L128-135 em :27, :32, :34, :36, :42). Logo o JVM não estava em pausa global: só a **Server thread** estava bloqueada.

### 1.2 Mecanismo (bytecode MC 26.3)

- `PlayerList.respawn(...)` chama `ServerPlayer.findRespawnPositionAndUseSpawnBlock`. Sem cama, cai em `TeleportTransition.createDefault` e depois em `findAdjustedSharedSpawnPos`, que chama `Entity/ServerPlayer.adjustSpawnLocation(level, respawnData.pos)`.
- `ServerPlayer.adjustSpawnLocation` chama `PlayerSpawnFinder.findSpawn(...)`, depois **`MinecraftServer.managedBlock(BooleanSupplier)`** e por fim `CompletableFuture.join()`. A server thread fica presa até a busca acabar.
- `PlayerSpawnFinder.findSpawn`: em ADVENTURE vai direto para `fixupSpawnHeight`. Nos outros modos usa `radius = max(0, gamerule RESPAWN_RADIUS)` (padrão 10), limitado pela borda do mundo, e testa `candidateCount = (2r+1)^2 = 441` candidatos **em sequência** (`scheduleNext` chama `scheduleCandidate`, que chama `ServerChunkCache.addTicketAndLoadWithRadius(...)` e depois `whenCompleteAsync(..., server)`). No void, `getLevelRespawnPos` não acha bloco nenhum, então todos falham. O fallback `fixupSpawnHeight` sobe e desce com `noCollisionNoLiquid` até `getMinY()`, ou seja, varre os 2048 blocos da altura (`dimension_type/mirror.json`: `height 2048`, `min_y -1024`). Cada `noCollision` passa pelo nosso `BlockCollisionsMixin` e pelo `W3Dig.wallShape` (que consulta chunks até 5 vezes por bloco de ar, `W3Dig.java:660-703`).
- 20 s divididos por 441 candidatos dão uns 45 ms por candidato. Cada candidato precisa processar o ticket e esperar uma tarefa no executor dentro de `managedBlock`. Os workers de chunk competem com o W3 pela CPU. O número é coerente.
- É por isso que o spawn termina em (8.6, ~-1024, 8.5): o centro do chunk (0,0) no fundo do mundo.

**Confiança: alta** que os picos vêm do respawn bloqueante (a correlação morte/pico é 2 de 2, e o mecanismo foi confirmado no bytecode). **Média** sobre a distribuição exata do tempo entre os 441 tickets e a varredura de 2048 blocos. Para confirmar: `jcmd <pid> Thread.print` durante um travamento deve mostrar `Server thread` dentro de `PlayerSpawnFinder` / `managedBlock`. Também dá para usar o mod spark, ou o `tools/tick_monitor.py` do SkyCraft.

### 1.3 Candidatos descartados ou secundários

| Candidato | Veredito | Evidência |
|---|---|---|
| Mixin de colisão por entidade/tick | Secundário | Fora dos respawns o servidor roda a 20 TPS cravados por 6 min (MC L137-171). O mixin (`BlockCollisionsMixin.java:31-50`) faz só uma consulta em hash. O custo real é `W3Dig.wallShape` para cada bloco de ar (`getChunkForCollisions` até 5 vezes), e esse custo é multiplicado pela varredura de 2048 blocos do respawn. |
| Volume de colisão | Descartado | MW3 L85-98: 100 regiões 8x8 e 6349 triângulos, fila 0. Pouco dado, e é processado na thread `MineWitcher3 collision` (`W3Collision.java:246-305`), fora da server thread. |
| Worldgen/chunks | Contribui (pico de 2 s) | Teleporte longo (log -3: 2102 ms). A view distance 8 dá cerca de 17^2 a 19^2 chunks de **128 seções** (altura 2048) para gerar e enviar a cada teleporte longo. |
| Mobs/proxies | Descartado | `MineWitcher3.java:41-45`: SPAWN_MOBS/MONSTERS/PHANTOMS/PATROLS/TRADERS = false. Proxies são poucas entidades. |
| Autosave (6000 ticks = 5 min) | Descartado | Teria caído em ~09:06:22, e a cadência de 10 s continua intacta ali (MC L165-167). |
| GC (heap 3 GB) | Descartado como causa | A render thread continuou logando durante o travamento (não foi pausa global). O log não tem OOM nem aviso de memória. Não há log de GC, então convém ligar (§3). |
| W3 roubando CPU | Agravante | Não explica 20 s sozinho (o resto do tempo fica a 20 TPS). Mas deixa mais lentos os workers de chunk que o `managedBlock` espera. |
| FrameExporter/readback | Descartado | Roda na render thread e é assíncrono (`FrameExporter.java:92`: `copyTextureToBuffer` com callback, 3 staging buffers). |
| Datafixer | Descartado | Só no boot (MC L55, 875 ms). |
| view/simulation distance | Agravante | 8/8 (MC L109-110, `W3Client.java:436-437`, `options.txt`). |

### 1.4 Comparação com o SkyCraft

O SkyCraft usa **a mesma configuração**: o mesmo `dimension_type` (2048/-1024), as mesmas gamerules (`SkyCraft.java:38-48` é igual a `MineWitcher3.java:39-49`), as mesmas opções (`SkyClient.java:425-438`: pauseOnLostFocus=false, vsync off, 260 fps, render/sim 8) e o mesmo `requestTeleport` (`SkyClient.java:307-326`). A única diferença no bundle é **MaxMemAlloc=4096** (`tools/minecraft-bundle/Prism/instances/SkyCraft/instance.cfg`) contra 3072 no nosso. O SkyCraft **não evita** o problema do respawn. Ele só não o dispara, porque lá a colisão do Skyrim chega e o jogador não morre caindo no void.

## 2. O movimento do cliente depende do servidor?

Na maior parte, não: o `LocalPlayer` simula o próprio movimento e manda a posição ao servidor, e com `PLAYER_MOVEMENT_CHECK=false` (`MineWitcher3.java:46`) o servidor aceita. **Mas aqui há três dependências que travam o jogador:**

1. **Respawn/login:** `LocalPlayer.tick()` só roda se `ClientPacketListener.hasClientLoaded()` for verdadeiro (bytecode). Depois de morrer, o novo player só existe e só anda quando o servidor termina o `respawn`. Isso dá 14 a 20 s parado por morte, e houve 5 mortes em 7 minutos (MC L125, 177, 185 e seguintes).
2. **Teleportes vindos do servidor:** `W3Client.requestTeleport` (`W3Client.java:310-329`) faz `server.execute(() -> sp.teleportTo(...))`. Com o servidor atrasado, o teleporte é aplicado segundos depois e manda `ClientboundPlayerPositionPacket` para um destino **velho**. O cliente é puxado de volta (rubber-band), e o servidor ignora os move packets até o cliente confirmar o teleporte.
3. **Chunks:** o cliente só faz tick de entidades em chunks que recebeu. Depois de um teleporte longo (respawn em 0,0 e volta para -298,867), o jogador espera o servidor enviar os chunks de 128 seções.

Tem também uma causa **só do cliente** que é provavelmente a mais frequente: **o hold de 6 s "mid-air"**. Cada teleporte define `holdPos`, e o release só acontece com chão a até 0.5 bloco dos pés ou depois de **6000 ms** (`W3Client.java:282-287`). O nativo pede teleporte sempre que o Geralt se afasta mais de **2.0 m** (`native/src/session.cpp:29`, `kResyncDistance`, e L284-291). O MC fica com os pés abaixo do chão do W3 (MW3 L80, L135: "pes do MC abaixo do chao do W3"). Resultado no log: holds de 6042, 6040, 6069, 6050, 6015 e 10352 ms (MC L132, 135, 175, 184 e seguintes). **O jogador fica congelado 6 s a cada poucos segundos.**

**Como o SkyCraft evita isso:** não evita de propósito. O código é idêntico. Lá a colisão chega a tempo, o jogador fica no chão (então o release é "ground", em 0 ms) e não morre caindo no void.

## 3. Correções, da mais para a menos impactante

| # | Impacto | Onde | Mudança | Confiança |
|---|---|---|---|---|
| 1 | **Muito alto** (acaba com os picos de 14-20 s) | novo mixin `fabric/src/main/java/dev/minewitcher3/mixin/ServerPlayerSpawnMixin.java` + `minewitcher3.mixins.json` | `@Inject(method="adjustSpawnLocation", at=@At("HEAD"), cancellable=true)` em `ServerPlayer`, devolvendo o `BlockPos` recebido sem busca nenhuma. O cliente já teleporta para a posição do W3 logo depois. Opcional: guardar a última posição boa do W3 e usá-la como `respawnData`. | Alta |
| 1b | Alto (mitigação de uma linha) | `MineWitcher3.java:configureServer` (L37-52) | `rules.set(GameRules.RESPAWN_RADIUS, 0, server);` reduz 441 candidatos para 1. A varredura de 2048 blocos do `fixupSpawnHeight` continua, então é bom combinar com o #1. | Alta |
| 2 | **Alto** (acaba com as mortes que disparam tudo) | `W3Client.java:286-298` | No `timedOut`, **não soltar em queda livre**: manter o hold, ou soltar com `abilities.flying`/sem gravidade até `trisKnownAround`. Hoje o timeout de 30 s solta o jogador sem colisão (MC L122) e ele morre na sequência (L125). | Alta |
| 2b | Alto | `MineWitcher3.java` ou um mixin de dano | Ignorar dano de queda e de void quando a queda começou sem colisão do W3 conhecida, ou quando Y < `min_y`. Hoje toda morte do MC também **mata o Geralt** (`ServerPlayerMixin.die`, logo "telling the game"). Alternativa mais drástica: `GameRules.FALL_DAMAGE=false`, se o W3 já cuida da queda. Decisão de design. | Média |
| 3 | **Alto** para "não consigo andar" | `W3Client.java:282-287` | Reduzir a espera "mid-air" de 6000 para ~500-1000 ms, ou soltar assim que `trisReady` (e deixar a física vanilla assentar). Além disso, não reaplicar o hold em teleportes curtos de resync (distância < ~4 m). | Alta |
| 3b | Médio-alto | `native/src/session.cpp:29, 284-296` e `mod/.../mw3_puppet.ws` | Histerese no resync: `kResyncDistance` 2.0 é pouco. Usar ~4-6 m, ou ignorar quando o MC está morto, abaixo de `min_y`, ou deu um salto > 50 m (respawn). Assim o Geralt não segue o MC até o void (MC L131-134). | Média-alta |
| 4 | Médio | `W3Client.java:310-329` | Não chamar `sp.teleportTo` quando o servidor está atrasado. Com PLAYER_MOVEMENT_CHECK off, o move packet do cliente já leva a posição. Ou chamar `teleportTo` só em saltos grandes (> 32 blocos), para evitar puxar o jogador de volta com um destino velho. Testar, porque o SkyCraft faz igual. | Média |
| 5 | Médio | `W3Client.java:436-437` (+ `options.txt`) | `renderDistance` e `simulationDistance` de 8 para **4** (ou 5). O MC não desenha o mundo. 9^2 = 81 chunks em vez de 17^2 = 289, cerca de 3.5x menos chunks de 128 seções por teleporte longo. Conferir se flechas e blocos colocados a até 64 blocos ainda importam. | Média |
| 6 | Médio-baixo | `Prism/instances/MineWitcher3/instance.cfg` (via `tools/setup_minecraft.ps1`) | `MaxMemAlloc=4096` (igual ao SkyCraft), `MinMemAlloc=4096` (sem crescer o heap), e nos JvmArgs `-XX:+UseZGC -XX:+AlwaysPreTouch -Xlog:gc*:file=logs/gc.log:time,uptime:filecount=3,filesize=10M`. O log de GC é para **ter a evidência**: hoje não dá para medir pausas. | Média (diagnóstico), baixa (como correção) |
| 7 | Baixo-médio | `W3Dig.java:670-690` | Saída rápida em `wallShape`: um contador global de colunas cavadas, e se for 0 retorna `null` antes de chamar `getChunkForCollisions` 5 vezes por bloco de ar. Isso alivia toda consulta de colisão e a varredura do spawn. | Média |
| 8 | Baixo | `W3Client.applyLinkedOptions` (L427-445) | `options.inactivityFpsLimit().set(InactivityFpsLimit.MINIMIZED)`. Hoje está `"afk"` no `options.txt`, e o `FramerateLimitTracker` corta para 30 fps depois de 60 s e para 10 fps depois de 600 s sem input. Isso afeta o overlay e o export, não o servidor. | Média |
| 9 | Baixo / estrutural | `data/minewitcher3/dimension_type/mirror.json` | Altura 2048 = 128 seções por chunk, o que encarece geração, luz, envio e as varreduras. Reduzir para a faixa que o W3 usa de fato (por exemplo `min_y -256`, `height 1024`), coordenado com o mapeamento de coordenadas. **Isso muda o mundo existente.** | Baixa-média |
| 10 | Diagnóstico | | Capturar um `jcmd <pid> Thread.print` durante o próximo "Can't keep up", ou instalar o spark, para confirmar o #1 antes e depois. | |

**Ordem sugerida:** 1 + 1b e 2 + 3 (as duas pontas: não travar no respawn e não morrer nem congelar), depois 3b e 5. Os itens 6 e 10 servem para medir.

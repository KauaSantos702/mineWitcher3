# 04 — Colisão/física e atores/dano (MineWitcher3)

Escopo: como ler a colisão do mundo do W3 para exportar AABBs ao Minecraft, como dar colisão no W3 a blocos colocados no Minecraft, e como enumerar/danificar atores (NPCs/monstros). Preferência por WitcherScript.

Todas as análises foram só de leitura (sem rodar o jogo, sem injetar nada, sem alterar arquivos do jogo).
Caminhos abreviados:
- `SCR` = `C:\Program Files (x86)\Steam\steamapps\common\The Witcher 3\content\content0\scripts`
- `BIN` = `...\The Witcher 3\bin\x64_dx12`
- `C0` = `...\The Witcher 3\content\content0`

Scripts criados (todos read-only, rodar com `tools\.venv\Scripts\python.exe -I`), em `C:\codes\rea\mineWitcher3\tools\scripts\collision\`:
| script | o que faz |
|---|---|
| `physx_probe.py <BIN>` | imports/exports de física + strings de PhysX/APEX no exe |
| `pe_version.py <pe...>` | VS_VERSIONINFO das DLLs |
| `find_str_xref.py <pe> <off_hex> [antes] [depois]` | acha LEA rip-relative para uma string e desmonta (capstone) |
| `rtti_vtable.py <pe> <.?AV...@@>` | TypeDescriptor → CompleteObjectLocator → vtable (MSVC x64) |
| `bundle_list.py <out.tsv> <bundles...>` | lista a tabela de arquivos dos `.bundle` (POTATO70) |
| `cr2w_props.py <bundle> <off> <size>` | parser CR2W mínimo (inclusive CR2W aninhado em `flatCompiledData`) |

Legenda: **[OBS]** observado diretamente; **[INF]** inferido; **[?]** desconhecido / precisa de teste em jogo.

---

## 1. Motor de física

### 1.1 Qual é
- **[OBS] NVIDIA PhysX 3.3 com link estático no `witcher3.exe`**, mais APEX (também estático). Só `PhysX3Common_x64.dll` é DLL separada.
  - `BIN\PhysX3Common_x64.dll`: FileVersion **3.3.0.0**, ©2013 NVIDIA (`pe_version.py`).
  - O exe importa **362 símbolos** de `PhysX3Common_x64.dll` (`physx::shdfnd`, `Gu::HeightField*`, `Cm::*`). Ele **não importa** `PhysX3_x64.dll` nem `PhysX3Cooking_x64.dll`, e essas DLLs não existem na pasta. Logo o núcleo (Np/Sc/Pxs, SceneQuery) está compilado dentro do exe.
  - Paths de fonte embutidos (asserts): `E:\Main.Lava\dev\external\PhysX3\Source\PhysX\src\NpScene.cpp`, `NpSceneQueries.cpp`, `NpBatchQuery.cpp`, `NpRigidStatic.cpp`, `NpPhysics.cpp`, `cloth\NpCloth.cpp`, `gpu\NpPhysicsGpu.cpp`, e `E:\Main.Lava\dev\external\Apex\framework\src\ApexCreateSDK.cpp` (file offsets 0x029fa790 a 0x02a27da0).
  - **Checagem de versão**: `PxCreateBasePhysics` fica em **VA 0x14117cbe0**. Lá aparece `cmp ecx, 0x3030000` e depois `lea rdx, "Wrong version: PhysX version is 0x%08x, tried to create 0x%08x"` (xref em 0x14117cbfe, via `find_str_xref.py ... 02a03a18`). Então `PX_PHYSICS_VERSION = 0x03030000` → **PhysX 3.3.x**.
  - Há strings da API pública, por exemplo `PxScene::addActor(): ...`, `PxScene::lockRead()`, `PxScene::fetchResults()`, `PxScene::getActiveTransforms()`.
  - `GRB_1_1_api3_x64.dll` (GPU Rigid Bodies 1.3.0.7, exporta `GrbCreatePhysics`) e `cudart64_50_35.dll` estão presentes. É o caminho opcional de GPU, com string `PxCreatePhysXGpu`.
  - **Havok: nenhuma evidência.** As strings do probe não mostram Havok.
- **[OBS] Classes do engine (RTTI no exe)**: `CPhysicsEngine`, `CPhysXEngine`, `CPhysicsWorld`, `CPhysicsWorldPhysXImpl`, `CPhysicsWorldBatchQuery`, `CPhysicsTileWrapper` (terreno em tiles), `CPhysicsSimpleWrapper`, `CPhysicsRagdollWrapper`, `CPhysicsCharacterWrapper`, `CPhysicsDestructionWrapper`, `CPhysxStaticActorCreateTask`, `CPhysxActorsAllocateReleaseTask`, `CSectorDataObjectCollision`, `CCollisionCacheReadOnly`, `CCollisionMesh`, `CCollisionShapeBox/Capsule/Convex/Sphere/TriMesh`, `CScriptBatchQueryAccessor`. Também classes PhysX: `NpScene`, `NpPhysics`, `NpRigidStatic`, `NpRigidDynamic`, `NpShape`, `NpBatchQuery`, `NpVolumeCache`, `NpCloth`, `NpParticleFluid`, `NpAggregate`, `NpArticulation`.
  - vtables encontradas por `rtti_vtable.py`:
    - `CPhysicsWorldPhysXImpl`: VA **0x143828068** (mais 2 sub-vtables com offsets 0x60a50/0x60a58)
    - `CPhysicsWorldBatchQuery`: 0x143828408
    - `CPhysicsTileWrapper`: 0x1439ce520
    - `CScriptBatchQueryAccessor`: 0x1439a8260
  - Os TypeDescriptors de `Np*` existem, mas **nenhum COL foi achado** para eles. [INF] O PhysX provavelmente foi compilado sem RTTI de vtable; para achar a vtable do `NpScene`, use xrefs das strings de `NpScene.cpp`.
- **[OBS] Cache de colisão cozida**: `C0\collision.cache` (890 MB, magic `CC3W`, versão 11). Ela traz os nomes de cada `.w2mesh` com a colisão PhysX cozida, por exemplo `environment\architecture\human\blockout_box.w2mesh`, `engine\meshes\editor\box.w2mesh`, `dlc\bob\...\blockout_base\box_stone.w2mesh`.
- **[OBS] Matriz de grupos de colisão**: `gameplay\globals\collision_matrix.csv` dentro de `C0\bundles\startup.bundle` (offset 96244639, 1988 bytes, sem compressão). A ordem dos grupos é:
  `None;Character;Static;Debris;Ragdoll;Destructible;Terrain;ClothCollider;RigidBody;DeniedArea;Dangles;Camera;BoatDestruction;Water;Weapon;Projectile;Foliage;Boat;BoatDocking;Door;Platforms;Corpse;ParticleCollider;Fence;BoatSide;BoatCollider;LockClimb;UnlockClimb;CameraOccludable;SoftKinematicContact;Dismemberment`.
  - Grupos que colidem com **Character** (=1 na coluna Character): **Static, Terrain, DeniedArea, Weapon, Projectile, Foliage, Boat, BoatDocking, Door, Platforms, Fence, BoatSide**. Os que **não** colidem com o personagem: Debris, Ragdoll, **Destructible**, **RigidBody**, Water, Corpse.
  - É exatamente esse conjunto que o Minecraft precisa como "sólido para o jogador".

### 1.2 "Walk the physics scene" nativo (Stage C) — só viabilidade
Requer injeção (fora do escopo agora). O que seria preciso:
1. **Achar o `PxScene*`.** Partir de `CWorld` → `CPhysicsWorld`/`CPhysicsWorldPhysXImpl` (vtable 0x143828068) → membro `PxScene*`. O offset ainda não foi descoberto [?]. Rota alternativa: hook em `PxCreateBasePhysics` (0x14117cbe0) e depois em `PxPhysics::createScene` (slot de vtable de `NpPhysics`) para capturar a cena criada.
2. **Ler a cena, entre `PxScene::lockRead()` e `unlockRead()`:**
   - `getNbActors(PxActorTypeSelectionFlag::eRIGID_STATIC|eRIGID_DYNAMIC)` e `getActors(flags, buf, n, start)`.
   - Por ator: `getNbShapes`/`getShapes`, depois `PxShape::getGeometry()` (Heightfield = terreno, TriangleMesh, Convex, Box, Capsule, Sphere), `getQueryFilterData()`/`getSimulationFilterData()` (word0 = máscara de grupo [INF], na ordem do CSV acima) e `PxShapeExt::getWorldBounds`.
   - Os slots de vtable vêm dos headers públicos do PhysX 3.3 (`PxScene.h`, `PxRigidActor.h`, `PxShape.h`). Como a lib é estática, não há exports: os endereços saem só da vtable.
3. **Voxelizar** triângulos e heightfields em células de 1/8 de bloco, em vez de usar só AABBs de shape. Um AABB de tri-mesh de um prédio inteiro é grosseiro demais.
4. **Streaming**: a colisão estática é carregada por setor (`CSectorDataObjectCollision`, `CPhysxStaticActorCreateTask`). [INF] A cena só tem o entorno do jogador, o que é bom: Stage C pode rodar continuamente nos atores novos.
5. **Viabilidade**: média/alta tecnicamente (API PhysX 3.3 bem documentada), mas depende de RE de offsets do engine e de injeção. Risco de threading: chamar só fora de `simulate`/`fetchResults`, ou usar `lockRead`.
6. **Alternativa offline** [INF]: parsear `collision.cache` (meshes cozidas) + layers `.w2l` (posições). É grande e pesado; não recomendo para o MVP.

---

## 2. Stage A por script (grade de raycasts em WitcherScript)

### 2.1 APIs disponíveis [OBS]
`SCR\engine\world.ws` (classe `CWorld`, obtida via `theGame.GetWorld()`):
| linha | função |
|---|---|
| 15 | `PointProjectionTest(point, normal:EulerAngles, range)` |
| 18 | `StaticTrace(pointA, pointB, out position, out normal, optional collisionGroupsNames:array<name>) : bool` (1º hit) |
| 20 | `StaticTraceWithAdditionalInfo(..., out material:name, out component:CComponent, optional groups)` |
| 24 | `SweepTest(pointA, pointB, radius, out position, normal, optional groups, optional treeSizeThreshold)` |
| 27 | `SphereOverlapTest(out entities:array<CEntity>, position, radius, optional groups) : int` |
| 30 / 33 | `GetWaterLevel(point, optional dontUseApproximation) : float` / `GetWaterDepth(...)` |
| 43–70 | `NavigationLineTest`, `NavigationCircleTest`, `NavigationClosestObstacleTo*`, `NavigationFindSafeSpot` (navmesh, não física) |
| 74 | `NavigationComputeZ(position, zMin, zMax, out z) : bool` (Z do navmesh) |
| 78 | `PhysicsCorrectZ(position, out z) : bool` (semântica exata [?]; usado em btTaskDeath para assentar loot no chão) |
| 86 | `GetTerrainParameters(out terrainSize:float, out tilesCount:int) : bool` |
| 89 | `GetTraceManager() : CScriptBatchQueryAccessor` |

`SCR\engine\physicsTraceManager.ws` (classe `CScriptBatchQueryAccessor`, ligada às batch queries do PhysX):
| linha | função |
|---|---|
| 10–16 | `SRaycastHitResult { position, normal, distance, component }` |
| 31–32 | `latent RayCast/RayCastDir(..., out result:array<SRaycastHitResult>, optional groups, optional queryFlags:int)` |
| 34–35 | `RayCastSync/RayCastDirSync(..., out result:array<...>, optional groups)` (sem queryFlags) |
| 37–40 | `RayCastAsync(start, end, optional groups, optional queryFlags) : SScriptRaycastId` e `GetRayCastState(id, out result) : EBatchQueryState` |
| 42–48 | `Sweep`, `SweepDir`, `SweepAsync`, `SweepDirAsync`, `GetSweepState` |

Enums (nomes vindos do RTTI do exe, valores numéricos [?]):
- `EQQF_BLOCKING_HIT, EQQF_TOUCHING_HIT, EQQF_IMPACT, EQQF_NORMAL, EQQF_DISTANCE, EQQF_UV, EQQF_MESH_BOTH_SIDES, EQQF_PRECISE_SWEEP, EQQF_NO_INITIAL_OVERLAP`. [INF] Espelham `PxQueryFlag`/`PxHitFlag`, e `EQQF_TOUCHING_HIT` deve permitir **multi-hit** num único raio.
- `BQS_NotReady, BQS_Processed, BQS_NotFound`.

Exemplos vanilla:
- Padrão async: `SCR\game\behavior_tree\tasks\movement\VolumetricMove.ws:205–216` (`SweepAsync(..., EQQF_IMPACT)`, depois `GetSweepState`, `case BQS_NotReady/BQS_Processed`).
- `CComponent.HasCollisionType(name, ...)` (`SCR\engine\components.ws:32`) serve para classificar o `component` de um hit.

### 2.2 Nomes válidos de grupos [OBS]
Contagem de uso em `.PushBack('X')` nos scripts vanilla: Static 58, Terrain 55, Destructible 27, Foliage 19, Ragdoll 18, Water 16, Door 14, RigidBody 11, Boat 10, Platforms 9, Character 9, Fence 7, Dynamic 7, BoatDocking 7, Projectile 4, Debris 4, Corpse 4, Phantom 3, ParticleCollider 2, Weapon, CommunityCollidables, Dangles, Elevator.
- `Phantom`, `CommunityCollidables`, `Dynamic` e `Elevator` não estão no CSV. [?] Podem ser aliases ou grupos definidos no código.

Listas vanilla de referência:
- `SCR\game\player\r4Player.ws:637–644`: `Static, Foliage, Fence, Terrain, Door, RigidBody, Dynamic, Destructible`.
- `SCR\game\vehicles\horse\states\exploration.ws:3559–3562`: `Terrain, Static, Destructible, Door`.
- `SCR\game\player\playerAiming.ws:33–49`: lista longa, inclui `Water, Foliage, Platforms, Boat, Corpse, Phantom, Dangles`.

**Recomendação para Stage A** (sólido para o jogador MC): `['Terrain','Static','Foliage','Fence','Door','Platforms','Boat','BoatDocking','DeniedArea']`. `DeniedArea` é o grupo das "invisible walls" (ver §3). `Water` vai numa query separada.

### 2.3 Custo / quantos traces por frame
- [OBS] O vanilla nunca faz grandes lotes. O máximo visto foi `FindMeditateHeading` com 12 `RayCastSync` num loop (`SCR\game\player\playerWitcher.ws:10228–10237`). `r4Player.ws` tem 8 call sites e `btCondCleanShot.ws` tem 7. São 227 call sites no total, todos pontuais.
- [INF] Cada `StaticTrace` é uma chamada nativa saindo do VM de script interpretado: algumas dezenas de µs de overhead de VM mais o raycast PhysX em cena estática (1–10 µs). Orçamento inicial sugerido: **~200–500 raios síncronos por frame** (≈1–3 ms). Com `RayCastAsync`, o custo de query sai da thread de script e vai para o batch do PhysX: emitir N por frame e coletar no frame seguinte. **[?] Medir em jogo**; o limite de queries async pendentes também é desconhecido.
- Rodar via timer: `AddTimer('MW3_Scan', 0.0, true)` (`SCR\engine\entity.ws:19`) numa entidade/estado do mod, ou no tick do player.

### 2.4 Terreno e água
- **Terreno**: não há API de heightmap em script. Use raycast restrito a `['Terrain']`. `GetTerrainParameters` dá só tamanho e nº de tiles. `NavigationComputeZ` dá o Z do navmesh, que não é o mesmo que colisão.
- **Água**: `GetWaterLevel(p)` é barato e aproximado (com `dontUseApproximation=true` fica exato). `GetWaterDepth(p) > 1000` é o sentinela de "sem água", como no vanilla `SCR\game\behavior_tree\tasks\custom\btTaskSpawnAnim.ws:131–132` e `btTaskDiveCheck.ws:44–46`. Uma query por coluna basta; mande como fluido, não-sólido.

### 2.5 Algoritmo proposto (Stage A em script)
Unidade: 1 m W3 = 1 bloco MC [INF; confirmar com o time de coordenadas]. W3 é Z-up.

1. **Grade de colunas** 97×97 (raio de 48 blocos), passo de 1 m, centrada no bloco do jogador. Ordem em anéis do centro para fora; refazer as colunas sujas quando o jogador anda (faixa nova) e periodicamente (portas, destrutíveis).
2. **Por coluna (x, y)**, com `top = playerZ + 48` e `bot = playerZ − 48`:
   - a. **Raio para baixo** `top → bot` com os grupos sólidos. Prefira `RayCastAsync(..., EQQF_TOUCHING_HIT|EQQF_NORMAL|EQQF_DISTANCE)`, que deve dar todos os hits [INF]. Fallback com `StaticTrace`: re-lançar a partir de `hit.z − 0.02` até `bot`, no máximo K=6 superfícies por coluna (overhangs, pontes, telhados, cavernas).
   - b. **Raio para cima** `bot → top` com o mesmo método, para achar as faces inferiores (tetos e undersides).
   - c. **Montar intervalos sólidos**: ordenar as superfícies superiores (de a, normal.z > 0.3) e inferiores (de b). Cada topo `t` casa com o fundo `f` mais alto que esteja abaixo dele (`f < t`). Se não houver fundo, é sólido até `bot`; isso cobre o heightfield do terreno, que é de face única.
   - d. **Paredes e objetos finos**: onde colunas vizinhas diferem em Z por mais de 0,5 m, ou onde um hit tem |normal.z| < 0.3, subdividir aquela célula em 8×8 subcolunas de 1/8 m, só nas bordas (refinamento adaptativo). Para postes e cercas finos, usar `SweepTest` com raio 0.06 m em vez de raio fino.
   - e. **Água**: `w = GetWaterLevel(center)`. Se `GetWaterDepth < 1000` e `w > soloTop`, marcar fluido de `soloTop` até `w`.
3. **Quantizar** Z para 1/8 de bloco. Arredondar o topo para cima e o fundo para baixo, para o jogador não afundar.
4. **Bucketizar** em seções 16³ e fazer **greedy merge** dos intervalos de colunas vizinhas em caixas.
5. **Custo estimado**: 9409 colunas × ~2–4 raios + refinamento de bordas ≈ 30–60 mil raios por varredura completa. Com 400 raios/frame isso dá ~1,5–2,5 s a 60 fps no primeiro preenchimento; depois, incremental (faixa de 97 colunas por bloco andado ≈ 400–800 raios).

### 2.6 Layout de dados (W3 → MC)
```
SectionBoxes {
  int32 sx, sy, sz;        // índice de seção MC (16 blocos)
  uint32 version;          // incrementa a cada rescan da seção
  uint16 count;
  Box[count] {             // coordenadas locais em 1/8 de bloco (0..128 cabe em u8)
    uint8 x0, y0, z0, x1, y1, z1;   // y = altura MC (= Z do W3)
    uint8 kind;            // 0 solid, 1 water(fluid), 2 foliage, 3 door/dynamic, 4 deniedArea
  }
}
ColumnInvalidate { int32 bx, bz; }  // opcional: zera colunas
```
Eixos: `mc.x = w3.x`, `mc.y = w3.z`, `mc.z = −w3.y` [INF: handedness; confirmar com o time de coordenadas]. Uma seção cheia cabe em poucos KB.

---

## 3. Colisão em W3 para blocos colocados no Minecraft

### 3.1 Evidência de "invisible walls" vanilla [OBS]
Encontradas por `bundle_list.py` + `cr2w_props.py`:
| template (bundle, offset, tamanho) | conteúdo |
|---|---|
| `dlc\bob\data\quests\main_quests\quest_files\q705_epilog\entities\q705_invisible_wall.w2ent` (bob.bundle, 512190480, 3227 B) | `CEntity` + `CStaticMeshComponent`, mesh `dlc\bob\data\environment\blockout_base\box_stone.w2mesh`, `transform{pos(-0.47,0,0), scale(1, 0.135, 2.66)}`, `drawableFlags=[DF_ClimbBlock]` (**sem `DF_IsVisible` → invisível**), `pathLibCollisionType=PLC_Dynamic`, tag `no_climb` |
| `dlc\ep1\data\quests\quest_files\q601_intro\entities\q601_invisible_wall.w2ent` (ep1.bundle, 232369680, 3439 B) | `CStaticMeshComponent` com mesh `...\q601_intro\entities\invisible_wall_mesh.w2mesh` (bbox 2,86 × 0,17 × 6,04 m), `scale(1,1,0.358)`, `drawableFlags=[]`, `physicalCollisionType` = CPhysicalCollision `DeniedArea` |
| `dlc\bob\data\quests\main_quests\quest_files\q704b_fairy_tale\entities\q704_ft_barrier_block.w2ent` (bob.bundle, 508791952, 2867 B) | `CStaticMeshComponent` com mesh `environment\architecture\human\blockout_box.w2mesh`, `scale(20,5,40)`, `drawableFlags=[]` |

Meshes cúbicas de 1 m (bbox lida do CR2W), todas com entrada em `collision.cache`:
- `environment\architecture\human\blockout_box.w2mesh`: X/Y −0,5..0,5, Z 0..1 (base no chão, centrado).
- `engine\meshes\editor\box.w2mesh`: −0,5..0,5 nos 3 eixos.
- `(dlc\bob\data\)environment\blockout_base\box_stone.w2mesh`: X 0..1, Y −1..0, Z 0..1.

Outros templates úteis:
- Caixas sólidas visíveis: `environment\decorations\decoration_sets\boxes\decoration_set_boxes_crate_b.w2ent` (blob.bundle, comprimido 3612/9922).
- Exemplos com nomes de colisão: `...\q603_bank\entities\elf_collision_boxes\q603_collision_planks.w2ent`, `sq701_table_bench_collision_invisible.w2ent`, `th701_invisible_wall.w2ent`, `mq7023_invisible_wall_one_sided.w2ent`, `q104_ice_wall_collision.w2ent`, `q210_lab_barrier_collision.w2ent`.
- Há ~40 `.w2l` de `invisible_walls` nos `world_*_startup.bundle`.

[INF] Conclusão: uma invisible wall vanilla é um `CStaticMeshComponent` com mesh-caixa, escala no transform do componente e `drawableFlags` sem `DF_IsVisible`. A colisão vem da mesh cozida em `collision.cache`, escalada.

### 3.2 Opções
1. **(Recomendada) Template próprio do mod**: `mw3_block.w2ent`, criado com WolvenKit e empacotado no `mods\` (não altera arquivos vanilla). Conteúdo: `CStaticMeshComponent` com mesh `environment\architecture\human\blockout_box.w2mesh`, `drawableFlags` sem `DF_IsVisible` (ou com, se quiser ver o bloco), escala 1 e opcionalmente `physicalCollisionType=Static`.
   - Spawn: `ent = theGame.CreateEntity((CEntityTemplate)LoadResource("dlc\\mw3\\mw3_block.w2ent", true), pos, rot)`. Assinaturas: `SCR\core\resource.ws:6`, `SCR\engine\game.ws:129`; uso vanilla de `LoadResource(path, true)` em `SCR\game\gameplay\items\spells\signEntity.ws:708`.
   - Remoção: `ent.Destroy()` (`SCR\engine\entity.ws:34`).
   - Fazer variantes de tamanho (1×1×1, 1×1×⅛ laje, 2×2×2, 4×4×4…) para casar com o greedy-merge e reduzir o nº de entidades.
2. **Sem tooling, só vanilla**: spawnar `q704_ft_barrier_block.w2ent` (20×5×40 m, grande demais) ou caixas visíveis (crate_b), e chamar `SetVisible(false)` nos `CDrawableComponent` (`SCR\engine\components.ws:470`). [?] Ocultar em runtime provavelmente mantém a colisão, mas isso precisa de teste.
3. **Escala em runtime**: `CComponent.SetScale(Vector)` existe (`SCR\engine\components.ws:28`; usado em `SCR\game\gameplay\items\spells\yrdenEntity.ws:222`). [?] Não se sabe se o shape estático PhysX é recriado ou escalado. Teste: spawnar, `SetScale`, depois `StaticTrace` contra o bloco.
4. **Teste de validação** em jogo: depois do spawn, `StaticTrace` de cima para baixo no bloco com `['Static']`. A colisão estática é criada de forma assíncrona (`CPhysxStaticActorCreateTask`) [INF]: esperar 1–2 frames.
5. `SphereOverlapTest` (`world.ws:27`) serve para checar se um bloco vai prender um ator antes do spawn.

---

## 4. Atores: enumeração, dados e dano

### 4.1 Enumeração [OBS]
`SCR\game\actorsStorage.ws`:
| linha | função |
|---|---|
| 44–68 | `GetActorsInRange(center:CNode, range, optional maxResults, optional tag, optional onlyAlive)` (usa `FindGameplayEntitiesInSphere` + `FLAG_OnlyActors`/`FLAG_OnlyAliveActors`) |
| 84 | `FindGameplayEntitiesInRange(out ents, center:CNode, range, maxResults, optional tag, optional queryFlags, optional target, optional className)` |
| 96 | `FindGameplayEntitiesInSphere(out ents, point:Vector, range, maxResults, optional tag, optional queryFlags, optional target, optional className)` |
| 106 / 118 / 130 | `...InCylinder` / `...InCone` / `...InBox` |
| 150 | `FindActorsAtLine(start, end, radius, out array<SRaycastHitResult>, optional groups)` (útil para raycast de ataque vindo do MC) |
| 20 | `GetAttitudeBetween(one, two) : EAIAttitude` (`AIA_Hostile/Neutral/Friendly`) |

- Flags (nomes do RTTI, constantes nativas): `FLAG_OnlyActors, FLAG_OnlyAliveActors, FLAG_ExcludePlayer, FLAG_ExcludeTarget, FLAG_Attitude_Friendly/Neutral/Hostile, FLAG_TestLineOfSight, FLAG_PathLibTest, FLAG_Vehicles, FLAG_NotVehicles, ...`. Uso vanilla: `SCR\game\player\r4Player.ws:15353` (`FLAG_ExcludePlayer + FLAG_OnlyAliveActors`).
- `CActor.GetNPCsAndPlayersInRange(range, maxResults, tag, queryFlags)`: `SCR\game\actor.ws:1408`.

### 4.2 Dados por ator [OBS]
| dado | função |
|---|---|
| posição / rotação / heading | `GetWorldPosition()`, `GetWorldRotation()`, `GetHeading()` (`SCR\engine\node.ws:24,27,39`) |
| id estável | `GetGuidHash() : int` (`SCR\engine\entity.ws:323`; usado como id em `hudModuleInteractions.ws:479`) [?]: colisão de hash ou 0 em entidades spawnadas por community; fallback: tabela script `array<CActor>` com ids próprios |
| vida | `GetHealth()`, `GetMaxHealth()`, `GetHealthPercents()` (`SCR\game\actor.ws:2390, 2413, 2377`). Humanos usam `BCS_Vitality` e monstros `BCS_Essence` (`UsesVitality/UsesEssence`, 2444/2453) |
| vivo / combate / atacável | `IsAlive()` (929), `IsInCombat()` (561), `IsAttackableByPlayer()` (1196), `GetAttitude(actor)` (545), `GetTarget()` (537) |
| tipo | `IsHuman()` (1641), `IsMonster()` (1691); `CNewNPC.GetNPCType()` (`SCR\game\npc\npc.ws:270`), `GetLevel()` (npc.ws:2085); `GetDisplayName()` (`SCR\game\gameplayEntity.ws:81`); `GetVoicetag()` (actor.ws:994) |
| cápsula | `((CMovingPhysicalAgentComponent)GetMovingAgentComponent()).GetCapsuleHeight()/GetCapsuleRadius()` (`SCR\game\components\movingAgentComponent.ws:275–276`); `GetRadius()` (actor.ws:903) |
| velocidade | `GetMovingAgentComponent().GetVelocity()` (movingAgentComponent.ws:110) |

`ActorUpsert` a 20 Hz (timer de 0,05 s):
```
ActorUpsert { int32 id; float x,y,z; float heading; float radius,height;
              float hp, hpMax; uint8 attitude; uint8 flags(alive|combat|attackable|human|monster); name tag/voicetag }
ActorRemove { int32 id; }
```

### 4.3 Dano "correto" (XP, loot, quests)
Caminho vanilla:
1. `theGame.damageMgr` é `W3DamageManager` (`SCR\game\r4Game.ws:342, 697`).
2. `ProcessAction` (`SCR\game\gameplay\damage\damageManager.ws:8`) rejeita se o atacante é o player e `!npc.isAttackableByPlayer` (linha 28). Depois chama `W3DamageManagerProcessor.ProcessAction` (`damageManagerProcessor.ws:22`), que aplica resistências, hit reaction e morte.
3. Na morte, `CNewNPC.OnDeath` (`npc.ws:2514`) chama `ShouldGiveExp(damageAction.attacker)` (2155, 2688). O XP só sai se o NPC for hostil ao player e o atacante for `thePlayer` (2166–2173), via `CalculateExperiencePoints` (2692).
4. O loot é criado no `btTaskDeath` (`SCR\game\behavior_tree\tasks\high_level\btTaskDeath.ws:595–617`). Quests reagem à morte do ator.
- `Kill(source, ignoreImmortality, attacker)` (`actor.ws:1784`) usa `GetKillAction` (1763–1782): `DAMAGE_NAME_DIRECT` com MaxF(vit, ess) e `ProcessAction`. **Passe `thePlayer` como `attacker` para ganhar XP.**

Snippet recomendado, que é o mesmo padrão vanilla de `SCR\game\behavior_tree\tasks\custom\btTaskDettlaffDash.ws:58–62` e `SCR\game\npc\npc.ws:2989–3004`:
```witcherscript
function MW3_DamageActor(victim : CActor, amount : float, optional heavy : bool)
{
    var action : W3DamageAction;
    if (!victim || !victim.IsAlive()) return;
    action = new W3DamageAction in theGame.damageMgr;              // padrão de actor.ws:1774
    // Initialize(att, vict, causer, src, hitReaction, powerStat, isMelee, isRanged, isWitcherSign, isEnvironment)
    action.Initialize(thePlayer, victim, NULL, "MineWitcher3",
                      heavy ? EHRT_Heavy : EHRT_Light, CPS_AttackPower,
                      true, false, false, false);                 // baseAction.ws:109
    action.AddDamage(theGame.params.DAMAGE_NAME_SLASHING, amount); // baseAction.ws:233; gameParams.ws:130-136
    theGame.damageMgr.ProcessAction(action);                       // damageManager.ws:8
    delete action;
}
```
- Para escalar com a espada e os atributos do Geralt (crit, sons, hit FX), use `W3Action_Attack.Init(attacker, victim, causer, weaponId, attName, src, hrt, canParry, canDodge, skillName, swType, swDir, isM, isR, isW, isE, ...)` (`SCR\game\gameplay\actions\attackAction.ws:27`) com:
  - `weaponId = GetWitcherPlayer().GetEquippedSword(true)` (`playerWitcher.ws:1734`)
  - `attName = theGame.params.ATTACK_NAME_LIGHT` (`'attack_light'`, `gameParams.ws:90`)
  - `AST_Horizontal` / `ASD_LeftRight` (`types.ws:244`)
  - Modelo vanilla: `actor.ws:5609–5640` (`Attack`/`PrepareAttackAction`).
- Para golpes vindos do MC (raycast do jogador MC), resolva o alvo por id, ou por `FindActorsAtLine(start, end, radius, hits)` (`actorsStorage.ws:150`).

---

## 5. Lacunas / próximos passos
- [?] Orçamento real de traces por frame (sync vs async) e limite de queries async pendentes: medir em jogo com um `exec function`.
- [?] Valores numéricos de `EQQF_*` e se `EQQF_TOUCHING_HIT` retorna multi-hit via `RayCastAsync`/`RayCast` latente.
- [?] Grupos default de `StaticTrace` quando `collisionGroupsNames` é omitido.
- [?] Semântica de `PhysicsCorrectZ`.
- [?] Se `SetScale`/`SetVisible(false)` em runtime mantêm ou atualizam a colisão PhysX.
- [?] Unicidade de `GetGuidHash` para NPCs de community/spawners.
- [?] Offset de `PxScene*` em `CPhysicsWorldPhysXImpl` e a vtable do `NpScene` (só se Stage C nativo for adotado).
- O transporte script ↔ bridge nativo não é escopo deste doc.

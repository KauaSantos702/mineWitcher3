// Derived from SkyCraft (https://github.com/chasmlol/skycraft), Copyright (c) 2026 chasmlol, MIT License.
// Ported from protocol/skycraft_protocol.h at upstream commit bfcaf17 (SkyCraft protocol v11).
// See THIRD-PARTY-NOTICES.md for the full license text.
//
// MineWitcher3 shared-memory protocol (The Witcher 3 native plugin mw3.asi <-> Minecraft Fabric mod).
//
// This header is the single source of truth for the byte layout. The Java side mirrors it in
// fabric/src/main/java/dev/minewitcher3/link/Proto.java; if you change anything here, change it
// there too (and in tools/fake_witcher.py) and bump kVersion.
//
// All multi-byte values are little-endian. The game (witcher3.exe) creates the mapping; Minecraft
// opens it. Coordinates in this protocol are always Minecraft space (blocks, Y up, Z south) unless
// noted; the conversion from/to Witcher 3 space lives on the game side only (see "coordinates").
//
// The byte layout is intentionally identical to SkyCraft v11 (same offsets, same struct sizes):
// only names, the magic, the version, the mapping name and the scale changed. Keeping it that way
// lets the Java side stay a near-verbatim port of upstream.
#pragma once

#include <cstdint>

namespace mw3::proto
{
	inline constexpr std::uint32_t kMagic = 0x4333574D;  // "MW3C"
	inline constexpr std::uint32_t kVersion = 1;
	inline constexpr wchar_t       kMappingName[] = L"Local\\MineWitcher3_v1";

	// Named mutex Minecraft holds while it runs (kMappingName + "_minecraft"), so the game side
	// never starts a second Minecraft.
	inline constexpr wchar_t       kRunningMutexName[] = L"Local\\MineWitcher3_v1_minecraft";

	// ---- coordinates --------------------------------------------------------------------------
	// 1 Minecraft block == 1 Witcher 3 metre (Geralt ~1.9 m, MC player 1.8 blocks).
	inline constexpr double kUnitsPerBlock = 1.0;

	// Witcher 3 world space: metres, Z up, +Y north. Minecraft: blocks, Y up, Z south.
	//   mc = (w.x, w.z, -w.y)          w = (mc.x, -mc.z, mc.y)
	// Use doubles: W3 X/Y reach several thousand metres. Meshes in the render ring are section-
	// relative floats, so precision is kept on both sides.
	// Yaw: still to be confirmed in game (sign and offset of EulerAngles.Yaw vs MC yaw, where MC
	// yaw 0 faces +Z/south and grows clockwise seen from above). Keep the conversion on the game side.
	struct McVec { double x, y, z; };
	struct W3Vec { double x, y, z; };

	constexpr McVec W3ToMc(W3Vec w) noexcept { return { w.x / kUnitsPerBlock, w.z / kUnitsPerBlock, -w.y / kUnitsPerBlock }; }
	constexpr W3Vec McToW3(McVec m) noexcept { return { m.x * kUnitsPerBlock, -m.z * kUnitsPerBlock, m.y * kUnitsPerBlock }; }

	// GameState::worldId / WaterGrid::worldId / RenDug::worldId: 32-bit FNV-1a of the lower-case W3
	// world name (e.g. "novigrad", "skellige", "kaer_morhen", "bob", or the world file stem). The
	// game side may append an area suffix if it wants separate collision epochs per area. 0 = none.
	constexpr std::uint32_t WorldIdFromName(const char* name) noexcept
	{
		std::uint32_t h = 2166136261u;
		for (; *name; ++name) {
			char c = *name;
			if (c >= 'A' && c <= 'Z') {
				c = char(c - 'A' + 'a');
			}
			h = (h ^ std::uint8_t(c)) * 16777619u;
		}
		return h;
	}

	// ---- region offsets ---------------------------------------------------------------------
	inline constexpr std::uint64_t kOffHeader = 0x0;
	inline constexpr std::uint64_t kOffGameState = 0x100;
	inline constexpr std::uint64_t kOffMcState = 0x200;
	inline constexpr std::uint64_t kOffOverlayCtl = 0x300;
	inline constexpr std::uint64_t kOffOverlaySlotHdr = 0x340;  // 3 x 0x40
	inline constexpr std::uint64_t kOffWaterGrid = 0x400;       // game -> MC, see WaterGrid
	inline constexpr std::uint64_t kOffInputRing = 0x1000;
	inline constexpr std::uint64_t kOffCollisionRing = 0x20000;
	inline constexpr std::uint64_t kCollisionRingBytes = 32ull << 20;
	inline constexpr std::uint64_t kOffOverlayPixels = kOffCollisionRing + kCollisionRingBytes;
	inline constexpr std::uint32_t kMaxOverlayW = 3840;
	inline constexpr std::uint32_t kMaxOverlayH = 2160;
	inline constexpr std::uint64_t kOverlaySlotBytes = std::uint64_t(kMaxOverlayW) * kMaxOverlayH * 4;
	inline constexpr std::uint32_t kOverlaySlots = 3;
	inline constexpr std::uint64_t kOffActorTable = 0x12000;   // game -> MC, see ActorTable
	inline constexpr std::uint64_t kOffEventRing = 0x17000;    // MC -> game, see McEvent
	inline constexpr std::uint64_t kOffWorldEntities = 0x1C000;  // MC -> game, see WorldEntities
	inline constexpr std::uint64_t kOffRenderRing = kOffOverlayPixels + kOverlaySlotBytes * kOverlaySlots;
	inline constexpr std::uint64_t kRenderRingBytes = 64ull << 20;
	inline constexpr std::uint64_t kMappingBytes = kOffRenderRing + kRenderRingBytes;

	// ---- header @0x0 ------------------------------------------------------------------------
	struct Header
	{
		std::uint32_t magic;
		std::uint32_t version;
		std::uint32_t gamePid;            // witcher3.exe process id
		std::uint32_t mcPid;
		std::uint64_t gameHeartbeatMs;    // GetTickCount64() at last game Present (also beats in menus/loading)
		std::uint64_t mcHeartbeatMs;      // GetTickCount64() at last MC frame
	};
	static_assert(sizeof(Header) == 0x20);

	// ---- game -> MC state @0x100 (seqlock: seq odd while writing; +2 per game frame) ---------
	enum GameFlags : std::uint32_t
	{
		kGameInGame = 1u << 0,    // a save is loaded and the player exists
		kGameMenuOpen = 1u << 1,  // a game menu / panel / dialogue owns input; MC should drop held keys
		kGameLoading = 1u << 2,   // loading screen / world or area transition in progress
		// The game draws Minecraft's world itself (a render-ring consumer is attached): MC streams block
		// sections, the atlas, entities and the avatar through the render ring. Clear: MC skips all of
		// that (it would only be drained and dropped) and ships just the overlay frame.
		kGameWorldRender = 1u << 3,
	};

	// The game's water (lakes, rivers, the sea) around the player, for Minecraft to treat as its own
	// water: swimming, floating, drowning. Seqlock like GameState.
	inline constexpr std::uint32_t kWaterGridSize = 16;
	inline constexpr float         kNoWater = -1.0e30f;

	struct WaterGrid
	{
		std::uint32_t seq;
		std::int32_t  originX, originZ;  // Minecraft block column of surface[0]
		std::uint32_t worldId;           // as in GameState
		float         surface[kWaterGridSize * kWaterGridSize];  // [z * size + x]: MC y of the water surface; kNoWater: none
	};
	static_assert(sizeof(WaterGrid) <= 0xC00);

	struct GameState
	{
		std::uint32_t seq;
		std::uint32_t flags;           // GameFlags
		std::uint32_t worldId;         // WorldIdFromName(<W3 world name>)
		std::uint32_t collisionEpoch;  // bumps on world change; MC drops all collision data
		double        posX, posY, posZ;  // game player feet, MC coords
		float         yaw, pitch;        // authoritative look (MC degrees)
		std::uint32_t teleportSeq;       // MC teleports its player to pos when this changes
		std::uint32_t viewportW, viewportH;
		float         gameHour;          // 0..24, from the game clock (GameTime)
	};
	static_assert(sizeof(GameState) == 0x40);

	// ---- MC -> game state @0x200 (seqlock) ------------------------------------------------
	enum McFlags : std::uint32_t
	{
		kMcInWorld = 1u << 0,
		kMcScreenOpen = 1u << 1,  // an MC GUI screen (inventory, chat, ...) is open
		kMcOnGround = 1u << 2,
		kMcSneaking = 1u << 3,
		kMcSprinting = 1u << 4,
		kMcDead = 1u << 5,
		kMcSwimming = 1u << 6,
		kMcFlying = 1u << 7,
	};

	struct McState
	{
		std::uint32_t seq;
		std::uint32_t flags;          // McFlags
		double        x, y, z;        // interpolated feet position (MC coords)
		float         yaw, pitch;     // MC rotation (degrees)
		float         eyeHeight;      // blocks above feet
		float         sensitivity;    // MC mouse sensitivity option (0..1)
		std::uint32_t teleportAck;    // last GameState::teleportSeq applied
		std::uint32_t guiScale;
		std::uint64_t frameCounter;
		float         fovDeg;         // effective vertical FOV (includes sprint / fluid modifiers)
		float         bobPhase;       // MC walk-bob phase (interpolated walk distance); 0 if bobbing is off
		float         bobAmount;      // MC walk-bob amplitude
		std::uint32_t pad4C;
		double        eyeX, eyeY, eyeZ;  // MC camera position (interpolated, includes sneak eye lerp)

		// Raw 20 Hz physics ticks, so the game can interpolate on its own frame clock exactly like
		// Minecraft's renderer does with partial ticks (no judder from the two games' frame phase).
		std::int64_t tickQpc;             // QueryPerformanceCounter at the (remainder-corrected) tick
		double       prevX, prevY, prevZ;  // feet at the previous tick
		double       curX, curY, curZ;     // feet at the latest tick
		float        tickEyeO, tickEye;      // Camera's smoothed eye height, previous/latest tick
		float        walkDistO, walkDist;    // walk-bob phase inputs
		float        bobO, bob;              // walk-bob amplitude inputs
		float        tickMs;                 // milliseconds per tick (50 unless /tick rate changed)
		std::uint32_t tickPad;

		// Minecraft's camera (F5): 0 first person, 1 third person behind, 2 third person in front
		// (looking back at the player). cameraDistance is how far Minecraft's camera sits from
		// the eye, after its own zoom collision (Minecraft blocks and the game's triangles).
		std::uint32_t cameraMode;
		float         cameraDistance;
	};
	static_assert(sizeof(McState) == 0xC8);
	static_assert(sizeof(McState) <= 0x100);

	// ---- overlay triple buffer @0x300 --------------------------------------------------------
	// state: bits 0-1 = index of the "middle" slot, bit 2 = middle holds an unread frame.
	// Writer (MC) renders into its private back slot, then xchg(state, back | kDirty) and keeps
	// the returned index as its new back slot. Reader (game) does xchg(state, front) only when
	// the dirty bit is set and keeps the returned index as its new front slot.
	inline constexpr std::uint32_t kOverlayDirty = 1u << 2;

	struct OverlayCtl
	{
		std::uint32_t state;
		std::uint32_t pad;
		std::uint64_t framesPublished;
	};

	struct OverlaySlotHdr
	{
		std::uint32_t width;
		std::uint32_t height;
		std::uint32_t flags;  // bit0: rows are bottom-up
		std::uint32_t pad;
		std::uint64_t frameId;
		std::uint8_t  reserved[0x40 - 0x18];
	};
	static_assert(sizeof(OverlaySlotHdr) == 0x40);

	// ---- input ring @0x1000 (game produces, MC consumes) ------------------------------------
	inline constexpr std::uint32_t kInputRingEntries = 4096;  // power of two
	inline constexpr std::uint64_t kInputRingHeadOff = 0x00;  // u64, written by the game
	inline constexpr std::uint64_t kInputRingTailOff = 0x40;  // u64, written by MC
	inline constexpr std::uint64_t kInputRingDataOff = 0x80;

	enum InputType : std::uint16_t
	{
		kInKey = 1,          // code = SDL scancode, a = 1 press / 0 release
		kInMouseButton = 2,  // code = SDL button (1 L, 2 M, 3 R, 4 X1, 5 X2), a = 1 press / 0 release
		kInScroll = 3,       // a = wheel notches * 120 (positive = up)
		kInCursor = 4,       // a, b = absolute cursor position in overlay pixels
		kInText = 5,         // a = unicode code point
		kInReleaseAll = 6,   // release every held key/button (input focus left MC)
		kInHurt = 7,         // the game hit the player: code = HurtKind, a = damage * 100, b = attacker actorId, c = HurtFlags
		                     // (MineWitcher3 v6 sends the damage already in Minecraft health points: combat.cpp)
		kInOpenMenu = 8,     // open Minecraft's pause/options menu
	};

	enum HurtKind : std::uint16_t
	{
		kHurtMelee = 0,
		kHurtProjectile = 1,
		kHurtMagic = 2,
		kHurtOther = 3,
	};

	enum HurtFlags : std::uint32_t
	{
		kHurtBlockedInGame = 1u << 0,  // the game already counted it as parried/blocked
		kHurtHeavyAttack = 1u << 1,    // heavy/strong attack: MC applies extra knockback
	};

	// ---- actor table @0x12000 (game -> MC, seqlock) ------------------------------------------
	// Nearby game actors (NPCs, monsters), mirrored in Minecraft as invisible hittable proxy entities.
	inline constexpr std::uint32_t kMaxActors = 256;

	enum ActorFlags : std::uint32_t
	{
		kActorHostile = 1u << 0,    // hostile to the player right now
		kActorDead = 1u << 1,
		kActorEssential = 1u << 2,
		kActorInCombat = 1u << 3,
	};

	struct ActorRecord
	{
		std::uint32_t actorId;     // stable id of the W3 entity while it exists (game side picks it; 0 = invalid)
		std::uint32_t flags;       // ActorFlags
		float         x, y, z;     // feet, MC coords
		float         yaw;         // MC degrees
		float         width;       // blocks
		float         height;      // blocks
		float         healthFrac;  // 0..1
		std::uint16_t level;
		std::uint16_t pad;
		char          name[24];    // display name, UTF-8, NUL-terminated (truncated)
	};
	static_assert(sizeof(ActorRecord) == 64);

	struct ActorTable
	{
		std::uint32_t seq;
		std::uint32_t count;
		std::uint8_t  pad[0x40 - 8];
		ActorRecord   actors[kMaxActors];
	};
	static_assert(sizeof(ActorTable) == 0x40 + 64 * kMaxActors);

	// ---- event ring @0x17000 (MC -> game) -----------------------------------------------------
	inline constexpr std::uint32_t kEventRingEntries = 512;  // power of two
	inline constexpr std::uint64_t kEventRingHeadOff = 0x00;  // u64, written by MC
	inline constexpr std::uint64_t kEventRingTailOff = 0x40;  // u64, written by the game
	inline constexpr std::uint64_t kEventRingDataOff = 0x80;

	enum McEventType : std::uint32_t
	{
		kEvHitActor = 1,    // actorId, a = MC damage (after MC's own modifiers), b/c = knockback dir x/z (MC), d = knockback strength
		kEvPlayerDied = 2,  // the Minecraft player died: kill the game player (actorId = attacker or 0)
		kEvExplosion = 3,   // a Minecraft explosion (TNT, creeper, ...): a/b/c = centre (MC coords), d = radius (blocks)
		kEvArrowStuck = 4,  // an arrow stuck in a game actor: actorId, a/b/c = where it hit (MC coords), d = flight yaw,
		                    // flags = flight pitch (float bits), weapon = arrow texture (0 plain, 1 tipped, 2 spectral)
		// 5 is reserved: SkyCraft's kEvSkillUse (Skyrim ActorValue skill training). Not sent by MineWitcher3.
	};

	enum HitFlags : std::uint32_t
	{
		kHitCritical = 1u << 0,
		kHitProjectile = 1u << 1,
		kHitSweep = 1u << 2,
		kHitFire = 1u << 3,
	};

	// What landed a kEvHitActor (the game plays that weapon class's impact effect and sounds).
	enum HitWeapon : std::uint32_t
	{
		kWeaponUnarmed = 0,
		kWeaponBlade = 1,   // swords
		kWeaponAxe = 2,
		kWeaponBlunt = 3,   // maces, pickaxes, shovels, hoes, anything else held
		kWeaponPierce = 4,  // tridents, spears
		kWeaponArrow = 5,   // arrows and other projectiles
	};

	struct McEvent
	{
		std::uint32_t type;
		std::uint32_t actorId;
		float         a, b, c, d;
		std::uint32_t flags;
		std::uint32_t weapon;  // HitWeapon for kEvHitActor
	};
	static_assert(sizeof(McEvent) == 32);

	// ---- world entities @0x1C000 (MC -> game, seqlock) ---------------------------------------
	// Minecraft things the game draws itself each frame (arrows, dropped items, block cracks) + the
	// block outline.
	inline constexpr std::uint32_t kMaxWorldEntities = 160;

	enum WorldEntityKind : std::uint32_t
	{
		kWeArrow = 1,    // uv[0]: the arrow's item icon
		kWeItem = 2,     // dropped/thrown item: a flat sprite (uv[0]) turning about the vertical
		kWeTrident = 3,  // uv[0]: the trident's item icon
		kWeBlock = 4,    // dropped block item: a spinning cube of side `scale`, uv[0..2] = side, top, bottom
		kWeCrack = 5,    // block-breaking cracks over the box at (x, y, z) of size ext, uv[0] = crack stage
		kWeShadow = 6,   // a player's or mob's feet at (x, y, z), `scale` wide: its soft contact shadow
	};

	struct WorldEntity
	{
		std::uint32_t kind;        // WorldEntityKind
		std::uint32_t id;          // MC entity id (stable while it exists)
		float         x, y, z;     // MC coords (interpolated at MC's render time)
		float         yaw, pitch;  // MC degrees
		float         scale;
		float         ext[3];      // kWeCrack: box size
		float         uv[3][4];    // atlas rects {u0, v0, u1, v1}
		std::uint32_t tint;        // RGBA8 multiplier for the top face (grass, leaves); 0 = none
	};
	static_assert(sizeof(WorldEntity) == 96);

	struct WorldEntities
	{
		std::uint32_t seq;
		std::uint32_t count;
		std::uint32_t hasSelection;           // draw an outline around the targeted block
		float         selMin[3], selMax[3];   // MC coords
		std::uint8_t  pad[0x40 - 36];
		WorldEntity   entities[kMaxWorldEntities];
	};
	static_assert(sizeof(WorldEntities) == 0x40 + sizeof(WorldEntity) * kMaxWorldEntities);
	static_assert(kOffWorldEntities + sizeof(WorldEntities) <= kOffCollisionRing);

	// ---- render ring (MC -> game) -------------------------------------------------------------
	// Byte ring like the collision ring. Minecraft ships its own block meshes (built by Minecraft's
	// block renderer: models, tint, AO, lighting) and its block atlas; the game draws them in its own
	// frame so blocks stay locked to the world and are occluded by the game's geometry.
	inline constexpr std::uint64_t kRenRingHeadOff = 0x00;
	inline constexpr std::uint64_t kRenRingTailOff = 0x40;
	inline constexpr std::uint64_t kRenRingDataOff = 0x80;
	inline constexpr std::uint64_t kRenRingDataBytes = kRenderRingBytes - kRenRingDataOff;

	enum RenType : std::uint32_t
	{
		kRenPad = 0,
		kRenAtlas = 1,     // RenAtlas + RGBA8 pixels (w * h * 4), top row first
		kRenSection = 2,   // RenSection + RenVertex[vertexCount] (triangle list); 0 vertices = remove
		kRenClearAll = 3,  // drop every section (world change)
		kRenTexture = 4,   // RenTexture + RGBA8 pixels: an entity texture (player skin, armour, ...)
		kRenAvatar = 5,    // RenAvatar + RenBatch[batchCount] + RenVertex[vertexCount]: the player's
		                   // model this frame; 0 batches = not shown (first person)
		kRenScene = 6,     // RenScene + RenBatch[batchCount] + RenVertex[vertexCount]: every other
		                   // entity and all particles this frame, relative to RenScene's origin
		kRenAtlasRegion = 7,  // RenAtlasRegion + RGBA8 pixels: an animated sprite's current frame
		kRenLights = 8,       // RenLights + RenLight[count]: a section's light-emitting blocks (sent
		                      // after its kRenSection; 0 = none)
		kRenSolids = 10,      // RenSolids + 512-byte bitset (bit x + 16z + 256y): which blocks of a
		                      // section NPCs collide with (sent after its kRenSection; 0 = none)
		kRenDug = 11,         // RenDug + 512-byte bitset (bit x + 16z + 256y): which blocks of a section
		                      // were dug out of the game's world (its geometry there is gone); 0 = none
		kRenRagdoll = 9,      // RenAvatar + RenBatch[] + RenVertex[]: the player's body standing still,
		                      // relative to the feet and facing +Z, split into its parts (RenBatch
		                      // flags bits 8-11: RagdollPart). Sent about once a second while alive;
		                      // SkyCraft hung the parts on Skyrim's ragdoll when the player
		                      // died; on W3 the game side may use or ignore it.
	};

	struct RenSolids
	{
		std::int32_t  sx, sy, sz;  // section coords, as in RenSection
		std::uint32_t count;       // solid blocks (0: none, and no bitset follows)
	};

	struct RenDug
	{
		std::int32_t  sx, sy, sz;  // section coords, as in RenSection
		std::uint32_t count;       // dug blocks (0: none, and no bitset follows)
		std::uint32_t worldId;     // the game world (GameState::worldId) the bits belong to
		std::uint32_t pad;
	};
	static_assert(sizeof(RenDug) == 24);

	// What a piece of diggable game geometry is made of, as the Minecraft block it digs into
	// (ColTri flags bits 8-15). Chosen on the game side (W3: from PhysX/physical materials and
	// mesh/entity types). The values are Minecraft-side materials and stay game-agnostic.
	enum DigMaterial : std::uint8_t
	{
		kDigNone = 0,  // not known: stone
		kDigGrass = 1,
		kDigDirt = 2,
		kDigStone = 3,
		kDigCobble = 4,
		kDigSnow = 5,
		kDigIce = 6,
		kDigSand = 7,
		kDigGravel = 8,
		kDigMud = 9,
		kDigOakLog = 10,
		kDigSpruceLog = 11,
		kDigBirchLog = 12,
		kDigPlanks = 13,
		kDigMetal = 14,
		kDigGlass = 15,
		kDigOrganic = 16,
		kDigCloth = 17,
		kDigBone = 18,
		kDigWeb = 19,
		kDigAsh = 20,
		kDigBedrock = 21,  // Minecraft only: a few blocks under the land
		kDigMaterialCount
	};

	enum RagdollPart : std::uint32_t
	{
		kPartNone = 0,
		kPartHead = 1,
		kPartBody = 2,
		kPartRightArm = 3,
		kPartLeftArm = 4,
		kPartRightLeg = 5,
		kPartLeftLeg = 6,
		kPartCount = 7,
	};

	struct RenLights
	{
		std::int32_t  sx, sy, sz;  // section coords, as in RenSection
		std::uint32_t count;
	};

	enum LightKind : std::uint8_t
	{
		kLightSteady = 0,
		kLightFlame = 1,  // torches, fire, campfires, candles: flicker
		kLightLava = 2,   // lava, magma: a slow glow
	};

	enum BlockHazard : std::uint8_t
	{
		kHazardNone = 0,
		kHazardFire = 1,   // fire, soul fire, campfires: burns what stands in it
		kHazardLava = 2,   // lava: burns hard
		kHazardMagma = 3,  // magma block: hurts what stands on top of it
	};

	struct RenLight
	{
		std::uint8_t  x, y, z;  // block within the section
		std::uint8_t  level;    // Minecraft light emission, 1-15
		std::uint32_t color;    // RGB8 (r low byte); top byte: LightKind in bits 0-3, BlockHazard in 4-7
	};
	static_assert(sizeof(RenLight) == 8);

	struct RenAtlasRegion
	{
		std::uint32_t x, y, width, height;  // pixels in the combined atlas (kRenAtlas)
	};

	struct RenScene
	{
		double        originX, originY, originZ;  // MC block the positions are relative to
		std::uint32_t batchCount;
		std::uint32_t vertexCount;
	};

	struct RenTexture
	{
		std::uint32_t id;  // 1+, referenced by RenBatch::texture
		std::uint32_t width, height;
		std::uint32_t pad;
	};

	// The player as Minecraft's own entity renderer draws it (skin, armour, held items, cape),
	// posed and animated, with positions in blocks relative to the player's feet: the game puts
	// it at its own interpolated feet position, the one its camera follows.
	struct RenAvatar
	{
		std::uint32_t batchCount;
		std::uint32_t vertexCount;
	};

	struct RenBatch
	{
		std::uint32_t texture;  // 0: the block/item atlas, else a RenTexture id
		std::uint32_t first;    // first vertex
		std::uint32_t count;    // vertices (multiple of 3)
		std::uint32_t flags;    // bit0: translucent (blended, after the solid pass; casts no shadow)
	};

	struct RenAtlas
	{
		std::uint32_t width, height;
	};

	struct RenSection
	{
		std::int32_t  sx, sy, sz;   // section coords (16-block cubes)
		std::uint32_t vertexCount;  // multiple of 3
	};

	struct RenVertex
	{
		float         x, y, z;  // MC coords relative to the section origin (sx*16, sy*16, sz*16)
		float         u, v;     // atlas UV
		std::uint32_t color;    // RGBA8 (tint * ambient occlusion; Minecraft's fixed face shading is left out)
		std::uint32_t light;    // low byte: block light 0-15, next byte: sky light 0-15
		std::uint32_t flags;    // bit0: cutout (alpha test), bit1: translucent,
		                        // bits 4-6: face normal as MC Direction ordinal + 1 (0 = none: lit without a normal)
	};
	static_assert(sizeof(RenVertex) == 32);

	struct InputEvent
	{
		std::uint16_t type;
		std::uint16_t code;
		std::int32_t  a;
		std::int32_t  b;
		std::int32_t  c;
	};
	static_assert(sizeof(InputEvent) == 16);

	// ---- collision ring @0x20000 (game produces, MC consumes) -------------------------------
	// Byte ring. Every message starts 8-byte aligned with {u32 type, u32 payloadBytes}.
	// A kColPad message means "skip to the start of the ring".
	inline constexpr std::uint64_t kColRingHeadOff = 0x00;  // u64 total bytes written
	inline constexpr std::uint64_t kColRingTailOff = 0x40;  // u64 total bytes consumed
	inline constexpr std::uint64_t kColRingDataOff = 0x80;
	inline constexpr std::uint64_t kColRingDataBytes = kCollisionRingBytes - kColRingDataOff;

	enum ColType : std::uint32_t
	{
		kColPad = 0,
		kColClear = 1,   // payload: u32 epoch
		kColRegion = 2,  // payload: ColRegion + ColBlock[count]
		kColTris = 3,    // payload: ColRegion (count = triangles) + ColTri[count]; sent before kColRegion
	};

	// Exact game collision triangle (MC space) for the player's smooth collider.
	enum ColTriFlags : std::uint32_t
	{
		kTriStairHelper = 1u << 0,  // an invisible stair ramp/helper: walkable, never a wall
		kTriDiggable = 1u << 1,     // ground, rock, trees...: can be dug into (bits 8-15: DigMaterial).
		                            // Its normal faces out of the solid side (winding is outward).
		kTriGhost = 1u << 2,        // a diggable triangle as it was before blocks were dug out of it:
		                            // not collision, only for telling what's inside the game's geometry
		kTriTerrain = 1u << 3,      // the land (a height field)
	};

	inline constexpr std::uint32_t kTriMaterialShift = 8;

	struct ColTri
	{
		float         v[9];
		std::uint32_t flags;
	};
	static_assert(sizeof(ColTri) == 40);

	struct ColMsgHeader
	{
		std::uint32_t type;
		std::uint32_t payloadBytes;
	};

	// Replaces all game collision inside the inclusive block box [min, max].
	struct ColRegion
	{
		std::int32_t  minX, minY, minZ;
		std::int32_t  maxX, maxY, maxZ;
		std::uint32_t epoch;
		std::uint32_t count;
	};
	static_assert(sizeof(ColRegion) == 32);

	// One block's worth of game collision as an 8x8x8 occupancy mask.
	// bits[y] bit (z * 8 + x) is sub-voxel (x, y, z), each 1/8 block, in MC axes.
	struct ColBlock
	{
		std::int32_t  x, y, z;
		std::uint32_t pad;
		std::uint64_t bits[8];
	};
	static_assert(sizeof(ColBlock) == 80);

	// ---- layout guards (the Java mirror and fake_witcher.py hard-code these) -----------------
	static_assert(sizeof(InputEvent) == 16 && sizeof(McEvent) == 32 && sizeof(ActorRecord) == 64);
	static_assert(kOffGameState + sizeof(GameState) <= kOffMcState);
	static_assert(kOffMcState + sizeof(McState) <= kOffOverlayCtl);
	static_assert(kOffOverlaySlotHdr + sizeof(OverlaySlotHdr) * kOverlaySlots <= kOffWaterGrid);
	static_assert(kOffWaterGrid + sizeof(WaterGrid) <= kOffInputRing);
	static_assert(kOffInputRing + kInputRingDataOff + sizeof(InputEvent) * kInputRingEntries <= kOffActorTable);
	static_assert(kOffActorTable + sizeof(ActorTable) <= kOffEventRing);
	static_assert(kOffEventRing + kEventRingDataOff + sizeof(McEvent) * kEventRingEntries <= kOffWorldEntities);
	static_assert(W3ToMc({ 1.0, 2.0, 3.0 }).x == 1.0 && W3ToMc({ 1.0, 2.0, 3.0 }).y == 3.0 && W3ToMc({ 1.0, 2.0, 3.0 }).z == -2.0);
	static_assert(McToW3(W3ToMc({ 5.0, -7.0, 9.0 })).y == -7.0);
}

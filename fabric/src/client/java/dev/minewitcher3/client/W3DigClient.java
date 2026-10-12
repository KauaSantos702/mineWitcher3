// Derived from SkyCraft (https://github.com/chasmlol/skycraft), Copyright (c) 2026 chasmlol, MIT License.
package dev.minewitcher3.client;

import static dev.minewitcher3.link.Proto.DIG_STONE;

import dev.minewitcher3.client.render.WorldExporter;
import dev.minewitcher3.net.W3Net;
import dev.minewitcher3.world.W3Clip;
import dev.minewitcher3.world.W3Collision;
import dev.minewitcher3.world.W3Dig;
import dev.minewitcher3.world.W3Ray;
import it.unimi.dsi.fastutil.longs.Long2IntLinkedOpenHashMap;
import it.unimi.dsi.fastutil.longs.Long2ObjectOpenHashMap;
import it.unimi.dsi.fastutil.longs.LongOpenHashSet;
import java.util.ArrayList;
import java.util.List;
import net.fabricmc.fabric.api.client.networking.v1.ClientPlayNetworking;
import net.minecraft.client.particle.TerrainParticle;
import net.minecraft.client.resources.sounds.SimpleSoundInstance;
import net.minecraft.client.resources.sounds.SoundInstance;
import net.minecraft.sounds.SoundSource;
import net.minecraft.client.Minecraft;
import net.minecraft.client.multiplayer.ClientLevel;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.world.level.ChunkPos;
import net.minecraft.world.level.GameType;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.chunk.LevelChunk;
import net.minecraft.world.level.chunk.status.ChunkStatus;
import org.jspecify.annotations.Nullable;

/**
 * The digging player's side of {@link W3Dig}: hitting the game's geometry asks the server to open that
 * cell; a dug block breaking asks it to reveal what's around it (worked out from the game's geometry,
 * which only this client has); and the dug cells of loaded chunks go to the game (WorldExporter).
 */
public final class W3DigClient {
	private W3DigClient() {
	}

	private static final int REVEAL_RANGE = 24;
	private static final int REVEAL_TRIES = 60; // ticks to wait for the game's geometry around a cell

	// Mining the game surface: which cell, how far along (0..1), on which tick last.
	private static @Nullable BlockPos mining;
	private static float progress;
	private static int miningTick;
	private static int hits;
	private static int ticks;
	private static final Long2IntLinkedOpenHashMap REVEAL = new Long2IntLinkedOpenHashMap(); // cell -> tries left
	private static final Long2ObjectOpenHashMap<W3Dig.DugColumn> SEEN = new Long2ObjectOpenHashMap<>();
	private static final W3Dig.DugColumn NONE = new W3Dig.DugColumn(List.of());
	private static int seenWorld = Integer.MIN_VALUE;
	private static int pollTicks;

	/** The game world the player is in (W3 world, by name hash). */
	public static int world() {
		return W3Client.sky().worldId;
	}

	/**
	 * The player attacked (clicked or held) the game geometry under the crosshair: mining it, as long
	 * as mining the block it's made of would take. Returns true if it's being mined.
	 */
	public static boolean attack(Minecraft minecraft) {
		if (!(minecraft.hitResult instanceof W3Clip.GameHitResult result) || minecraft.level == null || minecraft.player == null || minecraft.gameMode == null) {
			return false;
		}
		GameType mode = minecraft.gameMode.getPlayerMode();
		if (mode == GameType.ADVENTURE || mode == GameType.SPECTATOR || (!W3Dig.destruction && minecraft.hasSingleplayerServer())) {
			return false; // (a guest asks anyway: the host's setting decides)
		}
		W3Ray.Hit hit = result.hit;
		if (hit.tri() == null || !hit.tri().diggable) {
			return false; // a building, or something else that stays
		}
		int[] cell = W3Ray.surfaceCell(hit);
		BlockPos pos = new BlockPos(cell[0], cell[1], cell[2]);
		BlockState here = minecraft.level.getBlockState(pos);
		if (!here.isAir() && !here.canBeReplaced()) {
			return false;
		}
		if (W3Dig.isDug(minecraft.level, world(), pos)) {
			return false;
		}
		// Once a tick (a click and a held button can both land in the same one).
		if (pos.equals(mining) && miningTick == ticks) {
			return true;
		}
		if (!pos.equals(mining) || ticks - miningTick > 2) {
			mining = pos;
			progress = 0.0F;
			hits = 0;
		}
		miningTick = ticks;
		int material = hit.tri().material == 0 ? DIG_STONE : hit.tri().material;
		BlockState state = W3Dig.materialState(material);
		progress += minecraft.player.getAbilities().instabuild ? 1.0F : state.getDestroyProgress(minecraft.player, minecraft.level, pos);
		if (hits++ % 4 == 0) {
			var sound = state.getSoundType();
			minecraft.getSoundManager().play(new SimpleSoundInstance(
				sound.getHitSound(), SoundSource.BLOCKS, (sound.getVolume() + 1.0F) / 8.0F, sound.getPitch() * 0.5F, SoundInstance.createUnseededRandom(), pos
			));
			minecraft.particleEngine.add(new TerrainParticle(minecraft.level, hit.x() + hit.nx() * 0.05, hit.y() + hit.ny() * 0.05, hit.z() + hit.nz() * 0.05, 0.0, 0.0, 0.0, state, pos)
				.setPower(0.2F).scale(0.6F));
		}
		if (progress >= 1.0F) {
			ClientPlayNetworking.send(new W3Net.DigOpen(world(), pos, material));
			REVEAL.put(pos.asLong(), REVEAL_TRIES); // what's around it: blocks, or walls
			mining = null;
			progress = 0.0F;
		}
		return true;
	}

	/** Any block change on this client: a dug block that broke gets what's around it revealed. */
	public static void blockChanged(ClientLevel level, BlockPos pos, BlockState before, BlockState after) {
		if (before.isAir() || !(after.isAir() || after.canBeReplaced())) {
			return;
		}
		Minecraft minecraft = Minecraft.getInstance();
		if (minecraft.player == null || minecraft.player.blockPosition().distSqr(pos) > REVEAL_RANGE * REVEAL_RANGE) {
			return;
		}
		if (W3Dig.isDug(level, world(), pos) && (W3Dig.destruction || !minecraft.hasSingleplayerServer())) {
			REVEAL.put(pos.asLong(), REVEAL_TRIES);
		}
	}

	public static void tick(Minecraft minecraft) {
		ticks++;
		ClientLevel level = minecraft.level;
		if (level == null || minecraft.player == null) {
			REVEAL.clear();
			SEEN.clear();
			W3Dig.clientDug = null;
			return;
		}
		int world = world();
		W3Dig.clientWorld = world;
		W3Dig.clientDug = (x, y, z) -> W3Dig.isDug(level, world, new BlockPos(x, y, z));
		reveal(level);
		if (++pollTicks % 5 == 0) {
			pollDug(minecraft, level);
		}
		// The game's geometry around dug cells arrived or changed: their walls are drawn again.
		W3Collision.takeChangedRegions(key -> {
			int rx = BlockPos.getX(key), ry = BlockPos.getY(key), rz = BlockPos.getZ(key);
			W3Dig.wallsChanged(rx, ry, rz, rx + 7, ry + 7, rz + 7);
			for (int sx = (rx - 1) >> 4; sx <= (rx + 8) >> 4; sx++) {
				for (int sz = (rz - 1) >> 4; sz <= (rz + 8) >> 4; sz++) {
					LevelChunk chunk = level.getChunkSource().getChunk(sx, sz, ChunkStatus.FULL, false);
					if (chunk == null) {
						continue;
					}
					for (int sy = (ry - 1) >> 4; sy <= (ry + 8) >> 4; sy++) {
						if (dugBits(chunk, sy) != null) {
							WorldExporter.markDirty(sx, sy, sz);
						}
					}
				}
			}
		});
	}

	private static void reveal(ClientLevel level) {
		if (REVEAL.isEmpty()) {
			return;
		}
		int world = world();
		List<BlockPos> cells = new ArrayList<>();
		List<Integer> materials = new ArrayList<>();
		LongOpenHashSet queued = new LongOpenHashSet();
		long[] keys = REVEAL.keySet().toLongArray();
		for (long key : keys) {
			BlockPos pos = BlockPos.of(key);
			boolean waiting = false;
			for (Direction d : Direction.values()) {
				BlockPos n = pos.relative(d);
				if (queued.contains(n.asLong()) || W3Dig.isDug(level, world, n)) {
					continue;
				}
				if (!W3Collision.isKnown(n.getX(), n.getY(), n.getZ())) {
					waiting = true;
					continue;
				}
				int material = W3Dig.classify(n.getX(), n.getY(), n.getZ());
				if (material <= W3Dig.AIR) {
					continue;
				}
				BlockState state = level.getBlockState(n);
				if (!state.isAir() && !state.canBeReplaced()) {
					material = 0; // a Minecraft block is already there: just cut the game's geometry out
				}
				queued.add(n.asLong());
				cells.add(n);
				materials.add(material);
			}
			int tries = REVEAL.get(key) - 1;
			if (waiting && tries > 0) {
				REVEAL.put(key, tries); // The game's geometry there hasn't arrived yet
			} else {
				REVEAL.remove(key);
			}
		}
		for (int i = 0; i < cells.size(); i += 64) {
			int end = Math.min(cells.size(), i + 64);
			ClientPlayNetworking.send(new W3Net.DigReveal(world, List.copyOf(cells.subList(i, end)), List.copyOf(materials.subList(i, end))));
		}
	}

	/** Dug cells of this section in the current the game world, or null. */
	public static long @Nullable [] dugBits(LevelChunk chunk, int sectionY) {
		W3Dig.DugColumn column = chunk.getAttached(W3Dig.DUG);
		return column == null ? null : column.bits(world(), sectionY);
	}

	/** The game needs every dug cell again (it cleared them). */
	public static void resendAll() {
		SEEN.clear();
	}

	// Chunks whose dug cells changed (or the player changed the game world): their sections go to the game.
	private static void pollDug(Minecraft minecraft, ClientLevel level) {
		int world = world();
		boolean worldChanged = world != seenWorld;
		seenWorld = world;
		if (worldChanged) {
			W3Dig.wallsChanged();
		}
		int radius = minecraft.options.getEffectiveRenderDistance() + 1;
		int pcx = minecraft.player.getBlockX() >> 4, pcz = minecraft.player.getBlockZ() >> 4;
		for (int cx = pcx - radius; cx <= pcx + radius; cx++) {
			for (int cz = pcz - radius; cz <= pcz + radius; cz++) {
				long key = ChunkPos.pack(cx, cz);
				LevelChunk chunk = level.getChunkSource().getChunk(cx, cz, ChunkStatus.FULL, false);
				if (chunk == null) {
					SEEN.remove(key);
					continue;
				}
				W3Dig.DugColumn column = chunk.getAttached(W3Dig.DUG);
				if (column == null) {
					column = NONE;
				}
				W3Dig.DugColumn previous = SEEN.get(key);
				if (previous == column && !worldChanged) {
					continue;
				}
				SEEN.put(key, column);
				for (W3Dig.DugColumn c : new W3Dig.DugColumn[] { column, previous == null ? NONE : previous }) {
					for (W3Dig.DugSection s : c.sections()) {
						if (worldChanged || s.world() == world) {
							// Its walls, and the walls next door (a dug cell can open one up).
							WorldExporter.markDirtyNow(cx, s.sectionY(), cz);
							WorldExporter.markDirty(cx - 1, s.sectionY(), cz);
							WorldExporter.markDirty(cx + 1, s.sectionY(), cz);
							WorldExporter.markDirty(cx, s.sectionY() - 1, cz);
							WorldExporter.markDirty(cx, s.sectionY() + 1, cz);
							WorldExporter.markDirty(cx, s.sectionY(), cz - 1);
							WorldExporter.markDirty(cx, s.sectionY(), cz + 1);
						}
					}
				}
			}
		}
		if (SEEN.size() > 8192) {
			SEEN.clear();
		}
	}
}

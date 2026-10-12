// Derived from SkyCraft (https://github.com/chasmlol/skycraft), Copyright (c) 2026 chasmlol, MIT License.
package dev.minewitcher3.client;

import dev.minewitcher3.world.W3Collision;
import dev.minewitcher3.world.W3Tri;
import dev.minewitcher3.world.TriCollider;
import java.util.ArrayList;
import java.util.List;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.phys.AABB;
import net.minecraft.world.phys.Vec3;

/** Feeds the local player's movement through {@link TriCollider} against nearby the game triangles. */
public final class W3Collider {
	private W3Collider() {
	}

	// Feet this far inside the game's ground (its voxel shell) count as sunk: lifted back on top. Deeper
	// than the shell can stick out over the triangles (1/8 rounding + a steep ramp's half cell, ~0.4).
	private static final double SUNK_PROBE = 0.5;
	private static final double LIFT_MIN = 0.25;
	private static final double LIFT_MAX = 2.0;
	// Triangles must have arrived this far around the player before it may walk there.
	private static final double KNOWN_MARGIN = 0.8;

	public static Vec3 collide(LocalPlayer player, Vec3 move) {
		AABB box = player.getBoundingBox();
		double step = player.maxUpStep();
		double cx = (box.minX + box.maxX) * 0.5, cz = (box.minZ + box.maxZ) * 0.5, y = box.minY;
		if (W3Collision.active() && !player.getAbilities().flying) {
			// The game's collision streams in region by region. Never walk (or fall) into a region whose
			// triangles haven't arrived: there is nothing there to stand on yet, and once they arrive the
			// player would be under the surface. Wait at the edge instead.
			if (!W3Collision.trisKnownAround(cx, y, cz, 0.0)) {
				return new Vec3(0.0, Math.max(0.0, move.y), 0.0);
			}
			if ((move.x != 0.0 || move.z != 0.0) && !W3Collision.trisKnownAround(cx + move.x, y + Math.min(0.0, move.y), cz + move.z, KNOWN_MARGIN)) {
				move = new Vec3(0.0, move.y, 0.0);
			}
			// Sunk into the ground (got under the surface somehow): back on top of it.
			if (W3Collision.insideSolid(cx, y + SUNK_PROBE, cz)) {
				double ground = groundAt(cx, y, cz, LIFT_MAX);
				if (!Double.isNaN(ground) && ground > y + LIFT_MIN) {
					return Entity.collideBoundingBox(player, new Vec3(0.0, ground - y, 0.0), box, player.level(), List.of());
				}
			}
		}
		List<W3Tri> tris = new ArrayList<>();
		W3Collision.trianglesNear(box.expandTowards(move).inflate(1.0, 1.0 + step, 1.0), tris);
		if (tris.isEmpty()) {
			return move;
		}
		double[] r = TriCollider.resolve(tris, cx, y, cz, box.getXsize() * 0.5, box.getYsize(), step, player.onGround(), move.x, move.y, move.z);
		if (r[0] == move.x && r[1] == move.y && r[2] == move.z) {
			return move;
		}
		// The triangle pass (snapping down a slope, pushing out of a wall) can move the player into a
		// Minecraft block placed on the terrain; collide that result with Minecraft blocks again.
		return Entity.collideBoundingBox(player, new Vec3(r[0], r[1], r[2]), box, player.level(), List.of());
	}

	/** Highest the game surface at or below {@code maxAbove} over the feet at (x, y, z), or NaN. */
	public static double groundAt(double x, double y, double z, double maxAbove) {
		List<W3Tri> tris = new ArrayList<>();
		W3Collision.trianglesNear(new AABB(x - 1, y - 4, z - 1, x + 1, y + maxAbove + 1, z + 1), tris);
		return TriCollider.groundAt(tris, x, y, z, maxAbove);
	}
}

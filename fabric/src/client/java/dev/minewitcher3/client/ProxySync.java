// Derived from SkyCraft (https://github.com/chasmlol/skycraft), Copyright (c) 2026 chasmlol, MIT License.
package dev.minewitcher3.client;

import dev.minewitcher3.combat.WitcherActorEntity;
import dev.minewitcher3.link.WitcherLink;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import net.minecraft.client.Minecraft;
import net.minecraft.world.entity.Entity;

/**
 * Puts the client's copies of the game actor stand-ins exactly where the game has the actors this
 * frame, so the crosshair and melee reach line up with what's on screen (the server copy only
 * moves once per tick and reaches the client a tick or two later).
 */
final class ProxySync {
	private static final List<WitcherLink.Actor> ACTORS = new ArrayList<>();
	private static final Map<Integer, WitcherLink.Actor> BY_ID = new HashMap<>();

	private ProxySync() {
	}

	static void frame(Minecraft minecraft) {
		if (minecraft.level == null || !WitcherLink.readActors(ACTORS)) {
			return;
		}
		BY_ID.clear();
		for (WitcherLink.Actor a : ACTORS) {
			BY_ID.put(a.actorId(), a);
		}
		for (Entity entity : minecraft.level.entitiesForRendering()) {
			if (entity instanceof WitcherActorEntity proxy) {
				WitcherLink.Actor a = BY_ID.get(proxy.actorId());
				if (a == null) {
					continue;
				}
				proxy.setSize(a.width(), a.height());
				proxy.setPos(a.x(), a.y(), a.z());
				proxy.xo = a.x();
				proxy.yo = a.y();
				proxy.zo = a.z();
				proxy.setYRot(a.yaw());
				proxy.yRotO = a.yaw();
			}
		}
	}
}

// Derived from SkyCraft (https://github.com/chasmlol/skycraft), Copyright (c) 2026 chasmlol, MIT License.
package dev.minewitcher3.combat;

import dev.minewitcher3.MineWitcher3;
import dev.minewitcher3.link.Proto;
import dev.minewitcher3.link.WitcherLink;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.Iterator;
import java.util.List;
import java.util.Map;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerTickEvents;
import net.fabricmc.fabric.api.object.builder.v1.entity.FabricDefaultAttributeRegistry;
import net.minecraft.core.Registry;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.core.registries.Registries;
import net.minecraft.network.chat.Component;
import net.minecraft.resources.Identifier;
import net.minecraft.resources.ResourceKey;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.damagesource.DamageSource;
import net.minecraft.world.damagesource.DamageSources;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.EntityType;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.entity.MobCategory;
import org.jspecify.annotations.Nullable;

/**
 * Combat between the Minecraft player and the game actors, server side.
 *
 * <p>Every the game actor near the player gets an invisible {@link WitcherActorEntity} at its exact
 * position. Minecraft weapons hit those like any mob; the resulting damage is sent to the game, which
 * applies it to the real actor (scaled by level) and makes it fight back. The game's hits on the player
 * come back as Minecraft damage from the attacker's stand-in, so armor, shields, knockback, hurt
 * sounds and death all work the Minecraft way.
 */
public final class W3Combat {
	public static final ResourceKey<EntityType<?>> WITCHER_ACTOR_KEY =
		ResourceKey.create(Registries.ENTITY_TYPE, Identifier.fromNamespaceAndPath(MineWitcher3.MOD_ID, "witcher_actor"));
	public static final EntityType<WitcherActorEntity> WITCHER_ACTOR = Registry.register(
		BuiltInRegistries.ENTITY_TYPE,
		WITCHER_ACTOR_KEY,
		EntityType.Builder.<WitcherActorEntity>of(WitcherActorEntity::new, MobCategory.MISC)
			.sized(0.6F, 1.8F)
			.noSave()
			.noSummon()
			.noLootTable()
			.clientTrackingRange(10)
			.updateInterval(1)
			.build(WITCHER_ACTOR_KEY)
	);

	/**
	 * Game damage is divided by this for Minecraft. SkyCraft used 5 (a 15-damage Skyrim bandit swing =
	 * 1.5 hearts). MineWitcher3's game side (mw3_combat.ws, native v6) already converts a Witcher 3 hit
	 * into Minecraft health points (its share of Geralt's max health times 20), so the default is 1;
	 * raise it to soften the game's hits: -Dminewitcher3.damageScale=N.
	 */
	public static final float GAME_TO_MC_DAMAGE = Float.parseFloat(System.getProperty("minewitcher3.damageScale", "1"));

	private static final Map<Integer, WitcherActorEntity> PROXIES = new HashMap<>();
	private static final List<WitcherLink.Actor> ACTORS = new ArrayList<>();

	private W3Combat() {
	}

	public static void init() {
		FabricDefaultAttributeRegistry.register(WITCHER_ACTOR, LivingEntity.createLivingAttributes());
		ServerTickEvents.END_SERVER_TICK.register(W3Combat::serverTick);
	}

	public static @Nullable WitcherActorEntity proxy(int actorId) {
		return PROXIES.get(actorId);
	}

	private static void serverTick(MinecraftServer server) {
		List<ServerPlayer> players = server.getPlayerList().getPlayers();
		if (!WitcherLink.active() || players.isEmpty()) {
			removeAll();
			return;
		}
		ServerLevel level = players.getFirst().level();
		for (ServerPlayer player : players) {
			pickUpNearby(player);
		}
		if (WitcherLink.readActors(ACTORS)) {
			sync(level);
		}
		// Hits land during the tick (melee, sweeps, arrows, fire); send one combined hit per actor.
		for (WitcherActorEntity proxy : PROXIES.values()) {
			float[] hit = proxy.takeHit();
			if (hit != null && (hit[0] > 0.0F || hit[3] > 0.0F)) {
				WitcherLink.pushEvent(
					Proto.EV_HIT_ACTOR, proxy.actorId(), hit[0], hit[1], hit[2], hit[3], Float.floatToRawIntBits(hit[4]), Float.floatToRawIntBits(hit[5])
				);
				MineWitcher3.LOG.info("MineWitcher3: hit {} for {} (knockback {})", proxy.getName().getString(), hit[0], hit[3]);
			}
		}
	}

	private static void sync(ServerLevel level) {
		Map<Integer, WitcherLink.Actor> live = new HashMap<>();
		for (WitcherLink.Actor a : ACTORS) {
			if (!a.dead()) {
				live.put(a.actorId(), a);
			}
		}
		for (Iterator<Map.Entry<Integer, WitcherActorEntity>> it = PROXIES.entrySet().iterator(); it.hasNext(); ) {
			Map.Entry<Integer, WitcherActorEntity> e = it.next();
			WitcherActorEntity proxy = e.getValue();
			if (!live.containsKey(e.getKey()) || proxy.isRemoved() || proxy.level() != level) {
				proxy.discard();
				it.remove();
			}
		}
		int before = PROXIES.size();
		for (WitcherLink.Actor a : live.values()) {
			WitcherActorEntity proxy = PROXIES.get(a.actorId());
			if (proxy == null) {
				proxy = new WitcherActorEntity(WITCHER_ACTOR, level);
				proxy.setActorId(a.actorId());
				proxy.setSize(a.width(), a.height());
				proxy.snapTo(a.x(), a.y(), a.z(), a.yaw(), 0.0F);
				if (!a.name().isEmpty()) {
					proxy.setCustomName(Component.literal(a.name()));
				}
				if (!level.addFreshEntity(proxy)) {
					continue;
				}
				PROXIES.put(a.actorId(), proxy);
				continue;
			}
			proxy.setSize(a.width(), a.height());
			proxy.setPos(a.x(), a.y(), a.z());
			proxy.setYRot(a.yaw());
			proxy.setYHeadRot(a.yaw());
			stepOnTriggers(level, proxy);
		}
		if (PROXIES.size() != before && (PROXIES.size() % 5 == 0 || PROXIES.size() < 5)) {
			MineWitcher3.LOG.info("MineWitcher3: {} the game actors mirrored as hittable stand-ins", PROXIES.size());
		}
	}

	/**
	 * The game's NPCs press pressure plates and trip tripwires. Their stand-ins are placed, not moved
	 * (no physics), so Minecraft never checks what they step into; do it for those blocks here.
	 */
	private static void stepOnTriggers(ServerLevel level, WitcherActorEntity proxy) {
		var box = proxy.getBoundingBox().deflate(1.0E-5);
		var from = net.minecraft.core.BlockPos.containing(box.minX, box.minY, box.minZ);
		var to = net.minecraft.core.BlockPos.containing(box.maxX, box.maxY, box.maxZ);
		for (var pos : net.minecraft.core.BlockPos.betweenClosed(from, to)) {
			var state = level.getBlockState(pos);
			if (state.getBlock() instanceof net.minecraft.world.level.block.BasePressurePlateBlock
				|| state.getBlock() instanceof net.minecraft.world.level.block.TripWireBlock) {
				state.entityInside(level, pos, proxy, net.minecraft.world.entity.InsideBlockEffectApplier.NOOP, true);
			}
		}
	}

	/**
	 * Items and stuck arrows on the game ground rest on its collision voxels, which on steep or rough
	 * terrain can sit a little off from where the player (on the game's exact triangles) stands.
	 * Touch them over a slightly bigger area than vanilla's so walking over them picks them up.
	 * playerTouch applies all of Minecraft's own rules (pickup delay, owner, inventory space).
	 */
	private static void pickUpNearby(ServerPlayer player) {
		if (!player.isAlive() || player.isSpectator()) {
			return;
		}
		for (Entity entity : player.level().getEntities(player, player.getBoundingBox().inflate(1.25, 1.0, 1.25))) {
			if (!entity.isRemoved() && (entity instanceof net.minecraft.world.entity.item.ItemEntity
				|| entity instanceof net.minecraft.world.entity.projectile.arrow.AbstractArrow)) {
				entity.playerTouch(player);
			}
		}
	}

	private static void removeAll() {
		if (PROXIES.isEmpty()) {
			return;
		}
		PROXIES.values().forEach(Entity::discard);
		PROXIES.clear();
	}

	/**
	 * The game hit the player. Runs on the server thread. {@code kind} is a Proto.HURT_* value and
	 * {@code gameDamage} is what the game would have taken off the player's health.
	 */
	public static void hurtPlayer(ServerPlayer player, int kind, float gameDamage, int attackerActorId, int flags) {
		if (!player.isAlive() || gameDamage <= 0.0F) {
			return;
		}
		ServerLevel level = player.level();
		WitcherActorEntity attacker = PROXIES.get(attackerActorId);
		if (attacker != null && attacker.distanceToSqr(player) > 24.0 * 24.0) {
			attacker = null; // a guest's own NPC with the same actor id as one of the host's
		}
		DamageSources sources = level.damageSources();
		DamageSource source = switch (kind) {
			case Proto.HURT_MELEE -> attacker != null ? sources.mobAttack(attacker) : sources.generic();
			case Proto.HURT_PROJECTILE -> attacker != null ? sources.mobProjectile(attacker, attacker) : sources.generic();
			case Proto.HURT_MAGIC -> attacker != null ? sources.indirectMagic(attacker, attacker) : sources.magic();
			default -> sources.generic();
		};
		float damage = gameDamage / GAME_TO_MC_DAMAGE;
		float healthBefore = player.getHealth();
		boolean hurt = player.hurtServer(level, source, damage);
		// (SkyCraft trained Skyrim's Block / Light / Heavy Armor skills here; W3 has no such skills.)
		MineWitcher3.LOG.info("MineWitcher3: The game hit the player for {} ({} Minecraft): health {} -> {}{}", gameDamage, damage, healthBefore, player.getHealth(),
			hurt ? "" : " (blocked/immune)");
		if (hurt && attacker != null && (flags & Proto.HURT_HEAVY_ATTACK) != 0 && !player.isBlocking()) {
			// Heavy attacks shove harder, like a sprint hit does in Minecraft.
			player.knockback(0.5, attacker.getX() - player.getX(), attacker.getZ() - player.getZ(), source, damage);
		}
	}

	/** Actor id of the game actor behind a damage source, or 0. */
	public static int attackerActorId(DamageSource source) {
		return source.getEntity() instanceof WitcherActorEntity proxy ? proxy.actorId() : 0;
	}
}

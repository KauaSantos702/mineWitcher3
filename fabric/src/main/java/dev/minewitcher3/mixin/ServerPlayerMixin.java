// Derived from SkyCraft (https://github.com/chasmlol/skycraft), Copyright (c) 2026 chasmlol, MIT License.
package dev.minewitcher3.mixin;

import dev.minewitcher3.MineWitcher3;
import dev.minewitcher3.combat.W3Combat;
import dev.minewitcher3.combat.WitcherActorEntity;
import dev.minewitcher3.link.Proto;
import dev.minewitcher3.link.WitcherLink;
import net.minecraft.world.damagesource.DamageSource;
import net.minecraft.world.entity.Entity;
import net.minecraft.server.level.ServerPlayer;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;
import net.minecraft.core.BlockPos;
import net.minecraft.server.level.ServerLevel;

@Mixin(ServerPlayer.class)
public abstract class ServerPlayerMixin {
	/** Critical hits on the game actor are flagged so the game can play them up. */
	@Inject(method = "crit", at = @At("HEAD"))
	private void mw3$critGame(Entity entity, CallbackInfo ci) {
		if (entity instanceof WitcherActorEntity proxy) {
			proxy.markCritical();
		}
	}

	/**
	 * No safe-spot search on respawn. Vanilla's PlayerSpawnFinder blocks the server thread
	 * (managedBlock + join) testing up to (2r+1)^2 candidates and then scanning the whole 2048-block
	 * column; in the void mirror world none qualifies, so every respawn froze the server for 14-20 s
	 * (research/08). The game teleports the player to its own position right after anyway.
	 */
	@Inject(method = "adjustSpawnLocation", at = @At("HEAD"), cancellable = true)
	private void mw3$noSpawnSearch(ServerLevel level, BlockPos pos, CallbackInfoReturnable<BlockPos> cir) {
		cir.setReturnValue(pos);
	}

	/** Dying in Minecraft is dying in the game: the host's through the link, a guest's through theirs. */
	@Inject(method = "die", at = @At("HEAD"))
	private void mw3$diesInGame(DamageSource source, CallbackInfo ci) {
		ServerPlayer self = (ServerPlayer) (Object) this;
		int attacker = W3Combat.attackerActorId(source);
		if (!dev.minewitcher3.net.W3Net.isHost(self)) {
			if (net.fabricmc.fabric.api.networking.v1.ServerPlayNetworking.canSend(self, dev.minewitcher3.net.W3Net.Died.TYPE)) {
				net.fabricmc.fabric.api.networking.v1.ServerPlayNetworking.send(self, new dev.minewitcher3.net.W3Net.Died(attacker));
			}
			MineWitcher3.LOG.info("MineWitcher3: guest {} died ({}); telling their game", self.getPlainTextName(), source.getMsgId());
			return;
		}
		if (WitcherLink.active()) {
			WitcherLink.pushEvent(Proto.EV_PLAYER_DIED, attacker, 0, 0, 0, 0, 0);
			MineWitcher3.LOG.info("MineWitcher3: player died ({}); telling the game", source.getMsgId());
		}
	}
}

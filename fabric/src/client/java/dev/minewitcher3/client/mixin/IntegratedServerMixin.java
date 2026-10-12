// Derived from SkyCraft (https://github.com/chasmlol/skycraft), Copyright (c) 2026 chasmlol, MIT License.
package dev.minewitcher3.client.mixin;

import net.minecraft.client.server.IntegratedServer;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * A world opened to friends takes up to 100 players, not Minecraft's fixed 8 for LAN worlds (it's the
 * host's own PC doing the serving, over e4mc).
 */
@Mixin(IntegratedServer.class)
public abstract class IntegratedServerMixin {
	@Inject(method = "getMaxPlayers", at = @At("HEAD"), cancellable = true)
	private void mw3$morePlayers(CallbackInfoReturnable<Integer> cir) {
		cir.setReturnValue(100);
	}
}

// Derived from SkyCraft (https://github.com/chasmlol/skycraft), Copyright (c) 2026 chasmlol, MIT License.
package dev.minewitcher3.mixin;

import dev.minewitcher3.link.WitcherLink;
import net.minecraft.server.MinecraftServer;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * Guests stand on their own game's ground, which this server only knows around the host; to it
 * they'd seem to hover and be kicked for flying. Their own clients keep them on the ground.
 */
@Mixin(MinecraftServer.class)
public abstract class MinecraftServerFlightMixin {
	@Inject(method = "allowFlight", at = @At("HEAD"), cancellable = true)
	private void mw3$guestsStandOnTheirGame(CallbackInfoReturnable<Boolean> cir) {
		if (WitcherLink.active()) {
			cir.setReturnValue(true);
		}
	}
}

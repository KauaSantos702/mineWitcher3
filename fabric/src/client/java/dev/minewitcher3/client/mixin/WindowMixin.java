// Derived from SkyCraft (https://github.com/chasmlol/skycraft), Copyright (c) 2026 chasmlol, MIT License.
package dev.minewitcher3.client.mixin;

import com.mojang.blaze3d.platform.Window;
import dev.minewitcher3.client.W3Client;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/** The MC window is hidden while linked; the game has the real focus, so pretend we do too. */
@Mixin(Window.class)
public abstract class WindowMixin {
	@Inject(method = "isFocused", at = @At("HEAD"), cancellable = true)
	private void mw3$focused(CallbackInfoReturnable<Boolean> cir) {
		if (W3Client.tookOver()) {
			// Focused while the game is connected; if the game goes away, act unfocused so MC
			// never tries to grab the (hidden) mouse.
			cir.setReturnValue(W3Client.linked());
		}
	}

	@Inject(method = "isIconified", at = @At("HEAD"), cancellable = true)
	private void mw3$notIconified(CallbackInfoReturnable<Boolean> cir) {
		if (W3Client.linked()) {
			cir.setReturnValue(false);
		}
	}
}

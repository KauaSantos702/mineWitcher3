// Derived from SkyCraft (https://github.com/chasmlol/skycraft), Copyright (c) 2026 chasmlol, MIT License.
package dev.minewitcher3.client.mixin;

import com.mojang.blaze3d.platform.InputConstants;
import com.mojang.blaze3d.platform.Window;
import dev.minewitcher3.client.InputBridge;
import dev.minewitcher3.client.W3Client;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/** Keyboard state and mouse capture come from the game while linked, not from SDL. */
@Mixin(InputConstants.class)
public abstract class InputConstantsMixin {
	@Inject(method = "isKeyDown", at = @At("HEAD"), cancellable = true)
	private static void mw3$isKeyDown(int key, CallbackInfoReturnable<Boolean> cir) {
		if (W3Client.tookOver()) {
			cir.setReturnValue(InputBridge.isKeyDown(key));
		}
	}

	@Inject(method = "grabMouse", at = @At("HEAD"), cancellable = true)
	private static void mw3$grabMouse(Window window, double xpos, double ypos, CallbackInfo ci) {
		if (W3Client.tookOver()) {
			ci.cancel();
		}
	}

	@Inject(method = "releaseMouse", at = @At("HEAD"), cancellable = true)
	private static void mw3$releaseMouse(Window window, double xpos, double ypos, CallbackInfo ci) {
		if (W3Client.tookOver()) {
			ci.cancel();
		}
	}
}

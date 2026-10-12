// Derived from SkyCraft (https://github.com/chasmlol/skycraft), Copyright (c) 2026 chasmlol, MIT License.
package dev.minewitcher3.mixin;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import dev.minewitcher3.world.W3Clip;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.level.ClipContext;
import net.minecraft.world.level.Level;
import net.minecraft.world.phys.BlockHitResult;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;

/**
 * The crosshair targets the game surfaces: blocks can be placed on terrain and walls, and NPCs behind a
 * wall can't be hit through it. A game hit points at the cell a placed block would occupy.
 */
@Mixin(Entity.class)
public abstract class EntityPickMixin {
	@WrapOperation(
		method = "pick",
		at = @At(value = "INVOKE", target = "Lnet/minecraft/world/level/Level;clip(Lnet/minecraft/world/level/ClipContext;)Lnet/minecraft/world/phys/BlockHitResult;")
	)
	private BlockHitResult mw3$pickGame(Level level, ClipContext context, Operation<BlockHitResult> original) {
		return W3Clip.refine(context.getFrom(), context.getTo(), original.call(level, context), W3Clip.Use.PICK);
	}
}

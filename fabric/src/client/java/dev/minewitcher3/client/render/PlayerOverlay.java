// Derived from SkyCraft (https://github.com/chasmlol/skycraft), Copyright (c) 2026 chasmlol, MIT License.
package dev.minewitcher3.client.render;

import com.mojang.blaze3d.ProjectionType;
import com.mojang.blaze3d.pipeline.RenderTarget;
import com.mojang.blaze3d.platform.Lighting;
import com.mojang.blaze3d.systems.RenderSystem;
import com.mojang.blaze3d.vertex.PoseStack;
import com.mojang.renderpearl.api.buffers.GpuBufferSlice;
import com.mojang.renderpearl.api.commands.RenderPass;
import dev.minewitcher3.MineWitcher3;
import java.util.Optional;
import java.util.OptionalDouble;
import net.minecraft.client.Camera;
import net.minecraft.client.Minecraft;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.client.renderer.GameRenderer;
import net.minecraft.client.renderer.ProjectionMatrixBuffer;
import net.minecraft.client.renderer.SubmitNodeStorage;
import net.minecraft.client.renderer.entity.EntityRenderDispatcher;
import net.minecraft.client.renderer.entity.state.EntityRenderState;
import net.minecraft.client.renderer.feature.FeatureRenderDispatcher;
import net.minecraft.client.renderer.state.level.CameraRenderState;
import org.joml.Matrix4fStack;

/**
 * Interim avatar (research/09-avatar-render.md, "Plano interino"): while linked the level is not drawn,
 * but in the F5 modes Minecraft still draws the local player (skin, armour, cape, held items) into
 * the main target, so it lands in the exported overlay exactly where the shared camera sees it.
 *
 * <p>Copies GameRenderer.renderItemInHand's pipeline (SubmitNodeStorage → prepareFrame →
 * render pass on main colour+depth, no clear → renderAllFeatures) plus the level pass state
 * (model-view = view rotation, shader fog, Lighting.LEVEL). The projection is the un-bobbed
 * {@code cam.projectionMatrix}, since the W3 camera has no view bob. No shadow, no name tag.
 */
public final class PlayerOverlay {
	private static SubmitNodeStorage storage;
	private static ProjectionMatrixBuffer projection;
	private static boolean failed;

	private PlayerOverlay() {
	}

	/** Called from LevelRenderer.render HEAD (world rendering is cancelled right after). */
	public static void render(Minecraft mc, CameraRenderState cam, GpuBufferSlice fog) {
		if (failed || cam == null) {
			return;
		}
		LocalPlayer player = mc.player;
		GameRenderer gr = mc.gameRenderer;
		if (player == null || gr == null || player.isSpectator()) {
			return;
		}
		Camera camera = gr.mainCamera();
		if (camera == null || !camera.isDetached()) {
			return; // first person: hand only (drawn by GameRenderer as usual)
		}
		try {
			draw(mc, gr, camera, player, cam, fog);
		} catch (Throwable t) {
			failed = true; // never take the frame down; log once and stop trying
			MineWitcher3.LOG.error("[MW3] player overlay failed, disabled", t);
		}
	}

	private static void draw(Minecraft mc, GameRenderer gr, Camera camera, LocalPlayer player,
			CameraRenderState cam, GpuBufferSlice fog) {
		if (storage == null) {
			storage = new SubmitNodeStorage();
			storage.setUseImprovedTransparency(false); // classic transparency path, like the hand
			projection = new ProjectionMatrixBuffer("MineWitcher3 player");
		}
		EntityRenderDispatcher d = mc.getEntityRenderDispatcher();
		d.prepare(camera, mc.crosshairPickEntity);
		EntityRenderState s = d.extractEntity(player, cam.cameraEntityPartialTicks);
		s.shadowPieces.clear();
		s.shadowRadius = 0.0F;
		s.nameTag = null;
		d.submit(s, cam, s.x - cam.pos.x, s.y - cam.pos.y, s.z - cam.pos.z, new PoseStack(), storage);

		Matrix4fStack mv = RenderSystem.getModelViewStack();
		mv.pushMatrix();
		mv.mul(cam.viewRotationMatrix);
		RenderSystem.backupProjectionMatrix();
		try {
			RenderSystem.setProjectionMatrix(projection.getBuffer(cam.projectionMatrix), ProjectionType.PERSPECTIVE);
			RenderSystem.setShaderFog(fog);
			gr.lighting().setupFor(Lighting.Entry.LEVEL);
			RenderTarget target = gr.mainRenderTarget();
			try (FeatureRenderDispatcher.PreparedFrame frame = gr.featureRenderDispatcher().prepareFrame(storage);
					RenderPass pass = RenderSystem.getDevice().createCommandEncoder().createRenderPass(
						() -> "MineWitcher3 player", target.getColorTextureView(), Optional.empty(),
						target.getDepthTextureView(), OptionalDouble.empty())) {
				RenderSystem.bindDefaultUniforms(pass);
				FeatureRenderDispatcher.renderAllFeatures(pass, frame);
			}
		} finally {
			RenderSystem.restoreProjectionMatrix();
			mv.popMatrix();
		}
	}
}

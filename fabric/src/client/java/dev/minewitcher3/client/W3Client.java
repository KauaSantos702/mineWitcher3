// Derived from SkyCraft (https://github.com/chasmlol/skycraft), Copyright (c) 2026 chasmlol, MIT License.
package dev.minewitcher3.client;

import dev.minewitcher3.MineWitcher3;
import dev.minewitcher3.client.render.WorldExporter;
import dev.minewitcher3.link.Proto;
import dev.minewitcher3.link.WitcherLink;
import dev.minewitcher3.world.W3Collision;
import net.minecraft.client.Camera;
import net.minecraft.client.Minecraft;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.phys.Vec3;
import org.lwjgl.sdl.SDLVideo;

/**
 * Per-frame glue between the Minecraft client and the game. Everything here runs on the render
 * thread, called from MinecraftMixin.
 */
public final class W3Client {
	private static final boolean SHOW_WINDOW = Boolean.getBoolean("minewitcher3.showWindow");
	// Started by the game (MineWitcher3's bundled instance passes -Dminewitcher3.startHidden=true): no window and
	// no title-screen music from the first frame, even while the game is paused (Alt-Tabbed) and the
	// two haven't linked up yet. Otherwise the window only goes once the game is there.
	private static final boolean START_HIDDEN = Boolean.getBoolean("minewitcher3.startHidden");
	private static boolean startedHidden;

	private static final WitcherLink.GameState sky = new WitcherLink.GameState();
	private static final WitcherLink.McState mc = new WitcherLink.McState();
	private static volatile boolean linked;
	private static boolean tookOver;
	private static boolean windowHidden;
	private static int appliedViewportW, appliedViewportH;

	// Teleport / hold state: The game decides where the player is after loads, doors and respawns.
	private static int lastTeleportSeq = -1;
	private static int teleportAck;
	private static boolean teleportPending;
	private static LocalPlayer lastPlayer;
	private static Vec3 holdPos;
	private static Vec3 unlinkedHold;
	private static long holdSince;
	private static long holdRetryAt;
	private static final double HOLD_RADIUS = 2.0; // blocks around the feet whose triangles must be there
	private static final double HOLD_GROUND = 0.5; // ground within this of the feet releases the hold
	// Triangles there but no ground under the feet (a ledge, a jump): let go after this. Was 6 s, which
	// froze the player after every resync (research/08 section 2).
	private static final long HOLD_MIDAIR_MS = 1000;
	// Still no collision from the game after this: ask for the teleport again (never release into a
	// free fall: that is what killed the player, research/07 3.1).
	private static final long HOLD_RETRY_MS = 10000;
	private static long qpcFreq;
	private static LocalPlayer eyePlayer;
	private static float eyeSmoothed;
	private static long frameCounter;
	private static int lastPacedSeq;
	private static boolean gameStalled;
	private static int exporterErrors;

	private W3Client() {
	}

	public static boolean linked() {
		return linked;
	}

	/**
	 * True once the game has connected in this session. From then on Minecraft never touches the
	 * real mouse or keyboard again (even if the game closes), since its window is hidden.
	 */
	public static boolean tookOver() {
		return tookOver;
	}

	public static WitcherLink.GameState sky() {
		return sky;
	}

	/** Start of Minecraft.runTick: pull state and input from the game before anything else runs. */
	public static void beginFrame() {
		WitcherLink.poll();
		quitWithGame(Minecraft.getInstance());
		if (START_HIDDEN && !startedHidden) {
			startedHidden = true;
			Minecraft minecraft = Minecraft.getInstance();
			hideWindowOnce(minecraft);
			minecraft.options.getSoundSourceOptionInstance(net.minecraft.sounds.SoundSource.MUSIC).set(0.0);
			minecraft.getMusicManager().stopPlaying();
		}
		boolean nowLinked = WitcherLink.active();
		if (nowLinked) {
			WitcherLink.readGameState(sky); // on a torn read we simply keep last frame's state
			dev.minewitcher3.world.W3Water.refresh();
		} else {
			dev.minewitcher3.world.W3Water.clear();
		}
		if (nowLinked != linked) {
			linked = nowLinked;
			MineWitcher3.LOG.info("MineWitcher3: The game link {}", linked ? "up" : "down");
			if (linked) {
				tookOver = true;
				unlinkedHold = null;
				W3Collision.startConsumer();
				applyLinkedOptions();
			} else {
				InputBridge.releaseAll();
				LocalPlayer player = Minecraft.getInstance().player;
				unlinkedHold = player != null ? player.position() : null;
			}
		}
		if (!linked) {
			return;
		}

		Minecraft minecraft = Minecraft.getInstance();
		hideWindowOnce(minecraft);
		applyViewportSize(minecraft);
		MirrorWorld.openWhenReady(minecraft);

		if (sky.menuOpen() || sky.loading()) {
			InputBridge.releaseAll();
		}
		InputBridge.drain(minecraft);
		ProxySync.frame(minecraft);

		LocalPlayer player = minecraft.player;
		if (player == null) {
			lastPlayer = null;
			return;
		}

		// A new player object means we just joined or respawned: put it where the game's player is.
		if (player != lastPlayer) {
			lastPlayer = player;
			teleportPending = true;
		}
		if (sky.teleportSeq != lastTeleportSeq) {
			lastTeleportSeq = sky.teleportSeq;
			teleportPending = true;
		}
		if (teleportPending && sky.inGame() && !sky.loading()) {
			requestTeleport(minecraft, sky.x, sky.y, sky.z, sky.yaw, sky.pitch);
			teleportAck = sky.teleportSeq;
			teleportPending = false;
			holdPos = new Vec3(sky.x, sky.y, sky.z);
			// A fresh hold: its timers start now, not when an earlier (menu/loading) hold began.
			holdSince = System.currentTimeMillis();
			holdRetryAt = holdSince + HOLD_RETRY_MS;
		}

		// Look direction is driven by the game (zero-latency camera); MC uses it for everything else.
		if (minecraft.gui.screen() == null) {
			player.setYRot(sky.yaw);
			player.setXRot(sky.pitch);
			player.yRotO = sky.yaw;
			player.xRotO = sky.pitch;
		}
	}

	// Minecraft is started with the game (the native plugin (mw3.asi) launches it), so it goes when that game has
	// closed for good: saved and shut down the normal way. -Dminewitcher3.quitWithGame=false keeps it
	// running instead (development: restarting the game without restarting Minecraft).
	private static final boolean QUIT_WITH_GAME = Boolean.parseBoolean(System.getProperty("minewitcher3.quitWithGame", "true"));
	private static long gameGoneSince;
	private static long nextGameCheck;
	// Started hidden by the game but never connected: nobody can see or use this Minecraft, and it
	// would stop the next the game from starting a fresh one ("already running"). It goes after this.
	private static final long NEVER_CONNECTED_QUIT_MS = 10 * 60 * 1000;
	private static final long STARTED_AT = System.currentTimeMillis();
	private static boolean gaveUpWaiting;

	private static void quitWithGame(Minecraft minecraft) {
		int pid = WitcherLink.gamePid();
		long now = System.currentTimeMillis();
		if (QUIT_WITH_GAME && START_HIDDEN && pid == 0 && !tookOver && !gaveUpWaiting && now - STARTED_AT > NEVER_CONNECTED_QUIT_MS) {
			gaveUpWaiting = true;
			MineWitcher3.LOG.warn("MineWitcher3: started hidden but the game never connected in {} minutes; quitting", NEVER_CONNECTED_QUIT_MS / 60000);
			minecraft.stop();
			return;
		}
		if (!QUIT_WITH_GAME || pid == 0 || now < nextGameCheck) {
			return;
		}
		nextGameCheck = now + 1000;
		if (ProcessHandle.of(pid).map(ProcessHandle::isAlive).orElse(false)) {
			gameGoneSince = 0;
			return;
		}
		if (gameGoneSince == 0) {
			gameGoneSince = now;
		} else if (now - gameGoneSince > 5000) {
			MineWitcher3.LOG.info("MineWitcher3: The game (pid {}) has closed; saving and quitting", pid);
			minecraft.stop();
		}
	}

	/** Called at the end of every client tick. */
	public static void clientTick(Minecraft minecraft) {
		MirrorWorld.tick(minecraft);
		W3DigClient.tick(minecraft);
		freezeWhileUnlinked(minecraft);
		holdUntilReady(minecraft);
		publishTick(minecraft);
	}

	/**
	 * The game went quiet (a long loading screen, a stall, or it closed). Its collision around the
	 * player may be about to change (interior doors), so keep the player exactly where they were
	 * instead of letting them fall; the game puts them where they belong when it's back.
	 */
	private static void freezeWhileUnlinked(Minecraft minecraft) {
		LocalPlayer player = minecraft.player;
		if (linked || !tookOver || player == null) {
			return;
		}
		if (unlinkedHold == null) {
			unlinkedHold = player.position();
		}
		player.setDeltaMovement(Vec3.ZERO);
		player.setPos(unlinkedHold.x, unlinkedHold.y, unlinkedHold.z);
		player.xo = unlinkedHold.x;
		player.yo = unlinkedHold.y;
		player.zo = unlinkedHold.z;
		player.resetFallDistance();
	}

	/**
	 * Hands the game the raw physics tick (previous + latest feet, smoothed eye height, walk bob) with a
	 * QueryPerformanceCounter timestamp. The game interpolates between them on its own frame clock,
	 * exactly like Minecraft's renderer does with partial ticks.
	 */
	private static void publishTick(Minecraft minecraft) {
		LocalPlayer player = minecraft.player;
		if (!linked || player == null) {
			return;
		}
		if (qpcFreq == 0) {
			qpcFreq = WitcherLink.qpcFrequency();
		}
		float tickMs = minecraft.level != null ? minecraft.level.tickRateManager().millisecondsPerTick() : 50.0F;
		// The tick really "happened" partial ticks ago (DeltaTracker keeps the remainder).
		float remainder = minecraft.getDeltaTracker().getGameTimeDeltaPartialTick(false);
		mc.tickQpc = WitcherLink.qpc() - (long) (remainder * tickMs * qpcFreq / 1000.0);
		mc.tickMs = tickMs;
		mc.prevX = player.xo;
		mc.prevY = player.yo;
		mc.prevZ = player.zo;
		mc.curX = player.getX();
		mc.curY = player.getY();
		mc.curZ = player.getZ();
		// Same smoothing as Camera.tick(): eye height eases halfway toward the target each tick.
		if (player != eyePlayer) {
			eyePlayer = player;
			eyeSmoothed = player.getEyeHeight();
		}
		mc.eyeHeightO = eyeSmoothed;
		eyeSmoothed += (player.getEyeHeight() - eyeSmoothed) * 0.5F;
		mc.eyeHeightT = eyeSmoothed;
		boolean bob = minecraft.options.bobView().get();
		var avatar = player.avatarState();
		mc.walkDistO = bob ? avatar.getInterpolatedWalkDistance(0.0F) : 0.0F;
		mc.walkDist = bob ? avatar.getInterpolatedWalkDistance(1.0F) : 0.0F;
		mc.bobO = bob ? avatar.getInterpolatedBob(0.0F) : 0.0F;
		mc.bob = bob ? avatar.getInterpolatedBob(1.0F) : 0.0F;
		WitcherLink.writeMcState(mc);
	}

	/** Freeze the player until the game's collision around them has arrived. */
	private static void holdUntilReady(Minecraft minecraft) {
		LocalPlayer player = minecraft.player;
		if (!linked || player == null) {
			return;
		}
		long now = System.currentTimeMillis();
		if (!sky.inGame() || sky.loading()) {
			// The game is on its main menu or a loading screen: park the player where they are. The
			// hold's clock only starts once the game is back (a 5-minute menu hold used to be "timed
			// out" the moment the first real teleport came in).
			if (holdPos == null) {
				holdPos = player.position();
			}
			teleportPending = true;
			holdSince = now;
			holdRetryAt = now + HOLD_RETRY_MS;
		}
		if (holdPos == null) {
			holdSince = 0;
			return;
		}
		if (holdSince == 0) {
			holdSince = now;
			holdRetryAt = now + HOLD_RETRY_MS;
		}
		// Release once the player's own triangles and its neighbours' (2 blocks around) have arrived and
		// there is ground within half a block of the feet. Voxels alone aren't enough: the player
		// collides with the triangles, and walking into a region without them sinks it under the
		// surface. Mid-air on purpose (no ground near the feet even with the triangles there): after
		// HOLD_MIDAIR_MS. Without the triangles the player is never let go: it keeps holding and asks
		// the game for the teleport again every HOLD_RETRY_MS.
		long held = now - holdSince;
		boolean trisReady = W3Collision.trisKnownAround(holdPos.x, holdPos.y, holdPos.z, HOLD_RADIUS);
		double ground = trisReady ? W3Collider.groundAt(holdPos.x, holdPos.y, holdPos.z, HOLD_GROUND) : Double.NaN;
		boolean grounded = !Double.isNaN(ground) && ground >= holdPos.y - HOLD_GROUND;
		boolean ready = trisReady && (grounded || held > HOLD_MIDAIR_MS);
		if (!ready && sky.inGame() && !sky.loading() && now >= holdRetryAt) {
			holdRetryAt = now + HOLD_RETRY_MS;
			MineWitcher3.LOG.warn("MineWitcher3: no collision from the game at {} after {} ms; still holding, asking for the teleport again", holdPos, held);
			teleportPending = true; // beginFrame re-teleports to where the game's player is now
		}
		if (ready && sky.inGame() && !sky.loading()) {
			// Stand exactly on the game's ground (its feet can be a little above or inside it).
			if (grounded && ground != holdPos.y) {
				player.setPos(holdPos.x, ground, holdPos.z);
				player.yo = ground;
				MineWitcher3.LOG.info("MineWitcher3: placed player {} blocks onto the ground", String.format("%.3f", ground - holdPos.y));
			}
			MineWitcher3.LOG.info("MineWitcher3: hold released after {} ms ({})", held, grounded ? "ground" : "mid-air");
			holdPos = null;
			return;
		}
		player.setDeltaMovement(Vec3.ZERO);
		player.setPos(holdPos.x, holdPos.y, holdPos.z);
		player.xo = holdPos.x;
		player.yo = holdPos.y;
		player.zo = holdPos.z;
		player.resetFallDistance();
	}

	private static void requestTeleport(Minecraft minecraft, double x, double y, double z, float yaw, float pitch) {
		LocalPlayer player = minecraft.player;
		player.setPos(x, y, z);
		player.setDeltaMovement(Vec3.ZERO);
		player.resetFallDistance();
		var server = minecraft.getSingleplayerServer();
		if (server != null) {
			var uuid = player.getUUID();
			server.execute(() -> {
				ServerPlayer sp = server.getPlayerList().getPlayer(uuid);
				if (sp != null) {
					sp.teleportTo(x, y, z);
					sp.setYRot(yaw);
					sp.setXRot(pitch);
					sp.resetFallDistance();
				}
			});
		}
		MineWitcher3.LOG.info("MineWitcher3: teleported to {} {} {}", x, y, z);
	}

	/** After GameRenderer.render(): report the player to the game and ship the overlay frame. */
	public static void afterRender() {
		if (!linked) {
			return;
		}
		Minecraft minecraft = Minecraft.getInstance();
		LocalPlayer player = minecraft.player;
		int flags = 0;
		if (player != null && minecraft.level != null) {
			float partial = minecraft.getDeltaTracker().getGameTimeDeltaPartialTick(false);
			Vec3 feet = player.getPosition(partial);
			Camera camera = minecraft.gameRenderer.mainCamera();
			flags |= Proto.MC_IN_WORLD;
			if (player.onGround()) {
				flags |= Proto.MC_ON_GROUND;
			}
			if (player.isShiftKeyDown()) {
				flags |= Proto.MC_SNEAKING;
			}
			if (player.isSprinting()) {
				flags |= Proto.MC_SPRINTING;
			}
			if (player.isDeadOrDying()) {
				flags |= Proto.MC_DEAD;
			}
			if (player.isSwimming()) {
				flags |= Proto.MC_SWIMMING;
			}
			if (player.getAbilities().flying) {
				flags |= Proto.MC_FLYING;
			}
			mc.x = feet.x;
			mc.y = feet.y;
			mc.z = feet.z;
			mc.yaw = player.getYRot();
			mc.pitch = player.getXRot();
			// The eye, not the camera: in third person Minecraft's camera sits behind or in front.
			Vec3 eye = camera.isDetached() ? player.getEyePosition(partial) : camera.position();
			mc.eyeHeight = (float) (eye.y - feet.y);
			mc.eyeX = eye.x;
			mc.eyeY = eye.y;
			mc.eyeZ = eye.z;
			mc.fov = camera.getFov();
			// Minecraft's F5 camera: The game puts its camera where Minecraft's would be.
			mc.cameraMode = minecraft.options.getCameraType().ordinal();
			mc.cameraDistance = camera.isDetached() ? (float) camera.position().distanceTo(player.getEyePosition(partial)) : 0.0F;
			// Walk bob, exactly what GameRenderer.bobView() uses this frame.
			var entityState = minecraft.gameRenderer.gameRenderState().levelRenderState.cameraRenderState.entityRenderState;
			boolean bob = minecraft.options.bobView().get() && entityState.isPlayer;
			mc.bobPhase = bob ? entityState.backwardsInterpolatedWalkDistance : 0.0F;
			mc.bobAmount = bob ? entityState.bob : 0.0F;
		}
		if (minecraft.gui.screen() != null) {
			flags |= Proto.MC_SCREEN_OPEN;
		}
		mc.flags = flags;
		mc.sensitivity = minecraft.options.sensitivity().get().floatValue();
		// Not "arrived" until we are released, nor while a teleport is still to be done, nor in the frame
		// a new player object (respawn) appeared after beginFrame: its feet are the world spawn, and
		// the game must never follow them (research/07 3.2).
		boolean arrived = holdPos == null && !teleportPending && player != null && player == lastPlayer;
		mc.teleportAck = arrived ? teleportAck : teleportAck - 1;
		mc.guiScale = minecraft.getWindow().getGuiScale();
		mc.frameCounter = ++frameCounter;
		WitcherLink.writeMcState(mc);

		WorldExporter.setEnabled(sky.worldRender());
		if ((flags & Proto.MC_IN_WORLD) != 0) {
			try {
				WorldExporter.frame(minecraft, minecraft.getDeltaTracker().getGameTimeDeltaPartialTick(false));
			} catch (RuntimeException e) {
				if (exporterErrors++ < 5) {
					MineWitcher3.LOG.error("MineWitcher3: world export failed", e);
				}
			}
			FrameExporter.capture(minecraft);
		}
	}

	/** End of the frame: render at most once per the game frame instead of spinning freely. */
	public static void paceFrame() {
		if (!linked) {
			return;
		}
		if (gameStalled && (WitcherLink.gameStateSeq() >>> 1) == lastPacedSeq) {
			return; // The game is paused (menu / alt-tab): don't block every frame waiting for it
		}
		gameStalled = false;
		long deadline = System.nanoTime() + 25_000_000L;
		// GameState.seq advances by 2 per the game frame (odd while writing).
		while ((WitcherLink.gameStateSeq() >>> 1) == lastPacedSeq && System.nanoTime() < deadline) {
			Thread.onSpinWait();
			if (deadline - System.nanoTime() > 2_000_000L) {
				Thread.yield();
			}
		}
		int seqNow = WitcherLink.gameStateSeq() >>> 1;
		gameStalled = seqNow == lastPacedSeq;
		lastPacedSeq = seqNow;
	}

	private static void applyLinkedOptions() {
		Minecraft minecraft = Minecraft.getInstance();
		var options = minecraft.options;
		options.pauseOnLostFocus = false;
		options.vignette().set(false);
		options.enableVsync().set(false);
		options.framerateLimit().set(260);
		// Minecraft doesn't draw the world itself; these only decide how far out placed blocks,
		// arrows and the game NPC stand-ins stay loaded and simulated.
		// 4, not 8: 81 chunks of 128 sections per long teleport instead of 289 (research/08 #5).
		options.renderDistance().set(4);
		options.simulationDistance().set(4);
		options.autoJump().set(false);
		// "afk" drops to 30 / 10 fps after 1 / 10 minutes without (Minecraft's own) input, which the
		// hidden window never gets (FramerateLimitTrackerMixin also overrides it while linked).
		options.inactivityFpsLimit().set(net.minecraft.client.InactivityFpsLimit.MINIMIZED);
		options.onboardAccessibility = false;
		if (options.tutorialStep != net.minecraft.client.tutorial.TutorialSteps.NONE) {
			minecraft.getTutorial().setStep(net.minecraft.client.tutorial.TutorialSteps.NONE);
		}
		options.getSoundSourceOptionInstance(net.minecraft.sounds.SoundSource.MUSIC).set(0.0);
		options.save();
	}

	private static void hideWindowOnce(Minecraft minecraft) {
		if (windowHidden || SHOW_WINDOW) {
			return;
		}
		windowHidden = true;
		SDLVideo.SDL_HideWindow(minecraft.getWindow().handle());
		MineWitcher3.LOG.info("MineWitcher3: game window hidden (run with -Dminewitcher3.showWindow=true to keep it)");
	}

	private static void applyViewportSize(Minecraft minecraft) {
		int w = Math.min(sky.viewportW, Proto.MAX_OVERLAY_W);
		int h = Math.min(sky.viewportH, Proto.MAX_OVERLAY_H);
		if (w <= 0 || h <= 0 || (w == appliedViewportW && h == appliedViewportH)) {
			return;
		}
		appliedViewportW = w;
		appliedViewportH = h;
		minecraft.getWindow().setWindowed(w, h);
		MineWitcher3.LOG.info("MineWitcher3: sizing overlay to the game viewport {}x{}", w, h);
	}
}

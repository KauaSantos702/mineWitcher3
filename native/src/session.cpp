// Derived from SkyCraft (https://github.com/chasmlol/skycraft), Copyright (c) 2026 chasmlol, MIT License.
// Game-side runtime of the link, adapted from skse/src/Game.cpp (PerFrame) and skse/src/Input.cpp
// (ConsumeLook / ReleaseAll). See THIRD-PARTY-NOTICES.md.
#include "session.h"

#include "combat.h"
#include "config.h"
#include "input_hook.h"
#include "log.h"
#include "motion.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>

namespace mw3::session
{
	namespace
	{
		// O script manda o estado a cada frame de gameplay (OnGameCameraPostTick). Se parar, o W3 esta
		// pausado num menu, numa cena ou carregando: o input volta todo para o W3.
		constexpr std::uint64_t kScriptStaleMs = 300;
		// Referencia = ultimo teleporte do MC ou, com o fantoche (v4), o ultimo ponto para onde o script
		// moveu o Geralt (NotePuppetTarget). Se o Geralt se afastar disso, foi o W3 que o moveu (cutscene,
		// porta de loading, fast travel, empurrao): teleporta o MC para la. 4.5 m ~ as 300 unidades
		// (~4.3 blocos) do SkyCraft (Game.cpp:19, 582); 2 m disparava com o Geralt so assentando.
		constexpr double kResyncDistance = 4.5;
		// Depois de loading / troca de mundo / reconexao: nem teleporte nem colisao por 1.5 s, enquanto
		// o W3 ainda carrega a fisica em volta (SkyCraft Game.cpp:93-96, 997-1000).
		constexpr std::uint64_t kSettleMs = 1500;
		// O MC segurando (ack != seq) a mais disto do Geralt por mais de kHoldMismatchMs: reteleporta
		// (holdMismatch do SkyCraft, Game.cpp:624-636).
		constexpr double        kHoldMismatchDistance = 8.0;
		constexpr std::uint64_t kHoldMismatchMs = 1000;
		// Fundo do mundo do MC (data/minewitcher3/dimension_type/mirror.json: min_y -1024).
		constexpr double kMcMinY = -1024.0;
		// Uma morte do MC que o script nao consumiu ate aqui (W3 pausado) e descartada.
		constexpr std::uint64_t kDeathLatchMs = 5000;

		std::atomic<bool>          g_linked{ false };
		std::atomic<bool>          g_screenOpen{ false };
		std::atomic<bool>          g_forwardEnabled{ true };
		std::atomic<bool>          g_gameMenu{ false };
		std::atomic<bool>          g_gameLoading{ false };
		std::atomic<float>         g_sensitivity{ 0.5f };
		std::atomic<std::uint64_t> g_lastScriptMs{ 0 };
		std::atomic<bool>          g_reconnected{ false };
		std::atomic<std::uint64_t> g_settleUntilMs{ 0 };
		std::atomic<std::uint64_t> g_mcDiedAtMs{ 0 };  // kEvPlayerDied ainda nao consumido (MW3_PollMcDeath)
		std::atomic<bool>          g_resyncRequested{ false };
		std::atomic<RenderSink>    g_renderSink{ nullptr };
		// mw3.ini [Motion] bInterpolate (1): pes/olho dos ticks do MC interpolados no relogio do jogo
		// (SkyCraft Game.cpp:662-786); 0 = os pes/olho que o MC interpolou no frame dele (modo antigo).
		std::atomic<bool>          g_interpolate{ true };
		// mw3.ini [Render] bStreamWorld (0): pedir ao MC o mundo/atlas/avatar pelo anel de render mesmo
		// sem consumidor (depuracao do futuro renderer). Com SetRenderSink o pedido sai sozinho.
		std::atomic<bool>          g_streamWorld{ false };

		// Tudo abaixo sob g_lock (WndProc, script e a thread do link mexem).
		std::mutex       g_lock;
		float            g_yaw = 0.0f;    // graus do MC
		float            g_pitch = 0.0f;  // graus do MC
		bool             g_resetLook = true;
		int              g_cursorX = 0;
		int              g_cursorY = 0;
		proto::GameState g_state{};
		std::uint32_t    g_teleportSeq = 0;
		bool             g_teleportPending = true;
		proto::McVec     g_lastTeleport{};
		bool             g_haveLastTeleport = false;
		// Ultimo McState lido pela thread do link (para os guardas do resync).
		bool             g_mcKnown = false;
		std::uint32_t    g_mcAck = 0;
		std::uint32_t    g_mcFlags = 0;
		proto::McVec     g_mcFeet{};
		std::uint64_t    g_holdMismatchSince = 0;

		Snapshot g_snapshot;

		void Viewport(int& w, int& h)
		{
			if (!GetViewportSize(w, h) || w <= 0 || h <= 0)
			{
				w = 1920;
				h = 1080;
			}
		}

		void WriteStateLocked()
		{
			int w = 0, h = 0;
			Viewport(w, h);
			g_state.yaw = g_yaw;
			g_state.pitch = g_pitch;
			g_state.teleportSeq = g_teleportSeq;
			g_state.viewportW = static_cast<std::uint32_t>(w);
			g_state.viewportH = static_cast<std::uint32_t>(h);
			Link::Get().WriteGameState(g_state);
		}

		bool ScriptFresh()
		{
			const auto last = g_lastScriptMs.load();
			return last != 0 && GetTickCount64() - last < kScriptStaleMs;
		}

		void StartSettle()
		{
			g_settleUntilMs = GetTickCount64() + kSettleMs;
		}

		// O jogo desenha o mundo do MC? So entao o MC exporta secoes/atlas/entidades/avatar
		// (proto::kGameWorldRender); sem isso eram ~8 MB/s drenados e jogados fora.
		bool WantWorldRender()
		{
			return g_streamWorld.load() || g_renderSink.load() != nullptr;
		}

		std::int64_t QpcNow()
		{
			LARGE_INTEGER t{};
			QueryPerformanceCounter(&t);
			return t.QuadPart;
		}

		// Thread do script (RefreshSnapshot) so.
		motion::Interpolator& Motion()
		{
			static motion::Interpolator interp([] {
				LARGE_INTEGER f{};
				QueryPerformanceFrequency(&f);
				return static_cast<std::int64_t>(f.QuadPart);
			}());
			return interp;
		}

		// A cada 10 s com a interpolacao ligada: quao suave o movimento do MC chegou (para o proximo log de campo).
		void LogMotionStats()
		{
			static std::uint64_t nextLog = 0;
			const auto           now = GetTickCount64();
			if (nextLog == 0)
				nextLog = now + 10000;
			if (now < nextLog)
				return;
			nextLog = now + 10000;
			const motion::Stats st = Motion().TakeStats();
			if (st.frames == 0)
				return;
			char line[320];
			std::snprintf(line, sizeof(line),
			              "movimento: %d ticks do MC, %d frames (media %.1f ms, max %.1f ms), %d frames esperando tick, %d ticks nao vistos, "
			              "%d teleportes, atraso de render %.1f ms, ruido dos stamps %.2f ms",
			              st.ticks, st.frames, st.frameMsSum / st.frames, st.frameMsMax, st.lateFrames, st.missedTicks, st.teleports, st.renderDelayMs,
			              st.stampSamples ? st.stampErrMs / st.stampSamples : 0.0);
			Log(line);
		}

		// Anel MC -> jogo (SkyCraft Combat::PerFrame, Combat.cpp:848-866: drena sempre, mesmo sem
		// fantoche, para eventos velhos nao chegarem depois). Por enquanto so a morte tem efeito.
		void DrainEvents(Link& link, bool alive)
		{
			proto::McEvent ev{};
			int            n = 0;
			while (n < 256 && link.PopEvent(ev))
			{
				++n;
				switch (ev.type)
				{
				case proto::kEvPlayerDied:
					if (alive)
					{
						g_mcDiedAtMs = GetTickCount64();
						Log("link: o jogador do MC morreu (atacante " + std::to_string(ev.actorId) + "); o Geralt morre tambem");
					}
					break;
				case proto::kEvHitActor:
					// v6: fila para o script (MW3_PollHit, mw3_combat.ws); combat.cpp descarta os velhos.
					if (alive)
						combat::OnMcHit(ev, GetTickCount64());
					break;
				case proto::kEvExplosion:
					Log("link: evento do MC: explosao em (" + std::to_string(ev.a) + ", " + std::to_string(ev.b) + ", " + std::to_string(ev.c) +
					    "), raio " + std::to_string(ev.d) + "; sem tratamento ainda");
					break;
				case proto::kEvArrowStuck:
					Log("link: evento do MC: flecha no ator " + std::to_string(ev.actorId) + "; sem tratamento ainda");
					break;
				default:
					Log("link: evento do MC desconhecido (tipo " + std::to_string(ev.type) + ")");
					break;
				}
			}
		}

		// Anel de render: o MC exporta o mundo/avatar a cada frame e os writeRender bloqueantes dele
		// esperam ate ~1 s por mensagem com o anel cheio (WitcherLink.java:558-584). Sem renderer
		// ainda: entrega a quem chamou SetRenderSink ou descarta.
		void DrainRenderRing(Link& link)
		{
			static std::uint64_t messages = 0, bytes = 0, nextLog = 0;
			const RenderSink     sink = g_renderSink.load();
			link.DrainRender(
			    [&](std::uint32_t type, const std::uint8_t* payload, std::uint32_t size) {
				    ++messages;
				    bytes += size;
				    if (sink)
					    sink(type, payload, size);
			    },
			    proto::kRenRingDataBytes);
			const auto now = GetTickCount64();
			if (messages && now >= nextLog)
			{
				nextLog = now + 60000;
				Log("link: anel de render drenado (" + std::to_string(messages) + " mensagens, " + std::to_string(bytes >> 20) + " MB ate agora; " +
				    (sink ? "com consumidor)" : "descartadas, sem renderer)"));
			}
		}

		// Thread propria por enquanto: sem hook de Present ainda.
		// TODO(present): o heartbeat e a leitura do McState vao para o hook de Present (roda ate em
		// menus e loading, como no SkyCraft), e esta thread some.
		DWORD WINAPI LinkThread(LPVOID)
		{
			auto&         link = Link::Get();
			bool          wasAlive = false;
			bool          wasLinked = false;
			bool          wasRouting = false;
			bool          wasScreen = false;
			std::uint32_t lastPid = 0;
			proto::McState mc{};

			for (;;)
			{
				link.Heartbeat();

				const bool alive = link.McAlive();
				const bool haveMc = alive && link.ReadMcState(mc);
				const auto pid = link.McPid();
				const bool newProcess = alive && pid != 0 && pid != lastPid;
				DrainEvents(link, alive);
				if (alive)
					DrainRenderRing(link);
				if (haveMc)
				{
					std::lock_guard<std::mutex> guard(g_lock);
					g_mcKnown = (mc.flags & proto::kMcInWorld) != 0;
					g_mcAck = mc.teleportAck;
					g_mcFlags = mc.flags;
					g_mcFeet = { mc.x, mc.y, mc.z };
				}
				else if (!alive)
				{
					std::lock_guard<std::mutex> guard(g_lock);
					g_mcKnown = false;
				}
				if (alive)
					lastPid = pid;
				if (alive && (!wasAlive || newProcess))
				{
					Log("link: Minecraft conectado (pid " + std::to_string(pid) + ")");
					link.ResetOverlay();
					g_reconnected = true;
				}
				else if (!alive && wasAlive)
				{
					Log("link: Minecraft sumiu (sem heartbeat ha 8 s)");
				}
				wasAlive = alive;

				// Uma leitura que falhou (MC no meio de uma escrita longa) mantem o ultimo valor.
				const bool linked = alive && (haveMc ? (mc.flags & proto::kMcInWorld) != 0 : g_linked.load());
				if (linked != wasLinked)
					Log(linked ? "link: Minecraft no mundo; camera e input passam a ser do MC" : "link: Minecraft fora do mundo; camera e input voltam ao W3");
				wasLinked = linked;
				g_linked = linked;

				const bool screen = linked && haveMc ? (mc.flags & proto::kMcScreenOpen) != 0 : (linked && g_screenOpen.load());
				if (screen && !wasScreen)
				{
					int w = 0, h = 0;
					Viewport(w, h);
					std::lock_guard<std::mutex> guard(g_lock);
					g_cursorX = w / 2;
					g_cursorY = h / 2;
				}
				wasScreen = screen;
				g_screenOpen = screen;
				if (haveMc && mc.sensitivity > 0.0f)
					g_sensitivity = mc.sensitivity;

				// Input saiu do MC (menu do W3, MC desconectado...): o MC solta tudo que estava apertado.
				const bool routing = RouteInput();
				if (wasRouting && !routing && alive)
					link.PushInput(proto::kInReleaseAll, 0);
				wasRouting = routing;

				// O script parou (pausa/menu/loading): continua mandando o estado, marcado como menu,
				// para o MC soltar as teclas e seguir com o pacing.
				if (!ScriptFresh())
				{
					std::lock_guard<std::mutex> guard(g_lock);
					g_state.flags |= proto::kGameMenuOpen;
					WriteStateLocked();
				}

				Sleep(10);  // ~64 Hz com a resolucao padrao de 15,6 ms do timer
			}
		}
	} // namespace

	void Start()
	{
		if (config::GetInt(L"Input", L"bForwardInput", 1) == 0)
		{
			g_forwardEnabled = false;
			Log("mw3.ini: bForwardInput=0, teclado/mouse nunca vao para o Minecraft");
		}
		if (config::GetInt(L"Motion", L"bInterpolate", 1) == 0)
		{
			g_interpolate = false;
			Log("mw3.ini: bInterpolate=0, camera e fantoche usam os pes do frame do MC (sem interpolar os ticks)");
		}
		if (config::GetInt(L"Render", L"bStreamWorld", 0) != 0)
		{
			g_streamWorld = true;
			Log("mw3.ini: bStreamWorld=1, o MC manda o mundo/avatar pelo anel de render mesmo sem renderer");
		}

		// Comeca num valor novo a cada execucao, para um MC que ainda confirma o teleporte da execucao
		// anterior nao parecer ja ter chegado neste.
		LARGE_INTEGER t{};
		QueryPerformanceCounter(&t);
		g_teleportSeq = static_cast<std::uint32_t>(t.QuadPart) | 1u;

		if (!Link::Get().Create())
		{
			Log("link: ERRO ao criar a memoria compartilhada; o Minecraft nao vai conectar");
			return;
		}

		HANDLE thread = CreateThread(nullptr, 0, LinkThread, nullptr, 0, nullptr);
		if (thread)
			CloseHandle(thread);
	}

	bool Linked()
	{
		return g_linked.load();
	}

	bool McScreenOpen()
	{
		return g_screenOpen.load();
	}

	bool RouteInput()
	{
		return g_forwardEnabled.load() && g_linked.load() && !g_gameMenu.load() && !g_gameLoading.load() && ScriptFresh();
	}

	void AddLook(long dx, long dy)
	{
		// Formula do MC (MouseHandler.turnPlayer): s = sens*0.6+0.2; graus = delta * s^3 * 8 * 0.15.
		const float s = g_sensitivity.load() * 0.6f + 0.2f;
		const float factor = s * s * s * 8.0f * 0.15f;
		std::lock_guard<std::mutex> guard(g_lock);
		g_yaw = std::fmod(g_yaw + static_cast<float>(dx) * factor, 360.0f);
		g_pitch = std::clamp(g_pitch + static_cast<float>(dy) * factor, -90.0f, 90.0f);
	}

	void MoveCursor(long dx, long dy)
	{
		int w = 0, h = 0;
		Viewport(w, h);
		int x = 0, y = 0;
		{
			std::lock_guard<std::mutex> guard(g_lock);
			g_cursorX = std::clamp(g_cursorX + static_cast<int>(dx), 0, w - 1);
			g_cursorY = std::clamp(g_cursorY + static_cast<int>(dy), 0, h - 1);
			x = g_cursorX;
			y = g_cursorY;
		}
		Link::Get().PushInput(proto::kInCursor, 0, x, y);
	}

	void SetGameState(bool inGame, bool menuOpen, bool loading, float gameHour)
	{
		g_gameMenu = menuOpen;
		g_gameLoading = loading;
		std::lock_guard<std::mutex> guard(g_lock);
		g_state.flags = (inGame ? proto::kGameInGame : 0u) | (menuOpen ? proto::kGameMenuOpen : 0u) | (loading ? proto::kGameLoading : 0u) |
		                (WantWorldRender() ? proto::kGameWorldRender : 0u);
		g_state.gameHour = gameHour;
	}

	void SetWorld(const char* depotPath)
	{
		// "levels\novigrad\novigrad.w2w" -> "novigrad"
		std::string name = depotPath ? depotPath : "";
		const auto  slash = name.find_last_of("\\/");
		if (slash != std::string::npos)
			name = name.substr(slash + 1);
		const auto dot = name.find_last_of('.');
		if (dot != std::string::npos)
			name = name.substr(0, dot);
		for (char& c : name)
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		const std::uint32_t id = name.empty() ? 0u : proto::WorldIdFromName(name.c_str());

		std::lock_guard<std::mutex> guard(g_lock);
		if (id == g_state.worldId)
			return;
		Log("link: mundo do W3 = '" + name + "' (worldId " + std::to_string(id) + ")");
		g_state.worldId = id;
		++g_state.collisionEpoch;
		g_teleportPending = true;
		g_haveLastTeleport = false;
		g_resetLook = true;
		StartSettle();
	}

	void SetPlayerState(double x, double y, double z, float w3Yaw)
	{
		g_lastScriptMs = GetTickCount64();
		const proto::McVec pos = proto::W3ToMc({ x, y, z });

		std::lock_guard<std::mutex> guard(g_lock);
		if (g_reconnected.exchange(false))
		{
			g_teleportPending = true;
			g_resetLook = true;
			StartSettle();
		}

		const bool inGame = (g_state.flags & proto::kGameInGame) != 0;
		const bool loading = (g_state.flags & proto::kGameLoading) != 0;
		if (!inGame || loading)
		{
			// Como o SkyCraft (Game.cpp:578-581): a referencia some e o settle recomeca a cada frame
			// de loading, entao o teleporte so sai 1.5 s depois do fim do loading.
			g_teleportPending = true;
			g_haveLastTeleport = false;
			g_holdMismatchSince = 0;
			g_resetLook = true;
			StartSettle();
		}
		else
		{
			// O MC comeca olhando para onde o Geralt olha (como o SkyCraft no teleporte).
			if (g_resetLook)
			{
				g_yaw = W3YawToMcYaw(w3Yaw);
				g_pitch = 0.0f;
				g_resetLook = false;
			}
			const auto now = GetTickCount64();
			// O MC ainda nao confirmou o ultimo teleporte (segurando ate a colisao chegar), morreu ou
			// esta abaixo do fundo do mundo dele (respawn no void): o gap nao quer dizer "o W3 moveu".
			const bool mcHolding = g_mcKnown && g_mcAck != g_teleportSeq;
			const bool mcDead = g_mcKnown && (g_mcFlags & proto::kMcDead) != 0;
			const bool mcBelow = g_mcKnown && g_mcFeet.y < kMcMinY;
			if (g_resyncRequested.exchange(false) && !g_teleportPending && Linked() && !mcHolding)
			{
				Log("link: o fantoche pediu resync (bloqueado); teleportando o MC para o Geralt");
				g_teleportPending = true;
			}
			if (g_haveLastTeleport && !g_teleportPending && !mcHolding && !mcDead && !mcBelow)
			{
				const double dx = pos.x - g_lastTeleport.x, dy = pos.y - g_lastTeleport.y, dz = pos.z - g_lastTeleport.z;
				const double gap = std::sqrt(dx * dx + dy * dy + dz * dz);
				if (gap > kResyncDistance && Linked())
				{
					Log("link: o Geralt se afastou " + std::to_string(gap) + " m do jogador do MC; teleportando o MC");
					g_teleportPending = true;
				}
				else if (gap > kResyncDistance)
				{
					g_teleportPending = true;  // sem MC: so mantem o ponto de referencia atualizado
				}
			}
			// holdMismatch: o MC segura longe do Geralt (o W3 o moveu durante o hold): manda de novo.
			if (mcHolding && !mcDead && !g_teleportPending && Linked())
			{
				const double dx = pos.x - g_mcFeet.x, dy = pos.y - g_mcFeet.y, dz = pos.z - g_mcFeet.z;
				if (std::sqrt(dx * dx + dy * dy + dz * dz) <= kHoldMismatchDistance)
				{
					g_holdMismatchSince = 0;
				}
				else if (g_holdMismatchSince == 0)
				{
					g_holdMismatchSince = now;
				}
				else if (now - g_holdMismatchSince > kHoldMismatchMs)
				{
					Log("link: o MC esta segurando longe do Geralt ha mais de 1 s; teleportando de novo");
					g_teleportPending = true;
					g_holdMismatchSince = 0;
				}
			}
			else
			{
				g_holdMismatchSince = 0;
			}
			if (g_teleportPending && now >= g_settleUntilMs.load())
			{
				++g_teleportSeq;
				g_lastTeleport = pos;
				g_haveLastTeleport = true;
				g_teleportPending = false;
			}
		}

		g_state.posX = pos.x;
		g_state.posY = pos.y;
		g_state.posZ = pos.z;
		WriteStateLocked();
	}

	const Snapshot& RefreshSnapshot()
	{
		auto&                 link = Link::Get();
		static proto::McState last{};
		static bool           haveLast = false;
		proto::McState        mc{};
		const bool            alive = link.McAlive();
		if (alive && link.ReadMcState(mc))
		{
			last = mc;
			haveLast = true;
		}
		else if (alive && haveLast)
		{
			mc = last;  // leitura rasgada (o MC escreve o McState duas vezes por frame): fica o ultimo
		}
		else
		{
			haveLast = false;
		}
		const bool ok = alive && haveLast && (mc.flags & proto::kMcInWorld) != 0;
		g_snapshot.linked = ok;
		if (ok)
		{
			g_snapshot.cameraMode = mc.cameraMode <= 2 ? static_cast<int>(mc.cameraMode) : 0;
			g_snapshot.cameraDistance = mc.cameraDistance;
			g_snapshot.fov = mc.fovDeg;

			// Modo antigo / sem ticks: o que o MC interpolou no frame dele (fase diferente da nossa).
			const double tickSeconds = mc.tickMs > 1.0f ? mc.tickMs / 1000.0 : 0.05;
			proto::McVec feetMc{ mc.x, mc.y, mc.z };
			proto::McVec eyeMc{ mc.eyeX, mc.eyeY, mc.eyeZ };
			proto::McVec velMc{ (mc.curX - mc.prevX) / tickSeconds, (mc.curY - mc.prevY) / tickSeconds, (mc.curZ - mc.prevZ) / tickSeconds };
			if (g_interpolate.load() && mc.tickQpc != 0 && mc.tickMs > 0.0f)
			{
				// Os ticks de 20 Hz interpolados no NOSSO relogio, como o renderer do MC faz com o partial
				// tick: a camera e o fantoche andam a cada frame do W3, sem depender de o MC ter
				// renderizado (e publicado) a tempo neste frame.
				motion::Tick tick{};
				tick.qpc = mc.tickQpc;
				tick.prevX = mc.prevX;
				tick.prevY = mc.prevY;
				tick.prevZ = mc.prevZ;
				tick.curX = mc.curX;
				tick.curY = mc.curY;
				tick.curZ = mc.curZ;
				tick.eyeO = mc.tickEyeO;
				tick.eye = mc.tickEye;
				tick.tickMs = mc.tickMs;
				const motion::Pose pose = Motion().Update(tick, QpcNow());
				// O olho fica sobre os pes (1a pessoa: Camera.position; 3a: getEyePosition), com a altura
				// suavizada do Camera.tick. Sem ela (Fabric antigo): a altura do frame do MC.
				const double eyeHeight = pose.eyeHeight > 0.01 ? pose.eyeHeight : static_cast<double>(mc.eyeHeight);
				feetMc = { pose.x, pose.y, pose.z };
				eyeMc = { pose.x, pose.y + eyeHeight, pose.z };
				velMc = { pose.vx, pose.vy, pose.vz };
				LogMotionStats();
			}
			else
			{
				Motion().Reset();
			}
			const proto::W3Vec eye = proto::McToW3(eyeMc);
			g_snapshot.eyeX = eye.x;
			g_snapshot.eyeY = eye.y;
			g_snapshot.eyeZ = eye.z;
			const proto::W3Vec feet = proto::McToW3(feetMc);
			g_snapshot.feetX = feet.x;
			g_snapshot.feetY = feet.y;
			g_snapshot.feetZ = feet.z;
			const proto::W3Vec vel = proto::McToW3(velMc);
			g_snapshot.velX = vel.x;
			g_snapshot.velY = vel.y;
			g_snapshot.velZ = vel.z;
			g_snapshot.mcFlags = mc.flags;
		}
		else
		{
			Motion().Reset();
			g_snapshot.velX = g_snapshot.velY = g_snapshot.velZ = 0.0;
			g_snapshot.mcFlags = 0;
		}
		std::lock_guard<std::mutex> guard(g_lock);
		g_snapshot.yaw = McYawToW3Yaw(g_yaw);
		g_snapshot.pitch = McPitchToW3Pitch(g_pitch);
		// O MC so confirma o teleporte depois que a colisao em volta chegou e ele soltou o jogador.
		g_snapshot.puppet = ok && mc.teleportAck == g_teleportSeq && !g_teleportPending && (g_state.flags & proto::kGameInGame) != 0 &&
		                    (g_state.flags & proto::kGameLoading) == 0;
		return g_snapshot;
	}

	bool Settling()
	{
		return GetTickCount64() < g_settleUntilMs.load();
	}

	void RequestResync()
	{
		g_resyncRequested = true;
	}

	bool ConsumeMcDeath()
	{
		const auto at = g_mcDiedAtMs.exchange(0);
		return at != 0 && GetTickCount64() - at < kDeathLatchMs;
	}

	void SetRenderSink(RenderSink sink)
	{
		g_renderSink = sink;
	}

	void NotePuppetTarget(double x, double y, double z)
	{
		std::lock_guard<std::mutex> guard(g_lock);
		g_lastTeleport = proto::W3ToMc({ x, y, z });
		g_haveLastTeleport = true;
	}

	std::uint32_t WorldEpoch(std::uint32_t* worldId)
	{
		std::lock_guard<std::mutex> guard(g_lock);
		if (worldId)
			*worldId = g_state.worldId;
		return g_state.collisionEpoch;
	}

	const Snapshot& CurrentSnapshot()
	{
		return g_snapshot;
	}
} // namespace mw3::session

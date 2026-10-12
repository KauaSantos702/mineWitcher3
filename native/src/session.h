// Derived from SkyCraft (https://github.com/chasmlol/skycraft), Copyright (c) 2026 chasmlol, MIT License.
// Game-side runtime of the link, adapted from skse/src/Game.cpp / Game.h (Runtime, PerFrame: teleport
// sequence, mouse look with Minecraft's sensitivity formula, Minecraft (re)connect). See THIRD-PARTY-NOTICES.md.
#pragma once

#include "link.h"

#include <cstdint>

namespace mw3::session
{
	// Cria o mapeamento e inicia a thread do link (heartbeat ~60 Hz, leitura do McState).
	void Start();

	// ---- estado do link (qualquer thread) ------------------------------------------------------
	// Minecraft vivo (heartbeat < 3 s), com estado consistente e com o jogador num mundo.
	bool Linked();
	// Uma tela do MC (inventario, chat, pausa...) esta aberta: mouse vira cursor, todas as teclas vao para o MC.
	bool McScreenOpen();
	// Encaminhar teclado/mouse ao MC agora (linkado, script vivo, sem menu do W3, bForwardInput=1).
	bool RouteInput();

	// ---- WndProc -> link -------------------------------------------------------------------------
	// Delta cru do mouse fora de telas do MC: integra yaw/pitch com a formula de sensibilidade do MC.
	void AddLook(long dx, long dy);
	// Delta cru do mouse com uma tela do MC aberta: move o cursor virtual (pixels do viewport).
	void MoveCursor(long dx, long dy);

	// ---- script -> link (natives; thread do script) ---------------------------------------------
	void SetGameState(bool inGame, bool menuOpen, bool loading, float gameHour);
	void SetWorld(const char* depotPath);
	// Posicao dos pes do Geralt (espaco W3, metros) e yaw (EulerAngles.Yaw do W3). Escreve o GameState.
	void SetPlayerState(double x, double y, double z, float w3Yaw);

	// ---- link -> script --------------------------------------------------------------------------
	struct Snapshot
	{
		bool   linked = false;
		double eyeX = 0, eyeY = 0, eyeZ = 0;  // olho do jogador do MC, espaco W3
		float  yaw = 0, pitch = 0;            // olhar autoritativo (integrado aqui, aplicado pelo MC), EulerAngles do W3
		int    cameraMode = 0;                // 0 1a pessoa, 1 atras, 2 frente
		float  cameraDistance = 0;            // blocos (= metros) do olho ate a camera do MC
		float  fov = 70;                      // FOV vertical do MC (graus)

		// Fantoche (native v4): o MC confirmou o ultimo teleporte (soltou o holdUntilReady), entao
		// e ele quem dirige o Geralt.
		bool          puppet = false;
		double        feetX = 0, feetY = 0, feetZ = 0;  // pes do jogador do MC (ticks interpolados no relogio do jogo, motion.h), espaco W3
		double        velX = 0, velY = 0, velZ = 0;     // velocidade do tick interpolado, m/s, espaco W3
		std::uint32_t mcFlags = 0;                      // proto::McFlags (no chao, agachado, correndo...)
	};
	// Le o McState e atualiza o snapshot que os getters das natives devolvem (MW3_LinkConnected).
	const Snapshot& RefreshSnapshot();
	const Snapshot& CurrentSnapshot();

	// ---- native v4 (colisao + fantoche) ------------------------------------------------------------
	// O script moveu o Geralt para estes pes (espaco W3) a pedido do MC: vira a referencia do
	// "o W3 moveu o jogador sozinho" (cutscene, porta de loading, fast travel) que reteleporta o MC.
	void NotePuppetTarget(double x, double y, double z);
	// Epoca do mundo do W3 (sobe a cada troca de mundo) e o worldId atual.
	std::uint32_t WorldEpoch(std::uint32_t* worldId);

	// ---- native v5 (morte, resync, settle, anel de render) -----------------------------------------
	// 1.5 s depois de loading / troca de mundo / reconexao do MC: sem teleporte e sem colisao.
	bool Settling();
	// O fantoche ficou bloqueado (pes do MC abaixo do chao do W3): teleporta o MC para o Geralt.
	void RequestResync();
	// true uma vez por kEvPlayerDied recente (a thread do link drena o anel de eventos).
	bool ConsumeMcDeath();
	// Ponto de extensao do futuro renderer do mundo: a thread do link drena o anel de render a cada
	// ~10 ms e entrega cada mensagem aqui (payload na memoria compartilhada, valido so durante a
	// chamada; roda na thread do link). nullptr = descarta. Um renderer que prefira drenar no
	// proprio thread (Present) deve trocar este dreno por Link::DrainRender la.
	using RenderSink = void (*)(std::uint32_t type, const std::uint8_t* payload, std::uint32_t bytes);
	void SetRenderSink(RenderSink sink);
} // namespace mw3::session

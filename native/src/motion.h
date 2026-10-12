// Derived from SkyCraft (https://github.com/chasmlol/skycraft), Copyright (c) 2026 chasmlol, MIT License.
// Interpolation of Minecraft's 20 Hz physics ticks on the game's own clock, ported from
// skse/src/Game.cpp:39-57, 662-786 (upstream commit bfcaf17). See THIRD-PARTY-NOTICES.md.
//
// Sem dependencia de Windows (o relogio e passado de fora em unidades de QPC), para o mw3_motiontest.
#pragma once

#include <array>
#include <cstdint>
#include <deque>

namespace mw3::motion
{
	// Um tick de fisica do MC como o Fabric publica (McState: tickQpc, prev*/cur*, tickEyeO/tickEye).
	// Coordenadas do MC (blocos).
	struct Tick
	{
		std::int64_t qpc = 0;  // QueryPerformanceCounter do tick (resto do partial tick descontado)
		double       prevX = 0, prevY = 0, prevZ = 0;
		double       curX = 0, curY = 0, curZ = 0;
		float        eyeO = 0, eye = 0;  // altura do olho suavizada (Camera.tick), tick anterior/ultimo
		float        tickMs = 50;
	};

	struct Pose
	{
		double x = 0, y = 0, z = 0;     // pes interpolados (MC)
		double eyeHeight = 0;           // blocos acima dos pes
		double vx = 0, vy = 0, vz = 0;  // velocidade do trecho interpolado (blocos/s, MC)
		double ticks = 0;               // posicao dentro do tick (0..1; >1 = o proximo tick nao chegou)
		bool   late = false;            // este frame parou esperando um tick (deveria ser raro)
	};

	// Medidas desde o ultimo TakeStats (log a cada 10 s).
	struct Stats
	{
		int    ticks = 0, frames = 0, lateFrames = 0, missedTicks = 0, teleports = 0, stampSamples = 0;
		double stampErrMs = 0, frameMsSum = 0, frameMsMax = 0;
		double renderDelayMs = 0;
	};

	// Historico curto dos ticks do MC, com os stamps travados no ritmo exato do tick (o ruido dos
	// stamps filtrado), renderizado renderDelayMs no passado (adaptativo, 4..30 ms) para o proximo
	// tick ja ter chegado: so interpola (o modelo do proprio renderer do MC), nunca extrapola. Um tick
	// perdido e emendado (fim de um -> inicio do proximo). Um salto maior que kTeleportBlocks entre
	// ticks (teleporte, respawn) recomeca o historico: a camera pula em vez de varrer o caminho.
	class Interpolator
	{
	public:
		// Mais que a velocidade terminal de queda do MC (3.92 blocos/tick): so teleporte.
		static constexpr double kTeleportBlocks = 10.0;

		explicit Interpolator(std::int64_t qpcFrequency);

		void Reset();
		// Uma vez por frame do jogo: registra o tick mais recente (se for novo) e devolve a pose em nowQpc.
		Pose Update(const Tick& latest, std::int64_t nowQpc);
		[[nodiscard]] double RenderDelayMs() const { return renderDelayMs_; }
		[[nodiscard]] std::size_t HistorySize() const { return history_.size(); }
		Stats TakeStats();

	private:
		struct Entry
		{
			Tick         s;
			std::int64_t at = 0;  // quando aconteceu (QPC), no ritmo travado
			int          slots = 1;
		};

		void Observe(const Tick& latest);

		double                 qpcPerMs_;
		std::deque<Entry>      history_;
		std::int64_t           lastFrameQpc_ = 0;
		int                    stampOutliers_ = 0;
		double                 renderDelayMs_ = 10.0;
		std::array<double, 40> tickDue_{};  // ultimos ~2 s: quanto cada tick ja estava vencido quando o vimos (ms)
		std::size_t            tickDueNext_ = 0;
		bool                   tickDueInit_ = false;
		Stats                  stats_{};
	};
} // namespace mw3::motion

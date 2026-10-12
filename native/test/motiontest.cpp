// Teste de console da interpolacao dos ticks do MC (motion.cpp); nao vai para o jogo.
// Simula o MC tickando a 20 Hz com stamps ruidosos e chegada atrasada, e o jogo rodando frames com
// jitter, e confere que a pose por frame anda com velocidade constante (sem judder), que teleportes
// pulam em vez de varrer, que um tick perdido e emendado e que um MC reiniciado recomeca o historico.
//
//   mw3_motiontest.exe   roda os testes e sai (0 = PASS)
#include "../src/motion.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

namespace
{
	int g_failures = 0;

	void Check(bool ok, const char* what)
	{
		std::printf("  %s %s\n", ok ? "ok " : "BAD", what);
		if (!ok)
			++g_failures;
	}

	constexpr std::int64_t kFreq = 10'000'000;  // QPC de 10 MHz, como no Windows 10+
	constexpr double       kQpcPerMs = kFreq / 1000.0;

	std::int64_t Ms(double ms)
	{
		return static_cast<std::int64_t>(std::llround(ms * kQpcPerMs));
	}

	struct McSim
	{
		double                 speed = 4.3;  // blocos/s em x
		std::vector<double>    arrivalMs;    // quando o jogo pode ver cada tick
		std::vector<double>    stampMs;      // o stamp que o MC escreve
		std::vector<double>    posAtTick;    // cur do tick k
		std::vector<bool>      dropped;      // o jogo nunca ve este tick (o MC rodou dois num frame)

		double Pos(int k) const { return k < 0 ? 0.0 : posAtTick[static_cast<std::size_t>(k)]; }
	};

	// O tick mais recente que o jogo ja ve em nowMs (ou -1).
	int Visible(const McSim& mc, double nowMs)
	{
		int best = -1;
		for (int k = 0; k < static_cast<int>(mc.arrivalMs.size()); ++k)
			if (mc.arrivalMs[static_cast<std::size_t>(k)] <= nowMs && !mc.dropped[static_cast<std::size_t>(k)])
				best = k;
		return best;
	}

	mw3::motion::Tick MakeTick(const McSim& mc, int k)
	{
		mw3::motion::Tick t{};
		t.qpc = Ms(mc.stampMs[static_cast<std::size_t>(k)]) + Ms(1000.0);
		t.prevX = mc.Pos(k - 1);
		t.curX = mc.Pos(k);
		t.prevY = t.curY = 64.0;
		t.eyeO = t.eye = 1.62f;
		t.tickMs = 50.0f;
		return t;
	}

	McSim BuildMc(int ticks, unsigned seed, double speed)
	{
		std::mt19937                           rng(seed);
		std::uniform_real_distribution<double> stampNoise(-4.0, 4.0);
		std::uniform_real_distribution<double> latency(1.0, 14.0);
		McSim                                  mc;
		mc.speed = speed;
		for (int k = 0; k < ticks; ++k)
		{
			const double at = k * 50.0;
			mc.stampMs.push_back(at + stampNoise(rng));
			mc.arrivalMs.push_back(at + latency(rng));
			mc.posAtTick.push_back(speed * at / 1000.0);
			mc.dropped.push_back(false);
		}
		return mc;
	}

	struct RunResult
	{
		double maxSpeedErr = 0;     // |velocidade aparente por frame - real|, depois do aquecimento
		double maxBackward = 0;     // maior passo para tras (m)
		double maxStep = 0;         // maior passo num frame (m)
		int    lateFrames = 0;
		int    frames = 0;
		double naiveMaxSpeedErr = 0;  // o mesmo, amostrando "a pose do MC no ultimo frame dele" (o modo antigo)
	};

	RunResult Run(const McSim& mc, unsigned seed, double frameMs, double frameJitterMs)
	{
		std::mt19937                           rng(seed);
		std::uniform_real_distribution<double> jitter(-frameJitterMs, frameJitterMs);
		mw3::motion::Interpolator              interp(kFreq);
		RunResult                              r;
		double                                 now = 60.0;
		double                                 lastX = 0, lastNow = 0;
		double                                 lastNaive = 0;
		bool                                   have = false;
		const double                           endMs = (static_cast<double>(mc.arrivalMs.size()) - 4) * 50.0;
		while (now < endMs)
		{
			const int k = Visible(mc, now);
			if (k >= 0)
			{
				const auto pose = interp.Update(MakeTick(mc, k), Ms(now) + Ms(1000.0));
				// Modo antigo: pes do frame anterior do MC (interpolados no relogio do MC, que tem fase
				// diferente): o MC amostrou em now - frameMs + (latencia do frame dele).
				const double mcSampleMs = now - frameMs + std::fabs(jitter(rng)) * 2.0;
				const double naive = mc.speed * std::max(0.0, mcSampleMs - 50.0) / 1000.0;
				if (have && now > 1500.0)
				{
					const double dt = (now - lastNow) / 1000.0;
					const double v = (pose.x - lastX) / dt;
					r.maxSpeedErr = std::max(r.maxSpeedErr, std::fabs(v - mc.speed));
					r.maxBackward = std::max(r.maxBackward, lastX - pose.x);
					r.maxStep = std::max(r.maxStep, pose.x - lastX);
					r.naiveMaxSpeedErr = std::max(r.naiveMaxSpeedErr, std::fabs((naive - lastNaive) / dt - mc.speed));
					if (pose.late)
						++r.lateFrames;
					++r.frames;
				}
				lastX = pose.x;
				lastNow = now;
				lastNaive = naive;
				have = true;
			}
			now += std::max(4.0, frameMs + jitter(rng));
		}
		return r;
	}
} // namespace

int main()
{
	std::printf("interpolacao: 60 fps com jitter de 3 ms, stamps +-4 ms, chegada 1..14 ms\n");
	{
		const McSim mc = BuildMc(400, 1, 4.3);
		const auto  r = Run(mc, 2, 16.67, 3.0);
		std::printf("    frames %d, atrasados %d, erro max de velocidade %.3f m/s (modo antigo %.3f), passo para tras %.4f\n", r.frames, r.lateFrames,
		            r.maxSpeedErr, r.naiveMaxSpeedErr, r.maxBackward);
		Check(r.frames > 900, "rodou os frames");
		Check(r.maxBackward < 1e-6, "nunca anda para tras");
		Check(r.lateFrames * 100 <= r.frames, "quase nenhum frame parado esperando tick (<= 1%)");
		Check(r.maxSpeedErr < 0.25 * 4.3, "velocidade por frame estavel (< 25% de erro)");
		Check(r.maxSpeedErr < r.naiveMaxSpeedErr, "mais suave que amostrar o frame do MC");
	}

	std::printf("interpolacao: 144 fps e 30 fps\n");
	{
		const McSim mc = BuildMc(400, 3, 5.6);
		const auto  fast = Run(mc, 4, 6.94, 1.0);
		const auto  slow = Run(mc, 5, 33.3, 5.0);
		std::printf("    144 fps: erro %.3f, atrasados %d | 30 fps: erro %.3f, atrasados %d\n", fast.maxSpeedErr, fast.lateFrames, slow.maxSpeedErr,
		            slow.lateFrames);
		Check(fast.maxBackward < 1e-6 && slow.maxBackward < 1e-6, "nunca anda para tras");
		Check(fast.maxSpeedErr < 0.3 * 5.6, "144 fps estavel");
		Check(slow.maxSpeedErr < 0.3 * 5.6, "30 fps estavel");
	}

	std::printf("engasgo do MC (dois ticks num frame dele; so vimos o segundo)\n");
	{
		// O tick k chega junto com o k+1 (o frame do MC demorou ~50 ms) e o jogo so ve o k+1. Parar
		// por ~1 tick e inevitavel sem extrapolar; o que importa e emendar sem pulo nem volta.
		McSim mc = BuildMc(400, 6, 4.3);
		for (int k = 40; k < 398; k += 37)
		{
			mc.dropped[static_cast<std::size_t>(k)] = true;
			mc.arrivalMs[static_cast<std::size_t>(k + 1)] = std::max(mc.arrivalMs[static_cast<std::size_t>(k + 1)], (k + 1) * 50.0 + 2.0);
		}
		const auto r = Run(mc, 7, 16.67, 2.0);
		std::printf("    atrasados %d, maior passo %.3f m (um tick = %.3f m)\n", r.lateFrames, r.maxStep, 4.3 * 0.05);
		Check(r.maxBackward < 1e-6, "nunca anda para tras");
		Check(r.maxStep < 4.3 * 0.05 * 1.6, "emenda sem pulo (passo < 1.6 tick)");
	}

	std::printf("teleporte\n");
	{
		mw3::motion::Interpolator interp(kFreq);
		mw3::motion::Tick         t{};
		t.tickMs = 50.0f;
		t.eyeO = t.eye = 1.62f;
		std::int64_t now = Ms(1000.0);
		for (int k = 0; k < 20; ++k)
		{
			t.qpc = Ms(1000.0 + k * 50.0);
			t.prevX = k * 0.2;
			t.curX = (k + 1) * 0.2;
			for (int f = 0; f < 3; ++f, now += Ms(16.67))
				interp.Update(t, now + Ms(5.0));
		}
		// O MC foi teleportado 500 blocos (o tick carrega prev = cur = destino, ou prev ainda na origem).
		t.qpc = Ms(1000.0 + 20 * 50.0);
		t.prevX = 4.0;
		t.curX = 504.0;
		bool swept = false, arrived = false;
		for (int f = 0; f < 12; ++f, now += Ms(16.67))
		{
			const auto pose = interp.Update(t, now + Ms(5.0));
			if (pose.x > 10.0 && pose.x < 500.0)
				swept = true;
			if (std::fabs(pose.x - 504.0) < 1e-6)
				arrived = true;
		}
		Check(!swept, "nao varre o caminho do teleporte");
		Check(arrived, "chega no destino");
		const auto stats = interp.TakeStats();
		Check(stats.ticks == 21, "contou os ticks");
	}

	std::printf("MC reiniciado (stamps voltam no tempo)\n");
	{
		mw3::motion::Interpolator interp(kFreq);
		mw3::motion::Tick         t{};
		t.tickMs = 50.0f;
		std::int64_t now = Ms(100000.0);
		for (int k = 0; k < 10; ++k, now += Ms(50.0))
		{
			t.qpc = now - Ms(3.0);
			t.prevX = t.curX = 1.0;
			interp.Update(t, now);
		}
		t.qpc = Ms(5.0);
		t.prevX = t.curX = 2.0;
		const auto pose = interp.Update(t, now);
		Check(interp.HistorySize() == 1, "historico recomecou");
		Check(std::fabs(pose.x - 2.0) < 1e-9, "pose do MC novo");
	}

	std::printf("pausa longa do MC: fica no ultimo tick, sem velocidade\n");
	{
		mw3::motion::Interpolator interp(kFreq);
		mw3::motion::Tick         t{};
		t.tickMs = 50.0f;
		t.qpc = Ms(1000.0);
		t.prevX = 0.0;
		t.curX = 0.2;
		interp.Update(t, Ms(1010.0));
		const auto pose = interp.Update(t, Ms(3000.0));
		Check(std::fabs(pose.x - 0.2) < 1e-9, "no cur do ultimo tick");
		Check(pose.vx == 0.0 && !pose.late, "parado, sem contar como atraso");
	}

	std::printf(g_failures ? "FAIL (%d)\n" : "PASS\n", g_failures);
	return g_failures ? 1 : 0;
}

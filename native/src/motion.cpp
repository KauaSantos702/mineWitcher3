// Derived from SkyCraft (https://github.com/chasmlol/skycraft), Copyright (c) 2026 chasmlol, MIT License.
// Port of the tick interpolation in skse/src/Game.cpp:662-786 (upstream commit bfcaf17). See THIRD-PARTY-NOTICES.md.
#include "motion.h"

#include <algorithm>
#include <cmath>

namespace mw3::motion
{
	namespace
	{
		double Dist(double ax, double ay, double az, double bx, double by, double bz)
		{
			const double dx = ax - bx, dy = ay - by, dz = az - bz;
			return std::sqrt(dx * dx + dy * dy + dz * dz);
		}
	} // namespace

	Interpolator::Interpolator(std::int64_t qpcFrequency) : qpcPerMs_(static_cast<double>(qpcFrequency > 0 ? qpcFrequency : 10'000'000) / 1000.0)
	{
	}

	void Interpolator::Reset()
	{
		history_.clear();
		lastFrameQpc_ = 0;
		stampOutliers_ = 0;
		renderDelayMs_ = 10.0;
		tickDueNext_ = 0;
		tickDueInit_ = false;
	}

	void Interpolator::Observe(const Tick& latest)
	{
		if (!history_.empty() && history_.back().s.qpc == latest.qpc)
			return;
		if (!history_.empty() && latest.qpc < history_.back().s.qpc)
			history_.clear();  // o MC reiniciou

		Entry tick{ latest, latest.qpc, 1 };
		// Teleporte dentro do proprio tick (prev longe de cur): sem trecho para varrer.
		if (Dist(tick.s.prevX, tick.s.prevY, tick.s.prevZ, tick.s.curX, tick.s.curY, tick.s.curZ) > kTeleportBlocks)
		{
			tick.s.prevX = tick.s.curX;
			tick.s.prevY = tick.s.curY;
			tick.s.prevZ = tick.s.curZ;
			tick.s.eyeO = tick.s.eye;
		}
		// Teleporte entre ticks (respawn, hold): recomeca daqui.
		if (!history_.empty())
		{
			const auto& b = history_.back().s;
			if (Dist(b.curX, b.curY, b.curZ, tick.s.prevX, tick.s.prevY, tick.s.prevZ) > kTeleportBlocks)
			{
				history_.clear();
				++stats_.teleports;
			}
		}

		const std::int64_t period = std::max<std::int64_t>(1, std::llround(static_cast<double>(latest.tickMs) * qpcPerMs_));
		if (!history_.empty())
		{
			auto&              last = history_.back();
			const std::int64_t n = std::llround(static_cast<double>(latest.qpc - last.at) / static_cast<double>(period));
			const std::int64_t err = latest.qpc - (last.at + n * period);
			if (n == 0 && last.slots >= 2)
			{
				// O MC rodou dois ticks num frame e vimos os dois: o primeiro leva o stamp do segundo.
				last.at -= period;
				last.slots -= 1;
				tick.at = last.at + period;
			}
			else if (n >= 1 && n <= 10 && std::llabs(err) < period * 3 / 10)
			{
				tick.at = last.at + n * period + err / 16;  // o ritmo e exato; os stamps tem ruido
				tick.slots = static_cast<int>(n);
				stampOutliers_ = 0;
				stats_.stampErrMs += std::fabs(static_cast<double>(err)) / qpcPerMs_;
				++stats_.stampSamples;
			}
			else if (n <= 10 && ++stampOutliers_ < 3)
			{
				tick.slots = static_cast<int>(std::max<std::int64_t>(n, 1));
				tick.at = last.at + tick.slots * period;  // um stamp estranho (engasgo): mantem o ritmo
			}
			else
			{
				stampOutliers_ = 0;  // perdeu o ritmo (pausa, outra taxa de tick): recomeca deste stamp
			}
			if (tick.slots >= 2)
				stats_.missedTicks += tick.slots - 1;
		}
		// Ja estava vencido no nosso frame anterior? Entao o render teve de esperar por ele.
		if (lastFrameQpc_ != 0)
		{
			if (!tickDueInit_)
			{
				tickDue_.fill(renderDelayMs_ - 1.0);
				tickDueInit_ = true;
			}
			const double dueMs = static_cast<double>(lastFrameQpc_ - tick.at) / qpcPerMs_;
			if (dueMs < 30.0)  // mais que isso e engasgo, nao um padrao a esperar
				tickDue_[tickDueNext_++ % tickDue_.size()] = dueMs;
		}
		history_.push_back(tick);
		if (history_.size() > 8)
			history_.pop_front();
		++stats_.ticks;
	}

	Pose Interpolator::Update(const Tick& latest, std::int64_t nowQpc)
	{
		Observe(latest);

		// O atraso segue o quanto os ticks chegaram tarde nos ultimos ~2 s: sobe 2% mais devagar que
		// o tempo real e desce 0.2% mais rapido, pouco demais para se ver.
		const double frameMs = lastFrameQpc_ != 0 ? static_cast<double>(nowQpc - lastFrameQpc_) / qpcPerMs_ : 0.0;
		lastFrameQpc_ = nowQpc;
		if (tickDueInit_)
		{
			const double target = std::clamp(*std::max_element(tickDue_.begin(), tickDue_.end()) + 1.0, 4.0, 30.0);
			const double dt = std::clamp(frameMs, 0.0, 100.0) / 1000.0;
			renderDelayMs_ = target > renderDelayMs_ ? std::min(target, renderDelayMs_ + 20.0 * dt) : std::max(target, renderDelayMs_ - 2.0 * dt);
		}
		const std::int64_t renderQpc = nowQpc - std::llround(renderDelayMs_ * qpcPerMs_);

		Pose pose{};
		if (history_.empty())
			return pose;

		const std::int64_t period = std::max<std::int64_t>(1, std::llround(static_cast<double>(history_.back().s.tickMs) * qpcPerMs_));
		std::size_t        i = 0;
		for (std::size_t k = history_.size(); k-- > 0;)
		{
			if (history_[k].at <= renderQpc)
			{
				i = k;
				break;
			}
		}
		const Entry&  tick = history_[i];
		const Entry*  next = i + 1 < history_.size() ? &history_[i + 1] : nullptr;
		const double  ticks = static_cast<double>(renderQpc - tick.at) / static_cast<double>(period);
		const double  t = std::clamp(ticks, 0.0, 1.0);
		const auto&   s = tick.s;
		const double  tickSeconds = s.tickMs > 1.0f ? s.tickMs / 1000.0 : 0.05;
		pose.ticks = ticks;
		pose.x = s.prevX + (s.curX - s.prevX) * t;
		pose.y = s.prevY + (s.curY - s.prevY) * t;
		pose.z = s.prevZ + (s.curZ - s.prevZ) * t;
		pose.eyeHeight = s.eyeO + (s.eye - s.eyeO) * t;
		pose.vx = (s.curX - s.prevX) / tickSeconds;
		pose.vy = (s.curY - s.prevY) / tickSeconds;
		pose.vz = (s.curZ - s.prevZ) / tickSeconds;
		if (ticks > 1.0 && next)
		{
			// Passou do fim deste tick e o proximo que temos comeca depois: o MC rodou um que nao vimos.
			// Segue do fim deste ate o inicio do proximo.
			const auto&  n = next->s;
			const double gap = static_cast<double>(next->at - (tick.at + period));
			const double u = gap > 0.0 ? std::clamp(static_cast<double>(renderQpc - (tick.at + period)) / gap, 0.0, 1.0) : 1.0;
			pose.x = s.curX + (n.prevX - s.curX) * u;
			pose.y = s.curY + (n.prevY - s.curY) * u;
			pose.z = s.curZ + (n.prevZ - s.curZ) * u;
			pose.eyeHeight = s.eye + (n.eyeO - s.eye) * u;
			const double span = gap > 0.0 ? gap / qpcPerMs_ / 1000.0 : tickSeconds;
			pose.vx = (n.prevX - s.curX) / span;
			pose.vy = (n.prevY - s.curY) / span;
			pose.vz = (n.prevZ - s.curZ) / span;
		}
		else if (ticks > 1.0 && ticks < 4.0)
		{
			// O proximo tick nao chegou: o jogador fica parado neste frame. Mais de alguns ticks sem
			// nada e pausa/hold do MC, nao atraso.
			pose.late = true;
			++stats_.lateFrames;
		}
		if (ticks >= 4.0 && !next)
			pose.vx = pose.vy = pose.vz = 0.0;  // MC parado (pausa/hold): sem passo para a animacao

		++stats_.frames;
		stats_.frameMsSum += frameMs;
		stats_.frameMsMax = std::max(stats_.frameMsMax, frameMs);
		return pose;
	}

	Stats Interpolator::TakeStats()
	{
		Stats out = stats_;
		out.renderDelayMs = renderDelayMs_;
		stats_ = Stats{};
		return out;
	}
} // namespace mw3::motion

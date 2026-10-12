// Combate W3 <-> MC (native v6). Derivado do SkyCraft skse/src/Combat.cpp (WriteActorTable, PerFrame,
// SendHurt), Copyright (c) 2026 chasmlol, MIT License. Ver combat.h e THIRD-PARTY-NOTICES.md.
#include "combat.h"

#include "log.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

namespace mw3::combat
{
	// O ActorRecord como o WitcherLink.readActors (Java) o le: id @0, flags @4, x/y/z @8/12/16, yaw @20,
	// width @24, height @28, healthFrac @32, level (u16) @36, name[24] @40.
	static_assert(offsetof(proto::ActorRecord, flags) == 4 && offsetof(proto::ActorRecord, x) == 8 && offsetof(proto::ActorRecord, yaw) == 20 &&
	              offsetof(proto::ActorRecord, width) == 24 && offsetof(proto::ActorRecord, height) == 28 &&
	              offsetof(proto::ActorRecord, healthFrac) == 32 && offsetof(proto::ActorRecord, level) == 36 && offsetof(proto::ActorRecord, name) == 40);
	static_assert(offsetof(proto::ActorTable, count) == 4 && offsetof(proto::ActorTable, actors) == 0x40);
	static_assert(offsetof(proto::McEvent, actorId) == 4 && offsetof(proto::McEvent, a) == 8 && offsetof(proto::McEvent, d) == 20 &&
	              offsetof(proto::McEvent, flags) == 24 && offsetof(proto::McEvent, weapon) == 28);

	namespace
	{
		// Thread do script.
		std::vector<proto::ActorRecord> g_records;
		std::uint32_t                   g_lastCount = 0xFFFFFFFFu;

		// Thread do link (OnMcHit) x thread do script (PopHit).
		std::mutex      g_hitLock;
		std::deque<Hit> g_hits;
		std::uint64_t   g_droppedHits = 0;

		float Finite(float v, float fallback)
		{
			return std::isfinite(v) ? v : fallback;
		}
	} // namespace

	void CopyName(const std::string& name, char (&out)[24])
	{
		std::memset(out, 0, sizeof(out));
		std::size_t n = (std::min)(name.size(), sizeof(out) - 1);
		// Nao corta uma sequencia UTF-8: recua enquanto o primeiro byte que ficaria de fora for de continuacao.
		if (n < name.size())
		{
			while (n > 0 && (static_cast<unsigned char>(name[n]) & 0xC0) == 0x80)
				--n;
		}
		std::memcpy(out, name.data(), n);
	}

	proto::ActorRecord MakeRecord(const ActorIn& in)
	{
		proto::ActorRecord r{};
		r.actorId = in.id;
		r.flags = in.flags;
		const proto::McVec mc = proto::W3ToMc({ in.x, in.y, in.z });
		r.x = static_cast<float>(mc.x);
		r.y = static_cast<float>(mc.y);
		r.z = static_cast<float>(mc.z);
		r.yaw = W3YawToMcYaw(Finite(in.w3Yaw, 0.0f));
		const float k = 1.0f / static_cast<float>(proto::kUnitsPerBlock);
		r.width = std::clamp(Finite(in.radius, 0.4f) * 2.0f * k, kMinSize, kMaxWidth);
		r.height = std::clamp(Finite(in.height, 1.8f) * k, kMinSize, kMaxHeight);
		const float maxHealth = Finite(in.maxHealth, 0.0f);
		r.healthFrac = maxHealth > 0.0f ? std::clamp(Finite(in.health, 0.0f) / maxHealth, 0.0f, 1.0f) : 0.0f;
		r.level = static_cast<std::uint16_t>(std::clamp(in.level, 0, 0xFFFF));
		CopyName(in.name, r.name);
		return r;
	}

	void BeginActors()
	{
		g_records.clear();
	}

	void AddActor(const ActorIn& in)
	{
		if (in.id == 0 || g_records.size() >= proto::kMaxActors)
			return;
		if (!std::isfinite(in.x) || !std::isfinite(in.y) || !std::isfinite(in.z))
			return;
		g_records.push_back(MakeRecord(in));
	}

	std::uint32_t CommitActors(Link& link)
	{
		const auto n = static_cast<std::uint32_t>(g_records.size());
		link.WriteActors(g_records.data(), n);
		if (n != g_lastCount && (n == 0 || g_lastCount == 0xFFFFFFFFu || n % 8 == 0 || g_lastCount == 0))
			Log("combat: " + std::to_string(n) + " atores do W3 na tabela do MC");
		g_lastCount = n;
		return n;
	}

	void ClearActors(Link& link)
	{
		g_records.clear();
		if (g_lastCount != 0)
		{
			link.WriteActors(nullptr, 0);
			if (g_lastCount != 0xFFFFFFFFu)
				Log("combat: tabela de atores esvaziada");
		}
		g_lastCount = 0;
	}

	void OnMcHit(const proto::McEvent& ev, std::uint64_t nowMs)
	{
		if (ev.type != proto::kEvHitActor || ev.actorId == 0)
			return;
		Hit h{};
		h.actorId = ev.actorId;
		h.mcDamage = std::clamp(Finite(ev.a, 0.0f), 0.0f, 10000.0f);
		// Direcao do knockback do MC (x, z) -> W3 (x, -z), como em proto::McToW3.
		const float dx = Finite(ev.b, 0.0f), dz = Finite(ev.c, 0.0f);
		const float len = std::sqrt(dx * dx + dz * dz);
		if (len > 1e-4f)
		{
			h.pushX = dx / len;
			h.pushY = -dz / len;
		}
		h.push = std::clamp(Finite(ev.d, 0.0f), 0.0f, 10.0f);
		h.flags = ev.flags;
		h.weapon = ev.weapon;
		h.atMs = nowMs;

		std::lock_guard<std::mutex> guard(g_hitLock);
		if (g_hits.size() >= kMaxPendingHits)
		{
			g_hits.pop_front();
			++g_droppedHits;
		}
		g_hits.push_back(h);
	}

	bool PopHit(Hit& out, std::uint64_t nowMs)
	{
		std::lock_guard<std::mutex> guard(g_hitLock);
		while (!g_hits.empty())
		{
			const Hit h = g_hits.front();
			g_hits.pop_front();
			if (nowMs - h.atMs <= kHitStaleMs)
			{
				out = h;
				return true;
			}
			++g_droppedHits;
		}
		return false;
	}

	void ClearHits()
	{
		std::lock_guard<std::mutex> guard(g_hitLock);
		g_hits.clear();
	}

	bool SendPlayerHurt(Link& link, std::uint32_t kind, float mcDamage, std::uint32_t attackerId, std::uint32_t flags)
	{
		if (!link.Valid() || !std::isfinite(mcDamage) || mcDamage <= 0.0f)
			return false;
		const float  dmg = (std::min)(mcDamage, kMaxHurt);
		const auto   a = static_cast<std::int32_t>(std::lround(dmg * 100.0f));
		const auto   code = static_cast<std::uint16_t>(kind <= proto::kHurtOther ? kind : proto::kHurtOther);
		if (a <= 0)
			return false;
		link.PushInput(proto::kInHurt, code, a, static_cast<std::int32_t>(attackerId), static_cast<std::int32_t>(flags));
		return true;
	}
} // namespace mw3::combat

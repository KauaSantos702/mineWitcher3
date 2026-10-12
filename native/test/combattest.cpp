// Teste do combate (native v6; nao vai para o jogo). Cria o mapeamento como o mw3.asi, abre uma
// segunda view como o Minecraft e confere, byte a byte com os offsets que o Java fixa:
//   - a tabela de atores (WitcherLink.readActors: seq @0, count @4, registros de 64 bytes @0x40);
//   - kEvHitActor escrito como o WitcherLink.pushEvent -> PopEvent -> combat::OnMcHit -> PopHit;
//   - kInHurt lido do anel de input como o InputBridge (a / 100 = dano do MC).
//
//   mw3_combattest.exe   (0 = PASS)
#include "../src/combat.h"
#include "../src/log.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace
{
	int g_failures = 0;

	void Check(bool ok, const char* what)
	{
		std::printf("  %s %s\n", ok ? "ok " : "BAD", what);
		if (!ok)
			++g_failures;
	}

	template <class T>
	T Get(const std::uint8_t* base, std::uint64_t off)
	{
		T v{};
		std::memcpy(&v, base + off, sizeof(T));
		return v;
	}

	template <class T>
	void Put(std::uint8_t* base, std::uint64_t off, T v)
	{
		std::memcpy(base + off, &v, sizeof(T));
	}

	bool Near(double a, double b, double eps = 1e-3)
	{
		return std::fabs(a - b) < eps;
	}

	// Como o WitcherLink.pushEvent (Java): type, actorId, a, b, c, d (float), flags, weapon (int).
	void JavaPushEvent(std::uint8_t* mc, std::uint32_t type, std::uint32_t actorId, float a, float b, float c, float d, std::uint32_t flags,
	                   std::uint32_t weapon)
	{
		using namespace mw3;
		const std::uint64_t base = proto::kOffEventRing;
		const auto          head = Get<std::uint64_t>(mc, base + 0x00);
		const std::uint64_t e = base + 0x80 + (head & (512 - 1)) * 32;
		Put<std::uint32_t>(mc, e, type);
		Put<std::uint32_t>(mc, e + 4, actorId);
		Put<float>(mc, e + 8, a);
		Put<float>(mc, e + 12, b);
		Put<float>(mc, e + 16, c);
		Put<float>(mc, e + 20, d);
		Put<std::uint32_t>(mc, e + 24, flags);
		Put<std::uint32_t>(mc, e + 28, weapon);
		Put<std::uint64_t>(mc, base + 0x00, head + 1);
	}
} // namespace

int main()
{
	using namespace mw3;
	InitLog();

	auto& link = Link::Get();
	Check(link.Create(), "Link::Create");
	if (!link.Valid())
		return 1;
	HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, proto::kMappingName);
	auto*  mc = mapping ? static_cast<std::uint8_t*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0)) : nullptr;
	Check(mc != nullptr, "segunda view (como o MC)");
	if (!mc)
		return 1;

	// ---- CopyName: corta em 23 bytes sem partir UTF-8 ------------------------------------------
	{
		char out[24];
		combat::CopyName("Drowner", out);
		Check(std::strcmp(out, "Drowner") == 0, "CopyName curto");
		combat::CopyName(std::string(40, 'a'), out);
		Check(std::strlen(out) == 23 && out[23] == 0, "CopyName trunca em 23 bytes + NUL");
		// 22 'a' + "e" com acento (2 bytes): o 2o byte cairia na posicao 23 -> o caractere inteiro sai.
		combat::CopyName(std::string(22, 'a') + "\xC3\xA9" + "zz", out);
		Check(std::strlen(out) == 22, "CopyName nao corta um caractere UTF-8 ao meio");
	}

	// ---- tabela de atores --------------------------------------------------------------------
	const std::uint64_t at = proto::kOffActorTable;
	{
		const auto seq0 = Get<std::uint32_t>(mc, at);
		combat::BeginActors();
		combat::ActorIn a;
		a.id = 1001;
		a.x = 1234.5;
		a.y = -678.25;  // W3 +Y norte -> MC z = 678.25
		a.z = 42.0;
		a.w3Yaw = 90.0f;  // W3 yaw 90 -> MC yaw 90 (W3YawToMcYaw = 180 - yaw)
		a.radius = 0.45f;
		a.height = 1.85f;
		a.health = 300.0f;
		a.maxHealth = 1200.0f;
		a.flags = proto::kActorHostile | proto::kActorInCombat;
		a.level = 12;
		a.name = "Afogador";
		combat::AddActor(a);

		combat::ActorIn big = a;
		big.id = 1002;
		big.radius = 50.0f;     // -> largura limitada a 6
		big.height = 0.01f;     // -> altura minima 0.3
		big.health = 5000.0f;   // > max -> fracao 1
		big.maxHealth = 1000.0f;
		big.flags = 0;
		big.level = 70000;      // -> 65535
		big.name.clear();
		combat::AddActor(big);

		combat::ActorIn bad = a;
		bad.id = 0;  // id invalido: descartado
		combat::AddActor(bad);
		bad.id = 1003;
		bad.x = std::nan("");  // posicao invalida: descartado
		combat::AddActor(bad);

		Check(combat::CommitActors(link) == 2, "CommitActors = 2 (id 0 e NaN descartados)");
		Check(Get<std::uint32_t>(mc, at + 0x00) == seq0 + 2, "ActorTable.seq += 2 (par)");
		Check(Get<std::uint32_t>(mc, at + 0x04) == 2, "ActorTable.count @0x04");

		const std::uint64_t r0 = at + 0x40, r1 = r0 + 64;
		Check(Get<std::uint32_t>(mc, r0 + 0) == 1001, "rec[0].actorId @+0");
		Check(Get<std::uint32_t>(mc, r0 + 4) == (proto::kActorHostile | proto::kActorInCombat), "rec[0].flags @+4 (hostil|combate)");
		Check(Near(Get<float>(mc, r0 + 8), 1234.5) && Near(Get<float>(mc, r0 + 12), 42.0) && Near(Get<float>(mc, r0 + 16), 678.25),
		      "rec[0].x/y/z @+8/+12/+16 = W3ToMc (x, z, -y)");
		Check(Near(Get<float>(mc, r0 + 20), 90.0), "rec[0].yaw @+20 = MC 90");
		Check(Near(Get<float>(mc, r0 + 24), 0.9) && Near(Get<float>(mc, r0 + 28), 1.85), "rec[0].width/height @+24/+28 = 2r, h");
		Check(Near(Get<float>(mc, r0 + 32), 0.25), "rec[0].healthFrac @+32 = 300/1200");
		Check(Get<std::uint16_t>(mc, r0 + 36) == 12, "rec[0].level @+36 (u16)");
		Check(std::strcmp(reinterpret_cast<const char*>(mc + r0 + 40), "Afogador") == 0, "rec[0].name @+40");

		Check(Get<std::uint32_t>(mc, r1 + 0) == 1002, "rec[1].actorId");
		Check(Near(Get<float>(mc, r1 + 24), combat::kMaxWidth) && Near(Get<float>(mc, r1 + 28), combat::kMinSize), "rec[1] tamanho limitado (6, 0.3)");
		Check(Near(Get<float>(mc, r1 + 32), 1.0) && Get<std::uint16_t>(mc, r1 + 36) == 0xFFFF, "rec[1] healthFrac 1, level 65535");
		Check(mc[r1 + 40] == 0, "rec[1] sem nome");

		// Cheia: so kMaxActors entram.
		combat::BeginActors();
		for (std::uint32_t i = 1; i <= proto::kMaxActors + 10; ++i)
		{
			combat::ActorIn x = a;
			x.id = i;
			combat::AddActor(x);
		}
		Check(combat::CommitActors(link) == proto::kMaxActors && Get<std::uint32_t>(mc, at + 4) == proto::kMaxActors, "limite de kMaxActors");

		combat::ClearActors(link);
		Check(Get<std::uint32_t>(mc, at + 4) == 0 && (Get<std::uint32_t>(mc, at) & 1) == 0, "ClearActors: count 0, seq par");
	}

	// ---- golpes do MC -> fila do script -----------------------------------------------------------
	{
		// Espada com crit, knockback para +x/-z do MC (= +x/+y do W3), forca 0.9.
		JavaPushEvent(mc, proto::kEvHitActor, 1001, 10.5f, 0.6f, -0.8f, 0.9f, proto::kHitCritical, proto::kWeaponBlade);
		JavaPushEvent(mc, proto::kEvHitActor, 0, 3.0f, 0, 0, 0, 0, 0);  // actorId 0: ignorado
		JavaPushEvent(mc, proto::kEvHitActor, 1002, 2.0f, 0, 0, 0.4f, proto::kHitProjectile, proto::kWeaponArrow);
		proto::McEvent ev{};
		const std::uint64_t now = 100000;
		int popped = 0;
		while (link.PopEvent(ev))
		{
			++popped;
			combat::OnMcHit(ev, now);
		}
		Check(popped == 3, "PopEvent le os 3 eventos do anel");

		combat::Hit h{};
		Check(combat::PopHit(h, now + 10) && h.actorId == 1001, "PopHit: 1o golpe (ator 1001)");
		Check(Near(h.mcDamage, 10.5) && h.flags == proto::kHitCritical && h.weapon == proto::kWeaponBlade, "golpe: dano 10.5, crit, lamina");
		Check(Near(h.pushX, 0.6) && Near(h.pushY, 0.8) && Near(h.push, 0.9), "golpe: knockback MC (0.6, -0.8) -> W3 (0.6, 0.8), forca 0.9");
		Check(combat::PopHit(h, now + 10) && h.actorId == 1002 && h.flags == proto::kHitProjectile && h.weapon == proto::kWeaponArrow,
		      "PopHit: 2o golpe (flecha; o de actorId 0 foi ignorado)");
		Check(!combat::PopHit(h, now + 10), "PopHit: fila vazia");

		// Velho demais (W3 pausado): descartado.
		combat::OnMcHit(proto::McEvent{ proto::kEvHitActor, 7, 1.0f, 0, 0, 0, 0, 0 }, now);
		Check(!combat::PopHit(h, now + combat::kHitStaleMs + 1), "golpe com mais de kHitStaleMs e descartado");

		// Outro tipo de evento nao entra.
		combat::OnMcHit(proto::McEvent{ proto::kEvPlayerDied, 7, 0, 0, 0, 0, 0, 0 }, now);
		Check(!combat::PopHit(h, now), "kEvPlayerDied nao vira golpe");

		// Fila limitada: os mais velhos saem.
		for (std::uint32_t i = 1; i <= combat::kMaxPendingHits + 5; ++i)
			combat::OnMcHit(proto::McEvent{ proto::kEvHitActor, i, 1.0f, 0, 0, 0, 0, 0 }, now);
		Check(combat::PopHit(h, now) && h.actorId == 6, "fila cheia: descarta os mais velhos");
		combat::ClearHits();
		Check(!combat::PopHit(h, now), "ClearHits");
	}

	// ---- dano no jogador -> kInHurt ------------------------------------------------------------
	{
		const std::uint64_t ir = proto::kOffInputRing;
		const auto          head0 = Get<std::uint64_t>(mc, ir + 0x00);
		Check(combat::SendPlayerHurt(link, proto::kHurtMelee, 3.25f, 1001, proto::kHurtHeavyAttack), "SendPlayerHurt melee 3.25");
		Check(!combat::SendPlayerHurt(link, proto::kHurtMelee, 0.0f, 1001, 0), "SendPlayerHurt 0 nao envia");
		Check(!combat::SendPlayerHurt(link, proto::kHurtMelee, std::nanf(""), 1001, 0), "SendPlayerHurt NaN nao envia");
		Check(combat::SendPlayerHurt(link, 99, 5000.0f, 0, 0), "SendPlayerHurt tipo invalido / dano enorme");
		Check(Get<std::uint64_t>(mc, ir + 0x00) == head0 + 2, "input head += 2");
		const std::uint64_t e0 = ir + 0x80 + ((head0 + 0) % 4096) * 16, e1 = ir + 0x80 + ((head0 + 1) % 4096) * 16;
		// InputBridge: type u16 @0, code u16 @2, a/b/c i32 @4/8/12; hurt(code, a / 100f, b, c).
		Check(Get<std::uint16_t>(mc, e0) == proto::kInHurt && Get<std::uint16_t>(mc, e0 + 2) == proto::kHurtMelee, "kInHurt, code = kHurtMelee");
		Check(Near(Get<std::int32_t>(mc, e0 + 4) / 100.0, 3.25) && Get<std::int32_t>(mc, e0 + 8) == 1001 &&
		          Get<std::int32_t>(mc, e0 + 12) == static_cast<std::int32_t>(proto::kHurtHeavyAttack),
		      "kInHurt a/100 = 3.25 MC, b = atacante, c = heavy");
		Check(Get<std::uint16_t>(mc, e1 + 2) == proto::kHurtOther && Get<std::int32_t>(mc, e1 + 4) == static_cast<std::int32_t>(combat::kMaxHurt * 100.0f),
		      "kInHurt: tipo invalido -> kHurtOther, dano limitado a kMaxHurt");
	}

	std::printf("combattest: %s (%d falha(s))\n", g_failures ? "FAIL" : "PASS", g_failures);
	UnmapViewOfFile(mc);
	CloseHandle(mapping);
	return g_failures ? 1 : 0;
}

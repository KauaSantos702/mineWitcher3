// Combate W3 <-> MC (native v6), adaptado do SkyCraft skse/src/Combat.cpp (WriteActorTable, ApplyHit,
// SendHurt; Copyright (c) 2026 chasmlol, MIT License, ver THIRD-PARTY-NOTICES.md).
//
// Tres fluxos, todos no layout de protocol/mw3_protocol.h (o Java ja le exatamente isso):
//   1. W3 -> MC, tabela de atores (@0x12000): o script enumera os atores perto do Geralt e manda um por
//      um (BeginActors / AddActor / CommitActors). Aqui viram ActorRecord em coordenadas do MC e a tabela
//      e escrita de uma vez (seqlock). O W3Combat.java cria um WitcherActorEntity invisivel para cada um,
//      e o ProxySync.java os poe na posicao exata a cada frame: os cliques do MC passam a acertar NPCs.
//   2. MC -> W3, golpes (kEvHitActor no anel de eventos): a thread do link drena o anel
//      (session.cpp DrainEvents) e entrega cada golpe a OnMcHit; o script os tira com PopHit e aplica o
//      dano pelo W3DamageAction + damageMgr.ProcessAction com o thePlayer como atacante.
//   3. W3 -> MC, dano no Geralt: o script zera a perda de vida do Geralt e manda o dano, ja convertido
//      para pontos de vida do MC, como kInHurt (anel de input); o W3Combat.hurtPlayer aplica com as
//      regras do MC (armadura, escudo, knockback, morte).
//
// Este modulo nao depende do WitcherNativeBridge nem da sessao (para o mw3_combattest linkar so ele,
// link.cpp e log.cpp). As natives ficam em natives_combat.cpp.
#pragma once

#include "link.h"

#include <cstdint>
#include <string>

namespace mw3::combat
{
	// ---- 1. tabela de atores (thread do script) ----------------------------------------------------
	// Um ator do W3 como o script o descreve: espaco W3 (metros, Z para cima), EulerAngles.Yaw.
	struct ActorIn
	{
		std::uint32_t id = 0;        // id estavel escolhido pelo script (0 = invalido, descartado)
		double        x = 0, y = 0, z = 0;  // pes, espaco W3
		float         w3Yaw = 0;     // GetWorldRotation().Yaw
		float         radius = 0.4f;  // raio da capsula (metros)
		float         height = 1.8f;  // altura da capsula (metros)
		float         health = 1, maxHealth = 1;
		std::uint32_t flags = 0;  // proto::ActorFlags
		int           level = 0;
		std::string   name;       // UTF-8
	};

	// Largura/altura limitadas como no SkyCraft (WriteActorTable): nada menor que 0.3 bloco, nada absurdo.
	inline constexpr float kMinSize = 0.3f;
	inline constexpr float kMaxWidth = 6.0f;
	inline constexpr float kMaxHeight = 12.0f;

	// O que vai para a memoria compartilhada (exposto para o teste).
	proto::ActorRecord MakeRecord(const ActorIn& in);
	// Copia ate 23 bytes sem cortar um caractere UTF-8 no meio; sempre termina em NUL.
	void CopyName(const std::string& name, char (&out)[24]);

	void          BeginActors();
	void          AddActor(const ActorIn& in);  // ignora id 0 e passa de proto::kMaxActors
	std::uint32_t CommitActors(Link& link);     // escreve a tabela; devolve quantos atores
	void          ClearActors(Link& link);      // tabela vazia (o MC remove todos os proxies)

	// ---- 2. golpes do MC (OnMcHit na thread do link; PopHit na thread do script) ----------------
	struct Hit
	{
		std::uint32_t actorId = 0;
		float         mcDamage = 0;       // dano do MC, ja com os modificadores dele (1 = meio coracao)
		float         pushX = 0, pushY = 0;  // direcao do knockback do MC, espaco W3 (unitaria ou 0)
		float         push = 0;           // forca do knockback do MC (0.4 golpe normal, ~0.9 correndo/Knockback)
		std::uint32_t flags = 0;          // proto::HitFlags
		std::uint32_t weapon = 0;         // proto::HitWeapon
		std::uint64_t atMs = 0;
	};

	// Golpes que o script nao tirou ate aqui (W3 pausado, menu) sao descartados: nao caem atrasados.
	inline constexpr std::uint64_t kHitStaleMs = 1000;
	inline constexpr std::size_t   kMaxPendingHits = 64;

	void OnMcHit(const proto::McEvent& ev, std::uint64_t nowMs);
	bool PopHit(Hit& out, std::uint64_t nowMs);
	void ClearHits();

	// ---- 3. dano no jogador ---------------------------------------------------------------------
	// kInHurt: code = HurtKind, a = dano do MC * 100, b = actorId do atacante (0 = nenhum), c = HurtFlags.
	// (O W3Combat.java divide por minewitcher3.damageScale, que no MineWitcher3 vale 1: o jogo ja manda
	// pontos de vida do MC.)
	inline constexpr float kMaxHurt = 1000.0f;
	bool SendPlayerHurt(Link& link, std::uint32_t kind, float mcDamage, std::uint32_t attackerId, std::uint32_t flags);
} // namespace mw3::combat

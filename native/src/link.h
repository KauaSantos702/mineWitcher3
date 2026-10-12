// Derived from SkyCraft (https://github.com/chasmlol/skycraft), Copyright (c) 2026 chasmlol, MIT License.
// Port of skse/src/Link.h (upstream commit bfcaf17) to protocol/mw3_protocol.h. See THIRD-PARTY-NOTICES.md.
#pragma once

#include "../../protocol/mw3_protocol.h"

#include <cstdint>
#include <functional>
#include <mutex>

namespace mw3
{
	// Dono do mapeamento compartilhado (o jogo cria; o Minecraft abre). Seguro para chamar de
	// qualquer thread: as escritas com mais de um produtor possivel (GameState, anel de input)
	// sao serializadas por mutex; o resto segue o modelo SPSC/seqlock do SkyCraft.
	class Link
	{
	public:
		static Link& Get();

		bool Create();
		[[nodiscard]] bool Valid() const { return base_ != nullptr; }

		// O Minecraft bateu o heartbeat dele ha pouco (kMcTimeoutMs).
		[[nodiscard]] bool McAlive() const;
		void               Heartbeat();
		// PID que o Minecraft escreveu ao abrir o mapeamento (muda quando o MC reinicia).
		[[nodiscard]] std::uint32_t McPid() const;

		// Seqlock write jogo -> MC (seq += 2 por chamada; o MC faz pacing por ele).
		void WriteGameState(const proto::GameState& state);
		void WriteWaterGrid(const proto::WaterGrid& grid);
		// Seqlock read MC -> jogo. false se nao conseguiu um snapshot consistente.
		bool ReadMcState(proto::McState& out) const;

		// Anel de input (produtor). Descarta o evento se o MC ficou um anel inteiro para tras.
		void PushInput(proto::InputType type, std::uint16_t code, std::int32_t a = 0, std::int32_t b = 0, std::int32_t c = 0);

		// Anel de colisao (produtor, uma thread so). false se o anel esta cheio.
		bool WriteCollision(proto::ColType type, const void* payload, std::uint32_t bytes);

		// Tabela de atores (produtor) e anel de eventos (consumidor).
		void WriteActors(const proto::ActorRecord* records, std::uint32_t count);
		bool PopEvent(proto::McEvent& out);
		// Entidades do mundo + contorno do bloco (seqlock read).
		bool ReadWorldEntities(proto::WorldEntities& out) const;
		// Anel de render (consumidor): fn(type, payload, bytes) para cada mensagem pendente, ate
		// ~maxBytes de payload. O payload aponta para a memoria compartilhada.
		void DrainRender(const std::function<void(std::uint32_t, const std::uint8_t*, std::uint32_t)>& fn, std::uint64_t maxBytes);

		// Triple buffer do overlay (consumidor). Sem hook de Present ainda: nao usado por enquanto.
		bool                                       AcquireOverlayFrame();
		void                                       ResetOverlay();
		[[nodiscard]] const std::uint8_t*          FrontPixels() const;
		[[nodiscard]] const proto::OverlaySlotHdr* FrontHeader() const;

	private:
		template <class T>
		T* At(std::uint64_t off) const
		{
			return reinterpret_cast<T*>(base_ + off);
		}

		void*         mapping_{ nullptr };
		std::uint8_t* base_{ nullptr };
		std::uint32_t overlayFront_{ 2 };
		std::mutex    gameStateLock_;
		std::mutex    inputLock_;
	};

	// ---- angulos (MC <-> W3) -------------------------------------------------------------------
	// MC: yaw 0 = +Z (sul), cresce no sentido horario visto de cima; pitch positivo = olhar para baixo.
	// W3 (SUPOSICAO, nao verificada em runtime): EulerAngles.Yaw 0 = +Y (norte), cresce no sentido
	// anti-horario visto de cima (frente = (-sin y, cos y)); Pitch positivo = olhar para cima.
	// Com mc = (w.x, w.z, -w.y), a frente do MC (-sin y, cos y) em xz vira (-sin y, -cos y) em xy do W3,
	// logo w3Yaw = 180 - mcYaw. Pitch: w3Pitch = -mcPitch.
	// TODO: confirmar no jogo (research/03 1.7: logar RotForward(EulerAngles(0,0,0)) e (0,90,0)). Se o
	// sinal estiver errado, mude so estas funcoes.
	float McYawToW3Yaw(float mcYaw);
	float W3YawToMcYaw(float w3Yaw);
	float McPitchToW3Pitch(float mcPitch);
	float W3PitchToMcPitch(float w3Pitch);
} // namespace mw3

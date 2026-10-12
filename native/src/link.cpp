// Derived from SkyCraft (https://github.com/chasmlol/skycraft), Copyright (c) 2026 chasmlol, MIT License.
// Port of skse/src/Link.cpp (upstream commit bfcaf17) to protocol/mw3_protocol.h. See THIRD-PARTY-NOTICES.md.
#include "link.h"

#include "log.h"

#include <windows.h>

#include <sddl.h>

#include <immintrin.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

namespace mw3
{
	// Os offsets que o Proto.java e o tools/fake_witcher.py fixam a mao (ver tambem test/linktest.cpp).
	static_assert(offsetof(proto::Header, gamePid) == 0x08 && offsetof(proto::Header, gameHeartbeatMs) == 0x10 &&
	              offsetof(proto::Header, mcHeartbeatMs) == 0x18);
	static_assert(offsetof(proto::GameState, flags) == 0x04 && offsetof(proto::GameState, posX) == 0x10 &&
	              offsetof(proto::GameState, yaw) == 0x28 && offsetof(proto::GameState, teleportSeq) == 0x30 &&
	              offsetof(proto::GameState, viewportW) == 0x34 && offsetof(proto::GameState, gameHour) == 0x3C);
	static_assert(offsetof(proto::McState, eyeX) == 0x50 && offsetof(proto::McState, tickQpc) == 0x68 &&
	              offsetof(proto::McState, tickMs) == 0xB8 && offsetof(proto::McState, cameraMode) == 0xC0 &&
	              offsetof(proto::McState, cameraDistance) == 0xC4);

	namespace
	{
		template <class T>
		std::atomic_ref<T> Atomic(T& value)
		{
			return std::atomic_ref<T>(value);
		}

		constexpr std::uint64_t kMcTimeoutMs = 8000;  // SkyCraft usa 8 s: carregar colisao/chunks trava o MC por alguns segundos

		bool Elevated()
		{
			HANDLE token = nullptr;
			if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token))
				return false;
			TOKEN_ELEVATION elevation{};
			DWORD           size = 0;
			const bool      ok = ::GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
			::CloseHandle(token);
			return ok && elevation.TokenIsElevated;
		}

		// Quem pode abrir a memoria compartilhada: este usuario do Windows (mais SYSTEM e
		// administradores), em integridade normal. Explicito porque um jogo rodando como admin a
		// deixaria so para administradores, e o Minecraft (iniciado pelo desktop, nunca elevado) nao
		// conseguiria abrir. Liberar com LocalFree.
		PSECURITY_DESCRIPTOR SharedWithThisUser()
		{
			std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)";
			HANDLE       token = nullptr;
			if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token))
			{
				DWORD size = 0;
				::GetTokenInformation(token, TokenUser, nullptr, 0, &size);
				std::vector<std::uint8_t> buffer(size);
				if (size && ::GetTokenInformation(token, TokenUser, buffer.data(), size, &size))
				{
					LPWSTR sid = nullptr;
					if (::ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &sid))
					{
						sddl += std::wstring(L"(A;;GA;;;") + sid + L")";
						::LocalFree(sid);
					}
				}
				::CloseHandle(token);
			}
			sddl += L"S:(ML;;NW;;;ME)";
			PSECURITY_DESCRIPTOR descriptor = nullptr;
			if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
			{
				Log("link: nao deu para montar a DACL da memoria compartilhada (" + std::to_string(::GetLastError()) + "); usando a padrao");
				return nullptr;
			}
			return descriptor;
		}

		float Wrap180(float a)
		{
			a = std::fmod(a, 360.0f);
			if (a > 180.0f)
				a -= 360.0f;
			else if (a <= -180.0f)
				a += 360.0f;
			return a;
		}
	} // namespace

	float McYawToW3Yaw(float mcYaw)
	{
		return Wrap180(180.0f - mcYaw);
	}

	float W3YawToMcYaw(float w3Yaw)
	{
		return Wrap180(180.0f - w3Yaw);
	}

	float McPitchToW3Pitch(float mcPitch)
	{
		return -mcPitch;
	}

	float W3PitchToMcPitch(float w3Pitch)
	{
		return -w3Pitch;
	}

	Link& Link::Get()
	{
		static Link link;
		return link;
	}

	bool Link::Create()
	{
		if (base_)
			return true;

		const auto size = proto::kMappingBytes;
		if (Elevated())
			Log("link: o jogo esta rodando como administrador");

		SECURITY_ATTRIBUTES access{ sizeof(access), SharedWithThisUser(), FALSE };
		HANDLE mapping = ::CreateFileMappingW(INVALID_HANDLE_VALUE, access.lpSecurityDescriptor ? &access : nullptr, PAGE_READWRITE,
		                                      static_cast<DWORD>(size >> 32), static_cast<DWORD>(size & 0xFFFFFFFF), proto::kMappingName);
		const DWORD created = ::GetLastError();
		if (access.lpSecurityDescriptor)
			::LocalFree(access.lpSecurityDescriptor);
		if (!mapping)
		{
			Log("link: ERRO CreateFileMapping (" + std::to_string(created) + ")");
			return false;
		}
		const bool existed = created == ERROR_ALREADY_EXISTS;
		base_ = static_cast<std::uint8_t*>(::MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0));
		if (!base_)
		{
			Log("link: ERRO MapViewOfFile (" + std::to_string(::GetLastError()) + ")");
			::CloseHandle(mapping);
			return false;
		}
		mapping_ = mapping;

		// Um mapeamento velho sobrevive se o Minecraft ainda o tem aberto de uma execucao anterior.
		// Zera tudo que o jogo possui, para os aneis e o overlay comecarem de um estado conhecido.
		auto* header = At<proto::Header>(proto::kOffHeader);
		std::memset(base_ + proto::kOffGameState, 0, sizeof(proto::GameState));
		std::memset(base_ + proto::kOffOverlayCtl, 0, 0x100);
		std::memset(base_ + proto::kOffWaterGrid, 0, sizeof(proto::WaterGrid));
		std::memset(base_ + proto::kOffInputRing, 0, proto::kInputRingDataOff);
		std::memset(base_ + proto::kOffCollisionRing, 0, proto::kColRingDataOff);
		std::memset(base_ + proto::kOffActorTable, 0, sizeof(proto::ActorTable));
		std::memset(base_ + proto::kOffEventRing, 0, proto::kEventRingDataOff);
		std::memset(base_ + proto::kOffWorldEntities, 0, sizeof(proto::WorldEntities));
		std::memset(base_ + proto::kOffRenderRing, 0, proto::kRenRingDataOff);
		header->version = proto::kVersion;
		header->gamePid = ::GetCurrentProcessId();
		header->gameHeartbeatMs = ::GetTickCount64();
		Atomic(header->magic).store(proto::kMagic, std::memory_order_release);

		Log("link: memoria compartilhada Local\\MineWitcher3_v1 (" + std::to_string(size >> 20) + " MB, " + (existed ? "reaproveitada" : "criada") + ")");
		return true;
	}

	bool Link::McAlive() const
	{
		if (!base_)
			return false;
		auto&      beat = At<proto::Header>(proto::kOffHeader)->mcHeartbeatMs;
		const auto last = Atomic(beat).load(std::memory_order_acquire);
		return last != 0 && ::GetTickCount64() - last < kMcTimeoutMs;
	}

	std::uint32_t Link::McPid() const
	{
		return base_ ? Atomic(At<proto::Header>(proto::kOffHeader)->mcPid).load(std::memory_order_acquire) : 0;
	}

	void Link::Heartbeat()
	{
		if (base_)
			Atomic(At<proto::Header>(proto::kOffHeader)->gameHeartbeatMs).store(::GetTickCount64(), std::memory_order_release);
	}

	void Link::WriteGameState(const proto::GameState& state)
	{
		if (!base_)
			return;
		std::lock_guard<std::mutex> guard(gameStateLock_);
		auto*      dst = At<proto::GameState>(proto::kOffGameState);
		auto       seq = Atomic(dst->seq);
		const auto s = seq.load(std::memory_order_relaxed);
		seq.store(s + 1, std::memory_order_relaxed);
		std::atomic_thread_fence(std::memory_order_release);
		std::memcpy(reinterpret_cast<std::uint8_t*>(dst) + 4, reinterpret_cast<const std::uint8_t*>(&state) + 4, sizeof(proto::GameState) - 4);
		seq.store(s + 2, std::memory_order_release);
	}

	void Link::WriteWaterGrid(const proto::WaterGrid& grid)
	{
		if (!base_)
			return;
		auto*      dst = At<proto::WaterGrid>(proto::kOffWaterGrid);
		auto       seq = Atomic(dst->seq);
		const auto s = seq.load(std::memory_order_relaxed);
		seq.store(s + 1, std::memory_order_relaxed);
		std::atomic_thread_fence(std::memory_order_release);
		std::memcpy(reinterpret_cast<std::uint8_t*>(dst) + 4, reinterpret_cast<const std::uint8_t*>(&grid) + 4, sizeof(proto::WaterGrid) - 4);
		seq.store(s + 2, std::memory_order_release);
	}

	bool Link::ReadMcState(proto::McState& out) const
	{
		if (!base_)
			return false;
		auto* src = At<proto::McState>(proto::kOffMcState);
		auto  seq = Atomic(src->seq);
		for (int attempt = 0; attempt < 64; ++attempt)
		{
			const auto s1 = seq.load(std::memory_order_acquire);
			if (s1 & 1)
			{
				_mm_pause();
				continue;
			}
			std::memcpy(&out, src, sizeof(proto::McState));
			std::atomic_thread_fence(std::memory_order_acquire);
			if (seq.load(std::memory_order_relaxed) == s1)
				return true;
		}
		return false;
	}

	void Link::PushInput(proto::InputType type, std::uint16_t code, std::int32_t a, std::int32_t b, std::int32_t c)
	{
		if (!base_)
			return;
		// WndProc e a thread do link produzem; o anel e SPSC, entao um produtor por vez.
		std::lock_guard<std::mutex> guard(inputLock_);
		auto*      ring = base_ + proto::kOffInputRing;
		auto&      headRef = *reinterpret_cast<std::uint64_t*>(ring + proto::kInputRingHeadOff);
		auto&      tailRef = *reinterpret_cast<std::uint64_t*>(ring + proto::kInputRingTailOff);
		const auto head = Atomic(headRef).load(std::memory_order_relaxed);
		const auto tail = Atomic(tailRef).load(std::memory_order_acquire);
		if (head - tail >= proto::kInputRingEntries)
			return;
		auto* entry = reinterpret_cast<proto::InputEvent*>(ring + proto::kInputRingDataOff) + (head & (proto::kInputRingEntries - 1));
		*entry = { static_cast<std::uint16_t>(type), code, a, b, c };
		Atomic(headRef).store(head + 1, std::memory_order_release);
	}

	bool Link::WriteCollision(proto::ColType type, const void* payload, std::uint32_t bytes)
	{
		if (!base_)
			return false;
		auto*          ring = base_ + proto::kOffCollisionRing;
		auto&          headRef = *reinterpret_cast<std::uint64_t*>(ring + proto::kColRingHeadOff);
		auto&          tailRef = *reinterpret_cast<std::uint64_t*>(ring + proto::kColRingTailOff);
		auto*          data = ring + proto::kColRingDataOff;
		constexpr auto size = proto::kColRingDataBytes;

		const std::uint64_t msgBytes = (sizeof(proto::ColMsgHeader) + bytes + 7) & ~7ull;
		if (msgBytes > size / 2)
		{
			Log("link: mensagem de colisao grande demais (" + std::to_string(msgBytes) + " bytes)");
			return false;
		}
		auto       head = Atomic(headRef).load(std::memory_order_relaxed);
		const auto tail = Atomic(tailRef).load(std::memory_order_acquire);
		auto       pos = head % size;
		const auto padBytes = (pos + msgBytes > size) ? size - pos : 0;
		if (size - (head - tail) < msgBytes + padBytes)
			return false;
		if (padBytes)
		{
			*reinterpret_cast<proto::ColMsgHeader*>(data + pos) = { proto::kColPad, 0 };
			head += padBytes;
			pos = 0;
		}
		*reinterpret_cast<proto::ColMsgHeader*>(data + pos) = { type, bytes };
		std::memcpy(data + pos + sizeof(proto::ColMsgHeader), payload, bytes);
		Atomic(headRef).store(head + msgBytes, std::memory_order_release);
		return true;
	}

	void Link::WriteActors(const proto::ActorRecord* records, std::uint32_t count)
	{
		if (!base_)
			return;
		auto*      table = At<proto::ActorTable>(proto::kOffActorTable);
		auto       seq = Atomic(table->seq);
		const auto s = seq.load(std::memory_order_relaxed);
		seq.store(s + 1, std::memory_order_relaxed);
		std::atomic_thread_fence(std::memory_order_release);
		const auto n = (std::min)(count, proto::kMaxActors);
		table->count = n;
		if (n)
			std::memcpy(table->actors, records, sizeof(proto::ActorRecord) * n);
		seq.store(s + 2, std::memory_order_release);
	}

	bool Link::PopEvent(proto::McEvent& out)
	{
		if (!base_)
			return false;
		auto*      ring = base_ + proto::kOffEventRing;
		auto&      headRef = *reinterpret_cast<std::uint64_t*>(ring + proto::kEventRingHeadOff);
		auto&      tailRef = *reinterpret_cast<std::uint64_t*>(ring + proto::kEventRingTailOff);
		const auto head = Atomic(headRef).load(std::memory_order_acquire);
		auto       tail = Atomic(tailRef).load(std::memory_order_relaxed);
		if (tail >= head)
			return false;
		if (head - tail > proto::kEventRingEntries)
			tail = head - proto::kEventRingEntries;
		out = reinterpret_cast<const proto::McEvent*>(ring + proto::kEventRingDataOff)[tail & (proto::kEventRingEntries - 1)];
		Atomic(tailRef).store(tail + 1, std::memory_order_release);
		return true;
	}

	bool Link::ReadWorldEntities(proto::WorldEntities& out) const
	{
		if (!base_)
			return false;
		auto* src = At<proto::WorldEntities>(proto::kOffWorldEntities);
		auto  seq = Atomic(src->seq);
		for (int attempt = 0; attempt < 16; ++attempt)
		{
			const auto s1 = seq.load(std::memory_order_acquire);
			if (s1 & 1)
			{
				_mm_pause();
				continue;
			}
			const auto count = (std::min)(src->count, proto::kMaxWorldEntities);
			std::memcpy(&out, src, offsetof(proto::WorldEntities, entities) + sizeof(proto::WorldEntity) * count);
			out.count = count;
			std::atomic_thread_fence(std::memory_order_acquire);
			if (seq.load(std::memory_order_relaxed) == s1)
				return true;
		}
		return false;
	}

	void Link::DrainRender(const std::function<void(std::uint32_t, const std::uint8_t*, std::uint32_t)>& fn, std::uint64_t maxBytes)
	{
		if (!base_)
			return;
		auto*          ring = base_ + proto::kOffRenderRing;
		auto&          headRef = *reinterpret_cast<std::uint64_t*>(ring + proto::kRenRingHeadOff);
		auto&          tailRef = *reinterpret_cast<std::uint64_t*>(ring + proto::kRenRingTailOff);
		const auto     head = Atomic(headRef).load(std::memory_order_acquire);
		auto           tail = Atomic(tailRef).load(std::memory_order_relaxed);
		auto*          data = ring + proto::kRenRingDataOff;
		constexpr auto size = proto::kRenRingDataBytes;
		std::uint64_t  done = 0;
		while (tail < head && done < maxBytes)
		{
			const auto  pos = tail % size;
			const auto* hdr = reinterpret_cast<const proto::ColMsgHeader*>(data + pos);
			if (hdr->type == proto::kRenPad)
			{
				tail += size - pos;
				continue;
			}
			fn(hdr->type, data + pos + sizeof(proto::ColMsgHeader), hdr->payloadBytes);
			const auto msgBytes = (sizeof(proto::ColMsgHeader) + hdr->payloadBytes + 7) & ~7ull;
			tail += msgBytes;
			done += msgBytes;
		}
		Atomic(tailRef).store(tail, std::memory_order_release);
	}

	bool Link::AcquireOverlayFrame()
	{
		if (!base_)
			return false;
		auto& state = At<proto::OverlayCtl>(proto::kOffOverlayCtl)->state;
		if (!(Atomic(state).load(std::memory_order_acquire) & proto::kOverlayDirty))
			return false;
		const auto old = Atomic(state).exchange(overlayFront_, std::memory_order_acq_rel);
		overlayFront_ = old & 3;
		return true;
	}

	void Link::ResetOverlay()
	{
		if (!base_)
			return;
		Atomic(At<proto::OverlayCtl>(proto::kOffOverlayCtl)->state).store(0, std::memory_order_release);
		overlayFront_ = 2;
	}

	const std::uint8_t* Link::FrontPixels() const
	{
		return base_ + proto::kOffOverlayPixels + proto::kOverlaySlotBytes * overlayFront_;
	}

	const proto::OverlaySlotHdr* Link::FrontHeader() const
	{
		return At<proto::OverlaySlotHdr>(proto::kOffOverlaySlotHdr + sizeof(proto::OverlaySlotHdr) * overlayFront_);
	}
} // namespace mw3

// Teste de console do link (nao vai para o jogo). Cria o mapeamento como o mw3.asi, abre uma segunda
// view como o Minecraft abriria e confere os bytes nos offsets que tools/fake_witcher.py e
// fabric/.../Proto.java fixam a mao. Tambem faz o papel do MC (McState + heartbeat) para testar a
// leitura por seqlock e as conversoes de coordenadas/angulos.
//
//   mw3_linktest.exe            roda os testes e sai (0 = PASS)
//   mw3_linktest.exe --hold 30  idem, e segura o mapeamento 30 s (para um leitor externo, ex. Python)
#include "../src/link.h"
#include "../src/log.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
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

	bool Near(double a, double b)
	{
		return std::fabs(a - b) < 1e-4;
	}
} // namespace

int main(int argc, char** argv)
{
	using namespace mw3;
	InitLog();

	auto& link = Link::Get();
	Check(link.Create(), "Link::Create (Local\\MineWitcher3_v1)");
	if (!link.Valid())
		return 1;

	// Segunda view, como o lado Java (OpenFileMappingW + MapViewOfFile).
	HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, proto::kMappingName);
	Check(mapping != nullptr, "OpenFileMappingW pelo nome (como o MC)");
	if (!mapping)
		return 1;
	auto* mc = static_cast<std::uint8_t*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0));
	Check(mc != nullptr, "MapViewOfFile");
	if (!mc)
		return 1;
	MEMORY_BASIC_INFORMATION mbi{};
	VirtualQuery(mc, &mbi, sizeof(mbi));
	Check(mbi.RegionSize >= proto::kMappingBytes, "tamanho do mapeamento >= kMappingBytes");

	// Header: "<IIII" magic, version, gamePid, mcPid; Q heartbeat em 0x10.
	Check(Get<std::uint32_t>(mc, 0x00) == 0x4333574D, "header.magic = MW3C");
	Check(Get<std::uint32_t>(mc, 0x04) == 1, "header.version = 1 (fake_witcher.VERSION)");
	Check(Get<std::uint32_t>(mc, 0x08) == GetCurrentProcessId(), "header.gamePid");
	Check(Get<std::uint64_t>(mc, 0x10) != 0, "header.gameHeartbeatMs");

	// GameState @0x100: "<I" seq + "<IIIdddffIIIf" (fake_witcher.write_game).
	proto::GameState gs{};
	gs.flags = proto::kGameInGame | proto::kGameLoading;
	gs.worldId = proto::WorldIdFromName("novigrad");
	gs.collisionEpoch = 7;
	gs.posX = 1234.5;
	gs.posY = 67.25;
	gs.posZ = -890.75;
	gs.yaw = 12.5f;
	gs.pitch = -30.0f;
	gs.teleportSeq = 99;
	gs.viewportW = 2560;
	gs.viewportH = 1440;
	gs.gameHour = 13.5f;
	const auto seq0 = Get<std::uint32_t>(mc, 0x100);
	link.WriteGameState(gs);
	Check(Get<std::uint32_t>(mc, 0x100) == seq0 + 2, "GameState.seq += 2 por escrita (par)");
	Check(Get<std::uint32_t>(mc, 0x104) == gs.flags, "GameState.flags @0x104");
	Check(Get<std::uint32_t>(mc, 0x108) == 0x4eb58f33, "GameState.worldId @0x108 = fnv1a('novigrad') do fake_witcher");
	Check(Get<std::uint32_t>(mc, 0x10C) == 7, "GameState.collisionEpoch @0x10C");
	Check(Get<double>(mc, 0x110) == 1234.5 && Get<double>(mc, 0x118) == 67.25 && Get<double>(mc, 0x120) == -890.75, "GameState.pos @0x110/0x118/0x120");
	Check(Get<float>(mc, 0x128) == 12.5f && Get<float>(mc, 0x12C) == -30.0f, "GameState.yaw/pitch @0x128/0x12C");
	Check(Get<std::uint32_t>(mc, 0x130) == 99, "GameState.teleportSeq @0x130");
	Check(Get<std::uint32_t>(mc, 0x134) == 2560 && Get<std::uint32_t>(mc, 0x138) == 1440, "GameState.viewport @0x134/0x138");
	Check(Get<float>(mc, 0x13C) == 13.5f, "GameState.gameHour @0x13C");

	// McState @0x200 escrito "pelo MC" (fake_witcher.read_mc: "<IIdddffffIIQfffIddd").
	Check(!link.McAlive(), "McAlive = false sem heartbeat do MC");
	Put<std::uint32_t>(mc, 0x200, 1);  // seq impar = escrevendo
	Put<std::uint32_t>(mc, 0x204, proto::kMcInWorld | proto::kMcScreenOpen);
	Put<double>(mc, 0x208, 10.0);
	Put<double>(mc, 0x210, 64.0);
	Put<double>(mc, 0x218, -20.0);
	Put<float>(mc, 0x220, 90.0f);
	Put<float>(mc, 0x224, 15.0f);
	Put<float>(mc, 0x22C, 0.75f);  // sensitivity
	Put<float>(mc, 0x240, 70.0f);  // fov
	Put<double>(mc, 0x250, 10.0);   // eyeX
	Put<double>(mc, 0x258, 65.62);  // eyeY
	Put<double>(mc, 0x260, -20.0);  // eyeZ
	Put<std::uint32_t>(mc, 0x2C0, 2);   // cameraMode
	Put<float>(mc, 0x2C4, 3.5f);        // cameraDistance
	proto::McState st{};
	Check(!link.ReadMcState(st), "ReadMcState recusa seq impar");
	Put<std::uint32_t>(mc, 0x200, 2);
	Put<std::uint32_t>(mc, 0x0C, 4242);
	Put<std::uint64_t>(mc, 0x18, GetTickCount64());
	Check(link.McAlive() && link.McPid() == 4242, "McAlive/McPid com heartbeat do MC");
	Check(link.ReadMcState(st), "ReadMcState com seq par");
	Check(st.flags == (proto::kMcInWorld | proto::kMcScreenOpen) && st.x == 10.0 && st.y == 64.0 && st.z == -20.0, "McState flags/pos");
	Check(st.yaw == 90.0f && st.pitch == 15.0f && st.sensitivity == 0.75f && st.fovDeg == 70.0f, "McState yaw/pitch/sens/fov");
	Check(st.eyeX == 10.0 && st.eyeY == 65.62 && st.eyeZ == -20.0, "McState eye @0x250");
	Check(st.cameraMode == 2 && st.cameraDistance == 3.5f, "McState cameraMode/cameraDistance @0x2C0/0x2C4");

	// Coordenadas: mc = (w.x, w.z, -w.y).
	const proto::W3Vec eye = proto::McToW3({ st.eyeX, st.eyeY, st.eyeZ });
	Check(eye.x == 10.0 && eye.y == 20.0 && eye.z == 65.62, "McToW3(eye) = (10, 20, 65.62)");
	// Angulos (suposicao documentada em link.h).
	Check(Near(McYawToW3Yaw(0.0f), 180.0) || Near(McYawToW3Yaw(0.0f), -180.0), "MC yaw 0 (sul) -> W3 yaw 180");
	Check(Near(McYawToW3Yaw(90.0f), 90.0), "MC yaw 90 (oeste) -> W3 yaw 90");
	Check(Near(McYawToW3Yaw(-90.0f), -90.0), "MC yaw -90 (leste) -> W3 yaw -90");
	Check(Near(McYawToW3Yaw(180.0f), 0.0), "MC yaw 180 (norte) -> W3 yaw 0");
	Check(Near(W3YawToMcYaw(McYawToW3Yaw(37.0f)), 37.0), "yaw ida e volta");
	Check(McPitchToW3Pitch(30.0f) == -30.0f, "MC pitch 30 (baixo) -> W3 -30");

	// Anel de input @0x1000: head u64 @+0, tail @+0x40, dados @+0x80 em "<HHiii".
	const auto head0 = Get<std::uint64_t>(mc, 0x1000);
	link.PushInput(proto::kInKey, 62, 1);
	link.PushInput(proto::kInMouseButton, 3, 0);
	link.PushInput(proto::kInScroll, 0, -120);
	Check(Get<std::uint64_t>(mc, 0x1000) == head0 + 3, "input head += 3");
	const std::uint64_t e0 = 0x1080 + ((head0 + 0) % 4096) * 16, e2 = 0x1080 + ((head0 + 2) % 4096) * 16;
	Check(Get<std::uint16_t>(mc, e0) == 1 && Get<std::uint16_t>(mc, e0 + 2) == 62 && Get<std::int32_t>(mc, e0 + 4) == 1, "input[0] = key F5 down");
	Check(Get<std::uint16_t>(mc, e2) == 3 && Get<std::int32_t>(mc, e2 + 4) == -120, "input[2] = scroll -120");

	// Anel de eventos MC -> jogo (o MC produz head; o jogo consome tail).
	{
		const std::uint64_t ring = proto::kOffEventRing;
		const auto          h = Get<std::uint64_t>(mc, ring + proto::kEventRingHeadOff);
		proto::McEvent      ev{};
		ev.type = proto::kEvPlayerDied;
		ev.actorId = 77;
		std::memcpy(mc + ring + proto::kEventRingDataOff + (h & (proto::kEventRingEntries - 1)) * sizeof(proto::McEvent), &ev, sizeof(ev));
		Put<std::uint64_t>(mc, ring + proto::kEventRingHeadOff, h + 1);
		proto::McEvent got{};
		Check(link.PopEvent(got) && got.type == proto::kEvPlayerDied && got.actorId == 77, "PopEvent le o kEvPlayerDied");
		Check(!link.PopEvent(got) && Get<std::uint64_t>(mc, ring + proto::kEventRingTailOff) == h + 1, "PopEvent: anel vazio, tail = head");
	}

	// Anel de render: duas mensagens; DrainRender consome tudo e descarta (tail = head).
	{
		const std::uint64_t ring = proto::kOffRenderRing;
		std::uint8_t*       data = mc + ring + proto::kRenRingDataOff;
		std::uint64_t       head = Get<std::uint64_t>(mc, ring + proto::kRenRingHeadOff);
		auto                write = [&](std::uint32_t type, std::uint32_t bytes) {
			const std::uint64_t       msg = (sizeof(proto::ColMsgHeader) + bytes + 7) & ~7ull;
			const proto::ColMsgHeader hdr{ type, bytes };
			std::memcpy(data + head % proto::kRenRingDataBytes, &hdr, sizeof(hdr));
			head += msg;
		};
		write(proto::kRenAvatar, 100);
		write(proto::kRenTexture, 4096);
		Put<std::uint64_t>(mc, ring + proto::kRenRingHeadOff, head);
		int           count = 0;
		std::uint64_t total = 0;
		link.DrainRender(
		    [&](std::uint32_t, const std::uint8_t*, std::uint32_t b) {
			    ++count;
			    total += b;
		    },
		    proto::kRenRingDataBytes);
		Check(count == 2 && total == 4196 && Get<std::uint64_t>(mc, ring + proto::kRenRingTailOff) == head, "DrainRender consome as mensagens (tail = head)");
	}

	std::printf("linktest: %s (%d falha(s))\n", g_failures ? "FAIL" : "PASS", g_failures);

	if (argc >= 3 && std::strcmp(argv[1], "--hold") == 0)
	{
		const int seconds = std::atoi(argv[2]);
		std::printf("segurando o mapeamento por %d s\n", seconds);
		std::fflush(stdout);
		for (int i = 0; i < seconds * 10; ++i)
		{
			link.Heartbeat();
			Sleep(100);
		}
	}

	UnmapViewOfFile(mc);
	CloseHandle(mapping);
	return g_failures ? 1 : 0;
}

// Stage A collision export, game side. See collision.h. The region scheduling (nearest first,
// refresh next to the player, epoch reset on world change / Minecraft reconnect, worker thread
// writing the ring) follows SkyCraft skse/src/Collision.cpp (Update / Reset / WorkerLoop / Send),
// Copyright (c) 2026 chasmlol, MIT License. See THIRD-PARTY-NOTICES.md.
#include "collision.h"

#include "collision_mesh.h"
#include "config.h"
#include "link.h"
#include "log.h"
#include "session.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace mw3::collision
{
	namespace
	{
		using colmesh::FloorDiv;
		using colmesh::Sample;

		constexpr int           kSpb = colmesh::kSamplesPerBlock;
		constexpr int           kRS = colmesh::kRegionSamples;
		constexpr int           kEdge = colmesh::kInputEdge;
		constexpr int           kScanRadiusBlocks = 32;  // columns traced around the player
		constexpr int           kScanRadius = kScanRadiusBlocks * kSpb;  // in samples
		constexpr float         kScanUp = 24.0f;     // trace from this far above the feet...
		constexpr float         kScanDown = 32.0f;   // ...to this far below
		constexpr float         kCoverMargin = 8.0f;  // rescan once the feet are this close to a column's scanned range
		constexpr int           kNearSamples = 6 * kSpb;  // refreshed often (doors, things moving)
		constexpr std::uint64_t kRefreshNearMs = 3000;
		constexpr std::uint64_t kMaxAgeMs = 90000;   // older samples are traced again
		constexpr std::uint64_t kResendMinMs = 250;  // a column goes to Minecraft at most this often
		constexpr std::uint64_t kEvictCheckMs = 1000;
		constexpr int           kEvictRadiusBlocks = kScanRadiusBlocks + 24;  // we forget (resend later) columns past this; Minecraft keeps them
		constexpr int           kStore = 2 * (kScanRadius + kRS + 2) + 8;  // toroidal sample store edge (no aliasing in range)
		constexpr int           kMaxChecksPerCall = 4096;
		constexpr int           kMaxColumnsPerFrame = 4;
		constexpr int           kDefaultBudget = 200;  // traces per frame (research/04 2.3: ~200-500)
		constexpr int           kMaxHits = 16;

		std::uint64_t Key(int a, int b)
		{
			return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(a)) << 32) | static_cast<std::uint32_t>(b);
		}

		int PosMod(int v, int m)
		{
			const int r = v % m;
			return r < 0 ? r + m : r;
		}

		struct Slot
		{
			Sample        s{};
			std::uint64_t stamp = 0;
			bool          used = false;
		};

		struct Job
		{
			enum Kind
			{
				kClear,
				kColumn
			} kind = kClear;
			std::uint32_t       epoch = 0;
			int                 rx = 0, rz = 0, ry0 = 0, ry1 = -1;
			std::vector<Sample> cells;    // kEdge * kEdge
			std::vector<bool>   present;  // kEdge * kEdge
		};

		// ---- worker: builds and writes the ring messages (single ring producer) ---------------
		std::mutex                 g_queueLock;
		std::condition_variable    g_queueCv;
		std::deque<Job>            g_queue;
		std::atomic<std::uint32_t> g_epoch{ 0 };
		std::atomic<std::uint64_t> g_trisSent{ 0 };
		std::atomic<std::uint64_t> g_columnsSent{ 0 };
		std::atomic<bool>          g_loggedDrop{ false };

		bool Send(proto::ColType type, const std::vector<std::uint8_t>& payload, std::uint32_t epoch)
		{
			auto& link = Link::Get();
			for (int attempt = 0; attempt < 2000; ++attempt)
			{
				if (link.WriteCollision(type, payload.data(), static_cast<std::uint32_t>(payload.size())))
					return true;
				if (g_epoch.load() != epoch)
					return false;  // a reset came in: this message is stale anyway
				Sleep(1);  // ring full: Minecraft is behind (or gone)
			}
			if (!g_loggedDrop.exchange(true))
				Log("colisao: o anel ficou cheio por 2 s; mensagem descartada (o Minecraft parou de ler?)");
			return false;
		}

		void RunJob(const Job& job)
		{
			std::vector<std::uint8_t> payload;
			if (job.kind == Job::kClear)
			{
				payload.resize(4);
				std::memcpy(payload.data(), &job.epoch, 4);
				Send(proto::kColClear, payload, job.epoch);
				return;
			}
			colmesh::ColumnInput input;
			input.rx = job.rx;
			input.rz = job.rz;
			for (std::size_t n = 0; n < input.cells.size(); ++n)
				input.cells[n] = job.present[n] ? &job.cells[n] : nullptr;
			std::vector<colmesh::Tri> tris;
			colmesh::BuildTriangles(input, tris);
			for (int ry = job.ry0; ry <= job.ry1; ++ry)
			{
				// SkyCraft order: the triangles first, then the voxels (which mark the region known).
				colmesh::BuildTrisPayload(tris, job.rx, ry, job.rz, job.epoch, payload);
				if (!Send(proto::kColTris, payload, job.epoch))
					return;
				g_trisSent += (payload.size() - sizeof(proto::ColRegion)) / sizeof(proto::ColTri);
				colmesh::BuildRegionPayload(input, ry, job.epoch, payload);
				if (!Send(proto::kColRegion, payload, job.epoch))
					return;
			}
			++g_columnsSent;
		}

		void WorkerLoop()
		{
			for (;;)
			{
				Job job;
				{
					std::unique_lock<std::mutex> lock(g_queueLock);
					g_queueCv.wait(lock, [] { return !g_queue.empty(); });
					job = std::move(g_queue.front());
					g_queue.pop_front();
				}
				if (job.epoch != g_epoch.load())
					continue;
				try
				{
					RunJob(job);
				}
				catch (const std::exception& e)
				{
					Log(std::string("colisao: erro no worker: ") + e.what());
				}
			}
		}

		void Enqueue(Job&& job)
		{
			std::lock_guard<std::mutex> lock(g_queueLock);
			g_queue.push_back(std::move(job));
			g_queueCv.notify_one();
		}

		std::size_t QueueSize()
		{
			std::lock_guard<std::mutex> lock(g_queueLock);
			return g_queue.size();
		}

		// ---- capture (mw3.ini [Collision] bCapture=1): every committed column, for --replay -----
		// Format: collision.h. Script thread only; buffered, flushed every 2 s.
		struct Capture
		{
			bool                      enabled = false;
			bool                      full = false;
			HANDLE                    file = INVALID_HANDLE_VALUE;
			std::uint64_t             bytes = 0;  // file size so far
			std::vector<std::uint8_t> buffer;
			std::uint64_t             lastFlush = 0;
		};

		Capture& Cap()
		{
			static Capture capture;
			return capture;
		}

		std::string Narrow(const std::wstring& w)
		{
			std::string out;
			for (const wchar_t ch : w)
				out += ch < 128 ? static_cast<char>(ch) : '?';
			return out;
		}

		std::wstring CapturePath()
		{
			wchar_t      path[MAX_PATH]{};
			const DWORD  len = GetModuleFileNameW(nullptr, path, MAX_PATH);
			std::wstring p(path, len < MAX_PATH ? len : 0);
			const auto   slash = p.find_last_of(L"\\/");
			return (slash == std::wstring::npos ? std::wstring() : p.substr(0, slash + 1)) + L"mw3_colcapture.bin";
		}

		void CaptureOpen()
		{
			Capture& c = Cap();
			if (config::GetInt(L"Collision", L"bCapture", 0) == 0)
				return;
			const std::wstring path = CapturePath();
			c.file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (c.file == INVALID_HANDLE_VALUE)
			{
				Log("colisao: captura ligada mas nao abriu " + Narrow(path));
				return;
			}
			LARGE_INTEGER size{};
			GetFileSizeEx(c.file, &size);
			c.bytes = static_cast<std::uint64_t>(size.QuadPart);
			c.enabled = true;
			if (c.bytes == 0)
			{
				const char          magic[8] = { 'M', 'W', '3', 'C', 'O', 'L', 'C', '1' };
				const std::uint32_t head[2] = { kCaptureVersion, 16 };
				c.buffer.insert(c.buffer.end(), magic, magic + 8);
				c.buffer.insert(c.buffer.end(), reinterpret_cast<const std::uint8_t*>(head), reinterpret_cast<const std::uint8_t*>(head) + 8);
			}
			Log("colisao: captura ligada em " + Narrow(path) + " (" + std::to_string(c.bytes / 1024) + " KB ja escritos, limite " +
			    std::to_string(kCaptureMaxBytes >> 20) + " MB)");
		}

		void CaptureFlush(bool force)
		{
			Capture& c = Cap();
			if (!c.enabled || c.buffer.empty())
				return;
			const std::uint64_t now = GetTickCount64();
			if (!force && c.buffer.size() < (256u << 10) && now - c.lastFlush < 2000)
				return;
			c.lastFlush = now;
			DWORD written = 0;
			WriteFile(c.file, c.buffer.data(), static_cast<DWORD>(c.buffer.size()), &written, nullptr);
			c.bytes += written;
			c.buffer.clear();
		}

		template <class T>
		void Put(std::vector<std::uint8_t>& b, const T& v)
		{
			const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
			b.insert(b.end(), p, p + sizeof(T));
		}

		bool CaptureRoom(std::size_t record)
		{
			Capture& c = Cap();
			if (!c.enabled || c.full)
				return false;
			if (c.bytes + c.buffer.size() + record > kCaptureMaxBytes)
			{
				c.full = true;
				CaptureFlush(true);
				Log("colisao: captura chegou ao limite de " + std::to_string(kCaptureMaxBytes >> 20) + " MB; parou de gravar");
				return false;
			}
			return true;
		}

		void CaptureEpoch(std::uint32_t epoch, std::uint32_t worldId)
		{
			if (!CaptureRoom(12))
				return;
			auto& b = Cap().buffer;
			Put(b, kCaptureEpoch);
			Put(b, std::uint8_t{ 0 });
			Put(b, std::uint16_t{ 0 });
			Put(b, epoch);
			Put(b, worldId);
		}

		void CaptureColumn(int i, int k, float lo, float hi, const float* down, int nd, const float* up, int nu, float water)
		{
			const std::size_t size = 32 + 4 * static_cast<std::size_t>(nd + nu);
			if (!CaptureRoom(size))
				return;
			auto& b = Cap().buffer;
			Put(b, kCaptureColumn);
			Put(b, static_cast<std::uint8_t>(nd));
			Put(b, static_cast<std::uint8_t>(nu));
			Put(b, std::uint8_t{ 0 });
			Put(b, static_cast<std::int32_t>(i));
			Put(b, static_cast<std::int32_t>(k));
			Put(b, static_cast<float>((i + 0.5) * colmesh::kSampleStep));   // W3 x
			Put(b, static_cast<float>(-(k + 0.5) * colmesh::kSampleStep));  // W3 y
			Put(b, lo);
			Put(b, hi);
			Put(b, water);
			for (int n = 0; n < nd; ++n)
				Put(b, down[n]);
			for (int n = 0; n < nu; ++n)
				Put(b, up[n]);
		}

		// ---- scheduler (script thread) --------------------------------------------------------
		struct Sent
		{
			int           ry0, ry1;
			std::uint64_t at;
		};

		struct State
		{
			bool                                started = false;
			bool                                initialized = false;
			int                                 budget = kDefaultBudget;
			std::vector<Slot>                   store;
			std::vector<std::pair<int, int>>    offsets;  // sorted by distance
			std::uint32_t                       worldEpoch = 0;
			std::uint32_t                       worldId = 0;
			std::uint32_t                       mcPid = 0;
			std::uint64_t                       now = 0;
			double                              px = 0, py = 0, pz = 0;  // player feet, MC
			int                                 originI = 0, originK = 0;
			bool                                haveOrigin = false;
			std::size_t                         cursor = 0;
			bool                                pending = false;
			int                                 pi = 0, pk = 0;
			float                               plo = 0, phi = 0;
			float                               down[kMaxHits]{};
			float                               up[kMaxHits]{};
			int                                 downCount = 0, upCount = 0;
			std::unordered_set<std::uint64_t>   dirty;
			std::unordered_map<std::uint64_t, Sent> sent;
			std::uint64_t                       lastEvict = 0;
			std::uint64_t                       lastStats = 0;
			std::uint64_t                       samples = 0;
			std::uint32_t                       captureEpoch = 0;
			proto::WaterGrid                    lastWater{};
			std::uint64_t                       lastWaterAt = 0;
		};

		State& S()
		{
			static State state;
			return state;
		}

		Slot& SlotAt(State& st, int i, int k)
		{
			return st.store[static_cast<std::size_t>(PosMod(k, kStore) * kStore + PosMod(i, kStore))];
		}

		const Sample* Find(State& st, int i, int k)
		{
			const Slot& slot = SlotAt(st, i, k);
			if (!slot.used || slot.s.i != i || slot.s.k != k || st.now - slot.stamp > kMaxAgeMs)
				return nullptr;
			return &slot.s;
		}

		void StartOnce(State& st)
		{
			if (st.started)
				return;
			st.started = true;
			st.budget = std::clamp(config::GetInt(L"Collision", L"iTracesPerFrame", kDefaultBudget), 16, 4000);
			st.store.resize(static_cast<std::size_t>(kStore) * kStore);
			for (int dk = -kScanRadius; dk <= kScanRadius; ++dk)
				for (int di = -kScanRadius; di <= kScanRadius; ++di)
					if (di * di + dk * dk <= kScanRadius * kScanRadius)
						st.offsets.emplace_back(di, dk);
			std::stable_sort(st.offsets.begin(), st.offsets.end(), [](const auto& a, const auto& b) {
				return a.first * a.first + a.second * a.second < b.first * b.first + b.second * b.second;
			});
			std::thread(WorkerLoop).detach();
			CaptureOpen();
			Log("colisao: Stage A ligado (raio " + std::to_string(kScanRadiusBlocks) + " blocos, passo " + std::to_string(colmesh::kSampleStep) +
			    ", " + std::to_string(st.budget) + " traces/frame, " + std::to_string(st.offsets.size()) + " colunas por varredura)");
		}

		void Reset(State& st, const char* why)
		{
			const std::uint32_t epoch = g_epoch.load() + 1;
			g_epoch = epoch;
			{
				std::lock_guard<std::mutex> lock(g_queueLock);
				g_queue.clear();
			}
			for (Slot& slot : st.store)
				slot.used = false;
			st.dirty.clear();
			st.sent.clear();
			st.pending = false;
			st.cursor = 0;
			st.haveOrigin = false;
			st.lastWaterAt = 0;
			st.captureEpoch = epoch;  // recorded once the new world id is known (Begin)
			Job clear;
			clear.kind = Job::kClear;
			clear.epoch = epoch;
			Enqueue(std::move(clear));
			Log(std::string("colisao: nova epoca ") + std::to_string(epoch) + " (" + why + ")");
		}

		bool Needed(State& st, int i, int k, int d2)
		{
			const Slot& slot = SlotAt(st, i, k);
			if (!slot.used || slot.s.i != i || slot.s.k != k)
				return true;
			const std::uint64_t age = st.now - slot.stamp;
			if (age > kMaxAgeMs || (d2 <= kNearSamples * kNearSamples && age > kRefreshNearMs))
				return true;
			// The feet left the height range this column was scanned over (climbed / fell a lot).
			return st.py < slot.s.lo + kCoverMargin || st.py > slot.s.hi - kCoverMargin;
		}

		bool SameSample(const Sample& a, const Sample& b)
		{
			if (a.count != b.count || std::fabs(a.water - b.water) > 0.01f)
				return false;
			for (std::uint32_t n = 0; n < a.count; ++n)
			{
				const bool openA = a.bottom[n] <= a.lo + 0.01f, openB = b.bottom[n] <= b.lo + 0.01f;
				if (std::fabs(a.top[n] - b.top[n]) > 0.01f || openA != openB || (!openA && std::fabs(a.bottom[n] - b.bottom[n]) > 0.01f))
					return false;
			}
			return true;
		}

		// Every region column whose mesh reads sample (i, k): its own and, near an edge, the
		// neighbours (their margin is kMargin samples: corner smoothing and the ramp test).
		void MarkDirty(State& st, int i, int k)
		{
			const int rx = FloorDiv(i, kRS), rz = FloorDiv(k, kRS);
			for (int dz = -1; dz <= 1; ++dz)
			{
				for (int dx = -1; dx <= 1; ++dx)
				{
					const int i0 = (rx + dx) * kRS, k0 = (rz + dz) * kRS;
					if (i >= i0 - colmesh::kMargin && i < i0 + kRS + colmesh::kMargin && k >= k0 - colmesh::kMargin && k < k0 + kRS + colmesh::kMargin)
						st.dirty.insert(Key(rx + dx, rz + dz));
				}
			}
		}

		void FlushDirty(State& st)
		{
			int                        jobs = 0;
			std::vector<std::uint64_t> keys(st.dirty.begin(), st.dirty.end());
			// Nearest columns first.
			const int pcx = FloorDiv(static_cast<int>(std::floor(st.px)), colmesh::kRegionSize);
			const int pcz = FloorDiv(static_cast<int>(std::floor(st.pz)), colmesh::kRegionSize);
			auto      dist = [&](std::uint64_t key) {
                const int rx = static_cast<int>(static_cast<std::int32_t>(key >> 32)), rz = static_cast<int>(static_cast<std::int32_t>(key & 0xFFFFFFFF));
                return (rx - pcx) * (rx - pcx) + (rz - pcz) * (rz - pcz);
			};
			std::sort(keys.begin(), keys.end(), [&](std::uint64_t a, std::uint64_t b) { return dist(a) < dist(b); });

			for (const std::uint64_t key : keys)
			{
				if (jobs >= kMaxColumnsPerFrame)
					break;
				const int rx = static_cast<int>(static_cast<std::int32_t>(key >> 32));
				const int rz = static_cast<int>(static_cast<std::int32_t>(key & 0xFFFFFFFF));
				const int i0 = rx * kRS, k0 = rz * kRS;

				// Out of reach for good: it would never complete.
				const double cx = (rx + 0.5) * colmesh::kRegionSize - st.px, cz = (rz + 0.5) * colmesh::kRegionSize - st.pz;
				if (std::sqrt(cx * cx + cz * cz) > kScanRadiusBlocks + colmesh::kRegionSize)
				{
					st.dirty.erase(key);
					continue;
				}
				const auto sentIt = st.sent.find(key);
				if (sentIt != st.sent.end() && st.now - sentIt->second.at < kResendMinMs)
					continue;

				bool  complete = true;
				float minLo = std::numeric_limits<float>::max(), maxHi = std::numeric_limits<float>::lowest();
				for (int dk = 0; dk < kRS && complete; ++dk)
				{
					for (int di = 0; di < kRS; ++di)
					{
						const Sample* s = Find(st, i0 + di, k0 + dk);
						if (!s)
						{
							complete = false;
							break;
						}
						minLo = std::min(minLo, s->lo);
						maxHi = std::max(maxHi, s->hi);
					}
				}
				if (!complete)
					continue;

				Job job;
				job.kind = Job::kColumn;
				job.epoch = g_epoch.load();
				job.rx = rx;
				job.rz = rz;
				job.ry0 = FloorDiv(static_cast<int>(std::floor(minLo)), colmesh::kRegionSize);
				job.ry1 = FloorDiv(static_cast<int>(std::floor(maxHi)), colmesh::kRegionSize);
				job.cells.resize(static_cast<std::size_t>(kEdge * kEdge));
				job.present.assign(static_cast<std::size_t>(kEdge * kEdge), false);
				for (int dk = -colmesh::kMargin; dk < kRS + colmesh::kMargin; ++dk)
				{
					for (int di = -colmesh::kMargin; di < kRS + colmesh::kMargin; ++di)
					{
						const std::size_t n = colmesh::ColumnInput::Index(di, dk);
						if (const Sample* s = Find(st, i0 + di, k0 + dk))
						{
							job.cells[n] = *s;
							job.present[n] = true;
						}
					}
				}
				st.sent[key] = { job.ry0, job.ry1, st.now };
				st.dirty.erase(key);
				Enqueue(std::move(job));
				++jobs;
			}
		}

		void EvictFar(State& st)
		{
			if (st.now - st.lastEvict < kEvictCheckMs)
				return;
			st.lastEvict = st.now;
			for (auto it = st.sent.begin(); it != st.sent.end();)
			{
				const int    rx = static_cast<int>(static_cast<std::int32_t>(it->first >> 32));
				const int    rz = static_cast<int>(static_cast<std::int32_t>(it->first & 0xFFFFFFFF));
				const double cx = (rx + 0.5) * colmesh::kRegionSize - st.px, cz = (rz + 0.5) * colmesh::kRegionSize - st.pz;
				if (std::sqrt(cx * cx + cz * cz) <= kEvictRadiusBlocks)
				{
					++it;
					continue;
				}
				// Only our own bookkeeping goes (SkyCraft Collision.cpp:284-285, 323-325). Minecraft keeps
				// what it has there: an empty payload would make it count the region as known and empty
				// (no ground), and the hold would release the player into a fall when coming back.
				it = st.sent.erase(it);
			}
		}

		void WriteWater(State& st)
		{
			proto::WaterGrid grid{};
			grid.originX = static_cast<std::int32_t>(std::floor(st.px)) - static_cast<std::int32_t>(proto::kWaterGridSize / 2);
			grid.originZ = static_cast<std::int32_t>(std::floor(st.pz)) - static_cast<std::int32_t>(proto::kWaterGridSize / 2);
			grid.worldId = st.worldId;
			for (std::uint32_t z = 0; z < proto::kWaterGridSize; ++z)
			{
				for (std::uint32_t x = 0; x < proto::kWaterGridSize; ++x)
				{
					// The water sample of block column (bx, bz) is the one at its (0.25, 0.25) corner.
					const Sample* s = Find(st, (grid.originX + static_cast<int>(x)) * kSpb, (grid.originZ + static_cast<int>(z)) * kSpb);
					grid.surface[z * proto::kWaterGridSize + x] = s && s->water > -1.0e29f ? s->water : proto::kNoWater;
				}
			}
			if (st.lastWaterAt != 0 && st.now - st.lastWaterAt < 500 && std::memcmp(&grid, &st.lastWater, sizeof(grid)) == 0)
				return;
			st.lastWater = grid;
			st.lastWaterAt = st.now;
			Link::Get().WriteWaterGrid(grid);
		}

		void LogStats(State& st)
		{
			if (st.now - st.lastStats < 10000)
				return;
			st.lastStats = st.now;
			Log("colisao: " + std::to_string(st.samples) + " colunas tracadas, " + std::to_string(g_columnsSent.load()) + " regioes 8x8 enviadas, " +
			    std::to_string(g_trisSent.load()) + " triangulos, fila " + std::to_string(QueueSize()));
		}
	} // namespace

	bool Begin(double w3x, double w3y, double w3z)
	{
		State& st = S();
		if (!Link::Get().Valid() || !session::Linked())
			return false;
		StartOnce(st);
		st.now = GetTickCount64();

		std::uint32_t worldId = 0;
		const std::uint32_t worldEpoch = session::WorldEpoch(&worldId);
		const std::uint32_t pid = Link::Get().McPid();
		if (!st.initialized)
			Reset(st, "primeira conexao");
		else if (pid != st.mcPid)
			Reset(st, "Minecraft reconectou");
		else if (worldEpoch != st.worldEpoch)
			Reset(st, "troca de mundo");
		st.initialized = true;
		st.mcPid = pid;
		st.worldEpoch = worldEpoch;
		st.worldId = worldId;
		if (st.captureEpoch)
		{
			CaptureEpoch(st.captureEpoch, worldId);
			st.captureEpoch = 0;
		}
		// Settle after a load / world change / reconnect (SkyCraft Game.cpp:93-96, 997): columns traced
		// while the game is still streaming come back empty and would reach Minecraft as "no ground".
		if (session::Settling())
			return false;

		const proto::McVec feet = proto::W3ToMc({ w3x, w3y, w3z });
		st.px = feet.x;
		st.py = feet.y;
		st.pz = feet.z;
		const int pi = static_cast<int>(std::floor(st.px * kSpb));
		const int pk = static_cast<int>(std::floor(st.pz * kSpb));
		if (!st.haveOrigin || std::abs(pi - st.originI) >= 2 || std::abs(pk - st.originK) >= 2)
		{
			st.originI = pi;
			st.originK = pk;
			st.haveOrigin = true;
			st.cursor = 0;
		}

		FlushDirty(st);
		EvictFar(st);
		WriteWater(st);
		LogStats(st);
		CaptureFlush(false);
		return true;
	}

	int Next()
	{
		State& st = S();
		if (!st.initialized || !st.haveOrigin)
			return 0;
		st.pending = false;  // an uncommitted column is dropped (picked again later)
		int checks = 0;
		while (st.cursor < st.offsets.size())
		{
			if (++checks > kMaxChecksPerCall)
				return 0;
			const auto [di, dk] = st.offsets[st.cursor++];
			const int i = st.originI + di, k = st.originK + dk;
			if (!Needed(st, i, k, di * di + dk * dk))
				continue;
			st.pending = true;
			st.pi = i;
			st.pk = k;
			st.plo = static_cast<float>(st.py) - kScanDown;
			st.phi = static_cast<float>(st.py) + kScanUp;
			st.downCount = 0;
			st.upCount = 0;
			return (PosMod(i, kSpb) == 0 && PosMod(k, kSpb) == 0) ? 2 : 1;
		}
		st.cursor = 0;  // pass done; the next frame starts again from the feet (near refresh)
		return 0;
	}

	float PendingX()
	{
		return static_cast<float>((S().pi + 0.5) * colmesh::kSampleStep);
	}

	float PendingY()
	{
		return static_cast<float>(-(S().pk + 0.5) * colmesh::kSampleStep);  // W3 y = -MC z
	}

	float PendingTop()
	{
		return S().phi;
	}

	float PendingBottom()
	{
		return S().plo;
	}

	void HitDown(float w3z)
	{
		State& st = S();
		if (st.pending && st.downCount < kMaxHits)
			st.down[st.downCount++] = w3z;
	}

	void HitUp(float w3z)
	{
		State& st = S();
		if (st.pending && st.upCount < kMaxHits)
			st.up[st.upCount++] = w3z;
	}

	void Commit(float w3Water)
	{
		State& st = S();
		if (!st.pending)
			return;
		st.pending = false;
		Sample sample;
		sample.i = st.pi;
		sample.k = st.pk;
		sample.lo = st.plo;
		sample.hi = st.phi;
		const float water = w3Water < -1.0e6f ? std::numeric_limits<float>::quiet_NaN() : w3Water;
		colmesh::BuildIntervals(sample, st.down, st.downCount, st.up, st.upCount, water);
		CaptureColumn(st.pi, st.pk, st.plo, st.phi, st.down, st.downCount, st.up, st.upCount, water);

		Slot&      slot = SlotAt(st, st.pi, st.pk);
		const bool same = slot.used && slot.s.i == sample.i && slot.s.k == sample.k && SameSample(slot.s, sample);
		slot.s = sample;
		slot.stamp = st.now;
		slot.used = true;
		++st.samples;
		if (!same)
			MarkDirty(st, st.pi, st.pk);
	}

	int TraceBudget()
	{
		return S().budget;
	}

	void FlushCapture()
	{
		CaptureFlush(true);
	}
} // namespace mw3::collision

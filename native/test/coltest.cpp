// Teste de console da colisao Stage A (nao vai para o jogo).
//
// Monta um mundo sintetico (chao plano, um penhasco de 4 m, uma rampa, um telhado sobre o chao),
// gera as amostras como o script faria (BuildIntervals com os hits de StaticTrace), passa pelo
// collision_mesh, escreve no anel de colisao REAL do link (Link::WriteCollision) e le o anel com um
// port linha a linha do lado Java:
//   W3Collision.drainOnce / readTris / readRegion / fillInfo / isKnown / hasSolidBelow
//   W3Client.holdUntilReady (condicao de soltar)
//   W3Tri + TriCollider.resolve / groundAt (colisor suave do jogador)
// e confere que o jogador do MC fica de pe, e solto, para nas paredes, sobe a rampa e bate no teto.
// A "cidade sintetica" reproduz o piso de Toussaint (Static 4 cm sobre o terreno, fundo enterrado,
// raios que ignoram faces de costas como o PhysX), degraus, meio-fio, rua inclinada e um telhado.
//
//   mw3_coltest.exe                    (0 = PASS)
//   mw3_coltest.exe --replay <arquivo>  refaz as malhas de uma captura (mw3.ini [Collision] bCapture=1)
#include "../src/collision.h"
#include "../src/collision_mesh.h"
#include "../src/config.h"
#include "../src/link.h"
#include "../src/log.h"
#include "../src/session.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

// ---- stubs: collision.cpp linked without the game (session.cpp needs the WndProc hook, config the ini)
namespace
{
	bool          g_stubLinked = true;
	bool          g_stubCapture = false;
	std::uint32_t g_stubWorldEpoch = 1;
} // namespace

namespace mw3::session
{
	bool Linked()
	{
		return g_stubLinked;
	}

	std::uint32_t WorldEpoch(std::uint32_t* worldId)
	{
		if (worldId)
			*worldId = 0x1234u;
		return g_stubWorldEpoch;
	}

	bool Settling()
	{
		return false;
	}
} // namespace mw3::session

namespace mw3::config
{
	int GetInt(const wchar_t*, const wchar_t* key, int fallback)
	{
		return g_stubCapture && std::wcscmp(key, L"bCapture") == 0 ? 1 : fallback;
	}
} // namespace mw3::config

namespace
{
	using namespace mw3;
	using colmesh::Sample;

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

	// ---- port of fabric/.../world/W3Tri.java -----------------------------------------------------
	struct JTri
	{
		double ax, ay, az, bx, by, bz, cx, cy, cz;
		double nx, ny, nz;
		double minX, minY, minZ, maxX, maxY, maxZ;
		bool   walkable;

		explicit JTri(const float* v)
		{
			ax = v[0], ay = v[1], az = v[2], bx = v[3], by = v[4], bz = v[5], cx = v[6], cy = v[7], cz = v[8];
			const double ux = bx - ax, uy = by - ay, uz = bz - az;
			const double wx = cx - ax, wy = cy - ay, wz = cz - az;
			const double qx = uy * wz - uz * wy, qy = uz * wx - ux * wz, qz = ux * wy - uy * wx;
			const double len = std::sqrt(qx * qx + qy * qy + qz * qz);
			if (len < 1e-12)
				nx = 0, ny = 1, nz = 0;
			else
				nx = qx / len, ny = qy / len, nz = qz / len;
			minX = std::min({ ax, bx, cx }), minY = std::min({ ay, by, cy }), minZ = std::min({ az, bz, cz });
			maxX = std::max({ ax, bx, cx }), maxY = std::max({ ay, by, cy }), maxZ = std::max({ az, bz, cz });
			walkable = std::fabs(ny) >= 0.7;
		}
		[[nodiscard]] bool Degenerate() const { return maxX - minX < 1e-9 && maxZ - minZ < 1e-9; }
		static double Edge(double px, double pz, double x0, double z0, double x1, double z1) { return (x1 - x0) * (pz - z0) - (z1 - z0) * (px - x0); }
		[[nodiscard]] double HeightAt(double x, double z) const
		{
			if (x < minX - 1e-9 || x > maxX + 1e-9 || z < minZ - 1e-9 || z > maxZ + 1e-9 || std::fabs(ny) < 0.05)
				return std::numeric_limits<double>::quiet_NaN();
			const double d1 = Edge(x, z, ax, az, bx, bz), d2 = Edge(x, z, bx, bz, cx, cz), d3 = Edge(x, z, cx, cz, ax, az);
			const bool   neg = d1 < -1e-12 || d2 < -1e-12 || d3 < -1e-12;
			const bool   pos = d1 > 1e-12 || d2 > 1e-12 || d3 > 1e-12;
			if (neg && pos)
				return std::numeric_limits<double>::quiet_NaN();
			return ay - (nx * (x - ax) + nz * (z - az)) / ny;
		}
	};

	// ---- port of W3Collision.java (consumer) -------------------------------------------------------
	using Key3 = std::tuple<int, int, int>;
	struct McStore
	{
		int                                  epoch = -1;
		std::map<Key3, std::vector<JTri>>    tris;     // TRIS (per region)
		std::map<Key3, int>                  fill;     // FILL (per block), presence = SHAPES
		std::map<Key3, std::array<std::uint64_t, 8>> bits;  // the SHAPES themselves (8 layers of 8x8)
		std::set<Key3>                       known;    // KNOWN_REGIONS
		std::vector<std::pair<int, int>>     order;    // (type, count) of every message, for layout checks
		int                                  staleDropped = 0;
		int                                  unknownTypes = 0;

		static int FloorDivJ(int v, int d) { return colmesh::FloorDiv(v, d); }
		static int Floor(double v) { return static_cast<int>(std::floor(v)); }

		void Clear(int e)
		{
			tris.clear();
			fill.clear();
			bits.clear();
			known.clear();
			epoch = e;
		}

		void ReadRegion(const std::uint8_t* s, std::uint64_t p)
		{
			const int minX = Get<std::int32_t>(s, p), minY = Get<std::int32_t>(s, p + 4), minZ = Get<std::int32_t>(s, p + 8);
			const int maxX = Get<std::int32_t>(s, p + 12), maxY = Get<std::int32_t>(s, p + 16), maxZ = Get<std::int32_t>(s, p + 20);
			const int msgEpoch = Get<std::int32_t>(s, p + 24), count = Get<std::int32_t>(s, p + 28);
			order.emplace_back(2, count);
			if (epoch == -1)
				epoch = msgEpoch;
			if (msgEpoch != epoch)
			{
				++staleDropped;
				return;
			}
			std::map<Key3, int>                          fresh;
			std::map<Key3, std::array<std::uint64_t, 8>> freshBits;
			std::uint64_t                                e = p + 32;  // COL_REGION_HEADER_BYTES
			for (int i = 0; i < count; ++i, e += 80)  // COL_BLOCK_BYTES
			{
				const int x = Get<std::int32_t>(s, e), y = Get<std::int32_t>(s, e + 4), z = Get<std::int32_t>(s, e + 8);
				bool      any = false;
				int       cnt = 0, info = 0, top = 0;
				std::array<std::uint64_t, 8> layers{};
				for (int ly = 0; ly < 8; ++ly)
				{
					const std::uint64_t layer = Get<std::uint64_t>(s, e + 16 + ly * 8);
					layers[static_cast<std::size_t>(ly)] = layer;
					any |= layer != 0;
					cnt += static_cast<int>(__popcnt64(layer));
					if (layer)
					{
						info |= ly < 4 ? (1 << 10) : (1 << 11);
						top = ly;
					}
				}
				if (any)
				{
					fresh[{ x, y, z }] = info | cnt | top << 12;
					freshBits[{ x, y, z }] = layers;
				}
			}
			for (int x = minX; x <= maxX; ++x)
				for (int y = minY; y <= maxY; ++y)
					for (int z = minZ; z <= maxZ; ++z)
					{
						const auto it = fresh.find({ x, y, z });
						if (it != fresh.end())
						{
							fill[{ x, y, z }] = it->second;
							bits[{ x, y, z }] = freshBits[{ x, y, z }];
						}
						else
						{
							fill.erase({ x, y, z });
							bits.erase({ x, y, z });
						}
					}
			for (int rx = FloorDivJ(minX, 8); rx <= FloorDivJ(maxX, 8); ++rx)
				for (int ry = FloorDivJ(minY, 8); ry <= FloorDivJ(maxY, 8); ++ry)
					for (int rz = FloorDivJ(minZ, 8); rz <= FloorDivJ(maxZ, 8); ++rz)
						known.insert({ rx, ry, rz });
		}

		void ReadTris(const std::uint8_t* s, std::uint64_t p)
		{
			const int minX = Get<std::int32_t>(s, p), minY = Get<std::int32_t>(s, p + 4), minZ = Get<std::int32_t>(s, p + 8);
			const int msgEpoch = Get<std::int32_t>(s, p + 24), count = Get<std::int32_t>(s, p + 28);
			order.emplace_back(3, count);
			if (epoch == -1)
				epoch = msgEpoch;
			if (msgEpoch != epoch)
			{
				++staleDropped;
				return;
			}
			std::vector<JTri> kept;
			std::uint64_t     e = p + 32;
			for (int i = 0; i < count; ++i, e += 40)  // COL_TRI_BYTES
			{
				float v[9];
				for (int k = 0; k < 9; ++k)
					v[k] = Get<float>(s, e + k * 4);
				JTri t(v);
				if (!t.Degenerate())
					kept.push_back(t);
			}
			tris[{ FloorDivJ(minX, 8), FloorDivJ(minY, 8), FloorDivJ(minZ, 8) }] = std::move(kept);
		}

		// WitcherLink.collisionHead/Tail + W3Collision.drainOnce
		bool Drain(std::uint8_t* s)
		{
			const std::uint64_t ring = proto::kOffCollisionRing;
			const std::uint64_t head = Get<std::uint64_t>(s, ring + 0x00);
			std::uint64_t       tail = Get<std::uint64_t>(s, ring + 0x40);
			if (tail >= head)
				return false;
			const std::uint64_t data = ring + 0x80, size = proto::kColRingDataBytes;
			while (tail < head)
			{
				const std::uint64_t pos = tail % size;
				const int           type = Get<std::int32_t>(s, data + pos);
				const int           payloadBytes = Get<std::int32_t>(s, data + pos + 4);
				if (type == 0)
				{
					tail += size - pos;
					continue;
				}
				const std::uint64_t payload = data + pos + 8;
				if (type == 1)
					Clear(Get<std::int32_t>(s, payload));
				else if (type == 2)
					ReadRegion(s, payload);
				else if (type == 3)
					ReadTris(s, payload);
				else
					++unknownTypes;
				tail += (8 + static_cast<std::uint64_t>(payloadBytes) + 7) & ~7ull;
			}
			std::memcpy(s + ring + 0x40, &tail, 8);
			return true;
		}

		[[nodiscard]] bool IsKnown(int x, int y, int z) const { return known.count({ FloorDivJ(x, 8), FloorDivJ(y, 8), FloorDivJ(z, 8) }) != 0; }
		[[nodiscard]] bool HasSolidBelow(int x, int y, int z, int depth) const
		{
			for (int dy = 0; dy <= depth; ++dy)
				for (int dx = -1; dx <= 1; ++dx)
					for (int dz = -1; dz <= 1; ++dz)
						if (fill.count({ x + dx, y - dy, z + dz }))
							return true;
			return false;
		}
		[[nodiscard]] bool Active() const { return !known.empty(); }
		// W3Collision.trisKnownAround
		[[nodiscard]] bool TrisKnownAround(double x, double y, double z, double r) const
		{
			if (tris.empty())
				return false;
			const int y0 = Floor(y) - 1, y1 = Floor(y);
			for (int rx = FloorDivJ(Floor(x - r), 8); rx <= FloorDivJ(Floor(x + r), 8); ++rx)
				for (int rz = FloorDivJ(Floor(z - r), 8); rz <= FloorDivJ(Floor(z + r), 8); ++rz)
					for (int ry = FloorDivJ(y0, 8); ry <= FloorDivJ(y1, 8); ++ry)
						if (!tris.count({ rx, ry, rz }))
							return false;
			return true;
		}
		// W3Collision.insideSolid (VoxelShape.toAabbs of the block = its set bits)
		[[nodiscard]] bool InsideSolid(double x, double y, double z) const
		{
			const int  bx = Floor(x), by = Floor(y), bz = Floor(z);
			const auto it = bits.find({ bx, by, bz });
			if (it == bits.end())
				return false;
			const int vx = std::min(7, static_cast<int>((x - bx) * 8)), vy = std::min(7, static_cast<int>((y - by) * 8)), vz = std::min(7, static_cast<int>((z - bz) * 8));
			return ((it->second[static_cast<std::size_t>(vy)] >> (vx + vz * 8)) & 1) != 0;
		}

		void TrianglesNear(double x0, double y0, double z0, double x1, double y1, double z1, std::vector<const JTri*>& out) const
		{
			for (int rx = FloorDivJ(Floor(x0), 8); rx <= FloorDivJ(Floor(x1), 8); ++rx)
				for (int ry = FloorDivJ(Floor(y0), 8); ry <= FloorDivJ(Floor(y1), 8); ++ry)
					for (int rz = FloorDivJ(Floor(z0), 8); rz <= FloorDivJ(Floor(z1), 8); ++rz)
					{
						const auto it = tris.find({ rx, ry, rz });
						if (it == tris.end())
							continue;
						for (const JTri& t : it->second)
							if (t.maxX >= x0 && t.minX <= x1 && t.maxY >= y0 && t.minY <= y1 && t.maxZ >= z0 && t.minZ <= z1)
								out.push_back(&t);
					}
		}
	};

	// ---- port of TriCollider.java -----------------------------------------------------------------
	namespace tc
	{
		constexpr double kFloorRadius = 0.15, kSubstep = 0.1, kAirStep = 0.3, kEps = 1e-4;
		std::vector<std::pair<double, double>> Samples()
		{
			std::vector<std::pair<double, double>> s{ { 0, 0 } };
			for (int i = 0; i < 8; ++i)
			{
				const double a = i * 3.14159265358979 / 4;
				s.emplace_back(std::cos(a) * kFloorRadius, std::sin(a) * kFloorRadius);
			}
			for (int i = 0; i < 4; ++i)
			{
				const double a = 3.14159265358979 / 4 + i * 3.14159265358979 / 2;
				s.emplace_back(std::cos(a) * kFloorRadius * 0.5, std::sin(a) * kFloorRadius * 0.5);
			}
			return s;
		}
		const auto kSamples = Samples();

		double Floor(const std::vector<const JTri*>& tris, double x, double y, double z, bool walkableOnly, double maxAbove)
		{
			double best = -std::numeric_limits<double>::infinity();
			const double limit = y + maxAbove;
			for (const JTri* t : tris)
			{
				if ((walkableOnly && !t->walkable) || t->minY > limit)
					continue;
				for (const auto& s : kSamples)
				{
					const double h = t->HeightAt(x + s.first, z + s.second);
					if (!std::isnan(h) && h <= limit && h > best)
						best = h;
				}
			}
			return best;
		}

		double CeilingAbove(const std::vector<const JTri*>& tris, double x, double head, double z, double r)
		{
			double best = std::numeric_limits<double>::quiet_NaN();
			for (const JTri* t : tris)
			{
				if (t->maxY < head - 0.05)
					continue;
				for (const auto& s : kSamples)
				{
					const double h = t->HeightAt(x + s.first * r / kFloorRadius, z + s.second * r / kFloorRadius);
					if (!std::isnan(h) && h >= head - 0.05 && (std::isnan(best) || h < best))
						best = h;
				}
			}
			return best;
		}

		int ClipPlane(const double* in, int n, double* out, double level, bool keepAbove)
		{
			int m = 0;
			for (int i = 0; i < n; ++i)
			{
				const int    j = (i + 1) % n;
				const double ay = in[i * 3 + 1], by = in[j * 3 + 1];
				const bool   aIn = keepAbove ? ay >= level : ay <= level;
				const bool   bIn = keepAbove ? by >= level : by <= level;
				if (aIn)
				{
					out[m * 3] = in[i * 3], out[m * 3 + 1] = ay, out[m * 3 + 2] = in[i * 3 + 2];
					++m;
				}
				if (aIn != bIn)
				{
					const double s = (level - ay) / (by - ay);
					out[m * 3] = in[i * 3] + (in[j * 3] - in[i * 3]) * s, out[m * 3 + 1] = level, out[m * 3 + 2] = in[i * 3 + 2] + (in[j * 3 + 2] - in[i * 3 + 2]) * s;
					++m;
				}
			}
			return m;
		}

		int ClipToSlab(const JTri& t, double lo, double hi, double* out)
		{
			const double a[9] = { t.ax, t.ay, t.az, t.bx, t.by, t.bz, t.cx, t.cy, t.cz };
			double       tmp[18];
			const int    n = ClipPlane(a, 3, tmp, lo, true);
			return n == 0 ? 0 : ClipPlane(tmp, n, out, hi, false);
		}

		void ClosestXZ(const double* poly, int n, double x, double z, double& ox, double& oz)
		{
			double area = 0;
			for (int i = 0; i < n; ++i)
			{
				const int j = (i + 1) % n;
				area += poly[i * 3] * poly[j * 3 + 2] - poly[j * 3] * poly[i * 3 + 2];
			}
			if (std::fabs(area) > 1e-9)
			{
				bool inside = true;
				for (int i = 0; i < n && inside; ++i)
				{
					const int    j = (i + 1) % n;
					const double cross = (poly[j * 3] - poly[i * 3]) * (z - poly[i * 3 + 2]) - (poly[j * 3 + 2] - poly[i * 3 + 2]) * (x - poly[i * 3]);
					inside = area > 0 ? cross >= -1e-12 : cross <= 1e-12;
				}
				if (inside)
				{
					ox = x, oz = z;
					return;
				}
			}
			double bestD = std::numeric_limits<double>::max(), bx = poly[0], bz = poly[2];
			for (int i = 0; i < n; ++i)
			{
				const int    j = (i + 1) % n;
				const double x0 = poly[i * 3], z0 = poly[i * 3 + 2], x1 = poly[j * 3], z1 = poly[j * 3 + 2];
				const double ex = x1 - x0, ez = z1 - z0, l2 = ex * ex + ez * ez;
				const double s = l2 > 1e-12 ? std::max(0.0, std::min(1.0, ((x - x0) * ex + (z - z0) * ez) / l2)) : 0;
				const double px = x0 + ex * s, pz = z0 + ez * s, d = (px - x) * (px - x) + (pz - z) * (pz - z);
				if (d < bestD)
					bestD = d, bx = px, bz = pz;
			}
			ox = bx, oz = bz;
		}

		void PushOutOfWalls(const std::vector<const JTri*>& tris, double& x, double y, double& z, double radius, double height, double wallFrom, double step, double prevX, double prevZ)
		{
			double poly[18];
			for (int iter = 0; iter < 4; ++iter)
			{
				double bestPen = 0, bestDx = 0, bestDz = 0;
				for (const JTri* t : tris)
				{
					const double lo = y + (t->walkable ? step : wallFrom), hi = y + height - 0.02;
					if (t->maxY < lo || t->minY > hi || t->maxX < x - radius || t->minX > x + radius || t->maxZ < z - radius || t->minZ > z + radius)
						continue;
					const int n = ClipToSlab(*t, lo, hi, poly);
					if (n == 0)
						continue;
					double cx, cz;
					ClosestXZ(poly, n, x, z, cx, cz);
					const double ddx = x - cx, ddz = z - cz, d = std::sqrt(ddx * ddx + ddz * ddz);
					double       pen, dirX, dirZ;
					if (d > 1e-6)
						pen = radius - d, dirX = ddx / d, dirZ = ddz / d;
					else
					{
						const double hl = std::hypot(t->nx, t->nz);
						if (hl < 1e-6)
							continue;
						dirX = t->nx / hl, dirZ = t->nz / hl;
						if ((prevX - t->ax) * dirX + (prevZ - t->az) * dirZ < 0)
							dirX = -dirX, dirZ = -dirZ;
						pen = radius;
					}
					if (pen > bestPen)
						bestPen = pen, bestDx = dirX, bestDz = dirZ;
				}
				if (bestPen <= kEps)
					break;
				x += bestDx * (bestPen + kEps);
				z += bestDz * (bestPen + kEps);
			}
		}

		// TriCollider.resolve: returns the allowed movement.
		void Resolve(const std::vector<const JTri*>& tris, double x0, double y0, double z0, double radius, double height, double step, bool wasOnGround, double mx, double my,
		             double mz, double out[3])
		{
			if (tris.empty())
			{
				out[0] = mx, out[1] = my, out[2] = mz;
				return;
			}
			double       x = x0, y = y0, z = z0;
			const double horizontal = std::hypot(mx, mz);
			const int    steps = std::max(1, static_cast<int>(std::ceil(horizontal / kSubstep)));
			const double wallFrom = wasOnGround ? step : 0.02;
			bool         hitWall = false;
			for (int i = 0; i < steps; ++i)
			{
				const double px = x, pz = z;
				x += mx / steps;
				z += mz / steps;
				const double bx = x, bz = z;
				PushOutOfWalls(tris, x, y, z, radius, height, wallFrom, step, px, pz);
				hitWall |= bx != x || bz != z;
			}
			double dy = my;
			if (dy > 0)
			{
				const double ceiling = CeilingAbove(tris, x, y + height, z, radius * 0.8);
				if (!std::isnan(ceiling))
					dy = std::max(0.0, std::min(dy, ceiling - (y + height)));
			}
			const double walkUp = wasOnGround ? step : kAirStep;
			const double floorWalk = Floor(tris, x, y, z, true, walkUp);
			const double floorAny = Floor(tris, x, y, z, false, kEps);
			const double fl = std::max(floorWalk, floorAny);
			const double targetY = y + dy;
			double       outY;
			if (targetY <= fl)
				outY = fl - y0;
			else if (wasOnGround && dy <= 0 && floorWalk > -std::numeric_limits<double>::infinity() && y - floorWalk <= std::max(step, horizontal * 1.5))
				outY = floorWalk - y0;
			else
				outY = dy;
			out[0] = hitWall ? x - x0 : mx;
			out[1] = outY;
			out[2] = hitWall ? z - z0 : mz;
		}
	} // namespace tc

	// Minecraft player: 0.6 wide, 1.8 tall, step 0.6. One tick of movement through the triangle collider.
	struct Player
	{
		double x, y, z;
		bool   onGround = true;
		bool   guard = true;  // W3Collider's streaming guard + sunk lift (false = the old collider)
		int    lifts = 0, waits = 0;
		void   Move(const McStore& store, double mx, double my, double mz)
		{
			if (guard && store.Active())
			{
				if (!store.TrisKnownAround(x, y, z, 0.0))
				{
					++waits;
					const double dy = std::max(0.0, my);
					onGround = my <= 0 && dy > my;
					y += dy;
					return;
				}
				if ((mx != 0.0 || mz != 0.0) && !store.TrisKnownAround(x + mx, y + std::min(0.0, my), z + mz, 0.8))
				{
					++waits;
					mx = mz = 0.0;
				}
				if (store.InsideSolid(x, y + 0.5, z))
				{
					const double g = GroundAt(store, 2.0);
					if (!std::isnan(g) && g > y + 0.25)
					{
						++lifts;
						onGround = my <= 0;
						y = g;
						return;
					}
				}
			}
			std::vector<const JTri*> tris;
			// W3Collider.collide: box.expandTowards(move).inflate(1.0, 1.0 + step, 1.0)
			const double x0 = std::min(x - 0.3, x - 0.3 + mx) - 1.0, x1 = std::max(x + 0.3, x + 0.3 + mx) + 1.0;
			const double y0 = std::min(y, y + my) - 1.6, y1 = std::max(y + 1.8, y + 1.8 + my) + 1.6;
			const double z0 = std::min(z - 0.3, z - 0.3 + mz) - 1.0, z1 = std::max(z + 0.3, z + 0.3 + mz) + 1.0;
			store.TrianglesNear(x0, y0, z0, x1, y1, z1, tris);
			double r[3];
			tc::Resolve(tris, x, y, z, 0.3, 1.8, 0.6, onGround, mx, my, mz, r);
			onGround = my <= 0 && r[1] > my;  // vertical collision while falling/standing = on ground (vanilla rule)
			x += r[0], y += r[1], z += r[2];
		}
		[[nodiscard]] double GroundAt(const McStore& store, double maxAbove) const
		{
			std::vector<const JTri*> tris;
			store.TrianglesNear(x - 1, y - 4, z - 1, x + 1, y + maxAbove + 1, z + 1, tris);
			const double f = tc::Floor(tris, x, y, z, false, maxAbove);
			return f == -std::numeric_limits<double>::infinity() ? std::numeric_limits<double>::quiet_NaN() : f;
		}
	};

	// W3Client.holdUntilReady: true when it lets go of the player held at (px, py, pz) for heldMs.
	bool HoldReleases(const McStore& store, double px, double py, double pz, long long heldMs = 0)
	{
		const bool   trisReady = store.TrisKnownAround(px, py, pz, 2.0);
		const double ground = trisReady ? Player{ px, py, pz }.GroundAt(store, 0.5) : std::numeric_limits<double>::quiet_NaN();
		const bool   grounded = !std::isnan(ground) && ground >= py - 0.5;
		return (trisReady && (grounded || heldMs > 6000)) || heldMs > 30000;
	}

	// ---- the synthetic world (MC coords) ----------------------------------------------------------
	//   z in [0, 8):   ground 64 for x < 4, a 4 m cliff (top 68) for x >= 4
	//   z in [8, 16):  a ramp y = 64 + 0.3 * (x + 8)
	//   z in [-8, 0):  ground 64, and a roof slab [70.8, 71.0] over x < 4
	//   elsewhere:     ground 64
	struct Column
	{
		std::vector<float> down, up;
	};

	Column WorldAt(double x, double z)
	{
		Column c;
		if (z >= 0 && z < 8 && x >= 4)
			c.down = { 68.0f };
		else if (z >= 8 && z < 16)
			c.down = { static_cast<float>(64.0 + 0.3 * (x + 8.0)) };
		else if (z >= -8 && z < 0 && x < 4)
		{
			c.down = { 71.0f, 64.0f };
			c.up = { 70.8f };
		}
		else if (z >= -4 && z < -2 && x >= 6 && x < 10)
			c.down = { 62.5f };  // a pond: its bed, water at 63.5 (WaterAt)
		else
			c.down = { 64.0f };
		return c;
	}

	float WaterAt(double x, double z)
	{
		return (z >= -4 && z < -2 && x >= 6 && x < 10) ? 63.5f : std::numeric_limits<float>::quiet_NaN();
	}

	// StaticTrace over the synthetic world, the way mw3_collision.ws walks a column. Returns traces used.
	int TraceColumn(double mcX, double mcZ, float top, float bottom)
	{
		const Column c = WorldAt(mcX, mcZ);
		int          traces = 0;
		float        z = top;
		for (int hits = 0; hits < 6 && z > bottom; ++hits)
		{
			++traces;
			float best = -std::numeric_limits<float>::infinity();
			for (const float t : c.down)
				if (t < z && t > bottom && t > best)
					best = t;
			if (best == -std::numeric_limits<float>::infinity())
				break;
			mw3::collision::HitDown(best);
			z = best - 0.05f;
		}
		z = bottom;
		for (int hits = 0; hits < 6 && z < top; ++hits)
		{
			++traces;
			float best = std::numeric_limits<float>::infinity();
			for (const float u : c.up)
				if (u > z && u < top && u < best)
					best = u;
			if (best == std::numeric_limits<float>::infinity())
				break;
			mw3::collision::HitUp(best);
			z = best + 0.05f;
		}
		return traces;
	}

	constexpr float kScanLo = 64.0f - 32.0f, kScanHi = 64.0f + 24.0f;

	Sample MakeSample(int i, int k)
	{
		Sample s;
		s.i = i;
		s.k = k;
		s.lo = kScanLo;
		s.hi = kScanHi;
		const Column c = WorldAt((i + 0.5) * colmesh::kSampleStep, (k + 0.5) * colmesh::kSampleStep);
		colmesh::BuildIntervals(s, c.down.data(), static_cast<int>(c.down.size()), c.up.data(), static_cast<int>(c.up.size()),
		                        std::numeric_limits<float>::quiet_NaN());
		return s;
	}

	// Builds every region of column (rx, rz) and pushes it through the link ring.
	void SendColumn(Link& link, const std::map<std::pair<int, int>, Sample>& samples, int rx, int rz, std::uint32_t epoch, std::size_t* triCount)
	{
		colmesh::ColumnInput in;
		in.rx = rx;
		in.rz = rz;
		for (int dk = -colmesh::kMargin; dk < colmesh::kRegionSamples + colmesh::kMargin; ++dk)
			for (int di = -colmesh::kMargin; di < colmesh::kRegionSamples + colmesh::kMargin; ++di)
			{
				const auto it = samples.find({ rx * colmesh::kRegionSamples + di, rz * colmesh::kRegionSamples + dk });
				in.cells[colmesh::ColumnInput::Index(di, dk)] = it != samples.end() ? &it->second : nullptr;
			}
		std::vector<colmesh::Tri> tris;
		colmesh::BuildTriangles(in, tris);
		if (triCount)
			*triCount += tris.size();
		std::vector<std::uint8_t> payload;
		const int                 ry0 = colmesh::FloorDiv(static_cast<int>(std::floor(kScanLo)), 8);
		const int                 ry1 = colmesh::FloorDiv(static_cast<int>(std::floor(kScanHi)), 8);
		for (int ry = ry0; ry <= ry1; ++ry)
		{
			colmesh::BuildTrisPayload(tris, rx, ry, rz, epoch, payload);
			link.WriteCollision(proto::kColTris, payload.data(), static_cast<std::uint32_t>(payload.size()));
			colmesh::BuildRegionPayload(in, ry, epoch, payload);
			link.WriteCollision(proto::kColRegion, payload.data(), static_cast<std::uint32_t>(payload.size()));
		}
	}

	// ---- town scene (Toussaint field data): single-sided StaticTrace model ----------------------
	// PhysX culls back faces: a ray going down hits terrain and box tops below its start, never a
	// box bottom; a ray going up hits box bottoms above its start, never the terrain or a box top.
	// That is how the field column at Geralt's feet looked: Static floor top 35.3357, terrain 35.297
	// (above the second down ray's start, so never traced), the floor's buried bottom ~0.7 lower.
	struct Box
	{
		double x0, x1, z0, z1, y0, y1;
		[[nodiscard]] bool Inside(double x, double z) const { return x >= x0 && x < x1 && z >= z0 && z < z1; }
	};

	struct Scene
	{
		double (*terrain)(double x, double z) = nullptr;
		std::vector<Box> boxes;
		double feet = 64.0;  // scan range feet-32 .. feet+24

		void Trace(double x, double z, float top, float bottom, std::vector<float>& down, std::vector<float>& up) const
		{
			down.clear();
			up.clear();
			double zc = top;
			for (int hits = 0; hits < 6 && zc > bottom; ++hits)
			{
				double best = -std::numeric_limits<double>::infinity();
				if (terrain)
				{
					const double h = terrain(x, z);
					if (h < zc && h > bottom)
						best = h;
				}
				for (const Box& b : boxes)
					if (b.Inside(x, z) && b.y1 < zc && b.y1 > bottom)
						best = std::max(best, b.y1);
				if (best == -std::numeric_limits<double>::infinity())
					break;
				down.push_back(static_cast<float>(best));
				zc = best - 0.05;
			}
			zc = bottom;
			for (int hits = 0; hits < 6 && zc < top; ++hits)
			{
				double best = std::numeric_limits<double>::infinity();
				for (const Box& b : boxes)
					if (b.Inside(x, z) && b.y0 > zc && b.y0 < top)
						best = std::min(best, b.y0);
				if (best == std::numeric_limits<double>::infinity())
					break;
				up.push_back(static_cast<float>(best));
				zc = best + 0.05;
			}
		}

		[[nodiscard]] Sample MakeSample(int i, int k) const
		{
			Sample s;
			s.i = i;
			s.k = k;
			s.lo = static_cast<float>(feet - 32.0);
			s.hi = static_cast<float>(feet + 24.0);
			std::vector<float> down, up;
			Trace((i + 0.5) * colmesh::kSampleStep, (k + 0.5) * colmesh::kSampleStep, s.hi, s.lo, down, up);
			colmesh::BuildIntervals(s, down.data(), static_cast<int>(down.size()), up.data(), static_cast<int>(up.size()), std::numeric_limits<float>::quiet_NaN());
			return s;
		}

		// The real walkable surface at (x, z): highest terrain/box top at most `limit`.
		[[nodiscard]] double TopAt(double x, double z, double limit) const
		{
			double best = -std::numeric_limits<double>::infinity();
			if (terrain)
			{
				const double h = terrain(x, z);
				bool         buried = false;
				for (const Box& b : boxes)
					if (b.Inside(x, z) && b.y0 < h && b.y1 > h)
						buried = true;
				if (!buried && h <= limit)
					best = h;
			}
			for (const Box& b : boxes)
				if (b.Inside(x, z) && b.y1 <= limit && b.y1 > best)
					best = b.y1;
			return best;
		}
	};

	constexpr int kMargin = (colmesh::kInputEdge - colmesh::kRegionSamples) / 2;

	void BuildColumnTris(const std::map<std::pair<int, int>, Sample>& samples, int rx, int rz, colmesh::ColumnInput& in, std::vector<colmesh::Tri>& tris)
	{
		in.rx = rx;
		in.rz = rz;
		for (int dk = -kMargin; dk < colmesh::kRegionSamples + kMargin; ++dk)
			for (int di = -kMargin; di < colmesh::kRegionSamples + kMargin; ++di)
			{
				const auto it = samples.find({ rx * colmesh::kRegionSamples + di, rz * colmesh::kRegionSamples + dk });
				in.cells[static_cast<std::size_t>((dk + kMargin) * colmesh::kInputEdge + (di + kMargin))] = it != samples.end() ? &it->second : nullptr;
			}
		colmesh::BuildTriangles(in, tris);
	}

	struct TriKinds
	{
		std::size_t up = 0, down = 0, wall = 0;
	};
	TriKinds g_kinds;

	void CountKinds(const std::vector<colmesh::Tri>& tris)
	{
		for (const colmesh::Tri& t : tris)
		{
			const JTri j(t.v);
			if (std::fabs(j.ny) < 0.05)
				++g_kinds.wall;
			else if (j.ny > 0)
				++g_kinds.up;
			else
				++g_kinds.down;
		}
	}

	// Builds region column (rx, rz) and loads it straight into `store` (no ring). Returns triangles.
	std::size_t LoadColumn(McStore& store, const std::map<std::pair<int, int>, Sample>& samples, int rx, int rz, float lo, float hi)
	{
		colmesh::ColumnInput      in;
		std::vector<colmesh::Tri> tris;
		BuildColumnTris(samples, rx, rz, in, tris);
		CountKinds(tris);
		std::vector<std::uint8_t> payload;
		for (int ry = colmesh::FloorDiv(static_cast<int>(std::floor(lo)), 8); ry <= colmesh::FloorDiv(static_cast<int>(std::floor(hi)), 8); ++ry)
		{
			colmesh::BuildTrisPayload(tris, rx, ry, rz, static_cast<std::uint32_t>(store.epoch), payload);
			store.ReadTris(payload.data(), 0);
			colmesh::BuildRegionPayload(in, ry, static_cast<std::uint32_t>(store.epoch), payload);
			store.ReadRegion(payload.data(), 0);
		}
		return tris.size();
	}

	// Highest up-facing (walkable) triangle exactly at (x, z), at most `limit`. NaN if none.
	double PointFloor(const McStore& store, double x, double z, double limit)
	{
		std::vector<const JTri*> tris;
		store.TrianglesNear(x - 0.01, limit - 8.0, z - 0.01, x + 0.01, limit + 0.01, z + 0.01, tris);
		double best = std::numeric_limits<double>::quiet_NaN();
		for (const JTri* t : tris)
		{
			if (!t->walkable)
				continue;
			const double h = t->HeightAt(x, z);
			if (!std::isnan(h) && h <= limit && (std::isnan(best) || h > best))
				best = h;
		}
		return best;
	}

	struct MeshReport
	{
		std::size_t tops = 0, missing = 0, bad = 0, ceilings = 0;
		double      maxErr = 0.0, sumErr = 0.0;
		double      worstX = 0.0, worstZ = 0.0, worstTop = 0.0, worstGot = 0.0;
	};

	// At every sample point: the surface the player stands on vs the traced top, and no surface
	// within 1.9 m above a top unless the sample has a real underside there.
	void CheckSamples(const McStore& store, const std::map<std::pair<int, int>, Sample>& samples, int i0, int i1, int k0, int k1, MeshReport& rep)
	{
		for (int k = k0; k < k1; ++k)
			for (int i = i0; i < i1; ++i)
			{
				const auto it = samples.find({ i, k });
				if (it == samples.end())
					continue;
				const Sample& s = it->second;
				const double  x = (i + 0.5) * colmesh::kSampleStep, z = (k + 0.5) * colmesh::kSampleStep;
				for (std::uint32_t m = 0; m < s.count; ++m)
				{
					const double t = s.top[m];
					++rep.tops;
					const double h = PointFloor(store, x, z, t + 0.3);
					const double err = std::isnan(h) ? 99.0 : std::fabs(h - t);
					if (std::isnan(h))
						++rep.missing;
					if (err > 0.06)
						++rep.bad;
					rep.sumErr += std::min(err, 1.0);
					if (err > rep.maxErr)
					{
						rep.maxErr = err;
						rep.worstX = x, rep.worstZ = z, rep.worstTop = t, rep.worstGot = h;
					}
					// Ceiling: any non-vertical surface 0.05..1.9 above this top.
					const double realUnder = m > 0 && s.bottom[m - 1] > s.lo + 0.01 ? s.bottom[m - 1] : std::numeric_limits<double>::quiet_NaN();
					std::vector<const JTri*> tris;
					store.TrianglesNear(x - 0.01, t, z - 0.01, x + 0.01, t + 1.9, z + 0.01, tris);
					for (const JTri* tri : tris)
					{
						const double c = tri->HeightAt(x, z);
						if (std::isnan(c) || c <= t + 0.05 || c >= t + 1.9)
							continue;
						if (!std::isnan(realUnder) && std::fabs(c - realUnder) < 0.03)
							continue;
						++rep.ceilings;
						break;
					}
				}
			}
	}

	struct WalkReport
	{
		int    ticks = 0, stuck = 0, sink = 0, floatUp = 0;
		double maxSink = 0.0, maxFloat = 0.0;
		double x = 0, y = 0, z = 0;
	};

	// Walks the MC player (vanilla gravity) in a straight line and compares its feet with the scene.
	template <class PlayerT>
	WalkReport Walk(const McStore& store, const Scene& scene, PlayerT& p, double dx, double dz, int ticks, double stopX = 1e9, double stopZ = 1e9)
	{
		WalkReport r;
		double     vy = -0.0784;
		int        still = 0;
		for (int t = 0; t < ticks; ++t)
		{
			const double bx = p.x, bz = p.z;
			p.Move(store, dx, vy, dz);
			vy = p.onGround ? -0.0784 : (vy - 0.08) * 0.98;
			++r.ticks;
			const double moved = std::hypot(p.x - bx, p.z - bz);
			const bool   beforeStop = (dx > 0 ? p.x < stopX : true) && (dz > 0 ? p.z < stopZ : true);
			if (moved < 0.02 && beforeStop)
			{
				if (++still == 3)
					++r.stuck;
			}
			else
				still = 0;
			if (!p.onGround)
				continue;
			const double here = scene.TopAt(p.x, p.z, p.y + 0.6);
			double       foot = here;
			for (double ox = -0.15; ox <= 0.151; ox += 0.15)
				for (double oz = -0.15; oz <= 0.151; oz += 0.15)
					foot = std::max(foot, scene.TopAt(p.x + ox, p.z + oz, p.y + 0.6));
			if (p.y < here - 0.08)
			{
				++r.sink;
				r.maxSink = std::max(r.maxSink, here - p.y);
			}
			if (p.y > foot + 0.08)
			{
				++r.floatUp;
				r.maxFloat = std::max(r.maxFloat, p.y - foot);
			}
		}
		r.x = p.x, r.y = p.y, r.z = p.z;
		return r;
	}

	double TownTerrain(double x, double z)
	{
		double h = 64.0 + 0.03 * std::sin(1.7 * x + 0.3) * std::cos(2.3 * z);  // cobbles
		if (z >= 12.0)
			h += 0.25 * std::max(0.0, x);  // a 14 deg street
		return h;
	}

	Scene TownScene()
	{
		Scene s;
		s.terrain = &TownTerrain;
		s.boxes.push_back({ 1, 7, 1, 7, 63.35, 64.04 });     // A: the field case, 4 cm over terrain, bottom buried 0.69 down
		s.boxes.push_back({ 8, 12, 1, 5, 63.0, 64.5 });      // C: floor raised 0.5
		s.boxes.push_back({ 1, 5, 8, 11.5, 64.3, 64.8 });    // B: floor raised 0.8, 0.3 gap above the terrain
		const double cum[] = { 0.1, 0.25, 0.45, 0.75, 1.2, 1.75, 2.45 };  // steps 0.1 0.15 0.2 0.3 0.45 0.55 0.7
		for (int n = 0; n < 7; ++n)
			s.boxes.push_back({ 13.0 + n, n == 6 ? 22.0 : 14.0 + n, 1, 6, 60.0, 64.0 + cum[n] });
		s.boxes.push_back({ 8, 22, 10, 12, 60.0, 64.15 });   // sidewalk, 0.15 curb
		s.boxes.push_back({ 0, 8, 0, 8, 75.0, 75.3 });       // roof 11 m up
		return s;
	}

	void RunTownTests()
	{
		std::printf("cidade sintetica (Toussaint): pisos Static, degraus, meio-fio, telhado a +11 m\n");
		const Scene scene = TownScene();
		std::map<std::pair<int, int>, Sample> samples;
		for (int k = -6; k < 2 * colmesh::kRegionSamples + 6; ++k)
			for (int i = -6; i < 3 * colmesh::kRegionSamples + 6; ++i)
				samples[{ i, k }] = scene.MakeSample(i, k);

		McStore     store;
		store.epoch = 9;
		std::size_t tris = 0;
		g_kinds = {};
		for (int rz = -1; rz <= 2; ++rz)
			for (int rx = -1; rx <= 3; ++rx)
				tris += LoadColumn(store, samples, rx, rz, static_cast<float>(scene.feet - 32.0), static_cast<float>(scene.feet + 24.0));
		std::printf("    %zu triangulos em 20 colunas de regiao (%zu topo, %zu parede, %zu face de baixo)\n", tris, g_kinds.up, g_kinds.wall, g_kinds.down);

		MeshReport rep;
		CheckSamples(store, samples, 0, 3 * colmesh::kRegionSamples, 0, 2 * colmesh::kRegionSamples, rep);
		std::printf("    topo nas amostras: %zu topos, erro max %.4f (x=%.2f z=%.2f topo %.4f malha %.4f), medio %.5f, %zu > 0.06, %zu sem chao, %zu tetos falsos\n", rep.tops,
		            rep.maxErr, rep.worstX, rep.worstZ, rep.worstTop, rep.worstGot, rep.sumErr / static_cast<double>(std::max<std::size_t>(rep.tops, 1)), rep.bad, rep.missing,
		            rep.ceilings);
		Check(rep.bad == 0 && rep.missing == 0, "toda amostra: o chao da malha fica a <= 0.06 do hit tracado");
		Check(rep.ceilings == 0, "nenhum teto a < 1.9 m de um topo sem face de baixo tracada ali");

		{
			Sample s = samples[{ 8, 8 }];  // (4.25, 4.25): inside floor A
			std::printf("    amostra no piso A: %u intervalo(s): [%.3f, %.3f]%s\n", s.count, s.count ? s.bottom[s.count - 1] : 0.0f, s.count ? s.top[s.count - 1] : 0.0f,
			            s.count && s.bottom[s.count - 1] <= s.lo + 0.01f ? " aberto" : " FECHADO (face de baixo enterrada)");
		}

		// The player dipped under floor A (e.g. it walked into a region whose triangles weren't there
		// yet and fell a few ticks): where does it end up?
		{
			Player p{ 4.25, 63.6, 4.25 };
			p.onGround = false;
			double vy = -0.08;
			for (int t = 0; t < 40; ++t)
			{
				p.Move(store, 0.0, vy, 0.0);
				vy = p.onGround ? -0.0784 : (vy - 0.08) * 0.98;
			}
			std::printf("    jogador 0.44 abaixo do piso A (64.04): termina em y = %.3f\n", p.y);
			Check(std::fabs(p.y - 64.04) < 0.06, "abaixo do piso: volta para o topo (nao fica preso 0.69 abaixo)");
		}

		// Along z = 3.5: terrain, floor A (+0.04), terrain, floor C (+0.5), terrain, stairs 0.1..0.55, then a 0.7 riser.
		{
			Player     p{ -0.5, scene.TopAt(-0.5, 3.5, 70), 3.5 };
			WalkReport w = Walk(store, scene, p, 0.2, 0.0, 140, 18.6);
			std::printf("    z=3.5 (pisos + escada): x %.3f y %.3f, %d travadas, %d afundou (max %.3f), %d flutuou (max %.3f)\n", w.x, w.y, w.stuck, w.sink, w.maxSink, w.floatUp,
			            w.maxFloat);
			Check(w.x > 18.6 && w.x < 18.75 && std::fabs(w.y - 65.75) < 0.06, "sobe degraus de 0.1..0.55 e para no de 0.7");
			Check(w.stuck == 0, "sem travar nos pisos/degraus <= 0.55");
			Check(w.sink == 0 && w.floatUp == 0 && p.lifts == 0, "pes seguem o chao real (+-0.08) o caminho todo, sem 'afundou, sobe' falso");
			Player     d{ 18.5, 65.75, 3.5 };
			WalkReport down = Walk(store, scene, d, -0.2, 0.0, 94);
			std::printf("    descendo: x %.3f y %.3f, %d travadas, %d afundou, %d flutuou\n", down.x, down.y, down.stuck, down.sink, down.floatUp);
			Check(down.x < -0.29 && down.stuck == 0 && down.sink == 0, "desce tudo de volta sem travar");
		}
		// The sidewalk curb (0.15) along +z, then the 14 deg street's side is a wall.
		{
			Player     p{ 15.25, scene.TopAt(15.25, 8.3, 70), 8.3 };
			WalkReport w = Walk(store, scene, p, 0.0, 0.2, 40, 1e9, 11.6);
			std::printf("    meio-fio: z %.3f y %.3f, %d travadas, %d afundou, %d flutuou\n", w.z, w.y, w.stuck, w.sink, w.floatUp);
			Check(w.z > 11.6 && w.z < 11.75 && std::fabs(w.y - 64.15) < 0.06 && w.stuck == 0 && w.sink == 0, "sobe o meio-fio de 0.15 e para no lado da rua inclinada");
		}
		// Up the 14 deg street: smooth, no stair-stepping.
		{
			Player     p{ 0.5, scene.TopAt(0.5, 14.25, 70), 14.25 };
			double     worst = 0.0;
			int        jerks = 0;
			double     vy = -0.0784;
			for (int t = 0; t < 100; ++t)
			{
				const double y0 = p.y, x0 = p.x;
				p.Move(store, 0.2, vy, 0.0);
				vy = p.onGround ? -0.0784 : (vy - 0.08) * 0.98;
				worst = std::max(worst, std::fabs(p.y - TownTerrain(p.x, 14.25)));
				if (std::fabs((p.y - y0) - 0.25 * (p.x - x0)) > 0.06)
					++jerks;
			}
			std::printf("    rua inclinada: x %.3f y %.3f, erro max %.4f, %d trancos\n", p.x, p.y, worst, jerks);
			Check(p.x > 19.5 && worst < 0.08 && jerks == 0, "sobe a rua de 14 graus colado e sem trancos");
			Check(p.lifts == 0, "rua inclinada: nenhum 'afundou, sobe' falso");
		}
		// Against floor B (0.8 high): a wall.
		{
			Player     p{ 3.0, 64.04, 7.3 };
			WalkReport w = Walk(store, scene, p, 0.0, 0.2, 20);
			std::printf("    contra o piso B (0.8): z %.3f y %.3f\n", w.z, w.y);
			Check(w.z < 7.75 && w.z > 7.6 && w.y < 64.1, "piso de 0.8 = parede");
		}
		// Bumpy road along x.
		{
			Player     p{ 8.3, scene.TopAt(8.3, 9.0, 70), 9.0 };
			WalkReport w = Walk(store, scene, p, 0.2, 0.0, 60, 21.5);
			std::printf("    chao de pedra: x %.3f, %d travadas, %d afundou (max %.3f), %d flutuou (max %.3f)\n", w.x, w.stuck, w.sink, w.maxSink, w.floatUp, w.maxFloat);
			Check(w.stuck == 0 && w.sink == 0 && w.floatUp == 0, "chao de pedra irregular: segue sem travar");
		}

		// holdUntilReady on the triangles.
		Check(HoldReleases(store, 4.25, 64.04, 4.25), "hold: solta no piso A (triangulos ate 2 blocos + chao a <= 0.5)");
		Check(!HoldReleases(store, 4.25, 70.0, 4.25, 100) && HoldReleases(store, 4.25, 70.0, 4.25, 6001), "hold no ar: so depois de 6 s");

		// Streaming: only the region columns x < 8 have arrived. Walking east stops at the edge of what
		// is known instead of walking off the triangles and sinking; continues once the next arrives.
		{
			McStore partial;
			partial.epoch = 9;
			for (int rz = -1; rz <= 1; ++rz)
				for (int rx = -1; rx <= 0; ++rx)
					LoadColumn(partial, samples, rx, rz, static_cast<float>(scene.feet - 32.0), static_cast<float>(scene.feet + 24.0));
			Check(!HoldReleases(partial, 7.5, 64.04, 4.25) && !HoldReleases(partial, 7.5, 64.04, 4.25, 6001), "hold: nao solta a 0.5 bloco de uma regiao sem triangulos");
			Player     p{ 5.0, 64.04, 3.5 };
			WalkReport w = Walk(partial, scene, p, 0.2, 0.0, 30);
			std::printf("    regiao a leste ainda nao chegou: para em x %.3f y %.3f (%d esperas)\n", w.x, w.y, p.waits);
			Check(w.x > 6.9 && w.x < 7.21 && w.sink == 0, "para na borda do que chegou, sem afundar");
			LoadColumn(partial, samples, 1, 0, static_cast<float>(scene.feet - 32.0), static_cast<float>(scene.feet + 24.0));
			LoadColumn(partial, samples, 1, -1, static_cast<float>(scene.feet - 32.0), static_cast<float>(scene.feet + 24.0));
			LoadColumn(partial, samples, 1, 1, static_cast<float>(scene.feet - 32.0), static_cast<float>(scene.feet + 24.0));
			WalkReport w2 = Walk(partial, scene, p, 0.2, 0.0, 20);
			std::printf("    chegou: segue ate x %.3f y %.3f\n", w2.x, w2.y);
			Check(w2.x > 10.5 && std::fabs(w2.y - 64.5) < 0.06 && w2.sink == 0, "regiao chegou: segue e sobe no piso C");

			// Old collider (no guard): walks off the triangles, falls, and the region arrives late.
			McStore old;
			old.epoch = 9;
			for (int rz = -1; rz <= 1; ++rz)
				for (int rx = -1; rx <= 0; ++rx)
					LoadColumn(old, samples, rx, rz, static_cast<float>(scene.feet - 32.0), static_cast<float>(scene.feet + 24.0));
			Player o{ 5.0, 64.04, 3.5 };
			o.guard = false;
			Walk(old, scene, o, 0.2, 0.0, 17);
			std::printf("    sem a guarda: cai ate y %.3f em x %.3f antes da regiao chegar\n", o.y, o.x);
		}
	}

	// ---- --replay: meshes rebuilt from a capture (mw3.ini [Collision] bCapture=1) ---------------
	struct ReplayTotals
	{
		std::size_t segments = 0, records = 0, samples = 0, columns = 0, complete = 0, tris = 0, walks = 0, stuck = 0, sink = 0;
		MeshReport  mesh;
		TriKinds    kinds;
		std::vector<std::string> notes;  // first few problem spots, W3 coordinates
	};

	void Note(ReplayTotals& t, const char* what, double mcX, double y, double mcZ)
	{
		if (t.notes.size() >= 12)
			return;
		char line[160];
		std::snprintf(line, sizeof(line), "%s em W3 (%.2f, %.2f, %.2f)", what, mcX, -mcZ, y);
		t.notes.emplace_back(line);
	}

	void ReplaySegment(const std::map<std::pair<int, int>, Sample>& samples, ReplayTotals& tot)
	{
		constexpr int R = colmesh::kRegionSamples;
		std::set<std::pair<int, int>> columns;
		for (const auto& [key, s] : samples)
			columns.insert({ colmesh::FloorDiv(key.first, R), colmesh::FloorDiv(key.second, R) });
		tot.samples += samples.size();
		tot.columns += columns.size();

		McStore store;
		store.epoch = 1;
		std::vector<std::pair<int, int>> complete;
		const TriKinds before = g_kinds;
		g_kinds = {};
		for (const auto& [rx, rz] : columns)
		{
			bool  full = true;
			float lo = std::numeric_limits<float>::max(), hi = std::numeric_limits<float>::lowest();
			for (int dk = 0; dk < R && full; ++dk)
				for (int di = 0; di < R && full; ++di)
				{
					const auto it = samples.find({ rx * R + di, rz * R + dk });
					full = it != samples.end();
					if (full)
						lo = std::min(lo, it->second.lo), hi = std::max(hi, it->second.hi);
				}
			if (!full)
				continue;
			complete.emplace_back(rx, rz);
			tot.tris += LoadColumn(store, samples, rx, rz, lo, hi);
		}
		tot.kinds.up += g_kinds.up, tot.kinds.wall += g_kinds.wall, tot.kinds.down += g_kinds.down;
		g_kinds = before;
		tot.complete += complete.size();

		for (const auto& [rx, rz] : complete)
		{
			MeshReport rep;
			CheckSamples(store, samples, rx * R, rx * R + R, rz * R, rz * R + R, rep);
			tot.mesh.tops += rep.tops, tot.mesh.missing += rep.missing, tot.mesh.bad += rep.bad, tot.mesh.ceilings += rep.ceilings, tot.mesh.sumErr += rep.sumErr;
			if (rep.maxErr > tot.mesh.maxErr)
			{
				tot.mesh.maxErr = rep.maxErr;
				tot.mesh.worstX = rep.worstX, tot.mesh.worstZ = rep.worstZ, tot.mesh.worstTop = rep.worstTop, tot.mesh.worstGot = rep.worstGot;
			}
			if (rep.bad || rep.missing)
				Note(tot, "topo fora do hit", rep.worstX, rep.worstTop, rep.worstZ);

			// Walks: every 2 m, from every top with room for the player, 2 m in each direction.
			for (int dk = 0; dk < R; dk += 4)
				for (int di = 0; di < R; di += 4)
				{
					const Sample& s = samples.at({ rx * R + di, rz * R + dk });
					for (std::uint32_t m = 0; m < s.count; ++m)
					{
						if (m > 0 && s.bottom[m - 1] - s.top[m] < 1.85f)
							continue;
						static const double kDirs[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
						for (const auto& d : kDirs)
						{
							Player p{ (s.i + 0.5) * colmesh::kSampleStep, s.top[m], (s.k + 0.5) * colmesh::kSampleStep };
							++tot.walks;
							double vy = -0.0784;
							int    still = 0;
							bool   stuck = false, sunk = false;
							for (int t = 0; t < 10; ++t)
							{
								if (!store.TrisKnownAround(p.x + d[0] * 0.5, p.y, p.z + d[1] * 0.5, 0.8))
									break;  // the edge of what was captured
								const double bx = p.x, bz = p.z;
								p.Move(store, 0.2 * d[0], vy, 0.2 * d[1]);
								vy = p.onGround ? -0.0784 : (vy - 0.08) * 0.98;
								// Sank: below the top of the sample under the feet that it should stand on.
								const auto under = samples.find({ static_cast<int>(std::floor(p.x * 2)), static_cast<int>(std::floor(p.z * 2)) });
								if (p.onGround && under != samples.end())
								{
									double ground = -1e30;
									for (std::uint32_t n = 0; n < under->second.count; ++n)
										if (under->second.top[n] <= p.y + 0.6 && under->second.top[n] > ground)
											ground = under->second.top[n];
									if (ground > -1e29 && p.y < ground - 0.1)
										sunk = true;
								}
								if (std::hypot(p.x - bx, p.z - bz) >= 0.02)
								{
									still = 0;
									continue;
								}
								if (++still < 3)
									continue;
								// Stopped. Stuck only if the next cell is walkable from here (a step <= 0.5
								// with room for the player) and nothing in between is a real wall.
								const auto ahead = samples.find({ static_cast<int>(std::floor((p.x + d[0] * 0.5) * 2)), static_cast<int>(std::floor((p.z + d[1] * 0.5) * 2)) });
								if (ahead == samples.end())
									break;
								for (std::uint32_t n = 0; n < ahead->second.count; ++n)
								{
									const double rise = ahead->second.top[n] - p.y;
									const bool   room = n == 0 || ahead->second.bottom[n - 1] - ahead->second.top[n] >= 1.85f;
									if (rise <= 0.5 && rise >= -1.0 && room)
										stuck = true;
								}
								break;
							}
							if (stuck)
							{
								++tot.stuck;
								Note(tot, "travou", p.x, p.y, p.z);
							}
							if (sunk)
							{
								++tot.sink;
								Note(tot, "afundou", p.x, p.y, p.z);
							}
						}
					}
				}
		}
	}

	template <class T>
	bool Read(const std::vector<std::uint8_t>& b, std::size_t& at, T& v)
	{
		if (at + sizeof(T) > b.size())
			return false;
		std::memcpy(&v, b.data() + at, sizeof(T));
		at += sizeof(T);
		return true;
	}

	// Returns the number of problems found (0 = clean), or -1 if the file can't be read.
	int Replay(const wchar_t* path, ReplayTotals& tot)
	{
		std::vector<std::uint8_t> b;
		{
			HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
			if (f == INVALID_HANDLE_VALUE)
				return -1;
			LARGE_INTEGER size{};
			GetFileSizeEx(f, &size);
			b.resize(static_cast<std::size_t>(size.QuadPart));
			DWORD got = 0;
			ReadFile(f, b.data(), static_cast<DWORD>(b.size()), &got, nullptr);
			CloseHandle(f);
			b.resize(got);
		}
		if (b.size() < 16 || std::memcmp(b.data(), "MW3COLC1", 8) != 0)
			return -1;
		std::size_t                           at = 16;
		std::map<std::pair<int, int>, Sample> samples;
		auto                                  flush = [&] {
            if (!samples.empty())
            {
                ++tot.segments;
                ReplaySegment(samples, tot);
            }
            samples.clear();
		};
		while (at < b.size())
		{
			std::uint8_t type = 0;
			if (!Read(b, at, type))
				break;
			if (type == collision::kCaptureEpoch)
			{
				std::uint8_t  pad8 = 0;
				std::uint16_t pad16 = 0;
				std::uint32_t epoch = 0, world = 0;
				if (!Read(b, at, pad8) || !Read(b, at, pad16) || !Read(b, at, epoch) || !Read(b, at, world))
					break;
				flush();
				continue;
			}
			if (type != collision::kCaptureColumn)
				break;  // truncated / unknown: stop here
			std::uint8_t nd = 0, nu = 0, pad = 0;
			std::int32_t i = 0, k = 0;
			float        w3x = 0, w3y = 0, lo = 0, hi = 0, water = 0;
			if (!Read(b, at, nd) || !Read(b, at, nu) || !Read(b, at, pad) || !Read(b, at, i) || !Read(b, at, k) || !Read(b, at, w3x) || !Read(b, at, w3y) ||
			    !Read(b, at, lo) || !Read(b, at, hi) || !Read(b, at, water))
				break;
			float down[256], up[256];
			bool  ok = true;
			for (int n = 0; n < nd && ok; ++n)
				ok = Read(b, at, down[n]);
			for (int n = 0; n < nu && ok; ++n)
				ok = Read(b, at, up[n]);
			if (!ok)
				break;
			Sample s;
			s.i = i;
			s.k = k;
			s.lo = lo;
			s.hi = hi;
			colmesh::BuildIntervals(s, down, nd, up, nu, water);
			samples[{ i, k }] = s;
			++tot.records;
		}
		flush();
		return static_cast<int>(tot.mesh.bad + tot.mesh.missing + tot.mesh.ceilings + tot.stuck + tot.sink);
	}

	void PrintReplay(const ReplayTotals& t)
	{
		std::printf("replay: %zu segmento(s), %zu colunas gravadas, %zu amostras, %zu/%zu colunas de regiao completas\n", t.segments, t.records, t.samples, t.complete, t.columns);
		std::printf("  triangulos: %zu (%.0f por coluna de regiao; %zu topo, %zu parede, %zu face de baixo)\n", t.tris,
		            t.complete ? static_cast<double>(t.tris) / static_cast<double>(t.complete) : 0.0, t.kinds.up, t.kinds.wall, t.kinds.down);
		std::printf("  topo nas amostras: %zu topos, erro max %.4f (W3 %.2f, %.2f: hit %.4f, malha %.4f), medio %.5f, %zu > 0.06, %zu sem chao\n", t.mesh.tops, t.mesh.maxErr,
		            t.mesh.worstX, -t.mesh.worstZ, t.mesh.worstTop, t.mesh.worstGot, t.mesh.sumErr / static_cast<double>(std::max<std::size_t>(t.mesh.tops, 1)), t.mesh.bad,
		            t.mesh.missing);
		std::printf("  tetos falsos (< 1.9 m sobre um topo, sem face de baixo tracada): %zu\n", t.mesh.ceilings);
		std::printf("  caminhadas de 2 m: %zu, travou %zu, afundou %zu\n", t.walks, t.stuck, t.sink);
		for (const std::string& n : t.notes)
			std::printf("    %s\n", n.c_str());
	}
} // namespace

int main(int argc, char** argv)
{
	InitLog();
	if (argc >= 3 && std::strcmp(argv[1], "--replay") == 0)
	{
		std::wstring path;
		for (const char* c = argv[2]; *c; ++c)
			path += static_cast<wchar_t>(static_cast<unsigned char>(*c));
		ReplayTotals t;
		const int    problems = Replay(path.c_str(), t);
		if (problems < 0)
		{
			std::printf("replay: nao li %s (nao existe ou nao e uma captura MW3COLC1)\n", argv[2]);
			return 2;
		}
		PrintReplay(t);
		std::printf("replay: %s (%d problema(s))\n", problems ? "FAIL" : "PASS", problems);
		return problems ? 1 : 0;
	}

	// ---- BuildIntervals ---------------------------------------------------------------------------
	std::printf("BuildIntervals\n");
	{
		Sample      s;
		s.lo = 32.0f;
		s.hi = 88.0f;
		const float down[] = { 71.0f, 64.0f };
		const float up[] = { 70.8f };
		colmesh::BuildIntervals(s, down, 2, up, 1, std::numeric_limits<float>::quiet_NaN());
		Check(s.count == 2 && s.top[0] == 71.0f && s.bottom[0] == 70.8f && s.top[1] == 64.0f && s.bottom[1] == 32.0f, "telhado [70.8, 71] + chao aberto ate o fundo do scan");

		const float down2[] = { 66.0f, 65.99f, 64.0f };  // duplicate hit, rock on the ground seen from inside
		colmesh::BuildIntervals(s, down2, 3, nullptr, 0, std::numeric_limits<float>::quiet_NaN());
		Check(s.count == 1 && s.top[0] == 66.0f && s.bottom[0] == 32.0f, "topos sem face de baixo entre eles = um solido so");

		const float down3[] = { 64.0f };
		colmesh::BuildIntervals(s, down3, 1, nullptr, 0, 64.6f);
		Check(std::fabs(s.water - 64.6f) < 1e-6f, "agua 0.6 acima do chao conta");
		colmesh::BuildIntervals(s, down3, 1, nullptr, 0, 63.0f);
		Check(s.water < -1e29f, "agua abaixo do chao nao conta");
		const float bridge[] = { 66.0f, 62.0f };
		const float bridgeUnder[] = { 65.7f };
		colmesh::BuildIntervals(s, bridge, 2, bridgeUnder, 1, 63.5f);
		Check(std::fabs(s.water - 63.5f) < 1e-6f, "rio debaixo de uma ponte conta");
		colmesh::BuildIntervals(s, nullptr, 0, nullptr, 0, std::numeric_limits<float>::quiet_NaN());
		Check(s.count == 0, "coluna sem hit = vazia");
		const float many[] = { 80, 78, 76, 74, 72, 70 };
		const float manyUp[] = { 69.5f, 71.5f, 73.5f, 75.5f, 77.5f, 79.5f };
		colmesh::BuildIntervals(s, many, 6, manyUp, 6, std::numeric_limits<float>::quiet_NaN());
		Check(s.count == colmesh::kMaxIntervals && s.top[0] == 80.0f && s.bottom[0] == 79.5f && s.bottom[colmesh::kMaxIntervals - 1] == s.lo,
		      "mais lajes que kMaxIntervals: a ultima engloba o resto (e, sem nada abaixo, vai ate o fundo)");
		// Field case (Toussaint): Static floor 4 cm over the terrain. Ray 1 hits the floor; ray 2 starts
		// 5 cm under it, below the terrain (back faces culled): only the floor's buried bottom is seen.
		const float field[] = { 35.3357f };
		const float fieldUp[] = { 34.65f };
		Sample      f;
		f.lo = 35.3357f - 32.0f;
		f.hi = 35.3357f + 24.0f;
		colmesh::BuildIntervals(f, field, 1, fieldUp, 1, std::numeric_limits<float>::quiet_NaN());
		Check(f.count == 1 && f.top[0] == 35.3357f && f.bottom[0] == f.lo, "piso Static 4 cm sobre o terreno: o fundo enterrado (0.69 abaixo) nao vira teto/chao falso");
		f.lo = 32.0f;
		f.hi = 88.0f;
		const float thinDown[] = { 64.8f, 64.0f };
		const float thinUp[] = { 64.3f };
		colmesh::BuildIntervals(f, thinDown, 2, thinUp, 1, std::numeric_limits<float>::quiet_NaN());
		Check(f.count == 1 && f.top[0] == 64.8f && f.bottom[0] == f.lo, "vao de 0.3 m sob um piso = solido (sem teto a 0.3 do chao)");
		const float tableDown[] = { 66.0f, 64.0f };
		const float tableUp[] = { 65.1f };
		colmesh::BuildIntervals(f, tableDown, 2, tableUp, 1, std::numeric_limits<float>::quiet_NaN());
		Check(f.count == 2 && f.bottom[0] == 65.1f, "vao de 1.1 m (>= kMinGap) fica aberto");
	}

	// ---- the link ring, as the game writes it and Minecraft reads it ------------------------------
	auto& link = Link::Get();
	Check(link.Create(), "Link::Create");
	if (!link.Valid())
		return 1;
	HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, proto::kMappingName);
	auto*  mc = mapping ? static_cast<std::uint8_t*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0)) : nullptr;
	Check(mc != nullptr, "segunda view (lado do MC)");
	if (!mc)
		return 1;

	std::map<std::pair<int, int>, Sample> samples;
	for (int k = -2 * colmesh::kRegionSamples - 1; k < 3 * colmesh::kRegionSamples + 1; ++k)
		for (int i = -2 * colmesh::kRegionSamples - 1; i < 3 * colmesh::kRegionSamples + 1; ++i)
			samples[{ i, k }] = MakeSample(i, k);

	McStore       store;
	std::size_t   tris = 0;
	std::uint32_t epoch = 5;
	{
		const std::uint8_t clearPayload[4] = { 5, 0, 0, 0 };
		link.WriteCollision(proto::kColClear, clearPayload, 4);
	}
	// Player's column first (what MineWitcher3 sends first), then the rest, draining as MC would.
	std::printf("anel de colisao + leitor Java\n");
	SendColumn(link, samples, 0, 0, epoch, &tris);
	Check(store.Drain(mc), "drainOnce leu mensagens");
	Check(store.epoch == 5, "kColClear: epoca 5");
	Check(store.unknownTypes == 0 && store.staleDropped == 0, "nenhum tipo desconhecido / nenhuma regiao velha");
	Check(store.order.size() >= 2 && store.order[0].first == 3 && store.order[1].first == 2, "ordem: kColTris e depois kColRegion");
	Check(HoldReleases(store, 2.3, 64.0, 4.5), "holdUntilReady solta so com a coluna do jogador (known + chao abaixo)");
	Check(!HoldReleases(store, 20.3, 64.0, 4.5), "...e nao solta onde ainda nao chegou nada");
	for (int rz = -1; rz <= 1; ++rz)
		for (int rx = -1; rx <= 1; ++rx)
			if (rx || rz)
			{
				SendColumn(link, samples, rx, rz, epoch, &tris);
				store.Drain(mc);
			}
	std::printf("  (%zu triangulos em 9 colunas, %zu blocos de voxel)\n", tris, store.fill.size());
	Check(Get<std::uint64_t>(mc, proto::kOffCollisionRing) == Get<std::uint64_t>(mc, proto::kOffCollisionRing + 0x40), "anel drenado (tail == head)");

	// ---- voxels (W3Collision SHAPES / FILL) -------------------------------------------------------
	std::printf("voxels\n");
	{
		auto has = [&](int x, int y, int z) { return store.fill.count({ x, y, z }) != 0; };
		Check(has(2, 63, 4) && !has(2, 64, 4), "chao em y=64: bloco 63 ocupado, 64 livre");
		const auto it = store.fill.find({ 2, 63, 4 });
		Check(it != store.fill.end() && (((it->second >> 12) & 7) + 1) == 8 && (it->second & 0x3FF) == 512, "groundTop(2,63,4) = 1.0 (casca de 1 bloco cheia)");
		Check(!has(2, 61, 4), "casca: 2 blocos abaixo do chao fica vazio");
		Check(has(4, 67, 4) && has(4, 65, 4) && has(4, 64, 4), "penhasco: o topo (67) e a parede ate o chao vizinho (64-66) sao solidos");
		Check(has(-4, 70, -4) && !has(-4, 69, -4), "laje do telhado: bloco 70 ocupado, 69 livre (da para passar por baixo)");
	}

	// ---- the player's smooth collider (TriCollider) -------------------------------------------------
	std::printf("colisor suave do jogador (TriCollider)\n");
	{
		Player p{ 2.3, 64.0, 4.5 };
		Check(std::fabs(p.GroundAt(store, 2.5) - 64.0) < 1e-6, "groundAt no chao plano = 64.0");
		for (int t = 0; t < 10; ++t)
			p.Move(store, 0.0, -0.08, 0.0);
		Check(std::fabs(p.y - 64.0) < 1e-6 && p.onGround, "parado com gravidade: fica de pe em 64 (nao afunda)");

		// Walk into the cliff (x = 4): stops one radius before it.
		for (int t = 0; t < 20; ++t)
			p.Move(store, 0.2, -0.08, 0.0);
		std::printf("    contra o penhasco: x = %.3f y = %.3f\n", p.x, p.y);
		Check(p.x <= 4.0 - 0.3 + 0.01 && p.x > 3.5 && std::fabs(p.y - 64.0) < 1e-6, "anda ate o penhasco de 4 m e para na parede");

		// Up the ramp (16.7 deg): follows the surface.
		Player r{ -6.0, 64.0 + 0.3 * 2.0, 12.25 };
		double worst = 0.0;
		for (int t = 0; t < 40; ++t)
		{
			r.Move(store, 0.2, -0.08, 0.0);
			worst = std::max(worst, std::fabs(r.y - (64.0 + 0.3 * (r.x + 8.0))));
		}
		std::printf("    rampa: x = %.3f y = %.3f, erro maximo %.4f\n", r.x, r.y, worst);
		// TriCollider pega o ponto mais alto sob a base de raio 0.15: numa rampa de 0.3 isso e +0.045.
		Check(r.x > 1.5 && worst < 0.3 * 0.15 + 0.005, "sobe a rampa colado na superficie (so o +0.045 da base do TriCollider)");

		// Under the roof: jumping stops at the slab's underside (70.8 - 1.8 = 69.0).
		Player j{ -4.25, 64.0, -4.25 };
		j.Move(store, 0.0, 10.0, 0.0);
		std::printf("    pulo sob o telhado: y = %.3f\n", j.y);
		Check(j.y <= 69.0 + 1e-4 && j.y > 68.5, "pulo para no teto (cabeca em 70.8)");
		// Walking out from under the roof to the open ground: no wall at the roof's edge.
		Player w{ 2.0, 64.0, -4.25 };
		for (int t = 0; t < 15; ++t)
			w.Move(store, 0.2, -0.08, 0.0);
		Check(w.x > 4.5 && std::fabs(w.y - 64.0) < 1e-6, "sai de baixo do telhado sem bater (a beira da laje nao vira parede ate o chao)");
		// Standing on top of the roof.
		Player top{ -4.25, 71.0, -4.25 };
		Check(std::fabs(top.GroundAt(store, 2.5) - 71.0) < 1e-6, "em cima do telhado: chao em 71.0");
		// Walk from the plain onto the ramp row along +z (64 -> ramp at x=-2: 65.8): a 1.8 m step is a wall.
		Player s{ -2.0, 64.0, 6.0 };
		for (int t = 0; t < 20; ++t)
			s.Move(store, 0.0, -0.08, 0.2);
		std::printf("    contra o degrau da rampa: z = %.3f y = %.3f\n", s.z, s.y);
		Check(s.z <= 8.0 - 0.3 + 0.01 && std::fabs(s.y - 64.0) < 1e-6, "degrau de 1.8 m entre o chao e a rampa = parede");
	}

	RunTownTests();

	// ---- eviction --------------------------------------------------------------------------------
	std::printf("evicao\n");
	{
		std::vector<std::uint8_t> payload;
		for (int ry = 4; ry <= 11; ++ry)
		{
			colmesh::BuildEmptyPayload(1, ry, 0, epoch, payload);
			link.WriteCollision(proto::kColTris, payload.data(), static_cast<std::uint32_t>(payload.size()));
			link.WriteCollision(proto::kColRegion, payload.data(), static_cast<std::uint32_t>(payload.size()));
		}
		store.Drain(mc);
		Check(!store.fill.count({ 12, 67, 4 }) && store.IsKnown(12, 67, 4), "coluna esvaziada: sem voxels, continua conhecida");
		Check(store.tris.count({ 1, 8, 0 }) && store.tris[{ 1, 8, 0 }].empty(), "coluna esvaziada: sem triangulos");
	}

	// ---- epoch / stale ---------------------------------------------------------------------------
	{
		const std::uint8_t clearPayload[4] = { 6, 0, 0, 0 };
		link.WriteCollision(proto::kColClear, clearPayload, 4);
		SendColumn(link, samples, 0, 0, 5, nullptr);  // old epoch: Minecraft must ignore it
		store.Drain(mc);
		Check(store.epoch == 6 && store.fill.empty() && store.staleDropped > 0, "nova epoca limpa tudo e ignora mensagens da epoca velha");
	}

	// ---- the scheduler (collision.cpp) end to end, as the script drives it ------------------------
	std::printf("agendador (collision.cpp + worker), dirigido como o mw3_collision.ws\n");
	{
		auto ringHead = [&] { return Get<std::uint64_t>(mc, proto::kOffCollisionRing); };
		// Player feet: MC (2.3, 64, 4.5) = W3 (2.3, -4.5, 64).
		const double w3x = 2.3, w3y = -4.5, w3z = 64.0;

		// The scheduler run is captured (bCapture=1) and replayed below.
		wchar_t exe[MAX_PATH]{};
		GetModuleFileNameW(nullptr, exe, MAX_PATH);
		std::wstring capture(exe);
		capture = capture.substr(0, capture.find_last_of(L"\\/") + 1) + L"mw3_colcapture.bin";
		DeleteFileW(capture.c_str());
		g_stubCapture = true;

		g_stubLinked = false;
		const auto headBefore = ringHead();
		Check(!collision::Begin(w3x, w3y, w3z) && collision::Next() == 0, "sem MC linkado: Begin = false e nenhuma coluna (inerte)");
		Sleep(50);
		Check(ringHead() == headBefore, "sem MC linkado: nada escrito no anel");
		g_stubLinked = true;

		int released = -1, frames = 0, maxTraces = 0;
		for (; frames < 600; ++frames)
		{
			if (!collision::Begin(w3x, w3y, w3z))
				break;
			int budget = collision::TraceBudget(), used = 0;
			while (budget > 0)
			{
				const int kind = collision::Next();
				if (kind == 0)
					break;
				const float x = collision::PendingX(), y = collision::PendingY();
				const int   t = TraceColumn(x, -y, collision::PendingTop(), collision::PendingBottom());
				budget -= t;
				used += t;
				float water = -1.0e7f;
				if (kind == 2 && !std::isnan(WaterAt(x, -y)))
					water = WaterAt(x, -y);
				collision::Commit(water);
			}
			maxTraces = std::max(maxTraces, used);
			Sleep(1);
			store.Drain(mc);
			if (released < 0 && HoldReleases(store, 2.3, 64.0, 4.5))
				released = frames;
		}
		for (int idle = 0; idle < 100; ++idle)  // let the worker finish
		{
			Sleep(20);
			if (!store.Drain(mc) && idle > 5)
				break;
		}
		std::printf("    hold solto no frame %d; ate %d traces/frame; %zu blocos de voxel, %zu regioes conhecidas\n", released, maxTraces, store.fill.size(),
		            store.known.size());
		Check(store.epoch == 1, "primeira conexao: kColClear com a epoca 1");
		Check(released >= 0 && released <= 30, "holdUntilReady do MC solta em <= 30 frames (colunas do jogador primeiro)");
		Check(maxTraces <= collision::TraceBudget() + 12, "orcamento por frame respeitado (+ no maximo uma coluna)");
		Player p{ 2.3, 64.0, 4.5 };
		Check(std::fabs(p.GroundAt(store, 2.5) - 64.0) < 1e-6, "chao sob o jogador = 64.0");
		for (int t = 0; t < 20; ++t)
			p.Move(store, 0.2, -0.08, 0.0);
		Check(p.x <= 3.71 && p.x > 3.5, "para no penhasco tambem pelo agendador");
		Check(HoldReleases(store, -12.5, 64.0, 16.5) && HoldReleases(store, 20.5, 64.0, -6.5), "colunas a ~20 blocos tambem chegaram");

		// Water grid: 16x16 blocks around the feet.
		const std::uint64_t wg = proto::kOffWaterGrid;
		const int           ox = Get<std::int32_t>(mc, wg + 4), oz = Get<std::int32_t>(mc, wg + 8);
		auto                surface = [&](int bx, int bz) { return Get<float>(mc, wg + 16 + 4 * static_cast<std::uint64_t>((bz - oz) * 16 + (bx - ox))); };
		Check(Get<std::uint32_t>(mc, wg) != 0 && (Get<std::uint32_t>(mc, wg) & 1) == 0 && ox == 2 - 8 && oz == 4 - 8, "WaterGrid escrito (seq par), origem = pes - 8");
		Check(std::fabs(surface(7, -3) - 63.5f) < 1e-6f && surface(2, 4) < -1e29f, "agua do lago em (7,-3) = 63.5; terra seca sem agua");

		// Far away: our bookkeeping forgets the old columns, but nothing is sent to Minecraft (an empty
		// payload would leave them "known and empty" and the hold would drop the player when coming back).
		Sleep(1100);
		collision::Begin(w3x + 300.0, w3y, w3z);
		for (int idle = 0; idle < 50; ++idle)
		{
			Sleep(20);
			store.Drain(mc);
		}
		Check(store.fill.count({ 2, 63, 4 }) == 1 && store.IsKnown(2, 63, 4), "longe: a evicao nao manda regioes vazias (o MC mantem o chao antigo)");

		// World change: new epoch, Minecraft drops everything.
		g_stubWorldEpoch = 2;
		collision::Begin(w3x, w3y, w3z);
		for (int idle = 0; idle < 20; ++idle)
		{
			Sleep(10);
			store.Drain(mc);
		}
		Check(store.epoch == 2 && store.known.empty(), "troca de mundo: kColClear com a epoca 2 e o MC esquece tudo");

		collision::FlushCapture();
		ReplayTotals t;
		const int    problems = Replay(capture.c_str(), t);
		PrintReplay(t);
		Check(problems == 0 && t.records > 1000 && t.complete > 20 && t.segments >= 1, "captura -> --replay: colunas lidas, malha refeita sem problemas");
	}

	std::printf("coltest: %s (%d falha(s))\n", g_failures ? "FAIL" : "PASS", g_failures);
	UnmapViewOfFile(mc);
	CloseHandle(mapping);
	return g_failures ? 1 : 0;
}

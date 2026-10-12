// Stage A collision export: samples -> triangles and voxels. See collision_mesh.h.
// The voxel packing and the region message layout follow SkyCraft skse/src/Collision.cpp
// (Voxelize / SendTriangles), Copyright (c) 2026 chasmlol, MIT License. See THIRD-PARTY-NOTICES.md.
#include "collision_mesh.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace mw3::colmesh
{
	namespace
	{
		constexpr float kNegInf = -std::numeric_limits<float>::infinity();
		constexpr float kSameSurface = 0.02f;  // hits closer than this are one surface
		constexpr float kOpenEps = 0.01f;      // a bottom this close to the scan floor is "open"
		constexpr int   kVoxPerSample = 8 / kSamplesPerBlock;
		constexpr int   kGrid = kRegionSize * 8;  // voxels per region edge (64)
		constexpr float kTriMargin = 0.5f;        // as SkyCraft: tris go to every region they come within 0.5 of
		constexpr int   kCorners = kRegionSamples + 1;

		static_assert(8 % kSamplesPerBlock == 0, "sample step must divide the 1/8 voxel grid");
		static_assert(kRegionSize * 8 == 64, "voxel rows are 64-bit masks");

		constexpr float kPlaneTol = 0.004f;  // undersides: corners this close to a plane are on it
		// Tops: cells whose corners and hit lie this close to one plane merge into one rectangle
		// (cobbles, gentle terrain). Well inside the 0.06 m a top may be off its traced hit.
		constexpr float kMergeTol = 0.025f;
		constexpr float kCentreTol = 0.01f;  // a 2-triangle quad is used when it passes this close to the hit

		bool Open(const Sample& s, std::uint32_t n)
		{
			return s.bottom[n] <= s.lo + kOpenEps;
		}

		// Top of `s` closest to `want`, NaN if none within maxDist.
		float Closest(const Sample* s, float want, float maxDist)
		{
			float best = std::numeric_limits<float>::quiet_NaN();
			if (!s)
				return best;
			float bestD = maxDist;
			for (std::uint32_t n = 0; n < s->count; ++n)
			{
				const float d = std::fabs(s->top[n] - want);
				if (d <= bestD)
				{
					bestD = d;
					best = s->top[n];
				}
			}
			return best;
		}

		// Are top am of cell a and top bn of cell b one continuous surface? Small differences always;
		// up to kSmooth only across an edge and only on a ramp: the same rise continues one cell
		// further on at least one side. A step between two flats (curb, stair, raised floor) does not.
		// Depends only on a, b and the cells beyond them, so both corners of an edge (and both region
		// columns sharing it) decide the same way.
		bool Linked(const ColumnInput& in, int ai, int ak, std::uint32_t am, int bi, int bk, std::uint32_t bn)
		{
			const Sample* a = in.At(ai, ak);
			const Sample* b = in.At(bi, bk);
			const float   ta = a->top[am], tb = b->top[bn];
			const float   rise = tb - ta;
			const float   dz = std::fabs(rise);
			if (dz <= kJoin)
				return true;
			if (dz > kSmooth || std::abs(bi - ai) + std::abs(bk - ak) != 1)
				return false;
			const int   di = bi - ai, dk = bk - ak;
			const float tol = 0.25f * dz + 0.02f;
			return !std::isnan(Closest(in.At(ai - di, ak - dk), ta - rise, tol)) || !std::isnan(Closest(in.At(bi + di, bk + dk), tb + rise, tol));
		}

		// ---- corners: which neighbouring tops form one continuous surface ----------------------
		// slot: 0 = cell (ci-1, ck-1), 1 = (ci, ck-1), 2 = (ci-1, ck), 3 = (ci, ck).
		struct Corner
		{
			std::int8_t cluster[4][kMaxIntervals];
			float       height[4 * kMaxIntervals];
		};

		void BuildCorner(const ColumnInput& in, int ci, int ck, Corner& out)
		{
			constexpr int kNodes = 4 * kMaxIntervals;
			const Sample* cell[4];
			for (int slot = 0; slot < 4; ++slot)
			{
				for (auto& c : out.cluster[slot])
					c = -1;
				cell[slot] = in.At(ci - 1 + (slot & 1), ck - 1 + (slot >> 1));
			}
			struct Link
			{
				float dz;
				int   a, b;  // node = slot * kMaxIntervals + m
			};
			Link      links[6 * kMaxIntervals * kMaxIntervals];
			int       nl = 0;
			const int pairs[6][2] = { { 0, 1 }, { 2, 3 }, { 0, 2 }, { 1, 3 }, { 0, 3 }, { 1, 2 } };
			for (const auto& pr : pairs)
			{
				const Sample* a = cell[pr[0]];
				const Sample* b = cell[pr[1]];
				if (!a || !b)
					continue;
				const int ai = ci - 1 + (pr[0] & 1), ak = ck - 1 + (pr[0] >> 1);
				const int bi = ci - 1 + (pr[1] & 1), bk = ck - 1 + (pr[1] >> 1);
				for (std::uint32_t m = 0; m < a->count; ++m)
					for (std::uint32_t n = 0; n < b->count; ++n)
						if (Linked(in, ai, ak, m, bi, bk, n))
							links[nl++] = { std::fabs(a->top[m] - b->top[n]), pr[0] * kMaxIntervals + static_cast<int>(m), pr[1] * kMaxIntervals + static_cast<int>(n) };
			}
			// Kruskal by height difference; a surface never takes two tops of the same cell.
			std::stable_sort(links, links + nl, [](const Link& x, const Link& y) { return x.dz < y.dz; });
			int      parent[kNodes];
			unsigned mask[kNodes];
			for (int v = 0; v < kNodes; ++v)
			{
				parent[v] = v;
				mask[v] = 1u << (v / kMaxIntervals);
			}
			auto find = [&](int v) {
				while (parent[v] != v)
					v = parent[v] = parent[parent[v]];
				return v;
			};
			for (int l = 0; l < nl; ++l)
			{
				const int ra = find(links[l].a), rb = find(links[l].b);
				if (ra == rb || (mask[ra] & mask[rb]))
					continue;
				parent[rb] = ra;
				mask[ra] |= mask[rb];
			}
			float sum[kNodes] = {};
			int   size[kNodes] = {};
			int   id[kNodes];
			for (int v = 0; v < kNodes; ++v)
				id[v] = -1;
			int clusters = 0;
			for (int slot = 0; slot < 4; ++slot)
			{
				if (!cell[slot])
					continue;
				for (std::uint32_t m = 0; m < cell[slot]->count; ++m)
				{
					const int r = find(slot * kMaxIntervals + static_cast<int>(m));
					if (id[r] < 0)
						id[r] = clusters++;
					out.cluster[slot][m] = static_cast<std::int8_t>(id[r]);
					sum[id[r]] += cell[slot]->top[m];
					++size[id[r]];
				}
			}
			for (int c = 0; c < clusters; ++c)
				out.height[c] = sum[c] / static_cast<float>(size[c]);
		}

		struct Corners
		{
			std::vector<Corner> c;
			explicit Corners(const ColumnInput& in) : c(static_cast<std::size_t>(kCorners * kCorners))
			{
				for (int ck = 0; ck < kCorners; ++ck)
					for (int ci = 0; ci < kCorners; ++ci)
						BuildCorner(in, ci, ck, c[static_cast<std::size_t>(ck * kCorners + ci)]);
			}
			[[nodiscard]] const Corner& At(int ci, int ck) const { return c[static_cast<std::size_t>(ck * kCorners + ci)]; }
		};

		// Slot of cell (di, dk) at corner (ci, ck).
		int SlotOf(int di, int dk, int ci, int ck)
		{
			return (di - ci + 1) + 2 * (dk - ck + 1);
		}

		float VertexHeight(const Corners& corners, int ci, int ck, int di, int dk, std::uint32_t m)
		{
			const Corner& c = corners.At(ci, ck);
			const int     id = c.cluster[SlotOf(di, dk, ci, ck)][m];
			return id >= 0 ? c.height[id] : 0.0f;
		}

		bool SameSurface(const Corners& corners, int ci, int ck, int adi, int adk, std::uint32_t am, int bdi, int bdk, std::uint32_t bn)
		{
			const Corner& c = corners.At(ci, ck);
			const int     a = c.cluster[SlotOf(adi, adk, ci, ck)][am];
			return a >= 0 && a == c.cluster[SlotOf(bdi, bdk, ci, ck)][bn];
		}

		// ---- triangle output ------------------------------------------------------------------
		struct P
		{
			float x, y, z;
		};

		// face: 1 = normal up, -1 = normal down, 0 = as given (walls; Minecraft only trusts |ny|).
		void Emit(std::vector<Tri>& out, const P& a, const P& b, const P& c, int face)
		{
			const float ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z;
			const float wx = c.x - a.x, wy = c.y - a.y, wz = c.z - a.z;
			const float nx = uy * wz - uz * wy, ny = uz * wx - ux * wz, nz = ux * wy - uy * wx;
			if (nx * nx + ny * ny + nz * nz < 1e-10f)
				return;  // degenerate
			if ((face > 0 && ny < 0.0f) || (face < 0 && ny > 0.0f))
				out.push_back({ { a.x, a.y, a.z, c.x, c.y, c.z, b.x, b.y, b.z } });
			else
				out.push_back({ { a.x, a.y, a.z, b.x, b.y, b.z, c.x, c.y, c.z } });
		}

		// Quad with corner heights h00 (x0,z0), h10 (x1,z0), h01 (x0,z1), h11 (x1,z1), split along
		// the diagonal 00-11 (diag0) or 10-01. faceUp: normal up (top) or down (underside).
		void Quad(std::vector<Tri>& out, float x0, float x1, float z0, float z1, float h00, float h10, float h01, float h11, bool faceUp, bool diag0)
		{
			const P   p00{ x0, h00, z0 }, p10{ x1, h10, z0 }, p01{ x0, h01, z1 }, p11{ x1, h11, z1 };
			const int face = faceUp ? 1 : -1;
			if (diag0)
			{
				Emit(out, p00, p11, p10, face);
				Emit(out, p00, p01, p11, face);
			}
			else
			{
				Emit(out, p00, p01, p10, face);
				Emit(out, p10, p01, p11, face);
			}
		}

		// A cell's top: through the 4 corners and exactly through the hit at the centre. Two triangles
		// when one diagonal already passes the centre within kCentreTol, else a 4-triangle fan.
		void TopFace(std::vector<Tri>& out, float x0, float x1, float z0, float z1, float h00, float h10, float h01, float h11, float centre)
		{
			const float d0 = std::fabs(0.5f * (h00 + h11) - centre);
			const float d1 = std::fabs(0.5f * (h10 + h01) - centre);
			if (std::min(d0, d1) <= kCentreTol)
			{
				Quad(out, x0, x1, z0, z1, h00, h10, h01, h11, true, d0 <= d1);
				return;
			}
			const P c{ 0.5f * (x0 + x1), centre, 0.5f * (z0 + z1) };
			const P p00{ x0, h00, z0 }, p10{ x1, h10, z0 }, p01{ x0, h01, z1 }, p11{ x1, h11, z1 };
			Emit(out, c, p00, p10, 1);
			Emit(out, c, p10, p11, 1);
			Emit(out, c, p11, p01, 1);
			Emit(out, c, p01, p00, 1);
		}

		// ---- greedy merge of coplanar faces ----------------------------------------------------
		// A planar cell face (corner heights). Cells on one plane merge into rectangles.
		struct Face
		{
			bool  valid = false;
			float h00 = 0, h10 = 0, h01 = 0, h11 = 0;
			float centre = 0;  // the traced hit at the cell centre
		};

		// Rectangles of cells whose faces lie on one plane, row-major greedy. faces[(dk * R + di) * M + m].
		template <class EmitRect>
		void MergeFaces(std::vector<Face>& faces, float tol, EmitRect&& emitRect)
		{
			constexpr int R = kRegionSamples;
			constexpr int M = kMaxIntervals;
			auto at = [&](int di, int dk, int m) -> Face& { return faces[static_cast<std::size_t>((dk * R + di) * M + m)]; };
			// The face in cell (di, dk) on the seed's plane (rise gx, gz per cell), or -1.
			auto match = [&](int di, int dk, float seed00, int sdi, int sdk, float gx, float gz) -> int {
				const float base = seed00 + gx * static_cast<float>(di - sdi) + gz * static_cast<float>(dk - sdk);
				for (int m = 0; m < M; ++m)
				{
					const Face& f = at(di, dk, m);
					if (f.valid && std::fabs(f.h00 - base) <= tol && std::fabs(f.h10 - (base + gx)) <= tol && std::fabs(f.h01 - (base + gz)) <= tol &&
					    std::fabs(f.h11 - (base + gx + gz)) <= tol && std::fabs(f.centre - (base + 0.5f * (gx + gz))) <= tol)
						return m;
				}
				return -1;
			};
			for (int dk = 0; dk < R; ++dk)
				for (int di = 0; di < R; ++di)
					for (int m = 0; m < M; ++m)
					{
						const Face seed = at(di, dk, m);
						if (!seed.valid)
							continue;
						const float gx = 0.5f * ((seed.h10 - seed.h00) + (seed.h11 - seed.h01));
						const float gz = 0.5f * ((seed.h01 - seed.h00) + (seed.h11 - seed.h10));
						at(di, dk, m).valid = false;
						int w = 1;
						while (di + w < R)
						{
							const int mm = match(di + w, dk, seed.h00, di, dk, gx, gz);
							if (mm < 0)
								break;
							at(di + w, dk, mm).valid = false;
							++w;
						}
						int h = 1;
						while (dk + h < R)
						{
							int  found[R];
							bool row = true;
							for (int x = 0; x < w && row; ++x)
							{
								found[x] = match(di + x, dk + h, seed.h00, di, dk, gx, gz);
								row = found[x] >= 0;
							}
							if (!row)
								break;
							for (int x = 0; x < w; ++x)
								at(di + x, dk + h, found[x]).valid = false;
							++h;
						}
						const float fw = static_cast<float>(w), fh = static_cast<float>(h);
						emitRect(di, dk, w, h, seed, seed.h00, seed.h00 + gx * fw, seed.h00 + gz * fh, seed.h00 + gx * fw + gz * fh);
					}
		}

		// ---- walls ---------------------------------------------------------------------------
		// A vertical quad on the line x = line (axis 0) or z = line (axis 1), from u0 to u1 along the
		// other axis, bottom b0..b1 and top t0..t1 at u0..u1.
		struct Wall
		{
			int   axis;
			float line, u0, u1, b0, b1, t0, t1;
		};

		void EmitWall(std::vector<Tri>& out, const Wall& w)
		{
			auto    pt = [&](float u, float y) { return w.axis == 0 ? P{ w.line, y, u } : P{ u, y, w.line }; };
			const P b1 = pt(w.u0, w.b0), b2 = pt(w.u1, w.b1), t2 = pt(w.u1, w.t1), t1 = pt(w.u0, w.t0);
			if (w.t1 - w.b1 > 1e-4f)
				Emit(out, b1, b2, t2, 0);
			if (w.t0 - w.b0 > 1e-4f)
				Emit(out, b1, t2, t1, 0);
		}

		// Collinear walls with the same flat bottom and top that touch become one; exact duplicates
		// (both sides of an edge emitting the same span) collapse.
		void MergeWalls(std::vector<Wall>& walls, std::vector<Tri>& out)
		{
			auto              rect = [](const Wall& w) { return std::fabs(w.b0 - w.b1) < 1e-4f && std::fabs(w.t0 - w.t1) < 1e-4f; };
			std::vector<Wall> merged;
			std::vector<Wall> flat;
			merged.reserve(walls.size());
			flat.reserve(walls.size());
			for (const Wall& w : walls)
				(rect(w) ? flat : merged).push_back(w);
			std::sort(flat.begin(), flat.end(), [](const Wall& a, const Wall& b) {
				if (a.axis != b.axis)
					return a.axis < b.axis;
				if (a.line != b.line)
					return a.line < b.line;
				if (a.b0 != b.b0)
					return a.b0 < b.b0;
				if (a.t0 != b.t0)
					return a.t0 < b.t0;
				return a.u0 < b.u0;
			});
			for (std::size_t n = 0; n < flat.size();)
			{
				Wall cur = flat[n++];
				while (n < flat.size() && flat[n].axis == cur.axis && flat[n].line == cur.line && std::fabs(flat[n].b0 - cur.b0) < 1e-4f &&
				       std::fabs(flat[n].t0 - cur.t0) < 1e-4f && flat[n].u0 <= cur.u1 + 1e-4f)
				{
					cur.u1 = std::max(cur.u1, flat[n].u1);
					++n;
				}
				merged.push_back(cur);
			}
			for (const Wall& w : merged)
				EmitWall(out, w);
		}

		struct Span
		{
			float lo, hi;
		};

		// [lo, hi] minus the union of covers.
		int Subtract(Span base, const Span* covers, int coverCount, Span* out, int maxOut)
		{
			Span parts[2 * kMaxIntervals + 2];
			int  n = 0;
			parts[n++] = base;
			for (int c = 0; c < coverCount; ++c)
			{
				Span next[2 * kMaxIntervals + 2];
				int  m = 0;
				for (int p = 0; p < n; ++p)
				{
					const Span s = parts[p];
					const Span v = covers[c];
					if (v.hi <= s.lo || v.lo >= s.hi)
					{
						if (m < 2 * kMaxIntervals + 2)
							next[m++] = s;
						continue;
					}
					if (v.lo > s.lo && m < 2 * kMaxIntervals + 2)
						next[m++] = { s.lo, v.lo };
					if (v.hi < s.hi && m < 2 * kMaxIntervals + 2)
						next[m++] = { v.hi, s.hi };
				}
				std::copy(next, next + m, parts);
				n = m;
			}
			int written = 0;
			for (int p = 0; p < n && written < maxOut; ++p)
				if (parts[p].hi - parts[p].lo > kSameSurface)
					out[written++] = parts[p];
			return written;
		}

		void WriteHeader(std::vector<std::uint8_t>& out, int rx, int ry, int rz, std::uint32_t epoch, std::uint32_t count, std::size_t elementBytes)
		{
			proto::ColRegion header{};
			header.minX = rx * kRegionSize;
			header.minY = ry * kRegionSize;
			header.minZ = rz * kRegionSize;
			header.maxX = header.minX + kRegionSize - 1;
			header.maxY = header.minY + kRegionSize - 1;
			header.maxZ = header.minZ + kRegionSize - 1;
			header.epoch = epoch;
			header.count = count;
			out.resize(sizeof(header) + count * elementBytes);
			std::memcpy(out.data(), &header, sizeof(header));
		}
	} // namespace

	void BuildIntervals(Sample& sample, const float* down, int downCount, const float* up, int upCount, float water)
	{
		float tops[32];
		float bots[32];
		int   nt = 0, nb = 0;
		for (int j = 0; j < downCount && nt < 32; ++j)
			if (std::isfinite(down[j]) && down[j] >= sample.lo - kOpenEps && down[j] <= sample.hi + kOpenEps)
				tops[nt++] = down[j];
		for (int j = 0; j < upCount && nb < 32; ++j)
			if (std::isfinite(up[j]) && up[j] >= sample.lo - kOpenEps && up[j] <= sample.hi + kOpenEps)
				bots[nb++] = up[j];
		std::sort(tops, tops + nt, [](float a, float b) { return a > b; });
		std::sort(bots, bots + nb);
		nt = static_cast<int>(std::unique(tops, tops + nt, [](float a, float b) { return std::fabs(a - b) < kSameSurface; }) - tops);
		nb = static_cast<int>(std::unique(bots, bots + nb, [](float a, float b) { return std::fabs(a - b) < kSameSurface; }) - bots);

		// Highest underside strictly between `below` and `above`, or NaN.
		auto underside = [&](float above, float below) {
			float best = std::numeric_limits<float>::quiet_NaN();
			for (int b = 0; b < nb; ++b)
				if (bots[b] < above - kSameSurface * 0.5f && bots[b] > below + kSameSurface * 0.5f)
					best = bots[b];  // ascending: the last match is the highest
			return best;
		};

		sample.count = 0;
		int j = 0;
		while (j < nt && sample.count < kMaxIntervals)
		{
			const float curTop = tops[j];
			const bool  last = sample.count + 1 == kMaxIntervals;
			// Walk down through tops with no underside between them (one solid, e.g. a rock on the
			// ground seen from inside), until an underside closes this interval.
			float bottom = sample.lo;
			int   k = j;
			for (; k < nt; ++k)
			{
				const float below = k + 1 < nt ? tops[k + 1] : kNegInf;
				const float b = underside(tops[k], below);
				if (!std::isnan(b) && !last)
				{
					bottom = b;
					break;
				}
				if (!std::isnan(b) && last && k + 1 >= nt)
				{
					bottom = b;
					break;
				}
			}
			bottom = std::max(bottom, sample.lo);
			if (curTop - bottom < kSameSurface)
				bottom = curTop - kSameSurface;
			sample.top[sample.count] = curTop;
			sample.bottom[sample.count] = bottom;
			++sample.count;
			j = k + 1;
		}

		// A gap the player can't use (thinner than kMinGap) is solid: one interval.
		for (std::uint32_t n = 0; n + 1 < sample.count;)
		{
			if (sample.bottom[n] - sample.top[n + 1] < kMinGap)
			{
				sample.bottom[n] = sample.bottom[n + 1];
				for (std::uint32_t r = n + 1; r + 1 < sample.count; ++r)
				{
					sample.top[r] = sample.top[r + 1];
					sample.bottom[r] = sample.bottom[r + 1];
				}
				--sample.count;
			}
			else
				++n;
		}
		// Nothing traced under the lowest interval: its bottom is buried (in the terrain the second
		// ray down started under), not an underside over open air. Solid down to the scan floor.
		if (sample.count > 0)
			sample.bottom[sample.count - 1] = sample.lo;

		// Water only counts in open space, above the ground it sits on (a river under a bridge
		// counts; the sea level under dry land doesn't).
		sample.water = kNoWaterLevel;
		if (std::isfinite(water) && water > -1.0e6f && water < sample.hi + 64.0f)
		{
			bool  insideSolid = false;
			float ground = kNegInf;
			for (std::uint32_t n = 0; n < sample.count; ++n)
			{
				if (water < sample.top[n] - 0.05f && water > sample.bottom[n])
					insideSolid = true;
				if (ground == kNegInf && sample.top[n] <= water + 0.05f)
					ground = sample.top[n];
			}
			if (!insideSolid && water - ground > 0.05f)
				sample.water = water;
		}
	}

	void BuildTriangles(const ColumnInput& in, std::vector<Tri>& out)
	{
		out.clear();
		const Corners corners(in);
		const float   ox = static_cast<float>(in.rx * kRegionSize);
		const float   oz = static_cast<float>(in.rz * kRegionSize);
		constexpr int R = kRegionSamples;
		constexpr int M = kMaxIntervals;

		struct Edge
		{
			int ndi, ndk;            // neighbour offset
			int c1i, c1k, c2i, c2k;  // edge corners relative to the cell
			int axis;                // 0: the edge lies on x = const, 1: on z = const
		};
		static constexpr Edge kEdges[4] = {
			{ -1, 0, 0, 0, 0, 1, 0 },  // -x
			{ 1, 0, 1, 0, 1, 1, 0 },   // +x
			{ 0, -1, 0, 0, 1, 0, 1 },  // -z
			{ 0, 1, 0, 1, 1, 1, 1 },   // +z
		};

		std::vector<Face> planarTops(static_cast<std::size_t>(R * R * M));
		std::vector<Face> undersides(static_cast<std::size_t>(R * R * M));
		std::vector<Wall> walls;
		for (int dk = 0; dk < R; ++dk)
		{
			for (int di = 0; di < R; ++di)
			{
				const Sample* a = in.At(di, dk);
				if (!a)
					continue;
				const float x0 = ox + static_cast<float>(di) * kSampleStep, x1 = x0 + kSampleStep;
				const float z0 = oz + static_cast<float>(dk) * kSampleStep, z1 = z0 + kSampleStep;

				for (std::uint32_t m = 0; m < a->count; ++m)
				{
					const float tA = a->top[m];
					// Top face: smooth over the corners shared with continuous neighbours, exact at the hit.
					const float       h00 = VertexHeight(corners, di, dk, di, dk, m);
					const float       h10 = VertexHeight(corners, di + 1, dk, di, dk, m);
					const float       h01 = VertexHeight(corners, di, dk + 1, di, dk, m);
					const float       h11 = VertexHeight(corners, di + 1, dk + 1, di, dk, m);
					const std::size_t slot = static_cast<std::size_t>((dk * R + di) * M + static_cast<int>(m));
					if (std::fabs(h00 + h11 - h10 - h01) <= kMergeTol && std::fabs(0.25f * (h00 + h10 + h01 + h11) - tA) <= kMergeTol)
						planarTops[slot] = { true, h00, h10, h01, h11, tA };  // merged below
					else
						TopFace(out, x0, x1, z0, z1, h00, h10, h01, h11, tA);

					// Underside (ceilings, bridges, overhangs), merged below.
					if (!Open(*a, m))
						undersides[slot] = { true, a->bottom[m], a->bottom[m], a->bottom[m], a->bottom[m], a->bottom[m] };

					// Walls: where this interval is solid and the neighbour isn't.
					for (const Edge& e : kEdges)
					{
						const int     bdi = di + e.ndi, bdk = dk + e.ndk;
						const Sample* b = in.At(bdi, bdk);
						if (!b)
							continue;  // unknown: no wall into the unscanned
						const int c1i = di + e.c1i, c1k = dk + e.c1k, c2i = di + e.c2i, c2k = dk + e.c2k;
						Span      covers[kMaxIntervals];
						bool      connected[kMaxIntervals];
						for (std::uint32_t n = 0; n < b->count; ++n)
						{
							connected[n] = SameSurface(corners, c1i, c1k, di, dk, m, bdi, bdk, n) && SameSurface(corners, c2i, c2k, di, dk, m, bdi, bdk, n);
							covers[n] = { Open(*b, n) ? kNegInf : b->bottom[n], connected[n] ? std::max(b->top[n], tA) : b->top[n] };
						}
						Span        segments[2 * kMaxIntervals + 2];
						const int   segCount = Subtract({ Open(*a, m) ? a->lo : a->bottom[m], tA }, covers, static_cast<int>(b->count), segments, 2 * kMaxIntervals + 2);
						const float ex1 = ox + static_cast<float>(c1i) * kSampleStep, ez1 = oz + static_cast<float>(c1k) * kSampleStep;
						const float ex2 = ox + static_cast<float>(c2i) * kSampleStep, ez2 = oz + static_cast<float>(c2k) * kSampleStep;
						for (int sgi = 0; sgi < segCount; ++sgi)
						{
							const Span seg = segments[sgi];
							float      top1 = seg.hi, top2 = seg.hi;
							if (seg.hi >= tA - 1e-4f)
							{
								top1 = VertexHeight(corners, c1i, c1k, di, dk, m);
								top2 = VertexHeight(corners, c2i, c2k, di, dk, m);
							}
							float bot1 = seg.lo, bot2 = seg.lo;
							for (std::uint32_t n = 0; n < b->count; ++n)
							{
								if (!connected[n] && std::fabs(b->top[n] - seg.lo) < 1e-4f)
								{
									bot1 = VertexHeight(corners, c1i, c1k, bdi, bdk, n);
									bot2 = VertexHeight(corners, c2i, c2k, bdi, bdk, n);
									break;
								}
							}
							bot1 = std::min(bot1, top1);
							bot2 = std::min(bot2, top2);
							if (top1 - bot1 <= 1e-4f && top2 - bot2 <= 1e-4f)
								continue;
							if (e.axis == 0)
								walls.push_back({ 0, ex1, ez1, ez2, bot1, bot2, top1, top2 });
							else
								walls.push_back({ 1, ez1, ex1, ex2, bot1, bot2, top1, top2 });
						}
					}
				}
			}
		}

		MergeFaces(planarTops, kMergeTol, [&](int di, int dk, int w, int h, const Face& seed, float h00, float h10, float h01, float h11) {
			const float x0 = ox + static_cast<float>(di) * kSampleStep, z0 = oz + static_cast<float>(dk) * kSampleStep;
			if (w == 1 && h == 1)  // nothing to merge with: the cell's own exact face
				TopFace(out, x0, x0 + kSampleStep, z0, z0 + kSampleStep, seed.h00, seed.h10, seed.h01, seed.h11, seed.centre);
			else
				Quad(out, x0, x0 + static_cast<float>(w) * kSampleStep, z0, z0 + static_cast<float>(h) * kSampleStep, h00, h10, h01, h11, true, true);
		});
		MergeFaces(undersides, kPlaneTol, [&](int di, int dk, int w, int h, const Face&, float h00, float h10, float h01, float h11) {
			const float x0 = ox + static_cast<float>(di) * kSampleStep, z0 = oz + static_cast<float>(dk) * kSampleStep;
			Quad(out, x0, x0 + static_cast<float>(w) * kSampleStep, z0, z0 + static_cast<float>(h) * kSampleStep, h00, h10, h01, h11, false, true);
		});
		MergeWalls(walls, out);
	}

	void BuildTrisPayload(const std::vector<Tri>& tris, int rx, int ry, int rz, std::uint32_t epoch, std::vector<std::uint8_t>& out)
	{
		const float lo = static_cast<float>(ry * kRegionSize) - kTriMargin;
		const float hi = static_cast<float>((ry + 1) * kRegionSize) + kTriMargin;
		std::vector<proto::ColTri> kept;
		kept.reserve(tris.size());
		for (const Tri& t : tris)
		{
			const float mn = std::min({ t.v[1], t.v[4], t.v[7] });
			const float mx = std::max({ t.v[1], t.v[4], t.v[7] });
			if (mx < lo || mn > hi)
				continue;
			proto::ColTri c{};
			std::memcpy(c.v, t.v, sizeof(c.v));
			c.flags = 0;  // not diggable, not a stair helper: Stage A doesn't know what it hit
			kept.push_back(c);
		}
		WriteHeader(out, rx, ry, rz, epoch, static_cast<std::uint32_t>(kept.size()), sizeof(proto::ColTri));
		if (!kept.empty())
			std::memcpy(out.data() + sizeof(proto::ColRegion), kept.data(), kept.size() * sizeof(proto::ColTri));
	}

	void BuildRegionPayload(const ColumnInput& in, int ry, std::uint32_t epoch, std::vector<std::uint8_t>& out)
	{
		constexpr int G = kGrid;
		std::vector<std::uint64_t> solid(static_cast<std::size_t>(G * G), 0);  // [y * G + z], bit x
		const int voxY0 = ry * G;

		for (int dk = 0; dk < kRegionSamples; ++dk)
		{
			for (int di = 0; di < kRegionSamples; ++di)
			{
				const Sample* a = in.At(di, dk);
				if (!a)
					continue;
				const Sample* neighbours[4] = { in.At(di - 1, dk), in.At(di + 1, dk), in.At(di, dk - 1), in.At(di, dk + 1) };
				for (std::uint32_t m = 0; m < a->count; ++m)
				{
					const float tA = a->top[m];
					const float floorA = Open(*a, m) ? a->lo : a->bottom[m];
					// A shell under the surface, reaching down to lower neighbours so walls are solid.
					float shellBottom = tA - kShell;
					for (const Sample* b : neighbours)
					{
						if (!b)
							continue;
						for (std::uint32_t n = 0; n < b->count; ++n)
						{
							if (b->top[n] < tA - kSmooth)
							{
								shellBottom = std::min(shellBottom, b->top[n]);
								break;  // descending: the first one below is the neighbour's ground
							}
						}
					}
					shellBottom = std::max(shellBottom, floorA);
					// Round the top up (never sink into the ground), the bottom down.
					const int v0 = std::max(0, static_cast<int>(std::floor(shellBottom * 8.0f)) - voxY0);
					const int v1 = std::min(G, static_cast<int>(std::ceil(tA * 8.0f - 1e-3f)) - voxY0);
					if (v1 <= v0)
						continue;
					const std::uint64_t xmask = ((1ull << kVoxPerSample) - 1) << (di * kVoxPerSample);
					for (int y = v0; y < v1; ++y)
						for (int z = dk * kVoxPerSample; z < (dk + 1) * kVoxPerSample; ++z)
							solid[static_cast<std::size_t>(y * G + z)] |= xmask;
				}
			}
		}

		std::vector<proto::ColBlock> blocks;
		for (int by = 0; by < kRegionSize; ++by)
		{
			for (int bz = 0; bz < kRegionSize; ++bz)
			{
				for (int bx = 0; bx < kRegionSize; ++bx)
				{
					proto::ColBlock blk{};
					bool            any = false;
					for (int sy = 0; sy < 8; ++sy)
					{
						std::uint64_t layer = 0;
						for (int sz = 0; sz < 8; ++sz)
						{
							const std::uint64_t row = (solid[static_cast<std::size_t>((by * 8 + sy) * G + (bz * 8 + sz))] >> (bx * 8)) & 0xFF;
							layer |= row << (sz * 8);
						}
						blk.bits[sy] = layer;
						any |= layer != 0;
					}
					if (!any)
						continue;
					blk.x = in.rx * kRegionSize + bx;
					blk.y = ry * kRegionSize + by;
					blk.z = in.rz * kRegionSize + bz;
					blocks.push_back(blk);
				}
			}
		}
		WriteHeader(out, in.rx, ry, in.rz, epoch, static_cast<std::uint32_t>(blocks.size()), sizeof(proto::ColBlock));
		if (!blocks.empty())
			std::memcpy(out.data() + sizeof(proto::ColRegion), blocks.data(), blocks.size() * sizeof(proto::ColBlock));
	}

	void BuildEmptyPayload(int rx, int ry, int rz, std::uint32_t epoch, std::vector<std::uint8_t>& out)
	{
		WriteHeader(out, rx, ry, rz, epoch, 0, 0);
	}
} // namespace mw3::colmesh

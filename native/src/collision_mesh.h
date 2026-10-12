// Stage A collision export (research/04 section 2): turns the script's vertical raycast samples
// into the exact messages the Fabric side parses (W3Collision.readTris / readRegion):
//   kColTris   ColRegion header + ColTri[]   -> the player's smooth collider (TriCollider)
//   kColRegion ColRegion header + ColBlock[] -> 1/8-block voxels (mobs, hold-until-ready, "known")
// Region = 8x8x8 blocks (W3Collision.REGION_SIZE). Pure code: no link, no threads (test/coltest.cpp).
// Message layout and the voxel packing follow SkyCraft skse/src/Collision.cpp (MIT), see
// THIRD-PARTY-NOTICES.md.
#pragma once

#include "../../protocol/mw3_protocol.h"

#include <array>
#include <cstdint>
#include <vector>

namespace mw3::colmesh
{
	inline constexpr int   kRegionSize = 8;  // blocks per region edge; must match W3Collision.REGION_SIZE
	inline constexpr int   kSamplesPerBlock = 2;  // sample step 0.5 block (= 0.5 m)
	inline constexpr int   kRegionSamples = kRegionSize * kSamplesPerBlock;  // 16 samples per region edge
	inline constexpr float kSampleStep = 1.0f / kSamplesPerBlock;
	inline constexpr int   kMaxIntervals = 4;  // solid spans kept per sample column
	// Samples of margin around the footprint: a corner's smoothing looks one cell past its 4 cells
	// (is this step part of a ramp?), so a region needs 2 samples of each neighbour.
	inline constexpr int   kMargin = 2;
	inline constexpr int   kInputEdge = kRegionSamples + 2 * kMargin;
	// Neighbouring sample tops closer than this are one continuous surface (smoothed through the
	// shared corners). 0.2 at a 0.5 step = slopes up to ~22 deg, cobbles, small curbs.
	inline constexpr float kJoin = 0.2f;
	// Bigger differences up to this are smoothed only when they are a ramp (the gradient continues
	// on at least one side); a step between two flats becomes a vertical riser + flat tops instead,
	// which Minecraft's step-up (0.6) climbs. 0.55 at a 0.5 step = ramps up to ~48 deg.
	inline constexpr float kSmooth = 0.55f;
	// A gap between a solid's underside and the top below it thinner than this can't hold the
	// player (1.8): the two solids become one (no ceiling face hovering just over a floor).
	inline constexpr float kMinGap = 1.0f;
	// Voxel shell thickness under a surface (blocks). Only the triangles collide with the player;
	// the voxels are for mobs, for Minecraft's "is there ground below" checks, and to tell the
	// player's collider that the feet ended up inside the ground (W3Collider lifts them back up).
	inline constexpr float kShell = 1.0f;
	inline constexpr float kNoWaterLevel = -1.0e30f;

	// One vertical sample column at the centre of cell (i, k): MC x = (i + 0.5) * step, z = (k + 0.5) * step.
	struct Sample
	{
		std::int32_t                      i = 0, k = 0;
		float                             lo = 0.0f, hi = 0.0f;  // MC y range that was scanned
		std::uint32_t                     count = 0;             // solid intervals
		std::array<float, kMaxIntervals> top{};     // sorted descending
		std::array<float, kMaxIntervals> bottom{};  // bottom[n] <= lo: open (solid below the scan)
		float                             water = kNoWaterLevel;  // MC y of the water surface
	};

	// Builds a sample's solid intervals (and validates its water) from the raw trace hits, MC y:
	// down[] = surfaces hit going down (tops), up[] = surfaces hit going up (undersides).
	// StaticTrace culls back faces, so a ray started inside a mesh (the second ray down, 5 cm under
	// a floor) misses whatever is above its start: a Static floor 4 cm over the terrain hides the
	// terrain, and the floor's buried bottom is then the only thing traced below it. So:
	//  - the lowest interval is never closed (nothing traced under it = buried, solid all the way);
	//  - gaps thinner than kMinGap are filled.
	void BuildIntervals(Sample& sample, const float* down, int downCount, const float* up, int upCount, float water);

	// One region column (rx, rz) to build: the 16x16 footprint samples plus kMargin samples of margin.
	struct ColumnInput
	{
		int                                              rx = 0, rz = 0;
		std::array<const Sample*, kInputEdge * kInputEdge> cells{};  // nullptr: unknown

		// di, dk in [-kMargin, kRegionSamples + kMargin): sample (rx*16 + di, rz*16 + dk).
		[[nodiscard]] const Sample* At(int di, int dk) const
		{
			if (di < -kMargin || dk < -kMargin || di >= kRegionSamples + kMargin || dk >= kRegionSamples + kMargin)
				return nullptr;
			return cells[static_cast<std::size_t>((dk + kMargin) * kInputEdge + (di + kMargin))];
		}
		static constexpr std::size_t Index(int di, int dk)
		{
			return static_cast<std::size_t>((dk + kMargin) * kInputEdge + (di + kMargin));
		}
	};

	struct Tri
	{
		float v[9];
	};

	// Every triangle of the column (top faces, walls, ceilings), MC space. Each sample's top passes
	// exactly through its traced hit (cell centre); coplanar cells and collinear walls are merged.
	void BuildTriangles(const ColumnInput& input, std::vector<Tri>& out);

	// kColTris payload for region (rx, ry, rz): the column's triangles that reach into it (+0.5 in y).
	void BuildTrisPayload(const std::vector<Tri>& tris, int rx, int ry, int rz, std::uint32_t epoch, std::vector<std::uint8_t>& out);
	// kColRegion payload for region (rx, ry, rz) from the column's intervals.
	void BuildRegionPayload(const ColumnInput& input, int ry, std::uint32_t epoch, std::vector<std::uint8_t>& out);
	// A region with nothing in it (count 0). Valid for both kColTris and kColRegion. Minecraft counts
	// it as known and empty, so it is never sent for eviction (EvictFar only drops our bookkeeping).
	void BuildEmptyPayload(int rx, int ry, int rz, std::uint32_t epoch, std::vector<std::uint8_t>& out);

	// floor(v / d) for d > 0.
	constexpr int FloorDiv(int v, int d) noexcept
	{
		return v >= 0 ? v / d : -((-v + d - 1) / d);
	}
} // namespace mw3::colmesh

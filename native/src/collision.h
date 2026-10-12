// Stage A collision export, game side (research/04 section 2.5). The WitcherScript side
// (mod/.../mw3_collision.ws) does the raycasts; this decides which columns to trace (nearest the
// player first, so Minecraft's holdUntilReady lets go quickly), keeps the results, and a worker
// thread turns finished 8x8-block region columns into collision ring messages (collision_mesh.h).
// Also fills the WaterGrid from the sampled water levels.
//
// All functions run on the script thread (the natives) unless noted. Inert while Minecraft isn't
// linked: nothing is traced, nothing is written.
#pragma once

#include <cstdint>

namespace mw3::collision
{
	// Start of the scan for this frame. Player feet in W3 space. False: don't trace this frame
	// (Minecraft not linked, ...). Also hands finished columns to the worker and writes the water grid.
	bool Begin(double w3x, double w3y, double w3z);

	// Picks the next sample column to trace. 0: nothing more this frame. 1: a column is ready
	// (X/Y/Top/Bottom). 2: same, and its water level is wanted too.
	int Next();
	// The pending column, W3 space: trace straight down from (x, y, top) to (x, y, bottom) and up.
	float PendingX();
	float PendingY();
	float PendingTop();
	float PendingBottom();
	// Raw hits of the pending column (W3 z): surfaces hit going down / undersides hit going up.
	void HitDown(float w3z);
	void HitUp(float w3z);
	// Stores the pending column. water: W3 z of the water surface, or < -1e6 for none.
	void Commit(float w3Water);

	// Traces per frame the script may spend (mw3.ini [Collision] iTracesPerFrame, default 200).
	int TraceBudget();

	// Writes what is still buffered of the capture (below). Also happens on its own every 2 s.
	void FlushCapture();

	// ---- capture: mw3.ini [Collision] bCapture=1 (default 0) -----------------------------------
	// Every committed column is appended to mw3_colcapture.bin next to witcher3.exe, up to
	// kCaptureMaxBytes (then it stops and logs once). `mw3_coltest.exe --replay <file>` rebuilds the
	// meshes from it and reports the top-height error and stuck checks. Little endian:
	//   header : char magic[8] = "MW3COLC1"; uint32 version (1); uint32 headerBytes (16)
	//   records: uint8 type, then
	//     type 1, new epoch (first connection / Minecraft reconnect / world change; a later session
	//             appends after the last one): uint8 0; uint16 0; uint32 epoch; uint32 worldId    12 bytes
	//     type 2, one column: uint8 nDown; uint8 nUp; uint8 0;
	//             int32 i, k (sample index: MC x = (i + 0.5) * 0.5, MC z = (k + 0.5) * 0.5);
	//             float w3x, w3y; float lo, hi (scanned W3 z = MC y range); float water (W3 z, NaN = none);
	//             float down[nDown] (W3 z of the surfaces hit going down, in trace order);
	//             float up[nUp] (W3 z of the undersides hit going up)                    32 + 4 * (nDown + nUp)
	inline constexpr std::uint32_t kCaptureVersion = 1;
	inline constexpr std::uint8_t  kCaptureEpoch = 1;
	inline constexpr std::uint8_t  kCaptureColumn = 2;
	inline constexpr std::uint64_t kCaptureMaxBytes = 50ull << 20;
} // namespace mw3::collision

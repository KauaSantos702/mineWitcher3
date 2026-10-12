#include "InlineHook.h"

#include <windows.h>

#include <cstring>

namespace witcher_native_bridge
{
	namespace
	{
		constexpr size_t kAbsoluteJumpSize = 14;

		void EmitAbsoluteJump(uint8_t* address, void* destination)
		{
			address[0] = 0xFF;
			address[1] = 0x25;
			address[2] = 0x00;
			address[3] = 0x00;
			address[4] = 0x00;
			address[5] = 0x00;
			std::memcpy(address + 6, &destination, sizeof(destination));
		}
	} // namespace

	bool InlineHook::Install(void* target, void* detour, const std::vector<uint8_t>& expectedPrologue)
	{
		if (installed_)
			return true;

		if (!target || !detour || expectedPrologue.size() < kAbsoluteJumpSize)
		{
			error_ = "invalid arguments";
			return false;
		}

		auto* code = static_cast<uint8_t*>(target);
		if (std::memcmp(code, expectedPrologue.data(), expectedPrologue.size()) != 0)
		{
			error_ = "prologue mismatch";
			return false;
		}

		const size_t stolenBytes = expectedPrologue.size();
		trampoline_ = VirtualAlloc(nullptr, stolenBytes + kAbsoluteJumpSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);

		if (!trampoline_)
		{
			error_ = "trampoline allocation failed";
			return false;
		}

		auto* trampolineBytes = static_cast<uint8_t*>(trampoline_);
		std::memcpy(trampolineBytes, code, stolenBytes);
		EmitAbsoluteJump(trampolineBytes + stolenBytes, code + stolenBytes);

		DWORD oldProtection = 0;
		if (!VirtualProtect(code, stolenBytes, PAGE_EXECUTE_READWRITE, &oldProtection))
		{
			VirtualFree(trampoline_, 0, MEM_RELEASE);
			trampoline_ = nullptr;
			error_ = "VirtualProtect failed";
			return false;
		}

		original_.assign(code, code + stolenBytes);
		EmitAbsoluteJump(code, detour);
		std::memset(code + kAbsoluteJumpSize, 0x90, stolenBytes - kAbsoluteJumpSize);

		DWORD ignored = 0;
		VirtualProtect(code, stolenBytes, oldProtection, &ignored);
		FlushInstructionCache(GetCurrentProcess(), code, stolenBytes);

		target_ = target;
		installed_ = true;
		return true;
	}

	void InlineHook::Remove()
	{
		if (!installed_)
			return;

		auto* code = static_cast<uint8_t*>(target_);
		DWORD oldProtection = 0;
		if (VirtualProtect(code, original_.size(), PAGE_EXECUTE_READWRITE, &oldProtection))
		{
			std::memcpy(code, original_.data(), original_.size());
			DWORD ignored = 0;
			VirtualProtect(code, original_.size(), oldProtection, &ignored);
			FlushInstructionCache(GetCurrentProcess(), code, original_.size());
		}

		if (trampoline_)
		{
			VirtualFree(trampoline_, 0, MEM_RELEASE);
			trampoline_ = nullptr;
		}

		target_ = nullptr;
		original_.clear();
		installed_ = false;
	}
} // namespace witcher_native_bridge

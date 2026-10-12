#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace witcher_native_bridge
{
	class InlineHook
	{
	  public:
		bool Install(void* target, void* detour, const std::vector<uint8_t>& expectedPrologue);

		void Remove();

		bool IsInstalled() const
		{
			return installed_;
		}
		void* Trampoline() const
		{
			return trampoline_;
		}
		const std::string& Error() const
		{
			return error_;
		}

	  private:
		void* target_ = nullptr;
		void* trampoline_ = nullptr;
		std::vector<uint8_t> original_;
		bool installed_ = false;
		std::string error_;
	};
} // namespace witcher_native_bridge

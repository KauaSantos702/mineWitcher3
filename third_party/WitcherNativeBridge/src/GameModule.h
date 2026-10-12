#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace witcher_native_bridge
{
	struct ModuleRegion
	{
		uint8_t* base = nullptr;
		size_t size = 0;

		bool IsValid() const
		{
			return base != nullptr && size != 0;
		}
	};

	class GameModule
	{
	  public:
		static bool Resolve();
		static bool IsResolved()
		{
			return image_.IsValid();
		}
		static bool IsSelfHosted()
		{
			return selfHosted_;
		}
		static bool IsInsideSelf(const uint8_t* address);

		static const ModuleRegion& Image()
		{
			return image_;
		}
		static const ModuleRegion& Text()
		{
			return text_;
		}
		static const ModuleRegion& Self()
		{
			return self_;
		}
		static const std::string& Version()
		{
			return version_;
		}
		static const std::string& HostName()
		{
			return hostName_;
		}

	  private:
		static bool ResolveImage();
		static bool ResolveSelf();
		static bool ResolveTextSection();
		static void ResolveVersion();

		static ModuleRegion image_;
		static ModuleRegion text_;
		static ModuleRegion self_;
		static std::string version_;
		static std::string hostName_;
		static bool selfHosted_;
	};
} // namespace witcher_native_bridge

#include "SignatureScanner.h"

#include <cstdint>

namespace witcher_native_bridge
{
	namespace
	{
		bool DecodeHex(char c, uint8_t& value)
		{
			if (c >= '0' && c <= '9')
			{
				value = static_cast<uint8_t>(c - '0');
				return true;
			}
			if (c >= 'a' && c <= 'f')
			{
				value = static_cast<uint8_t>(c - 'a' + 10);
				return true;
			}
			if (c >= 'A' && c <= 'F')
			{
				value = static_cast<uint8_t>(c - 'A' + 10);
				return true;
			}
			return false;
		}
	} // namespace

	SignaturePattern SignaturePattern::Parse(const std::string& pattern)
	{
		SignaturePattern out;
		size_t i = 0;

		while (i < pattern.size())
		{
			if (pattern[i] == ' ')
			{
				++i;
				continue;
			}

			if (pattern[i] == '?')
			{
				out.bytes_.push_back(0);
				out.mask_.push_back(false);
				++i;
				if (i < pattern.size() && pattern[i] == '?')
					++i;
				continue;
			}

			uint8_t high = 0;
			uint8_t low = 0;
			if (i + 1 >= pattern.size() || !DecodeHex(pattern[i], high) || !DecodeHex(pattern[i + 1], low))
			{
				out.bytes_.clear();
				out.mask_.clear();
				return out;
			}

			out.bytes_.push_back(static_cast<uint8_t>((high << 4) | low));
			out.mask_.push_back(true);
			i += 2;
		}

		return out;
	}

	bool SignaturePattern::MatchesAt(const uint8_t* address) const
	{
		for (size_t i = 0; i < bytes_.size(); ++i)
		{
			if (mask_[i] && address[i] != bytes_[i])
				return false;
		}
		return true;
	}

	std::vector<uint8_t*> SignatureScanner::FindAll(const ModuleRegion& region, const SignaturePattern& pattern, size_t limit)
	{
		std::vector<uint8_t*> matches;

		if (!region.IsValid() || !pattern.IsValid() || pattern.Size() > region.size)
			return matches;

		const size_t last = region.size - pattern.Size();
		for (size_t offset = 0; offset <= last; ++offset)
		{
			uint8_t* candidate = region.base + offset;
			if (!pattern.MatchesAt(candidate))
				continue;

			matches.push_back(candidate);
			if (limit != 0 && matches.size() >= limit)
				break;
		}

		return matches;
	}

	uint8_t* SignatureScanner::ResolveRelative(uint8_t* instruction, int operandOffset, int instructionLength)
	{
		if (!instruction)
			return nullptr;

		const int32_t displacement = *reinterpret_cast<int32_t*>(instruction + operandOffset);
		return instruction + instructionLength + displacement;
	}
} // namespace witcher_native_bridge

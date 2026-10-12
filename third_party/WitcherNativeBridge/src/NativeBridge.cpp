#include "NativeBridge.h"

#include "GameModule.h"
#include "InlineHook.h"
#include "SignatureScanner.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cwchar>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace witcher_native_bridge
{
	namespace
	{
		struct ScriptApi
		{
			void* alloc = nullptr;
			void* memsetFn = nullptr;
			void* namePool = nullptr;
			void* addName = nullptr;
			void* functionCtor = nullptr;
			void* scriptSystem = nullptr;
			void* registerGlobal = nullptr;

			void* opcodeTable = nullptr;
			void* bufferAlloc = nullptr;
			void* bufferCopy = nullptr;
			const char** emptyString = nullptr;
			const int* emptyStringLength = nullptr;

			size_t matches = 0;
			size_t consistent = 0;

			bool IsComplete() const
			{
				return alloc && memsetFn && namePool && addName && functionCtor && scriptSystem && registerGlobal;
			}

			bool CanMarshalStrings() const
			{
				return opcodeTable && bufferAlloc && bufferCopy && emptyString && emptyStringLength;
			}
		};

		struct AllocationResult
		{
			void* data = nullptr;
			size_t size = 0;
		};

		using AllocFn = AllocationResult* (*)(AllocationResult* result, size_t size, size_t alignment);
		using MemsetFn = void* (*)(void* destination, int value, size_t size);
		using NamePoolFn = void* (*)();
		using AddNameFn = int (*)(void* pool, const char* name);
		using FunctionCtorFn = void* (*)(void* self, int* nameIndex, void* implementation);
		using ScriptSystemFn = void* (*)();
		using RegisterGlobalFn = void (*)(void* system, void* function);
		using OpcodeHandlerFn = void (*)(void* context, void* frame, void* destination);
		using BufferAllocFn = void* (*)(size_t zero, size_t bytes, size_t kind, size_t tag);
		using BufferCopyFn = void* (*)(void* destination, const void* source, size_t bytes);

		constexpr const char* kRegistrationSignature = "BA F8 00 00 00 48 8D 4D F0 41 B8 10 00 00 00 E8 ?? ?? ?? ?? "
		                                               "48 8B 7D F0 33 D2 4C 8B 45 F8 48 8B CF E8 ?? ?? ?? ?? "
		                                               "48 85 FF 74 ?? E8 ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? "
		                                               "48 8B C8 E8 ?? ?? ?? ?? 4C 8D 05 ?? ?? ?? ?? "
		                                               "89 45 10 48 8D 55 10 48 8B CF E8 ?? ?? ?? ?? "
		                                               "48 8B F8 EB ?? 48 8B FB E8 ?? ?? ?? ?? "
		                                               "48 8B D7 48 8B C8 E8 ?? ?? ?? ??";

		constexpr int kOffsetAlloc = 15;
		constexpr int kOffsetMemset = 33;
		constexpr int kOffsetNamePool = 43;
		constexpr int kOffsetName = 48;
		constexpr int kOffsetAddName = 58;
		constexpr int kOffsetImplementation = 63;
		constexpr int kOffsetFunctionCtor = 80;
		constexpr int kOffsetScriptSystem = 93;
		constexpr int kOffsetRegisterGlobal = 104;
		constexpr size_t kMinimumMatches = 32;

		constexpr size_t kFunctionObjectSize = 0xF8;
		constexpr size_t kFunctionObjectAlignment = 0x10;

		ScriptApi g_api{};
		bool g_bindingAttempted = false;
		bool g_bindingResolved = false;
		std::map<std::string, void*> g_existingNatives;

		struct PendingNativeRegistration
		{
			std::string name;
			NativeImplementation implementation = nullptr;
		};

		InlineHook g_registerHook;
		std::atomic<bool> g_registrationPhaseStarted{false};
		std::atomic<DWORD> g_registrationThreadId{0};
		std::mutex g_registrationMutex;
		std::vector<PendingNativeRegistration> g_pendingRegistrations;
		std::set<std::string> g_claimedNativeNames;
		std::string g_registrationError;

		std::mutex g_logMutex;
		std::ofstream g_log;

		void* ResolveCall(uint8_t* site, int offset)
		{
			return SignatureScanner::ResolveRelative(site + offset, 1, 5);
		}

		void* ResolveLea(uint8_t* site, int offset)
		{
			return SignatureScanner::ResolveRelative(site + offset, 3, 7);
		}

		std::string NarrowAsciiName(const char* value, size_t limit = 96)
		{
			std::string out;
			if (!value)
				return out;

			for (size_t i = 0; i < limit && value[i] != '\0'; ++i)
			{
				const unsigned char ch = static_cast<unsigned char>(value[i]);
				if (ch < 32 || ch > 126)
					return {};
				out.push_back(static_cast<char>(ch));
			}
			return out;
		}

		bool IsInsideGameImage(void* address)
		{
			const ModuleRegion& image = GameModule::Image();
			if (!address || !image.IsValid())
				return false;

			auto* p = static_cast<uint8_t*>(address);
			return p >= image.base && p < (image.base + image.size);
		}

		bool LooksLikeOpcodeTable(void* address, size_t requiredEntries = 32)
		{
			if (!IsInsideGameImage(address))
				return false;

			const ModuleRegion& text = GameModule::Text();
			auto** entries = static_cast<uint8_t**>(address);
			size_t valid = 0;

			for (size_t i = 0; i < requiredEntries; ++i)
			{
				uint8_t* entry = entries[i];
				if (!entry)
					continue;
				if (entry < text.base || entry >= text.base + text.size)
					return false;
				++valid;
			}

			return valid * 2 >= requiredEntries;
		}

		void* FindExistingNative(const std::string& name)
		{
			const auto it = g_existingNatives.find(name);
			return it == g_existingNatives.end() ? nullptr : it->second;
		}

		void ResolveStringMarshalling(ScriptApi& api)
		{
			void* logChannel = FindExistingNative("LogChannel");
			if (!logChannel)
			{
				DebugLog("string marshalling: LogChannel native was not found");
				return;
			}

			auto* code = static_cast<uint8_t*>(logChannel);

			for (int i = 0; i < 0x40; ++i)
			{
				if (code[i] != 0x48 && code[i] != 0x4C)
					continue;
				if (code[i + 1] != 0x8D)
					continue;
				if ((code[i + 2] & 0xC7) != 0x05)
					continue;

				void* candidate = SignatureScanner::ResolveRelative(code + i, 3, 7);
				if (IsInsideGameImage(candidate))
				{
					api.opcodeTable = candidate;
					break;
				}
			}

			const SignaturePattern stringPattern = SignaturePattern::Parse("8B 15 ?? ?? ?? ?? 33 C9 41 B9 0E 00 00 00 89 54 24 ?? "
			                                                               "41 B8 01 00 00 00 48 C7 44 24 20 00 00 00 00 E8 ?? ?? ?? ?? 44 8B 44 24 ?? 48 8B C8 "
			                                                               "48 8B 15 ?? ?? ?? ?? 48 89 44 24 ?? E8 ?? ?? ?? ??");

			const ModuleRegion logChannelRegion{code, 0xC0};
			const std::vector<uint8_t*> stringSites = SignatureScanner::FindAll(logChannelRegion, stringPattern, 2);
			if (stringSites.size() == 1)
			{
				uint8_t* anchor = stringSites.front();
				api.emptyStringLength = reinterpret_cast<const int*>(SignatureScanner::ResolveRelative(anchor, 2, 6));
				api.bufferAlloc = SignatureScanner::ResolveRelative(anchor + 33, 1, 5);
				api.emptyString = reinterpret_cast<const char**>(SignatureScanner::ResolveRelative(anchor + 46, 3, 7));
				api.bufferCopy = SignatureScanner::ResolveRelative(anchor + 58, 1, 5);
			}
			else
			{
				DebugLog("string marshalling: LogChannel helper pattern matches=" + std::to_string(stringSites.size()));
			}

			DebugLog(std::string("string marshalling components: opcode=") + (api.opcodeTable ? "OK" : "missing") + " alloc=" + (api.bufferAlloc ? "OK" : "missing") + " copy=" + (api.bufferCopy ? "OK" : "missing") + " empty=" + (api.emptyString ? "OK" : "missing") + " length=" + (api.emptyStringLength ? "OK" : "missing"));
		}

		void* ConsensusAddress(const std::vector<void*>& values, size_t& agreeing)
		{
			std::map<void*, size_t> counts;
			for (void* value : values)
			{
				if (value)
					++counts[value];
			}

			void* winner = nullptr;
			agreeing = 0;
			for (const auto& [address, count] : counts)
			{
				if (count > agreeing)
				{
					winner = address;
					agreeing = count;
				}
			}
			return winner;
		}

		void DispatchParameter(void* frame, void* destination)
		{
			auto** instruction = reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(frame) + 0x30);
			if (!instruction || !*instruction)
				return;

			const uint8_t opcode = **instruction;
			*instruction += 1;

			void* context = *reinterpret_cast<void**>(frame);
			if (!LooksLikeOpcodeTable(g_api.opcodeTable))
				return;

			auto* table = static_cast<OpcodeHandlerFn*>(g_api.opcodeTable);
			OpcodeHandlerFn handler = table[opcode];
			if (handler)
				handler(context, frame, destination);
		}

		bool WideToUtf8(const wchar_t* value, size_t length, std::string& out)
		{
			out.clear();
			if (length == 0)
				return true;
			if (!value || length > static_cast<size_t>((std::numeric_limits<int>::max)()))
				return false;

			const int sourceLength = static_cast<int>(length);
			const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, sourceLength, nullptr, 0, nullptr, nullptr);
			if (bytes <= 0)
				return false;

			out.resize(static_cast<size_t>(bytes));
			return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, sourceLength, out.data(), bytes, nullptr, nullptr) == bytes;
		}

		std::wstring Utf8ToWide(const char* value, size_t length)
		{
			if (!value || length == 0 || length > static_cast<size_t>((std::numeric_limits<int>::max)()))
				return {};

			const int sourceLength = static_cast<int>(length);
			const int characters = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, sourceLength, nullptr, 0);
			if (characters <= 0)
				return {};

			std::wstring out(static_cast<size_t>(characters), L'\0');
			if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, sourceLength, out.data(), characters) != characters)
				return {};

			return out;
		}

		bool MakeEmptyScriptString(ScriptString& value)
		{
			if (!g_api.CanMarshalStrings())
				return false;

			const int length = *g_api.emptyStringLength;
			const char* source = *g_api.emptyString;
			if (length < 0 || length > 64)
				return false;

			value = {};
			value.size = static_cast<uint32_t>(length);
			if (length == 0)
				return true;
			if (!source)
				return false;

			auto allocate = reinterpret_cast<BufferAllocFn>(g_api.bufferAlloc);
			auto copy = reinterpret_cast<BufferCopyFn>(g_api.bufferCopy);

			void* buffer = allocate(0, static_cast<size_t>(length), 1, 14);
			if (!buffer)
				return false;

			copy(buffer, source, static_cast<size_t>(length));
			value.data = reinterpret_cast<decltype(value.data)>(buffer);
			return true;
		}

		uint32_t LogicalStringLength(const ScriptString& value)
		{
			return (value.data && value.size > 0) ? value.size - 1 : 0;
		}

		int InternNameUnguarded(const wchar_t* value)
		{
			if (!value)
				return 0;

			std::string utf8;
			if (!WideToUtf8(value, std::wcslen(value), utf8))
				return 0;

			auto getPool = reinterpret_cast<NamePoolFn>(g_api.namePool);
			auto addName = reinterpret_cast<AddNameFn>(g_api.addName);

			void* pool = getPool();
			if (!pool)
				return 0;

			return addName(pool, utf8.c_str());
		}

		int InternNameGuarded(const wchar_t* value)
		{
			__try
			{
				return InternNameUnguarded(value);
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return 0;
			}
		}

		bool WriteScriptStringUnguarded(void* result, const wchar_t* value, size_t length)
		{
			if (!result || !value || !g_api.CanMarshalStrings())
				return false;

			auto allocate = reinterpret_cast<BufferAllocFn>(g_api.bufferAlloc);
			auto copy = reinterpret_cast<BufferCopyFn>(g_api.bufferCopy);

			std::string utf8;
			if (!WideToUtf8(value, length, utf8))
				return false;

			const size_t bytes = utf8.size() + 1;
			if (bytes > static_cast<size_t>((std::numeric_limits<uint32_t>::max)()))
				return false;

			void* buffer = allocate(0, bytes, 1, 14);
			if (!buffer)
				return false;

			if (!utf8.empty())
				copy(buffer, utf8.data(), utf8.size());
			static_cast<char*>(buffer)[utf8.size()] = '\0';

			auto* destination = static_cast<ScriptString*>(result);
			destination->data = reinterpret_cast<decltype(destination->data)>(buffer);
			destination->size = static_cast<uint32_t>(bytes);
			destination->padding = 0;
			return true;
		}

		bool WriteScriptStringGuarded(void* result, const wchar_t* value, size_t length)
		{
			__try
			{
				return WriteScriptStringUnguarded(result, value, length);
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		void* RegisterNativeUnguarded(const char* name, NativeImplementation implementation)
		{
			auto alloc = reinterpret_cast<AllocFn>(g_api.alloc);
			auto zero = reinterpret_cast<MemsetFn>(g_api.memsetFn);
			auto getPool = reinterpret_cast<NamePoolFn>(g_api.namePool);
			auto addName = reinterpret_cast<AddNameFn>(g_api.addName);
			auto construct = reinterpret_cast<FunctionCtorFn>(g_api.functionCtor);
			auto getSystem = reinterpret_cast<ScriptSystemFn>(g_api.scriptSystem);
			auto registerGlobal = reinterpret_cast<RegisterGlobalFn>(g_registerHook.Trampoline());

			AllocationResult allocation{};
			alloc(&allocation, kFunctionObjectSize, kFunctionObjectAlignment);

			void* storage = allocation.data;
			if (!storage || allocation.size < kFunctionObjectSize)
				return nullptr;

			zero(storage, 0, allocation.size);

			void* pool = getPool();
			if (!pool)
				return nullptr;

			int nameIndex = addName(pool, name);
			void* function = construct(storage, &nameIndex, reinterpret_cast<void*>(implementation));
			if (!function)
				return nullptr;

			void* system = getSystem();
			if (!system)
				return nullptr;

			registerGlobal(system, function);
			return function;
		}

		bool IsValidPublicNativeName(const char* name)
		{
			if (!name || name[0] == '\0')
				return false;

			size_t length = 0;
			for (; name[length] != '\0'; ++length)
			{
				const unsigned char ch = static_cast<unsigned char>(name[length]);
				if (ch < 32 || ch > 126 || length >= 127)
					return false;
			}

			return length > 0;
		}

		bool ConflictsWithExistingGameNative(const std::string& name)
		{
			return g_existingNatives.find(name) != g_existingNatives.end();
		}

		bool RegisterNativeImmediateGuarded(const char* name, NativeImplementation implementation)
		{
			__try
			{
				return RegisterNativeUnguarded(name, implementation) != nullptr;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		bool RegisterNativeImmediate(const PendingNativeRegistration& registration)
		{
			if (!g_bindingResolved || !g_registerHook.IsInstalled())
				return false;

			if (ConflictsWithExistingGameNative(registration.name))
			{
				DebugLog("native name conflicts with existing REDengine function: " + registration.name);
				return false;
			}

			return RegisterNativeImmediateGuarded(registration.name.c_str(), registration.implementation);
		}

		void FlushPendingRegistrations()
		{
			std::vector<PendingNativeRegistration> pending;
			{
				std::lock_guard<std::mutex> lock(g_registrationMutex);
				if (g_pendingRegistrations.empty())
					return;
				pending.swap(g_pendingRegistrations);
			}

			for (const PendingNativeRegistration& registration : pending)
			{
				const bool registered = RegisterNativeImmediate(registration);
				const std::string& name = registration.name;
				DebugLog("native registration: " + name + "=" + (registered ? "OK" : "FAILED"));

				if (!registered)
				{
					if (!g_registrationError.empty())
						g_registrationError += ", ";
					g_registrationError += name;
				}
			}
		}

		void RegisterGlobalDetour(void* system, void* function)
		{
			auto original = reinterpret_cast<RegisterGlobalFn>(g_registerHook.Trampoline());
			original(system, function);

			g_registrationThreadId.store(GetCurrentThreadId(), std::memory_order_release);
			g_registrationPhaseStarted.store(true, std::memory_order_release);
			FlushPendingRegistrations();
		}
	} // namespace

	void InitLog(const std::string& path)
	{
		std::lock_guard<std::mutex> lock(g_logMutex);
		g_log.open(path, std::ios::out | std::ios::trunc);
	}

	void ShutdownLog()
	{
		std::lock_guard<std::mutex> lock(g_logMutex);
		if (g_log.is_open())
		{
			g_log.flush();
			g_log.close();
		}
	}

	void DebugLog(const std::string& text)
	{
		SYSTEMTIME now{};
		GetLocalTime(&now);

		char timestamp[32]{};
		sprintf_s(timestamp, "%02u:%02u:%02u.%03u", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);

		const std::string line = std::string(timestamp) + " WitcherNativeBridge: " + text;
		OutputDebugStringA((line + "\n").c_str());

		std::lock_guard<std::mutex> lock(g_logMutex);
		if (g_log.is_open())
		{
			g_log << line << '\n';
			g_log.flush();
		}
	}

	bool ResolveScriptApi()
	{
		if (g_bindingAttempted)
			return g_bindingResolved;

		g_bindingAttempted = true;

		if (!GameModule::Resolve())
		{
			DebugLog("script API: failed to resolve the host image");
			return false;
		}
		if (GameModule::IsSelfHosted())
		{
			DebugLog("script API: bridge was loaded as the host image");
			return false;
		}

		const SignaturePattern pattern = SignaturePattern::Parse(kRegistrationSignature);
		if (!pattern.IsValid())
		{
			DebugLog("script API: registration pattern is invalid");
			return false;
		}

		std::vector<uint8_t*> sites = SignatureScanner::FindAll(GameModule::Text(), pattern);
		g_api.matches = sites.size();
		if (sites.size() < kMinimumMatches)
		{
			DebugLog("script API: registration sites=" + std::to_string(g_api.matches) + " (minimum " + std::to_string(kMinimumMatches) + ")");
			return false;
		}

		std::vector<void*> allocs;
		std::vector<void*> memsets;
		std::vector<void*> pools;
		std::vector<void*> addNames;
		std::vector<void*> constructors;
		std::vector<void*> systems;
		std::vector<void*> registrars;

		allocs.reserve(sites.size());
		memsets.reserve(sites.size());
		pools.reserve(sites.size());
		addNames.reserve(sites.size());
		constructors.reserve(sites.size());
		systems.reserve(sites.size());
		registrars.reserve(sites.size());

		g_existingNatives.clear();

		for (uint8_t* site : sites)
		{
			const auto* name = static_cast<const char*>(ResolveLea(site, kOffsetName));
			void* implementation = ResolveLea(site, kOffsetImplementation);

			if (name && implementation)
			{
				const std::string decoded = NarrowAsciiName(name);
				if (!decoded.empty())
					g_existingNatives[decoded] = implementation;
			}

			allocs.push_back(ResolveCall(site, kOffsetAlloc));
			memsets.push_back(ResolveCall(site, kOffsetMemset));
			pools.push_back(ResolveCall(site, kOffsetNamePool));
			addNames.push_back(ResolveCall(site, kOffsetAddName));
			constructors.push_back(ResolveCall(site, kOffsetFunctionCtor));
			systems.push_back(ResolveCall(site, kOffsetScriptSystem));
			registrars.push_back(ResolveCall(site, kOffsetRegisterGlobal));
		}

		size_t agreeing = 0;
		size_t worstAgreement = sites.size();

		g_api.alloc = ConsensusAddress(allocs, agreeing);
		worstAgreement = (std::min)(worstAgreement, agreeing);

		g_api.memsetFn = ConsensusAddress(memsets, agreeing);
		worstAgreement = (std::min)(worstAgreement, agreeing);

		g_api.namePool = ConsensusAddress(pools, agreeing);
		worstAgreement = (std::min)(worstAgreement, agreeing);

		g_api.addName = ConsensusAddress(addNames, agreeing);
		worstAgreement = (std::min)(worstAgreement, agreeing);

		g_api.functionCtor = ConsensusAddress(constructors, agreeing);
		worstAgreement = (std::min)(worstAgreement, agreeing);

		g_api.scriptSystem = ConsensusAddress(systems, agreeing);
		worstAgreement = (std::min)(worstAgreement, agreeing);

		g_api.registerGlobal = ConsensusAddress(registrars, agreeing);
		worstAgreement = (std::min)(worstAgreement, agreeing);

		g_api.consistent = worstAgreement;
		g_bindingResolved = g_api.IsComplete() && worstAgreement == sites.size();

		if (g_bindingResolved)
			ResolveStringMarshalling(g_api);

		DebugLog("registration sites=" + std::to_string(g_api.matches) + " consensus=" + std::to_string(g_api.consistent));

		return g_bindingResolved;
	}

	bool CanMarshalStrings()
	{
		return g_api.CanMarshalStrings();
	}

	bool InstallRegistrationHook()
	{
		if (!g_bindingResolved)
			return false;
		if (g_registerHook.IsInstalled())
			return true;

		const std::vector<uint8_t> expectedPrologue = {0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20};

		if (!g_registerHook.Install(g_api.registerGlobal, reinterpret_cast<void*>(&RegisterGlobalDetour), expectedPrologue))
		{
			g_registrationError = g_registerHook.Error();
			return false;
		}

		return true;
	}

	void RemoveRegistrationHook()
	{
		g_registerHook.Remove();
	}

	const std::string& RegistrationError()
	{
		return g_registrationError;
	}

	bool QueueNativeRegistration(const char* name, NativeImplementation implementation)
	{
		if (!IsValidPublicNativeName(name) || !implementation)
			return false;

		const std::string ownedName(name);

		{
			std::lock_guard<std::mutex> lock(g_registrationMutex);
			if (!g_claimedNativeNames.insert(ownedName).second)
			{
				DebugLog("duplicate native rejected: " + ownedName);
				return false;
			}

			g_pendingRegistrations.push_back({ownedName, implementation});
		}

		DebugLog("queued native: " + ownedName);

		if (g_registrationPhaseStarted.load(std::memory_order_acquire) && g_registrationThreadId.load(std::memory_order_acquire) == GetCurrentThreadId())
			FlushPendingRegistrations();

		return true;
	}

	void AdvanceFrame(void* frame)
	{
		if (!frame)
			return;

		auto** instruction = reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(frame) + 0x30);
		if (instruction && *instruction)
			*instruction += 1;
	}

	int ReadIntParameter(void* frame)
	{
		int value = 0;
		__try
		{
			DispatchParameter(frame, &value);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			value = 0;
		}
		return value;
	}

	bool ReadBoolParameter(void* frame)
	{
		bool value = false;
		__try
		{
			DispatchParameter(frame, &value);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			value = false;
		}
		return value;
	}

	float ReadFloatParameter(void* frame)
	{
		float value = 0.0f;
		__try
		{
			DispatchParameter(frame, &value);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			value = 0.0f;
		}
		return value;
	}

	WNB_Name ReadNameParameter(void* frame)
	{
		WNB_Name value = 0;
		__try
		{
			DispatchParameter(frame, &value);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			value = 0;
		}
		return value;
	}

	int ReadStringParameter(void* frame, ScriptString& text)
	{
		__try
		{
			if (!MakeEmptyScriptString(text))
				return -2;

			DispatchParameter(frame, &text);
			return static_cast<int>(text.size);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return -3;
		}
	}

	std::wstring ScriptStringToWide(const ScriptString& text)
	{
		if (!text.data)
			return {};

		const uint32_t length = LogicalStringLength(text);
		return Utf8ToWide(reinterpret_cast<const char*>(text.data), length);
	}

	std::string ScriptStringToUtf8Lossy(const ScriptString& text)
	{
		if (!text.data)
			return {};

		const uint32_t length = LogicalStringLength(text);
		const char* bytes = reinterpret_cast<const char*>(text.data);
		return std::string(bytes, bytes + length);
	}

	void WriteIntResult(void* result, int value)
	{
		if (result)
			*static_cast<int*>(result) = value;
	}

	void WriteBoolResult(void* result, bool value)
	{
		if (result)
			*static_cast<bool*>(result) = value;
	}

	void WriteFloatResult(void* result, float value)
	{
		if (result)
			*static_cast<float*>(result) = value;
	}

	bool WriteStringResult(void* result, const wchar_t* value, size_t length)
	{
		return WriteScriptStringGuarded(result, value, length);
	}

	bool WriteStringResult(void* result, const std::wstring& value)
	{
		return WriteStringResult(result, value.c_str(), value.size());
	}

	void WriteNameIndexResult(void* result, WNB_Name value)
	{
		if (result)
			*static_cast<WNB_Name*>(result) = value;
	}

	void WriteNameResult(void* result, const wchar_t* value)
	{
		if (!result)
			return;

		if (!value || value[0] == L'\0' || wcscmp(value, L"None") == 0)
		{
			*static_cast<WNB_Name*>(result) = 0;
			return;
		}

		*static_cast<WNB_Name*>(result) = static_cast<WNB_Name>(InternNameGuarded(value));
	}

	void WriteNameResult(void* result, const std::wstring& value)
	{
		WriteNameResult(result, value.c_str());
	}

	void ReturnVoid(void* frame)
	{
		AdvanceFrame(frame);
	}

	void ReturnInt(void* frame, void* result, int value)
	{
		AdvanceFrame(frame);
		WriteIntResult(result, value);
	}

	void ReturnBool(void* frame, void* result, bool value)
	{
		AdvanceFrame(frame);
		WriteBoolResult(result, value);
	}

	void ReturnFloat(void* frame, void* result, float value)
	{
		AdvanceFrame(frame);
		WriteFloatResult(result, value);
	}

	bool ReturnString(void* frame, void* result, const wchar_t* value, size_t length)
	{
		AdvanceFrame(frame);
		return WriteStringResult(result, value, length);
	}

	void ReturnName(void* frame, void* result, WNB_Name value)
	{
		AdvanceFrame(frame);
		WriteNameIndexResult(result, value);
	}

	void ReturnNameFromString(void* frame, void* result, const wchar_t* value)
	{
		AdvanceFrame(frame);
		WriteNameResult(result, value);
	}
} // namespace witcher_native_bridge

extern "C" uint32_t WNB_GetApiVersion()
{
	return WNB_API_VERSION;
}

extern "C" bool WNB_RegisterNative(const char* name, WNB_NativeImplementation implementation)
{
	return witcher_native_bridge::QueueNativeRegistration(name, implementation);
}

extern "C" int WNB_ReadIntParameter(void* frame)
{
	return witcher_native_bridge::ReadIntParameter(frame);
}

extern "C" bool WNB_ReadBoolParameter(void* frame)
{
	return witcher_native_bridge::ReadBoolParameter(frame);
}

extern "C" float WNB_ReadFloatParameter(void* frame)
{
	return witcher_native_bridge::ReadFloatParameter(frame);
}

extern "C" WNB_Name WNB_ReadNameParameter(void* frame)
{
	return witcher_native_bridge::ReadNameParameter(frame);
}

extern "C" int WNB_ReadStringParameter(void* frame, WNB_String* text)
{
	if (!text)
		return -1;

	return witcher_native_bridge::ReadStringParameter(frame, *text);
}

extern "C" void WNB_ReturnVoid(void* frame)
{
	witcher_native_bridge::ReturnVoid(frame);
}

extern "C" void WNB_ReturnInt(void* frame, void* result, int value)
{
	witcher_native_bridge::ReturnInt(frame, result, value);
}

extern "C" void WNB_ReturnBool(void* frame, void* result, bool value)
{
	witcher_native_bridge::ReturnBool(frame, result, value);
}

extern "C" void WNB_ReturnFloat(void* frame, void* result, float value)
{
	witcher_native_bridge::ReturnFloat(frame, result, value);
}

extern "C" bool WNB_ReturnString(void* frame, void* result, const wchar_t* value, uint32_t length)
{
	if (!value)
	{
		witcher_native_bridge::ReturnVoid(frame);
		return false;
	}

	return witcher_native_bridge::ReturnString(frame, result, value, static_cast<size_t>(length));
}

extern "C" void WNB_ReturnName(void* frame, void* result, WNB_Name value)
{
	witcher_native_bridge::ReturnName(frame, result, value);
}

extern "C" void WNB_ReturnNameFromString(void* frame, void* result, const wchar_t* value)
{
	witcher_native_bridge::ReturnNameFromString(frame, result, value);
}

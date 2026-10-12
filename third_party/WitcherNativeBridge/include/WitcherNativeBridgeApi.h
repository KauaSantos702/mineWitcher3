#pragma once

#include <cstdint>

#ifdef WITCHER_NATIVE_BRIDGE_EXPORTS
#define WNB_API __declspec(dllexport)
#else
#define WNB_API __declspec(dllimport)
#endif

using WNB_NativeImplementation = void (*)(void* context, void* frame, void* result);
using WNB_Name = int32_t;

struct WNB_String
{
	wchar_t* data;
	uint32_t size;
	uint32_t padding;
};

inline constexpr uint32_t WNB_API_VERSION = 2;

extern "C"
{
	WNB_API uint32_t WNB_GetApiVersion();
	WNB_API bool WNB_RegisterNative(const char* name, WNB_NativeImplementation implementation);

	WNB_API int WNB_ReadIntParameter(void* frame);
	WNB_API bool WNB_ReadBoolParameter(void* frame);
	WNB_API float WNB_ReadFloatParameter(void* frame);
	WNB_API WNB_Name WNB_ReadNameParameter(void* frame);
	WNB_API int WNB_ReadStringParameter(void* frame, WNB_String* text);

	WNB_API void WNB_ReturnVoid(void* frame);
	WNB_API void WNB_ReturnInt(void* frame, void* result, int value);
	WNB_API void WNB_ReturnBool(void* frame, void* result, bool value);
	WNB_API void WNB_ReturnFloat(void* frame, void* result, float value);
	WNB_API bool WNB_ReturnString(void* frame, void* result, const wchar_t* value, uint32_t length);
	WNB_API void WNB_ReturnName(void* frame, void* result, WNB_Name value);
	WNB_API void WNB_ReturnNameFromString(void* frame, void* result, const wchar_t* value);
}

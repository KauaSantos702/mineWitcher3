#pragma once

#include "WitcherNativeBridgeApi.h"

#include <cstdint>
#include <string>

namespace witcher_native_bridge
{
	using ScriptString = WNB_String;
	using NativeImplementation = WNB_NativeImplementation;

	void InitLog(const std::string& path);
	void ShutdownLog();
	void DebugLog(const std::string& text);

	bool ResolveScriptApi();
	bool CanMarshalStrings();
	bool InstallRegistrationHook();
	void RemoveRegistrationHook();
	const std::string& RegistrationError();

	bool QueueNativeRegistration(const char* name, NativeImplementation implementation);

	void AdvanceFrame(void* frame);
	int ReadIntParameter(void* frame);
	bool ReadBoolParameter(void* frame);
	float ReadFloatParameter(void* frame);
	WNB_Name ReadNameParameter(void* frame);
	int ReadStringParameter(void* frame, ScriptString& text);

	std::wstring ScriptStringToWide(const ScriptString& text);
	std::string ScriptStringToUtf8Lossy(const ScriptString& text);

	void WriteIntResult(void* result, int value);
	void WriteBoolResult(void* result, bool value);
	void WriteFloatResult(void* result, float value);
	bool WriteStringResult(void* result, const wchar_t* value, size_t length);
	bool WriteStringResult(void* result, const std::wstring& value);
	void WriteNameIndexResult(void* result, WNB_Name value);
	void WriteNameResult(void* result, const wchar_t* value);
	void WriteNameResult(void* result, const std::wstring& value);

	void ReturnVoid(void* frame);
	void ReturnInt(void* frame, void* result, int value);
	void ReturnBool(void* frame, void* result, bool value);
	void ReturnFloat(void* frame, void* result, float value);
	bool ReturnString(void* frame, void* result, const wchar_t* value, size_t length);
	void ReturnName(void* frame, void* result, WNB_Name value);
	void ReturnNameFromString(void* frame, void* result, const wchar_t* value);
} // namespace witcher_native_bridge

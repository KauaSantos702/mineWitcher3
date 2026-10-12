# WitcherNativeBridge

Shared native bridge API for **The Witcher 3**, allowing C++ ASI mods to register custom WitcherScript import functions through one API.

## Building

Requirements:

- Visual Studio 2022 or newer
- x64
- C++17

Open `WitcherNativeBridge.sln` and build `Release | x64`.

The build produces:

```text
WitcherNativeBridge.asi
WitcherNativeBridge.lib
```

`WitcherNativeBridge.asi` is the runtime bridge.

`WitcherNativeBridge.lib` and `include/WitcherNativeBridgeApi.h` are used by ASI mods that want to register their own WitcherScript imports.

## Using the API

Include the public header:

```cpp
#include <WitcherNativeBridgeApi.h>
```

Create a native function:

```cpp
void ExampleInt(void*, void* frame, void* result)
{
	int value = WNB_ReadIntParameter(frame);

	WNB_ReturnInt(frame, result, value);
}
```

Register it:

```cpp
WNB_RegisterNative("MyMod_ExampleInt", &ExampleInt);
```

Then import the same function name in WitcherScript:

```witcherscript
import function MyMod_ExampleInt(value : int) : int;
```

Read parameters in the same order as the WitcherScript declaration, then finish the callback using the matching `WNB_Return*` function.

For example:

```cpp
void Example(void*, void* frame, void* result)
{
	int id = WNB_ReadIntParameter(frame);
	bool enabled = WNB_ReadBoolParameter(frame);

	WNB_ReturnBool(frame, result, enabled);
}
```

```witcherscript
import function MyMod_Example(id : int, enabled : bool) : bool;
```

`WNB_Return*` automatically finalizes the WitcherScript call before returning the result.

Available return functions:

```cpp
WNB_ReturnVoid(frame);

WNB_ReturnInt(frame, result, value);
WNB_ReturnBool(frame, result, value);
WNB_ReturnFloat(frame, result, value);
WNB_ReturnString(frame, result, value, length);
WNB_ReturnName(frame, result, value);
WNB_ReturnNameFromString(frame, result, value);
```

For a WitcherScript function with no return value, use `WNB_ReturnVoid`:

```cpp
void ExampleVoid(void*, void* frame, void*)
{
	int value = WNB_ReadIntParameter(frame);

	// Do something with value...

	WNB_ReturnVoid(frame);
}
```

```witcherscript
import function MyMod_ExampleVoid(value : int);
```

## Supported Types

WitcherNativeBridge currently supports reading and returning:

- `int`
- `bool`
- `float`
- `string`
- `name`

## Built-in Imports

The bridge also provides:

```witcherscript
import function WNB_GetApiVersion() : int;
import function WNB_StringToName(value : string) : name;
```

## Example Projects

Ready-to-build example projects for Next Gen (v4.04) and Remastered (v5.00+) are included here:

[`examples/Example4.04`](examples/Example4.04)
[`examples/ExampleRemastered`](examples/ExampleRemastered)

Each solution already includes the required header, import library, linker settings, and simple examples for each supported type.

## Notes

- Native function names must be unique. It is recommended to prefix your import functions with your mod name like `WO_Send()`.
- Functions must be registered at game startup and registered callbacks should remain loaded for the lifetime of the game.
- `WitcherNativeBridge.asi` must be installed for dependent mods to work.

## Credits
- [Flawkee](https://github.com/flawkee) - Created native import function system
- [rejuvenate7](https://github.com/rejuvenate7/WitcherOnline) - Created standalone api with it

## Known mods that are using WitcherNativeBridge

- [rejuvenate7/WitcherOnline](https://www.nexusmods.com/witcher3/mods/11590)

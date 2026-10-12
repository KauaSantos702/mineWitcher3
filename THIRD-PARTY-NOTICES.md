# Third-party notices

MineWitcher3 is a personal, educational mod. It contains code derived from, or redistributes, the
components below. Each one keeps its own license; the full texts follow the table.

| Component | Where it is used | License | Source |
|---|---|---|---|
| SkyCraft (chasmlol) | `protocol/mw3_protocol.h`, `fabric/` (almost all of it), `tools/fake_witcher.py`; the native plugin's IPC/launcher/overlay code where marked | MIT | https://github.com/chasmlol/skycraft (commit `bfcaf17`) |
| WitcherNativeBridge (rejuvenate7) | `third_party/WitcherNativeBridge/` (vendored, unmodified) | MIT | https://github.com/rejuvenate7/WitcherNativeBridge (commit `415e1c1`) |
| Ultimate ASI Loader (ThirteenAG) | `third_party/UltimateASILoader/winmm.dll` (v9.7.4 x64 `dinput8.dll`, renamed, unmodified) | MIT | https://github.com/ThirteenAG/Ultimate-ASI-Loader |
| Gradle Wrapper | `fabric/gradlew`, `fabric/gradlew.bat`, `fabric/gradle/wrapper/` (unmodified) | Apache-2.0 | https://github.com/gradle/gradle |
| Prism Launcher | `%LOCALAPPDATA%\MineWitcher3\Prism\` (v11.1.1 portable Windows x64 build, unmodified; **downloaded** from the official GitHub release by `tools/setup_minecraft.ps1`, not in this repository) | GPL-3.0 | https://github.com/PrismLauncher/PrismLauncher (tag `11.1.1`) |
| Fabric API (FabricMC) | `...\Prism\instances\MineWitcher3\.minecraft\mods\fabric-api-0.161.0+26.3.jar` (unmodified; **downloaded** from Modrinth by `tools/setup_minecraft.ps1`; also a Gradle build dependency) | Apache-2.0 | https://github.com/FabricMC/fabric |

Every file derived from SkyCraft carries the line
`Derived from SkyCraft (https://github.com/chasmlol/skycraft), Copyright (c) 2026 chasmlol, MIT License.`

Build/runtime dependencies that are downloaded by Gradle or the launcher and are **not** included in
this repository: Minecraft (Mojang), Fabric Loader (Apache-2.0), Fabric API (Apache-2.0), Fabric
Loom (MIT), e4mc (MIT, only on the `runClient` dev classpath), JUnit 5 (EPL-2.0, tests only).
The Witcher 3 (CD PROJEKT RED) is not included either. MineWitcher3 is not affiliated with or
endorsed by Mojang, Microsoft, CD PROJEKT RED or the SkyCraft author.

<!-- Add new entries above this line (and their full license text below). -->

---

## SkyCraft

```
MIT License

Copyright (c) 2026 chasmlol

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## WitcherNativeBridge

Upstream's `LICENSE` is the MIT template with the copyright line left unfilled
(`Copyright (c) [year] [fullname]`); it is reproduced as published. The author is the GitHub user
rejuvenate7. See `third_party/WitcherNativeBridge/LICENSE.txt`.

```
MIT License

Copyright (c) [year] [fullname]

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Ultimate ASI Loader

See `third_party/UltimateASILoader/LICENSE.txt`.

```
MIT License

Copyright (c) 2023 ThirteenAG

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Gradle Wrapper

The Gradle Wrapper scripts and jar are distributed by Gradle Inc. under the Apache License 2.0
(https://www.apache.org/licenses/LICENSE-2.0). They are copied unmodified from SkyCraft's
`fabric/` folder (Gradle 9.7.1).


## Prism Launcher

Prism Launcher is free software under the GNU General Public License v3.0. MineWitcher3 does not
modify, link against or vendor it: `tools/setup_minecraft.ps1` downloads the official, unmodified
portable build (`PrismLauncher-Windows-MSVC-Portable-11.1.1.zip`, SHA-256
`ab35a770fb06d89d2ccc098079db5db329fb4e68f42b72babd8b095efde3d2d7`) and runs it as a separate program.
The full license text is downloaded next to it as `LICENSE-PrismLauncher.txt` (from
https://raw.githubusercontent.com/PrismLauncher/PrismLauncher/11.1.1/LICENSE); the corresponding
source is at https://github.com/PrismLauncher/PrismLauncher/tree/11.1.1.
GPL-3.0: https://www.gnu.org/licenses/gpl-3.0.txt

## Fabric API

Fabric API (Copyright (c) FabricMC) is licensed under the Apache License 2.0
(https://www.apache.org/licenses/LICENSE-2.0). `tools/setup_minecraft.ps1` downloads the unmodified
jar `fabric-api-0.161.0+26.3.jar` from Modrinth (project `P7dR8mSH`, version `bNnaTiuM`, SHA-512
`ed6b2586d6fde11fde8472f5a527c51e99b67026e46f94d4bfd85e7e28ce5ee299173ee16ad576ceb51f39f98d30a811086a6deb1a86a524859cc16e12da109d`);
its license file is inside the jar.

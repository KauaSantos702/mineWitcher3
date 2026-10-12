cmake_minimum_required(VERSION 3.20)
project(MineWitcher3Native CXX)

# C++20: std::atomic_ref no link (memoria compartilhada).
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# CRT estatico: nao depende do VC++ redistributable instalado na maquina.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Compile em x64 (o witcher3.exe e 64 bits).")
endif()

# --- WitcherNativeBridge (third_party, MIT) -> WitcherNativeBridge.asi -------------
set(WNB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/WitcherNativeBridge)
add_library(WitcherNativeBridge SHARED
    ${WNB_DIR}/src/dllmain.cpp
    ${WNB_DIR}/src/GameModule.cpp
    ${WNB_DIR}/src/InlineHook.cpp
    ${WNB_DIR}/src/NativeBridge.cpp
    ${WNB_DIR}/src/NativeImports.cpp
    ${WNB_DIR}/src/SignatureScanner.cpp)
target_include_directories(WitcherNativeBridge PUBLIC ${WNB_DIR}/include)
target_compile_definitions(WitcherNativeBridge PRIVATE WITCHER_NATIVE_BRIDGE_EXPORTS WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(WitcherNativeBridge PROPERTIES SUFFIX ".asi")

# --- shader do overlay: compilado no build pelo fxc do Windows SDK (DXBC vs_5_0/ps_5_0) ------
set(MW3_SDK_VER "${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}")
find_program(MW3_FXC fxc
    HINTS "$ENV{WindowsSdkDir}bin/${MW3_SDK_VER}/x64"
          "C:/Program Files (x86)/Windows Kits/10/bin/${MW3_SDK_VER}/x64"
          "C:/Program Files (x86)/Windows Kits/10/bin/10.0.22621.0/x64"
    REQUIRED)
set(MW3_SHADER_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${MW3_SHADER_DIR})
set(MW3_OVERLAY_HLSL ${CMAKE_CURRENT_SOURCE_DIR}/src/overlay.hlsl)
add_custom_command(OUTPUT ${MW3_SHADER_DIR}/overlay_vs.h ${MW3_SHADER_DIR}/overlay_ps.h
    COMMAND ${MW3_FXC} /nologo /WX /O3 /T vs_5_0 /E VSMain /Vn kOverlayVS /Fh ${MW3_SHADER_DIR}/overlay_vs.h ${MW3_OVERLAY_HLSL}
    COMMAND ${MW3_FXC} /nologo /WX /O3 /T ps_5_0 /E PSMain /Vn kOverlayPS /Fh ${MW3_SHADER_DIR}/overlay_ps.h ${MW3_OVERLAY_HLSL}
    DEPENDS ${MW3_OVERLAY_HLSL}
    COMMENT "fxc overlay.hlsl"
    VERBATIM)
# Um alvo so roda o fxc (dois alvos com o mesmo OUTPUT no gerador do VS rodariam em paralelo).
add_custom_target(mw3_overlay_shaders DEPENDS ${MW3_SHADER_DIR}/overlay_vs.h ${MW3_SHADER_DIR}/overlay_ps.h)
set(MW3_OVERLAY_SOURCES src/overlay.cpp)

# --- colisao Stage A + fantoche (native v4) ---------------------------------------------
set(MW3_WORLD_SOURCES src/collision.cpp src/collision_mesh.cpp src/natives_world.cpp)
# --- combate W3 <-> MC (native v6) -------------------------------------------------------
set(MW3_COMBAT_SOURCES src/combat.cpp src/natives_combat.cpp)

# --- mw3.asi: ponte do MineWitcher3 --------------------------------------------------
add_library(mw3 SHARED
    ${MW3_OVERLAY_SOURCES}
    src/overlay_game.cpp
    src/config.cpp
    src/dllmain.cpp
    src/input_hook.cpp
    src/launcher.cpp
    src/link.cpp
    src/log.cpp
    src/natives.cpp
    src/session.cpp
    src/motion.cpp
    ${MW3_WORLD_SOURCES}
    ${MW3_COMBAT_SOURCES})
target_link_libraries(mw3 PRIVATE WitcherNativeBridge advapi32 ole32 oleaut32 shell32 user32)
target_compile_definitions(mw3 PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(mw3 PRIVATE /W4 /utf-8)
set_target_properties(mw3 PROPERTIES SUFFIX ".asi")
# d3d12/dxgi sao carregados do System32 em runtime (overlay.cpp); nada de d3d12.lib/dxgi.lib aqui.
target_include_directories(mw3 PRIVATE ${MW3_SHADER_DIR})
add_dependencies(mw3 mw3_overlay_shaders)

# --- mw3_linktest.exe: teste de console do link (NAO e instalado no jogo) -------------------
# Cria o mapeamento como o jogo, faz o papel do MC no McState e confere o layout com os offsets que
# o tools/fake_witcher.py e o Proto.java fixam. Rodar: native/build/Release/mw3_linktest.exe
add_executable(mw3_linktest
    test/linktest.cpp
    src/link.cpp
    src/log.cpp)
target_compile_definitions(mw3_linktest PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(mw3_linktest PRIVATE /W4 /utf-8)
target_link_libraries(mw3_linktest PRIVATE advapi32)

# --- mw3_coltest.exe: teste da colisao Stage A (NAO e instalado no jogo) -------------------------
# Amostras sinteticas -> collision_mesh -> anel de colisao real do link; le o anel como o
# W3Collision.java (drainOnce/readTris/readRegion) e confere chao, paredes, tetos e o hold do MC.
add_executable(mw3_coltest
    test/coltest.cpp
    src/collision.cpp
    src/collision_mesh.cpp
    src/link.cpp
    src/log.cpp)
target_compile_definitions(mw3_coltest PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(mw3_coltest PRIVATE /W4 /utf-8)
target_link_libraries(mw3_coltest PRIVATE advapi32)

# --- mw3_overlaytest.exe: teste do overlay D3D12 (NAO e instalado no jogo) --------------------------
# Janela + swapchain D3D12 proprias, hooks instalados como no jogo, frame sintetico (xadrez com alfa)
# pelo triple buffer do link; confere o back buffer por readback. Rodar: native/build-*/Release/mw3_overlaytest.exe
add_executable(mw3_overlaytest
    test/overlaytest.cpp
    ${MW3_OVERLAY_SOURCES}
    src/link.cpp
    src/log.cpp)
target_include_directories(mw3_overlaytest PRIVATE ${MW3_SHADER_DIR})
add_dependencies(mw3_overlaytest mw3_overlay_shaders)
target_compile_definitions(mw3_overlaytest PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(mw3_overlaytest PRIVATE /W4 /utf-8)
target_link_libraries(mw3_overlaytest PRIVATE advapi32 user32 d3d12 dxgi)

# --- mw3_motiontest.exe: teste da interpolacao dos ticks do MC (NAO e instalado no jogo) ------------
# MC sintetico a 20 Hz (stamps ruidosos, chegada atrasada, ticks perdidos, teleporte) contra frames
# do jogo com jitter: a pose por frame tem de andar com velocidade constante.
add_executable(mw3_motiontest
    test/motiontest.cpp
    src/motion.cpp)
target_compile_options(mw3_motiontest PRIVATE /W4 /utf-8)

# --- mw3_combattest.exe: teste do combate (native v6; NAO e instalado no jogo) ----------------------
# Atores -> tabela @0x12000 lida byte a byte como o WitcherLink.readActors (Java); kEvHitActor escrito
# no anel como o WitcherLink.pushEvent -> fila de golpes; kInHurt lido do anel de input como o InputBridge.
add_executable(mw3_combattest
    test/combattest.cpp
    src/combat.cpp
    src/link.cpp
    src/log.cpp)
target_compile_definitions(mw3_combattest PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(mw3_combattest PRIVATE /W4 /utf-8)
target_link_libraries(mw3_combattest PRIVATE advapi32)

// Derived from SkyCraft (https://github.com/chasmlol/skycraft), Copyright (c) 2026 chasmlol, MIT License.
// Port of the Launcher part of skse/src/Game.h (upstream commit bfcaf17). See THIRD-PARTY-NOTICES.md.
#pragma once

namespace mw3::launcher
{
	enum class Status
	{
		kOff,         // nao iniciado pelo MW3 (bStartMinecraft = 0)
		kRunning,     // ja estava rodando
		kStarting,    // sendo iniciado
		kNoLauncher,  // nada com que inicia-lo (Prism portatil nao instalado)
		kFailed,      // falhou ao iniciar
	};

	// Na carga do mw3.asi: inicia o Minecraft (conforme mw3.ini) se ele ainda nao estiver rodando.
	// Nunca bloqueia nem derruba o jogo: sem Prism so loga e o W3 segue normal.
	void StartMinecraft();
	Status GetStatus();

	// Um Minecraft com o mod MineWitcher3 esta rodando (segura o mutex kRunningMutexName desde cedo).
	bool MinecraftRunning();
} // namespace mw3::launcher

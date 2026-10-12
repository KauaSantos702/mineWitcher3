#pragma once

namespace mw3
{
	// Subclassa a WndProc da janela do jogo (classe W2ViewportClass, ver research/03 secao 3).
	// Roda numa thread propria: espera a janela existir e instala o hook.
	//
	// Sem Minecraft linkado: so o F5 e engolido (o motor usa F5 fixo para quicksave) e contado para o
	// script (MW3_PollCameraKey). Com o MC linkado e em gameplay (session::RouteInput), teclado e mouse
	// vao para o anel de input do MC e o W3 nao os ve, exceto as teclas de menu do W3 (Esc, I, M, J, K,
	// F6-F9, console) e as solturas de tecla/botao.
	void StartInputHook();

	// Quantas vezes o F5 foi apertado desde a ultima chamada (so conta sem MC linkado).
	int ConsumeCameraKeyPresses();

	// Tamanho da area cliente da janela do jogo (pixels). false se a janela ainda nao foi achada.
	bool GetViewportSize(int& width, int& height);
} // namespace mw3

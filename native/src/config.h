#pragma once

#include <string>

namespace mw3::config
{
	// mw3.ini ao lado do witcher3.exe (bin\x64_dx12\mw3.ini). Opcional: sem o arquivo valem os padroes.
	//
	//   [Minecraft]
	//   bStartMinecraft=1                 ; iniciar o Minecraft junto com o jogo
	//   sLauncher=                        ; vazio = %LOCALAPPDATA%\MineWitcher3\Prism\prismlauncher.exe
	//   sArguments=--launch MineWitcher3
	//   [Input]
	//   bForwardInput=1                   ; 0 = nunca mandar teclado/mouse ao MC (depuracao)
	//   [Collision]
	//   iTracesPerFrame=200               ; StaticTrace por frame (16..4000)
	//   bCapture=0                        ; 1 = mw3_colcapture.bin (colunas tracadas, ate 50 MB; coltest --replay)
	//   [Overlay]
	//   bEnable=1                         ; 0 = nao desenhar mao/HUD/telas do MC (sem hook de Present)
	//   iHdrPaperWhiteNits=200            ; brilho do overlay quando o jogo esta em HDR
	//   [Motion]
	//   bInterpolate=1                    ; 0 = camera/fantoche nos pes do frame do MC (sem interpolar os ticks)
	//   [Render]
	//   bStreamWorld=0                    ; 1 = o MC manda mundo/atlas/avatar pelo anel de render mesmo sem renderer
	std::wstring IniPath();
	bool         IniExists();
	int          GetInt(const wchar_t* section, const wchar_t* key, int fallback);
	std::wstring GetString(const wchar_t* section, const wchar_t* key, const wchar_t* fallback);

	// [Overlay]
	bool OverlayEnabled();
	int  OverlayPaperWhiteNits();

	std::string  Narrow(const std::wstring& text);
	std::wstring ExpandEnv(const std::wstring& path);
} // namespace mw3::config

# Minecraft dedicado do MineWitcher3 (Prism Launcher portátil)

O MineWitcher3 roda um cliente Minecraft escondido ao lado do The Witcher 3. Esse Minecraft é uma
instalação **separada** (como no SkyCraft): um Prism Launcher portátil com uma instância limpa só
com Fabric API + o nosso mod. O `.minecraft` normal (`%APPDATA%\.minecraft`, com Iris) **não é
tocado**.

| O quê | Onde |
|---|---|
| Prism Launcher 11.1.1 (portátil, sem modificação) | `%LOCALAPPDATA%\MineWitcher3\Prism\prismlauncher.exe` |
| Instância `MineWitcher3` (MC 26.3, Fabric Loader 0.19.5) | `%LOCALAPPDATA%\MineWitcher3\Prism\instances\MineWitcher3\` |
| Mods | `...\instances\MineWitcher3\.minecraft\mods\` (`fabric-api-0.161.0+26.3.jar`, `minewitcher3-0.1.0.jar`) |
| Logs do Minecraft | `...\instances\MineWitcher3\.minecraft\logs\latest.log` |
| Logs do Prism | `%LOCALAPPDATA%\MineWitcher3\Prism\logs\` |

O lado nativo (`mw3.asi`) inicia com:

```bat
"%LOCALAPPDATA%\MineWitcher3\Prism\prismlauncher.exe" --launch MineWitcher3
```

## 1. Rodar o setup

```powershell
powershell -ExecutionPolicy Bypass -File C:\codes\rea\mineWitcher3\tools\setup_minecraft.ps1
```

O script:

- compila o mod (`fabric\gradle25.cmd build`) se `fabric\build\libs\minewitcher3-0.1.0.jar` não
  existir (ou com `-Rebuild`);
- baixa o Prism Launcher 11.1.1 portátil (Windows x64 MSVC) do GitHub oficial numa pasta temporária
  nova e vazia, confere o SHA-256 fixado e extrai em `%LOCALAPPDATA%\MineWitcher3\Prism`
  (nada baixado é executado);
- grava as opções do Prism (iguais às do SkyCraft: sem aviso de "pouca RAM", sem checagem automática
  de atualização, fecha o Prism depois de iniciar o jogo e sai quando o jogo fecha; o Prism não tem
  telemetria);
- grava a instância: `mmc-pack.json` (LWJGL 3.4.3, Minecraft 26.3, Intermediary 26.3, Fabric Loader
  0.19.5), `instance.cfg` com
  `--enable-native-access=ALL-UNNAMED -Dminewitcher3.startHidden=true`, heap de 512 MB a 3072 MB e o
  Java 25 do Temurin (`C:\Program Files\Eclipse Adoptium\jdk-25*`; com `-NoJavaOverride` o Prism
  escolhe/baixa o Java sozinho);
- baixa a Fabric API 0.161.0+26.3 pela API do Modrinth (versão fixada, confere o SHA-512 que o
  Modrinth informa) e copia ela e o nosso jar para `mods`.

Pode rodar de novo quando quiser (por exemplo depois de recompilar o mod): ele mantém os dados do
Prism (conta Microsoft, jogo baixado, mundos, `options.txt`, logs) e só troca os nossos jars e as
nossas chaves de configuração. Feche o Prism/Minecraft da instância antes (o script recusa rodar se
estiverem abertos).

## 2. Passo manual (uma vez só)

1. Abra `%LOCALAPPDATA%\MineWitcher3\Prism\prismlauncher.exe`.
2. Adicione a sua conta Microsoft (o assistente do Prism pede na primeira vez; ou *Accounts →
   Manage Accounts → Add Microsoft*). A conta precisa ter o Minecraft: Java Edition. Ninguém além de
   você digita a senha.
3. Opcional, para conferir que o mod carrega: clique com o botão direito na instância
   **MineWitcher3 → Edit → Settings → Java arguments** e troque temporariamente
   `-Dminewitcher3.startHidden=true` por `-Dminewitcher3.showWindow=true`, inicie a instância uma vez
   (o Prism baixa o Minecraft 26.3 e as bibliotecas) e veja no `latest.log` as linhas do
   `minewitcher3`. Depois feche e volte o argumento (ou rode o setup de novo, que restaura os
   argumentos certos).

## 3. Testar o lado Minecraft sozinho (sem o Witcher 3)

O `tools\fake_witcher.py` faz o papel do `mw3.asi`: cria a memória compartilhada
`Local\MineWitcher3_v1`, manda um chão + escada de colisão, controla o jogador com input de teste
(soca um NPC falso, leva dano, coloca bloco, atira flecha, larga e pega item), mostra o estado que o MC
reporta e salva o overlay em PNG.

```powershell
# só confere o layout do protocolo (header C, Proto.java e o script)
python C:\codes\rea\mineWitcher3\tools\fake_witcher.py --check

# teste completo: [segundos] [overlay.png] [atlas.png]  (padrão: 90 s, overlay.png)
python C:\codes\rea\mineWitcher3\tools\fake_witcher.py 90 overlay.png
```

Com o script rodando (ele espera o Minecraft aparecer), inicie a instância em outro terminal:

```bat
"%LOCALAPPDATA%\MineWitcher3\Prism\prismlauncher.exe" --launch MineWitcher3
```

O MC sai sozinho quando o "jogo" some; para testes longos use `-Dminewitcher3.quitWithGame=false`
nos argumentos da instância. Para ver a janela, `-Dminewitcher3.showWindow=true`. Saída esperada do
script: `MC in world at ...`, contagens de entidades e `saved overlay ...`. Se algo falhar, veja
`...\instances\MineWitcher3\.minecraft\logs\latest.log`.

## Versões fixadas

| Componente | Versão | Verificação |
|---|---|---|
| Prism Launcher | 11.1.1, `PrismLauncher-Windows-MSVC-Portable-11.1.1.zip` | SHA-256 `ab35a770fb06d89d2ccc098079db5db329fb4e68f42b72babd8b095efde3d2d7` (digest publicado pelo GitHub) |
| Fabric API | 0.161.0+26.3 (Modrinth `P7dR8mSH` / versão `bNnaTiuM`) | SHA-512 `ed6b2586…12da109d` (o do Modrinth, conferido também com o valor fixado) |
| Minecraft / LWJGL / Intermediary / Fabric Loader | 26.3 / 3.4.3 / 26.3 / 0.19.5 | componentes do meta do Prism |

Para atualizar, troque as constantes no topo de `setup_minecraft.ps1` (e `fabric_api_version` em
`fabric\gradle.properties`).

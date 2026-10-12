# 02 — Natives de WitcherScript e ponte para DLL ("script extender")

Alvo: `bin\x64_dx12\witcher3.exe` v5.0.0.1048522 (SHA-256 2DF76539…1FB101), ImageBase 0x140000000.
Todas as addresses abaixo são **RVAs** (somar ImageBase / base do módulo em runtime).
Método: análise estática somente leitura (pefile + capstone + varredura de bytes). Nada foi executado/injetado.

Legenda: **[O]** observado no binário · **[I]** inferido · **[?]** desconhecido / precisa runtime ou Ghidra.

Scripts usados (em `C:\codes\rea\mineWitcher3\tools\scripts\natives\`):

| script | função |
|---|---|
| `find_native_xrefs.py` | acha a string do nome e os LEA RIP-relative que a referenciam |
| `disasm.py` | disassembly de uma janela numa RVA, anotando strings |
| `scan_reg.py` | conta as chamadas aos registradores, extrai os pares (nome, fn) e as strings de RTTI MSVC |
| `global_natives.txt` | saída: 394 sites `call_site  nome  fnRVA` |
| `find_refs.py` | xrefs `call`/`jmp` E8/E9, LEA e ponteiros absolutos em `.data`/`.rdata` |
| `find_indirect.py` | xrefs `call [rip+x]` e `mov r,[rip+x]` |
| `pdata_func.py` | função que contém uma RVA (via `.pdata`, seguindo chained unwind) |
| `grep_strings.py` | grep em strings ASCII e UTF-16 |
| `read_ptr.py` | lê qword ou slot de IAT |
| `check_patterns.py` | confere prólogos e a assinatura do WNB |
| `list_imports.py` | lista as DLLs importadas pelo exe |

---

## 1. Como `import function X` é ligado ao C++

### 1.1 Registro de natives globais — padrão de código [O]

Cada native global é registrado por uma sequência inline idêntica (exemplo `FloorF`, site 0x27b485a):

```
mov  edx, 0F8h                ; sizeof(CFunction) = 0xF8
lea  rcx, [rbp-10h]
mov  r8d, 10h                 ; alinhamento 16
call 0xBCB710                 ; Memory alloc -> {ptr,size} em [rbp-10h]
mov  rdi, [rbp-10h]
xor  edx, edx
mov  r8, [rbp-8]
call 0x28CBDB0                ; memset(ptr, 0, size)            [I]
call 0x31C440                 ; CNamesPool::GetInstance()        [I nome]
lea  rdx, "FloorF"            ; 0x363A238 (ASCII, .rdata)
mov  rcx, rax
call 0x26B90A0                ; CNamesPool::AddName(char*) -> eax = índice CName
lea  r8, 0x27B2880            ; <- ponteiro da implementação nativa
mov  [rbp+10h], eax
lea  rdx, [rbp+10h]           ; &CName
mov  rcx, rdi
call 0x26FDDA0                ; CFunction::CFunction(CName const&, NativeFn)  (global)
call 0x31E800                 ; SRTTI::GetInstance() -> CRTTISystem*
mov  rdx, rdi
mov  rcx, rax
call 0x26CE2D0                ; CRTTISystem::RegisterGlobalFunction(CFunction*)
```

Strings e xrefs (LEA RIP-relative a partir de `.text`) [O]:

| native | string RVA | xref (LEA) | implementação |
|---|---|---|---|
| FloorF | 0x363A238 | 0x27B488A | 0x27B2880 |
| VecDistance | 0x363A2E8 | 0x27B5034 | 0x27B18D0 |
| LogChannel | 0x362ED68 | 0x2745B9B | 0x2745340 |
| Log | 0x362ED74 | 0x2745B2E | 0x2745420 |
| LoadCSV | — | 0x2289309 | 0x26F7690 |
| StaticTrace (método) | 0x395BF78 | 0x896E50 | thunk em 0x22A3640 (via struct) |
| CreateEntity (método) | 0x39540B0 | 0x87A4D8 | — |
| GetWorldPosition (método) | 0x38AAC40 | 0x6C34A5, 0x8718EB | — |
| AddTimer (método) | 0x39516E0 | 0x86C2E2 | — |

Estatísticas [O] (contagem de `call rel32`):

| alvo | sites | interpretação |
|---|---|---|
| 0x26FDDA0 | 394 | ctor de CFunction **global** |
| 0x26CE2D0 | 355 | RegisterGlobalFunction (as funções de script também passam por ele, ver 1.4) |
| 0x26FDA50 | 2467 | ctor de CFunction de **método** (classe) |
| 0x26C2FD0 | 419 | CClass::AddNativeFunction / AddFunction [I] |
| 0x26B90A0 | 18077 | AddName / CName(const char*) |
| 0x31E800 | 16519 | SRTTI::GetInstance |

### 1.2 Quando o registro acontece [O]

- 0x2288F80 = **InitializeRTTI** (loga `"Initializing RTTI..."` em 0x35C72D0). Ele chama, entre outras, 0x27454E0 (core: `Log`, `LogChannel`… e chama 0x27B3B80 = 97 natives math/vector e 0x27ACB80), 0x1F3B280, 0x2754170 e 0x22B8A60. As funções têm múltiplos chunks em `.pdata`; o corpo vai de 0x2288F91 a 0x228976C.
- InitializeRTTI é chamado em 0x228AAE9, dentro da função de init do engine 0x228A3D0. Antes dele há o parse de `-debugsaves`/`-savehack`; logo depois, `"RTTI"`. 0x228A3D0 é referenciado por call em 0x1D69274 e pela vtable em 0x3950AC8.
- Ou seja, registro e compilação acontecem no boot, antes da compilação de scripts (`"Compiling scripts (finding script files)..."` em 0x3632F80) [I quanto à ordem exata; pela lógica tem que ser assim, porque o compilador consulta o mapa].

### 1.3 Estrutura de CFunction (parcial) [O]

O ctor global 0x26FDDA0 é `(this, CName const* name, NativeFn fn)`:

| offset | conteúdo |
|---|---|
| +0x00 | vtable 0x3A66DB0 |
| +0x08 | CClass* owner (0 para global; no ctor de método vem em rdx) |
| +0x10 | CName (u32) |
| +0x18 | flags: global = 3, método = 1. bit0 = native, testado pelo compilador (`test byte [rsi+18h],1`); bit1 = global/static [I] |
| +0x60 | sub-objeto inicializado por 0x276DD00 (código/params [I]) |
| +0xE0 | índice do native na tabela global |
| +0xF0 | 0 |
| tamanho | 0xF8 |

- Natives globais: `GNativeGlobalFuncs[idx] = fn`, com a tabela em `.bss` 0x5A75A70 (0x8000 bytes = 4096 slots) e o contador em `[ [0x5A7DA70] + 0x88 ]`.
- **A mesma tabela é usada pelos opcodes de bytecode**: o leitor de parâmetro indexa 0x5A75A70 pelo byte de opcode [O]. Então os opcodes ocupam os índices baixos e os natives são anexados depois [I].
- Natives de método: tabela 0x5A5DA70 com entradas de 24 bytes (ponteiro de membro + ajuste, copiado com `vmovups/vmovsd`) [O]. Para a ponte, **use só funções globais**.

### 1.4 O compilador resolve o import por nome [O]

Função do compilador perto de 0x2784BD0–0x2784F26 (strings em 0x3633xxx):

- Faz `SRTTI::GetInstance()` e procura **inline** no hash map de funções globais: `[rtti+0x40]` = nº de buckets, `[rtti+0x44]` = count, `[rtti+0x60]` = buckets. Cada nó é `{+0 key CName, +8 CFunction*, +0x10 hash, +0x18 next}`.
- Não achou → `"Cannot find global function '%s' to import."` (0x3633690), **erro de compilação**: aparece o diálogo "Script Compilation Errors".
- Achou, mas flag native = 0 → `"Cannot import non-native global function '%s'."`.
- Achou um native que já foi importado → `"Global native function '%s' was already exported."`.
- Funções globais definidas em script também são criadas e registradas pelo mesmo `RegisterGlobalFunction` (call em 0x2784E7E).
- **Conclusão**: basta o nome estar no mapa (com flag native) antes de compilar. **A assinatura (params/retorno) vem da declaração `import function` no .ws**; o C++ fornece só nome + ponteiro [O no ctor, que não recebe tipos; I quanto ao compilador preencher os params].

### 1.5 Convenção de chamada do native e `CScriptStackFrame` [O]

```cpp
// x64 MSVC: rcx, rdx, r8
void Native(IScriptable* context, CScriptStackFrame& stack, void* result);
```

`FloorF` em 0x27B2880 (o original é `int FloorF(float)`):

```
mov  rax, [rdx+30h]             ; stack.m_code (u8* bytecode IP)
movzx ecx, byte [rax]           ; opcode
inc  rax ; mov [rdx+30h], rax
mov  r9, [0x5A75A70 + rcx*8]    ; handler
mov  rcx, [rdx]                 ; stack.m_context (+0x00)
lea  r8, [rsp+38h]              ; destino do parâmetro (float)
call r9                         ; handler(context, &stack, &dst)  == GET_PARAMETER
inc  qword [rbx+30h]            ; FINISH_PARAMETERS (pula o opcode ParamEnd)
test rdi, rdi ; jz              ; result pode ser NULL (retorno ignorado)
vroundss ... ; vcvttss2si ; mov [rdi], eax   ; *(int*)result = floor(x)
```

- O layout de `CScriptStackFrame` conhecido é parcial: +0x00 context, +0x30 code ptr. O resto fica para Ghidra/runtime [?].
- Macros equivalentes (estilo REDengine 3 / W2):

  ```cpp
  GET_PARAMETER(T, v, def) {
      T v = def;
      u8 op = *stack.code++;
      if (tbl[op]) tbl[op](stack.ctx, &stack, &v);
  }
  FINISH_PARAMETERS: stack.code++;
  ```

  É preciso ler **todos** os params, na ordem, e depois avançar +1. Caso contrário o bytecode dessincroniza e o jogo crasha.
- Tipos simples são seguros: `int` 4 B, `float` 4 B, `bool` 1 B, `name` = CName u32.
- `String`: buffer + tamanho. O default vazio é construído por 0x31CD30 + 0x28CB710 a partir de 0x5E60490/0x5E60498, e o buffer é liberado com 0xBCAA70 [O]. Segundo o WitcherNativeBridge, no next-gen o `String` guarda **UTF-8** com tamanho que inclui o terminador [I, não verificado].

---

## 2. Prior art (pesquisa web)

| Projeto | Licença | Versão suportada | Relevância |
|---|---|---|---|
| **WitcherNativeBridge** (rejuvenate7) — github.com/rejuvenate7/WitcherNativeBridge, Nexus 13743 | **MIT** | Builds separados para **4.04** e **Remastered "v5.00+"** (exemplos `Example4.04` / `ExampleRemastered`) | **Exatamente o que precisamos.** É um `.asi` (Ultimate ASI Loader). API: `WNB_RegisterNative("Nome", fn)`, `WNB_Read{Int,Bool,Float,String,Name}Parameter(frame)`, `WNB_Return*(frame,result,v)`. Callback `void fn(void*, void* frame, void* result)`. Detalhes abaixo. |
| **TW3SE – The Witcher 3 Script Extender** (Nexus 13837) | ? (página deu 403) | changelog 0.1.1 cita **patch 5.01** | Adiciona funções de script e plugins C++. Não verifiquei fonte nem licença. |
| **RED3ext / RED3lib** (wopss) | MIT | não declarada | Pela descrição, é o "RED4ext para W3". Sem releases. Versão alvo [?]. |
| **w3m** (momo5502) — "Witcher 3: Multiplayer" | **GPL-3.0** | addresses fixos (ctor CFunction 0x13DB730, register 0x13AF000…), que **não batem com este exe** → outra build (provavelmente 4.0x) [I] | Bom modelo de arquitetura: `scripting.cpp` registra natives fazendo hook num call-site depois do registro original; também tem `scripting_filesystem` (injeta .ws). Licença GPL: só como referência. |
| WitcherMMO (eofim), Witcher3-MP (Hyjarion) | ? | ? | Multiplayer com cliente nativo e bridge. Não analisados. |
| RED4ext (Cyberpunk) | MIT | — | Design de RTTI parecido (CRTTISystem, CStackFrame, `code` ptr + opcode table). Serve de referência conceitual. |

Como o WNB funciona (lido do `src/NativeBridge.cpp`) e o que **confere** com este exe:

- Faz pattern scan da sequência `BA F8 00 00 00 48 8D 4D F0 41 B8 10 00 00 00 E8 …` e exige ≥ 32 matches com consenso dos alvos (alloc, memset, name pool, AddName, ctor, SRTTI, RegisterGlobal). **Neste exe o prefixo casa 324 vezes** [O], o mesmo bloco mostrado em 1.1.
- Faz inline hook **só** em RegisterGlobalFunction, checando um prólogo de 15 bytes. Aqui, em 0x26CE2D0, o prólogo é `48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20` (exatamente 15 bytes) [O]. No primeiro call, roda o original e faz o flush dos registros da fila.
- Acha a opcode table (0x5A75A70) pelo LEA dentro de `LogChannel` [O: o LEA existe em 0x2745350].
- Frame: `+0x0` context, `+0x30` IP. O return avança o IP em 1. Tudo igual ao observado [O].

O exe importa `dinput8.dll`, que **não existe** na pasta `x64_dx12` [O] → o Ultimate ASI Loader como `dinput8.dll` é carregado como import estático, antes de qualquer init do engine.

---

## 3. Plano da ponte (ranking por esforço × robustez)

### Opção A — Usar o WitcherNativeBridge (MIT) como dependência ou vendorizado ★ recomendada para começar

- **Esforço**: baixo (horas). **Robustez**: alta. É pattern-based, tem versão Remastered, e o código já resolve SEH e strings.
- Passos:
  1. Ultimate ASI Loader (`dinput8.dll`) + `WitcherNativeBridge.asi` + `MineWitcher3.asi` em `bin\x64_dx12`.
  2. No nosso DllMain (ou numa thread de init), chamar `WNB_RegisterNative("MW3_SendPlayerState", &MW3_SendPlayerState)` etc. Fica na fila até o primeiro RegisterGlobalFunction.
  3. No `.ws`:

     ```
     import function MW3_SendPlayerState(x : float, y : float, z : float, yaw : float) : bool;
     import function MW3_PollCommand() : int;
     import function MW3_GetFloat(slot : int) : float;
     ```

  4. A implementação lê os params com `WNB_ReadFloatParameter` e escreve/lê o shared memory com o cliente Minecraft.
- Risco: se o `.asi` não carregar, o jogo dá **erro de compilação** ("Cannot find global function…"). Mitigação: pôr os imports num `.ws` separado, que o instalador só copia junto com a DLL.

### Opção B — Implementação própria, mesmos 6 calls (sem dependência)

- **Esforço**: médio (1–2 dias). **Robustez**: média-alta, se usar pattern scan. Com RVA fixa, só para esta build (validar o SHA-256 antes de aplicar).
- Hook (MinHook, trampoline) em `CRTTISystem::RegisterGlobalFunction` (0x26CE2D0) ou no fim de `InitializeRTTI` (0x2288F80, prólogo `48 89 4C 24 08 55 53 57 48 8B EC 48 83 EC 40 33 FF`). Na primeira entrada, depois do original:

  ```cpp
  struct AllocRes { void* p; size_t sz; };
  AllocRes r; Alloc(&r, 0xF8, 0x10);              // 0xBCB710 (assinatura exata [I])
  memset(r.p, 0, 0xF8);
  uint32_t nm = AddName(GetNamesPool(), "MW3_SendPlayerState");  // 0x31C440 / 0x26B90A0
  CFunction_ctorGlobal(r.p, &nm, &MW3_SendPlayerState);           // 0x26FDDA0
  RegisterGlobalFunction(SRTTI_Get(), r.p);                       // 0x31E800 / 0x26CE2D0
  ```

- O native segue o padrão de 1.5. Ler param = chamar `tbl[*ip++](ctx, &frame, &dst)`, onde `tbl = base + 0x5A75A70`. Depois `ip++`, e por fim `if(result) *(T*)result = v`.
- **Restrinja a ABI a int/float/bool/name.** Dados grandes (estado do jogador, comandos) passam pelo shared memory, não por `String`.

### Opção C — Canal "low-tech" via `Log`/`LogChannel` (one-way, script → DLL) ★ fallback / prova de conceito

- **Achado importante [O]**: no build retail, `Log` (0x2745420) e `LogChannel` (0x2745340) avaliam os argumentos e chamam `call qword ptr [0x405BAA0]`. Esse slot fica em **`.data`** (gravável) e aponta para 0x27454D0, que é um stub `ret 0`. São **só 2 call sites** no exe (0x27453FA e 0x27454AC), ambos nesses natives.
- Trocar esse ponteiro por um callback nosso: **sem patch de código, sem trampoline**. Assinatura [I pelos registradores]:

  ```cpp
  void OnScriptLog(const CName* channel, const String* msg);
  ```

  - `LogChannel`: rcx = &CName (o canal), rdx = &String.
  - `Log`: rcx = &CName padrão (0x5C79214), rdx = &String.
- Uso no script: `LogChannel('MW3', "pos " + FloatToString(x) + …)`. A DLL filtra pelo índice de `CName("MW3")` e faz o parse do texto.
- Vantagens: degrada bem, porque **sem a DLL o script compila e roda normalmente** (o log vira no-op) e não depende de registro antes da compilação. Desvantagens: só ida, custo de formatar string por frame, layout de `String` ainda [?] (UTF-8 vs UTF-16; testar em runtime).
- Volta (DLL → script): precisaria da opção A/B, ou de hook em algum native "getter". Por isso C serve como PoC, e não como solução final.

### Opção D — Arquivos / exec functions

- `exec function` só é chamável pelo console ("Cannot call exec function '%s' from scripts…" em 0x363BFB0). Fazer a DLL chamar uma exec exigiria invocar o executor de console. Script lendo arquivo arbitrário: só via `LoadCSV`/C2dArray de recursos. **Não recomendado.**

**Recomendação**: A (ou B, se quisermos zero dependências) para a ponte bidirecional definitiva. C serve como PoC de 30 minutos e canal de debug. Em qualquer caso, manter as natives em funções **globais** com tipos primitivos e trocar dados volumosos pelo shared memory.

---

## 4. RTTI MSVC [O]

- **Sim**: há 9131 type descriptors `.?AV…@@` / `.?AU…@@` em `.data`. Isso permite achar vtables via COL (Complete Object Locator) e nomear classes no Ghidra (plugin RTTI/"Recover Classes").

| Classe | `.?AV` presente? | observação |
|---|---|---|
| CFunction | sim | vtable usada no ctor: 0x3A66DB0 [O] |
| CClass | sim | |
| CR4Game | sim | |
| CR4Player | sim | |
| IScriptable, CObject, CGame, CPlayer, CEntity, CActor, CNewNPC, CProperty, CScriptThread, CScriptingSystem | sim | |
| **CScriptStackFrame** | **não** | não é polimórfica, logo sem RTTI |
| **CRTTISystem** | **não** | singleton sem vtable (acessado via 0x31E800) |
| CNamesPool, CScriptCompiler | não | |

- Também existem os builders `…ClassBuilder` (p.ex. `CR4TelemetryScriptProxyClassBuilder`), que registram natives de classe por objeto estático.

---

## 5. Lacunas (precisam de Ghidra ou runtime)

1. Layout completo de `CScriptStackFrame` (só +0x00 e +0x30 confirmados) e de `String` no next-gen (UTF-8 vs UTF-16, campos de tamanho/capacidade).
2. Assinatura exata do alocador 0xBCB710 (retorna par `{ptr,size}` por out-param; o terceiro argumento é o alinhamento) e se o engine libera os CFunction no shutdown (usar o próprio alocador é mais seguro).
3. Confirmar em runtime que InitializeRTTI roda **antes** de "Compiling scripts" e que o cache `scripts.redscriptsblob` recompila ou relinka ao adicionar imports.
4. Passar objetos (`CActor`/handles) como parâmetro de native: o formato do handle não foi analisado. Por ora, evitar.
5. Compatibilidade real do WNB com 5.0.0.1048522: os padrões casam estaticamente, mas falta testar em runtime. Também falta verificar fonte, licença e versão do TW3SE (Nexus deu 403).
6. Verificar se o Ultimate ASI Loader como `dinput8.dll` é aceito sem conflito com o REDlauncher/Galaxy. É provável, mas não foi testado.

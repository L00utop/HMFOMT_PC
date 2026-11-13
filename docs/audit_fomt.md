# Auditoria Técnica Fase 1: Repositório `StanHash/fomt`

> Documento produzido na Fase 1 após clonagem local de `https://github.com/StanHash/fomt` em `external/fomt` e inspeção direta dos arquivos de build, linker script, código-fonte e assembly.

---

## 1. Ambiente de Compilação Nativo (Windows x86_64)

Instalado em espaço de usuário via `winget` + **MSYS2 UCRT64** (`%LOCALAPPDATA%\msys64`):

| Ferramenta / Biblioteca | Versão Verificada | Finalidade |
| :--- | :--- | :--- |
| `gcc` / `g++` (MinGW-w64 UCRT64) | `16.2.0` | Compilação nativa C99 / C++17 (x86_64) |
| `cmake` | `4.4.5` | Orquestração de build e CTest |
| `ninja` | `1.13.2` | Executor rápido de compilação |
| `SDL2` | `2.32.10` | Janela, framebuffer, áudio (`SDL_AudioCallback`) e input |
| `git` | `2.56.1` | Gestão do repositório de referência |
| `ws2_32` (Winsock2 SDK) | Nativo MinGW UCRT64 | Sockets UDP não-bloqueantes para Multiplayer |

---

## 2. Inventário de Código-Fonte e Composição (`external/fomt`)

Medição direta na árvore clonada em `external/fomt`:

| Diretório | Tamanho Total (bytes) | Conteúdo Principal |
| :--- | :---: | :--- |
| `src/` | `299.496` | Classes de domínio em C++ (`*.cc`), driver de áudio `m4a.c` / `m4a_1.s` e boot `crt0.s` |
| `include/` | `91.421` | Cabeçalhos C++ (`*.hh`), I/O do GBA (`gbaio.h`), SVCs BIOS (`gbasvc.h`), `utility/` |
| `asm/` (total incl. `asm/data/`) | `7.713.435` | Rotinas ARM/Thumb não-descompiladas (`*.s`) + inclusões de dados |
| `asm/data/` | `101.168` | Tabelas e diretivas `.incbin "baserom.gba", offset, len` |
| `data/` | `44.867` | Tabelas macro (`item/tool.def`, `food.def`, `article.def`, `product.def`) e áudio |
| `tools/` | `282.981` | Headers `libagbc++`, `libsix`, scripts Python/Perl auxiliares de descompilação |

### Descoberta Crítica da Auditoria
Embora a maioria dos bytes em `asm/` ainda represente rotinas de motor/entidades/estado em Assembly (`code_0803EE94.s` com 3,11 MB, `code_809E804.s` com 1,75 MB, `code_entities.s` com 675 KB, `game_state.s` com 585 KB), **uma parte expressiva das estruturas centrais de domínio e gameplay já está em C++ dentro de `src/` e `include/`**:

- **Fazenda & Campo:** `farm.cc`/`farm.hh`, `field.cc`/`field.hh` (`Field<43, 25>`, `FieldPlot`), `shipping_bin.cc`, `farm_house.cc`, `coop.cc`, `barn.cc`, `fridge.cc`, `shelf.cc`, `tool_chest.cc`, `record_player.cc`.
- **Fazendeiro & Inventário:** `farmer.cc`/`farmer.hh` (`Farmer`, `ToolLevel`, `stamina`, `step_count`), `rucksack.cc`/`rucksack.hh`, `rucksack_item.cc`, `held_item.cc`, `item.cc`/`item.hh`, mais `data/item/tool.def` (IDs `0x00`..`0x50`).
- **Atores, Animais & NPCs:** `entity.cc`, `entity_actor.cc`, `npc_entity.cc`, `actor.cc`, `animal.cc`, `pet.cc`, `livestock.cc`, `barn_animal.cc`, `dog.cc`, `horse.cc`, `chicken.cc`, `cow.cc`, `sheep.cc`, `npc.cc`, `bachelorette.cc`, `harvest_sprite.cc`, `data_schedules.cc`.
- **Motor de Scripts (`mary`):** `script_engine.cc` (`13.103` bytes) / `script_engine.hh` — implementa `AScriptEngine` em C++ padrão lendo contêineres binários `'RIFF'` / `'SCR '` com chunks `'CODE'`, `'STR '` e `'JUMP'`.
- **Áudio:** `m4a.c` (`48.154` bytes) / `m4a.h` + `m4a_1.s`.

---

## 3. Ponto de Entrada Real e Mapa de Memória (`crt0.s`, `main.s`, `fomt.lds`)

1. **Símbolo de Entrada:** Não existe `AgmMain`. O fluxo real observado é:
   - `_start` (`src/crt0.s`) → `entrypoint` (configura `sp_irq = 0x03007FA0`, `sp_usr = 0x03007F00`, executa `SVC_REGISTERRAMRESET`, copia `.iwram` via DMA3, inicializa heap em EWRAM de `0x02001F10` até `0x02040000` via `func_080D01E0`, executa `init_array` de construtores estáticos C++) → **`AgbMain` (`0x0800018C` em `asm/main.s`)**.
2. **Inicialização em `AgbMain` (`asm/main.s`):**
   - Configura `REG_WAITCNT` (`0x04000204 = 0x4014`).
   - Chama `func_080004C4` (`asm/interrupt.s`), que limpa tabela de 14 interrupções e registra dispatcher em `0x03007FFC`.
   - Configura `REG_KEYCNT` (`0x04000132 = 0xC00F` para combo de soft-reset `A+B+Start+Select`).
   - Instancia a cena inicial (`func_080036F8` → `AScene`) e entra no gerenciador de cenas (`SceneMain` / `func_0800082C` em `asm/scene.s`), que executa `scene_ptr->Run()` e encadeia cenas subsequentes via `SmartPtr<AScene>`.
3. **Mapa de Memória (`fomt.lds`):**
   - `EWRAM`: `0x02000000` (256 KB)
   - `IWRAM`: `0x03000000` (32 KB)
   - `ROM`: `0x08000000` (8 MB preenchidos até `0x08800000`, código + dados até `0x0875C240`)
4. **Ativos e ROM Base (`baserom.gba`):**
   - Hash esperado em `fomt.sha1` / `README.md`: `a2fc3574f0a65a4fcf7682fb274b9d7eebdef963` (`fomt.gba`, título no cabeçalho `"HARVESTMOGBA"`, código `"A4NE"`, maker `"E9"`).
   - `asm/data/data_0813B288.s` extrai os dados gráficos, paletas, mapas e áudios via `.incbin "baserom.gba"` a partir do offset `0x13B288` até `0x75C240`.

---

## 4. Estratégia de Integração no Port Nativo (`HMFOMT_PC`)

1. **HAL GBA Completa (`src/hal/` + `src/ppu/`):** Barramento virtual 32-bit seguro contra casts `u32`↔ponteiro x86_64, PPU scanline Modos 0–5 + OAM 128 sprites + Janelas/Alpha/Mosaico, SPU 4 canais DMG + 2 DirectSound FIFOs, Input 10 botões e Flash/SRAM `.sav`.
2. **Núcleo Portátil de Domínio (`src/game/`):** Reutilizar/adaptar as classes reais de `external/fomt` (`FieldPlot`, `Field<43, 25>`, `AScriptEngine`, `Tool` enum de `tool.def`) para C++17 portátil desacoplado de ponteiros 32-bit, mantendo suporte opcional de carregamento/inspeção de `baserom.gba` + banco embutido de tiles/sprites para execução imediata em PC.
3. **Multiplayer Autoritativo (`src/net/`):** Servidor autoritativo sobre `Field<43, 25>`, relógio contínuo, clima e `FarmerNetState` por jogador (suportando `map_id` independente por fazendeiro), replicando deltas e sprites OAM remotos sobre UDP (Winsock2).

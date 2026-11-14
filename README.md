# Harvest Moon: Friends of Mineral Town - Native PC Port & Co-op

Clean-room C++20 native PC port and enhanced multiplayer co-op engine for *Harvest Moon: Friends of Mineral Town* (Game Boy Advance).

Featuring full hardware-level emulation (ARM7TDMI CPU, GBA PPU rasterizer, direct audio FIFO) coupled with live memory bridges for real-time multiplayer co-op over UDP.

---

## ⚖️ Legal Disclaimer (Bring Your Own ROM)

This repository **does not contain any copyrighted game ROMs, proprietary binaries, or copyrighted audio/graphical assets**.

* All source code in this repository was written independently as an open-source engine and HAL.
* To play with official game code and assets, **you must provide your own ROM copy** of *Harvest Moon: Friends of Mineral Town (US)* in `.gba` format.
* **Filename does not matter**: The engine checks the internal cartridge header (`HARVESTMOGBA` / `A4NE`), so you do **not** need to rename your ROM file.
* This project follows the same legal model as other preservation and PC port projects (such as *Ship of Harkinian*, *SM64-PC*, *OpenGOAL*, and standalone emulators).

---

## 🚀 How to Run

> [!NOTE]
> If your ROM is inside a compressed archive (`.zip` or `.7z`), make sure to extract the `.gba` file first.

### Option 1: Place in the Game Folder (Easiest)
Copy your `.gba` file (with any filename) into the same folder as `hmfomt_pc.exe`.
The game automatically detects and validates it on startup.

### Option 2: Drag & Drop
Drag and drop any `.gba` file directly onto `hmfomt_pc.exe` in Windows Explorer.

### Option 3: Command Line
Launch with the `--rom` parameter:
```bash
hmfomt_pc.exe --rom "path/to/any_rom_name.gba"
```

---

## 🎮 Multiplayer Co-op (1–4 Players)

### Host a Game:
```bash
hmfomt_pc.exe --host 7777
```
*(Or press `F9` in-game to start hosting on port 7777)*

### Join a Game:
```bash
hmfomt_pc.exe --connect <HOST_IP>:7777
```
*(Or press `F10` in-game to connect to localhost)*

### What is Synchronized:
* **Time & Clock**: Time passes indoors and outdoors, synchronized across all players with customizable day lengths.
* **Farm & Field**: Arable soil, watering, weeds, rocks, stumps, crops, and fences update live across players.
* **Farm Upgrades**: Farmhouse expansions, Gotz building construction, Barn, Chicken Coop, and Dog.
* **Wild Forageables**: Bamboo shoots, flowers, and seasonal berries outside the farm.
* **Visuals & Items**: Other players appear on screen with their custom color palettes, ground shadows, and currently held items (weeds, stones, crops, tools).
* **Independent Economies**: Money (Gold), personal rucksacks, tool levels, shipping bins, and personal stamina remain separate per player.

---

## ⌨️ Controls

| Action | Keyboard | Gamepad |
| :--- | :--- | :--- |
| **Move** | Arrow Keys / WASD | D-Pad / Left Stick |
| **Use Tool / Confirm** | `Z` / `K` | `A` Button |
| **Cancel / Talk / Run** | `X` / `J` | `B` Button |
| **Cycle Tool** | `Q` / `E` / `U` / `L` | `LB` / `RB` Bumpers |
| **Sleep / Next Day** | `Enter` / `Space` | `Start` Button |
| **Save Game (.sav)** | `F5` / `Backspace` | — |
| **Scale Window (1x–5x)** | `1` .. `5` | — |
| **Toggle Fullscreen** | `F11` | — |
| **Host UDP** | `F9` | — |
| **Join UDP** | `F10` | — |

---

## 🛠️ Building from Source

### Requirements
* **C++20 Compiler** (GCC 13+, Clang 16+, or MSVC 2022)
* **CMake 3.20+**
* **SDL2** development libraries

### Build Commands:
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j
ctest --test-dir build --output-on-failure
```

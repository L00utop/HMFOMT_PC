# Harvest Moon: Friends of Mineral Town - Native PC Port & Co-op

Clean-room C++20 native PC port and enhanced multiplayer co-op engine for *Harvest Moon: Friends of Mineral Town* (Game Boy Advance).

Featuring full hardware-level emulation (ARM7TDMI CPU, GBA PPU rasterizer, direct audio FIFO) coupled with live memory bridges for real-time multiplayer co-op over UDP.

---

## ⚖️ Legal Disclaimer (Bring Your Own ROM)

This repository **does not contain any copyrighted game ROMs, proprietary binaries, or copyrighted audio/graphical assets**.

* All source code in this repository was written independently as an open-source engine and HAL.
* To play with original assets and code, **you must provide your own legally dumped copy** of *Harvest Moon: Friends of Mineral Town (US)* (`baserom.gba`, Game Code: `A4NE`).
* This project follows the same legal model as other preservation and PC port projects (such as *Ship of Harkinian*, *SM64-PC*, *OpenGOAL*, and standalone emulators).

---

## 🚀 How to Run

### Option 1: Drag & Drop (Easiest)
Drag and drop your `Friends of Mineral Town (U).gba` file directly onto `hmfomt_pc.exe` in Windows Explorer.

### Option 2: Place in Game Directory
Rename or copy your ROM as `baserom.gba` (or place any valid `.gba` file) in the same directory as `hmfomt_pc.exe`. The game automatically detects and loads it on startup.

### Option 3: Command Line
Launch with the `--rom` parameter:
```bash
hmfomt_pc.exe --rom "C:/Path/To/Your/HarvestMoon.gba"
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
hmfomt_pc.exe --connect 192.168.1.100:7777
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

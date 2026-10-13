# PS2-Style Engine Bootstrap

A minimal C++17 application shell using GLFW, an OpenGL 3.1 context loaded by
GLAD, GLM for camera math, and Dear ImGui. It opens a 1280x720 VSync-limited
window containing a spinning colored cube and an **Engine Debug** overlay.
Press **Esc** to exit.

## Prerequisites

- CMake 3.20 or newer
- Git (CMake fetches pinned dependency revisions during the first configure)
- A C++17 compiler:
  - Windows: Visual Studio 2022 Build Tools (Desktop development with C++)
  - Linux: GCC or Clang, plus your distribution's OpenGL/X11 development files

## Build on Windows

From a Developer PowerShell in this directory:

```powershell
cmake -S . -B build
cmake --build build --config Release
.\build\Release\ps2_engine.exe
```

For a single-config generator such as Ninja:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
.\build\ps2_engine.exe
```

## Build on Linux

Install the compiler, CMake, Git, and OpenGL/X11 development packages using
your distribution's package manager, then run:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/ps2_engine
```

The first configure requires internet access to fetch GLFW 3.4, GLAD 0.1.36,
GLM 1.0.1, and Dear ImGui 1.91.9b. Later builds reuse CMake's local dependency
checkout.

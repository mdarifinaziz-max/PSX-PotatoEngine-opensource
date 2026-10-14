# PotatoEngine

A lightweight C++17 engine bootstrap using GLFW, OpenGL 3.1, GLAD, GLM, and
Dear ImGui.

## 1. Generate GLAD

Open the [GLAD 0.1.36 web generator](https://glad.dav1d.de/) and choose:

- **Language:** C/C++
- **Specification:** OpenGL
- **gl:** Version 3.1
- **Profile:** Core
- **Extensions:** leave empty
- **Generate a loader:** leave unchecked; GLFW supplies `glfwGetProcAddress`

Click **Generate**, download the ZIP, and extract it. Copy the generated
`include` and `src` folders into a new `glad` folder beside `main.cpp`.

The final project must look exactly like this before configuring:

```text
PSX-opensource-game_engine/
|-- CMakeLists.txt
|-- main.cpp
`-- glad/
    |-- include/
    |   |-- glad/
    |   |   `-- glad.h
    |   `-- KHR/
    |       `-- khrplatform.h
    `-- src/
        `-- glad.c
```

Do not put the downloaded ZIP itself in `glad`, and do not add another nested
folder such as `glad/glad/include`. CMake expects the paths shown above.

## 2. Configure and build on Windows

Install Git, CMake, and Visual Studio 2022 Build Tools with the **Desktop
development with C++** workload. Open **Developer PowerShell for VS 2022** in
the project folder and run:

```powershell
cmake -S . -B build
cmake --build build --config Release
```

Run the generated executable:

```powershell
.\build\Release\PotatoEngine.exe
```

If using the Ninja generator instead:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
.\build\PotatoEngine.exe
```

The first configure needs internet access. CMake automatically downloads
pinned GLFW, Dear ImGui, and GLM ZIP archives from GitHub into the build
directory. Git is not required for these dependency downloads.

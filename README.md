# PotatoEngine

A lightweight C++17 engine bootstrap using GLFW, OpenGL 3.1, GLAD, GLM, and
Dear ImGui. Assimp imports OBJ/glTF models, stb_image loads their PNG/JPG
diffuse textures, ReactPhysics3D supplies rigid-body physics, and Portable File
Dialogs provides the native model picker. ImGuizmo supplies Blender-style
translate, rotate, and scale controls directly over the OpenGL viewport.

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
PotatoEngine/
|-- CMakeLists.txt
|-- ImGuizmo.cpp
|-- ImGuizmo.h
|-- main.cpp
|-- portable-file-dialogs.h
|-- stb_image.h
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

## 2. Add stb_image

Download the official single header from:

<https://raw.githubusercontent.com/nothings/stb/master/stb_image.h>

Save it as `stb_image.h` directly beside `main.cpp`. Do not place it inside the
`glad` directory. The copy in this workspace is already in the correct place.

## 3. Add Portable File Dialogs

Download the official single header from:

<https://raw.githubusercontent.com/samhocevar/portable-file-dialogs/main/portable-file-dialogs.h>

Save it as `portable-file-dialogs.h` directly beside `main.cpp`. The copy in
this workspace is already in the correct place, and CMake reports a clear error
if it is missing.

## 4. Add ImGuizmo

Download these two files from the stable ImGuizmo 1.10 release:

- <https://raw.githubusercontent.com/CedricGuillemet/ImGuizmo/1.10/src/ImGuizmo.h>
- <https://raw.githubusercontent.com/CedricGuillemet/ImGuizmo/1.10/src/ImGuizmo.cpp>

Save both directly beside `main.cpp`. `CMakeLists.txt` compiles
`ImGuizmo.cpp` into `PotatoEngine` and reports a clear error if either file is
missing. Both files are already present in this workspace.

## 5. Add a textured model asset

Keep the OBJ, its MTL file, and its referenced PNG/JPG together. For example:

```text
assets/
|-- car.obj
|-- car.mtl
`-- car_diffuse.png
```

The MTL should reference the image with a relative diffuse-map line such as
`map_Kd car_diffuse.png`. Run PotatoEngine from the project root, create an
entity, select it in the Inspector, then click **Browse for Model...**.
The picker accepts `.obj` and `.gltf` files.

## 6. Configure and build on Windows

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
pinned GLFW, Dear ImGui, GLM, Assimp, and ReactPhysics3D ZIP archives from
GitHub into the build directory. Assimp is built statically with only its OBJ
and glTF importers enabled; ReactPhysics3D's tests, testbed, documentation, and
profiling are disabled. Git is not required for these dependency downloads.

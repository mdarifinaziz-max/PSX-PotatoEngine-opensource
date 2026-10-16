cmake_minimum_required(VERSION 3.22)

# GLAD is generated as C while the engine itself is C++17.
project(PotatoEngine LANGUAGES C CXX)

# CMake 3.24+ warns unless downloaded archives receive extraction-time
# timestamps. Keep the file compatible with the requested CMake 3.14 minimum.
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

# Do not spend time building GLFW documentation, tests, or sample programs.
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)

# Keep Assimp lean: static library, OBJ/glTF importing only, no tools/tests/exporters.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ASSIMP_TOOLS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(ASSIMP_INSTALL OFF CACHE BOOL "" FORCE)
set(ASSIMP_NO_EXPORT ON CACHE BOOL "" FORCE)
set(ASSIMP_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(ASSIMP_IGNORE_GIT_HASH ON CACHE BOOL "" FORCE)
set(ASSIMP_INJECT_DEBUG_POSTFIX OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_OBJ_IMPORTER ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_GLTF_IMPORTER ON CACHE BOOL "" FORCE)

# ReactPhysics3D is intentionally built without its testbed, examples, tests,
# or profiling data. The engine only needs the small runtime library.
set(RP3D_COMPILE_TESTBED OFF CACHE BOOL "" FORCE)
set(RP3D_COMPILE_TESTS OFF CACHE BOOL "" FORCE)
set(RP3D_GENERATE_DOCUMENTATION OFF CACHE BOOL "" FORCE)
set(RP3D_PROFILING_ENABLED OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    glfw
    URL https://github.com/glfw/glfw/archive/refs/tags/3.4.zip
)

# Dear ImGui deliberately has no required upstream CMake target. Its source
# files are added directly to PotatoEngine below.
FetchContent_Declare(
    imgui
    # Docking is maintained as an official parallel release tag.
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.91.9b-docking.zip
)

# The current main.cpp uses GLM for model/view/projection matrices, so fetch
# this header-only dependency too.
set(GLM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    glm
    URL https://github.com/g-truc/glm/archive/refs/tags/1.0.1.zip
)

FetchContent_Declare(
    assimp
    URL https://github.com/assimp/assimp/archive/refs/tags/v6.0.5.zip
)

FetchContent_Declare(
    reactphysics3d
    URL https://github.com/DanielChappuis/reactphysics3d/archive/refs/tags/v0.10.2.zip
)

FetchContent_MakeAvailable(glfw imgui glm assimp reactphysics3d)

# ReactPhysics3D 0.10.2's logger uses std::chrono without including <chrono>.
# Older MSVC releases accepted that transitively; current MSVC correctly does
# not. Force-include the standard header for this third-party target only.
if(MSVC)
    target_compile_options(reactphysics3d PRIVATE /FIchrono)
endif()

# Locate the operating system's standard OpenGL library (opengl32.lib on
# Windows, for example).
find_package(OpenGL REQUIRED)

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/stb_image.h")
    message(FATAL_ERROR
        "Missing stb_image.h. Download it from "
        "https://raw.githubusercontent.com/nothings/stb/master/stb_image.h "
        "and place it beside main.cpp.")
endif()

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/portable-file-dialogs.h")
    message(FATAL_ERROR
        "Missing portable-file-dialogs.h. Download it from "
        "https://raw.githubusercontent.com/samhocevar/portable-file-dialogs/main/portable-file-dialogs.h "
        "and place it beside main.cpp.")
endif()

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ImGuizmo.h" OR
   NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ImGuizmo.cpp")
    message(FATAL_ERROR
        "Missing ImGuizmo.h or ImGuizmo.cpp. Download both files from "
        "https://github.com/CedricGuillemet/ImGuizmo/tree/1.10/src "
        "and place them beside main.cpp.")
endif()

add_executable(PotatoEngine
    main.cpp
    ImGuizmo.cpp

    # GLAD 0.1.x files generated for OpenGL 3.1 Core.
    glad/src/glad.c

    # Dear ImGui core.
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp

    # Dear ImGui platform and renderer backends.
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)

target_include_directories(PotatoEngine PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/glad/include
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)

target_link_libraries(PotatoEngine PRIVATE
    glfw
    glm::glm
    assimp::assimp
    ReactPhysics3D::reactphysics3d
    OpenGL::GL
)

# GetProcessMemoryInfo() powers the live working-set display in Engine Debug.
if(WIN32)
    target_link_libraries(PotatoEngine PRIVATE Psapi)
endif()

if(MSVC)
    target_compile_options(PotatoEngine PRIVATE /W4 /permissive-)
else()
    target_compile_options(PotatoEngine PRIVATE -Wall -Wextra -Wpedantic)
endif()

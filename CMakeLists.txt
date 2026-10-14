cmake_minimum_required(VERSION 3.14)

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

FetchContent_Declare(
    glfw
    URL https://github.com/glfw/glfw/archive/refs/tags/3.4.zip
)

# Dear ImGui deliberately has no required upstream CMake target. Its source
# files are added directly to PotatoEngine below.
FetchContent_Declare(
    imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.91.9b.zip
)

# The current main.cpp uses GLM for model/view/projection matrices, so fetch
# this header-only dependency too.
set(GLM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    glm
    URL https://github.com/g-truc/glm/archive/refs/tags/1.0.1.zip
)

FetchContent_MakeAvailable(glfw imgui glm)

# Locate the operating system's standard OpenGL library (opengl32.lib on
# Windows, for example).
find_package(OpenGL REQUIRED)

add_executable(PotatoEngine
    main.cpp

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
    ${CMAKE_CURRENT_SOURCE_DIR}/glad/include
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)

target_link_libraries(PotatoEngine PRIVATE
    glfw
    glm::glm
    OpenGL::GL
)

if(MSVC)
    target_compile_options(PotatoEngine PRIVATE /W4 /permissive-)
else()
    target_compile_options(PotatoEngine PRIVATE -Wall -Wextra -Wpedantic)
endif()

cmake_minimum_required(VERSION 3.20)
project(fzgx_course_editor CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# Format library (no GUI dependencies).
add_library(fzgx STATIC src/lz.cpp src/coli.cpp src/gma.cpp src/tpl.cpp src/project.cpp)
target_include_directories(fzgx PUBLIC src)
target_compile_definitions(fzgx PUBLIC FZGX_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(fzgx PRIVATE -Wall -Wextra)

add_executable(fzgx-cli src/cli.cpp)
target_link_libraries(fzgx-cli PRIVATE fzgx)

option(FZGX_BUILD_GUI "Build the graphical editor" ON)
if(FZGX_BUILD_GUI)
  include(FetchContent)
  FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.9)
  FetchContent_Declare(imguizmo
    GIT_REPOSITORY https://github.com/CedricGuillemet/ImGuizmo.git
    GIT_TAG a2da036ad300de1fd8d706f969b46e1c5a837d82)
  FetchContent_MakeAvailable(imgui imguizmo)

  find_package(SDL3 REQUIRED CONFIG)
  find_package(OpenGL REQUIRED)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(EPOXY REQUIRED IMPORTED_TARGET epoxy)

  add_library(imgui_lib STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
    ${imguizmo_SOURCE_DIR}/ImGuizmo.cpp)
  target_include_directories(imgui_lib PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends
    ${imgui_SOURCE_DIR}/misc/cpp ${imguizmo_SOURCE_DIR})
  target_link_libraries(imgui_lib PUBLIC SDL3::SDL3 OpenGL::GL)

  add_executable(fzgx-editor src/main.cpp src/renderer.cpp src/editor.cpp)
  target_link_libraries(fzgx-editor PRIVATE fzgx imgui_lib PkgConfig::EPOXY)
  target_compile_options(fzgx-editor PRIVATE -Wall -Wextra)
endif()

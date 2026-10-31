cmake_minimum_required(VERSION 3.20)
project(uc_menu LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# статическая среда выполнения: на машине игрока не нужен Visual C++ Redistributable
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

include(FetchContent)
FetchContent_Declare(imgui   GIT_REPOSITORY https://github.com/ocornut/imgui.git          GIT_TAG v1.91.5)
FetchContent_Declare(minhook GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git    GIT_TAG v1.3.3)
FetchContent_MakeAvailable(imgui minhook)

add_library(uc_menu SHARED
    src/main.cpp src/state.cpp src/ui.cpp src/title.cpp src/postfx.cpp
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    ${minhook_SOURCE_DIR}/src/buffer.c
    ${minhook_SOURCE_DIR}/src/hook.c
    ${minhook_SOURCE_DIR}/src/trampoline.c
    ${minhook_SOURCE_DIR}/src/hde/hde64.c)

target_include_directories(uc_menu PRIVATE
    src ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends ${minhook_SOURCE_DIR}/include)
target_compile_definitions(uc_menu PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(uc_menu PRIVATE opengl32 gdi32 user32 shell32 psapi)
set_target_properties(uc_menu PROPERTIES PREFIX "")   # файл называется uc_menu.dll, без «lib»
if (MSVC)
    target_compile_options(uc_menu PRIVATE /utf-8)   # русские строки в исходниках
else()
    # clang / mingw: всё статически, чтобы рядом с dll не требовались libstdc++ / libc++
    target_link_options(uc_menu PRIVATE -static -static-libgcc)
endif()

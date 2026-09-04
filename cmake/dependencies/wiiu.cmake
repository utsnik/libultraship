#=================== Wii U / CafeOS dependencies ===================
#
# devkitPro's portlibs cover libzip and tinyxml2 for ppc, but ship neither
# nlohmann_json nor spdlog. Both are header-only (spdlog in header-only mode), so
# fetch them and hand the resulting targets to the unconditional
# find_package(... REQUIRED) calls in src/CMakeLists.txt via OVERRIDE_FIND_PACKAGE
# rather than patching those call sites -- that keeps this platform's plumbing
# entirely inside this file and avoids diverging from upstream.

include(FetchContent)

FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    OVERRIDE_FIND_PACKAGE
)
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)

# Header-only spdlog: the compiled variant drags in <thread>/<mutex> paths that
# fight wut's newlib configuration.
set(SPDLOG_BUILD_SHARED OFF CACHE INTERNAL "")
set(SPDLOG_NO_THREAD_ID ON CACHE INTERNAL "")
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.14.1
    OVERRIDE_FIND_PACKAGE
)
FetchContent_MakeAvailable(spdlog)

#=================== ImGui ===================
#
# The Wii U has no SDL3 in devkitPro's portlibs and no OpenGL at all, so unlike
# every other platform ImGui is built here with NO platform/renderer backend
# linked in: rendering goes through imgui_impl_gx2 and input through
# imgui_impl_wiiu, both of which live in libultraship's own Fast3D backend dir.
#
# Deliberately NOT linked: SDL3, OpenGL, GLEW.

find_library(WUT_LIB wut REQUIRED HINTS "$ENV{DEVKITPRO}/wut/lib")

target_include_directories(ImGui PUBLIC
    "$ENV{DEVKITPRO}/wut/include"
)

target_link_libraries(ImGui PUBLIC ${WUT_LIB})

# NOTE: do NOT define IMGUI_DISABLE_OBSOLETE_FUNCTIONS here. libultraship's own
# InputEditorWindow.cpp still calls ImGui::Push/PopButtonRepeat, which 1.91 keeps
# only as obsolete API. Disabling it breaks upstream sources that have nothing to
# do with this platform. The Wii U needs no ImGui defines beyond the backends
# exclusion already handled in common.cmake.

#=================== SDL3-over-SDL2 shim ===================
# SDL3 has no Wii U video driver upstream and devkitPro ships only wiiu-sdl2, so
# there is no SDL3 to link. include/port/wiiu/sdl3compat provides an <SDL3/SDL.h>
# that maps the 107 SDL3-only symbols libultraship uses onto SDL2 equivalents.
# It is placed BEFORE the normal include dirs so #include <SDL3/SDL.h> resolves here.
find_library(WIIU_SDL2_LIB SDL2 REQUIRED HINTS "$ENV{DEVKITPRO}/portlibs/wiiu/lib")
list(APPEND ADDITIONAL_LIB_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/include/port/wiiu/sdl3compat)
target_include_directories(ImGui BEFORE PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include/port/wiiu/sdl3compat)

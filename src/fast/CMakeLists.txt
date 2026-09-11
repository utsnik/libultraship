#=================== Top-Level ===================
file(GLOB Source_Files__TopLevel RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} "*.cpp")

source_group("" FILES ${Source_Files__TopLevel})
target_sources(libultraship PRIVATE ${Source_Files__TopLevel})

#=================== Debug ===================

file(GLOB Source_Files__Debug RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} "debug/*.cpp")

source_group("debug" FILES ${Source_Files__Debug})
target_sources(libultraship PRIVATE ${Source_Files__Debug})

#=================== Resource ===================

file(GLOB Source_Files__Resource__Types RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} "resource/type/*.h" "resource/type/*.cpp")
source_group("resource/type" FILES ${Source_Files__Resource__Types})

file(GLOB Source_Files__Resource__Factories RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} "resource/factory/*.h" "resource/factory/*.cpp")
source_group("resource/factory" FILES ${Source_Files__Resource__Factories})

target_sources(libultraship PRIVATE ${Source_Files__Resource} ${Source_Files__Resource__Types} ${Source_Files__Resource__Factories} ${Source_Files__Resource__Archive})

# Keep shader templates visible to source-package generators. The Vita template
# is packed into the game's resource archive alongside the existing backends.
file(GLOB_RECURSE Shader_Files CONFIGURE_DEPENDS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} "shaders/*")
source_group("shaders" FILES ${Shader_Files})
target_sources(libultraship PRIVATE ${Shader_Files})

#=================== Graphic ===================

file(GLOB Source_Files__Graphic RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} "backends/*.cpp")

if (NOT CMAKE_SYSTEM_NAME STREQUAL "PSVita")
    list(FILTER Source_Files__Graphic EXCLUDE REGEX "gfx_vita_shader\\.cpp$")
endif()

if (CMAKE_SYSTEM_NAME STREQUAL "PSVita")
    list(FILTER Source_Files__Graphic EXCLUDE REGEX "gfx_wiiu\\.cpp$")
    list(FILTER Source_Files__Graphic EXCLUDE REGEX "imgui_impl_(gx2|wiiu)\\.cpp$")
endif()

if (NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
    list(FILTER Source_Files__Graphic EXCLUDE REGEX "graphic/Fast3D/backends/gfx_dxgi*")
    list(FILTER Source_Files__Graphic EXCLUDE REGEX "graphic/Fast3D/backends/gfx_direct3d*")
    list(FILTER Source_Files__Graphic EXCLUDE REGEX "graphic/Fast3D/backends/dxsdk/*")
endif()

if (NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    list(FILTER Source_Files__Graphic EXCLUDE REGEX "graphic/Fast3D/backends/gfx_metal*")
endif()

if (CMAKE_SYSTEM_NAME STREQUAL "iOS")
    list(FILTER Source_Files__Graphic EXCLUDE REGEX "graphic/Fast3D/backends/gfx_opengl*")
endif()

if (CMAKE_SYSTEM_NAME STREQUAL "CafeOS")
    # Wii U has no OpenGL/SDL video path; it renders through GX2.
    list(FILTER Source_Files__Graphic EXCLUDE REGEX "gfx_opengl")
    list(FILTER Source_Files__Graphic EXCLUDE REGEX "gfx_sdl")
else()
    list(FILTER Source_Files__Graphic EXCLUDE REGEX "gfx_gx2")
    list(FILTER Source_Files__Graphic EXCLUDE REGEX "gx2_shader_gen")
endif()

source_group("graphic" FILES ${Source_Files__Graphic})
target_sources(libultraship PRIVATE ${Source_Files__Graphic})

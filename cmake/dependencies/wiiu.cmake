#=================== ImGui (Wii U / CafeOS) ===================
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

# ImGui's default backends assume a desktop windowing system; keep them out.
target_compile_definitions(ImGui PUBLIC
    IMGUI_DISABLE_DEFAULT_ALLOCATORS=0
    IMGUI_IMPL_API=
)

# GPU7 has no compute/geometry stages and a hard 8192 texture limit; nothing here
# needs the docking backends' multi-viewport support, which requires a real WM.
target_compile_definitions(ImGui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)

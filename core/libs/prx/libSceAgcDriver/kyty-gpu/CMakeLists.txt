set(kytyRoot ${CMAKE_SOURCE_DIR}/3rdparty/KytyPS5)
set(kytySource ${kytyRoot}/src)
set(kytyGenerated ${CMAKE_CURRENT_BINARY_DIR}/generated)

find_package(Git REQUIRED)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/kyty.patch)
execute_process(COMMAND ${GIT_EXECUTABLE} -C ${kytyRoot} apply --reverse --check ${CMAKE_CURRENT_SOURCE_DIR}/kyty.patch RESULT_VARIABLE kytyPatched OUTPUT_QUIET ERROR_QUIET)
if(NOT kytyPatched EQUAL 0)
    execute_process(COMMAND ${GIT_EXECUTABLE} -C ${kytyRoot} apply ${CMAKE_CURRENT_SOURCE_DIR}/kyty.patch RESULT_VARIABLE kytyApplied)
    if(NOT kytyApplied EQUAL 0)
        message(FATAL_ERROR "Cannot apply kyty-gpu/kyty.patch to ${kytyRoot}; the submodule must be at 099de8dea62cbf17c8e06a6c24e72b38475d1566 with no other changes")
    endif()
endif()
if(NOT EXISTS ${kytyRoot}/3rdparty/xbyak/xbyak/xbyak.h)
    message(FATAL_ERROR "KytyPS5 needs xbyak: git clone https://github.com/herumi/xbyak ${kytyRoot}/3rdparty/xbyak && git -C ${kytyRoot}/3rdparty/xbyak checkout 44a72f369268f7d552650891b296693e91db86bb")
endif()

file(GLOB_RECURSE kytyGraphicsSources CONFIGURE_DEPENDS ${kytySource}/graphics/*.cpp)
list(FILTER kytyGraphicsSources EXCLUDE REGEX "presentation/(window/(window|hostInput)\.cpp|systemOverlay|performanceOverlay|renderDoc|ddsImage)")
file(GLOB kytyCommonSources CONFIGURE_DEPENDS
        ${kytySource}/common/*.cpp
        ${kytySource}/common/logging/*.cpp
        ${kytySource}/common/platform/*.cpp
)
list(FILTER kytyCommonSources EXCLUDE REGEX "/(archive|trophies|localization|additionalContent|systemInfo|hostStats|profiler)\\.cpp$")

find_program(kytyGlslang NAMES glslangValidator HINTS "$ENV{VULKAN_SDK}/Bin" "C:/VulkanSDK/1.4.363.0/Bin" REQUIRED)
set(kytyShaderDir ${kytySource}/graphics/host_gpu/shaders)
set(kytyShaderHeaders)
function(kyty_embed_shader directory name source)
    set(spv ${kytyGenerated}/${directory}/${name}.spv)
    set(header ${kytyGenerated}/${directory}/${name}_spv.h)
    string(TOUPPER "${name}_SPV" symbol)
    add_custom_command(
            OUTPUT ${header}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${kytyGenerated}/${directory}
            COMMAND ${kytyGlslang} -V --target-env vulkan1.3 -Os ${ARGN} -I${kytyShaderDir} -o ${spv} ${kytyShaderDir}/${source}
            COMMAND ${CMAKE_COMMAND} -DINPUT=${spv} -DOUTPUT=${header} -DSYMBOL=${symbol} -P ${kytySource}/embed_spirv.cmake
            DEPENDS ${kytyShaderDir}/${source}
            VERBATIM
    )
    set(kytyShaderHeaders ${kytyShaderHeaders} ${header} PARENT_SCOPE)
endfunction()
foreach(tiler IN ITEMS standard256 standard4 standard4_3d standard64 standard64_3d prt prt_3d render_target depth promote_d16 demote_d16 color_transform)
    kyty_embed_shader(gpu_tiler_shaders gpu_tiler_${tiler} gpu_tiler_${tiler}.comp)
endforeach()
kyty_embed_shader(gpu_tiler_shaders fault_buffer_process fault_buffer_process.comp -DCACHING_PAGEBITS=14 -DMAX_PAGE_FAULTS=1024)
kyty_embed_shader(gpu_blit_shaders gpu_blit_fs_triangle gpu_blit_fs_triangle.vert)
kyty_embed_shader(gpu_blit_shaders gpu_blit_color_to_ms_depth gpu_blit_color_to_ms_depth.frag)
kyty_embed_shader(gpu_blit_shaders gpu_video_out_overlay gpu_video_out_overlay.frag)
kyty_embed_shader(gpu_blit_shaders gpu_imgui_hdr gpu_video_out_overlay.frag -DIMGUI_HDR=1)

file(WRITE ${kytyGenerated}/cmake_config.h.in "#define KYTY_VERSION \"anyps5\"\n#define KYTY_BUILD_REPOSITORY \"KytyPS5/KytyPS5\"\n#define KYTY_RELEASE_TAG \"\"\n#define KYTY_ENDIAN KYTY_ENDIAN_LITTLE\n#define KYTY_PLATFORM KYTY_PLATFORM_WINDOWS\n#define KYTY_COMPILER KYTY_COMPILER_GCC\n#define KYTY_LINKER KYTY_LINKER_LD\n#define KYTY_BUILD KYTY_BUILD_RELEASE\n")
configure_file(${kytyGenerated}/cmake_config.h.in ${kytyGenerated}/cmake_config.h COPYONLY)
file(WRITE ${kytyGenerated}/kytyGitVersion.h.in "#include \"cmake_config.h\"\n#define KYTY_GIT_VERSION \"099de8d\"\n#define KYTY_GIT_HASH \"099de8d\"\n#define KYTY_GIT_REVISION \"099de8dea62cbf17c8e06a6c24e72b38475d1566\"\n#define KYTY_BUILD_LABEL \"AnyPS5 build 099de8d\"\n")
configure_file(${kytyGenerated}/kytyGitVersion.h.in ${kytyGenerated}/kytyGitVersion.h COPYONLY)

add_library(kytygpu SHARED EXCLUDE_FROM_ALL
        ${kytyGraphicsSources}
        ${kytyCommonSources}
        ${kytyShaderHeaders}
        ${CMAKE_CURRENT_SOURCE_DIR}/src/KytyGpuHost.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/kytygpu.def
)
target_include_directories(kytygpu PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${kytyGenerated}
        ${kytySource}
        ${kytyRoot}/3rdparty/Vulkan-Headers/include
        ${kytyRoot}/3rdparty/VulkanMemoryAllocator/include
        ${kytyRoot}/3rdparty/SPIRV-Headers/include
        ${kytyRoot}/3rdparty/fmt/include
        ${kytyRoot}/3rdparty/spdlog/include
        ${kytyRoot}/3rdparty/magic_enum/include/magic_enum
        ${kytyRoot}/3rdparty/xxHash
        ${kytyRoot}/3rdparty/nlohmann_json/include
        ${kytyRoot}/3rdparty/imgui
        ${kytyRoot}/3rdparty/tracy/public
        ${kytyRoot}/3rdparty/xbyak
        ${CMAKE_SOURCE_DIR}/3rdparty/SPIRV-Tools/include
)
target_compile_definitions(kytygpu PRIVATE NOMINMAX FMT_HEADER_ONLY XXH_INLINE_ALL VK_NO_PROTOTYPES VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1 XBYAK_NO_EXCEPTION SPDLOG_FMT_EXTERNAL SPDLOG_WCHAR_FILENAMES)
target_compile_options(kytygpu PRIVATE -Wno-deprecated-declarations -fexceptions)
target_link_libraries(kytygpu PRIVATE SPIRV-Tools-opt SPIRV-Tools-static)
target_link_options(kytygpu PRIVATE -static -static-libgcc -static-libstdc++)
set_target_properties(kytygpu PROPERTIES
        PREFIX ""
        RUNTIME_OUTPUT_DIRECTORY ${PATCHED_DIR}
        LIBRARY_OUTPUT_DIRECTORY ${PATCHED_DIR}
        ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

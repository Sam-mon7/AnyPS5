function(ensure_agc_glslang)
    if(TARGET glslang AND TARGET SPIRV AND TARGET glslang-standalone)
        return()
    endif()

    if(TARGET glslang)
        if(NOT TARGET SPIRV)
            message(FATAL_ERROR "glslang exists but SPIRV target is missing")
        endif()
        if(NOT TARGET glslang-standalone)
            message(FATAL_ERROR "glslang exists but glslang-standalone target is missing")
        endif()
        return()
    endif()

    set(BUILD_SHARED_LIBS OFF)
    set(BUILD_EXTERNAL OFF)
    set(GLSLANG_TESTS OFF)
    set(GLSLANG_ENABLE_INSTALL OFF)
    set(ENABLE_GLSLANG_BINARIES ON)
    set(ENABLE_SPIRV ON)
    set(ENABLE_SPVREMAPPER OFF)
    set(ENABLE_OPT OFF)

    add_subdirectory(
            "${CMAKE_SOURCE_DIR}/3rdparty/glslang"
            "${CMAKE_BINARY_DIR}/3rdparty/glslang"
            EXCLUDE_FROM_ALL
    )
endfunction()

ensure_agc_glslang()

set(agcTextureDetileShaderSource ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/shaders/TextureDetile.comp)
set(agcTextureDetileGeneratedRoot ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(agcTextureDetileGeneratedDir ${agcTextureDetileGeneratedRoot}/prx/libSceAgcDriver/Graphics/shaders)
set(agcTextureDetileShaderSpv ${agcTextureDetileGeneratedDir}/TextureDetile.spv)
set(agcTextureDetileShaderHeader ${agcTextureDetileGeneratedDir}/TextureDetile_spv.h)
add_custom_command(
        OUTPUT "${agcTextureDetileShaderHeader}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${agcTextureDetileGeneratedDir}"
        COMMAND "$<TARGET_FILE:glslang-standalone>" -V --target-env vulkan1.1 -o "${agcTextureDetileShaderSpv}" "${agcTextureDetileShaderSource}"
        COMMAND ${CMAKE_COMMAND} -DINPUT=${agcTextureDetileShaderSpv} -DOUTPUT=${agcTextureDetileShaderHeader}
        -DSYMBOL=TEXTURE_DETILE_SPV -P "${CMAKE_CURRENT_SOURCE_DIR}/embed_spirv.cmake"
        DEPENDS "${agcTextureDetileShaderSource}" glslang-standalone
        VERBATIM
)
set(agcTextureDetileImageShaderSpv ${agcTextureDetileGeneratedDir}/TextureDetileImage.spv)
set(agcTextureDetileImageShaderHeader ${agcTextureDetileGeneratedDir}/TextureDetileImage_spv.h)
add_custom_command(
        OUTPUT "${agcTextureDetileImageShaderHeader}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${agcTextureDetileGeneratedDir}"
        COMMAND "$<TARGET_FILE:glslang-standalone>" -V --target-env vulkan1.1 -DLINEAR_IMAGE=1 -o "${agcTextureDetileImageShaderSpv}" "${agcTextureDetileShaderSource}"
        COMMAND ${CMAKE_COMMAND} -DINPUT=${agcTextureDetileImageShaderSpv} -DOUTPUT=${agcTextureDetileImageShaderHeader}
        -DSYMBOL=TEXTURE_DETILE_IMAGE_SPV -P "${CMAKE_CURRENT_SOURCE_DIR}/embed_spirv.cmake"
        DEPENDS "${agcTextureDetileShaderSource}" glslang-standalone
        VERBATIM
)

set(agcMeshArgumentsShaderSource ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/shaders/MeshArguments.comp)
set(agcMeshArgumentsShaderSpv ${agcTextureDetileGeneratedDir}/MeshArguments.spv)
set(agcMeshArgumentsShaderHeader ${agcTextureDetileGeneratedDir}/MeshArguments_spv.h)
add_custom_command(
        OUTPUT "${agcMeshArgumentsShaderHeader}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${agcTextureDetileGeneratedDir}"
        COMMAND "$<TARGET_FILE:glslang-standalone>" -V --target-env vulkan1.1 -o "${agcMeshArgumentsShaderSpv}" "${agcMeshArgumentsShaderSource}"
        COMMAND ${CMAKE_COMMAND} -DINPUT=${agcMeshArgumentsShaderSpv} -DOUTPUT=${agcMeshArgumentsShaderHeader}
        -DSYMBOL=MESH_ARGUMENTS_SPV -P "${CMAKE_CURRENT_SOURCE_DIR}/embed_spirv.cmake"
        DEPENDS "${agcMeshArgumentsShaderSource}" glslang-standalone
        VERBATIM
)

set(agcCmaskClearShaderSource ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/shaders/CmaskClear.comp)
set(agcCmaskClearShaderSpv ${agcTextureDetileGeneratedDir}/CmaskClear.spv)
set(agcCmaskClearShaderHeader ${agcTextureDetileGeneratedDir}/CmaskClear_spv.h)
add_custom_command(
        OUTPUT "${agcCmaskClearShaderHeader}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${agcTextureDetileGeneratedDir}"
        COMMAND "$<TARGET_FILE:glslang-standalone>" -V --target-env vulkan1.1 -o "${agcCmaskClearShaderSpv}" "${agcCmaskClearShaderSource}"
        COMMAND ${CMAKE_COMMAND} -DINPUT=${agcCmaskClearShaderSpv} -DOUTPUT=${agcCmaskClearShaderHeader}
        -DSYMBOL=CMASK_CLEAR_SPV -P "${CMAKE_CURRENT_SOURCE_DIR}/embed_spirv.cmake"
        DEPENDS "${agcCmaskClearShaderSource}" glslang-standalone
        VERBATIM
)

set(agcSampleCounterShaderSource ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/shaders/SampleCounter.comp)
set(agcSampleCounterShaderSpv ${agcTextureDetileGeneratedDir}/SampleCounter.spv)
set(agcSampleCounterShaderHeader ${agcTextureDetileGeneratedDir}/SampleCounter_spv.h)
add_custom_command(
        OUTPUT "${agcSampleCounterShaderHeader}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${agcTextureDetileGeneratedDir}"
        COMMAND "$<TARGET_FILE:glslang-standalone>" -V --target-env vulkan1.1 -o "${agcSampleCounterShaderSpv}" "${agcSampleCounterShaderSource}"
        COMMAND ${CMAKE_COMMAND} -DINPUT=${agcSampleCounterShaderSpv} -DOUTPUT=${agcSampleCounterShaderHeader}
        -DSYMBOL=SAMPLE_COUNTER_SPV -P "${CMAKE_CURRENT_SOURCE_DIR}/embed_spirv.cmake"
        DEPENDS "${agcSampleCounterShaderSource}" glslang-standalone
        VERBATIM
)

set(agcSampleDumpsShaderSource ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/shaders/SampleDumps.comp)
set(agcSampleDumpsShaderSpv ${agcTextureDetileGeneratedDir}/SampleDumps.spv)
set(agcSampleDumpsShaderHeader ${agcTextureDetileGeneratedDir}/SampleDumps_spv.h)
add_custom_command(
        OUTPUT "${agcSampleDumpsShaderHeader}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${agcTextureDetileGeneratedDir}"
        COMMAND "$<TARGET_FILE:glslang-standalone>" -V --target-env vulkan1.1 -o "${agcSampleDumpsShaderSpv}" "${agcSampleDumpsShaderSource}"
        COMMAND ${CMAKE_COMMAND} -DINPUT=${agcSampleDumpsShaderSpv} -DOUTPUT=${agcSampleDumpsShaderHeader}
        -DSYMBOL=SAMPLE_DUMPS_SPV -P "${CMAKE_CURRENT_SOURCE_DIR}/embed_spirv.cmake"
        DEPENDS "${agcSampleDumpsShaderSource}" glslang-standalone
        VERBATIM
)

set(agcDriverSources
        ${CMAKE_CURRENT_SOURCE_DIR}/Unimplemented.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/ComputePipelineCapture.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/DisplayBuffer.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/DisplayFormat.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/DeviceAccess.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Diagnostics.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Dispatch/DataHitVerification.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Dispatch/Dispatch.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Dispatch/DispatchAhead.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Dispatch/DispatchCache.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Dispatch/DispatchCacheStatistics.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Dispatch/DispatchInsert.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Dispatch/DispatchLookup.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Dispatch/DispatchTiming.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Dispatch/IndirectDispatch.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Dispatch/VariantValidation.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Draw/Draw.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Draw/DrawCache.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Draw/DrawCacheStatistics.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Draw/DrawCapture.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Draw/DrawDecode.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Draw/DrawLookup.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Draw/DrawPipeline.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Draw/DrawPrecheck.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Draw/DrawRegisterKey.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Draw/DrawRelocate.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Draw/DrawTiming.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Draw/IndirectDraw.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/EntryPoints.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Lifecycle.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Memory/BufferCopy.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Memory/BufferFill.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Memory/CaptureDiagnostics.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Memory/CaptureValidation.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Memory/WriteEvidence.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Memory/WrittenBuffers.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Packets/CommandValidation.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Packets/PacketExecution.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Packets/PacketFlush.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Packets/PacketHistory.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Packets/PacketMemory.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Packets/PacketTiming.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Presentation.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Queues/QueueWorker.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Queues/Submission.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Queues/SubmissionRing.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Queues/WorkerAffinity.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Shaders/ShaderDiagnostics.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Shaders/ShaderRegistry.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Synchronization/DeferredLabels.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Synchronization/LabelHistory.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Synchronization/SynchronizationStatistics.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Synchronization/WaitMemory.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Synchronization/WaitStatistics.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/WorkerSampler.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/PerformanceTrace.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/TraceFile.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/ThreadPriority.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/GuestMemory.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/ShaderMemory.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/BdaFeatures.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/SubgroupClock.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/ShaderDeviceProfile.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Pm4.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/VulkanDevice.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/AspectFit.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/PresentationScaler.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/src/Draw.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/src/DepthSurface.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/src/MultisampleTarget.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/src/ColorTargetLayout.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/src/ColorTargetTransfer.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/src/Pipeline.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/src/PipelineLibrary.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/src/PipelineCache.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/src/Resources.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/src/PlaceholderImages.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/src/Recorder.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/src/ParallelCompare.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/src/ShaderInputState.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/src/ShaderResources.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/src/ShaderAddressResources.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/src/GuestBufferMemory.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/src/BdaResources.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/src/BufferAddress.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/src/ShaderValidation.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/src/State.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/src/GuestTextureResource.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/src/GuestSamplerResource.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/src/TextureFormat.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/src/DccMetadata.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/src/TextureTiling.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/src/TextureDetiler.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/src/Texture.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/src/PassHazards.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/src/UnitShadow.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/src/Sampler.cpp
        ${agcTextureDetileShaderHeader}
        ${agcCmaskClearShaderHeader}
        ${agcMeshArgumentsShaderHeader}
        ${agcTextureDetileImageShaderHeader}
        ${agcSampleCounterShaderHeader}
        ${agcSampleDumpsShaderHeader}
        ${CMAKE_CURRENT_SOURCE_DIR}/Eq/src/Event.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Eq/src/Query.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Resource/src/Lifecycle.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Resource/src/Registration.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/State/src/Configuration.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/State/src/Status.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Submit/src/Acb.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Submit/src/CommandBuffer.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Submit/src/Dcb.cpp
)

find_package(Git QUIET)
set(agcTraceRevisionHeader ${agcTextureDetileGeneratedRoot}/prx/libSceAgcDriver/TraceRevision.hpp)
add_custom_target(agc_trace_revision
        COMMAND ${CMAKE_COMMAND} -DGIT=${GIT_EXECUTABLE} -DSOURCE=${CMAKE_SOURCE_DIR} -DOUTPUT=${agcTraceRevisionHeader} -P ${CMAKE_CURRENT_SOURCE_DIR}/trace_revision.cmake
        BYPRODUCTS ${agcTraceRevisionHeader}
        VERBATIM
)
set(agcZlibIncludeDir ${CMAKE_SOURCE_DIR}/3rdparty/libjpeg-turbo/src/spng/zlib)
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/TraceFile.cpp PROPERTIES
        INCLUDE_DIRECTORIES ${agcZlibIncludeDir}
)

add_library(libSceAgcDriver SHARED EXCLUDE_FROM_ALL
        ${agcDriverSources}
)
add_dependencies(libSceAgcDriver agc_trace_revision)

target_include_directories(libSceAgcDriver PRIVATE
        ${LIBS_INCLUDE_DIR}
        ${agcTextureDetileGeneratedRoot}
        ${CMAKE_SOURCE_DIR}/core/shader/recompiler/RdnaDecoder/include
        ${ANYPS5_VULKAN_HEADERS_INCLUDE_DIR}
        ${ANYPS5_SPIRV_HEADERS_INCLUDE_DIR}
)

file(GLOB_RECURSE agcShaderCacheGuardSources CONFIGURE_DEPENDS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
)
list(FILTER agcShaderCacheGuardSources EXCLUDE REGEX "^(tests|tools)/")
add_shader_recompiler(shader_recompiler_agc_driver libc
        GUARD_SOURCES
        ${agcShaderCacheGuardSources}
)

target_link_libraries(libSceAgcDriver PRIVATE
        shader_recompiler_agc_driver
        SDL2-static
        libc
        libkernel
        jpeg_turbo
        ${CMAKE_DL_LIBS}
)
set(ANYPS5_AFTERMATH_SDK_ROOT "" CACHE PATH "Optional local Nsight Aftermath SDK root (contains include and lib)")
if(ANYPS5_AFTERMATH_SDK_ROOT)
    if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "Optional Aftermath capture currently supports Windows x64 only")
    endif()
    get_filename_component(agcAftermathRoot "${ANYPS5_AFTERMATH_SDK_ROOT}" ABSOLUTE)
    foreach(header IN ITEMS GFSDK_Aftermath_GpuCrashDump.h GFSDK_Aftermath_Defines.h GFSDK_Aftermath.h)
        if(NOT EXISTS "${agcAftermathRoot}/include/${header}")
            message(FATAL_ERROR "Aftermath SDK header missing: ${agcAftermathRoot}/include/${header}")
        endif()
    endforeach()
    set(agcAftermathDll "${agcAftermathRoot}/lib/x64/GFSDK_Aftermath_Lib.x64.dll")
    if(NOT EXISTS "${agcAftermathDll}")
        message(FATAL_ERROR "Aftermath SDK DLL missing: ${agcAftermathDll}")
    endif()
    file(TO_CMAKE_PATH "${agcAftermathDll}" agcAftermathDll)
    set(agcAftermathGenerated "${CMAKE_CURRENT_BINARY_DIR}/aftermath_generated")
    file(CONFIGURE OUTPUT "${agcAftermathGenerated}/AftermathSdkPath.hpp"
        CONTENT "inline constexpr wchar_t AftermathSdkPath[] = LR\"aftermath(@agcAftermathDll@)aftermath\";\n" @ONLY)
    target_sources(libSceAgcDriver PRIVATE Graphics/src/AftermathCapture.cpp)
    target_include_directories(libSceAgcDriver PRIVATE "${agcAftermathRoot}/include" "${agcAftermathGenerated}")
    target_compile_definitions(libSceAgcDriver PRIVATE ANYPS5_AFTERMATH_ENABLED=1)
endif()

if(APS5_ENABLE_TRACY)
    target_link_libraries(libSceAgcDriver PUBLIC TracyClient)
endif()

set_target_properties(libSceAgcDriver PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(libSceAgcDriver)

add_test_executable(agc_aftermath_disabled_tests EXCLUDE_FROM_ALL tests/AftermathCapture.cpp)
target_include_directories(agc_aftermath_disabled_tests PRIVATE
        ${LIBS_INCLUDE_DIR} ${ANYPS5_VULKAN_HEADERS_INCLUDE_DIR})
target_link_libraries(agc_aftermath_disabled_tests PRIVATE libc)
set_target_properties(agc_aftermath_disabled_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_aftermath_disabled_tests)
if(BUILD_TESTING)
    set_target_properties(agc_aftermath_disabled_tests PROPERTIES EXCLUDE_FROM_ALL OFF)
    add_test(NAME agc_aftermath_disabled COMMAND agc_aftermath_disabled_tests disabled)
    set_tests_properties(agc_aftermath_disabled PROPERTIES TIMEOUT 10)
    if(WIN32)
        set_tests_properties(agc_aftermath_disabled PROPERTIES ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

if(WIN32 AND ANYPS5_AFTERMATH_SDK_ROOT)
    set(agcAftermathTestInclude "${agcAftermathRoot}/include")
    add_test_executable(agc_aftermath_capture_tests EXCLUDE_FROM_ALL
            tests/AftermathCapture.cpp Graphics/src/AftermathCapture.cpp)
    target_include_directories(agc_aftermath_capture_tests PRIVATE
            ${LIBS_INCLUDE_DIR} ${ANYPS5_VULKAN_HEADERS_INCLUDE_DIR} ${agcAftermathTestInclude})
    target_compile_definitions(agc_aftermath_capture_tests PRIVATE
            ANYPS5_AFTERMATH_ENABLED=1 ANYPS5_AFTERMATH_TESTING=1)
    target_link_libraries(agc_aftermath_capture_tests PRIVATE libc)
    set_target_properties(agc_aftermath_capture_tests PROPERTIES CXX_EXTENSIONS OFF)
    configure_windows_unwind(agc_aftermath_capture_tests)
    if(BUILD_TESTING)
        set_target_properties(agc_aftermath_capture_tests PROPERTIES EXCLUDE_FROM_ALL OFF)
        foreach(scenario IN ITEMS
                unset lifecycle original-error status-sequence collect-failed query-failed timeout
                callback-bytes callback-throws callback-io-fails held-callback held-callback-finished
                concurrent-deadline serialized-acquire abandon-locked disable-failed
                load-failed pin-failed enable-failed relative-output missing-export)
            add_test(NAME agc_aftermath_${scenario} COMMAND agc_aftermath_capture_tests ${scenario})
            set_tests_properties(agc_aftermath_${scenario} PROPERTIES TIMEOUT 10)
            set_tests_properties(agc_aftermath_${scenario} PROPERTIES ENVIRONMENT_MODIFICATION
                    "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
        endforeach()
    endif()
endif()

if(BUILD_TESTING)
    add_test_executable(agc_write_watch_coverage_tests tests/WriteWatchCoverage.cpp)
    target_include_directories(agc_write_watch_coverage_tests PRIVATE ${LIBS_INCLUDE_DIR})
    target_link_libraries(agc_write_watch_coverage_tests PRIVATE libc)
    configure_windows_unwind(agc_write_watch_coverage_tests)
    add_test(NAME agc_write_watch_coverage COMMAND agc_write_watch_coverage_tests)
    if(WIN32)
        set_tests_properties(agc_write_watch_coverage PROPERTIES ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()

    add_test_executable(agc_profile_output_tests tests/ProfileOutput.cpp)
    target_include_directories(agc_profile_output_tests PRIVATE ${LIBS_INCLUDE_DIR})
    target_link_libraries(agc_profile_output_tests PRIVATE libc)
    configure_windows_unwind(agc_profile_output_tests)
    add_test(NAME agc_profile_output COMMAND agc_profile_output_tests)
    set_tests_properties(agc_profile_output PROPERTIES TIMEOUT 20)
    if(WIN32)
        set_tests_properties(agc_profile_output PROPERTIES ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()

    add_test_executable(agc_stack_walk_tests tests/StackWalk.cpp)
    target_include_directories(agc_stack_walk_tests PRIVATE ${LIBS_INCLUDE_DIR})
    target_link_libraries(agc_stack_walk_tests PRIVATE libc)
    configure_windows_unwind(agc_stack_walk_tests)
    add_test(NAME agc_stack_walk COMMAND agc_stack_walk_tests)
    set_tests_properties(agc_stack_walk PROPERTIES TIMEOUT 20)
    if(WIN32)
        set_tests_properties(agc_stack_walk PROPERTIES ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()

    add_test_executable(agc_performance_trace_tests tests/PerformanceTrace.cpp Execution/src/TraceFile.cpp)
    target_include_directories(agc_performance_trace_tests PRIVATE ${LIBS_INCLUDE_DIR} ${agcZlibIncludeDir})
    target_link_libraries(agc_performance_trace_tests PRIVATE libc jpeg_turbo)
    configure_windows_unwind(agc_performance_trace_tests)
    add_test(NAME agc_performance_trace COMMAND agc_performance_trace_tests)
    set_tests_properties(agc_performance_trace PROPERTIES TIMEOUT 20)
    if(WIN32)
        set_tests_properties(agc_performance_trace PROPERTIES ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

if(WIN32)
    set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/GuestMemory.cpp ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Lifecycle.cpp PROPERTIES COMPILE_DEFINITIONS _WIN32_WINNT=0x0602)
endif()

foreach(runtimeLinkFlag IN ITEMS -static-libgcc -static-libstdc++ -static)
    string(REPLACE "${runtimeLinkFlag}" "" CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")
endforeach()

add_test_executable(agc_driver_register_key_tests EXCLUDE_FROM_ALL tests/RegisterKey.cpp)
target_include_directories(agc_driver_register_key_tests PRIVATE
        ${LIBS_INCLUDE_DIR}
        ${CMAKE_SOURCE_DIR}/core/shader/recompiler
        ${ANYPS5_VULKAN_HEADERS_INCLUDE_DIR}
)
target_link_libraries(agc_driver_register_key_tests PRIVATE
        libSceAgcDriver
        libc
        libkernel
)
set_target_properties(agc_driver_register_key_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_driver_register_key_tests)
if(BUILD_TESTING)
    set_target_properties(agc_driver_register_key_tests PROPERTIES EXCLUDE_FROM_ALL OFF)
    add_test(NAME agc_driver_register_key COMMAND agc_driver_register_key_tests)
    set_tests_properties(agc_driver_register_key PROPERTIES TIMEOUT 30 ENVIRONMENT "APS5_VERIFY_REGISTER_KEYS=1")
    if(WIN32)
        set_tests_properties(agc_driver_register_key PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

add_test_executable(agc_driver_tests EXCLUDE_FROM_ALL
        tests/Submit.cpp
)

target_include_directories(agc_driver_tests PRIVATE
        ${LIBS_INCLUDE_DIR}
        ${CMAKE_SOURCE_DIR}/core/shader/recompiler
        ${ANYPS5_VULKAN_HEADERS_INCLUDE_DIR}
        ${ANYPS5_SPIRV_HEADERS_INCLUDE_DIR}
)

target_link_libraries(agc_driver_tests PRIVATE
        libSceAgcDriver
        libkernel
        libc
)

set_target_properties(agc_driver_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_driver_tests)

add_test_executable(agc_driver_wait_shutdown_tests EXCLUDE_FROM_ALL tests/WaitShutdown.cpp)
target_include_directories(agc_driver_wait_shutdown_tests PRIVATE ${LIBS_INCLUDE_DIR})
target_link_libraries(agc_driver_wait_shutdown_tests PRIVATE libSceAgcDriver libkernel libc)
set_target_properties(agc_driver_wait_shutdown_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_driver_wait_shutdown_tests)
if(BUILD_TESTING)
    set_target_properties(agc_driver_wait_shutdown_tests PROPERTIES EXCLUDE_FROM_ALL OFF)
    add_test(NAME agc_driver_wait_shutdown COMMAND agc_driver_wait_shutdown_tests)
    set_tests_properties(agc_driver_wait_shutdown PROPERTIES
            ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_GPU_WAIT_TIMEOUT_MS=20"
            TIMEOUT 10 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    add_test(NAME agc_driver_resolution_shutdown COMMAND agc_driver_wait_shutdown_tests --resolution-shutdown)
    set_tests_properties(agc_driver_resolution_shutdown PROPERTIES TIMEOUT 10 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    if(WIN32)
        set_tests_properties(agc_driver_wait_shutdown agc_driver_resolution_shutdown PROPERTIES ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

add_test_executable(agc_shader_memory_tests EXCLUDE_FROM_ALL
        tests/ShaderMemory.cpp
        Execution/src/ShaderMemory.cpp
        Execution/src/GuestMemory.cpp
)
target_include_directories(agc_shader_memory_tests PRIVATE ${LIBS_INCLUDE_DIR})
target_link_libraries(agc_shader_memory_tests PRIVATE shader_recompiler_agc_driver libc)
set_target_properties(agc_shader_memory_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_shader_memory_tests)

add_test_executable(agc_shader_wave64_layout_tests EXCLUDE_FROM_ALL tests/Wave64Layout.cpp)
target_link_libraries(agc_shader_wave64_layout_tests PRIVATE shader_recompiler_agc_driver libc)
set_target_properties(agc_shader_wave64_layout_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_shader_wave64_layout_tests)

add_test_executable(agc_shared_write_tests EXCLUDE_FROM_ALL tests/SharedWrites.cpp)
target_include_directories(agc_shared_write_tests PRIVATE ${LIBS_INCLUDE_DIR})
target_link_libraries(agc_shared_write_tests PRIVATE libSceAgcDriver libkernel libc)
set_target_properties(agc_shared_write_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_shared_write_tests)

add_test_executable(agc_write_tracking_tests EXCLUDE_FROM_ALL
        tests/WriteTracking.cpp
        Execution/src/GuestMemory.cpp
)
target_include_directories(agc_write_tracking_tests PRIVATE ${LIBS_INCLUDE_DIR})
target_link_libraries(agc_write_tracking_tests PRIVATE shader_recompiler_agc_driver libc)
set_target_properties(agc_write_tracking_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_write_tracking_tests)
if(BUILD_TESTING)
    set_target_properties(agc_write_tracking_tests PROPERTIES EXCLUDE_FROM_ALL OFF)
    add_test(NAME agc_write_tracking COMMAND agc_write_tracking_tests)
    set_tests_properties(agc_write_tracking PROPERTIES SKIP_RETURN_CODE 77)
    if(WIN32)
        set_tests_properties(agc_write_tracking PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

add_test_executable(agc_thread_priority_tests EXCLUDE_FROM_ALL
        tests/ThreadPriority.cpp
        Execution/src/ThreadPriority.cpp
)
target_include_directories(agc_thread_priority_tests PRIVATE ${LIBS_INCLUDE_DIR})
target_link_libraries(agc_thread_priority_tests PRIVATE libc ${CMAKE_DL_LIBS})
set_target_properties(agc_thread_priority_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_thread_priority_tests)
if(BUILD_TESTING)
    set_target_properties(agc_thread_priority_tests PROPERTIES EXCLUDE_FROM_ALL OFF)
    add_test(NAME agc_thread_priority COMMAND agc_thread_priority_tests)
    foreach(service on off invalid)
        set(serviceMode service-off)
        set(serviceValue 0)
        if(service STREQUAL "on")
            set(serviceMode service-on)
            set(serviceValue 1)
        elseif(service STREQUAL "invalid")
            set(serviceValue yes)
        endif()
        add_test(NAME agc_thread_priority_service_${service} COMMAND agc_thread_priority_tests ${serviceMode})
        set_tests_properties(agc_thread_priority_service_${service} PROPERTIES ENVIRONMENT
            "APS5_SERVICE_THREAD_PRIORITY=${serviceValue};DBUS_SYSTEM_BUS_ADDRESS=unix:path=/nonexistent/aps5-no-rtkit")
        if(WIN32)
            set_tests_properties(agc_thread_priority_service_${service} PROPERTIES ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
        endif()
    endforeach()
    if(WIN32)
        set_tests_properties(agc_thread_priority PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    else()
        foreach(mode rt high)
            add_test(NAME agc_thread_priority_refused_${mode} COMMAND agc_thread_priority_tests refused)
            set_tests_properties(agc_thread_priority_refused_${mode} PROPERTIES ENVIRONMENT
                "APS5_THREAD_PRIORITY=${mode};DBUS_SYSTEM_BUS_ADDRESS=unix:path=/nonexistent/aps5-no-rtkit")
        endforeach()
    endif()
endif()

add_test_executable(agc_structurizer_tests EXCLUDE_FROM_ALL tests/Structurizer.cpp)
target_link_libraries(agc_structurizer_tests PRIVATE shader_recompiler_agc_driver libc)
set_target_properties(agc_structurizer_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_structurizer_tests)

add_test_executable(agc_fragment_barycentric_tests EXCLUDE_FROM_ALL tests/FragmentBarycentric.cpp)
target_include_directories(agc_fragment_barycentric_tests PRIVATE ${LIBS_INCLUDE_DIR} ${ANYPS5_VULKAN_HEADERS_INCLUDE_DIR})
set_target_properties(agc_fragment_barycentric_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_fragment_barycentric_tests)
if(BUILD_TESTING)
    set_target_properties(agc_fragment_barycentric_tests PROPERTIES EXCLUDE_FROM_ALL OFF)
    add_test(NAME agc_fragment_barycentric COMMAND agc_fragment_barycentric_tests)
endif()

add_test_executable(agc_pipeline_link_optimization_tests EXCLUDE_FROM_ALL tests/PipelineLinkOptimization.cpp)
target_include_directories(agc_pipeline_link_optimization_tests PRIVATE ${LIBS_INCLUDE_DIR} ${ANYPS5_VULKAN_HEADERS_INCLUDE_DIR})
set_target_properties(agc_pipeline_link_optimization_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_pipeline_link_optimization_tests)
if(BUILD_TESTING)
    set_target_properties(agc_pipeline_link_optimization_tests PROPERTIES EXCLUDE_FROM_ALL OFF)
    add_test(NAME agc_pipeline_link_optimization COMMAND agc_pipeline_link_optimization_tests)
endif()

add_test_executable(agc_device_fault_tests EXCLUDE_FROM_ALL tests/DeviceFault.cpp)
target_include_directories(agc_device_fault_tests PRIVATE ${LIBS_INCLUDE_DIR} ${ANYPS5_VULKAN_HEADERS_INCLUDE_DIR})
target_link_libraries(agc_device_fault_tests PRIVATE libc libkernel)
set_target_properties(agc_device_fault_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_device_fault_tests)
if(BUILD_TESTING)
    set_target_properties(agc_device_fault_tests PROPERTIES EXCLUDE_FROM_ALL OFF)
    add_test(NAME agc_device_fault COMMAND agc_device_fault_tests)
    if(WIN32)
        set_tests_properties(agc_device_fault PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

add_test_executable(agc_subgroup_clock_tests EXCLUDE_FROM_ALL
        tests/SubgroupClock.cpp
        Execution/src/SubgroupClock.cpp
)
target_include_directories(agc_subgroup_clock_tests PRIVATE ${LIBS_INCLUDE_DIR} ${ANYPS5_VULKAN_HEADERS_INCLUDE_DIR})
set_target_properties(agc_subgroup_clock_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_subgroup_clock_tests)
if(BUILD_TESTING)
    set_target_properties(agc_subgroup_clock_tests PROPERTIES EXCLUDE_FROM_ALL OFF)
    add_test(NAME agc_subgroup_clock COMMAND agc_subgroup_clock_tests)
endif()

add_test_executable(agc_tessellation_factor_ring_tests EXCLUDE_FROM_ALL
        tests/TessellationFactorRing.cpp
        State/src/Configuration.cpp
)
target_include_directories(agc_tessellation_factor_ring_tests PRIVATE ${LIBS_INCLUDE_DIR})
target_link_libraries(agc_tessellation_factor_ring_tests PRIVATE libc)
set_target_properties(agc_tessellation_factor_ring_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_tessellation_factor_ring_tests)
if(BUILD_TESTING)
    set_target_properties(agc_tessellation_factor_ring_tests PROPERTIES EXCLUDE_FROM_ALL OFF)
    add_test(NAME agc_tessellation_factor_ring COMMAND agc_tessellation_factor_ring_tests)
    if(WIN32)
        set_tests_properties(agc_tessellation_factor_ring PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

add_test_executable(agc_spirv_module_tests EXCLUDE_FROM_ALL tests/SpirvModule.cpp)
target_link_libraries(agc_spirv_module_tests PRIVATE shader_recompiler_agc_driver libc)
set_target_properties(agc_spirv_module_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_spirv_module_tests)

add_test_executable(agc_spirv_narrow_constant_store_tests EXCLUDE_FROM_ALL tests/SpirvNarrowConstantStore.cpp)
target_link_libraries(agc_spirv_narrow_constant_store_tests PRIVATE shader_recompiler_agc_driver libc)
set_target_properties(agc_spirv_narrow_constant_store_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_spirv_narrow_constant_store_tests)
if(ANYPS5_ENABLE_SPIRV_TOOLS)
    target_link_libraries(agc_spirv_narrow_constant_store_tests PRIVATE SPIRV-Tools-static)
    add_test_executable(agc_spirv13_image_tests EXCLUDE_FROM_ALL tests/Spirv13Image.cpp)
    target_link_libraries(agc_spirv13_image_tests PRIVATE shader_recompiler_agc_driver libc)
    set_target_properties(agc_spirv13_image_tests PROPERTIES CXX_EXTENSIONS OFF)
    configure_windows_unwind(agc_spirv13_image_tests)
    add_test_executable(agc_spirv_optimizer_tests EXCLUDE_FROM_ALL tests/SpirvOptimizer.cpp)
    target_link_libraries(agc_spirv_optimizer_tests PRIVATE shader_recompiler_agc_driver libc SPIRV-Tools-static)
    set_target_properties(agc_spirv_optimizer_tests PROPERTIES CXX_EXTENSIONS OFF)
    configure_windows_unwind(agc_spirv_optimizer_tests)
endif()

add_test_executable(agc_float16_conversion_tests EXCLUDE_FROM_ALL tests/Float16Conversion.cpp)
target_link_libraries(agc_float16_conversion_tests PRIVATE shader_recompiler_agc_driver libc)
set_target_properties(agc_float16_conversion_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_float16_conversion_tests)
if(BUILD_TESTING)
    set_target_properties(agc_float16_conversion_tests PROPERTIES EXCLUDE_FROM_ALL OFF)
    add_test(NAME agc_float16_conversion COMMAND agc_float16_conversion_tests)
    set_tests_properties(agc_float16_conversion PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1")
    if(WIN32)
        set_tests_properties(agc_float16_conversion PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

add_test_executable(agc_spirv_branch_targets_tests EXCLUDE_FROM_ALL tests/SpirvBranchTargets.cpp)
target_link_libraries(agc_spirv_branch_targets_tests PRIVATE shader_recompiler_agc_driver libc)
set_target_properties(agc_spirv_branch_targets_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_spirv_branch_targets_tests)
if(BUILD_TESTING)
    set_target_properties(agc_spirv_branch_targets_tests PROPERTIES EXCLUDE_FROM_ALL OFF)
    add_test(NAME agc_spirv_branch_targets COMMAND agc_spirv_branch_targets_tests)
    set_tests_properties(agc_spirv_branch_targets PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1")
    if(WIN32)
        set_tests_properties(agc_spirv_branch_targets PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

add_test_executable(agc_spirv_lds_atomic_lock_tests EXCLUDE_FROM_ALL tests/SpirvLdsAtomicLock.cpp)
target_link_libraries(agc_spirv_lds_atomic_lock_tests PRIVATE shader_recompiler_agc_driver libc)
set_target_properties(agc_spirv_lds_atomic_lock_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_spirv_lds_atomic_lock_tests)
if(BUILD_TESTING)
    set_target_properties(agc_spirv_lds_atomic_lock_tests PROPERTIES EXCLUDE_FROM_ALL OFF)
    add_test(NAME agc_spirv_lds_atomic_lock COMMAND agc_spirv_lds_atomic_lock_tests)
    set_tests_properties(agc_spirv_lds_atomic_lock PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1")
    if(WIN32)
        set_tests_properties(agc_spirv_lds_atomic_lock PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

add_test_executable(agc_dpp_write_tests EXCLUDE_FROM_ALL tests/DppWrite.cpp)
target_link_libraries(agc_dpp_write_tests PRIVATE shader_recompiler_agc_driver libc)
set_target_properties(agc_dpp_write_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_dpp_write_tests)

add_test_executable(agc_masked_select_tests EXCLUDE_FROM_ALL tests/MaskedSelect.cpp)
target_link_libraries(agc_masked_select_tests PRIVATE shader_recompiler_agc_driver libc)
set_target_properties(agc_masked_select_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_masked_select_tests)

add_test_executable(agc_ir_reclaim_tests EXCLUDE_FROM_ALL tests/IrReclaim.cpp)
target_link_libraries(agc_ir_reclaim_tests PRIVATE shader_recompiler_agc_driver libc)
set_target_properties(agc_ir_reclaim_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_ir_reclaim_tests)

add_test_executable(agc_vector_window_tests EXCLUDE_FROM_ALL tests/VectorWindow.cpp)
target_link_libraries(agc_vector_window_tests PRIVATE shader_recompiler_agc_driver libc)
set_target_properties(agc_vector_window_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_vector_window_tests)

add_test_executable(agc_constant_bit_test_tests EXCLUDE_FROM_ALL tests/ConstantBitTest.cpp)
target_link_libraries(agc_constant_bit_test_tests PRIVATE shader_recompiler_agc_driver libc)
set_target_properties(agc_constant_bit_test_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_constant_bit_test_tests)

add_test_executable(agc_integer_narrowing_tests EXCLUDE_FROM_ALL tests/IntegerNarrowing.cpp)
target_link_libraries(agc_integer_narrowing_tests PRIVATE shader_recompiler_agc_driver libc)
set_target_properties(agc_integer_narrowing_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_integer_narrowing_tests)

add_test_executable(agc_scalar_select_mask_tests EXCLUDE_FROM_ALL tests/ScalarSelectMask.cpp)
target_link_libraries(agc_scalar_select_mask_tests PRIVATE shader_recompiler_agc_driver libc)
set_target_properties(agc_scalar_select_mask_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_scalar_select_mask_tests)

add_test_executable(agc_wave_control_decode_tests EXCLUDE_FROM_ALL tests/WaveControlDecode.cpp)
target_include_directories(agc_wave_control_decode_tests PRIVATE ${LIBS_INCLUDE_DIR})
target_link_libraries(agc_wave_control_decode_tests PRIVATE shader_recompiler_agc_driver libc)
set_target_properties(agc_wave_control_decode_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_wave_control_decode_tests)
if(BUILD_TESTING)
    set_target_properties(agc_wave_control_decode_tests PROPERTIES EXCLUDE_FROM_ALL OFF)
    add_test(NAME agc_wave_control_decode COMMAND agc_wave_control_decode_tests)
    if(WIN32)
        set_tests_properties(agc_wave_control_decode PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

add_test_executable(agc_denormal_flush_elimination_tests EXCLUDE_FROM_ALL tests/DenormalFlushElimination.cpp)
target_link_libraries(agc_denormal_flush_elimination_tests PRIVATE shader_recompiler_agc_driver libc)
set_target_properties(agc_denormal_flush_elimination_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_denormal_flush_elimination_tests)
if(BUILD_TESTING)
    set_target_properties(agc_denormal_flush_elimination_tests PROPERTIES EXCLUDE_FROM_ALL OFF)
    add_test(NAME agc_denormal_flush_elimination COMMAND agc_denormal_flush_elimination_tests)
    if(WIN32)
        set_tests_properties(agc_denormal_flush_elimination PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

add_test_executable(agc_redundant_value_elimination_tests EXCLUDE_FROM_ALL tests/RedundantValueElimination.cpp)
target_link_libraries(agc_redundant_value_elimination_tests PRIVATE shader_recompiler_agc_driver libc)
set_target_properties(agc_redundant_value_elimination_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_redundant_value_elimination_tests)
if(BUILD_TESTING)
    set_target_properties(agc_redundant_value_elimination_tests PROPERTIES EXCLUDE_FROM_ALL OFF)
    add_test(NAME agc_redundant_value_elimination COMMAND agc_redundant_value_elimination_tests)
    if(WIN32)
        set_tests_properties(agc_redundant_value_elimination PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

add_test_executable(agc_vop1_control_decode_tests EXCLUDE_FROM_ALL tests/Vop1ControlDecode.cpp)
target_link_libraries(agc_vop1_control_decode_tests PRIVATE shader_recompiler_agc_driver libc)
set_target_properties(agc_vop1_control_decode_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_vop1_control_decode_tests)
add_test_executable(agc_vop2_encoding_decode_tests EXCLUDE_FROM_ALL tests/Vop2EncodingDecode.cpp)
target_link_libraries(agc_vop2_encoding_decode_tests PRIVATE shader_recompiler_agc_driver libc)
set_target_properties(agc_vop2_encoding_decode_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_vop2_encoding_decode_tests)
if(BUILD_TESTING)
    set_target_properties(agc_vop2_encoding_decode_tests PROPERTIES EXCLUDE_FROM_ALL OFF)
    add_test(NAME agc_vop2_encoding_decode COMMAND agc_vop2_encoding_decode_tests)
    if(WIN32)
        set_tests_properties(agc_vop2_encoding_decode PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

add_test_executable(agc_vop3_interp_decode_tests EXCLUDE_FROM_ALL tests/Vop3InterpDecode.cpp)
target_link_libraries(agc_vop3_interp_decode_tests PRIVATE shader_recompiler_agc_driver libc)
set_target_properties(agc_vop3_interp_decode_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_vop3_interp_decode_tests)
if(BUILD_TESTING)
    set_target_properties(agc_vop3_interp_decode_tests PROPERTIES EXCLUDE_FROM_ALL OFF)
    add_test(NAME agc_vop3_interp_decode COMMAND agc_vop3_interp_decode_tests)
    if(WIN32)
        set_tests_properties(agc_vop3_interp_decode PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

add_test_executable(agc_pass_hazards_tests EXCLUDE_FROM_ALL tests/PassHazards.cpp Graphics/src/PassHazards.cpp)
target_include_directories(agc_pass_hazards_tests PRIVATE ${LIBS_INCLUDE_DIR} ${ANYPS5_VULKAN_HEADERS_INCLUDE_DIR})
target_link_libraries(agc_pass_hazards_tests PRIVATE libc)
set_target_properties(agc_pass_hazards_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_pass_hazards_tests)

add_test_executable(agc_dcc_key_sharing_tests EXCLUDE_FROM_ALL tests/DccKeySharing.cpp)
target_include_directories(agc_dcc_key_sharing_tests PRIVATE ${LIBS_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/core/shader/recompiler ${ANYPS5_VULKAN_HEADERS_INCLUDE_DIR})
target_link_libraries(agc_dcc_key_sharing_tests PRIVATE libc)
set_target_properties(agc_dcc_key_sharing_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_dcc_key_sharing_tests)
if(BUILD_TESTING)
    set_target_properties(agc_dcc_key_sharing_tests PROPERTIES EXCLUDE_FROM_ALL OFF)
    add_test(NAME agc_dcc_key_sharing COMMAND agc_dcc_key_sharing_tests)
    if(WIN32)
        set_tests_properties(agc_dcc_key_sharing PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

add_test_executable(agc_buffer_pool_tests EXCLUDE_FROM_ALL tests/BufferPool.cpp)
target_include_directories(agc_buffer_pool_tests PRIVATE ${LIBS_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/core/shader/recompiler ${ANYPS5_VULKAN_HEADERS_INCLUDE_DIR})
target_link_libraries(agc_buffer_pool_tests PRIVATE libSceAgcDriver libc)
set_target_properties(agc_buffer_pool_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_buffer_pool_tests)
if(BUILD_TESTING)
    set_target_properties(agc_buffer_pool_tests PROPERTIES EXCLUDE_FROM_ALL OFF)
    add_test(NAME agc_buffer_pool COMMAND agc_buffer_pool_tests)
    if(WIN32)
        set_tests_properties(agc_buffer_pool PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

add_test_executable(agc_device_memory_fallback_tests EXCLUDE_FROM_ALL tests/DeviceMemoryFallback.cpp)
target_include_directories(agc_device_memory_fallback_tests PRIVATE ${LIBS_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/core/shader/recompiler ${ANYPS5_VULKAN_HEADERS_INCLUDE_DIR})
target_link_libraries(agc_device_memory_fallback_tests PRIVATE libSceAgcDriver libc)
set_target_properties(agc_device_memory_fallback_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_device_memory_fallback_tests)
if(BUILD_TESTING)
    set_target_properties(agc_device_memory_fallback_tests PROPERTIES EXCLUDE_FROM_ALL OFF)
    add_test(NAME agc_device_memory_fallback COMMAND agc_device_memory_fallback_tests)
    if(WIN32)
        set_tests_properties(agc_device_memory_fallback PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

add_test_executable(agc_display_buffer_dcc_tests EXCLUDE_FROM_ALL tests/DisplayBufferDcc.cpp)
target_include_directories(agc_display_buffer_dcc_tests PRIVATE ${LIBS_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/core/shader/recompiler ${ANYPS5_VULKAN_HEADERS_INCLUDE_DIR})
target_link_libraries(agc_display_buffer_dcc_tests PRIVATE libSceAgcDriver libc)
set_target_properties(agc_display_buffer_dcc_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_display_buffer_dcc_tests)
if(BUILD_TESTING)
    set_target_properties(agc_display_buffer_dcc_tests PROPERTIES EXCLUDE_FROM_ALL OFF)
    add_test(NAME agc_display_buffer_dcc COMMAND agc_display_buffer_dcc_tests)
    if(WIN32)
        set_tests_properties(agc_display_buffer_dcc PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

add_test_executable(agc_validate_program_tests EXCLUDE_FROM_ALL tests/ValidateProgram.cpp)
target_link_libraries(agc_validate_program_tests PRIVATE shader_recompiler_agc_driver libc)
set_target_properties(agc_validate_program_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_validate_program_tests)

add_test_executable(agc_formatted_buffer_once_tests EXCLUDE_FROM_ALL tests/FormattedBufferOnce.cpp)
target_link_libraries(agc_formatted_buffer_once_tests PRIVATE shader_recompiler_agc_driver libc)
set_target_properties(agc_formatted_buffer_once_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_formatted_buffer_once_tests)

add_test_executable(agc_fma_mix_once_tests EXCLUDE_FROM_ALL tests/FmaMixOnce.cpp)
target_link_libraries(agc_fma_mix_once_tests PRIVATE shader_recompiler_agc_driver libc)
set_target_properties(agc_fma_mix_once_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_fma_mix_once_tests)

add_test_executable(agc_rect_list_position_invariant_tests EXCLUDE_FROM_ALL tests/RectListPositionInvariant.cpp)
target_link_libraries(agc_rect_list_position_invariant_tests PRIVATE shader_recompiler_agc_driver libc)
set_target_properties(agc_rect_list_position_invariant_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_rect_list_position_invariant_tests)
if(BUILD_TESTING)
    set_target_properties(agc_rect_list_position_invariant_tests PROPERTIES EXCLUDE_FROM_ALL OFF)
    add_test(NAME agc_rect_list_position_invariant COMMAND agc_rect_list_position_invariant_tests)
    if(WIN32)
        set_tests_properties(agc_rect_list_position_invariant PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

add_test_executable(agc_formatted_buffer_byte_stores_tests EXCLUDE_FROM_ALL tests/FormattedBufferByteStores.cpp)
target_link_libraries(agc_formatted_buffer_byte_stores_tests PRIVATE shader_recompiler_agc_driver libc)
set_target_properties(agc_formatted_buffer_byte_stores_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_formatted_buffer_byte_stores_tests)

add_test_executable(agc_shader_disk_cache_tests EXCLUDE_FROM_ALL tests/ShaderDiskCache.cpp)
target_include_directories(agc_shader_disk_cache_tests PRIVATE ${LIBS_INCLUDE_DIR})
target_link_libraries(agc_shader_disk_cache_tests PRIVATE shader_recompiler_agc_driver libc)
set_target_properties(agc_shader_disk_cache_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_shader_disk_cache_tests)
if(BUILD_TESTING)
    add_test(NAME agc_shader_specialized_disk_cache COMMAND agc_shader_disk_cache_tests --specialized-suite)
    set_tests_properties(agc_shader_specialized_disk_cache PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1")
    if(WIN32)
        set_tests_properties(agc_shader_specialized_disk_cache PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

add_test_executable(agc_shared_spirv_tests EXCLUDE_FROM_ALL tests/SharedSpirv.cpp ${PROJECT_SOURCE_DIR}/core/shader/recompiler/SharedSpirv.cpp)
target_include_directories(agc_shared_spirv_tests PRIVATE ${PROJECT_SOURCE_DIR}/core/shader/recompiler)
set_target_properties(agc_shared_spirv_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_shared_spirv_tests)
if(MINGW)
    target_link_libraries(agc_shared_spirv_tests PRIVATE libc)
endif()
if(WIN32)
    target_link_libraries(agc_shared_spirv_tests PRIVATE psapi)
endif()
if(BUILD_TESTING)
    set_target_properties(agc_shared_spirv_tests PROPERTIES EXCLUDE_FROM_ALL OFF)
    add_test(NAME agc_shared_spirv_heap COMMAND agc_shared_spirv_tests)
    add_test(NAME agc_shared_spirv_file COMMAND agc_shared_spirv_tests)
    set_tests_properties(agc_shared_spirv_heap PROPERTIES ENVIRONMENT_MODIFICATION "APS5_FILE_BACKED_SPIRV=set:0")
    set_tests_properties(agc_shared_spirv_file PROPERTIES ENVIRONMENT_MODIFICATION "APS5_FILE_BACKED_SPIRV=set:1")
    if(MINGW)
        set_property(TEST agc_shared_spirv_heap agc_shared_spirv_file APPEND PROPERTY ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${UNPATCHED_DIR}")
    endif()
endif()


add_test_executable(agc_placeholder_images_tests EXCLUDE_FROM_ALL tests/PlaceholderImages.cpp)
target_include_directories(agc_placeholder_images_tests PRIVATE ${LIBS_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/3rdparty/Vulkan-Headers/include)
target_link_libraries(agc_placeholder_images_tests PRIVATE shader_recompiler_agc_driver libc)
set_target_properties(agc_placeholder_images_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_placeholder_images_tests)

add_test_executable(agc_shader_device_profile_tests EXCLUDE_FROM_ALL tests/ShaderDeviceProfile.cpp Execution/src/ShaderDeviceProfile.cpp)
target_include_directories(agc_shader_device_profile_tests PRIVATE ${LIBS_INCLUDE_DIR} ${ANYPS5_VULKAN_HEADERS_INCLUDE_DIR})
target_link_libraries(agc_shader_device_profile_tests PRIVATE shader_recompiler_agc_driver libc)
set_target_properties(agc_shader_device_profile_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_shader_device_profile_tests)

add_test_executable(agc_shader_control_flow_tests EXCLUDE_FROM_ALL
        tests/ControlFlow.cpp
)
target_include_directories(agc_shader_control_flow_tests PRIVATE ${LIBS_INCLUDE_DIR})
target_link_libraries(agc_shader_control_flow_tests PRIVATE shader_recompiler_agc_driver libc)
set_target_properties(agc_shader_control_flow_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_shader_control_flow_tests)

add_test_executable(agc_shader_mode_register_tests EXCLUDE_FROM_ALL tests/ShaderModeRegisters.cpp)
target_include_directories(agc_shader_mode_register_tests PRIVATE ${LIBS_INCLUDE_DIR})
target_link_libraries(agc_shader_mode_register_tests PRIVATE shader_recompiler_agc_driver libc)
set_target_properties(agc_shader_mode_register_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_shader_mode_register_tests)

add_test_executable(agc_driver_flip_tests EXCLUDE_FROM_ALL
        tests/Flip.cpp
)

target_include_directories(agc_driver_flip_tests PRIVATE
        ${LIBS_INCLUDE_DIR}
        ${CMAKE_SOURCE_DIR}/3rdparty/Vulkan-Headers/include
)

target_link_libraries(agc_driver_flip_tests PRIVATE
        libSceAgcDriver
        libc
)

set_target_properties(agc_driver_flip_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_driver_flip_tests)

set(agcSampleLodShaderSource ${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/SampleLod.comp)
set(agcTestShaderGeneratedRoot ${CMAKE_CURRENT_BINARY_DIR}/generated-tests)
set(agcSampleLodShaderSpv ${agcTestShaderGeneratedRoot}/SampleLod.spv)
set(agcSampleLodShaderHeader ${agcTestShaderGeneratedRoot}/SampleLod_spv.h)
add_custom_command(
        OUTPUT "${agcSampleLodShaderHeader}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${agcTestShaderGeneratedRoot}"
        COMMAND "$<TARGET_FILE:glslang-standalone>" -V --target-env vulkan1.1 -o "${agcSampleLodShaderSpv}" "${agcSampleLodShaderSource}"
        COMMAND ${CMAKE_COMMAND} -DINPUT=${agcSampleLodShaderSpv} -DOUTPUT=${agcSampleLodShaderHeader}
        -DSYMBOL=SAMPLE_LOD_SPV -P "${CMAKE_CURRENT_SOURCE_DIR}/embed_spirv.cmake"
        DEPENDS "${agcSampleLodShaderSource}" glslang-standalone
        VERBATIM
)

set(agcSampleArrayShaderSource ${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/SampleArray.comp)
set(agcSampleArrayShaderSpv ${agcTestShaderGeneratedRoot}/SampleArray.spv)
set(agcSampleArrayShaderHeader ${agcTestShaderGeneratedRoot}/SampleArray_spv.h)
add_custom_command(
        OUTPUT "${agcSampleArrayShaderHeader}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${agcTestShaderGeneratedRoot}"
        COMMAND "$<TARGET_FILE:glslang-standalone>" -V --target-env vulkan1.1 -o "${agcSampleArrayShaderSpv}" "${agcSampleArrayShaderSource}"
        COMMAND ${CMAKE_COMMAND} -DINPUT=${agcSampleArrayShaderSpv} -DOUTPUT=${agcSampleArrayShaderHeader}
        -DSYMBOL=SAMPLE_ARRAY_SPV -P "${CMAKE_CURRENT_SOURCE_DIR}/embed_spirv.cmake"
        DEPENDS "${agcSampleArrayShaderSource}" glslang-standalone
        VERBATIM
)

foreach(agcTriangleStage IN ITEMS vert frag)
    set(agcTriangleSource ${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/Triangle.${agcTriangleStage})
    set(agcTriangleSpv ${agcTestShaderGeneratedRoot}/Triangle_${agcTriangleStage}.spv)
    set(agcTriangleHeader ${agcTestShaderGeneratedRoot}/Triangle_${agcTriangleStage}_spv.h)
    add_custom_command(
            OUTPUT "${agcTriangleHeader}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${agcTestShaderGeneratedRoot}"
            COMMAND "$<TARGET_FILE:glslang-standalone>" -V --target-env vulkan1.1 -o "${agcTriangleSpv}" "${agcTriangleSource}"
            COMMAND ${CMAKE_COMMAND} -DINPUT=${agcTriangleSpv} -DOUTPUT=${agcTriangleHeader}
            -DSYMBOL=TRIANGLE_${agcTriangleStage}_SPV -P "${CMAKE_CURRENT_SOURCE_DIR}/embed_spirv.cmake"
            DEPENDS "${agcTriangleSource}" glslang-standalone
            VERBATIM
    )
    list(APPEND agcTriangleHeaders "${agcTriangleHeader}")
endforeach()

foreach(agcCoverStage IN ITEMS vert frag)
    string(TOUPPER "${agcCoverStage}" agcCoverSymbol)
    set(agcCoverShaderSpv ${agcTestShaderGeneratedRoot}/Cover_${agcCoverStage}.spv)
    set(agcCoverShaderHeader ${agcTestShaderGeneratedRoot}/Cover_${agcCoverStage}_spv.h)
    add_custom_command(
            OUTPUT "${agcCoverShaderHeader}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${agcTestShaderGeneratedRoot}"
            COMMAND "$<TARGET_FILE:glslang-standalone>" -V --target-env vulkan1.1 -o "${agcCoverShaderSpv}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/Cover.${agcCoverStage}"
            COMMAND ${CMAKE_COMMAND} -DINPUT=${agcCoverShaderSpv} -DOUTPUT=${agcCoverShaderHeader}
            -DSYMBOL=COVER_${agcCoverSymbol}_SPV -P "${CMAKE_CURRENT_SOURCE_DIR}/embed_spirv.cmake"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/Cover.${agcCoverStage}" glslang-standalone
            VERBATIM
    )
    list(APPEND agcCoverShaderHeaders ${agcCoverShaderHeader})
endforeach()

add_test_executable(agc_driver_cmask_fast_clear_tests EXCLUDE_FROM_ALL
        tests/CmaskFastClear.cpp
        ${agcCoverShaderHeaders}
)

target_include_directories(agc_driver_cmask_fast_clear_tests PRIVATE
        ${LIBS_INCLUDE_DIR}
        ${agcTestShaderGeneratedRoot}
        ${CMAKE_SOURCE_DIR}/core/shader/recompiler
        ${ANYPS5_VULKAN_HEADERS_INCLUDE_DIR}
)

target_link_libraries(agc_driver_cmask_fast_clear_tests PRIVATE
        libSceAgcDriver
        SDL2-static
        libc
)

set_target_properties(agc_driver_cmask_fast_clear_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_driver_cmask_fast_clear_tests)

set(agcBufferUpdateSource ${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/BufferUpdate.comp)
set(agcBufferUpdateSpv ${agcTestShaderGeneratedRoot}/BufferUpdate.spv)
set(agcBufferUpdateHeader ${agcTestShaderGeneratedRoot}/BufferUpdate_spv.h)
add_custom_command(
        OUTPUT "${agcBufferUpdateHeader}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${agcTestShaderGeneratedRoot}"
        COMMAND "$<TARGET_FILE:glslang-standalone>" -V --target-env vulkan1.1 -o "${agcBufferUpdateSpv}" "${agcBufferUpdateSource}"
        COMMAND ${CMAKE_COMMAND} -DINPUT=${agcBufferUpdateSpv} -DOUTPUT=${agcBufferUpdateHeader}
        -DSYMBOL=BUFFER_UPDATE_SPV -P "${CMAKE_CURRENT_SOURCE_DIR}/embed_spirv.cmake"
        DEPENDS "${agcBufferUpdateSource}" glslang-standalone
        VERBATIM
)

add_test_executable(agc_driver_recorder_tests EXCLUDE_FROM_ALL
        tests/Recorder.cpp
        tests/ResidentHeapMirror.cpp
        tests/BdaShader.cpp
        tests/ResidentPresent.cpp
        ${agcTriangleHeaders}
        ${agcSampleLodShaderHeader}
        ${agcSampleArrayShaderHeader}
        ${agcCoverShaderHeaders}
        ${agcBufferUpdateHeader}
)

target_include_directories(agc_driver_recorder_tests PRIVATE
        ${LIBS_INCLUDE_DIR}
        ${agcTestShaderGeneratedRoot}
        ${CMAKE_SOURCE_DIR}/core/shader/recompiler
        ${ANYPS5_VULKAN_HEADERS_INCLUDE_DIR}
)

target_link_libraries(agc_driver_recorder_tests PRIVATE
        shader_recompiler_agc_driver
        libkernel
        libSceAgcDriver
        SDL2-static
        libc
)

set_target_properties(agc_driver_recorder_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_driver_recorder_tests)
if(BUILD_TESTING)
    set_target_properties(agc_driver_recorder_tests PROPERTIES EXCLUDE_FROM_ALL OFF)
    if(WIN32)
        foreach(residentHeapMode IN ITEMS off on)
            add_test(NAME agc_resident_heap_${residentHeapMode} COMMAND agc_driver_recorder_tests --resident-heap-only)
            set_tests_properties(agc_resident_heap_${residentHeapMode} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60
                ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>;APS5_RESIDENT_HEAP_MIRRORS=unset:")
            if(residentHeapMode STREQUAL "on")
                set_property(TEST agc_resident_heap_${residentHeapMode} APPEND PROPERTY ENVIRONMENT_MODIFICATION "APS5_RESIDENT_HEAP_MIRRORS=set:1")
            endif()
        endforeach()
    endif()
    add_test(NAME agc_pipeline_cache COMMAND agc_driver_recorder_tests --pipeline-cache-only)
    add_test(NAME agc_pipeline_cache_file COMMAND agc_driver_recorder_tests --pipeline-cache-file-only)
    add_test(NAME agc_pipeline_library COMMAND agc_driver_recorder_tests --pipeline-library-only)
    add_test(NAME agc_pipeline_library_off COMMAND agc_driver_recorder_tests --pipeline-library-only)
    set_tests_properties(agc_pipeline_library agc_pipeline_library_off PROPERTIES SKIP_RETURN_CODE 77)
    set_tests_properties(agc_pipeline_library_off PROPERTIES ENVIRONMENT "APS5_NO_GPL=1")
    add_test(NAME agc_pipeline_library_fast_only COMMAND agc_driver_recorder_tests --pipeline-library-only)
    set_tests_properties(agc_pipeline_library_fast_only PROPERTIES ENVIRONMENT "APS5_NO_PIPELINE_LTO=1" SKIP_RETURN_CODE 77)
    if(WIN32)
        set_tests_properties(agc_pipeline_library agc_pipeline_library_off agc_pipeline_library_fast_only PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    set_tests_properties(agc_pipeline_cache agc_pipeline_cache_file PROPERTIES SKIP_RETURN_CODE 77)
    if(WIN32)
        set_tests_properties(agc_pipeline_cache agc_pipeline_cache_file PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test(NAME agc_unchanged_cpu_stamps COMMAND agc_driver_recorder_tests --unchanged-cpu-stamps-only)
    set_tests_properties(agc_unchanged_cpu_stamps PROPERTIES SKIP_RETURN_CODE 77)
    if(WIN32)
        set_tests_properties(agc_unchanged_cpu_stamps PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test(NAME agc_storage_kept_unit_settle COMMAND agc_driver_recorder_tests --kept-unit-settle-only)
    set_tests_properties(agc_storage_kept_unit_settle PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
    if(WIN32)
        set_tests_properties(agc_storage_kept_unit_settle PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test(NAME agc_storage_baseline_settled COMMAND agc_driver_recorder_tests --settled-baseline-only)
    add_test(NAME agc_storage_integer_clear COMMAND agc_driver_recorder_tests --integer-clear-only)
    set_tests_properties(agc_storage_integer_clear PROPERTIES SKIP_RETURN_CODE 77)
    if(WIN32)
        set_tests_properties(agc_storage_integer_clear PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test(NAME agc_storage_upload_lifetime COMMAND agc_driver_recorder_tests --storage-upload-lifetime-only)
    add_test(NAME agc_sampled_storage_base_level_usage COMMAND agc_driver_recorder_tests --sampled-storage-base-level-usage-only)
    set_tests_properties(agc_sampled_storage_base_level_usage PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
    if(WIN32)
        set_tests_properties(agc_sampled_storage_base_level_usage PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test(NAME agc_storage_upload_failure COMMAND agc_driver_recorder_tests --storage-upload-failure-only)
    set_tests_properties(agc_storage_upload_lifetime agc_storage_upload_failure PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
    if(WIN32)
        set_tests_properties(agc_storage_upload_lifetime agc_storage_upload_failure PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    set_tests_properties(agc_storage_baseline_settled PROPERTIES SKIP_RETURN_CODE 77)
    if(WIN32)
        set_tests_properties(agc_storage_baseline_settled PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test(NAME agc_readonly_fallback_reuse COMMAND agc_driver_recorder_tests --readonly-fallback-only)
    set_tests_properties(agc_readonly_fallback_reuse PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
    if(WIN32)
        set_tests_properties(agc_readonly_fallback_reuse PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test(NAME agc_storage_written_buffer_order COMMAND agc_driver_recorder_tests --written-buffer-order-only)
    set_tests_properties(agc_storage_written_buffer_order PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
    if(WIN32)
        set_tests_properties(agc_storage_written_buffer_order PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test(NAME agc_copied_direct_ordering COMMAND agc_driver_recorder_tests --copied-direct-only)
    set_tests_properties(agc_copied_direct_ordering PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
    if(WIN32)
        set_tests_properties(agc_copied_direct_ordering PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test(NAME agc_host_import_unmap COMMAND agc_driver_recorder_tests --host-import-unmap-only)
    set_tests_properties(agc_host_import_unmap PROPERTIES SKIP_RETURN_CODE 77)
    add_test(NAME agc_parked_import_unmap COMMAND agc_driver_recorder_tests --parked-import-unmap-only)
    set_tests_properties(agc_parked_import_unmap PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30 ENVIRONMENT "APS5_PIN_WAIT_MS=3000")
    add_test(NAME agc_host_import_retirement COMMAND agc_driver_recorder_tests --host-import-retirement-only)
    set_tests_properties(agc_host_import_retirement PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
    add_test(NAME agc_sparse_run_import COMMAND agc_driver_recorder_tests --sparse-run-import-only)
    set_tests_properties(agc_sparse_run_import PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
    add_test(NAME agc_sparse_run_import_copied COMMAND agc_driver_recorder_tests --sparse-run-import-only)
    set_tests_properties(agc_sparse_run_import_copied PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30 ENVIRONMENT "APS5_NO_SPARSE_RUN_IMPORT=1")
    add_test(NAME agc_unit_shadow COMMAND agc_driver_recorder_tests --unit-shadow-only)
    set_tests_properties(agc_unit_shadow PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30 ENVIRONMENT "APS5_PIN_WAIT_MS=3000")
    if(WIN32)
        add_test(NAME agc_storage_cpu_snapshot COMMAND agc_driver_recorder_tests --storage-cpu-snapshot-only)
        set_tests_properties(agc_storage_cpu_snapshot PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1" TIMEOUT 30 SKIP_RETURN_CODE 77
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test(NAME agc_descriptor_pool_recycling COMMAND agc_driver_recorder_tests --descriptor-pool-only)
    set_tests_properties(agc_descriptor_pool_recycling PROPERTIES SKIP_RETURN_CODE 77)
    add_test(NAME agc_draw_snapshot_cap COMMAND agc_driver_recorder_tests --draw-snapshot-cap-only)
    set_tests_properties(agc_draw_snapshot_cap PROPERTIES SKIP_RETURN_CODE 77)
    add_test(NAME agc_multisample_guest_access COMMAND agc_driver_recorder_tests --multisample-only)
    set_tests_properties(agc_multisample_guest_access PROPERTIES SKIP_RETURN_CODE 77)
    add_test(NAME agc_draw_inputs_in_place COMMAND agc_driver_recorder_tests --draw-inputs-in-place-only)
    add_test(NAME agc_pending_write_snapshot COMMAND agc_driver_recorder_tests --pending-write-snapshot-only)
    set_tests_properties(agc_pending_write_snapshot PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
    if(WIN32)
        set_tests_properties(agc_pending_write_snapshot PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    set_tests_properties(agc_draw_inputs_in_place PROPERTIES SKIP_RETURN_CODE 77)
    add_test(NAME agc_space_pending_writes COMMAND agc_driver_recorder_tests --space-pending-writes-only)
    set_tests_properties(agc_space_pending_writes PROPERTIES SKIP_RETURN_CODE 77)
    if(WIN32)
        set_tests_properties(agc_host_import_unmap agc_parked_import_unmap agc_host_import_retirement agc_sparse_run_import agc_sparse_run_import_copied agc_unit_shadow agc_descriptor_pool_recycling agc_draw_snapshot_cap agc_multisample_guest_access agc_draw_inputs_in_place agc_space_pending_writes PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test(NAME agc_pending_key_stores COMMAND agc_driver_recorder_tests --pending-key-stores-only)
    set_tests_properties(agc_pending_key_stores PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
    add_test(NAME agc_shadowed_key_stores COMMAND agc_driver_recorder_tests --shadowed-key-stores-only)
    set_tests_properties(agc_shadowed_key_stores PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
    add_test(NAME agc_metadata_pass COMMAND agc_driver_recorder_tests --metadata-pass-only)
    set_tests_properties(agc_metadata_pass PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
    if(WIN32)
        set_tests_properties(agc_pending_key_stores agc_shadowed_key_stores agc_metadata_pass PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

add_test_executable(agc_recorder_kept_bytes_tests EXCLUDE_FROM_ALL tests/RecorderKeptBytes.cpp)
target_include_directories(agc_recorder_kept_bytes_tests PRIVATE ${LIBS_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/core/shader/recompiler ${ANYPS5_VULKAN_HEADERS_INCLUDE_DIR})
target_link_libraries(agc_recorder_kept_bytes_tests PRIVATE libSceAgcDriver libc)
set_target_properties(agc_recorder_kept_bytes_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_recorder_kept_bytes_tests)
if(BUILD_TESTING)
    set_target_properties(agc_recorder_kept_bytes_tests PROPERTIES EXCLUDE_FROM_ALL OFF)
    add_test(NAME agc_recorder_kept_bytes COMMAND agc_recorder_kept_bytes_tests)
    add_test(NAME agc_recorder_kept_bytes_background_off COMMAND agc_recorder_kept_bytes_tests)
    set_tests_properties(agc_recorder_kept_bytes_background_off PROPERTIES ENVIRONMENT "APS5_BACKGROUND_RELEASE=0")
    add_test(NAME agc_recorder_kept_bytes_background COMMAND agc_recorder_kept_bytes_tests background)
    set_tests_properties(agc_recorder_kept_bytes_background PROPERTIES ENVIRONMENT "APS5_BACKGROUND_RELEASE=1" TIMEOUT 15)
    if(WIN32)
        set_tests_properties(agc_recorder_kept_bytes agc_recorder_kept_bytes_background_off agc_recorder_kept_bytes_background PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

add_test_executable(agc_host_import_retire_tests EXCLUDE_FROM_ALL tests/HostImportRetireHold.cpp)
target_include_directories(agc_host_import_retire_tests PRIVATE ${LIBS_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/core/shader/recompiler ${ANYPS5_VULKAN_HEADERS_INCLUDE_DIR})
target_link_libraries(agc_host_import_retire_tests PRIVATE libSceAgcDriver libc)
set_target_properties(agc_host_import_retire_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_host_import_retire_tests)
if(BUILD_TESTING)
    set_target_properties(agc_host_import_retire_tests PROPERTIES EXCLUDE_FROM_ALL OFF)
    add_test(NAME agc_host_import_retire COMMAND agc_host_import_retire_tests)
    if(WIN32)
        set_tests_properties(agc_host_import_retire PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

add_test_executable(agc_driver_pm4_tests EXCLUDE_FROM_ALL
        tests/Pm4.cpp
)

target_include_directories(agc_driver_pm4_tests PRIVATE
        ${LIBS_INCLUDE_DIR}
)

target_link_libraries(agc_driver_pm4_tests PRIVATE
        libSceAgcDriver
        libc
        libkernel
)

set_target_properties(agc_driver_pm4_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_driver_pm4_tests)

add_test_executable(agc_driver_graphics_tests EXCLUDE_FROM_ALL
        tests/Graphics.cpp
        tests/ColorTargetLayout.cpp
        tests/BdaResources.cpp
        tests/GuestAllocations.cpp
        tests/LiveStack.cpp
        tests/TextureFormat.cpp
        tests/TextureTiling.cpp
        tests/GuestTextureResource.cpp
        tests/GuestSamplerResource.cpp
        tests/TextureDetiler.cpp
        tests/DepthSurfaceReuse.cpp
)

target_include_directories(agc_driver_graphics_tests PRIVATE
        ${LIBS_INCLUDE_DIR}
        $<TARGET_PROPERTY:shader_recompiler_agc_driver,INTERFACE_INCLUDE_DIRECTORIES>
        ${agcTextureDetileGeneratedRoot}
        ${CMAKE_SOURCE_DIR}/core/shader/recompiler
        ${CMAKE_SOURCE_DIR}/core/shader/recompiler/IntermediateRepresentation/include
        ${CMAKE_SOURCE_DIR}/core/shader/recompiler/ControlFlow/include
        ${CMAKE_SOURCE_DIR}/core/shader/recompiler/RdnaDecoder/include
        ${ANYPS5_VULKAN_HEADERS_INCLUDE_DIR}
        ${ANYPS5_SPIRV_HEADERS_INCLUDE_DIR}
)

target_link_libraries(agc_driver_graphics_tests PRIVATE
        libSceAgcDriver
        libc
        libkernel
)

set_target_properties(agc_driver_graphics_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_driver_graphics_tests)

foreach(agcVertexRangeShader IN ITEMS VertexRange.vert VertexRangeProducer.frag)
    string(REPLACE "." "_" agcVertexRangeName "${agcVertexRangeShader}")
    string(TOUPPER "${agcVertexRangeName}_SPV" agcVertexRangeSymbol)
    set(agcVertexRangeSpv ${agcTestShaderGeneratedRoot}/${agcVertexRangeName}.spv)
    set(agcVertexRangeHeader ${agcTestShaderGeneratedRoot}/${agcVertexRangeName}_spv.h)
    add_custom_command(
            OUTPUT "${agcVertexRangeHeader}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${agcTestShaderGeneratedRoot}"
            COMMAND "$<TARGET_FILE:glslang-standalone>" -V --target-env vulkan1.1 -o "${agcVertexRangeSpv}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/${agcVertexRangeShader}"
            COMMAND ${CMAKE_COMMAND} -DINPUT=${agcVertexRangeSpv} -DOUTPUT=${agcVertexRangeHeader}
            -DSYMBOL=${agcVertexRangeSymbol} -P "${CMAKE_CURRENT_SOURCE_DIR}/embed_spirv.cmake"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/${agcVertexRangeShader}" glslang-standalone
            VERBATIM
    )
    list(APPEND agcVertexRangeHeaders ${agcVertexRangeHeader})
endforeach()

target_sources(agc_driver_recorder_tests PRIVATE ${agcVertexRangeHeaders})

if(BUILD_TESTING)
    add_test(NAME agc_driver_vertex_target_range_copied COMMAND agc_driver_recorder_tests --vertex-target-range-copied)
    add_test(NAME agc_driver_vertex_target_range_imported COMMAND agc_driver_recorder_tests --vertex-target-range-imported)
    set_tests_properties(agc_driver_vertex_target_range_copied agc_driver_vertex_target_range_imported PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
    if(WIN32)
        set_tests_properties(agc_driver_vertex_target_range_copied agc_driver_vertex_target_range_imported PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

if(WIN32 AND ANYPS5_AFTERMATH_SDK_ROOT)
    add_test_executable(agc_aftermath_recorder_release_tests EXCLUDE_FROM_ALL
        tests/RecorderKeptBytes.cpp Graphics/src/Recorder.cpp Graphics/src/AftermathCapture.cpp)
    target_include_directories(agc_aftermath_recorder_release_tests PRIVATE
        ${LIBS_INCLUDE_DIR} ${agcTextureDetileGeneratedRoot} ${agcAftermathTestInclude}
        $<TARGET_PROPERTY:shader_recompiler_agc_driver,INTERFACE_INCLUDE_DIRECTORIES>
        ${CMAKE_SOURCE_DIR}/core/shader/recompiler ${ANYPS5_VULKAN_HEADERS_INCLUDE_DIR} ${ANYPS5_SPIRV_HEADERS_INCLUDE_DIR})
    target_compile_definitions(agc_aftermath_recorder_release_tests PRIVATE
            ANYPS5_AFTERMATH_ENABLED=1 ANYPS5_AFTERMATH_TESTING=1)
    target_link_libraries(agc_aftermath_recorder_release_tests PRIVATE libSceAgcDriver shader_recompiler_agc_driver libc libkernel)
    set_target_properties(agc_aftermath_recorder_release_tests PROPERTIES CXX_EXTENSIONS OFF)
    configure_windows_unwind(agc_aftermath_recorder_release_tests)
    if(BUILD_TESTING)
        set_target_properties(agc_aftermath_recorder_release_tests PROPERTIES EXCLUDE_FROM_ALL OFF)
        add_test(NAME agc_aftermath_recorder_release COMMAND agc_aftermath_recorder_release_tests)
        set_tests_properties(agc_aftermath_recorder_release PROPERTIES TIMEOUT 10)
        if(WIN32)
            set_tests_properties(agc_aftermath_recorder_release PROPERTIES ENVIRONMENT_MODIFICATION
                    "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>;PATH=path_list_prepend:$<TARGET_FILE_DIR:libkernel>")
        endif()
    endif()
endif()


add_test_executable(agc_driver_bda_device_tests EXCLUDE_FROM_ALL
        tests/BdaDevice.cpp
        tests/BdaShader.cpp
        tests/BdaExecution.cpp
        tests/BdaContracts.cpp
)

target_include_directories(agc_driver_bda_device_tests PRIVATE
        ${LIBS_INCLUDE_DIR}
        $<TARGET_PROPERTY:shader_recompiler_agc_driver,INTERFACE_INCLUDE_DIRECTORIES>
        ${CMAKE_SOURCE_DIR}/core/shader/recompiler
        ${ANYPS5_VULKAN_HEADERS_INCLUDE_DIR}
)
target_link_libraries(agc_driver_bda_device_tests PRIVATE libSceAgcDriver SDL2-static libc)
set_target_properties(agc_driver_bda_device_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_driver_bda_device_tests)

add_test_executable(agc_driver_mesh_tests EXCLUDE_FROM_ALL tests/Mesh.cpp)
target_include_directories(agc_driver_mesh_tests PRIVATE
        ${LIBS_INCLUDE_DIR}
        ${CMAKE_SOURCE_DIR}/core/shader/recompiler
        ${ANYPS5_VULKAN_HEADERS_INCLUDE_DIR}
        ${ANYPS5_SPIRV_HEADERS_INCLUDE_DIR}
)
target_link_libraries(agc_driver_mesh_tests PRIVATE libSceAgcDriver SDL2-static libc)
set_target_properties(agc_driver_mesh_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_driver_mesh_tests)

set(ANYPS5_AGC_EXECUTION_TESTS "" CACHE STRING "tests/execution sources to build, by file name without extension; the others build a stub that reports a skip. Empty builds all")
file(GLOB agcExecutionTestSources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/execution/*.cpp)
set(agcExecutionTests)
foreach(agcExecutionTestSource IN LISTS agcExecutionTestSources)
    get_filename_component(agcExecutionTestName ${agcExecutionTestSource} NAME_WE)
    if(ANYPS5_AGC_EXECUTION_TESTS AND NOT agcExecutionTestName IN_LIST ANYPS5_AGC_EXECUTION_TESTS)
        set(agcExecutionTestSource ${CMAKE_CURRENT_SOURCE_DIR}/tests/ExecutionTestNotBuilt.cpp)
    endif()
    string(REGEX REPLACE "([a-z0-9])([A-Z])" "\\1_\\2" agcExecutionTestName "${agcExecutionTestName}")
    string(TOLOWER "agc_driver_${agcExecutionTestName}_tests" agcExecutionTest)
    add_test_executable(${agcExecutionTest} EXCLUDE_FROM_ALL ${agcExecutionTestSource})
    target_include_directories(${agcExecutionTest} PRIVATE
            ${LIBS_INCLUDE_DIR}
            ${CMAKE_SOURCE_DIR}/core/shader/recompiler
            ${ANYPS5_VULKAN_HEADERS_INCLUDE_DIR}
            ${ANYPS5_SPIRV_HEADERS_INCLUDE_DIR}
    )
    target_link_libraries(${agcExecutionTest} PRIVATE libSceAgcDriver SDL2-static libc)
    set_target_properties(${agcExecutionTest} PROPERTIES CXX_EXTENSIONS OFF)
    configure_windows_unwind(${agcExecutionTest})
    list(APPEND agcExecutionTests ${agcExecutionTest})
endforeach()
target_include_directories(agc_driver_draw_offset_indices_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/core/shader/recompiler/RdnaDecoder/include
        ${CMAKE_SOURCE_DIR}/core/shader/recompiler/Translation/include)
target_include_directories(agc_driver_indirect_instance_offset_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/core/shader/recompiler/RdnaDecoder/include
        ${CMAKE_SOURCE_DIR}/core/shader/recompiler/Translation/include)
foreach(agcExecutionTestName IN LISTS ANYPS5_AGC_EXECUTION_TESTS)
    if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/execution/${agcExecutionTestName}.cpp)
        message(FATAL_ERROR "ANYPS5_AGC_EXECUTION_TESTS names ${agcExecutionTestName}, which has no source in tests/execution")
    endif()
endforeach()

target_link_libraries(agc_driver_gpu_map_on_fiber_tests PRIVATE libSceFiber)
if(BUILD_TESTING)
    add_test(NAME agc_driver_depth_zero_reads COMMAND agc_driver_depth_plane_sampling_tests --zero-only)
    add_test(NAME agc_driver_depth_large_array COMMAND agc_driver_depth_plane_sampling_tests --large-only)
    add_test(NAME agc_driver_depth_large_array_no_memo COMMAND agc_driver_depth_plane_sampling_tests --large-only)
    set_tests_properties(agc_driver_depth_zero_reads agc_driver_depth_large_array agc_driver_depth_large_array_no_memo PROPERTIES
            ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_ASSERT_GPU_LOCK=1" SKIP_RETURN_CODE 77 TIMEOUT 30 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_property(TEST agc_driver_depth_large_array_no_memo APPEND PROPERTY ENVIRONMENT "APS5_NO_DEPTH_UPLOAD_MEMO=1")
    if(WIN32)
        set_tests_properties(agc_driver_depth_zero_reads agc_driver_depth_large_array agc_driver_depth_large_array_no_memo PROPERTIES
                ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test(NAME agc_driver_gpu_address_bindings_overlap COMMAND agc_driver_gpu_address_bindings_tests --overlap-compile-only)
    set_tests_properties(agc_driver_gpu_address_bindings_overlap PROPERTIES
            ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1" SKIP_RETURN_CODE 77 TIMEOUT 30 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    if(WIN32)
        set_tests_properties(agc_driver_gpu_address_bindings_overlap PROPERTIES
                ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test(NAME agc_driver_image_indirect_arguments_written_ahead_thread_dimensions
            COMMAND agc_driver_image_indirect_arguments_written_ahead_tests --thread-dimensions)
    set_tests_properties(agc_driver_image_indirect_arguments_written_ahead_thread_dimensions PROPERTIES
            ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1" SKIP_RETURN_CODE 77 TIMEOUT 30 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    if(WIN32)
        set_tests_properties(agc_driver_image_indirect_arguments_written_ahead_thread_dimensions PROPERTIES
                ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

target_include_directories(agc_driver_alpha_to_coverage_tests PRIVATE $<TARGET_PROPERTY:shader_recompiler_agc_driver,INTERFACE_INCLUDE_DIRECTORIES>)

target_include_directories(agc_driver_dual_source_blend_tests PRIVATE $<TARGET_PROPERTY:shader_recompiler_agc_driver,INTERFACE_INCLUDE_DIRECTORIES>)

target_include_directories(agc_driver_swap_alt_blend_tests PRIVATE $<TARGET_PROPERTY:shader_recompiler_agc_driver,INTERFACE_INCLUDE_DIRECTORIES>)

target_include_directories(agc_driver_packed_unorm_color_target_tests PRIVATE $<TARGET_PROPERTY:shader_recompiler_agc_driver,INTERFACE_INCLUDE_DIRECTORIES>)

target_include_directories(agc_driver_color1555_target_tests PRIVATE $<TARGET_PROPERTY:shader_recompiler_agc_driver,INTERFACE_INCLUDE_DIRECTORIES>)

foreach(stage IN ITEMS vert frag)
    set(agcProvokingShaderSource ${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/ProvokingVertex.${stage})
    set(agcProvokingShaderSpv ${agcTestShaderGeneratedRoot}/ProvokingVertex_${stage}.spv)
    set(agcProvokingShaderHeader ${agcTestShaderGeneratedRoot}/ProvokingVertex_${stage}_spv.h)
    string(TOUPPER "PROVOKING_VERTEX_${stage}_SPV" agcProvokingShaderSymbol)
    add_custom_command(
        OUTPUT "${agcProvokingShaderHeader}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${agcTestShaderGeneratedRoot}"
        COMMAND "$<TARGET_FILE:glslang-standalone>" -V --target-env vulkan1.1 -o "${agcProvokingShaderSpv}" "${agcProvokingShaderSource}"
        COMMAND ${CMAKE_COMMAND} -DINPUT=${agcProvokingShaderSpv} -DOUTPUT=${agcProvokingShaderHeader}
        -DSYMBOL=${agcProvokingShaderSymbol} -P "${CMAKE_CURRENT_SOURCE_DIR}/embed_spirv.cmake"
        DEPENDS "${agcProvokingShaderSource}" glslang-standalone
        VERBATIM
    )
    target_sources(agc_driver_provoking_vertex_tests PRIVATE ${agcProvokingShaderHeader})
endforeach()
target_include_directories(agc_driver_provoking_vertex_tests PRIVATE ${agcTestShaderGeneratedRoot})

target_include_directories(agc_driver_prepared_exports_tests PRIVATE $<TARGET_PROPERTY:shader_recompiler_agc_driver,INTERFACE_INCLUDE_DIRECTORIES>)

target_include_directories(agc_driver_buffer_descriptor_runtime_tests PRIVATE $<TARGET_PROPERTY:shader_recompiler_agc_driver,INTERFACE_INCLUDE_DIRECTORIES>)

target_include_directories(agc_driver_prepared_publication_tests PRIVATE $<TARGET_PROPERTY:shader_recompiler_agc_driver,INTERFACE_INCLUDE_DIRECTORIES>)

target_include_directories(agc_driver_prepared_graphics_tests PRIVATE $<TARGET_PROPERTY:shader_recompiler_agc_driver,INTERFACE_INCLUDE_DIRECTORIES>)
target_link_libraries(agc_driver_interpolation_modes_tests PRIVATE shader_recompiler_agc_driver)

target_include_directories(agc_driver_prepared_unregistered_graphics_tests PRIVATE $<TARGET_PROPERTY:shader_recompiler_agc_driver,INTERFACE_INCLUDE_DIRECTORIES>)

target_include_directories(agc_driver_prepared_shaders_tests PRIVATE
        $<TARGET_PROPERTY:shader_recompiler_agc_driver,INTERFACE_INCLUDE_DIRECTORIES>
)

if(BUILD_TESTING)
    foreach(preparedBudget IN ITEMS zero lru)
        add_test(NAME agc_driver_prepared_shader_budget_${preparedBudget} COMMAND agc_driver_prepared_shaders_tests --budget-${preparedBudget})
        set_tests_properties(agc_driver_prepared_shader_budget_${preparedBudget} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180 ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_LAZY_SHADER_PREPARE=1")
        if(WIN32)
            set_property(TEST agc_driver_prepared_shader_budget_${preparedBudget} PROPERTY ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
        endif()
    endforeach()
    set_property(TEST agc_driver_prepared_shader_budget_zero APPEND PROPERTY ENVIRONMENT "APS5_PREPARED_SHADER_MIB=0" "APS5_SUSPEND_DRAIN=1")
    set_property(TEST agc_driver_prepared_shader_budget_lru APPEND PROPERTY ENVIRONMENT "APS5_PREPARED_SHADER_MIB=1")
    foreach(preparedMaintenance IN ITEMS raw profile)
        add_test(NAME agc_driver_prepared_shader_maintenance_${preparedMaintenance} COMMAND agc_driver_prepared_shaders_tests --budget-maintenance)
        set_tests_properties(agc_driver_prepared_shader_maintenance_${preparedMaintenance} PROPERTIES TIMEOUT 30 ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_PREPARED_SHADER_MIB=1")
        if(WIN32)
            set_property(TEST agc_driver_prepared_shader_maintenance_${preparedMaintenance} PROPERTY ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
        endif()
    endforeach()
    set_property(TEST agc_driver_prepared_shader_maintenance_profile APPEND PROPERTY ENVIRONMENT "APS5_PROFILE_DRAW=1")
    add_test(NAME agc_driver_prepared_shader_memo_memory COMMAND agc_driver_prepared_shaders_tests --memo-memory)
    set_tests_properties(agc_driver_prepared_shader_memo_memory PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30 ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_PREPARED_SHADER_MIB=0")
    if(WIN32)
        set_property(TEST agc_driver_prepared_shader_memo_memory PROPERTY ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

if(BUILD_TESTING)
    set_target_properties(agc_shader_mode_register_tests PROPERTIES EXCLUDE_FROM_ALL OFF)
    add_test(NAME agc_driver_readonly_resummarize COMMAND agc_driver_tests --readonly-resummarize)
    set_tests_properties(agc_driver_readonly_resummarize PROPERTIES TIMEOUT 10)
    if(WIN32)
        set_tests_properties(agc_driver_readonly_resummarize PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test(NAME agc_shader_mode_register COMMAND agc_shader_mode_register_tests)
    set_tests_properties(agc_shader_mode_register PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1" WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" TIMEOUT 20)
    if(WIN32)
        set_tests_properties(agc_shader_mode_register PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test(NAME agc_shader_bindless COMMAND agc_shader_memory_tests --bindless)
    set_tests_properties(agc_shader_bindless PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1" WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    add_test(NAME agc_shader_srt_read_patching COMMAND agc_shader_memory_tests --srt-read-patching)
    set_tests_properties(agc_shader_srt_read_patching PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1" WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    add_test(NAME agc_shader_type_zero_image_materialization COMMAND agc_shader_memory_tests --type-zero-image-materialization)
    set_tests_properties(agc_shader_type_zero_image_materialization PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1" WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    add_test(NAME agc_shader_frontend_pair COMMAND agc_shader_memory_tests --frontend-pair)
    add_test(NAME agc_shader_frontend_pair_off COMMAND agc_shader_memory_tests --frontend-pair)
    set_tests_properties(agc_shader_frontend_pair PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1" WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(agc_shader_frontend_pair_off PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_NO_PERF_FRONTEND_PAIR=1" WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    if(WIN32)
        set_tests_properties(agc_shader_bindless agc_shader_srt_read_patching agc_shader_type_zero_image_materialization agc_shader_frontend_pair agc_shader_frontend_pair_off PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    foreach(agcTest IN ITEMS agc_shared_write_tests agc_driver_tests agc_shader_memory_tests agc_shader_wave64_layout_tests agc_structurizer_tests agc_spirv_module_tests agc_dpp_write_tests agc_masked_select_tests agc_ir_reclaim_tests agc_vector_window_tests agc_constant_bit_test_tests agc_integer_narrowing_tests agc_scalar_select_mask_tests agc_vop1_control_decode_tests agc_validate_program_tests agc_pass_hazards_tests agc_shader_disk_cache_tests agc_shader_device_profile_tests agc_placeholder_images_tests agc_shader_control_flow_tests agc_driver_flip_tests agc_driver_pm4_tests agc_driver_graphics_tests agc_driver_bda_device_tests agc_driver_mesh_tests agc_driver_cmask_fast_clear_tests agc_formatted_buffer_once_tests agc_formatted_buffer_byte_stores_tests ${agcExecutionTests} agc_fma_mix_once_tests)
        if(agcTest STREQUAL "agc_driver_constant_store_tests" OR agcTest STREQUAL "agc_driver_flat_apertures_tests" OR agcTest STREQUAL "agc_driver_pipeline_optimized_readback_tests")
            continue()
        endif()
        string(REGEX REPLACE "_tests$" "" agcTestName "${agcTest}")
        set_target_properties(${agcTest} PROPERTIES EXCLUDE_FROM_ALL OFF)
        add_test(NAME ${agcTestName} COMMAND ${agcTest})
        set_tests_properties(${agcTestName} PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1" SKIP_RETURN_CODE 77 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
        if(WIN32)
            set_tests_properties(${agcTestName} PROPERTIES ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
        endif()
    endforeach()
    foreach(releaseSnapshots IN ITEMS 0 1)
        add_test(NAME agc_driver_snapshot_retention_${releaseSnapshots} COMMAND agc_driver_image_table_reuse_tests --snapshot-retention)
        set_tests_properties(agc_driver_snapshot_retention_${releaseSnapshots} PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_RELEASE_TEXTURE_SNAPSHOTS=${releaseSnapshots}" SKIP_RETURN_CODE 77 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" TIMEOUT 30)
        if(WIN32)
            set_tests_properties(agc_driver_snapshot_retention_${releaseSnapshots} PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
        endif()
        # The same retention checks with snapshot buffers reused (APS5_TEXTURE_SNAPSHOT_POOL_MIB).
        add_test(NAME agc_driver_snapshot_pool_${releaseSnapshots} COMMAND agc_driver_image_table_reuse_tests --snapshot-retention)
        set_tests_properties(agc_driver_snapshot_pool_${releaseSnapshots} PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_RELEASE_TEXTURE_SNAPSHOTS=${releaseSnapshots};APS5_TEXTURE_SNAPSHOT_POOL_MIB=64;APS5_TEXTURE_CACHE_REPORT=1" SKIP_RETURN_CODE 77 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" TIMEOUT 30)
        if(WIN32)
            set_tests_properties(agc_driver_snapshot_pool_${releaseSnapshots} PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
        endif()
    endforeach()
    # The sampled texture budget with only the storage images in use kept out of it.
    add_test(NAME agc_driver_graphics_storage_in_use COMMAND agc_driver_graphics_tests)
    set_tests_properties(agc_driver_graphics_storage_in_use PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_SAMPLED_BUDGET_STORAGE_IN_USE=1" SKIP_RETURN_CODE 77 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    if(WIN32)
        set_tests_properties(agc_driver_graphics_storage_in_use PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test(NAME agc_driver_partially_resident_snapshot_pool COMMAND agc_driver_partially_resident_snapshot_tests)
    set_tests_properties(agc_driver_partially_resident_snapshot_pool PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_TEXTURE_SNAPSHOT_POOL_MIB=64" SKIP_RETURN_CODE 77 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" TIMEOUT 30)
    if(WIN32)
        set_tests_properties(agc_driver_partially_resident_snapshot_pool PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test(NAME agc_driver_image_table_reuse_across_epochs COMMAND agc_driver_image_table_reuse_tests)
    set_tests_properties(agc_driver_image_table_reuse_across_epochs PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_TABLE_REUSE_ACROSS_EPOCHS=1" SKIP_RETURN_CODE 77 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" TIMEOUT 30)
    if(WIN32)
        set_tests_properties(agc_driver_image_table_reuse_across_epochs PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    foreach(mode IN ITEMS zero eviction overflow)
        set(pinBudget 1)
        set(textureBudget 1)
        set(pinArgument --pin-budget)
        if(mode STREQUAL zero)
            set(pinBudget 0)
        elseif(mode STREQUAL overflow)
            set(textureBudget 4)
            set(pinArgument --pin-overflow)
        endif()
        add_test(NAME agc_driver_image_table_pins_${mode} COMMAND agc_driver_image_table_reuse_tests ${pinArgument})
        set_tests_properties(agc_driver_image_table_pins_${mode} PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_TABLE_REUSE_ACROSS_EPOCHS=1;APS5_TABLE_PIN_MIB=${pinBudget};APS5_TEXTURE_CACHE_MIB=${textureBudget}" SKIP_RETURN_CODE 77 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" TIMEOUT 30)
        if(WIN32)
            set_tests_properties(agc_driver_image_table_pins_${mode} PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
        endif()
    endforeach()
    add_test(NAME agc_driver_partially_resident_pending_stores COMMAND agc_driver_partially_resident_snapshot_tests --pending-stores)
    set_tests_properties(agc_driver_partially_resident_pending_stores PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1" SKIP_RETURN_CODE 77 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" TIMEOUT 30)
    if(WIN32)
        set_tests_properties(agc_driver_partially_resident_pending_stores PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test(NAME agc_sparse_buffer_holes COMMAND agc_driver_graphics_tests --sparse-buffer-holes)
    set_tests_properties(agc_sparse_buffer_holes PROPERTIES TIMEOUT 30)
    set_tests_properties(agc_driver_sparse_buffer_holes PROPERTIES TIMEOUT 30)
    if(WIN32)
        set_tests_properties(agc_sparse_buffer_holes PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test(NAME agc_driver_emulated_color_compare_srgb_decode COMMAND agc_driver_emulated_color_compare_tests)
    set_tests_properties(agc_driver_emulated_color_compare_srgb_decode PROPERTIES
            ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_SRGB_SHADER_DECODE=1"
            SKIP_RETURN_CODE 77 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(agc_driver_emulated_color_compare agc_driver_emulated_color_compare_srgb_decode PROPERTIES TIMEOUT 30)
    if(WIN32)
        set_tests_properties(agc_driver_emulated_color_compare_srgb_decode PROPERTIES ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test(NAME agc_shared_write_partial_collect COMMAND agc_shared_write_tests --partial-collect)
    set_tests_properties(agc_shared_write_partial_collect PROPERTIES SKIP_RETURN_CODE 77 ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1" WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" TIMEOUT 30)
    if(WIN32)
        add_test(NAME agc_shared_write_partial_collect_two_pass COMMAND agc_shared_write_tests --partial-collect)
        set_tests_properties(agc_shared_write_partial_collect_two_pass PROPERTIES SKIP_RETURN_CODE 77 ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_NO_SINGLE_PASS_COLLECT=1" WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" TIMEOUT 30)
        set_tests_properties(agc_shared_write_partial_collect agc_shared_write_partial_collect_two_pass PROPERTIES ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    set_target_properties(agc_driver_pipeline_optimized_readback_tests PROPERTIES EXCLUDE_FROM_ALL OFF)
    foreach(pipelineReadbackCase IN ITEMS d32 d16 color32 color64)
        foreach(pipelineReadbackMode IN ITEMS default fast)
            set(pipelineReadbackTest agc_driver_pipeline_optimized_readback_${pipelineReadbackCase})
            set(pipelineReadbackEnvironment "ANYPS5_NO_SHADER_CACHE=1")
            if(pipelineReadbackMode STREQUAL "fast")
                string(APPEND pipelineReadbackTest "_fast")
                list(APPEND pipelineReadbackEnvironment "APS5_NO_PIPELINE_LTO=1")
            endif()
            add_test(NAME ${pipelineReadbackTest} COMMAND agc_driver_pipeline_optimized_readback_tests ${pipelineReadbackCase})
            set_tests_properties(${pipelineReadbackTest} PROPERTIES ENVIRONMENT "${pipelineReadbackEnvironment}" SKIP_RETURN_CODE 77 TIMEOUT 30 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
            if(WIN32)
                set_tests_properties(${pipelineReadbackTest} PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
            endif()
        endforeach()
    endforeach()
    add_test(NAME agc_vector_window_stage_inputs COMMAND agc_vector_window_tests --stage-inputs-only)
    set_tests_properties(agc_vector_window_stage_inputs PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1" WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(agc_vector_window_stage_inputs agc_driver_relative_stage_inputs PROPERTIES TIMEOUT 30)
    if(WIN32)
        set_tests_properties(agc_vector_window_stage_inputs PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test(NAME agc_driver_layered_volume_vertex COMMAND agc_driver_layered_volume_draw_tests --vertex-only)
    add_test(NAME agc_driver_layered_volume_vertex_no_gpl COMMAND agc_driver_layered_volume_draw_tests --vertex-only)
    set_tests_properties(agc_driver_layered_volume_vertex agc_driver_layered_volume_vertex_no_gpl PROPERTIES
            ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1" SKIP_RETURN_CODE 77 TIMEOUT 30 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_property(TEST agc_driver_layered_volume_vertex_no_gpl APPEND PROPERTY ENVIRONMENT "APS5_NO_GPL=1")
    if(WIN32)
        set_tests_properties(agc_driver_layered_volume_vertex agc_driver_layered_volume_vertex_no_gpl PROPERTIES
                ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test(NAME agc_driver_f64_denorm_preserve_spirv COMMAND agc_driver_f64_fma_subnormals_tests --verify-spv-denorm-mode)
    set_tests_properties(agc_driver_f64_denorm_preserve_spirv PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1" WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    if(WIN32)
        set_tests_properties(agc_driver_f64_denorm_preserve_spirv PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test(NAME agc_driver_f16_to_f32_unpack_spirv COMMAND agc_driver_f16_to_f32_unpack_tests --verify-spirv)
    set_tests_properties(agc_driver_f16_to_f32_unpack_spirv PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1" WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    if(WIN32)
        set_tests_properties(agc_driver_f16_to_f32_unpack_spirv PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test(NAME agc_structurizer_intersection_off COMMAND agc_structurizer_tests)
    set_tests_properties(agc_structurizer_intersection_off PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_NO_PERF_DOM_INTERSECTION=1" WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    add_test(NAME agc_structurizer_iteration_selection_off COMMAND agc_structurizer_tests)
    set_tests_properties(agc_structurizer_iteration_selection_off PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_NO_PERF_ITERATION_SELECTION=1" WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    add_test(NAME agc_shader_control_flow_iteration_selection_off COMMAND agc_shader_control_flow_tests)
    set_tests_properties(agc_shader_control_flow_iteration_selection_off PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_NO_PERF_ITERATION_SELECTION=1" WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    if(WIN32)
        set_tests_properties(agc_structurizer_intersection_off agc_structurizer_iteration_selection_off agc_shader_control_flow_iteration_selection_off PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test(NAME agc_driver_gated_dispatch_capture_short_timeout COMMAND agc_driver_gated_dispatch_capture_tests)
    set_tests_properties(agc_driver_gated_dispatch_capture_short_timeout PROPERTIES
            ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_GPU_WAIT_TIMEOUT_MS=1;APS5_NO_PIPELINE_LTO=1"
            SKIP_RETURN_CODE 77 TIMEOUT 30 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    if(WIN32)
        set_tests_properties(agc_driver_gated_dispatch_capture_short_timeout PROPERTIES ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    set_tests_properties(agc_driver_provoking_vertex PROPERTIES TIMEOUT 30)
    add_test(NAME agc_driver_uint8x4_color_target_no_resident COMMAND agc_driver_uint8x4_color_target_tests)
    set_tests_properties(agc_driver_uint8x4_color_target_no_resident PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_NO_RESIDENT_TARGETS=1" SKIP_RETURN_CODE 77 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" TIMEOUT 30)
    if(WIN32)
        set_tests_properties(agc_driver_uint8x4_color_target_no_resident PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    set_tests_properties(agc_driver_flat_store PROPERTIES RUN_SERIAL TRUE)
    set_target_properties(agc_driver_flat_apertures_tests PROPERTIES EXCLUDE_FROM_ALL OFF)
    foreach(flatAperturesPart RANGE 13)
        add_test(NAME agc_driver_flat_apertures_${flatAperturesPart} COMMAND agc_driver_flat_apertures_tests --part ${flatAperturesPart}/14)
        set_tests_properties(agc_driver_flat_apertures_${flatAperturesPart} PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1" SKIP_RETURN_CODE 77 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
        if(WIN32)
            set_tests_properties(agc_driver_flat_apertures_${flatAperturesPart} PROPERTIES ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
        endif()
    endforeach()
    set_tests_properties(agc_driver_storage_cache_budget PROPERTIES TIMEOUT 30)
    add_test(NAME agc_driver_sampled_cache_budget_memory COMMAND agc_driver_sampled_cache_budget_tests memory)
    set_tests_properties(agc_driver_sampled_cache_budget_memory PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1" SKIP_RETURN_CODE 77)
    add_test(NAME agc_driver_resource_cache_storage_admission COMMAND agc_driver_sampled_cache_budget_tests resource-admission)
    add_test(NAME agc_driver_resource_cache_oversized_storage_admission COMMAND agc_driver_sampled_cache_budget_tests resource-oversized)
    add_test(NAME agc_driver_resource_cache_storage_clear COMMAND agc_driver_sampled_cache_budget_tests resource-clear)
    set_tests_properties(agc_driver_resource_cache_storage_admission agc_driver_resource_cache_oversized_storage_admission agc_driver_resource_cache_storage_clear PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1" SKIP_RETURN_CODE 77 TIMEOUT 30)
    if(WIN32)
        set_tests_properties(agc_driver_sampled_cache_budget_memory agc_driver_resource_cache_storage_admission agc_driver_resource_cache_oversized_storage_admission agc_driver_resource_cache_storage_clear PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    # The driver draw test again with the draw key off: the draw cache then keys on the decoded vertex
    # stage, which must include PA_CL_VS_OUT_CNTL.
    add_test(NAME agc_driver_vertex_clip_cull_draw_no_key COMMAND agc_driver_vertex_clip_cull_draw_tests no-key)
    set_tests_properties(agc_driver_vertex_clip_cull_draw_no_key PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_NO_DRAW_KEY=1" SKIP_RETURN_CODE 77)
    if(WIN32)
        set_tests_properties(agc_driver_vertex_clip_cull_draw_no_key PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test(NAME agc_driver_pm4_failure COMMAND agc_driver_pm4_tests failure)
    set_tests_properties(agc_driver_pm4_failure PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1" SKIP_RETURN_CODE 77)
    foreach(pm4RingCase IN ITEMS "ring|APS5_PM4_RING=4" "ring-empty|APS5_PM4_RING=16" "ring-disabled|APS5_PM4_RING=0" "ring-invalid|APS5_PM4_RING=4x" "ring-report|APS5_PM4_RING=2" "ring-report-heap|APS5_PM4_RING=2" "stall-watchdog|APS5_STALL_WATCHDOG_SECS=1"
            "stall-watchdog-invalid|APS5_STALL_WATCHDOG_SECS=1x" "stall-watchdog-rewind|APS5_STALL_WATCHDOG_SECS=1" "stall-watchdog-order|APS5_STALL_WATCHDOG_SECS=1")
        string(REPLACE "|" ";" pm4RingCase "${pm4RingCase}")
        list(GET pm4RingCase 0 pm4RingMode)
        list(GET pm4RingCase 1 pm4RingEnvironment)
        string(REPLACE "-" "_" pm4RingTest "agc_driver_pm4_${pm4RingMode}")
        add_test(NAME ${pm4RingTest} COMMAND agc_driver_pm4_tests ${pm4RingMode})
        set_tests_properties(${pm4RingTest} PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;${pm4RingEnvironment}" SKIP_RETURN_CODE 77 TIMEOUT 30)
        if(WIN32)
            set_tests_properties(${pm4RingTest} PROPERTIES ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
        endif()
    endforeach()
    add_test(NAME agc_descriptor_pool_reset_failure COMMAND agc_driver_graphics_tests --descriptor-pool-reset-failure)
    if(WIN32)
        set_tests_properties(agc_driver_pm4_failure agc_descriptor_pool_reset_failure PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    if(NOT WIN32)
        add_test(NAME agc_driver_pm4_priority_refused COMMAND agc_driver_pm4_tests priority-refused)
        set_tests_properties(agc_driver_pm4_priority_refused PROPERTIES ENVIRONMENT
            "ANYPS5_NO_SHADER_CACHE=1;APS5_THREAD_PRIORITY=high;DBUS_SYSTEM_BUS_ADDRESS=unix:path=/nonexistent/aps5-no-rtkit" SKIP_RETURN_CODE 77)
    endif()
    add_test(NAME agc_driver_present_mode COMMAND agc_driver_flip_tests present-mode)
    add_test(NAME agc_driver_flip_compute COMMAND agc_driver_flip_tests compute)
    set_tests_properties(agc_driver_flip_compute PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1" SKIP_RETURN_CODE 77)
    if(WIN32)
        set_tests_properties(agc_driver_flip_compute agc_driver_present_mode PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test(NAME agc_driver_draw_cache_reuse_off COMMAND agc_driver_draw_cache_reuse_tests)
    set_tests_properties(agc_driver_draw_cache_reuse_off PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_NO_GPL=1" SKIP_RETURN_CODE 77)
    add_test(NAME agc_driver_draw_cache_reuse_indirect COMMAND agc_driver_draw_cache_reuse_tests)
    set_tests_properties(agc_driver_draw_cache_reuse_indirect PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_PIPELINE_INDIRECT=1" SKIP_RETURN_CODE 77)
    add_test(NAME agc_driver_draw_cache_reuse_serial COMMAND agc_driver_draw_cache_reuse_tests)
    set_tests_properties(agc_driver_draw_cache_reuse_serial PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_PIPELINED_DRAWS=0" SKIP_RETURN_CODE 77)
    set_tests_properties(agc_driver_draw_cache_reuse agc_driver_draw_cache_reuse_off agc_driver_draw_cache_reuse_indirect agc_driver_draw_cache_reuse_serial PROPERTIES FAIL_REGULAR_EXPRESSION "FATAL:")
    set_property(TEST agc_driver_draw_cache_reuse agc_driver_draw_cache_reuse_off agc_driver_draw_cache_reuse_indirect agc_driver_draw_cache_reuse_serial APPEND PROPERTY ENVIRONMENT "APS5_DRAW_CACHE_ENTRIES=16")
    add_test(NAME agc_driver_draw_cache_relocation_off COMMAND agc_driver_draw_cache_relocation_tests)
    set_tests_properties(agc_driver_draw_cache_relocation_off PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_NO_GPL=1" SKIP_RETURN_CODE 77)
    add_test(NAME agc_driver_draw_cache_relocation_verify COMMAND agc_driver_draw_cache_relocation_tests)
    set_tests_properties(agc_driver_draw_cache_relocation_verify PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_VERIFY_REGISTER_KEYS=1;APS5_VERIFY_DRAW_ENTRIES=1;APS5_VERIFY_DRAW_DATA_HITS=1;APS5_VERIFY_SRT_COMPACT=1" SKIP_RETURN_CODE 77)
    if(WIN32)
        set_tests_properties(agc_driver_draw_cache_reuse_off agc_driver_draw_cache_reuse_indirect agc_driver_draw_cache_reuse_serial agc_driver_draw_cache_relocation_off agc_driver_draw_cache_relocation_verify PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    set_tests_properties(agc_driver agc_driver_pm4_failure PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_PIPELINED_DRAWS=0")
    add_test(NAME agc_driver_pipelined_dispatch_item COMMAND agc_driver_pipelined_dispatch_tests)
    set_tests_properties(agc_driver_pipelined_dispatch PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_PIPELINE_DISPATCH=0")
    set_tests_properties(agc_driver_pipelined_dispatch_item PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_PIPELINE_DISPATCH=1" SKIP_RETURN_CODE 77)
    if(WIN32)
        set_tests_properties(agc_driver_pipelined_dispatch_item PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()

    add_test(NAME agc_driver_stencil_replay_no_gpl COMMAND agc_driver_stencil_replay_tests)
    set_tests_properties(agc_driver_stencil_replay_no_gpl PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_NO_GPL=1" TIMEOUT 30 SKIP_RETURN_CODE 77)
    if(WIN32)
        set_tests_properties(agc_driver_stencil_replay_no_gpl PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()

    foreach(agcTest IN ITEMS agc_spirv_narrow_constant_store_tests agc_spirv13_image_tests agc_spirv_optimizer_tests agc_driver_constant_store_tests)
        if(NOT TARGET ${agcTest})
            continue()
        endif()
        set_target_properties(${agcTest} PROPERTIES EXCLUDE_FROM_ALL OFF)
        string(REGEX REPLACE "_tests$" "" agcTestBaseName "${agcTest}")
        foreach(agcSpirvMode IN ITEMS default full none)
            if(agcSpirvMode STREQUAL "default")
                set(agcTestName "${agcTestBaseName}")
                set(agcSpirvEnvironment "--unset=APS5_SPIRV_OPT")
            else()
                set(agcTestName "${agcTestBaseName}_${agcSpirvMode}")
                set(agcSpirvEnvironment "APS5_SPIRV_OPT=${agcSpirvMode}")
            endif()
            add_test(NAME ${agcTestName} COMMAND ${CMAKE_COMMAND} -E env
                    ${agcSpirvEnvironment} "$<TARGET_FILE:${agcTest}>")
            set_tests_properties(${agcTestName} PROPERTIES
                    ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1" TIMEOUT 60 SKIP_RETURN_CODE 77)
            if(agcTest STREQUAL "agc_spirv13_image_tests")
                set_tests_properties(${agcTestName} PROPERTIES TIMEOUT 30)
            endif()
            if(agcTest STREQUAL "agc_driver_constant_store_tests")
                set_tests_properties(${agcTestName} PROPERTIES RUN_SERIAL TRUE)
            endif()
            if(WIN32)
                set_tests_properties(${agcTestName} PROPERTIES ENVIRONMENT_MODIFICATION
                        "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
            endif()
        endforeach()
    endforeach()
    foreach(preparedMode IN ITEMS indirect uncached debug debug_failure deferred deferred_undecodable registered_workgroup registered_workgroup_sync lazy lazy_sync)
        set(preparedTest "agc_driver_prepared_shaders_${preparedMode}")
        if(preparedMode STREQUAL "indirect")
            add_test(NAME ${preparedTest} COMMAND agc_driver_prepared_shaders_tests --indirect)
        elseif(preparedMode STREQUAL "deferred")
            add_test(NAME ${preparedTest} COMMAND agc_driver_prepared_shaders_tests --deferred)
        elseif(preparedMode STREQUAL "deferred_undecodable")
            add_test(NAME ${preparedTest} COMMAND agc_driver_prepared_shaders_tests --deferred-undecodable)
        elseif(preparedMode STREQUAL "lazy" OR preparedMode STREQUAL "lazy_sync")
            add_test(NAME ${preparedTest} COMMAND agc_driver_prepared_shaders_tests --lazy)
        elseif(preparedMode STREQUAL "registered_workgroup" OR preparedMode STREQUAL "registered_workgroup_sync")
            add_test(NAME ${preparedTest} COMMAND agc_driver_prepared_shaders_tests --registered-workgroup)
        elseif(preparedMode STREQUAL "debug_failure")
            add_test(NAME ${preparedTest} COMMAND agc_driver_prepared_shaders_tests --fail-before-registration)
            set_tests_properties(${preparedTest} PROPERTIES WILL_FAIL TRUE TIMEOUT 30)
        else()
            add_test(NAME ${preparedTest} COMMAND agc_driver_prepared_shaders_tests)
        endif()
        set_tests_properties(${preparedTest} PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1" SKIP_RETURN_CODE 77 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
        if(preparedMode STREQUAL "uncached")
            set_property(TEST ${preparedTest} APPEND PROPERTY ENVIRONMENT "APS5_NO_DISPATCH_CACHE=1")
        elseif(preparedMode STREQUAL "registered_workgroup_sync")
            set_property(TEST ${preparedTest} APPEND PROPERTY ENVIRONMENT "APS5_SYNC_SHADER_PREPARE=1")
        elseif(preparedMode STREQUAL "lazy" OR preparedMode STREQUAL "lazy_sync")
            set_property(TEST ${preparedTest} APPEND PROPERTY ENVIRONMENT "APS5_LAZY_SHADER_PREPARE=1" "APS5_SUSPEND_DRAIN=1")
            if(preparedMode STREQUAL "lazy_sync")
                set_property(TEST ${preparedTest} APPEND PROPERTY ENVIRONMENT "APS5_SYNC_SHADER_PREPARE=1")
            endif()
        elseif(preparedMode STREQUAL "debug" OR preparedMode STREQUAL "debug_failure")
            set_property(TEST ${preparedTest} APPEND PROPERTY ENVIRONMENT "APS5_PROFILE_DRAW=1" "APS5_NO_UNLOCKED_DEVICE=1")
        endif()
        if(WIN32)
            set_tests_properties(${preparedTest} PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
        endif()
    endforeach()
    add_test(NAME agc_driver_compute_user_data_calls_registered COMMAND agc_driver_compute_user_data_calls_tests --registered)
    set_tests_properties(agc_driver_compute_user_data_calls_registered PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1" SKIP_RETURN_CODE 77 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    if(WIN32)
        set_tests_properties(agc_driver_compute_user_data_calls_registered PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    set_tests_properties(agc_driver_prepared_shaders
        agc_driver_prepared_shaders_indirect agc_driver_prepared_shaders_uncached
        agc_driver_prepared_shaders_debug agc_driver_prepared_shaders_debug_failure
        PROPERTIES RESOURCE_LOCK shader_failure_capture)
endif()

option(AGC_BUILD_VISUAL_TEST "Enable the standalone AGC SPIR-V visual test" OFF)

if(AGC_BUILD_VISUAL_TEST)
    ensure_agc_glslang()

    set(visualShaders)

    foreach(stage IN ITEMS vert frag)
        set(shaderOutput "${ANYPS5_TEST_OUTPUT_DIRECTORY}/Triangle.${stage}.spv")

        add_custom_command(
                OUTPUT "${shaderOutput}"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${ANYPS5_TEST_OUTPUT_DIRECTORY}"
                COMMAND "$<TARGET_FILE:glslang-standalone>"
                -V
                --target-env vulkan1.1
                -o "${shaderOutput}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/Triangle.${stage}"
                DEPENDS
                glslang-standalone
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/Triangle.${stage}"
                VERBATIM
        )

        list(APPEND visualShaders "${shaderOutput}")
    endforeach()

    add_test_executable(agc_driver_visual_test EXCLUDE_FROM_ALL
            tests/Visual.cpp
            ${agcDriverSources}
            ${visualShaders}
    )

    target_include_directories(agc_driver_visual_test PRIVATE
            ${LIBS_INCLUDE_DIR}
            ${agcTextureDetileGeneratedRoot}
            ${CMAKE_SOURCE_DIR}/core/shader/recompiler
            ${CMAKE_SOURCE_DIR}/core/shader/recompiler/IntermediateRepresentation/include
            ${CMAKE_SOURCE_DIR}/core/shader/recompiler/ControlFlow/include
            ${CMAKE_SOURCE_DIR}/core/shader/recompiler/RdnaDecoder/include
            ${ANYPS5_VULKAN_HEADERS_INCLUDE_DIR}
            ${ANYPS5_SPIRV_HEADERS_INCLUDE_DIR}
    )

    target_link_libraries(agc_driver_visual_test PRIVATE
            shader_recompiler_agc_driver
            SDL2-static
            libc
            libkernel
            jpeg_turbo
    )
    add_dependencies(agc_driver_visual_test agc_trace_revision)

    set_target_properties(agc_driver_visual_test PROPERTIES CXX_EXTENSIONS OFF)
    configure_windows_unwind(agc_driver_visual_test)

    if(WIN32)
        set(visualRuntimeFiles "$<TARGET_FILE:libc>" "$<TARGET_FILE:libkernel>")

        if(MINGW)
            get_filename_component(visualCompilerDirectory "${CMAKE_CXX_COMPILER}" DIRECTORY)

            foreach(runtimeName IN ITEMS
                    libgcc_s_seh-1.dll
                    libstdc++-6.dll
                    libwinpthread-1.dll
            )
                set(runtimePath "${visualCompilerDirectory}/${runtimeName}")

                if(NOT EXISTS "${runtimePath}")
                    message(FATAL_ERROR "Cannot locate MinGW runtime: ${runtimeName}")
                endif()

                list(APPEND visualRuntimeFiles "${runtimePath}")
            endforeach()
        endif()

        add_custom_target(agc_driver_visual_runtime
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                ${visualRuntimeFiles}
                "$<TARGET_FILE_DIR:agc_driver_visual_test>"
                DEPENDS libc libkernel
                COMMAND_EXPAND_LISTS
                VERBATIM
        )

        add_dependencies(agc_driver_visual_test agc_driver_visual_runtime)
    endif()
endif()

include(PostCompile.cmake)
# Replays shader_*.req files the driver writes when a compute dispatch cannot be recompiled.
add_executable(agc_shader_replay EXCLUDE_FROM_ALL
        tools/ShaderReplay.cpp
)
target_include_directories(agc_shader_replay PRIVATE ${LIBS_INCLUDE_DIR})
target_link_libraries(agc_shader_replay PRIVATE shader_recompiler_agc_driver libc)
if(ANYPS5_ENABLE_SPIRV_TOOLS)
    target_link_libraries(agc_shader_replay PRIVATE SPIRV-Tools-static)
endif()
set_target_properties(agc_shader_replay PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_shader_replay)

add_executable(agc_compute_pipeline_replay EXCLUDE_FROM_ALL tools/ComputePipelineReplay.cpp)
target_include_directories(agc_compute_pipeline_replay PRIVATE
        ${LIBS_INCLUDE_DIR}
        ${CMAKE_SOURCE_DIR}/core/shader/recompiler
        ${ANYPS5_VULKAN_HEADERS_INCLUDE_DIR}
        ${ANYPS5_SPIRV_HEADERS_INCLUDE_DIR}
)
target_link_libraries(agc_compute_pipeline_replay PRIVATE libSceAgcDriver libc)
set_target_properties(agc_compute_pipeline_replay PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_compute_pipeline_replay)

if(BUILD_TESTING)
    add_test_executable(agc_compute_pipeline_capture_tests tests/ComputePipelineCapture.cpp Execution/src/ComputePipelineCapture.cpp)
    target_include_directories(agc_compute_pipeline_capture_tests PRIVATE ${LIBS_INCLUDE_DIR} ${ANYPS5_VULKAN_HEADERS_INCLUDE_DIR})
    target_link_libraries(agc_compute_pipeline_capture_tests PRIVATE libc)
    set_target_properties(agc_compute_pipeline_capture_tests PROPERTIES CXX_EXTENSIONS OFF)
    configure_windows_unwind(agc_compute_pipeline_capture_tests)
    add_test(NAME agc_compute_descriptor_layout_flags COMMAND agc_driver_graphics_tests --descriptor-layout-flags)
    add_test(NAME agc_pipeline_library_lifetime COMMAND agc_driver_graphics_tests --pipeline-library-lifetime)
    add_test(NAME agc_pipeline_library_lifetime_zero_budget COMMAND agc_driver_graphics_tests --pipeline-library-lifetime)
    set_tests_properties(agc_pipeline_library_lifetime agc_pipeline_library_lifetime_zero_budget PROPERTIES TIMEOUT 30)
    set_tests_properties(agc_pipeline_library_lifetime_zero_budget PROPERTIES ENVIRONMENT "APS5_PREPARED_SHADER_MIB=0")
    set_tests_properties(agc_compute_descriptor_layout_flags PROPERTIES TIMEOUT 30)
    add_test(NAME agc_driver_covered_centroid_state COMMAND agc_driver_graphics_tests --covered-centroid-state)
    set_tests_properties(agc_driver_covered_centroid_state PROPERTIES TIMEOUT 30)
    if(WIN32)
        set_tests_properties(agc_compute_descriptor_layout_flags PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
        set_tests_properties(agc_pipeline_library_lifetime agc_pipeline_library_lifetime_zero_budget PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
        set_tests_properties(agc_driver_covered_centroid_state PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test(NAME agc_compute_pipeline_capture COMMAND agc_compute_pipeline_capture_tests)
    set_tests_properties(agc_compute_pipeline_capture PROPERTIES TIMEOUT 30)
    if(WIN32)
        set_tests_properties(agc_compute_pipeline_capture PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

if(BUILD_TESTING AND ANYPS5_ENABLE_SPIRV_TOOLS)
    add_test_executable(agc_compute_pipeline_replay_tests tests/ComputePipelineReplay.cpp)
    target_include_directories(agc_compute_pipeline_replay_tests PRIVATE
            ${LIBS_INCLUDE_DIR}
            ${CMAKE_SOURCE_DIR}/core/shader/recompiler
            ${ANYPS5_VULKAN_HEADERS_INCLUDE_DIR}
            ${ANYPS5_SPIRV_HEADERS_INCLUDE_DIR}
    )
    target_link_libraries(agc_compute_pipeline_replay_tests PRIVATE libSceAgcDriver libc)
    set_target_properties(agc_compute_pipeline_replay_tests PROPERTIES CXX_EXTENSIONS OFF)
    configure_windows_unwind(agc_compute_pipeline_replay_tests)
    add_test(NAME agc_compute_pipeline_replay COMMAND agc_compute_pipeline_replay_tests)
    set_tests_properties(agc_compute_pipeline_replay PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77)
    add_dependencies(agc_compute_pipeline_replay_tests agc_compute_pipeline_replay agc_driver_branch_past_endpgm_tests)
    add_test(NAME agc_compute_pipeline_capture_run COMMAND ${CMAKE_COMMAND}
            -DFIXTURE=$<TARGET_FILE:agc_driver_branch_past_endpgm_tests>
            -DREPLAY=$<TARGET_FILE:agc_compute_pipeline_replay>
            -DOUTPUT_ROOT=${CMAKE_CURRENT_BINARY_DIR}/capture-tests
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/ComputePipelineCaptureRun.cmake)
    set_tests_properties(agc_compute_pipeline_capture_run PROPERTIES TIMEOUT 30 SKIP_REGULAR_EXPRESSION "compute pipeline capture run: skipped, no usable Vulkan device")
    if(WIN32)
        set_tests_properties(agc_compute_pipeline_replay agc_compute_pipeline_capture_run PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

if(BUILD_TESTING)
    add_test_executable(texture_format_lookup_tests ${LIBS_INCLUDE_DIR}/tests/TextureFormatLookup.cpp)
    target_include_directories(texture_format_lookup_tests PRIVATE ${LIBS_INCLUDE_DIR} ${ANYPS5_VULKAN_HEADERS_INCLUDE_DIR})
    target_link_libraries(texture_format_lookup_tests PRIVATE libSceAgcDriver libc)
    configure_windows_unwind(texture_format_lookup_tests)
    add_test(NAME texture_format_lookup COMMAND texture_format_lookup_tests)
    if(WIN32)
        set_tests_properties(texture_format_lookup PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test_executable(linear_display_buffer_tests ${LIBS_INCLUDE_DIR}/tests/LinearDisplayBuffer.cpp)
    target_include_directories(linear_display_buffer_tests PRIVATE ${LIBS_INCLUDE_DIR} ${ANYPS5_VULKAN_HEADERS_INCLUDE_DIR})
    target_link_libraries(linear_display_buffer_tests PRIVATE libSceAgcDriver libc)
    configure_windows_unwind(linear_display_buffer_tests)
    add_test(NAME linear_display_buffer COMMAND linear_display_buffer_tests)
    if(WIN32)
        set_tests_properties(linear_display_buffer PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test_executable(guest_agc_driver_resources_tests ${LIBS_INCLUDE_DIR}/tests/GuestAgcDriverResources.cpp)
    target_include_directories(guest_agc_driver_resources_tests PRIVATE ${LIBS_INCLUDE_DIR})
    target_link_libraries(guest_agc_driver_resources_tests PRIVATE libSceAgcDriver libc)
    configure_windows_unwind(guest_agc_driver_resources_tests)
    add_test(NAME guest_agc_driver_resources COMMAND guest_agc_driver_resources_tests)
    if(WIN32)
        set_tests_properties(guest_agc_driver_resources PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test_executable(guest_agc_driver_debug_unavailable_tests ${LIBS_INCLUDE_DIR}/tests/GuestAgcDriverDebugUnavailable.cpp)
    target_include_directories(guest_agc_driver_debug_unavailable_tests PRIVATE ${LIBS_INCLUDE_DIR})
    target_link_libraries(guest_agc_driver_debug_unavailable_tests PRIVATE libSceAgcDriver libc)
    configure_windows_unwind(guest_agc_driver_debug_unavailable_tests)
    add_test(NAME guest_agc_driver_debug_unavailable COMMAND guest_agc_driver_debug_unavailable_tests)
    if(WIN32)
        set_tests_properties(guest_agc_driver_debug_unavailable PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test_executable(guest_rendering_wait_tests ${LIBS_INCLUDE_DIR}/tests/GuestRenderingWait.cpp)
    target_include_directories(guest_rendering_wait_tests PRIVATE ${LIBS_INCLUDE_DIR})
    target_link_libraries(guest_rendering_wait_tests PRIVATE libSceAgcDriver libc)
    configure_windows_unwind(guest_rendering_wait_tests)
    add_test(NAME guest_rendering_wait COMMAND guest_rendering_wait_tests)
    if(WIN32)
        set_tests_properties(guest_rendering_wait PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test_executable(shader_buffer_address_tests ${LIBS_INCLUDE_DIR}/tests/ShaderBufferAddress.cpp)
    target_link_libraries(shader_buffer_address_tests PRIVATE shader_recompiler_agc_driver libc)
    configure_windows_unwind(shader_buffer_address_tests)
    add_test(NAME shader_buffer_address COMMAND shader_buffer_address_tests)
    if(WIN32)
        set_tests_properties(shader_buffer_address PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test_executable(ray_tracing_miss_tests ${LIBS_INCLUDE_DIR}/tests/RayTracingMiss.cpp)
    target_link_libraries(ray_tracing_miss_tests PRIVATE shader_recompiler_agc_driver libc)
    configure_windows_unwind(ray_tracing_miss_tests)
    add_test(NAME ray_tracing_miss COMMAND ray_tracing_miss_tests)
    if(WIN32)
        set_tests_properties(ray_tracing_miss PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test_executable(wave32_wide_subgroup_tests ${LIBS_INCLUDE_DIR}/tests/Wave32WideSubgroup.cpp)
    target_link_libraries(wave32_wide_subgroup_tests PRIVATE shader_recompiler_agc_driver libc)
    configure_windows_unwind(wave32_wide_subgroup_tests)
    add_test(NAME wave32_wide_subgroup COMMAND wave32_wide_subgroup_tests)
    if(WIN32)
        set_tests_properties(wave32_wide_subgroup PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test_executable(vector_compare_i16_u16_tests ${LIBS_INCLUDE_DIR}/tests/VectorCompareI16U16.cpp)
    target_link_libraries(vector_compare_i16_u16_tests PRIVATE shader_recompiler_agc_driver libc)
    configure_windows_unwind(vector_compare_i16_u16_tests)
    add_test(NAME vector_compare_i16_u16 COMMAND vector_compare_i16_u16_tests)
    if(WIN32)
        set_tests_properties(vector_compare_i16_u16 PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test_executable(vector_compare_const_tests ${LIBS_INCLUDE_DIR}/tests/VectorCompareConst.cpp)
    target_link_libraries(vector_compare_const_tests PRIVATE shader_recompiler_agc_driver libc)
    configure_windows_unwind(vector_compare_const_tests)
    add_test(NAME vector_compare_const COMMAND vector_compare_const_tests)
    if(WIN32)
        set_tests_properties(vector_compare_const PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test_executable(vector_min_max_med16_tests ${LIBS_INCLUDE_DIR}/tests/VectorMinMaxMed16.cpp)
    target_link_libraries(vector_min_max_med16_tests PRIVATE shader_recompiler_agc_driver libc)
    configure_windows_unwind(vector_min_max_med16_tests)
    add_test(NAME vector_min_max_med16 COMMAND vector_min_max_med16_tests)
    if(WIN32)
        set_tests_properties(vector_min_max_med16 PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test_executable(vector_mad_legacy_tests ${LIBS_INCLUDE_DIR}/tests/VectorMadLegacy.cpp)
    target_link_libraries(vector_mad_legacy_tests PRIVATE shader_recompiler_agc_driver libc)
    configure_windows_unwind(vector_mad_legacy_tests)
    add_test(NAME vector_mad_legacy COMMAND vector_mad_legacy_tests)
    if(WIN32)
        set_tests_properties(vector_mad_legacy PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test_executable(vector_fused_multiply_add_tests ${LIBS_INCLUDE_DIR}/tests/VectorFusedMultiplyAdd.cpp)
    target_link_libraries(vector_fused_multiply_add_tests PRIVATE shader_recompiler_agc_driver libc)
    configure_windows_unwind(vector_fused_multiply_add_tests)
    add_test(NAME vector_fused_multiply_add COMMAND vector_fused_multiply_add_tests)
    if(WIN32)
        set_tests_properties(vector_fused_multiply_add PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test_executable(vector_constant_factor_tininess_tests ${LIBS_INCLUDE_DIR}/tests/VectorConstantFactorTininess.cpp)
    target_link_libraries(vector_constant_factor_tininess_tests PRIVATE shader_recompiler_agc_driver libc)
    configure_windows_unwind(vector_constant_factor_tininess_tests)
    add_test(NAME vector_constant_factor_tininess COMMAND vector_constant_factor_tininess_tests)
    if(WIN32)
        set_tests_properties(vector_constant_factor_tininess PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test_executable(scalar_saveexec_tests ${LIBS_INCLUDE_DIR}/tests/ScalarSaveexec.cpp)
    target_link_libraries(scalar_saveexec_tests PRIVATE shader_recompiler_agc_driver libc)
    configure_windows_unwind(scalar_saveexec_tests)
    add_test(NAME scalar_saveexec COMMAND scalar_saveexec_tests)
    if(WIN32)
        set_tests_properties(scalar_saveexec PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test_executable(scalar_swappc_tests ${LIBS_INCLUDE_DIR}/tests/ScalarSwappc.cpp)
    target_link_libraries(scalar_swappc_tests PRIVATE shader_recompiler_agc_driver libc)
    configure_windows_unwind(scalar_swappc_tests)
    add_test(NAME scalar_swappc COMMAND scalar_swappc_tests)
    if(WIN32)
        set_tests_properties(scalar_swappc PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test_executable(scalar_bitcount64_tests ${LIBS_INCLUDE_DIR}/tests/ScalarBitCount64.cpp)
    target_link_libraries(scalar_bitcount64_tests PRIVATE shader_recompiler_agc_driver libc)
    configure_windows_unwind(scalar_bitcount64_tests)
    add_test(NAME scalar_bitcount64 COMMAND scalar_bitcount64_tests)
    if(WIN32)
        set_tests_properties(scalar_bitcount64 PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test_executable(bitfield_mask_tests ${LIBS_INCLUDE_DIR}/tests/BitfieldMask.cpp)
    target_link_libraries(bitfield_mask_tests PRIVATE shader_recompiler_agc_driver libc)
    configure_windows_unwind(bitfield_mask_tests)
    add_test(NAME bitfield_mask COMMAND bitfield_mask_tests)
    if(WIN32)
        set_tests_properties(bitfield_mask PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test_executable(scalar_scratch_load_tests ${LIBS_INCLUDE_DIR}/tests/ScalarScratchLoad.cpp)
    target_link_libraries(scalar_scratch_load_tests PRIVATE shader_recompiler_agc_driver libc)
    configure_windows_unwind(scalar_scratch_load_tests)
    add_test(NAME scalar_scratch_load COMMAND scalar_scratch_load_tests)
    if(WIN32)
        set_tests_properties(scalar_scratch_load PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test_executable(aperture_sources_tests ${LIBS_INCLUDE_DIR}/tests/ApertureSources.cpp)
    target_link_libraries(aperture_sources_tests PRIVATE shader_recompiler_agc_driver libc)
    configure_windows_unwind(aperture_sources_tests)
    add_test(NAME aperture_sources COMMAND aperture_sources_tests)
    if(WIN32)
        set_tests_properties(aperture_sources PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test_executable(scalar_call_tests ${LIBS_INCLUDE_DIR}/tests/ScalarCall.cpp)
    target_link_libraries(scalar_call_tests PRIVATE shader_recompiler_agc_driver libc)
    configure_windows_unwind(scalar_call_tests)
    add_test(NAME scalar_call COMMAND scalar_call_tests)
    if(WIN32)
        set_tests_properties(scalar_call PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test_executable(image_sample_gather_tests ${LIBS_INCLUDE_DIR}/tests/ImageSampleGather.cpp)
    target_link_libraries(image_sample_gather_tests PRIVATE shader_recompiler_agc_driver libc)
    configure_windows_unwind(image_sample_gather_tests)
    add_test(NAME image_sample_gather COMMAND image_sample_gather_tests)
    if(WIN32)
        set_tests_properties(image_sample_gather PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test_executable(pixel_ancillary_tests ${LIBS_INCLUDE_DIR}/tests/PixelAncillary.cpp)
    target_link_libraries(pixel_ancillary_tests PRIVATE shader_recompiler_agc_driver libc)
    configure_windows_unwind(pixel_ancillary_tests)
    add_test(NAME pixel_ancillary COMMAND pixel_ancillary_tests)
    if(WIN32)
        set_tests_properties(pixel_ancillary PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test_executable(guest_indexed_draw_tests ${LIBS_INCLUDE_DIR}/tests/GuestIndexedDraw.cpp)
    target_include_directories(guest_indexed_draw_tests PRIVATE ${LIBS_INCLUDE_DIR})
    target_link_libraries(guest_indexed_draw_tests PRIVATE libSceAgc libSceAgcDriver libc)
    configure_windows_unwind(guest_indexed_draw_tests)
    add_test(NAME guest_indexed_draw COMMAND guest_indexed_draw_tests)
    if(WIN32)
        set_tests_properties(guest_indexed_draw PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

add_test_executable(agc_embedded_vertex_fetch_tests EXCLUDE_FROM_ALL tests/EmbeddedVertexFetch.cpp)
target_link_libraries(agc_embedded_vertex_fetch_tests PRIVATE shader_recompiler_agc_driver libc)
set_target_properties(agc_embedded_vertex_fetch_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_embedded_vertex_fetch_tests)
if(BUILD_TESTING)
    set_target_properties(agc_embedded_vertex_fetch_tests PROPERTIES EXCLUDE_FROM_ALL OFF)
    add_test(NAME agc_embedded_vertex_fetch COMMAND agc_embedded_vertex_fetch_tests)
    if(WIN32)
        set_tests_properties(agc_embedded_vertex_fetch PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

add_test_executable(agc_rdna_truncated_modifier_tests EXCLUDE_FROM_ALL tests/RdnaTruncatedModifier.cpp)
target_link_libraries(agc_rdna_truncated_modifier_tests PRIVATE shader_recompiler_agc_driver libc)
set_target_properties(agc_rdna_truncated_modifier_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_rdna_truncated_modifier_tests)
if(BUILD_TESTING)
    set_target_properties(agc_rdna_truncated_modifier_tests PROPERTIES EXCLUDE_FROM_ALL OFF)
    add_test(NAME agc_rdna_truncated_modifier COMMAND agc_rdna_truncated_modifier_tests)
    if(WIN32)
        set_tests_properties(agc_rdna_truncated_modifier PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

add_test_executable(agc_mubuf_lds_decode_tests EXCLUDE_FROM_ALL tests/MubufLdsDecode.cpp)
target_link_libraries(agc_mubuf_lds_decode_tests PRIVATE shader_recompiler_agc_driver libc)
set_target_properties(agc_mubuf_lds_decode_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_mubuf_lds_decode_tests)
if(BUILD_TESTING)
    set_target_properties(agc_mubuf_lds_decode_tests PROPERTIES EXCLUDE_FROM_ALL OFF)
    add_test(NAME agc_mubuf_lds_decode COMMAND agc_mubuf_lds_decode_tests)
    if(WIN32)
        set_tests_properties(agc_mubuf_lds_decode PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

add_test_executable(agc_vop1_sdwa_move_decode_tests EXCLUDE_FROM_ALL tests/Vop1SdwaMoveDecode.cpp)
target_link_libraries(agc_vop1_sdwa_move_decode_tests PRIVATE shader_recompiler_agc_driver libc)
set_target_properties(agc_vop1_sdwa_move_decode_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_vop1_sdwa_move_decode_tests)
if(BUILD_TESTING)
    set_target_properties(agc_vop1_sdwa_move_decode_tests PROPERTIES EXCLUDE_FROM_ALL OFF)
    add_test(NAME agc_vop1_sdwa_move_decode COMMAND agc_vop1_sdwa_move_decode_tests)
    if(WIN32)
        set_tests_properties(agc_vop1_sdwa_move_decode PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

add_test_executable(agc_pixel_position_export_tests EXCLUDE_FROM_ALL tests/PixelPositionExport.cpp)
target_link_libraries(agc_pixel_position_export_tests PRIVATE shader_recompiler_agc_driver libc)
set_target_properties(agc_pixel_position_export_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_pixel_position_export_tests)
if(BUILD_TESTING)
    set_target_properties(agc_pixel_position_export_tests PROPERTIES EXCLUDE_FROM_ALL OFF)
    add_test(NAME agc_pixel_position_export COMMAND agc_pixel_position_export_tests)
    set_tests_properties(agc_pixel_position_export PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1")
    if(WIN32)
        set_tests_properties(agc_pixel_position_export PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

add_test_executable(agc_position_invariance_tests EXCLUDE_FROM_ALL tests/PositionInvariance.cpp)
target_link_libraries(agc_position_invariance_tests PRIVATE shader_recompiler_agc_driver libc)
set_target_properties(agc_position_invariance_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_position_invariance_tests)

add_test_executable(agc_vertex_layer_export_tests EXCLUDE_FROM_ALL tests/VertexLayerExport.cpp)
target_link_libraries(agc_vertex_layer_export_tests PRIVATE shader_recompiler_agc_driver libc)
set_target_properties(agc_vertex_layer_export_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_vertex_layer_export_tests)
if(BUILD_TESTING)
    set_target_properties(agc_vertex_layer_export_tests PROPERTIES EXCLUDE_FROM_ALL OFF)
    add_test(NAME agc_vertex_layer_export COMMAND agc_vertex_layer_export_tests)
    set_tests_properties(agc_vertex_layer_export PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1")
    if(WIN32)
        set_tests_properties(agc_vertex_layer_export PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

if(BUILD_TESTING)
    set_target_properties(agc_position_invariance_tests PROPERTIES EXCLUDE_FROM_ALL OFF)
    add_test(NAME agc_position_invariance COMMAND agc_position_invariance_tests)
    set_tests_properties(agc_position_invariance PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1")
    if(WIN32)
        set_tests_properties(agc_position_invariance PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

add_test_executable(agc_vertex_barrier_tests EXCLUDE_FROM_ALL tests/VertexBarrier.cpp)
target_link_libraries(agc_vertex_barrier_tests PRIVATE shader_recompiler_agc_driver libc)
set_target_properties(agc_vertex_barrier_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_vertex_barrier_tests)
if(BUILD_TESTING)
    set_target_properties(agc_vertex_barrier_tests PROPERTIES EXCLUDE_FROM_ALL OFF)
    add_test(NAME agc_vertex_barrier COMMAND agc_vertex_barrier_tests)
    set_tests_properties(agc_vertex_barrier PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1")
    if(WIN32)
        set_tests_properties(agc_vertex_barrier PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

if(BUILD_TESTING)
    add_test_executable(image_by_tests ${LIBS_INCLUDE_DIR}/tests/ImageBy.cpp)
    target_link_libraries(image_by_tests PRIVATE shader_recompiler_agc_driver libc)
    configure_windows_unwind(image_by_tests)
    add_test(NAME image_by COMMAND image_by_tests)
    if(WIN32)
        set_tests_properties(image_by PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test_executable(image_pck_n_tests ${LIBS_INCLUDE_DIR}/tests/ImagePckN.cpp)
    target_link_libraries(image_pck_n_tests PRIVATE shader_recompiler_agc_driver libc)
    configure_windows_unwind(image_pck_n_tests)
    add_test(NAME image_pck_n COMMAND image_pck_n_tests)
    if(WIN32)
        set_tests_properties(image_pck_n PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()

    add_test_executable(image_msaa_load_tests ${LIBS_INCLUDE_DIR}/tests/ImageMsaaLoad.cpp)
    target_link_libraries(image_msaa_load_tests PRIVATE shader_recompiler_agc_driver libc)
    configure_windows_unwind(image_msaa_load_tests)
    add_test(NAME image_msaa_load COMMAND image_msaa_load_tests)
    if(WIN32)
        set_tests_properties(image_msaa_load PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

add_test_executable(agc_embedded_vertex_fetch_aliasing_tests EXCLUDE_FROM_ALL tests/EmbeddedVertexFetchAliasing.cpp)
target_link_libraries(agc_embedded_vertex_fetch_aliasing_tests PRIVATE shader_recompiler_agc_driver libc)
set_target_properties(agc_embedded_vertex_fetch_aliasing_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_embedded_vertex_fetch_aliasing_tests)
if(BUILD_TESTING)
    set_target_properties(agc_embedded_vertex_fetch_aliasing_tests PROPERTIES EXCLUDE_FROM_ALL OFF)
    add_test(NAME agc_embedded_vertex_fetch_aliasing COMMAND agc_embedded_vertex_fetch_aliasing_tests)
    if(WIN32)
        set_tests_properties(agc_embedded_vertex_fetch_aliasing PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

add_test_executable(agc_constant_phi_tests EXCLUDE_FROM_ALL tests/ConstantPhi.cpp)
target_link_libraries(agc_constant_phi_tests PRIVATE shader_recompiler_agc_driver libc)
set_target_properties(agc_constant_phi_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_constant_phi_tests)
if(BUILD_TESTING)
    set_target_properties(agc_constant_phi_tests PROPERTIES EXCLUDE_FROM_ALL OFF)
    add_test(NAME agc_constant_phi COMMAND agc_constant_phi_tests)
    if(WIN32)
        set_tests_properties(agc_constant_phi PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

if(BUILD_TESTING)
    add_test(NAME agc_rebar_uploads COMMAND agc_buffer_pool_tests)
    add_test(NAME agc_rebar_uploads_budget_full COMMAND agc_buffer_pool_tests)
    set_tests_properties(agc_rebar_uploads agc_rebar_uploads_budget_full PROPERTIES ENVIRONMENT "APS5_REBAR_UPLOADS=1;APS5_REBAR_CHECK=1")
    set_property(TEST agc_rebar_uploads_budget_full APPEND PROPERTY ENVIRONMENT "APS5_REBAR_MIB=0")
    if(WIN32)
        set_tests_properties(agc_rebar_uploads agc_rebar_uploads_budget_full PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test(NAME agc_storage_upload_lifetime_rebar COMMAND agc_driver_recorder_tests --storage-upload-lifetime-only)
    add_test(NAME agc_storage_upload_failure_rebar COMMAND agc_driver_recorder_tests --storage-upload-failure-only)
    add_test(NAME agc_readonly_fallback_reuse_rebar COMMAND agc_driver_recorder_tests --readonly-fallback-only)
    add_test(NAME agc_driver_depth_zero_reads_rebar COMMAND agc_driver_depth_plane_sampling_tests --zero-only)
    add_test(NAME agc_driver_depth_large_array_rebar COMMAND agc_driver_depth_plane_sampling_tests --large-only)
    add_test(NAME agc_driver_depth_large_array_no_memo_rebar COMMAND agc_driver_depth_plane_sampling_tests --large-only)
    add_test(NAME agc_driver_graphics_rebar COMMAND agc_driver_graphics_tests)
    add_test(NAME agc_driver_depth_bias_per_face_rebar COMMAND agc_driver_depth_bias_per_face_tests)
    add_test(NAME agc_driver_depth_compare_copy_rebar COMMAND agc_driver_depth_compare_copy_tests)
    add_test(NAME agc_driver_depth_fast_clear_rebar COMMAND agc_driver_depth_fast_clear_tests)
    add_test(NAME agc_driver_depth_fast_clear_submit_rebar COMMAND agc_driver_depth_fast_clear_submit_tests)
    add_test(NAME agc_driver_depth_plane_guest_layers_rebar COMMAND agc_driver_depth_plane_guest_layers_tests)
    add_test(NAME agc_driver_depth_plane_sampling_rebar COMMAND agc_driver_depth_plane_sampling_tests)
    add_test(NAME agc_driver_depth_storage_rebar COMMAND agc_driver_depth_storage_tests)
    add_test(NAME agc_driver_depth_storage_dispatch_rebar COMMAND agc_driver_depth_storage_dispatch_tests)
    add_test(NAME agc_driver_depth_storage_reuse_rebar COMMAND agc_driver_depth_storage_reuse_tests)
    add_test(NAME agc_driver_prepared_graphics_rebar COMMAND agc_driver_prepared_graphics_tests)
    add_test(NAME agc_driver_prepared_unregistered_graphics_rebar COMMAND agc_driver_prepared_unregistered_graphics_tests)
    foreach(rebarTest IN ITEMS
        agc_driver_depth_bias_per_face
        agc_driver_depth_compare_copy
        agc_driver_depth_fast_clear
        agc_driver_depth_fast_clear_submit
        agc_driver_depth_plane_guest_layers
        agc_driver_depth_plane_sampling
        agc_driver_depth_storage
        agc_driver_depth_storage_dispatch
        agc_driver_depth_storage_reuse
        agc_driver_prepared_graphics
        agc_driver_prepared_unregistered_graphics
        agc_storage_upload_lifetime
        agc_storage_upload_failure
        agc_readonly_fallback_reuse
        agc_driver_depth_zero_reads
        agc_driver_depth_large_array
        agc_driver_depth_large_array_no_memo
        agc_driver_graphics
)
        foreach(rebarProperty IN ITEMS ENVIRONMENT ENVIRONMENT_MODIFICATION SKIP_RETURN_CODE TIMEOUT FAIL_REGULAR_EXPRESSION PASS_REGULAR_EXPRESSION WILL_FAIL WORKING_DIRECTORY)
            get_test_property(${rebarTest} ${rebarProperty} rebarValue)
            if(NOT rebarValue STREQUAL "NOTFOUND")
                set_property(TEST ${rebarTest}_rebar PROPERTY ${rebarProperty} "${rebarValue}")
            endif()
        endforeach()
        set_property(TEST ${rebarTest}_rebar APPEND PROPERTY ENVIRONMENT "APS5_REBAR_UPLOADS=1;APS5_REBAR_CHECK=1")
    endforeach()
endif()

if(BUILD_TESTING)
    add_test(NAME agc_recorder COMMAND agc_driver_recorder_tests)
    add_test(NAME agc_completion_probe COMMAND agc_driver_recorder_tests --completion-probe-only)
    add_test(NAME agc_completion_probe_off COMMAND agc_driver_recorder_tests --completion-probe-only)
    set_tests_properties(agc_completion_probe PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 20 ENVIRONMENT "APS5_COMPLETION_PROBE=1")
    set_tests_properties(agc_completion_probe_off PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 20 ENVIRONMENT "APS5_COMPLETION_PROBE=0")
    if(WIN32)
        set_tests_properties(agc_completion_probe agc_completion_probe_off PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    set_tests_properties(agc_recorder PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180 ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1")
    if(WIN32)
        set_tests_properties(agc_recorder PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

if(BUILD_TESTING AND WIN32)
    foreach(rebarMirrorMode IN ITEMS resident fallback requires_resident)
        add_test(NAME agc_rebar_mirrors_${rebarMirrorMode} COMMAND agc_driver_recorder_tests --resident-heap-only)
        set_tests_properties(agc_rebar_mirrors_${rebarMirrorMode} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60 ENVIRONMENT "APS5_REBAR_MIRRORS=1;APS5_REBAR_CHECK=1"
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>;APS5_RESIDENT_HEAP_MIRRORS=unset:")
        if(NOT rebarMirrorMode STREQUAL "requires_resident")
            set_property(TEST agc_rebar_mirrors_${rebarMirrorMode} APPEND PROPERTY ENVIRONMENT_MODIFICATION "APS5_RESIDENT_HEAP_MIRRORS=set:1")
        endif()
    endforeach()
    set_property(TEST agc_rebar_mirrors_fallback APPEND PROPERTY ENVIRONMENT "APS5_REBAR_MIB=0")
    add_test(NAME agc_vram_mirrors_recorder COMMAND agc_driver_recorder_tests)
    add_test(NAME agc_vram_mirrors_heap COMMAND agc_driver_recorder_tests --resident-heap-only)
    add_test(NAME agc_vram_mirrors_heap_fallback COMMAND agc_driver_recorder_tests --resident-heap-only)
    add_test(NAME agc_vram_mirrors_bda_device COMMAND agc_driver_bda_device_tests)
    set_tests_properties(agc_vram_mirrors_recorder agc_vram_mirrors_heap agc_vram_mirrors_heap_fallback agc_vram_mirrors_bda_device PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180 ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_VRAM_MIRRORS=1"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>;APS5_RESIDENT_HEAP_MIRRORS=unset:")
    set_property(TEST agc_vram_mirrors_heap_fallback APPEND PROPERTY ENVIRONMENT "APS5_REBAR_MIB=0")
    foreach(vramDescriptorMode IN ITEMS off on)
        add_test(NAME agc_vram_descriptors_${vramDescriptorMode} COMMAND agc_driver_recorder_tests --vram-descriptors-only)
        set_tests_properties(agc_vram_descriptors_${vramDescriptorMode} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30
            ENVIRONMENT "ANYPS5_REQUIRE_VULKAN=1;ANYPS5_NO_SHADER_CACHE=1;APS5_VRAM_MIRRORS=1;APS5_VRAM_DESCRIPTORS=0;APS5_RESIDENT_HEAP_MIRRORS=0"
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>;APS5_VRAM_WRITABLE=unset:;APS5_VRAM_DEFERRED=unset:;APS5_VRAM_CLEANUP=unset:;APS5_VRAM_TEXTURES=unset:;APS5_TEXTURE_AGE_EVICTION=unset:")
    endforeach()
    set_property(TEST agc_vram_descriptors_on APPEND PROPERTY ENVIRONMENT_MODIFICATION "APS5_VRAM_DESCRIPTORS=set:1")
    add_test(NAME agc_vram_descriptors_recorder COMMAND agc_driver_recorder_tests)
    add_test(NAME agc_vram_descriptors_heap COMMAND agc_driver_recorder_tests --resident-heap-only)
    add_test(NAME agc_vram_descriptors_heap_fallback COMMAND agc_driver_recorder_tests --resident-heap-only)
    add_test(NAME agc_vram_descriptors_bda_device COMMAND agc_driver_bda_device_tests)
    add_test(NAME agc_vram_descriptors_graphics COMMAND agc_driver_graphics_tests)
    set_tests_properties(agc_vram_descriptors_recorder agc_vram_descriptors_heap agc_vram_descriptors_heap_fallback agc_vram_descriptors_bda_device agc_vram_descriptors_graphics
        PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        ENVIRONMENT "ANYPS5_REQUIRE_VULKAN=1;ANYPS5_NO_SHADER_CACHE=1;APS5_VRAM_MIRRORS=1;APS5_VRAM_DESCRIPTORS=1;APS5_RESIDENT_HEAP_MIRRORS=0"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>;APS5_VRAM_WRITABLE=unset:;APS5_VRAM_DEFERRED=unset:;APS5_VRAM_CLEANUP=unset:;APS5_VRAM_TEXTURES=unset:;APS5_TEXTURE_AGE_EVICTION=unset:")
    set_property(TEST agc_vram_descriptors_heap_fallback APPEND PROPERTY ENVIRONMENT "APS5_REBAR_MIB=0")
    foreach(vramWritableMode IN ITEMS off on)
        add_test(NAME agc_vram_writable_${vramWritableMode} COMMAND agc_driver_recorder_tests --vram-writable-only)
        add_test(NAME agc_vram_writable_label_${vramWritableMode} COMMAND agc_driver_vram_writable_label_tests)
        add_test(NAME agc_vram_writable_draw_${vramWritableMode} COMMAND agc_driver_draw_offset_indices_tests --vram-writable-sync)
        add_test(NAME agc_vram_writable_copy_${vramWritableMode} COMMAND agc_driver_copied_writer_copy_tests --vram-writable-copy)
        set_tests_properties(agc_vram_writable_${vramWritableMode} agc_vram_writable_label_${vramWritableMode} agc_vram_writable_draw_${vramWritableMode} agc_vram_writable_copy_${vramWritableMode} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30
            ENVIRONMENT "ANYPS5_REQUIRE_VULKAN=1;ANYPS5_NO_SHADER_CACHE=1;APS5_VRAM_MIRRORS=1;APS5_VRAM_DESCRIPTORS=1;APS5_VRAM_WRITABLE=0;APS5_RESIDENT_HEAP_MIRRORS=0"
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>;APS5_VRAM_DEFERRED=unset:;APS5_VRAM_CLEANUP=unset:;APS5_VRAM_TEXTURES=unset:;APS5_TEXTURE_AGE_EVICTION=unset:")
        set_property(TEST agc_vram_writable_draw_${vramWritableMode} APPEND PROPERTY ENVIRONMENT "APS5_SYNC_DRAWS=1")
    endforeach()
    set_property(TEST agc_vram_writable_on APPEND PROPERTY ENVIRONMENT_MODIFICATION "APS5_VRAM_WRITABLE=set:1")
    set_property(TEST agc_vram_writable_label_on APPEND PROPERTY ENVIRONMENT_MODIFICATION "APS5_VRAM_WRITABLE=set:1")
    set_property(TEST agc_vram_writable_draw_on agc_vram_writable_copy_on APPEND PROPERTY ENVIRONMENT_MODIFICATION "APS5_VRAM_WRITABLE=set:1")
    add_test(NAME agc_vram_writable_recorder COMMAND agc_driver_recorder_tests --vram-writable-recorder-only)
    add_test(NAME agc_vram_writable_descriptors COMMAND agc_driver_recorder_tests --vram-descriptors-only)
    add_test(NAME agc_vram_writable_heap COMMAND agc_driver_recorder_tests --resident-heap-only)
    add_test(NAME agc_vram_writable_heap_fallback COMMAND agc_driver_recorder_tests --resident-heap-only)
    add_test(NAME agc_vram_writable_bda_device COMMAND agc_driver_bda_device_tests)
    add_test(NAME agc_vram_writable_graphics COMMAND agc_driver_graphics_tests --vram-writable-bda)
    add_test(NAME agc_vram_writable_storage_refresh COMMAND agc_driver_recorder_tests --storage-refresh-only)
    add_test(NAME agc_vram_writable_write_tracking COMMAND agc_write_tracking_tests)
    add_test(NAME agc_vram_writable_copied_writer_copy COMMAND agc_driver_copied_writer_copy_tests --vram-writable-copy)
    set_tests_properties(agc_vram_writable_recorder agc_vram_writable_descriptors agc_vram_writable_heap agc_vram_writable_heap_fallback agc_vram_writable_bda_device agc_vram_writable_graphics agc_vram_writable_storage_refresh agc_vram_writable_write_tracking agc_vram_writable_copied_writer_copy
        PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        ENVIRONMENT "ANYPS5_REQUIRE_VULKAN=1;ANYPS5_NO_SHADER_CACHE=1;APS5_VRAM_MIRRORS=1;APS5_VRAM_DESCRIPTORS=1;APS5_VRAM_WRITABLE=1;APS5_RESIDENT_HEAP_MIRRORS=0"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>;APS5_VRAM_DEFERRED=unset:;APS5_VRAM_CLEANUP=unset:;APS5_VRAM_TEXTURES=unset:;APS5_TEXTURE_AGE_EVICTION=unset:")
    set_property(TEST agc_vram_writable_heap_fallback APPEND PROPERTY ENVIRONMENT "APS5_REBAR_MIB=0")
    foreach(vramCleanupMode IN ITEMS off on)
        add_test(NAME agc_vram_cleanup_pool_${vramCleanupMode} COMMAND agc_buffer_pool_tests)
        add_test(NAME agc_vram_cleanup_staging_zero_${vramCleanupMode} COMMAND agc_buffer_pool_tests --staging-zero-only)
        add_test(NAME agc_vram_cleanup_age_${vramCleanupMode} COMMAND agc_driver_recorder_tests --vram-cleanup-only)
        set_tests_properties(agc_vram_cleanup_pool_${vramCleanupMode} agc_vram_cleanup_staging_zero_${vramCleanupMode} agc_vram_cleanup_age_${vramCleanupMode} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60
            ENVIRONMENT "ANYPS5_REQUIRE_VULKAN=1;ANYPS5_NO_SHADER_CACHE=1;APS5_VRAM_MIRRORS=1;APS5_VRAM_DESCRIPTORS=1;APS5_VRAM_WRITABLE=1;APS5_VRAM_DEFERRED=1;APS5_VRAM_CLEANUP=0;APS5_RESIDENT_HEAP_MIRRORS=0"
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>;APS5_REBAR_MIB=unset:;APS5_VRAM_TEXTURES=unset:;APS5_TEXTURE_AGE_EVICTION=unset:")
        set_property(TEST agc_vram_cleanup_staging_zero_${vramCleanupMode} APPEND PROPERTY ENVIRONMENT "APS5_STAGING_POOL_MIB=0")
    endforeach()
    set_property(TEST agc_vram_cleanup_pool_on agc_vram_cleanup_staging_zero_on agc_vram_cleanup_age_on APPEND PROPERTY ENVIRONMENT_MODIFICATION "APS5_VRAM_CLEANUP=set:1")
    add_test(NAME agc_vram_cleanup_recorder COMMAND agc_driver_recorder_tests --vram-writable-recorder-only)
    add_test(NAME agc_vram_cleanup_descriptors COMMAND agc_driver_recorder_tests --vram-descriptors-only)
    add_test(NAME agc_vram_cleanup_heap COMMAND agc_driver_recorder_tests --resident-heap-only)
    add_test(NAME agc_vram_cleanup_bda_device COMMAND agc_driver_bda_device_tests)
    add_test(NAME agc_vram_cleanup_graphics COMMAND agc_driver_graphics_tests --vram-writable-bda)
    add_test(NAME agc_vram_cleanup_storage_refresh COMMAND agc_driver_recorder_tests --storage-refresh-only)
    add_test(NAME agc_vram_cleanup_write_tracking COMMAND agc_write_tracking_tests)
    set_tests_properties(agc_vram_cleanup_recorder agc_vram_cleanup_descriptors agc_vram_cleanup_heap agc_vram_cleanup_bda_device agc_vram_cleanup_graphics agc_vram_cleanup_storage_refresh agc_vram_cleanup_write_tracking
        PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        ENVIRONMENT "ANYPS5_REQUIRE_VULKAN=1;ANYPS5_NO_SHADER_CACHE=1;APS5_VRAM_MIRRORS=1;APS5_VRAM_DESCRIPTORS=1;APS5_VRAM_WRITABLE=1;APS5_VRAM_DEFERRED=1;APS5_VRAM_CLEANUP=1;APS5_RESIDENT_HEAP_MIRRORS=0"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>;APS5_REBAR_MIB=unset:;APS5_VRAM_TEXTURES=unset:;APS5_TEXTURE_AGE_EVICTION=unset:")
endif()

add_test_executable(agc_vram_options_tests EXCLUDE_FROM_ALL tests/VramOptions.cpp)
target_link_libraries(agc_vram_options_tests PRIVATE libc)
target_include_directories(agc_vram_options_tests PRIVATE ${LIBS_INCLUDE_DIR})
set_target_properties(agc_vram_options_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_vram_options_tests)
if(BUILD_TESTING)
    set_target_properties(agc_vram_options_tests PROPERTIES EXCLUDE_FROM_ALL OFF)
    add_test(NAME agc_vram_options_off COMMAND agc_vram_options_tests)
    add_test(NAME agc_vram_options_descriptors COMMAND agc_vram_options_tests)
    add_test(NAME agc_vram_options_descriptors_requires_mirrors COMMAND agc_vram_options_tests "APS5_VRAM_DESCRIPTORS requires APS5_VRAM_MIRRORS=1")
    add_test(NAME agc_vram_options_descriptors_resident_conflict COMMAND agc_vram_options_tests "APS5_RESIDENT_HEAP_MIRRORS to be off")
    set_tests_properties(agc_vram_options_off agc_vram_options_descriptors agc_vram_options_descriptors_requires_mirrors agc_vram_options_descriptors_resident_conflict
        PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>;APS5_VRAM_MIRRORS=unset:;APS5_VRAM_DESCRIPTORS=unset:;APS5_RESIDENT_HEAP_MIRRORS=unset:;APS5_VRAM_WRITABLE=unset:;APS5_VRAM_DEFERRED=unset:;APS5_VRAM_CLEANUP=unset:;APS5_VRAM_TEXTURES=unset:;APS5_TEXTURE_AGE_EVICTION=unset:")
    set_property(TEST agc_vram_options_descriptors APPEND PROPERTY ENVIRONMENT_MODIFICATION "APS5_VRAM_MIRRORS=set:1;APS5_VRAM_DESCRIPTORS=set:1")
    set_property(TEST agc_vram_options_descriptors_requires_mirrors APPEND PROPERTY ENVIRONMENT_MODIFICATION "APS5_VRAM_DESCRIPTORS=set:1")
    set_property(TEST agc_vram_options_descriptors_resident_conflict APPEND PROPERTY ENVIRONMENT_MODIFICATION "APS5_VRAM_MIRRORS=set:1;APS5_VRAM_DESCRIPTORS=set:1;APS5_RESIDENT_HEAP_MIRRORS=set:1")
    add_test(NAME agc_vram_options_writable_requires_descriptors COMMAND agc_vram_options_tests "APS5_VRAM_WRITABLE requires APS5_VRAM_DESCRIPTORS=1")
    set_tests_properties(agc_vram_options_writable_requires_descriptors PROPERTIES ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>;APS5_VRAM_MIRRORS=set:1;APS5_VRAM_DESCRIPTORS=unset:;APS5_VRAM_WRITABLE=set:1;APS5_VRAM_DEFERRED=unset:;APS5_RESIDENT_HEAP_MIRRORS=unset:")
    if(WIN32)
        add_test(NAME agc_vram_options_writable COMMAND agc_vram_options_tests)
        add_test(NAME agc_vram_options_writable_resident_conflict COMMAND agc_vram_options_tests "APS5_RESIDENT_HEAP_MIRRORS to be off")
        set_tests_properties(agc_vram_options_writable agc_vram_options_writable_resident_conflict PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>;APS5_VRAM_MIRRORS=set:1;APS5_VRAM_DESCRIPTORS=set:1;APS5_VRAM_WRITABLE=set:1;APS5_VRAM_DEFERRED=unset:;APS5_RESIDENT_HEAP_MIRRORS=unset:")
        set_property(TEST agc_vram_options_writable_resident_conflict APPEND PROPERTY ENVIRONMENT_MODIFICATION "APS5_RESIDENT_HEAP_MIRRORS=set:1")
        add_test(NAME agc_vram_options_cleanup COMMAND agc_vram_options_tests)
        add_test(NAME agc_vram_options_cleanup_requires_deferred COMMAND agc_vram_options_tests "APS5_VRAM_CLEANUP requires APS5_VRAM_DEFERRED=1")
        add_test(NAME agc_vram_options_cleanup_resident_conflict COMMAND agc_vram_options_tests "APS5_RESIDENT_HEAP_MIRRORS to be off")
        set_tests_properties(agc_vram_options_cleanup agc_vram_options_cleanup_requires_deferred agc_vram_options_cleanup_resident_conflict PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>;APS5_VRAM_MIRRORS=set:1;APS5_VRAM_DESCRIPTORS=set:1;APS5_VRAM_WRITABLE=set:1;APS5_VRAM_DEFERRED=set:1;APS5_VRAM_CLEANUP=set:1;APS5_RESIDENT_HEAP_MIRRORS=unset:;APS5_VRAM_TEXTURES=unset:;APS5_TEXTURE_AGE_EVICTION=unset:")
        set_property(TEST agc_vram_options_cleanup_requires_deferred APPEND PROPERTY ENVIRONMENT_MODIFICATION "APS5_VRAM_DEFERRED=unset:")
        set_property(TEST agc_vram_options_cleanup_resident_conflict APPEND PROPERTY ENVIRONMENT_MODIFICATION "APS5_RESIDENT_HEAP_MIRRORS=set:1")
    else()
        add_test(NAME agc_vram_options_writable_unsupported COMMAND agc_vram_options_tests "APS5_VRAM_WRITABLE requires Windows guest page protection support")
        set_tests_properties(agc_vram_options_writable_unsupported PROPERTIES ENVIRONMENT_MODIFICATION
            "APS5_VRAM_MIRRORS=set:1;APS5_VRAM_DESCRIPTORS=set:1;APS5_VRAM_WRITABLE=set:1;APS5_VRAM_DEFERRED=unset:;APS5_RESIDENT_HEAP_MIRRORS=unset:")
    endif()
endif()

if(BUILD_TESTING)
    foreach(residentStorageMode IN ITEMS off on)
        add_test(NAME agc_resident_storage_${residentStorageMode} COMMAND agc_driver_recorder_tests --resident-storage-only)
        set_tests_properties(agc_resident_storage_${residentStorageMode} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180)
        if(WIN32)
            set_property(TEST agc_resident_storage_${residentStorageMode} PROPERTY ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
        endif()
    endforeach()
    set_property(TEST agc_resident_storage_off APPEND PROPERTY ENVIRONMENT_MODIFICATION "APS5_RESIDENT_STORAGE_IMAGES=unset:")
    set_property(TEST agc_resident_storage_on APPEND PROPERTY ENVIRONMENT "APS5_RESIDENT_STORAGE_IMAGES=1")
    add_test(NAME agc_recorder_resident_storage COMMAND agc_driver_recorder_tests)
    add_test(NAME agc_storage_refresh_resident COMMAND agc_driver_recorder_tests --storage-refresh-only)
    set_tests_properties(agc_recorder_resident_storage agc_storage_refresh_resident PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180 ENVIRONMENT "APS5_RESIDENT_STORAGE_IMAGES=1;ANYPS5_NO_SHADER_CACHE=1")
    if(WIN32)
        set_tests_properties(agc_recorder_resident_storage agc_storage_refresh_resident PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    foreach(indirectFoldMode IN ITEMS off on)
        add_test(NAME agc_driver_indirect_sgpr_fold_${indirectFoldMode} COMMAND agc_driver_draw_offset_indices_tests)
        set_tests_properties(agc_driver_indirect_sgpr_fold_${indirectFoldMode} PROPERTIES SKIP_RETURN_CODE 77 ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1" WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
        if(WIN32)
            set_tests_properties(agc_driver_indirect_sgpr_fold_${indirectFoldMode} PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
        endif()
    endforeach()
    set_property(TEST agc_driver_indirect_sgpr_fold_off APPEND PROPERTY ENVIRONMENT "APS5_INDIRECT_SGPR_FOLD=0")
    set_property(TEST agc_driver_indirect_sgpr_fold_on APPEND PROPERTY ENVIRONMENT "APS5_INDIRECT_SGPR_FOLD=1")
    foreach(instanceOffsetMode IN ITEMS off on)
        add_test(NAME agc_driver_indirect_instance_offset_${instanceOffsetMode} COMMAND agc_driver_indirect_instance_offset_tests)
        set_tests_properties(agc_driver_indirect_instance_offset_${instanceOffsetMode} PROPERTIES SKIP_RETURN_CODE 77 ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1" WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
        if(WIN32)
            set_property(TEST agc_driver_indirect_instance_offset_${instanceOffsetMode} PROPERTY ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
        endif()
    endforeach()
    set_property(TEST agc_driver_indirect_instance_offset_off APPEND PROPERTY ENVIRONMENT "APS5_INDIRECT_SGPR_FOLD=0")
    set_property(TEST agc_driver_indirect_instance_offset_on APPEND PROPERTY ENVIRONMENT "APS5_INDIRECT_SGPR_FOLD=1")
    foreach(drainTest IN ITEMS agc_driver_draw_pipeline_commits agc_driver_same_queue_label_wait agc_driver_gated_dispatch_capture agc_driver_indirect_arguments_written_ahead agc_driver_gpu_indirect_arguments_written_ahead agc_driver_write_data_written_ahead)
        add_test(NAME ${drainTest}_narrow COMMAND ${drainTest}_tests)
        foreach(drainProperty IN ITEMS ENVIRONMENT ENVIRONMENT_MODIFICATION SKIP_RETURN_CODE TIMEOUT FAIL_REGULAR_EXPRESSION PASS_REGULAR_EXPRESSION WILL_FAIL WORKING_DIRECTORY)
            get_test_property(${drainTest} ${drainProperty} drainValue)
            if(NOT drainValue STREQUAL "NOTFOUND")
                set_property(TEST ${drainTest}_narrow PROPERTY ${drainProperty} "${drainValue}")
            endif()
        endforeach()
        set_property(TEST ${drainTest}_narrow APPEND PROPERTY ENVIRONMENT "APS5_NARROW_PACKET_DRAINS=1")
    endforeach()
endif()

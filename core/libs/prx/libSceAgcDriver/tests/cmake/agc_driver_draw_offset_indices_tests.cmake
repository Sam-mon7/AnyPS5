if(BUILD_TESTING)
    foreach(runtimeFetchIndex IN ITEMS 65535 65536 65537)
        foreach(runtimeFetchPath IN ITEMS indexed compact-restart)
            set(runtimeFetchArguments runtime-fetch ${runtimeFetchIndex})
            if(runtimeFetchPath STREQUAL "compact-restart")
                list(APPEND runtimeFetchArguments compact-restart)
            endif()
            set(runtimeFetchTest agc_driver_runtime_fetch_${runtimeFetchPath}_${runtimeFetchIndex})
            add_test(NAME ${runtimeFetchTest} COMMAND agc_driver_draw_offset_indices_tests ${runtimeFetchArguments})
            set_tests_properties(${runtimeFetchTest} PROPERTIES
                    ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1" TIMEOUT 45 SKIP_RETURN_CODE 77
                    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
            if(WIN32)
                set_tests_properties(${runtimeFetchTest} PROPERTIES ENVIRONMENT_MODIFICATION
                        "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
            endif()
        endforeach()
        foreach(runtimeFetchScan IN ITEMS skipped kept)
            set(runtimeFetchTest agc_driver_runtime_fetch_in_place_${runtimeFetchScan}_${runtimeFetchIndex})
            add_test(NAME ${runtimeFetchTest} COMMAND agc_driver_draw_offset_indices_tests runtime-fetch ${runtimeFetchIndex} in-place)
            set_tests_properties(${runtimeFetchTest} PROPERTIES
                    ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_DRAW_INPUTS_IN_PLACE=1;APS5_ARCHB_FIX_DERIVED_INDEX_CACHE=0" TIMEOUT 45 SKIP_RETURN_CODE 77
                    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
            if(runtimeFetchScan STREQUAL "skipped")
                set_property(TEST ${runtimeFetchTest} PROPERTY ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_DRAW_INPUTS_IN_PLACE=1;APS5_ARCHB_FIX_DERIVED_INDEX_CACHE=1;TEST_EXPECT_HIGHEST_UNUSED=1")
            endif()
            if(WIN32)
                set_tests_properties(${runtimeFetchTest} PROPERTIES ENVIRONMENT_MODIFICATION
                        "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
            endif()
        endforeach()
    endforeach()
endif()

target_include_directories(agc_driver_draw_offset_indices_tests PRIVATE $<TARGET_PROPERTY:shader_recompiler_agc_driver,INTERFACE_INCLUDE_DIRECTORIES>)

if(BUILD_TESTING)
    add_test(NAME agc_driver_indirect_push_patch COMMAND agc_driver_draw_offset_indices_tests --indirect-push-patch)
    add_test(NAME agc_driver_indirect_push_patch_disabled COMMAND agc_driver_draw_offset_indices_tests --indirect-push-patch)
    add_test(NAME agc_driver_indirect_push_patch_verify COMMAND agc_driver_draw_offset_indices_tests --indirect-push-patch)
    add_test(NAME agc_driver_indirect_push_patch_buffer_base COMMAND agc_driver_draw_offset_indices_tests --indirect-buffer-base)
    set_property(TEST agc_driver_indirect_push_patch_disabled APPEND PROPERTY ENVIRONMENT_MODIFICATION "APS5_NO_INDIRECT_PUSH_PATCH=set:1")
    set_property(TEST agc_driver_indirect_push_patch_verify APPEND PROPERTY ENVIRONMENT_MODIFICATION "APS5_VERIFY_INDIRECT_PUSH_PATCH=set:1")
    add_test(NAME agc_driver_indirect_builtin_offsets COMMAND agc_driver_draw_offset_indices_tests --builtin-offsets)
endif()

if(BUILD_TESTING)
    set_property(TEST agc_driver_draw_offset_indices APPEND PROPERTY ENVIRONMENT_MODIFICATION "APS5_NO_INDIRECT_BUILTIN_OFFSETS=set:1")
endif()

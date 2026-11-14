add_test_executable(agc_masked_select_tests EXCLUDE_FROM_ALL tests/MaskedSelect.cpp)
target_link_libraries(agc_masked_select_tests PRIVATE shader_recompiler_agc_driver libc)
set_target_properties(agc_masked_select_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_masked_select_tests)

if(BUILD_TESTING)
    agc_add_default_test(agc_masked_select_tests)
    add_test(NAME agc_masked_select_strict_stencil COMMAND agc_masked_select_tests)
    set_tests_properties(agc_masked_select_strict_stencil PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_STRICT_STENCIL_REPLAY=1" SKIP_RETURN_CODE 77 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    if(WIN32)
        set_tests_properties(agc_masked_select_strict_stencil PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

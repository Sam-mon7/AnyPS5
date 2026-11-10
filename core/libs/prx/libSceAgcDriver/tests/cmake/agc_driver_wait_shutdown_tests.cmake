add_test_executable(agc_driver_wait_shutdown_tests EXCLUDE_FROM_ALL tests/WaitShutdown.cpp)
target_include_directories(agc_driver_wait_shutdown_tests PRIVATE
        ${LIBS_INCLUDE_DIR}
        ${ANYPS5_VULKAN_HEADERS_INCLUDE_DIR}
        ${ANYPS5_SPIRV_HEADERS_INCLUDE_DIR}
        $<TARGET_PROPERTY:shader_recompiler_agc_driver,INTERFACE_INCLUDE_DIRECTORIES>
)
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
    add_test(NAME agc_driver_wait_epochs COMMAND agc_driver_wait_shutdown_tests --wait-epochs)
    add_test(NAME agc_driver_wait_epochs_legacy COMMAND agc_driver_wait_shutdown_tests --wait-epochs)
    set_tests_properties(agc_driver_wait_epochs agc_driver_wait_epochs_legacy PROPERTIES
            SKIP_RETURN_CODE 77 TIMEOUT 10 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            ENVIRONMENT_MODIFICATION "APS5_NO_COLLECT_MEMO=unset:")
    set_property(TEST agc_driver_wait_epochs APPEND PROPERTY ENVIRONMENT_MODIFICATION "APS5_NO_HELD_WAIT_EPOCH=unset:")
    set_tests_properties(agc_driver_wait_epochs_legacy PROPERTIES ENVIRONMENT "APS5_NO_HELD_WAIT_EPOCH=1")
    add_test(NAME agc_driver_wait_history COMMAND agc_driver_wait_shutdown_tests --wait-history)
    add_test(NAME agc_driver_wait_history_legacy COMMAND agc_driver_wait_shutdown_tests --wait-history)
    set_tests_properties(agc_driver_wait_history agc_driver_wait_history_legacy PROPERTIES
            TIMEOUT 10 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_property(TEST agc_driver_wait_history APPEND PROPERTY ENVIRONMENT_MODIFICATION "APS5_NO_WAIT_HISTORY_REUSE=unset:")
    set_tests_properties(agc_driver_wait_history_legacy PROPERTIES ENVIRONMENT "APS5_NO_WAIT_HISTORY_REUSE=1")
    if(WIN32)
        set_property(TEST agc_driver_wait_shutdown agc_driver_resolution_shutdown agc_driver_wait_epochs agc_driver_wait_epochs_legacy agc_driver_wait_history agc_driver_wait_history_legacy APPEND PROPERTY ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

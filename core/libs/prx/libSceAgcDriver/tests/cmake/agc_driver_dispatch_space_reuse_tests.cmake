if(BUILD_TESTING)
    set_tests_properties(agc_driver_dispatch_space_reuse PROPERTIES TIMEOUT 30)
endif()
if(BUILD_TESTING AND Python3_Interpreter_FOUND)
    foreach(agcDispatchSpaceCase IN ITEMS compare verify negative in-flight remap imports snapshot writable)
        string(REPLACE "-" "_" agcDispatchSpaceName "${agcDispatchSpaceCase}")
        add_test(NAME agc_driver_dispatch_space_reuse_${agcDispatchSpaceName}
            COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_dispatch_space_reuse.py"
            "$<TARGET_FILE:agc_driver_dispatch_space_reuse_tests>" "${agcDispatchSpaceCase}")
        set_tests_properties(agc_driver_dispatch_space_reuse_${agcDispatchSpaceName} PROPERTIES
            ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1"
            SKIP_RETURN_CODE 77 TIMEOUT 30 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
        if(WIN32)
            set_tests_properties(agc_driver_dispatch_space_reuse_${agcDispatchSpaceName} PROPERTIES
                ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
        endif()
    endforeach()
endif()

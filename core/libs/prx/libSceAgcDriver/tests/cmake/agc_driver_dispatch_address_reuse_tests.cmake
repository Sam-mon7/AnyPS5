if(BUILD_TESTING)
    set_tests_properties(agc_driver_dispatch_address_reuse PROPERTIES
        ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_ARCHB_DISPATCH_ADDRESS_REUSE=0;APS5_RELEASE_ON_UNLOCK=1;APS5_NO_OPPORTUNISTIC_REAP=1"
        TIMEOUT 30)
endif()
if(BUILD_TESTING AND Python3_Interpreter_FOUND)
    foreach(agcAddressReuseCase IN ITEMS compare verify negative cache in-flight remap imports ranges no-pool)
        string(REPLACE "-" "_" agcAddressReuseName "${agcAddressReuseCase}")
        add_test(NAME agc_driver_dispatch_address_reuse_${agcAddressReuseName}
            COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_dispatch_address_reuse.py"
            "$<TARGET_FILE:agc_driver_dispatch_address_reuse_tests>" "${agcAddressReuseCase}")
        set_tests_properties(agc_driver_dispatch_address_reuse_${agcAddressReuseName} PROPERTIES
            ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1"
            SKIP_RETURN_CODE 77 TIMEOUT 30 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
        if(WIN32)
            set_tests_properties(agc_driver_dispatch_address_reuse_${agcAddressReuseName} PROPERTIES
                ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
        endif()
    endforeach()
endif()

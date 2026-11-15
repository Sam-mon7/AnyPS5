if(BUILD_TESTING AND Python3_EXECUTABLE AND TARGET agc_driver_dispatch_data_hit_words_tests)
    add_test(NAME agc_driver_dispatch_timing_analyzer COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/tests/test_dispatch_timing.py)
    set_tests_properties(agc_driver_dispatch_timing_analyzer PROPERTIES TIMEOUT 30)
    foreach(dispatchTimingMode IN ITEMS off on)
        set(dispatchTimingTest agc_driver_dispatch_timing_${dispatchTimingMode})
        add_test(NAME ${dispatchTimingTest} COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_dispatch_timing.py
            $<TARGET_FILE:agc_driver_dispatch_data_hit_words_tests>
            ${CMAKE_SOURCE_DIR}/tools/dispatch_timing.py
            ${CMAKE_CURRENT_BINARY_DIR}/dispatch-timing-tests
            ${dispatchTimingMode})
        set_tests_properties(${dispatchTimingTest} PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_NO_PIPELINE_JOURNAL=1")
        if(WIN32)
            set_property(TEST ${dispatchTimingTest} APPEND PROPERTY ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
        endif()
    endforeach()
endif()

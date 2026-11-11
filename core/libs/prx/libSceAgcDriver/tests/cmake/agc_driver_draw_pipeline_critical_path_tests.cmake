if(BUILD_TESTING)
    set_tests_properties(agc_driver_draw_pipeline_critical_path PROPERTIES
        ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_PIPELINED_DRAWS=4;APS5_CRITICAL_PATH=0;APS5_HANDOFF_SPIN_US=0"
        TIMEOUT 30)
    if(Python3_Interpreter_FOUND)
        foreach(mode IN ITEMS full hysteresis no-block overcommit idle stop)
            string(REPLACE "-" "_" suffix "${mode}")
            add_test(NAME agc_driver_draw_pipeline_critical_path_${suffix}
                COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/execution/DrawPipelineCriticalPath.py"
                    "$<TARGET_FILE:agc_driver_draw_pipeline_critical_path_tests>"
                    "${CMAKE_SOURCE_DIR}/tools/critical_path.py"
                    "${CMAKE_CURRENT_BINARY_DIR}/critical_path_pipe_tests" "${mode}")
            set_tests_properties(agc_driver_draw_pipeline_critical_path_${suffix} PROPERTIES
                ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1" TIMEOUT 30 SKIP_RETURN_CODE 77
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
            if(WIN32)
                set_property(TEST agc_driver_draw_pipeline_critical_path_${suffix} APPEND PROPERTY
                    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
            endif()
        endforeach()
    endif()
endif()

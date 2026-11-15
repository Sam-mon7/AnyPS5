if(BUILD_TESTING)
    foreach(handoffMode IN ITEMS off on verify)
        set(handoffTest agc_driver_label_handoff_${handoffMode})
        add_test(NAME ${handoffTest} COMMAND agc_driver_label_handoff_tests ${handoffMode})
        set_tests_properties(${handoffTest} PROPERTIES TIMEOUT 30 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            ENVIRONMENT "APS5_ARCHB_RECORDER=1;APS5_PIPELINED_DRAWS=8"
            ENVIRONMENT_MODIFICATION "APS5_ARCHB_FIX_LABEL_HANDOFF=set:0;APS5_ARCHB_FIX_LABEL_HANDOFF_VERIFY=unset:;APS5_ARCHB_CONTROL_FILE=unset:;APS5_ARCHB_PIPE_RESOLVE_POOL=set:0")
        if(NOT handoffMode STREQUAL "off")
            set_property(TEST ${handoffTest} APPEND PROPERTY ENVIRONMENT_MODIFICATION "APS5_ARCHB_FIX_LABEL_HANDOFF=set:1")
        endif()
        if(handoffMode STREQUAL "verify")
            set_property(TEST ${handoffTest} APPEND PROPERTY ENVIRONMENT_MODIFICATION "APS5_ARCHB_FIX_LABEL_HANDOFF_VERIFY=set:1")
        endif()
        if(WIN32)
            set_property(TEST ${handoffTest} APPEND PROPERTY ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
        endif()
    endforeach()
    set_tests_properties(agc_driver_label_handoff PROPERTIES TIMEOUT 30
        ENVIRONMENT "APS5_ARCHB_RECORDER=1;APS5_PIPELINED_DRAWS=8"
        ENVIRONMENT_MODIFICATION "APS5_ARCHB_FIX_LABEL_HANDOFF=set:0;APS5_ARCHB_FIX_LABEL_HANDOFF_VERIFY=unset:;APS5_ARCHB_CONTROL_FILE=unset:;APS5_ARCHB_PIPE_RESOLVE_POOL=set:0")
    if(WIN32)
        set_property(TEST agc_driver_label_handoff APPEND PROPERTY ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

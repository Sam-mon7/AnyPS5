if(BUILD_TESTING)
    add_test(NAME agc_driver_address_draw_space_reuse_shadow_ranges COMMAND agc_driver_address_draw_space_reuse_tests --case shadow-ranges)
    set_tests_properties(agc_driver_address_draw_space_reuse_shadow_ranges PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
    if(WIN32)
        set_tests_properties(agc_driver_address_draw_space_reuse_shadow_ranges PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test(NAME agc_driver_address_draw_space_reuse_pool_faults COMMAND agc_driver_address_draw_space_reuse_tests --case fault-per-use)
    set_tests_properties(agc_driver_address_draw_space_reuse_pool_faults PROPERTIES
        ENVIRONMENT "APS5_ARCHB_FIX_DRAW_SET_POOL=1;APS5_ARCHB_FIX_MOVED_WORD_UPLOAD=1;APS5_ARCHB_ZERO_COPY_STORAGE=1;APS5_ARCHB_ZERO_COPY_MOVED=1"
        SKIP_RETURN_CODE 77 TIMEOUT 60)
    if(WIN32)
        set_tests_properties(agc_driver_address_draw_space_reuse_pool_faults PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test(NAME agc_driver_address_draw_space_reuse_off COMMAND agc_driver_address_draw_space_reuse_tests --off)
    set_tests_properties(agc_driver_address_draw_space_reuse_off PROPERTIES ENVIRONMENT "APS5_NO_ADDRESS_DRAW_REUSE=1" SKIP_RETURN_CODE 77 TIMEOUT 60)
    if(WIN32)
        set_tests_properties(agc_driver_address_draw_space_reuse_off PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test(NAME agc_driver_address_draw_space_reuse_transitions COMMAND agc_driver_address_draw_space_reuse_tests --transitions)
    set_tests_properties(agc_driver_address_draw_space_reuse_transitions PROPERTIES ENVIRONMENT "APS5_DRAW_TRANSITIONS=1" SKIP_RETURN_CODE 77 TIMEOUT 60)
    if(WIN32)
        set_tests_properties(agc_driver_address_draw_space_reuse_transitions PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test(NAME agc_driver_address_draw_space_reuse_verify COMMAND agc_driver_address_draw_space_reuse_tests --verify)
    set_tests_properties(agc_driver_address_draw_space_reuse_verify PROPERTIES ENVIRONMENT "APS5_VERIFY_ADDRESS_DRAW_REUSE=1" SKIP_RETURN_CODE 77 TIMEOUT 60)
    if(WIN32)
        set_tests_properties(agc_driver_address_draw_space_reuse_verify PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test(NAME agc_driver_address_draw_space_reuse_held COMMAND agc_driver_address_draw_space_reuse_tests --held)
    set_tests_properties(agc_driver_address_draw_space_reuse_held PROPERTIES ENVIRONMENT "APS5_RELEASE_ON_UNLOCK=1" SKIP_RETURN_CODE 77 TIMEOUT 60)
    if(WIN32)
        set_tests_properties(agc_driver_address_draw_space_reuse_held PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

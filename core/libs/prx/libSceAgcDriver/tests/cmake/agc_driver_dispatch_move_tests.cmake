if(BUILD_TESTING)
    add_test(NAME agc_driver_private_compute_completion COMMAND agc_driver_dispatch_move_tests --completion-recipe)
    set_tests_properties(agc_driver_private_compute_completion PROPERTIES
        ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_PRIVATE_COMPUTE=1;APS5_PROFILE_DRAW=1;APS5_VERIFY_RECIPE=1" SKIP_RETURN_CODE 77 TIMEOUT 30 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    if(WIN32)
        set_tests_properties(agc_driver_private_compute_completion PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    foreach(agcCompletionRecipeMode IN ITEMS on off)
        add_test(NAME agc_driver_dispatch_completion_recipe_${agcCompletionRecipeMode} COMMAND agc_driver_dispatch_move_tests --completion-recipe)
        set_tests_properties(agc_driver_dispatch_completion_recipe_${agcCompletionRecipeMode} PROPERTIES
            ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_VERIFY_RECIPE=1" SKIP_RETURN_CODE 77 TIMEOUT 30 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
        if(agcCompletionRecipeMode STREQUAL "off")
            set_property(TEST agc_driver_dispatch_completion_recipe_${agcCompletionRecipeMode} APPEND PROPERTY ENVIRONMENT "APS5_NO_COMPLETION_RECIPE=1")
        endif()
        if(WIN32)
            set_tests_properties(agc_driver_dispatch_completion_recipe_${agcCompletionRecipeMode} PROPERTIES
                ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
        endif()
    endforeach()
endif()
if(BUILD_TESTING AND Python3_Interpreter_FOUND)
    add_test(NAME agc_driver_dispatch_build_trace COMMAND "${Python3_EXECUTABLE}" -I
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_dispatch_build_trace.py"
        "$<TARGET_FILE:agc_driver_dispatch_move_tests>"
        "$<TARGET_FILE:agc_driver_dispatch_data_hit_words_tests>")
    set_tests_properties(agc_driver_dispatch_build_trace PROPERTIES
        ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1" SKIP_RETURN_CODE 77 TIMEOUT 30 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    if(WIN32)
        set_tests_properties(agc_driver_dispatch_build_trace PROPERTIES
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

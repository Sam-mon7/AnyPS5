add_test_executable(agc_archb_gen_tests EXCLUDE_FROM_ALL tests/GenTable.cpp)
target_include_directories(agc_archb_gen_tests PRIVATE
        ${LIBS_INCLUDE_DIR}
        ${CMAKE_CURRENT_BINARY_DIR}/generated
        ${ANYPS5_VULKAN_HEADERS_INCLUDE_DIR}
        ${ANYPS5_SPIRV_HEADERS_INCLUDE_DIR}
        $<TARGET_PROPERTY:shader_recompiler_agc_driver,INTERFACE_INCLUDE_DIRECTORIES>
)
target_link_libraries(agc_archb_gen_tests PRIVATE libSceAgcDriver libc)
set_target_properties(agc_archb_gen_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_archb_gen_tests)

if(BUILD_TESTING)
    set_target_properties(agc_archb_gen_tests PROPERTIES EXCLUDE_FROM_ALL OFF)
    foreach(genCase IN ITEMS monotonic events coarse tickets journal check stress shadow-classify lockfree draw-validation)
        string(REPLACE "-" "_" genName "${genCase}")
        add_test(NAME agc_archb_gen_table_${genName} COMMAND agc_archb_gen_tests --${genCase})
        set_tests_properties(agc_archb_gen_table_${genName} PROPERTIES TIMEOUT 30)
        if(WIN32)
            set_tests_properties(agc_archb_gen_table_${genName} PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
        endif()
    endforeach()
endif()

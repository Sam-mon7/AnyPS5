add_test_executable(agc_buffer_pool_tests EXCLUDE_FROM_ALL tests/BufferPool.cpp)
target_sources(agc_buffer_pool_tests PRIVATE Graphics/src/BufferPool.cpp Graphics/src/Resources.cpp tests/AllocationFailure.cpp)
target_include_directories(agc_buffer_pool_tests PRIVATE ${LIBS_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/core/shader/recompiler ${ANYPS5_VULKAN_HEADERS_INCLUDE_DIR})
target_link_libraries(agc_buffer_pool_tests PRIVATE libSceAgcDriver libc)
set_target_properties(agc_buffer_pool_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_buffer_pool_tests)

if(BUILD_TESTING)
    set_target_properties(agc_buffer_pool_tests PROPERTIES EXCLUDE_FROM_ALL OFF)
    add_test(NAME agc_buffer_pool COMMAND agc_buffer_pool_tests)
    add_test(NAME agc_memory_accounting COMMAND agc_buffer_pool_tests --memory-accounting)
    set_tests_properties(agc_memory_accounting PROPERTIES ENVIRONMENT "APS5_TRACE_MEMORY_BUDGET=1" TIMEOUT 30)
    add_test(NAME agc_memory_accounting_budget_cap COMMAND agc_buffer_pool_tests --memory-accounting-budget-cap)
    set_tests_properties(agc_memory_accounting_budget_cap PROPERTIES ENVIRONMENT "APS5_TRACE_MEMORY_BUDGET=1"
        PASS_REGULAR_EXPRESSION "cap=1073741824 selected=1073741824" TIMEOUT 30)
    add_test(NAME agc_memory_accounting_budget_floor COMMAND agc_buffer_pool_tests --memory-accounting-budget-floor)
    set_tests_properties(agc_memory_accounting_budget_floor PROPERTIES ENVIRONMENT "APS5_TRACE_MEMORY_BUDGET=1"
        PASS_REGULAR_EXPRESSION "cap=536870912 selected=536870912" TIMEOUT 30)
    foreach(idleCase IN ITEMS enabled disabled failure)
        if(idleCase STREQUAL "failure")
            add_test(NAME agc_buffer_pool_idle_${idleCase} COMMAND agc_buffer_pool_tests --idle-trim-failure)
        else()
            add_test(NAME agc_buffer_pool_idle_${idleCase} COMMAND agc_buffer_pool_tests --idle-trim)
        endif()
        set_tests_properties(agc_buffer_pool_idle_${idleCase} PROPERTIES TIMEOUT 30
            ENVIRONMENT "APS5_TEXTURE_SCRATCH_POOL=1;APS5_STAGING_POOL_MIB=512;APS5_IMAGE_POOL_MIB=128;APS5_BUFFER_POOL_SLOTS=4096"
            ENVIRONMENT_MODIFICATION "APS5_NO_IDLE_POOL_TRIM=unset:;APS5_TEXTURE_SCRATCH_DEVICE_MIB=unset:;APS5_TEXTURE_SCRATCH_HOST_MIB=unset:;APS5_NO_BUFFER_CLASSES=unset:;APS5_BUFFER_POOL_SHARED=unset:")
        if(idleCase STREQUAL "disabled")
            set_property(TEST agc_buffer_pool_idle_${idleCase} APPEND PROPERTY ENVIRONMENT_MODIFICATION "APS5_NO_IDLE_POOL_TRIM=set:1")
        endif()
        if(WIN32)
            set_property(TEST agc_buffer_pool_idle_${idleCase} APPEND PROPERTY ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
        endif()
    endforeach()
    set_tests_properties(agc_memory_accounting_budget_cap agc_memory_accounting_budget_floor PROPERTIES
        FAIL_REGULAR_EXPRESSION "FAIL:")
    add_test(NAME agc_buffer_pool_small_device_shared COMMAND agc_buffer_pool_tests --small-device-retention)
    set_tests_properties(agc_buffer_pool_small_device_shared PROPERTIES ENVIRONMENT
        "APS5_STAGING_POOL_MIB=256;APS5_BUFFER_POOL_SHARED=1")
    add_test(NAME agc_buffer_pool_small_device_no_staging COMMAND agc_buffer_pool_tests --small-device-retention)
    set_tests_properties(agc_buffer_pool_small_device_no_staging PROPERTIES ENVIRONMENT
        "APS5_STAGING_POOL_MIB=0")
    add_test(NAME agc_memory_accounting_budget_snapshot COMMAND agc_buffer_pool_tests --memory-accounting-budget-snapshot)
    set_tests_properties(agc_memory_accounting_budget_snapshot PROPERTIES ENVIRONMENT "APS5_TRACE_MEMORY_BUDGET=1"
        PASS_REGULAR_EXPRESSION "storage heap=2 sampled_live=1073741824 storage_cached=2147483648 reserved=8321499136 available=2415919104 cap=5368709120 selected=2415919104"
        FAIL_REGULAR_EXPRESSION "FAIL:" TIMEOUT 30)
    if(WIN32)
        set_tests_properties(agc_memory_accounting agc_memory_accounting_budget_cap agc_memory_accounting_budget_floor agc_memory_accounting_budget_snapshot agc_buffer_pool agc_buffer_pool_small_device_shared agc_buffer_pool_small_device_no_staging PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

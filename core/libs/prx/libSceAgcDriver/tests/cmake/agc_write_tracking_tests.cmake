add_test_executable(agc_write_tracking_tests EXCLUDE_FROM_ALL
        tests/WriteTracking.cpp
)
target_include_directories(agc_write_tracking_tests PRIVATE ${LIBS_INCLUDE_DIR})
target_link_libraries(agc_write_tracking_tests PRIVATE libSceAgcDriver libc)
if(WIN32)
    target_link_libraries(agc_write_tracking_tests PRIVATE libSceFiber)
endif()
set_target_properties(agc_write_tracking_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_write_tracking_tests)

if(BUILD_TESTING)
    set_target_properties(agc_write_tracking_tests PROPERTIES EXCLUDE_FROM_ALL OFF)
    add_test(NAME agc_write_tracking COMMAND agc_write_tracking_tests)
    set_tests_properties(agc_write_tracking PROPERTIES SKIP_RETURN_CODE 77)
    add_test(NAME agc_write_tracking_generation_wrap COMMAND agc_write_tracking_tests --generation-wrap)
    set_tests_properties(agc_write_tracking_generation_wrap PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 20)
    add_test(NAME agc_write_tracking_mark_order COMMAND agc_write_tracking_tests --mark-order)
    set_tests_properties(agc_write_tracking_mark_order PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 20)
    if(WIN32)
        set_tests_properties(agc_write_tracking_mark_order PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
        add_test(NAME agc_write_tracking_metadata_commit COMMAND agc_write_tracking_tests --metadata-commit)
        set_tests_properties(agc_write_tracking_metadata_commit PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
        add_test(NAME agc_write_tracking_cpu_write_pin COMMAND agc_write_tracking_tests --cpu-write-pin)
        set_tests_properties(agc_write_tracking_cpu_write_pin PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 20)
    endif()
    add_test(NAME agc_archb_gen_tracker_wrap COMMAND agc_write_tracking_tests --gen-tracker-wrap)
    set_tests_properties(agc_archb_gen_tracker_wrap PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
    add_test(NAME agc_archb_gen_shadow_tracker COMMAND agc_write_tracking_tests --gen-shadow)
    add_test(NAME agc_archb_gen_verify_catches COMMAND agc_write_tracking_tests --gen-verify-catches)
    set_tests_properties(agc_archb_gen_shadow_tracker agc_archb_gen_verify_catches PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30
        ENVIRONMENT "APS5_ARCHB_GEN_TABLE=1;APS5_ARCHB_GEN_SHADOW=1;APS5_ARCHB_GEN_VERIFY=1")
    if(WIN32)
        set_tests_properties(agc_archb_gen_tracker_wrap agc_archb_gen_shadow_tracker agc_archb_gen_verify_catches PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test(NAME agc_archb_gen_lockfree COMMAND agc_write_tracking_tests --gen-lockfree)
    add_test(NAME agc_archb_gen_lockfree_verify_catches COMMAND agc_write_tracking_tests --gen-lockfree-verify-catches)
    add_test(NAME agc_archb_gen_lockfree_stress COMMAND agc_write_tracking_tests --gen-lockfree-stress)
    set_tests_properties(agc_archb_gen_lockfree agc_archb_gen_lockfree_verify_catches agc_archb_gen_lockfree_stress PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30
        ENVIRONMENT "APS5_ARCHB_GEN_TABLE=1;APS5_ARCHB_GEN_LOCKFREE=1;APS5_ARCHB_GEN_VERIFY=1")
    add_test(NAME agc_write_tracking_gen_lockfree COMMAND agc_write_tracking_tests)
    set_tests_properties(agc_write_tracking_gen_lockfree PROPERTIES SKIP_RETURN_CODE 77
        ENVIRONMENT "APS5_ARCHB_GEN_TABLE=1;APS5_ARCHB_GEN_LOCKFREE=1;APS5_ARCHB_GEN_VERIFY=1;APS5_ARCHB_GEN_VERIFY_ABORT=1")
    if(WIN32)
        set_tests_properties(agc_archb_gen_lockfree agc_archb_gen_lockfree_verify_catches agc_archb_gen_lockfree_stress agc_write_tracking_gen_lockfree PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test(NAME agc_archb_fix_snapshot_page_refresh_pages COMMAND agc_write_tracking_tests --changed-pages)
    set_tests_properties(agc_archb_fix_snapshot_page_refresh_pages PROPERTIES
        SKIP_RETURN_CODE 77 TIMEOUT 30 ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    add_test(NAME agc_archb_fix_unstamped_alias_tracker_off COMMAND agc_write_tracking_tests --unstamped-alias)
    add_test(NAME agc_archb_fix_unstamped_alias_tracker_on COMMAND agc_write_tracking_tests --unstamped-alias)
    add_test(NAME agc_archb_fix_unstamped_alias_suite COMMAND agc_write_tracking_tests)
    set_tests_properties(agc_archb_fix_unstamped_alias_tracker_off agc_archb_fix_unstamped_alias_tracker_on agc_archb_fix_unstamped_alias_suite PROPERTIES
        SKIP_RETURN_CODE 77 TIMEOUT 30 ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    set_tests_properties(agc_archb_fix_unstamped_alias_tracker_on agc_archb_fix_unstamped_alias_suite PROPERTIES ENVIRONMENT "APS5_ARCHB_FIX_UNSTAMPED_ALIAS=1")
    add_test(NAME agc_archb_wt_fault COMMAND agc_write_tracking_tests --wt-fault)
    if(WIN32)
        foreach(stackCase IN ITEMS registered armed watched lazy fiber before-enable overlap replaced guard guard-pointer off cost)
            add_test(NAME agc_archb_wt_fault_stack_${stackCase} COMMAND agc_write_tracking_tests --wt-stack-${stackCase})
            set(stackFault 1)
            if(stackCase STREQUAL "off")
                set(stackFault 0)
            endif()
            set_tests_properties(agc_archb_wt_fault_stack_${stackCase} PROPERTIES TIMEOUT 30
                ENVIRONMENT "APS5_ARCHB_WT_FAULT=${stackFault};APS5_ARCHB_WT_FAULT_VERIFY=${stackFault};APS5_ARCHB_WT_FAULT_CONSUMERS=${stackFault};APS5_ARCHB_WT_SYNC_SKIP=${stackFault};APS5_WT_FAULT_GUEST_HOT=0;APS5_ARCHB_WT_FAULT_DIRTY_ARM=0"
                ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
        endforeach()
    endif()
    add_test(NAME agc_archb_wt_fault_verify COMMAND agc_write_tracking_tests --wt-fault)
    add_test(NAME agc_archb_wt_fault_suite COMMAND agc_write_tracking_tests)
    set_tests_properties(agc_archb_wt_fault agc_archb_wt_fault_verify agc_archb_wt_fault_suite PROPERTIES
        SKIP_RETURN_CODE 77 TIMEOUT 60 ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    set_tests_properties(agc_archb_wt_fault PROPERTIES ENVIRONMENT "APS5_ARCHB_WT_FAULT=1;APS5_ARCHB_WT_FAULT_VERIFY=0;APS5_ARCHB_WT_FAULT_CONSUMERS=0;APS5_WT_FAULT_GUEST_HOT=0;APS5_ARCHB_WT_FAULT_DIRTY_ARM=0")
    set_tests_properties(agc_archb_wt_fault_verify agc_archb_wt_fault_suite PROPERTIES ENVIRONMENT "APS5_ARCHB_WT_FAULT=1;APS5_ARCHB_WT_FAULT_VERIFY=1;APS5_ARCHB_WT_FAULT_CONSUMERS=0;APS5_WT_FAULT_GUEST_HOT=0;APS5_ARCHB_WT_FAULT_DIRTY_ARM=0")
    add_test(NAME agc_archb_wt_fault_host_copy COMMAND agc_write_tracking_tests --wt-fault-host-copy)
    add_test(NAME agc_archb_wt_fault_host_copy_kept COMMAND agc_write_tracking_tests --wt-fault-host-copy)
    set_tests_properties(agc_archb_wt_fault_host_copy agc_archb_wt_fault_host_copy_kept PROPERTIES
        SKIP_RETURN_CODE 77 TIMEOUT 60 ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    set_tests_properties(agc_archb_wt_fault_host_copy PROPERTIES ENVIRONMENT "APS5_ARCHB_WT_FAULT=1;APS5_ARCHB_WT_FAULT_VERIFY=0;APS5_ARCHB_WT_FAULT_CONSUMERS=0;APS5_WT_FAULT_GUEST_HOT=0;APS5_ARCHB_WT_FAULT_DIRTY_ARM=0")
    set_tests_properties(agc_archb_wt_fault_host_copy_kept PROPERTIES ENVIRONMENT "APS5_ARCHB_WT_FAULT=1;APS5_ARCHB_WT_FAULT_VERIFY=1;APS5_ARCHB_WT_FAULT_CONSUMERS=1;APS5_ARCHB_WT_SYNC_SKIP=1;APS5_ARCHB_WT_FAULT_DIRTY_ARM=0")
    add_test(NAME agc_archb_wt_fault_consumers COMMAND agc_write_tracking_tests --wt-fault-consumers)
    add_test(NAME agc_archb_wt_fault_consumers_verify COMMAND agc_write_tracking_tests --wt-fault-consumers)
    add_test(NAME agc_archb_wt_fault_consumers_suite COMMAND agc_write_tracking_tests)
    set_tests_properties(agc_archb_wt_fault_consumers agc_archb_wt_fault_consumers_verify agc_archb_wt_fault_consumers_suite PROPERTIES
        SKIP_RETURN_CODE 77 TIMEOUT 60 ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    set_tests_properties(agc_archb_wt_fault_consumers PROPERTIES ENVIRONMENT "APS5_ARCHB_WT_FAULT=1;APS5_ARCHB_WT_FAULT_VERIFY=0;APS5_ARCHB_WT_FAULT_CONSUMERS=1;APS5_WT_FAULT_GUEST_HOT=0;APS5_ARCHB_WT_FAULT_DIRTY_ARM=0")
    set_tests_properties(agc_archb_wt_fault_consumers_verify agc_archb_wt_fault_consumers_suite PROPERTIES ENVIRONMENT "APS5_ARCHB_WT_FAULT=1;APS5_ARCHB_WT_FAULT_VERIFY=1;APS5_ARCHB_WT_FAULT_CONSUMERS=1;APS5_WT_FAULT_GUEST_HOT=0;APS5_ARCHB_WT_FAULT_DIRTY_ARM=0")
    add_test(NAME agc_archb_wt_sync_skip COMMAND agc_write_tracking_tests --wt-fault-consumers)
    add_test(NAME agc_archb_wt_sync_skip_verify COMMAND agc_write_tracking_tests --wt-fault-consumers)
    add_test(NAME agc_archb_wt_sync_skip_suite COMMAND agc_write_tracking_tests)
    set_tests_properties(agc_archb_wt_sync_skip agc_archb_wt_sync_skip_verify agc_archb_wt_sync_skip_suite PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120 ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    set_tests_properties(agc_archb_wt_sync_skip agc_archb_wt_sync_skip_suite PROPERTIES ENVIRONMENT "APS5_ARCHB_WT_FAULT=1;APS5_ARCHB_WT_FAULT_VERIFY=0;APS5_ARCHB_WT_FAULT_CONSUMERS=1;APS5_WT_FAULT_GUEST_HOT=0;APS5_ARCHB_WT_FAULT_DIRTY_ARM=0;APS5_ARCHB_WT_SYNC_SKIP=1")
    set_tests_properties(agc_archb_wt_sync_skip_verify PROPERTIES ENVIRONMENT "APS5_ARCHB_WT_FAULT=1;APS5_ARCHB_WT_FAULT_VERIFY=0;APS5_ARCHB_WT_FAULT_CONSUMERS=1;APS5_WT_FAULT_GUEST_HOT=0;APS5_ARCHB_WT_FAULT_DIRTY_ARM=0;APS5_ARCHB_WT_SYNC_SKIP=1;APS5_ARCHB_WT_SYNC_SKIP_VERIFY=1")
    add_test(NAME agc_archc_switches COMMAND agc_write_tracking_tests --archc-switches)
    set_tests_properties(agc_archc_switches PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30 ENVIRONMENT "APS5_ARCHB_WT_FAULT=1;APS5_ARCHB_C_TARGET_FASTPATH=1"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    add_test(NAME agc_archc_log COMMAND agc_write_tracking_tests --archc-log)
    add_test(NAME agc_archc_epochs COMMAND agc_write_tracking_tests --archc-epochs)
    set_tests_properties(agc_archc_log agc_archc_epochs PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30
        ENVIRONMENT "APS5_ARCHB_WT_FAULT=1;APS5_ARCHB_WT_FAULT_VERIFY=0;APS5_ARCHB_WT_FAULT_CONSUMERS=1;APS5_ARCHB_WT_FAULT_DIRTY_ARM=0;APS5_ARCHB_C_IMAGE_EPOCHS=1"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    add_test(NAME agc_archc_lazy_watch COMMAND agc_write_tracking_tests --archc-watch)
    set_tests_properties(agc_archc_lazy_watch PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30
        ENVIRONMENT "APS5_ARCHB_WT_FAULT=1;APS5_ARCHB_WT_FAULT_VERIFY=0;APS5_ARCHB_WT_FAULT_CONSUMERS=1;APS5_ARCHB_WT_FAULT_DIRTY_ARM=0;APS5_WT_FAULT_GUEST_HOT=0;APS5_ARCHB_C_LAZY_CENSUS=1"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    add_test(NAME agc_archb_wt_fault_hot COMMAND agc_write_tracking_tests --wt-fault-hot)
    set_tests_properties(agc_archb_wt_fault_hot PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60
        ENVIRONMENT "APS5_ARCHB_WT_FAULT=1;APS5_ARCHB_WT_FAULT_VERIFY=1;APS5_ARCHB_WT_FAULT_CONSUMERS=0;APS5_ARCHB_WT_FAULT_DIRTY_ARM=0"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    add_test(NAME agc_archb_wt_fault_dirty_arm COMMAND agc_write_tracking_tests --wt-fault-dirty-arm)
    add_test(NAME agc_archb_wt_fault_dirty_arm_consumers COMMAND agc_write_tracking_tests --wt-fault-dirty-arm)
    add_test(NAME agc_archb_wt_fault_dirty_arm_suite COMMAND agc_write_tracking_tests)
    set_tests_properties(agc_archb_wt_fault_dirty_arm agc_archb_wt_fault_dirty_arm_consumers agc_archb_wt_fault_dirty_arm_suite PROPERTIES
        SKIP_RETURN_CODE 77 TIMEOUT 60 ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    set_tests_properties(agc_archb_wt_fault_dirty_arm PROPERTIES ENVIRONMENT "APS5_ARCHB_WT_FAULT=1;APS5_ARCHB_WT_FAULT_VERIFY=0;APS5_ARCHB_WT_FAULT_CONSUMERS=0;APS5_ARCHB_WT_FAULT_DIRTY_ARM=1")
    set_tests_properties(agc_archb_wt_fault_dirty_arm_consumers PROPERTIES ENVIRONMENT "APS5_ARCHB_WT_FAULT=1;APS5_ARCHB_WT_FAULT_VERIFY=1;APS5_ARCHB_WT_FAULT_CONSUMERS=1;APS5_ARCHB_WT_FAULT_DIRTY_ARM=1")
    set_tests_properties(agc_archb_wt_fault_dirty_arm_suite PROPERTIES ENVIRONMENT "APS5_ARCHB_WT_FAULT=1;APS5_ARCHB_WT_FAULT_VERIFY=1;APS5_ARCHB_WT_FAULT_CONSUMERS=1;APS5_ARCHB_WT_FAULT_DIRTY_ARM=1;APS5_WT_FAULT_GUEST_HOT=0")
    add_test(NAME agc_failed_collect_memo COMMAND agc_write_tracking_tests --failed-collect-memo)
    add_test(NAME agc_failed_collect_memo_off COMMAND agc_write_tracking_tests --failed-collect-memo)
    set_tests_properties(agc_failed_collect_memo agc_failed_collect_memo_off PROPERTIES
        SKIP_RETURN_CODE 77 TIMEOUT 30 ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    set_tests_properties(agc_failed_collect_memo_off PROPERTIES ENVIRONMENT "APS5_NO_FAILED_COLLECT_MEMO=1")
    add_test(NAME agc_archb_fix_stable_failed_collects_on COMMAND agc_write_tracking_tests --failed-collect-memo)
    add_test(NAME agc_archb_fix_stable_failed_collects_verify COMMAND agc_write_tracking_tests --failed-collect-memo)
    add_test(NAME agc_archb_fix_stable_failed_collects_suite COMMAND agc_write_tracking_tests)
    set_tests_properties(agc_archb_fix_stable_failed_collects_on agc_archb_fix_stable_failed_collects_verify agc_archb_fix_stable_failed_collects_suite PROPERTIES
        SKIP_RETURN_CODE 77 TIMEOUT 60 ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    set_tests_properties(agc_archb_fix_stable_failed_collects_on agc_archb_fix_stable_failed_collects_suite PROPERTIES ENVIRONMENT "APS5_ARCHB_FIX_STABLE_FAILED_COLLECTS=1")
    set_tests_properties(agc_archb_fix_stable_failed_collects_verify PROPERTIES ENVIRONMENT "APS5_ARCHB_FIX_STABLE_FAILED_COLLECTS=1;APS5_ARCHB_FIX_STABLE_FAILED_COLLECTS_VERIFY=1")
    add_test(NAME agc_write_tracking_alloc_churn COMMAND agc_write_tracking_tests)
    add_test(NAME agc_failed_collect_memo_alloc_churn COMMAND agc_write_tracking_tests --failed-collect-memo)
    add_test(NAME agc_archb_fix_alloc_churn_memo COMMAND agc_write_tracking_tests --alloc-churn-memo)
    set_tests_properties(agc_write_tracking_alloc_churn agc_failed_collect_memo_alloc_churn agc_archb_fix_alloc_churn_memo PROPERTIES
        SKIP_RETURN_CODE 77 TIMEOUT 60 ENVIRONMENT "APS5_ARCHB_FIX_ALLOC_CHURN=1" ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    if(WIN32)
        foreach(v1Case IN ITEMS reuse commit protect map unwatch forget coverage threads toggle)
            foreach(v1Switch IN ITEMS 1 0)
                set(v1Test agc_archb_fix_v1_${v1Case})
                if(NOT v1Switch)
                    string(APPEND v1Test "_off")
                endif()
                add_test(NAME ${v1Test} COMMAND agc_write_tracking_tests --vq-memo-${v1Case})
                set_tests_properties(${v1Test} PROPERTIES TIMEOUT 30
                    ENVIRONMENT "APS5_VERIFY_VQ_MEMO=1;APS5_ARCHB_FIX_VQ_MEMO=${v1Switch}"
                    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
            endforeach()
        endforeach()
        foreach(bookkeepingCase IN ITEMS epoch threads remap reset runs regions toggle)
            add_test(NAME agc_archb_fix_wt_bookkeeping_${bookkeepingCase} COMMAND agc_write_tracking_tests --wt-bookkeeping-${bookkeepingCase})
            add_test(NAME agc_archb_fix_wt_bookkeeping_${bookkeepingCase}_verify COMMAND agc_write_tracking_tests --wt-bookkeeping-${bookkeepingCase})
            set_tests_properties(agc_archb_fix_wt_bookkeeping_${bookkeepingCase} PROPERTIES TIMEOUT 60
                ENVIRONMENT "APS5_ARCHB_FIX_WRITE_TRACKING_BOOKKEEPING=1"
                ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
            set_tests_properties(agc_archb_fix_wt_bookkeeping_${bookkeepingCase}_verify PROPERTIES TIMEOUT 60
                ENVIRONMENT "APS5_ARCHB_FIX_WRITE_TRACKING_BOOKKEEPING=1;APS5_VERIFY_WRITE_TRACKING_BOOKKEEPING=1"
                ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
        endforeach()
        add_test(NAME agc_archb_page_state_memo COMMAND agc_write_tracking_tests --page-state-memo)
        set_tests_properties(agc_archb_page_state_memo PROPERTIES TIMEOUT 60
            ENVIRONMENT "APS5_ARCHB_PAGE_STATE_MEMO=1"
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
        foreach(bookkeepingCase IN ITEMS runs remap regions)
            add_test(NAME agc_archb_page_state_memo_bookkeeping_${bookkeepingCase} COMMAND agc_write_tracking_tests --wt-bookkeeping-${bookkeepingCase})
            set_tests_properties(agc_archb_page_state_memo_bookkeeping_${bookkeepingCase} PROPERTIES TIMEOUT 60
                ENVIRONMENT "APS5_ARCHB_FIX_WRITE_TRACKING_BOOKKEEPING=1;APS5_VERIFY_WRITE_TRACKING_BOOKKEEPING=1;APS5_ARCHB_PAGE_STATE_MEMO=1"
                ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
        endforeach()
        foreach(shareMode IN ITEMS on verify off)
            add_test(NAME agc_archb_pipe_collect_share_${shareMode} COMMAND agc_write_tracking_tests --collect-share-${shareMode})
            set(shareValue 1)
            if(shareMode STREQUAL "off")
                set(shareValue 0)
            endif()
            set(shareVerify 0)
            if(shareMode STREQUAL "verify")
                set(shareVerify 1)
            endif()
            set_tests_properties(agc_archb_pipe_collect_share_${shareMode} PROPERTIES TIMEOUT 30
                ENVIRONMENT "APS5_ARCHB_PIPE_Q0_COMMITTER=1;APS5_ARCHB_PIPE_COLLECT_SHARE=${shareValue};APS5_ARCHB_PIPE_COLLECT_SHARE_VERIFY=${shareVerify}"
                ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
        endforeach()
        add_test(NAME agc_write_tracking_wt_bookkeeping COMMAND agc_write_tracking_tests)
        add_test(NAME agc_failed_collect_memo_wt_bookkeeping COMMAND agc_write_tracking_tests --failed-collect-memo)
        add_test(NAME agc_target_clean_proof_wt_bookkeeping COMMAND agc_write_tracking_tests --target-clean-proof)
        set_tests_properties(agc_write_tracking_wt_bookkeeping agc_failed_collect_memo_wt_bookkeeping agc_target_clean_proof_wt_bookkeeping PROPERTIES
            SKIP_RETURN_CODE 77 TIMEOUT 60 ENVIRONMENT "APS5_ARCHB_FIX_WRITE_TRACKING_BOOKKEEPING=1;APS5_TARGET_CLEAN_PROOF=1"
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
        add_test(NAME agc_target_clean_proof COMMAND agc_write_tracking_tests --target-clean-proof)
        set_tests_properties(agc_target_clean_proof PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30 ENVIRONMENT "APS5_TARGET_CLEAN_PROOF=1")
        set_tests_properties(agc_target_clean_proof PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
        add_test(NAME agc_resident_direct_remap COMMAND agc_write_tracking_tests --resident-direct-remap)
        set_tests_properties(agc_resident_direct_remap PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 20)
        add_test(NAME agc_rewatched_direct_shared COMMAND agc_write_tracking_tests --rewatched-direct-shared)
        set_tests_properties(agc_rewatched_direct_shared PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 20)
        add_test(NAME agc_import_alias_shared COMMAND agc_write_tracking_tests --import-alias-shared)
        set_tests_properties(agc_import_alias_shared PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 20)
        add_test(NAME agc_compared_shared_generations COMMAND agc_write_tracking_tests --compared-shared-generations)
        set_tests_properties(agc_compared_shared_generations PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 20)
        add_test(NAME agc_archb_fix_stable_failed_collects_compared COMMAND agc_write_tracking_tests --compared-shared-generations)
        set_tests_properties(agc_archb_fix_stable_failed_collects_compared PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 20
            ENVIRONMENT "APS5_ARCHB_FIX_STABLE_FAILED_COLLECTS=1"
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
        add_test(NAME agc_write_tracking_rewatch_import_pages COMMAND agc_write_tracking_tests --rewatch-import-pages)
        set_tests_properties(agc_write_tracking_rewatch_import_pages PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 20)
        add_test(NAME agc_write_tracking_fresh_view COMMAND agc_write_tracking_tests --fresh-view)
        set_tests_properties(agc_write_tracking_fresh_view PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 20)
        add_test(NAME agc_write_tracking_own_store_images COMMAND agc_write_tracking_tests --own-store-images)
        set_tests_properties(agc_write_tracking_own_store_images PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 20)
        set_tests_properties(agc_write_tracking agc_write_tracking_generation_wrap agc_write_tracking_metadata_commit agc_write_tracking_cpu_write_pin agc_resident_direct_remap agc_rewatched_direct_shared agc_import_alias_shared agc_compared_shared_generations agc_write_tracking_rewatch_import_pages agc_write_tracking_fresh_view agc_write_tracking_own_store_images PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

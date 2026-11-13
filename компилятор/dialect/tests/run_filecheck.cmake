execute_process(
    COMMAND "${SPLAV_OPT}" "${INPUT}"
    COMMAND "${FILECHECK}" "${INPUT}"
    RESULTS_VARIABLE results
)
foreach(result IN LISTS results)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "splav-opt | FileCheck: коды завершения ${results}")
    endif()
endforeach()

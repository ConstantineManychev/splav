splav_add_executable(splav_tests
    main.cpp
    support_tests.cpp
    test.cpp
    unicode_tests.cpp
)
target_link_libraries(splav_tests PRIVATE splav_support splav_unicode)

add_test(NAME юнит COMMAND splav_tests "--корень=${PROJECT_SOURCE_DIR}")

string(REPLACE "." "\\." SPLAV_VERSION_PATTERN "${SPLAV_VERSION}")
add_test(NAME сплав-версия COMMAND splav версия)
set_tests_properties(сплав-версия PROPERTIES PASS_REGULAR_EXPRESSION "^сплав ${SPLAV_VERSION_PATTERN}")
add_test(NAME сплав-version COMMAND splav version)
set_tests_properties(сплав-version PROPERTIES PASS_REGULAR_EXPRESSION "^сплав ${SPLAV_VERSION_PATTERN}")

find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    set(SPLAV_PYTHON_CHECKS
        "проверка-грамматики|грамматика/проверка.py"
        "проверка-злых|грамматика/проверка_злых.py"
        "проверка-сочетаний|модель/проверка_сочетаний.py"
        "проверка-корпуса|модель/проверка_корпуса.py"
    )
    foreach(check IN LISTS SPLAV_PYTHON_CHECKS)
        string(REPLACE "|" ";" parts "${check}")
        list(GET parts 0 name)
        list(GET parts 1 script)
        add_test(NAME ${name} COMMAND Python3::Interpreter ${script} WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
        set_tests_properties(${name} PROPERTIES ENVIRONMENT "PYTHONUTF8=1" LABELS "python")
    endforeach()
endif()

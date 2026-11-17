add_library(splav_interp STATIC
    builtins.cpp
    machine.cpp
    value.cpp
)
target_link_libraries(splav_interp PUBLIC splav_sema splav_support splav_unicode)

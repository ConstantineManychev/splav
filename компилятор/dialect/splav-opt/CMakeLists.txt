add_llvm_executable(splav-opt splav_opt.cpp)
llvm_update_compile_flags(splav-opt)
target_link_libraries(splav-opt PRIVATE splav_dialect MLIRFuncDialect MLIROptLib)

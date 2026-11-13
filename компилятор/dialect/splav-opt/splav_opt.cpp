#include "dialect/splav_dialect.hpp"

#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/DialectRegistry.h"
#include "mlir/Tools/mlir-opt/MlirOptMain.h"

int main(int argc, char** argv)
{
    mlir::DialectRegistry registry;
    registry.insert<splav::dialect::SplavDialect, mlir::func::FuncDialect>();
    return mlir::asMainReturnCode(mlir::MlirOptMain(argc, argv, "Проходы над диалектом splav\n", registry));
}

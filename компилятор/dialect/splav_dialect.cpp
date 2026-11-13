#include "dialect/splav_dialect.hpp"

#include "dialect/splav_ops.hpp"

#include "dialect/splav_dialect.cpp.inc"

namespace splav::dialect
{

void SplavDialect::initialize()
{
    addOperations<
#define GET_OP_LIST
#include "dialect/splav_ops.cpp.inc"
        >();
}

}

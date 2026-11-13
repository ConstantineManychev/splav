#ifndef SPLAV_OPS_TD
#define SPLAV_OPS_TD

include "mlir/IR/OpBase.td"
include "mlir/Interfaces/InferTypeOpInterface.td"

def SplavDialect : Dialect
{
    let name = "splav";
    let summary = "Диалект языка Сплав";
    let cppNamespace = "::splav::dialect";
}

class SplavOp<string mnemonic, list<Trait> traits = []> : Op<SplavDialect, mnemonic, traits>;

def AddOp : SplavOp<"add", [SameOperandsAndResultType]>
{
    let summary = "Сложение целых с ловушкой при переполнении";
    let arguments = (ins AnySignlessInteger:$left, AnySignlessInteger:$right);
    let results = (outs AnySignlessInteger:$sum);
    let assemblyFormat = "$left `,` $right attr-dict `:` type($sum)";
}

#endif

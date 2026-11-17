#pragma once

#include "sema/declarations.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace splav
{

struct Value;
using Cell = std::shared_ptr<Value>;

enum class ValueKind : uint8_t
{
    Void,
    Bool,
    Integer,
    Float,
    Char,
    Str,
    Bytes,
    Struct,
    Enum,
    Optional,
    Tuple,
    List,
    Reference,
    Range,
    Vector,
    Builder,
    Graphemes,
    Chars,
    Enumeration,
    Function,
    Closure,
    Type,
    Module,
};

enum class Number : uint8_t
{
    Untyped,
    I8,
    I16,
    I32,
    I64,
    Offset,
    U8,
    U16,
    U32,
    U64,
    Size,
    F32,
    F64,
};

struct WideInteger
{
    bool negative = false;
    uint64_t magnitude = 0;
};

std::optional<Number> numberNamed(std::string_view name);
std::string_view numberName(Number number);
bool isFloatNumber(Number number);
unsigned numberBits(Number number);
bool numberSigned(Number number);

WideInteger signedInteger(int64_t value);
WideInteger unsignedInteger(uint64_t value);
bool fits(const WideInteger& value, Number number);
WideInteger integerFromBits(uint64_t bits, Number number);
uint64_t integerBits(const WideInteger& value, Number number);
int compareIntegers(const WideInteger& left, const WideInteger& right);
std::optional<WideInteger> addIntegers(WideInteger left, WideInteger right);
std::optional<WideInteger> subtractIntegers(WideInteger left, WideInteger right);
std::optional<WideInteger> multiplyIntegers(WideInteger left, WideInteger right);
std::optional<WideInteger> divideIntegers(WideInteger left, WideInteger right);
std::optional<WideInteger> remainderIntegers(WideInteger left, WideInteger right);
std::optional<int64_t> toSigned(const WideInteger& value);
std::optional<uint64_t> toUnsigned(const WideInteger& value);
double integerToDouble(const WideInteger& value);
std::string integerText(const WideInteger& value);
std::string floatText(double value, Number number);

struct Closure
{
    SyntaxNodeView node;
    Module* module = nullptr;
    const FunctionInfo* enclosing = nullptr;
    std::vector<std::pair<std::string, Cell>> captures;
};

struct Value
{
    ValueKind kind = ValueKind::Void;
    Number number = Number::Untyped;
    bool truth = false;
    bool inclusive = false;
    bool growable = false;
    WideInteger integer;
    double floating = 0;
    char32_t character = 0;
    size_t start = 0;
    size_t length = 0;
    uint32_t variant = 0;
    const TypeDecl* decl = nullptr;
    const FunctionInfo* function = nullptr;
    Module* module = nullptr;
    std::string name;
    std::shared_ptr<const std::string> text;
    std::shared_ptr<std::vector<Cell>> storage;
    std::shared_ptr<std::string> builder;
    std::vector<Cell> items;
    Cell target;
    std::shared_ptr<const Closure> closure;
};

Cell makeCell(Value value);
Cell voidCell();
Cell boolCell(bool value);
Cell integerCell(WideInteger value, Number number);
Cell floatCell(double value, Number number);
Cell charCell(char32_t value);
Cell stringCell(std::string text);
Cell viewCell(ValueKind kind, std::shared_ptr<const std::string> text, size_t start, size_t length);
Cell referenceCell(Cell target);
Cell optionalCell(Cell payload);
Cell tupleCell(std::vector<Cell> items);
Cell listCell(std::vector<Cell> items, bool growable);
Cell copyCell(const Cell& cell);
const Cell& dereference(const Cell& cell);
std::string_view textView(const Value& value);
size_t listLength(const Value& value);
const Cell& listElement(const Value& value, size_t index);

}

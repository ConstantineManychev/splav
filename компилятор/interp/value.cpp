#include "interp/value.hpp"

#include <array>
#include <charconv>
#include <cmath>

namespace splav
{

namespace
{

struct NumberInfo
{
    Number number;
    std::string_view name;
    unsigned bits;
    bool isSigned;
};

constexpr std::array NUMBERS = {
    NumberInfo{Number::I8, "ц8", 8, true},
    NumberInfo{Number::I16, "ц16", 16, true},
    NumberInfo{Number::I32, "ц32", 32, true},
    NumberInfo{Number::I64, "ц64", 64, true},
    NumberInfo{Number::Offset, "смещ", 64, true},
    NumberInfo{Number::U8, "н8", 8, false},
    NumberInfo{Number::U16, "н16", 16, false},
    NumberInfo{Number::U32, "н32", 32, false},
    NumberInfo{Number::U64, "н64", 64, false},
    NumberInfo{Number::Size, "разм", 64, false},
    NumberInfo{Number::F32, "в32", 32, true},
    NumberInfo{Number::F64, "в64", 64, true},
};

constexpr uint64_t SIGNED_LIMIT = uint64_t{1} << 63;

const NumberInfo* infoOf(Number number)
{
    for (const NumberInfo& info : NUMBERS)
    {
        if (info.number == number)
        {
            return &info;
        }
    }
    return nullptr;
}

uint64_t maskOf(unsigned bits)
{
    return bits >= 64 ? UINT64_MAX : (uint64_t{1} << bits) - 1;
}

WideInteger normalized(WideInteger value)
{
    if (value.magnitude == 0)
    {
        value.negative = false;
    }
    return value;
}

WideInteger negated(WideInteger value)
{
    value.negative = !value.negative;
    return normalized(value);
}

}

std::optional<Number> numberNamed(std::string_view name)
{
    for (const NumberInfo& info : NUMBERS)
    {
        if (info.name == name)
        {
            return info.number;
        }
    }
    return std::nullopt;
}

std::string_view numberName(Number number)
{
    const NumberInfo* info = infoOf(number);
    return info != nullptr ? info->name : std::string_view("ц64");
}

bool isFloatNumber(Number number)
{
    return number == Number::F32 || number == Number::F64;
}

unsigned numberBits(Number number)
{
    const NumberInfo* info = infoOf(number);
    return info != nullptr ? info->bits : 64;
}

bool numberSigned(Number number)
{
    const NumberInfo* info = infoOf(number);
    return info == nullptr || info->isSigned;
}

WideInteger signedInteger(int64_t value)
{
    if (value >= 0)
    {
        return WideInteger{false, static_cast<uint64_t>(value)};
    }
    return WideInteger{true, ~static_cast<uint64_t>(value) + 1};
}

WideInteger unsignedInteger(uint64_t value)
{
    return WideInteger{false, value};
}

bool fits(const WideInteger& value, Number number)
{
    if (number == Number::Untyped)
    {
        return !value.negative || value.magnitude <= SIGNED_LIMIT;
    }
    const unsigned bits = numberBits(number);
    if (numberSigned(number))
    {
        const uint64_t limit = uint64_t{1} << (bits - 1);
        return value.negative ? value.magnitude <= limit : value.magnitude < limit;
    }
    return !value.negative && value.magnitude <= maskOf(bits);
}

WideInteger integerFromBits(uint64_t bits, Number number)
{
    const unsigned width = numberBits(number);
    const uint64_t mask = maskOf(width);
    bits &= mask;
    if (numberSigned(number) && (bits >> (width - 1)) != 0)
    {
        return WideInteger{true, (~bits + 1) & mask};
    }
    return WideInteger{false, bits};
}

uint64_t integerBits(const WideInteger& value, Number number)
{
    const uint64_t mask = maskOf(numberBits(number));
    return (value.negative ? ~value.magnitude + 1 : value.magnitude) & mask;
}

int compareIntegers(const WideInteger& left, const WideInteger& right)
{
    if (left.negative != right.negative)
    {
        return left.negative ? -1 : 1;
    }
    if (left.magnitude == right.magnitude)
    {
        return 0;
    }
    const bool smaller = left.magnitude < right.magnitude;
    return (smaller != left.negative) ? -1 : 1;
}

std::optional<WideInteger> addIntegers(WideInteger left, WideInteger right)
{
    if (left.negative == right.negative)
    {
        const uint64_t sum = left.magnitude + right.magnitude;
        if (sum < left.magnitude)
        {
            return std::nullopt;
        }
        return normalized(WideInteger{left.negative, sum});
    }
    if (left.magnitude >= right.magnitude)
    {
        return normalized(WideInteger{left.negative, left.magnitude - right.magnitude});
    }
    return normalized(WideInteger{right.negative, right.magnitude - left.magnitude});
}

std::optional<WideInteger> subtractIntegers(WideInteger left, WideInteger right)
{
    return addIntegers(left, negated(right));
}

std::optional<WideInteger> multiplyIntegers(WideInteger left, WideInteger right)
{
    if (left.magnitude != 0 && right.magnitude > UINT64_MAX / left.magnitude)
    {
        return std::nullopt;
    }
    return normalized(WideInteger{left.negative != right.negative, left.magnitude * right.magnitude});
}

std::optional<WideInteger> divideIntegers(WideInteger left, WideInteger right)
{
    if (right.magnitude == 0)
    {
        return std::nullopt;
    }
    return normalized(WideInteger{left.negative != right.negative, left.magnitude / right.magnitude});
}

std::optional<WideInteger> remainderIntegers(WideInteger left, WideInteger right)
{
    if (right.magnitude == 0)
    {
        return std::nullopt;
    }
    return normalized(WideInteger{left.negative, left.magnitude % right.magnitude});
}

std::optional<int64_t> toSigned(const WideInteger& value)
{
    if (!fits(value, Number::I64))
    {
        return std::nullopt;
    }
    return static_cast<int64_t>(integerBits(value, Number::I64));
}

std::optional<uint64_t> toUnsigned(const WideInteger& value)
{
    if (value.negative)
    {
        return std::nullopt;
    }
    return value.magnitude;
}

double integerToDouble(const WideInteger& value)
{
    const double magnitude = static_cast<double>(value.magnitude);
    return value.negative ? -magnitude : magnitude;
}

std::string integerText(const WideInteger& value)
{
    std::string digits = std::to_string(value.magnitude);
    return value.negative ? "-" + digits : digits;
}

std::string floatText(double value, Number number)
{
    if (std::isnan(value))
    {
        return "NaN";
    }
    if (std::isinf(value))
    {
        return value < 0 ? "-inf" : "inf";
    }
    std::array<char, 64> buffer{};
    const std::to_chars_result result = number == Number::F32
        ? std::to_chars(buffer.data(), buffer.data() + buffer.size(), static_cast<float>(value))
        : std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    return std::string(buffer.data(), result.ptr);
}

Cell makeCell(Value value)
{
    return std::make_shared<Value>(std::move(value));
}

Cell voidCell()
{
    return std::make_shared<Value>();
}

Cell boolCell(bool value)
{
    auto cell = std::make_shared<Value>();
    cell->kind = ValueKind::Bool;
    cell->truth = value;
    return cell;
}

Cell integerCell(WideInteger value, Number number)
{
    auto cell = std::make_shared<Value>();
    cell->kind = ValueKind::Integer;
    cell->number = number;
    cell->integer = value;
    return cell;
}

Cell floatCell(double value, Number number)
{
    auto cell = std::make_shared<Value>();
    cell->kind = ValueKind::Float;
    cell->number = number;
    cell->floating = number == Number::F32 ? static_cast<double>(static_cast<float>(value)) : value;
    return cell;
}

Cell charCell(char32_t value)
{
    auto cell = std::make_shared<Value>();
    cell->kind = ValueKind::Char;
    cell->character = value;
    return cell;
}

Cell stringCell(std::string text)
{
    auto shared = std::make_shared<const std::string>(std::move(text));
    const size_t length = shared->size();
    return viewCell(ValueKind::Str, std::move(shared), 0, length);
}

Cell viewCell(ValueKind kind, std::shared_ptr<const std::string> text, size_t start, size_t length)
{
    auto cell = std::make_shared<Value>();
    cell->kind = kind;
    cell->text = std::move(text);
    cell->start = start;
    cell->length = length;
    return cell;
}

Cell referenceCell(Cell target)
{
    auto cell = std::make_shared<Value>();
    cell->kind = ValueKind::Reference;
    cell->target = std::move(target);
    return cell;
}

Cell optionalCell(Cell payload)
{
    auto cell = std::make_shared<Value>();
    cell->kind = ValueKind::Optional;
    if (payload)
    {
        cell->items.push_back(std::move(payload));
    }
    return cell;
}

Cell tupleCell(std::vector<Cell> items)
{
    auto cell = std::make_shared<Value>();
    cell->kind = ValueKind::Tuple;
    cell->items = std::move(items);
    return cell;
}

Cell listCell(std::vector<Cell> items, bool growable)
{
    auto cell = std::make_shared<Value>();
    cell->kind = ValueKind::List;
    cell->length = items.size();
    cell->storage = std::make_shared<std::vector<Cell>>(std::move(items));
    cell->growable = growable;
    return cell;
}

Cell copyCell(const Cell& cell)
{
    auto copy = std::make_shared<Value>(*cell);
    switch (copy->kind)
    {
    case ValueKind::Struct:
    case ValueKind::Enum:
    case ValueKind::Optional:
    case ValueKind::Tuple:
    case ValueKind::Range:
    case ValueKind::Vector:
        for (Cell& item : copy->items)
        {
            if (item)
            {
                item = copyCell(item);
            }
        }
        break;
    default:
        break;
    }
    return copy;
}

const Cell& dereference(const Cell& cell)
{
    const Cell* current = &cell;
    while ((*current)->kind == ValueKind::Reference)
    {
        current = &(*current)->target;
    }
    return *current;
}

std::string_view textView(const Value& value)
{
    if (!value.text)
    {
        return {};
    }
    return std::string_view(*value.text).substr(value.start, value.length);
}

size_t listLength(const Value& value)
{
    return value.growable ? value.storage->size() : value.length;
}

const Cell& listElement(const Value& value, size_t index)
{
    return (*value.storage)[value.start + index];
}

}

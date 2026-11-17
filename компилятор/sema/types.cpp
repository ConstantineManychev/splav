#include "sema/types.hpp"

#include <algorithm>
#include <array>

namespace splav
{

namespace
{

constexpr std::array INTEGER_TYPES = {
    "ц8", "ц16", "ц32", "ц64", "ц128", "н8", "н16", "н32", "н64", "н128", "разм", "смещ"};
constexpr std::array FLOAT_TYPES = {"в16", "в32", "в64"};

std::string lastSegment(const std::string& name)
{
    const size_t dot = name.rfind('.');
    return dot == std::string::npos ? name : name.substr(dot + 1);
}

std::string listText(const std::vector<TypePtr>& types, const Module* context, const Module* prelude)
{
    std::string text;
    for (const TypePtr& type : types)
    {
        text += (text.empty() ? "" : ", ") + typeText(type, context, prelude);
    }
    return text;
}

}

TypePtr makeType(TypeKind kind)
{
    auto type = std::make_shared<Type>();
    type->kind = kind;
    return type;
}

TypePtr unknownType()
{
    static const TypePtr unknown = makeType(TypeKind::Unknown);
    return unknown;
}

TypePtr builtinType(const std::string& name)
{
    auto type = std::make_shared<Type>();
    type->builtin = name;
    if (std::find(INTEGER_TYPES.begin(), INTEGER_TYPES.end(), name) != INTEGER_TYPES.end())
    {
        type->kind = TypeKind::Integer;
    }
    else if (std::find(FLOAT_TYPES.begin(), FLOAT_TYPES.end(), name) != FLOAT_TYPES.end())
    {
        type->kind = TypeKind::Float;
    }
    else if (name == "бул")
    {
        type->kind = TypeKind::Bool;
    }
    else if (name == "сим")
    {
        type->kind = TypeKind::Char;
    }
    else if (name == "стр")
    {
        type->kind = TypeKind::Str;
    }
    else if (name == "пусто")
    {
        type->kind = TypeKind::Void;
    }
    else if (name == "никогда")
    {
        type->kind = TypeKind::Never;
    }
    else if (name == "компил_цел")
    {
        type->kind = TypeKind::LiteralInteger;
    }
    else if (name == "компил_вещ")
    {
        type->kind = TypeKind::LiteralFloat;
    }
    else
    {
        return unknownType();
    }
    return type;
}

TypePtr referenceTo(TypePtr inner, bool mutableAccess)
{
    auto type = std::make_shared<Type>();
    type->kind = TypeKind::Reference;
    type->mutableAccess = mutableAccess;
    type->inner = std::move(inner);
    return type;
}

TypePtr optionalOf(TypePtr inner)
{
    auto type = std::make_shared<Type>();
    type->kind = TypeKind::Optional;
    type->inner = std::move(inner);
    return type;
}

TypePtr namedType(const TypeDecl* decl, std::vector<TypePtr> arguments)
{
    auto type = std::make_shared<Type>();
    type->kind = TypeKind::Named;
    type->decl = decl;
    type->arguments = std::move(arguments);
    return type;
}

bool isKnown(const TypePtr& type)
{
    if (!type || type->kind == TypeKind::Unknown || type->kind == TypeKind::Parameter || type->kind == TypeKind::Meta)
    {
        return false;
    }
    if (type->inner && !isKnown(type->inner))
    {
        return false;
    }
    return std::all_of(type->arguments.begin(), type->arguments.end(), isKnown);
}

bool isNumeric(const TypePtr& type)
{
    return type && (type->kind == TypeKind::Integer || type->kind == TypeKind::Float);
}

bool sameType(const TypePtr& left, const TypePtr& right)
{
    if (!left || !right || left->kind != right->kind || left->builtin != right->builtin || left->decl != right->decl
        || left->mutableAccess != right->mutableAccess || left->arguments.size() != right->arguments.size())
    {
        return false;
    }
    if ((left->inner == nullptr) != (right->inner == nullptr) || (left->inner && !sameType(left->inner, right->inner)))
    {
        return false;
    }
    for (size_t index = 0; index < left->arguments.size(); ++index)
    {
        if (!sameType(left->arguments[index], right->arguments[index]))
        {
            return false;
        }
    }
    return true;
}

TypePtr stripReferences(TypePtr type)
{
    while (type && type->kind == TypeKind::Reference)
    {
        type = type->inner;
    }
    return type;
}

std::string typeText(const TypePtr& type, const Module* context, const Module* prelude)
{
    if (!type)
    {
        return "?";
    }
    switch (type->kind)
    {
    case TypeKind::Unknown:
        return "_";
    case TypeKind::Never:
        return "никогда";
    case TypeKind::Void:
        return "пусто";
    case TypeKind::LiteralInteger:
        return "компил_цел";
    case TypeKind::LiteralFloat:
        return "компил_вещ";
    case TypeKind::Bool:
    case TypeKind::Char:
    case TypeKind::Str:
    case TypeKind::Integer:
    case TypeKind::Float:
    case TypeKind::Parameter:
        return type->builtin;
    case TypeKind::Meta:
        return "тип";
    case TypeKind::Reference:
        return std::string(type->mutableAccess ? "&изм " : "&") + typeText(type->inner, context, prelude);
    case TypeKind::Optional:
        return "?" + typeText(type->inner, context, prelude);
    case TypeKind::Slice:
        return std::string(type->mutableAccess ? "[]изм " : "[]") + typeText(type->inner, context, prelude);
    case TypeKind::Array:
        return "[" + (type->builtin.empty() ? std::string("_") : type->builtin) + "]"
            + typeText(type->inner, context, prelude);
    case TypeKind::Pointer:
        return "*" + typeText(type->inner, context, prelude);
    case TypeKind::Tuple:
        return "(" + listText(type->arguments, context, prelude) + ")";
    case TypeKind::Vector:
        return "Вектор(" + typeText(type->inner, context, prelude) + ", " + type->builtin + ")";
    case TypeKind::Function:
        return "функ(" + listText(type->arguments, context, prelude) + ") -> "
            + typeText(type->inner, context, prelude);
    case TypeKind::Named:
    {
        std::string text = type->decl->display;
        if (type->decl->module != context && type->decl->module != prelude)
        {
            text = lastSegment(type->decl->module->name) + "." + text;
        }
        return type->arguments.empty() ? text : text + "(" + listText(type->arguments, context, prelude) + ")";
    }
    }
    return "?";
}

std::optional<IntegerRange> integerRange(const std::string& builtin)
{
    struct Entry
    {
        std::string_view name;
        int bits;
        bool isSigned;
    };
    constexpr std::array ENTRIES = {
        Entry{"ц8", 8, true},
        Entry{"ц16", 16, true},
        Entry{"ц32", 32, true},
        Entry{"ц64", 64, true},
        Entry{"ц128", 128, true},
        Entry{"н8", 8, false},
        Entry{"н16", 16, false},
        Entry{"н32", 32, false},
        Entry{"н64", 64, false},
        Entry{"н128", 128, false},
        Entry{"разм", 64, false},
        Entry{"смещ", 64, true},
    };
    for (const Entry& entry : ENTRIES)
    {
        if (entry.name != builtin)
        {
            continue;
        }
        IntegerRange range;
        if (entry.bits > 64)
        {
            range.wide = true;
            return range;
        }
        const int magnitudeBits = entry.isSigned ? entry.bits - 1 : entry.bits;
        range.maximum = magnitudeBits == 64 ? UINT64_MAX : (uint64_t{1} << magnitudeBits) - 1;
        range.negativeLimit = entry.isSigned ? (uint64_t{1} << magnitudeBits) : 0;
        range.minimumText = entry.isSigned ? "-" + std::to_string(range.negativeLimit) : "0";
        range.maximumText = std::to_string(range.maximum);
        return range;
    }
    return std::nullopt;
}

}

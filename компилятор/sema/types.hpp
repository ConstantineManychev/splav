#pragma once

#include "sema/program.hpp"
#include "sema/syntax_view.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace splav
{

struct TypeDecl;
struct Type;
using TypePtr = std::shared_ptr<const Type>;

enum class TypeKind
{
    Unknown,
    Never,
    Void,
    Bool,
    Char,
    Str,
    Integer,
    Float,
    LiteralInteger,
    LiteralFloat,
    Meta,
    Reference,
    Optional,
    Slice,
    Array,
    Pointer,
    Tuple,
    Named,
    Parameter,
    Function,
    Vector,
};

struct Type
{
    TypeKind kind = TypeKind::Unknown;
    std::string builtin;
    bool mutableAccess = false;
    const TypeDecl* decl = nullptr;
    std::vector<TypePtr> arguments;
    TypePtr inner;
};

struct FieldDecl
{
    std::string key;
    std::string display;
    SyntaxNodeView type;
    bool open = false;
    uint32_t offset = 0;
    uint32_t length = 0;
};

struct VariantDecl
{
    std::string key;
    std::string display;
    std::vector<SyntaxNodeView> payload;
};

enum class DeclKind
{
    Struct,
    Enum,
    Trait,
};

struct TypeDecl
{
    DeclKind kind = DeclKind::Struct;
    std::string key;
    std::string display;
    Module* module = nullptr;
    SyntaxNodeView node;
    std::vector<std::string> parameters;
    std::vector<std::string> parameterDisplays;
    std::vector<SyntaxNodeView> bounds;
    std::vector<FieldDecl> fields;
    std::vector<VariantDecl> variants;
    uint32_t nameOffset = 0;
    uint32_t nameLength = 0;
};

TypePtr makeType(TypeKind kind);
TypePtr unknownType();
TypePtr builtinType(const std::string& name);
TypePtr referenceTo(TypePtr inner, bool mutableAccess);
TypePtr optionalOf(TypePtr inner);
TypePtr namedType(const TypeDecl* decl, std::vector<TypePtr> arguments);
bool isKnown(const TypePtr& type);
bool isNumeric(const TypePtr& type);
bool sameType(const TypePtr& left, const TypePtr& right);
TypePtr stripReferences(TypePtr type);
std::string typeText(const TypePtr& type, const Module* context, const Module* prelude);
struct IntegerRange
{
    uint64_t maximum = 0;
    uint64_t negativeLimit = 0;
    bool wide = false;
    std::string minimumText;
    std::string maximumText;
};

std::optional<IntegerRange> integerRange(const std::string& builtin);

}

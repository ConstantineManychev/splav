#pragma once

#include "syntax/green.hpp"

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace splav::memory
{

enum class Kind : uint8_t
{
    Module,
    Import,
    Include,
    Impl,
    Const,
    Struct,
    Set,
    Enum,
    Trait,
    Function,
    Field,
    Variant,
    Signature,
    Parameter,
    Attribute,
    Block,
    Binding,
    Assign,
    Expression,
    Discard,
    Return,
    Break,
    Continue,
    If,
    Loop,
    Region,
    Through,
    TogetherBlock,
    UnsafeBlock,
    Defer,
    Spawn,
    Literal,
    Paren,
    Name,
    Member,
    Index,
    Call,
    Reference,
    Prefix,
    Binary,
    Comparison,
    Membership,
    Logical,
    Not,
    Is,
    Cast,
    Range,
    CompoundLiteral,
    Tuple,
    Match,
    Arm,
    Closure,
    Together,
    Placeholder,
    Asm,
    OptionalType,
    DynType,
    RawPointer,
    SliceType,
    SentinelSlice,
    SentinelPointer,
    ArrayType,
    FunctionType,
    StructType,
    SetType,
    EnumType,
    AnyPattern,
    LiteralPattern,
    OrPattern,
    MutPattern,
    TuplePattern,
    NamePattern,
};

enum class LiteralKind : uint8_t
{
    Integer,
    Float,
    Char,
    Byte,
    String,
    Bool,
};

struct Token
{
    uint32_t offset = 0;
    uint32_t end = 0;
    std::string text;
};

struct Node;

struct Labeled
{
    std::optional<std::string> label;
    const Node* value = nullptr;
    std::optional<Token> token;
};

struct Node
{
    Kind kind = Kind::Module;
    uint32_t first = 0;
    uint32_t last = 0;
    uint32_t lastStart = 0;
    uint32_t line = 0;
    uint32_t column = 0;
    std::optional<std::string> name;
    std::optional<Token> nameToken;
    std::string op;
    std::optional<std::string> label;
    std::optional<std::string> alias;
    std::optional<std::string> owner;
    std::optional<std::string> requirement;
    std::string text;
    LiteralKind literal = LiteralKind::Integer;
    bool mutableAccess = false;
    bool comptime = false;
    bool negated = false;
    bool inclusive = false;
    bool open = false;
    bool hasArguments = false;
    bool hasFields = false;
    const Node* value = nullptr;
    const Node* type = nullptr;
    const Node* left = nullptr;
    const Node* right = nullptr;
    const Node* object = nullptr;
    const Node* function = nullptr;
    const Node* operand = nullptr;
    const Node* condition = nullptr;
    const Node* then = nullptr;
    const Node* otherwise = nullptr;
    const Node* source = nullptr;
    const Node* pattern = nullptr;
    const Node* body = nullptr;
    const Node* guard = nullptr;
    const Node* from = nullptr;
    const Node* to = nullptr;
    const Node* index = nullptr;
    const Node* result = nullptr;
    const Node* length = nullptr;
    const Node* device = nullptr;
    const Node* signature = nullptr;
    const Node* trait = nullptr;
    std::vector<const Node*> items;
    std::vector<const Node*> attributes;
    std::vector<const Node*> ownerParameters;
    std::vector<Labeled> labeled;
    std::vector<Labeled> named;
    std::vector<std::string> path;

    Token lastToken(std::string_view sourceText) const;
};

class Tree
{
public:
    Tree(std::string_view source, const GreenNode& root);

    const Node* root() const;
    std::string_view source() const;
    std::string text(const Node& node) const;
    std::string firstLine(const Node& node) const;
    std::pair<uint32_t, uint32_t> position(uint32_t offset) const;

private:
    friend class Lowering;

    Node& make(Kind kind);

    std::string_view sourceText;
    std::vector<uint32_t> lineStarts;
    std::deque<Node> nodes;
    const Node* top = nullptr;
};

std::string kindText(Kind kind);
std::string dumpTree(const Tree& tree);

}

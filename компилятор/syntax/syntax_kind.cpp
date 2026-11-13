#include "syntax/syntax_kind.hpp"

#include <array>
#include <cstddef>

namespace splav
{

namespace
{

constexpr size_t KIND_COUNT = static_cast<size_t>(SyntaxKind::Count);

constexpr std::array<std::string_view, KIND_COUNT> KIND_NAMES = {
    "Whitespace",
    "Newline",
    "LineComment",
    "DocComment",
    "ModuleDocComment",
    "BlockComment",

    "Identifier",
    "Underscore",
    "Integer",
    "Float",
    "Char",
    "Byte",
    "String",
    "RawString",

    "KwStruct",
    "KwSet",
    "KwEnum",
    "KwTrait",
    "KwImpl",
    "KwFor",
    "KwDyn",
    "KwFn",
    "KwReturn",
    "KwRequires",
    "KwSelf",
    "KwSelfType",
    "KwIf",
    "KwElse",
    "KwLoop",
    "KwIn",
    "KwBreak",
    "KwContinue",
    "KwMatch",
    "KwIs",
    "KwDefer",
    "KwTry",
    "KwConst",
    "KwMut",
    "KwComptime",
    "KwRegion",
    "KwThrough",
    "KwOn",
    "KwUnsafe",
    "KwAsm",
    "KwTogether",
    "KwSpawn",
    "KwImport",
    "KwAs",
    "KwPub",
    "KwExtern",
    "KwInclude",
    "KwLink",
    "KwAnd",
    "KwOr",
    "KwNot",
    "KwBitOr",
    "KwBitXor",
    "KwBitNot",
    "KwTrue",
    "KwFalse",

    "PlusPercentEq",
    "MinusPercentEq",
    "StarPercentEq",
    "ShlEq",
    "ShrEq",
    "DotDotEq",
    "Ellipsis",
    "PlusPercent",
    "MinusPercent",
    "StarPercent",
    "PlusQuestion",
    "MinusQuestion",
    "StarQuestion",
    "Shl",
    "Shr",
    "EqEq",
    "NotEq",
    "LessEq",
    "GreaterEq",
    "DotDot",
    "PlusEq",
    "MinusEq",
    "StarEq",
    "SlashEq",
    "PercentEq",
    "AmpEq",
    "Arrow",
    "FatArrow",
    "Plus",
    "Minus",
    "Star",
    "Slash",
    "Percent",
    "Amp",
    "Less",
    "Greater",
    "Eq",
    "Question",
    "Dot",
    "Comma",
    "Colon",
    "LParen",
    "RParen",
    "LBracket",
    "RBracket",
    "LBrace",
    "RBrace",
    "BitOrEq",
    "BitXorEq",

    "Error",
    "StatementEnd",
    "EndOfFile",

    "Module",
    "Fragment",
    "Manifest",
    "Attribute",
    "AttributeArgument",
    "Import",
    "CInclude",
    "ConstDecl",
    "StructDecl",
    "EnumDecl",
    "TraitDecl",
    "ImplDecl",
    "FunctionDecl",
    "Signature",
    "ComptimeParamList",
    "ParamList",
    "Param",
    "FieldList",
    "Field",
    "VariantList",
    "Variant",
    "VariantField",
    "SignatureList",
    "MethodList",

    "Block",
    "BindingStmt",
    "AssignStmt",
    "ExprStmt",
    "ReturnStmt",
    "BreakStmt",
    "ContinueStmt",
    "SpawnStmt",
    "DiscardStmt",
    "IfStmt",
    "LoopStmt",
    "RegionStmt",
    "ThroughStmt",
    "TogetherStmt",
    "UnsafeStmt",
    "DeferStmt",

    "LogicalExpr",
    "NotExpr",
    "ComparisonExpr",
    "MembershipExpr",
    "IsExpr",
    "RangeExpr",
    "BinaryExpr",
    "CastExpr",
    "PrefixExpr",
    "RefExpr",
    "OptionalType",
    "DynType",
    "PointerType",
    "SliceType",
    "ArrayType",
    "SentinelType",
    "FieldExpr",
    "CallExpr",
    "ArgList",
    "IndexExpr",
    "CompoundLiteral",
    "LiteralElement",
    "Literal",
    "NameExpr",
    "Placeholder",
    "ParenExpr",
    "TupleExpr",
    "MatchExpr",
    "MatchArm",
    "FunctionExpr",
    "FunctionParam",
    "TogetherExpr",
    "AsmExpr",
    "AsmOperand",
    "StructType",
    "EnumType",

    "OrPattern",
    "WildcardPattern",
    "LiteralPattern",
    "BindingPattern",
    "TuplePattern",
    "PathPattern",
    "PatternArgList",
    "PatternArg",
    "PatternFieldList",
    "PatternField",
};

static_assert(!KIND_NAMES.back().empty());

struct FixedText
{
    SyntaxKind kind;
    std::string_view text;
};

constexpr FixedText KEYWORDS[] = {
    {SyntaxKind::KwStruct, "структ"},
    {SyntaxKind::KwSet, "набор"},
    {SyntaxKind::KwEnum, "переч"},
    {SyntaxKind::KwTrait, "типаж"},
    {SyntaxKind::KwImpl, "реализ"},
    {SyntaxKind::KwFor, "для"},
    {SyntaxKind::KwDyn, "дин"},
    {SyntaxKind::KwFn, "функ"},
    {SyntaxKind::KwReturn, "возврат"},
    {SyntaxKind::KwRequires, "требует"},
    {SyntaxKind::KwSelf, "сам"},
    {SyntaxKind::KwSelfType, "Сам"},
    {SyntaxKind::KwIf, "если"},
    {SyntaxKind::KwElse, "иначе"},
    {SyntaxKind::KwLoop, "цикл"},
    {SyntaxKind::KwIn, "в"},
    {SyntaxKind::KwBreak, "прервать"},
    {SyntaxKind::KwContinue, "продолжить"},
    {SyntaxKind::KwMatch, "выбор"},
    {SyntaxKind::KwIs, "это"},
    {SyntaxKind::KwDefer, "отложить"},
    {SyntaxKind::KwTry, "попытка"},
    {SyntaxKind::KwConst, "конст"},
    {SyntaxKind::KwMut, "изм"},
    {SyntaxKind::KwComptime, "компил"},
    {SyntaxKind::KwRegion, "регион"},
    {SyntaxKind::KwThrough, "через"},
    {SyntaxKind::KwOn, "на"},
    {SyntaxKind::KwUnsafe, "опасно"},
    {SyntaxKind::KwAsm, "асм"},
    {SyntaxKind::KwTogether, "вместе"},
    {SyntaxKind::KwSpawn, "запуск"},
    {SyntaxKind::KwImport, "подключить"},
    {SyntaxKind::KwAs, "как"},
    {SyntaxKind::KwPub, "откр"},
    {SyntaxKind::KwExtern, "внеш"},
    {SyntaxKind::KwInclude, "вставка"},
    {SyntaxKind::KwLink, "связать"},
    {SyntaxKind::KwAnd, "и"},
    {SyntaxKind::KwOr, "или"},
    {SyntaxKind::KwNot, "не"},
    {SyntaxKind::KwBitOr, "бит_или"},
    {SyntaxKind::KwBitXor, "бит_искл"},
    {SyntaxKind::KwBitNot, "бит_не"},
    {SyntaxKind::KwTrue, "да"},
    {SyntaxKind::KwFalse, "нет"},
};

constexpr FixedText PUNCTUATION[] = {
    {SyntaxKind::PlusPercentEq, "+%="},
    {SyntaxKind::MinusPercentEq, "-%="},
    {SyntaxKind::StarPercentEq, "*%="},
    {SyntaxKind::ShlEq, "<<="},
    {SyntaxKind::ShrEq, ">>="},
    {SyntaxKind::DotDotEq, "..="},
    {SyntaxKind::Ellipsis, "..."},
    {SyntaxKind::PlusPercent, "+%"},
    {SyntaxKind::MinusPercent, "-%"},
    {SyntaxKind::StarPercent, "*%"},
    {SyntaxKind::PlusQuestion, "+?"},
    {SyntaxKind::MinusQuestion, "-?"},
    {SyntaxKind::StarQuestion, "*?"},
    {SyntaxKind::Shl, "<<"},
    {SyntaxKind::Shr, ">>"},
    {SyntaxKind::EqEq, "=="},
    {SyntaxKind::NotEq, "!="},
    {SyntaxKind::LessEq, "<="},
    {SyntaxKind::GreaterEq, ">="},
    {SyntaxKind::DotDot, ".."},
    {SyntaxKind::PlusEq, "+="},
    {SyntaxKind::MinusEq, "-="},
    {SyntaxKind::StarEq, "*="},
    {SyntaxKind::SlashEq, "/="},
    {SyntaxKind::PercentEq, "%="},
    {SyntaxKind::AmpEq, "&="},
    {SyntaxKind::Arrow, "->"},
    {SyntaxKind::FatArrow, "=>"},
    {SyntaxKind::Plus, "+"},
    {SyntaxKind::Minus, "-"},
    {SyntaxKind::Star, "*"},
    {SyntaxKind::Slash, "/"},
    {SyntaxKind::Percent, "%"},
    {SyntaxKind::Amp, "&"},
    {SyntaxKind::Less, "<"},
    {SyntaxKind::Greater, ">"},
    {SyntaxKind::Eq, "="},
    {SyntaxKind::Question, "?"},
    {SyntaxKind::Dot, "."},
    {SyntaxKind::Comma, ","},
    {SyntaxKind::Colon, ":"},
    {SyntaxKind::LParen, "("},
    {SyntaxKind::RParen, ")"},
    {SyntaxKind::LBracket, "["},
    {SyntaxKind::RBracket, "]"},
    {SyntaxKind::LBrace, "{"},
    {SyntaxKind::RBrace, "}"},
    {SyntaxKind::BitOrEq, "бит_или="},
    {SyntaxKind::BitXorEq, "бит_искл="},
    {SyntaxKind::Underscore, "_"},
};

}

bool isTrivia(SyntaxKind kind)
{
    return kind <= SyntaxKind::BlockComment;
}

bool isKeyword(SyntaxKind kind)
{
    return kind >= SyntaxKind::KwStruct && kind <= SyntaxKind::KwFalse;
}

bool isLiteralToken(SyntaxKind kind)
{
    return kind >= SyntaxKind::Integer && kind <= SyntaxKind::RawString;
}

bool isToken(SyntaxKind kind)
{
    return kind <= SyntaxKind::EndOfFile;
}

std::string_view kindName(SyntaxKind kind)
{
    return KIND_NAMES[static_cast<size_t>(kind)];
}

std::string_view fixedText(SyntaxKind kind)
{
    for (const FixedText& keyword : KEYWORDS)
    {
        if (keyword.kind == kind)
        {
            return keyword.text;
        }
    }
    for (const FixedText& punctuation : PUNCTUATION)
    {
        if (punctuation.kind == kind)
        {
            return punctuation.text;
        }
    }
    return {};
}

std::optional<SyntaxKind> keywordKind(std::string_view text)
{
    for (const FixedText& keyword : KEYWORDS)
    {
        if (keyword.text == text)
        {
            return keyword.kind;
        }
    }
    return std::nullopt;
}

}

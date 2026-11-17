#include "interp/machine.hpp"

#include "sema/naming.hpp"
#include "support/platform.hpp"
#include "support/utf8_path.hpp"
#include "syntax/diagnostic.hpp"
#include "unicode/unicode.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>

namespace splav
{

using SourceModule = Module;

using enum SyntaxKind;

namespace
{

constexpr size_t MAX_CALL_DEPTH = 50000;
constexpr size_t STACK_BYTES = size_t{1} << 30;
constexpr std::string_view SELF_KEY = "сам";
constexpr std::string_view SOME_KEY = "Есть";
constexpr std::string_view NONE_KEY = "Ничего";
constexpr std::string_view SUCCESS_KEY = "Успех";
constexpr std::string_view FAILURE_KEY = "Ошибка";
constexpr std::string_view ENTRY_KEY = "вход";
constexpr std::string_view TEST_ATTRIBUTE = "Тест";
constexpr std::string_view ASSERT_KEY = "утвердить";
constexpr std::string_view VECTOR_KEY = "Вектор";
constexpr std::array VECTOR_COMPONENTS = {"х", "у", "з", "в"};
constexpr std::array BUILTIN_TYPE_NAMES = {"стр", "сим", "бул", "пусто", "тип", "никогда"};

class ScopeGuard
{
public:
    explicit ScopeGuard(Frame& owner) : frame(owner), depth(owner.scopes.size())
    {
        frame.scopes.emplace_back();
    }

    ~ScopeGuard()
    {
        frame.scopes.resize(depth);
    }

    ScopeGuard(const ScopeGuard&) = delete;
    ScopeGuard& operator=(const ScopeGuard&) = delete;

private:
    Frame& frame;
    size_t depth;
};

bool isStatementKind(SyntaxKind kind)
{
    switch (kind)
    {
    case BindingStmt:
    case AssignStmt:
    case ExprStmt:
    case ReturnStmt:
    case BreakStmt:
    case ContinueStmt:
    case SpawnStmt:
    case DiscardStmt:
    case IfStmt:
    case LoopStmt:
    case RegionStmt:
    case ThroughStmt:
    case TogetherStmt:
    case UnsafeStmt:
    case DeferStmt:
    case Block:
        return true;
    default:
        return false;
    }
}

std::optional<SyntaxKind> compoundOperation(SyntaxKind kind)
{
    switch (kind)
    {
    case PlusEq:
        return Plus;
    case MinusEq:
        return Minus;
    case StarEq:
        return Star;
    case SlashEq:
        return Slash;
    case PercentEq:
        return Percent;
    case AmpEq:
        return Amp;
    case BitOrEq:
        return KwBitOr;
    case BitXorEq:
        return KwBitXor;
    case ShlEq:
        return Shl;
    case ShrEq:
        return Shr;
    case PlusPercentEq:
        return PlusPercent;
    case MinusPercentEq:
        return MinusPercent;
    case StarPercentEq:
        return StarPercent;
    default:
        return std::nullopt;
    }
}

bool isAssignmentOperator(SyntaxKind kind)
{
    return kind == Eq || compoundOperation(kind).has_value();
}

bool isComparisonOperator(SyntaxKind kind)
{
    return kind == EqEq || kind == NotEq || kind == Less || kind == LessEq || kind == Greater || kind == GreaterEq;
}

std::string decodeEscapes(std::string_view body)
{
    std::string text;
    for (size_t position = 0; position < body.size();)
    {
        if (body[position] != '\\' || position + 1 >= body.size())
        {
            text += body[position++];
            continue;
        }
        const char kind = body[position + 1];
        position += 2;
        switch (kind)
        {
        case 'n':
            text += '\n';
            break;
        case 't':
            text += '\t';
            break;
        case 'r':
            text += '\r';
            break;
        case '0':
            text += '\0';
            break;
        case 'u':
        {
            const size_t close = body.find('}', position);
            uint32_t value = 0;
            std::from_chars(body.data() + position + 1, body.data() + close, value, 16);
            unicode::appendUtf8(text, static_cast<char32_t>(value));
            position = close + 1;
            break;
        }
        default:
            text += kind;
            break;
        }
    }
    return text;
}

std::string decodeRawString(std::string_view raw)
{
    std::string text;
    bool first = true;
    while (!raw.empty())
    {
        const size_t end = raw.find('\n');
        std::string_view line = raw.substr(0, end);
        raw = end == std::string_view::npos ? std::string_view() : raw.substr(end + 1);
        const size_t content = line.find_first_not_of(" \t");
        line = content == std::string_view::npos ? std::string_view() : line.substr(content);
        if (line.starts_with("\\\\"))
        {
            line.remove_prefix(2);
        }
        if (!first)
        {
            text += '\n';
        }
        text += line;
        first = false;
    }
    return text;
}

std::string valueKindName(const Value& value)
{
    switch (value.kind)
    {
    case ValueKind::Void:
        return "пусто";
    case ValueKind::Bool:
        return "бул";
    case ValueKind::Integer:
        return value.number == Number::Untyped ? "целое" : std::string(numberName(value.number));
    case ValueKind::Float:
        return value.number == Number::Untyped ? "в64" : std::string(numberName(value.number));
    case ValueKind::Char:
        return "сим";
    case ValueKind::Str:
        return "стр";
    case ValueKind::Bytes:
        return "[]н8";
    case ValueKind::Struct:
    case ValueKind::Enum:
        return value.decl != nullptr ? value.decl->display : "?";
    case ValueKind::Optional:
        return "?Т";
    case ValueKind::Tuple:
        return "кортеж";
    case ValueKind::List:
        return value.growable ? "Массив" : "срез";
    case ValueKind::Reference:
        return "&Т";
    case ValueKind::Range:
        return "Диапазон";
    case ValueKind::Vector:
        return "Вектор";
    case ValueKind::Builder:
        return "Построитель";
    case ValueKind::Graphemes:
        return "Графемы";
    case ValueKind::Chars:
        return "Символы";
    case ValueKind::Enumeration:
        return "перечисление";
    case ValueKind::Function:
    case ValueKind::Closure:
        return "функ";
    case ValueKind::Type:
        return "тип";
    case ValueKind::Module:
        return "модуль";
    }
    return "?";
}

uint64_t signExtended(uint64_t bits, unsigned width)
{
    if (width >= 64 || (bits >> (width - 1)) == 0)
    {
        return bits;
    }
    return bits | ~((uint64_t{1} << width) - 1);
}

}

const Value& Call::argument(size_t index) const
{
    return *argumentCell(index);
}

const Cell& Call::argumentCell(size_t index) const
{
    if (index >= arguments.size())
    {
        machine.panic("встроенной функции `" + function.display + "` не хватает аргументов");
    }
    return dereference(arguments[index]);
}

Machine::Machine(Program& owner, SourceModule& main, const RunOptions& chosen)
    : program(owner), entryModule(main), options(chosen), declarations(owner, main)
{
    registerBuiltins(builtins);
    for (const TypeDecl& decl : declarations.types)
    {
        if (decl.kind != DeclKind::Enum)
        {
            continue;
        }
        for (uint32_t index = 0; index < decl.variants.size(); ++index)
        {
            variants[decl.variants[index].key].emplace_back(&decl, index);
        }
    }
    resultType = findType("ядро.прелюдия", "Итог");
    arrayType = findType("ядро.прелюдия", "Массив");
    rangeType = findType("ядро.прелюдия", "Диапазон");
    graphemesType = findType("ядро.прелюдия", "Графемы");
    charsType = findType("ядро.прелюдия", "Символы");
    builderType = findType("ядро.текст", "Построитель");
    failureType = findType("ядро.тест", "Провал");
    inputErrorType = findType("ядро.ос", "ОшибкаВвода");
    fileType = findType("ядро.фс", "Файл");
    regionType = findType("ядро.прелюдия", "Регион");
    arenaType = findType("ядро.память", "Рег");
    uniqueType = findType("ядро.прелюдия", "Уник");
    currentModule = &entryModule;
}

Machine::~Machine()
{
    for (const auto& [descriptor, file] : files)
    {
        std::fclose(file);
    }
}

const TypeDecl* Machine::findType(std::string_view module, std::string_view key) const
{
    const std::string wanted = nameKey(key);
    for (const TypeDecl& decl : declarations.types)
    {
        if (decl.key == wanted && decl.module != nullptr && decl.module->name == module)
        {
            return &decl;
        }
    }
    return nullptr;
}

Frame& Machine::frame()
{
    return frames.back();
}

const std::string& Machine::keyOf(const SyntaxToken& token)
{
    auto [found, inserted] = keys.try_emplace(token.text.data());
    if (inserted)
    {
        found->second = nameKey(token.text);
    }
    return found->second;
}

std::string Machine::locationOf(const SourceModule* module, uint32_t offset) const
{
    if (module == nullptr)
    {
        return "?";
    }
    const Position position = positionAt(module->source, offset);
    const std::string file = module->path.empty() ? module->name : utf8FromPath(module->path.filename());
    return file + ":" + std::to_string(position.line) + ":" + std::to_string(position.column);
}

void Machine::panic(const std::string& message) const
{
    throw Panic{message, locationOf(currentModule, currentOffset)};
}

void Machine::write(bool error, std::string_view text) const
{
    if (options.write)
    {
        options.write(error, text);
    }
}

Cell* Machine::findLocal(const std::string& key)
{
    std::vector<Scope>& scopes = frame().scopes;
    for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope)
    {
        for (auto local = scope->locals.rbegin(); local != scope->locals.rend(); ++local)
        {
            if (local->key == key)
            {
                return &local->cell;
            }
        }
    }
    return nullptr;
}

void Machine::bindLocal(const std::string& key, Cell cell)
{
    frame().scopes.back().locals.push_back(Local{key, std::move(cell)});
}

Cell Machine::consume(const Cell& cell) const
{
    return copyCell(cell);
}

void Machine::assignTo(const Cell& place, Cell value) const
{
    const Cell& target =
        place->kind == ValueKind::Reference && value->kind != ValueKind::Reference ? dereference(place) : place;
    *target = std::move(*value);
}

std::pair<const Entry*, SourceModule*> Machine::lookup(SourceModule* module, const std::string& key) const
{
    if (const auto scope = declarations.scopes.find(module); scope != declarations.scopes.end())
    {
        if (const auto found = scope->second.find(key); found != scope->second.end())
        {
            return {&found->second, module};
        }
    }
    if (declarations.prelude != nullptr && module != declarations.prelude)
    {
        if (const auto scope = declarations.scopes.find(declarations.prelude); scope != declarations.scopes.end())
        {
            if (const auto found = scope->second.find(key); found != scope->second.end())
            {
                return {&found->second, declarations.prelude};
            }
        }
    }
    return {nullptr, nullptr};
}

const TypeDecl* Machine::resolveDecl(const SyntaxNodeView& node, SourceModule* module) const
{
    switch (node.kind())
    {
    case NameExpr:
    {
        const std::optional<SyntaxToken> name = node.token(Identifier);
        if (!name)
        {
            return nullptr;
        }
        const auto [entry, owner] = lookup(module, nameKey(name->text));
        return entry != nullptr && entry->kind == EntryKind::Type ? entry->type : nullptr;
    }
    case FieldExpr:
    {
        const std::vector<SyntaxNodeView> parts = node.nodes();
        const std::optional<SyntaxToken> member = node.token(Identifier);
        if (parts.empty() || !member || parts.front().kind() != NameExpr || !parts.front().token(Identifier))
        {
            return nullptr;
        }
        const auto [entry, owner] = lookup(module, nameKey(parts.front().token(Identifier)->text));
        if (entry == nullptr || entry->kind != EntryKind::Module || entry->target == nullptr)
        {
            return nullptr;
        }
        const auto [memberEntry, memberOwner] = lookup(entry->target, nameKey(member->text));
        return memberEntry != nullptr && memberEntry->kind == EntryKind::Type ? memberEntry->type : nullptr;
    }
    case CallExpr:
    {
        const std::vector<SyntaxNodeView> parts = node.nodes();
        return parts.empty() ? nullptr : resolveDecl(parts.front(), module);
    }
    default:
        return nullptr;
    }
}

std::optional<Number> Machine::resolveNumber(const SyntaxNodeView& node, SourceModule* module) const
{
    if (node.kind() != NameExpr || !node.token(Identifier))
    {
        return std::nullopt;
    }
    const std::string key = nameKey(node.token(Identifier)->text);
    if (lookup(module, key).first != nullptr)
    {
        return std::nullopt;
    }
    return numberNamed(key);
}

std::optional<Number> Machine::numberOf(
    const SyntaxNodeView& type, SourceModule* module, const std::vector<Local>* bindings)
{
    if (bindings != nullptr && type.kind() == NameExpr && type.token(Identifier))
    {
        const std::string& key = keyOf(*type.token(Identifier));
        for (auto bound = bindings->rbegin(); bound != bindings->rend(); ++bound)
        {
            if (bound->key == key)
            {
                const Value& value = *dereference(bound->cell);
                return value.kind == ValueKind::Type ? numberNamed(value.name) : std::nullopt;
            }
        }
    }
    return resolveNumber(type, module);
}

void Machine::coerce(
    const Cell& cell, const SyntaxNodeView& type, SourceModule* module, const std::vector<Local>* bindings)
{
    if (!cell || !type)
    {
        return;
    }
    Value& value = *cell;
    switch (type.kind())
    {
    case OptionalType:
    {
        const std::vector<SyntaxNodeView> inner = type.nodes();
        if (value.kind != ValueKind::Optional)
        {
            Cell payload = std::make_shared<Value>(std::move(value));
            value = Value();
            value.kind = ValueKind::Optional;
            value.items.push_back(std::move(payload));
        }
        if (!value.items.empty() && !inner.empty())
        {
            coerce(value.items.front(), inner.front(), module, bindings);
        }
        return;
    }
    case NameExpr:
    {
        if ((value.kind != ValueKind::Integer && value.kind != ValueKind::Float) || value.number != Number::Untyped)
        {
            return;
        }
        const std::optional<Number> number = numberOf(type, module, bindings);
        if (!number)
        {
            return;
        }
        if (value.kind == ValueKind::Integer && !isFloatNumber(*number))
        {
            if (!fits(value.integer, *number))
            {
                panic("значение " + integerText(value.integer) + " не помещается в `" + std::string(numberName(*number))
                    + "`");
            }
            value.number = *number;
        }
        else if (value.kind == ValueKind::Float && isFloatNumber(*number))
        {
            value.number = *number;
            if (*number == Number::F32)
            {
                value.floating = static_cast<double>(static_cast<float>(value.floating));
            }
        }
        return;
    }
    case CallExpr:
    {
        if (value.kind != ValueKind::Enum || value.decl != resultType || value.items.empty())
        {
            return;
        }
        const SyntaxNodeView arguments = type.node(ArgList);
        const std::vector<SyntaxNodeView> parts = arguments ? arguments.nodes() : std::vector<SyntaxNodeView>();
        if (value.variant < parts.size())
        {
            coerce(value.items.front(), parts[value.variant], module, bindings);
        }
        return;
    }
    default:
        return;
    }
}

const Prepared& Machine::prepare(const FunctionInfo& function)
{
    auto [found, inserted] = prepared.try_emplace(&function);
    Prepared& result = found->second;
    if (!inserted)
    {
        return result;
    }
    result.body = function.item.node(Block);
    bool afterArrow = false;
    function.signature.forEachChild([&](const SyntaxNodeView& child, const std::optional<SyntaxToken>& token)
    {
        if (token)
        {
            afterArrow = afterArrow || token->kind == Arrow;
            return;
        }
        if (afterArrow && !result.result)
        {
            result.result = child;
        }
    });
    if (const SyntaxNodeView list = function.signature.node(ParamList))
    {
        for (const SyntaxNodeView& node : list.nodes())
        {
            if (node.kind() != Param)
            {
                continue;
            }
            Parameter parameter;
            const std::vector<SyntaxNodeView> parts = node.nodes();
            parameter.type = parts.empty() ? SyntaxNodeView() : parts.back();
            if (node.hasToken(KwSelf))
            {
                parameter.self = true;
                parameter.key = std::string(SELF_KEY);
                parameter.reference = node.hasToken(Amp);
            }
            else
            {
                const std::optional<SyntaxToken> name = node.token(Identifier);
                parameter.key = name ? nameKey(name->text) : std::string();
                parameter.comptime = node.hasToken(KwComptime);
                parameter.variadic = node.hasToken(Ellipsis);
                parameter.reference = parameter.type && parameter.type.kind() == RefExpr;
                result.runtimeCount += 1;
                result.comptimeCount += parameter.comptime ? 1 : 0;
            }
            result.parameters.push_back(parameter);
        }
    }
    if (result.result && result.result.kind() == CallExpr)
    {
        const std::vector<SyntaxNodeView> parts = result.result.nodes();
        const SyntaxNodeView arguments = result.result.node(ArgList);
        const std::vector<SyntaxNodeView> values = arguments ? arguments.nodes() : std::vector<SyntaxNodeView>();
        if (!parts.empty() && resolveDecl(parts.front(), function.module) == resultType && values.size() == 2)
        {
            result.error = resolveDecl(values[1], function.module);
        }
    }
    return result;
}

Cell Machine::defaultValue(const SyntaxNodeView& type, SourceModule* module)
{
    if (!type)
    {
        return voidCell();
    }
    if (type.kind() == ArrayType)
    {
        const std::vector<SyntaxNodeView> parts = type.nodes();
        if (parts.size() < 2)
        {
            return voidCell();
        }
        const size_t count = sizeOf(eval(parts.front()));
        std::vector<Cell> items;
        items.reserve(count);
        for (size_t index = 0; index < count; ++index)
        {
            items.push_back(defaultValue(parts.back(), module));
        }
        return listCell(std::move(items), false);
    }
    if (const std::optional<Number> number = resolveNumber(type, module))
    {
        return isFloatNumber(*number) ? floatCell(0, *number) : integerCell(WideInteger{}, *number);
    }
    if (type.kind() == NameExpr && type.token(Identifier) && nameKey(type.token(Identifier)->text) == "бул")
    {
        return boolCell(false);
    }
    return voidCell();
}

Cell Machine::wrapError(const Cell& error)
{
    const TypeDecl* target = frame().errorType;
    Cell value = consume(dereference(error));
    if (target == nullptr || value->decl == target || value->decl == nullptr || target->kind != DeclKind::Enum)
    {
        return value;
    }
    for (uint32_t index = 0; index < target->variants.size(); ++index)
    {
        const VariantDecl& variant = target->variants[index];
        if (variant.payload.size() == 1 && resolveDecl(variant.payload.front(), target->module) == value->decl)
        {
            return construct(target, index, {value});
        }
    }
    return value;
}

std::optional<std::pair<const TypeDecl*, uint32_t>> Machine::findVariant(const std::string& key) const
{
    const auto found = variants.find(key);
    if (found == variants.end() || found->second.empty())
    {
        return std::nullopt;
    }
    const auto& candidates = found->second;
    for (const auto& candidate : candidates)
    {
        if (candidate.first->module == currentModule)
        {
            return candidate;
        }
    }
    if (const auto scope = declarations.scopes.find(currentModule); scope != declarations.scopes.end())
    {
        for (const auto& candidate : candidates)
        {
            for (const auto& [name, entry] : scope->second)
            {
                if (entry.kind == EntryKind::Module && entry.target == candidate.first->module)
                {
                    return candidate;
                }
            }
        }
    }
    for (const auto& candidate : candidates)
    {
        if (candidate.first->module == declarations.prelude)
        {
            return candidate;
        }
    }
    return candidates.front();
}

Cell Machine::constant(const Entry& entry, SourceModule* owner)
{
    const std::pair<SourceModule*, uint32_t> key{owner, entry.constant.offset()};
    if (const auto found = constants.find(key); found != constants.end())
    {
        return found->second;
    }
    std::vector<SyntaxNodeView> parts;
    for (const SyntaxNodeView& child : entry.constant.nodes())
    {
        if (child.kind() != Attribute)
        {
            parts.push_back(child);
        }
    }
    if (parts.empty())
    {
        panic("у константы нет значения");
    }
    const SyntaxNodeView type = entry.constant.hasToken(Colon) && parts.size() > 1 ? parts.front() : SyntaxNodeView();
    Frame& context = frames.emplace_back();
    context.module = owner;
    context.scopes.emplace_back();
    SourceModule* const savedModule = currentModule;
    currentModule = owner;
    Cell value = consume(eval(parts.back()));
    coerce(value, type, owner);
    currentModule = savedModule;
    frames.pop_back();
    constants.emplace(key, value);
    return value;
}

Cell Machine::entryValue(const Entry& entry, SourceModule* owner)
{
    Value value;
    switch (entry.kind)
    {
    case EntryKind::Type:
        value.kind = ValueKind::Type;
        value.decl = entry.type;
        return makeCell(std::move(value));
    case EntryKind::Function:
        value.kind = ValueKind::Function;
        value.function = entry.function;
        return makeCell(std::move(value));
    case EntryKind::Module:
        value.kind = ValueKind::Module;
        value.module = entry.target;
        return makeCell(std::move(value));
    case EntryKind::Const:
        return constant(entry, owner);
    case EntryKind::Header:
        panic("модули `вставка` исполняются с этапа 5: интерпретатор не вызывает C");
    }
    return voidCell();
}

const FunctionInfo* Machine::methodFor(const Value& value, const std::string& key) const
{
    switch (value.kind)
    {
    case ValueKind::Struct:
    case ValueKind::Enum:
        return declarations.method(value.decl, "", key);
    case ValueKind::Integer:
        return declarations.method(nullptr, std::string(numberName(value.number)), key);
    case ValueKind::Float:
        return declarations.method(nullptr, value.number == Number::F32 ? std::string("в32") : std::string("в64"), key);
    case ValueKind::Char:
        return declarations.method(nullptr, "сим", key);
    case ValueKind::Str:
        return declarations.method(nullptr, "стр", key);
    case ValueKind::Bool:
        return declarations.method(nullptr, "бул", key);
    case ValueKind::List:
        return value.growable && arrayType != nullptr ? declarations.method(arrayType, "", key) : nullptr;
    case ValueKind::Builder:
        return builderType != nullptr ? declarations.method(builderType, "", key) : nullptr;
    case ValueKind::Graphemes:
        return graphemesType != nullptr ? declarations.method(graphemesType, "", key) : nullptr;
    case ValueKind::Chars:
        return charsType != nullptr ? declarations.method(charsType, "", key) : nullptr;
    default:
        return nullptr;
    }
}

Cell Machine::enumValue(const TypeDecl* decl, std::string_view variant, std::vector<Cell> payload) const
{
    const std::string key = nameKey(variant);
    if (decl == nullptr)
    {
        panic("перечисление для варианта `" + key + "` не загружено");
    }
    for (uint32_t index = 0; index < decl->variants.size(); ++index)
    {
        if (decl->variants[index].key == key)
        {
            Value value;
            value.kind = ValueKind::Enum;
            value.decl = decl;
            value.variant = index;
            value.items = std::move(payload);
            return makeCell(std::move(value));
        }
    }
    panic("у `" + decl->display + "` нет варианта `" + key + "`");
}

Cell Machine::structValue(const TypeDecl* decl, std::vector<Cell> items) const
{
    if (decl == nullptr)
    {
        panic("структура не загружена");
    }
    Value value;
    value.kind = ValueKind::Struct;
    value.decl = decl;
    value.items = std::move(items);
    return makeCell(std::move(value));
}

Cell Machine::success(Cell payload) const
{
    std::vector<Cell> items;
    if (payload)
    {
        items.push_back(std::move(payload));
    }
    return enumValue(resultType, SUCCESS_KEY, std::move(items));
}

Cell Machine::failure(Cell error) const
{
    return enumValue(resultType, FAILURE_KEY, {std::move(error)});
}

Cell Machine::construct(const TypeDecl* decl, uint32_t variant, std::vector<Cell> arguments)
{
    const VariantDecl& declared = decl->variants[variant];
    std::vector<Cell> payload;
    payload.reserve(arguments.size());
    for (size_t index = 0; index < arguments.size(); ++index)
    {
        Cell item = consume(arguments[index]);
        if (index < declared.payload.size())
        {
            const auto known = typeArguments.find(decl);
            coerce(
                item, declared.payload[index], decl->module, known != typeArguments.end() ? &known->second : nullptr);
        }
        payload.push_back(std::move(item));
    }
    Value value;
    value.kind = ValueKind::Enum;
    value.decl = decl;
    value.variant = variant;
    value.items = std::move(payload);
    return makeCell(std::move(value));
}

Cell Machine::invoke(const FunctionInfo& function, Cell receiver, std::vector<Cell> arguments)
{
    if (function.builtin)
    {
        auto [cached, inserted] = builtinCache.try_emplace(&function, nullptr);
        if (inserted)
        {
            const std::string id = function.owner != nullptr ? function.owner->key + "." + function.key
                : !function.builtinOwner.empty()             ? function.builtinOwner + "." + function.key
                                                             : nameKey(function.module->name) + "." + function.key;
            const auto found = builtins.find(id);
            if (found == builtins.end())
            {
                builtinCache.erase(cached);
                panic("встроенная функция `" + id + "` пока не исполняется интерпретатором");
            }
            cached->second = found->second;
        }
        Call call{*this, function, receiver ? dereference(receiver) : Cell(), arguments};
        return cached->second(call);
    }
    const Prepared& signature = prepare(function);
    if (!signature.body)
    {
        panic("у функции `" + function.display + "` нет тела");
    }
    if (depth >= MAX_CALL_DEPTH)
    {
        panic("стек задачи исчерпан: глубина вызовов больше " + std::to_string(MAX_CALL_DEPTH));
    }
    Frame callee;
    callee.function = &function;
    callee.module = function.module;
    callee.errorType = signature.error;
    callee.scopes.emplace_back();
    const bool skipComptime = arguments.size() + signature.comptimeCount <= signature.runtimeCount;
    std::vector<Local> typeBindings;
    if (skipComptime && signature.comptimeCount > 0)
    {
        typeBindings = inferTypeArguments(signature, arguments);
        for (const Local& binding : typeBindings)
        {
            callee.scopes.back().locals.push_back(binding);
        }
    }
    size_t next = 0;
    for (const Parameter& parameter : signature.parameters)
    {
        Cell bound;
        if (parameter.self)
        {
            if (!receiver)
            {
                panic("метод `" + function.display + "` вызван без получателя");
            }
            bound = parameter.reference
                ? (receiver->kind == ValueKind::Reference ? consume(receiver) : referenceCell(receiver))
                : consume(dereference(receiver));
        }
        else if (parameter.comptime && skipComptime)
        {
            continue;
        }
        else if (parameter.variadic)
        {
            std::vector<Cell> rest;
            for (; next < arguments.size(); ++next)
            {
                rest.push_back(consume(arguments[next]));
            }
            bound = listCell(std::move(rest), false);
        }
        else
        {
            if (next >= arguments.size())
            {
                panic("функции `" + function.display + "` не хватает аргументов");
            }
            const Cell& argument = arguments[next++];
            if (parameter.reference)
            {
                bound = argument->kind == ValueKind::Reference ? consume(argument) : referenceCell(argument);
            }
            else
            {
                bound = consume(dereference(argument));
                coerce(bound, parameter.type, function.module, &typeBindings);
            }
            if (parameter.comptime)
            {
                typeBindings.push_back(Local{parameter.key, bound});
            }
        }
        callee.scopes.back().locals.push_back(Local{parameter.key, std::move(bound)});
    }
    frames.push_back(std::move(callee));
    ++depth;
    SourceModule* const savedModule = currentModule;
    const uint32_t savedOffset = currentOffset;
    currentModule = function.module;
    const Flow flow = execBlock(signature.body);
    Cell result = flow == Flow::Return && frame().returned ? frame().returned : voidCell();
    frames.pop_back();
    --depth;
    currentModule = savedModule;
    currentOffset = savedOffset;
    coerce(result, signature.result, function.module, &typeBindings);
    return result;
}

std::vector<Local> Machine::inferTypeArguments(const Prepared& signature, const std::vector<Cell>& arguments)
{
    std::vector<Local> inferred;
    for (const Parameter& parameter : signature.parameters)
    {
        if (!parameter.comptime)
        {
            continue;
        }
        Cell found;
        size_t position = 0;
        for (const Parameter& candidate : signature.parameters)
        {
            if (candidate.self || candidate.comptime)
            {
                continue;
            }
            SyntaxNodeView type = candidate.type;
            while (type && (type.kind() == RefExpr || type.kind() == OptionalType))
            {
                type = type.child(0);
            }
            if (!found && position < arguments.size() && type && type.kind() == NameExpr && type.token(Identifier)
                && keyOf(*type.token(Identifier)) == parameter.key)
            {
                found = typeOf(arguments[position]);
            }
            ++position;
        }
        if (!found)
        {
            Value unknown;
            unknown.kind = ValueKind::Type;
            unknown.name = "?";
            found = makeCell(std::move(unknown));
        }
        inferred.push_back(Local{parameter.key, found});
    }
    return inferred;
}

Cell Machine::typeOf(const Cell& cell) const
{
    const Value& value = *dereference(cell);
    Value type;
    type.kind = ValueKind::Type;
    switch (value.kind)
    {
    case ValueKind::Integer:
        type.name = std::string(numberName(value.number));
        break;
    case ValueKind::Float:
        type.name = value.number == Number::F32 ? "в32" : "в64";
        break;
    case ValueKind::Bool:
        type.name = "бул";
        break;
    case ValueKind::Char:
        type.name = "сим";
        break;
    case ValueKind::Str:
        type.name = "стр";
        break;
    case ValueKind::Struct:
    case ValueKind::Enum:
        type.decl = value.decl;
        break;
    case ValueKind::Vector:
        type.name = std::string(VECTOR_KEY);
        type.number = value.number;
        type.length = value.items.size();
        break;
    case ValueKind::Type:
        type.name = "тип";
        break;
    default:
        type.name = valueKindName(value);
        break;
    }
    return makeCell(std::move(type));
}

std::string Machine::typeName(const Cell& cell) const
{
    const Value& value = *dereference(cell);
    if (value.kind != ValueKind::Type)
    {
        return valueKindName(value);
    }
    if (value.decl != nullptr)
    {
        return value.decl->display;
    }
    if (value.name == VECTOR_KEY)
    {
        return "Вектор(" + std::string(numberName(value.number)) + ", " + std::to_string(value.length) + ")";
    }
    return value.name;
}

Cell Machine::synthesize(const SyntaxNodeView& node)
{
    Frame& current = frame();
    std::vector<Local> bindings;
    std::string arguments;
    if (current.function != nullptr && !current.function->builtin)
    {
        for (const Parameter& parameter : prepare(*current.function).parameters)
        {
            if (!parameter.comptime)
            {
                continue;
            }
            if (Cell* bound = findLocal(parameter.key))
            {
                bindings.push_back(Local{parameter.key, *bound});
                arguments += (arguments.empty() ? "" : ", ") + typeName(*bound);
            }
        }
    }
    const std::string identity = (current.module != nullptr ? current.module->name : std::string()) + ":"
        + std::to_string(node.offset()) + "(" + arguments + ")";
    Value result;
    result.kind = ValueKind::Type;
    if (const auto found = instances.find(identity); found != instances.end())
    {
        result.decl = found->second;
        return makeCell(std::move(result));
    }
    TypeDecl& decl = synthesizedTypes.emplace_back();
    decl.kind = node.kind() == EnumType ? DeclKind::Enum : DeclKind::Struct;
    decl.module = current.module;
    decl.node = node;
    decl.nameOffset = node.offset();
    const std::string base = current.function != nullptr ? current.function->display
        : node.kind() == EnumType                        ? std::string("переч")
                                                         : std::string("структ");
    decl.key = current.function != nullptr ? current.function->key : base;
    decl.display = arguments.empty() ? base : base + "(" + arguments + ")";
    if (const SyntaxNodeView fields = node.node(FieldList))
    {
        for (const SyntaxNodeView& field : fields.nodes())
        {
            const std::optional<SyntaxToken> name = field.token(Identifier);
            if (field.kind() != Field || !name)
            {
                continue;
            }
            FieldDecl described;
            described.key = keyOf(*name);
            described.display = std::string(name->text);
            for (const SyntaxNodeView& part : field.nodes())
            {
                if (part.kind() != Attribute)
                {
                    described.type = part;
                }
            }
            described.open = true;
            decl.fields.push_back(described);
        }
    }
    if (const SyntaxNodeView list = node.node(VariantList))
    {
        for (const SyntaxNodeView& variant : list.nodes())
        {
            const std::optional<SyntaxToken> name = variant.token(Identifier);
            if (variant.kind() != Variant || !name)
            {
                continue;
            }
            VariantDecl described;
            described.key = keyOf(*name);
            described.display = std::string(name->text);
            for (const SyntaxNodeView& field : variant.nodes())
            {
                if (field.kind() == VariantField)
                {
                    for (const SyntaxNodeView& part : field.nodes())
                    {
                        described.payload.push_back(part);
                    }
                }
            }
            variants[described.key].emplace_back(&decl, static_cast<uint32_t>(decl.variants.size()));
            decl.variants.push_back(described);
        }
    }
    typeArguments[&decl] = std::move(bindings);
    instances.emplace(identity, &decl);
    result.decl = &decl;
    return makeCell(std::move(result));
}

Cell Machine::closureValue(const SyntaxNodeView& node)
{
    const bool literal = node.hasToken(FatArrow) || node.node(Block);
    if (!literal)
    {
        Value type;
        type.kind = ValueKind::Type;
        type.name = "функ";
        return makeCell(std::move(type));
    }
    const bool copied = node.hasToken(LBracket);
    auto closure = std::make_shared<Closure>();
    closure->node = node;
    closure->module = frame().module;
    closure->enclosing = frame().function;
    for (const Scope& scope : frame().scopes)
    {
        for (const Local& local : scope.locals)
        {
            closure->captures.emplace_back(local.key, copied ? consume(local.cell) : local.cell);
        }
    }
    Value value;
    value.kind = ValueKind::Closure;
    value.closure = std::move(closure);
    return makeCell(std::move(value));
}

Cell Machine::callClosure(const Closure& closure, std::vector<Cell> arguments)
{
    if (depth >= MAX_CALL_DEPTH)
    {
        panic("стек задачи исчерпан: глубина вызовов больше " + std::to_string(MAX_CALL_DEPTH));
    }
    Frame callee;
    callee.function = closure.enclosing;
    callee.module = closure.module;
    callee.scopes.emplace_back();
    for (const auto& [key, cell] : closure.captures)
    {
        callee.scopes.back().locals.push_back(Local{key, cell});
    }
    callee.scopes.emplace_back();
    SyntaxNodeView body;
    SyntaxNodeView result;
    bool afterArrow = false;
    bool afterFatArrow = false;
    size_t next = 0;
    closure.node.forEachChild([&](const SyntaxNodeView& child, const std::optional<SyntaxToken>& token)
    {
        if (token)
        {
            afterArrow = afterArrow || token->kind == Arrow;
            afterFatArrow = afterFatArrow || token->kind == FatArrow;
            return;
        }
        if (child.kind() == FunctionParam)
        {
            const std::optional<SyntaxToken> name = child.token(Identifier);
            const SyntaxNodeView type = child.hasToken(Colon) ? child.child(0) : SyntaxNodeView();
            if (next >= arguments.size())
            {
                panic("замыканию не хватает аргументов");
            }
            const Cell& argument = arguments[next++];
            Cell bound;
            if (type && type.kind() == RefExpr)
            {
                bound = argument->kind == ValueKind::Reference ? consume(argument) : referenceCell(argument);
            }
            else
            {
                bound = consume(dereference(argument));
                coerce(bound, type, closure.module);
            }
            if (name)
            {
                callee.scopes.back().locals.push_back(Local{keyOf(*name), std::move(bound)});
            }
        }
        else if (child.kind() == Block || afterFatArrow)
        {
            body = child;
        }
        else if (afterArrow && !result)
        {
            result = child;
        }
    });
    if (result && result.kind() == CallExpr)
    {
        const SyntaxNodeView values = result.node(ArgList);
        if (resolveDecl(result.child(0), closure.module) == resultType && values && values.child(1))
        {
            callee.errorType = resolveDecl(values.child(1), closure.module);
        }
    }
    frames.push_back(std::move(callee));
    ++depth;
    SourceModule* const savedModule = currentModule;
    const uint32_t savedOffset = currentOffset;
    currentModule = closure.module;
    Cell value;
    if (body.kind() == Block)
    {
        const Flow flow = execBlock(body);
        value = flow == Flow::Return && frame().returned ? frame().returned : voidCell();
    }
    else
    {
        try
        {
            value = consume(eval(body));
        }
        catch (const Escape&)
        {
            value = frame().returned ? frame().returned : voidCell();
        }
    }
    frames.pop_back();
    --depth;
    currentModule = savedModule;
    currentOffset = savedOffset;
    coerce(value, result, closure.module);
    return value;
}

Flow Machine::execBlock(const SyntaxNodeView& block)
{
    Frame& current = frame();
    const size_t level = current.scopes.size();
    current.scopes.emplace_back();
    Flow flow = Flow::Normal;
    for (const SyntaxNodeView& statement : block.nodes())
    {
        try
        {
            flow = exec(statement);
        }
        catch (const Escape& escape)
        {
            flow = escape.flow;
            current.scopes.resize(level + 1);
        }
        if (flow != Flow::Normal)
        {
            break;
        }
    }
    std::vector<SyntaxNodeView> defers = std::move(current.scopes[level].defers);
    runDefers(std::move(defers));
    current.scopes.resize(level);
    return flow;
}

void Machine::runDefers(std::vector<SyntaxNodeView> defers)
{
    for (auto deferred = defers.rbegin(); deferred != defers.rend(); ++deferred)
    {
        Frame& current = frame();
        const size_t level = current.scopes.size();
        const Cell returned = current.returned;
        const std::string label = current.label;
        try
        {
            if (isStatementKind(deferred->kind()))
            {
                exec(*deferred);
            }
            else
            {
                eval(*deferred);
            }
        }
        catch (const Escape&)
        {
            current.scopes.resize(level);
        }
        current.returned = returned;
        current.label = label;
    }
}

Flow Machine::exec(const SyntaxNodeView& statement)
{
    currentOffset = statement.offset();
    switch (statement.kind())
    {
    case BindingStmt:
        return execBinding(statement);
    case AssignStmt:
        return execAssign(statement);
    case ExprStmt:
    {
        const SyntaxNodeView expression = statement.child(0);
        if (!expression)
        {
            return Flow::Normal;
        }
        if (expression.kind() == MatchExpr)
        {
            Flow flow = Flow::Normal;
            match(expression, &flow);
            return flow;
        }
        eval(expression);
        return Flow::Normal;
    }
    case DiscardStmt:
    {
        if (const SyntaxNodeView expression = statement.child(0))
        {
            eval(expression);
        }
        return Flow::Normal;
    }
    case ReturnStmt:
    {
        const SyntaxNodeView expression = statement.child(0);
        Cell value = expression ? consume(eval(expression)) : voidCell();
        frame().returned = std::move(value);
        return Flow::Return;
    }
    case BreakStmt:
    case ContinueStmt:
    {
        const std::optional<SyntaxToken> label = statement.token(Identifier);
        frame().label = label ? keyOf(*label) : std::string();
        return statement.kind() == BreakStmt ? Flow::Break : Flow::Continue;
    }
    case IfStmt:
        return execIf(statement);
    case LoopStmt:
        return execLoop(statement);
    case DeferStmt:
    {
        const std::vector<SyntaxNodeView> parts = statement.nodes();
        if (!parts.empty())
        {
            frame().scopes.back().defers.push_back(parts.front());
        }
        return Flow::Normal;
    }
    case Block:
        return execBlock(statement);
    case RegionStmt:
    case ThroughStmt:
    case UnsafeStmt:
    {
        const SyntaxNodeView block = statement.node(Block);
        if (!block)
        {
            return Flow::Normal;
        }
        for (const SyntaxNodeView& child : statement.nodes())
        {
            if (child.kind() != Block)
            {
                eval(child);
            }
        }
        const std::optional<SyntaxToken> name =
            statement.kind() == RegionStmt ? statement.token(Identifier) : std::nullopt;
        if (!name)
        {
            return execBlock(block);
        }
        ScopeGuard guard(frame());
        bindLocal(keyOf(*name), structValue(regionType, {}));
        return execBlock(block);
    }
    default:
        panic("инструкция «" + std::string(kindName(statement.kind())) + "» пока не исполняется интерпретатором");
    }
}

Flow Machine::execBinding(const SyntaxNodeView& statement)
{
    const std::optional<SyntaxToken> name = statement.token(Identifier);
    const std::vector<SyntaxNodeView> parts = statement.nodes();
    SyntaxNodeView type;
    SyntaxNodeView initializer;
    if (statement.hasToken(Colon))
    {
        type = parts.empty() ? SyntaxNodeView() : parts.front();
        initializer = parts.size() > 1 ? parts[1] : SyntaxNodeView();
    }
    else if (!parts.empty())
    {
        initializer = parts.front();
    }
    Cell value = initializer ? consume(eval(initializer)) : defaultValue(type, frame().module);
    coerce(value, type, frame().module);
    if (name)
    {
        bindLocal(keyOf(*name), std::move(value));
    }
    return Flow::Normal;
}

Flow Machine::execAssign(const SyntaxNodeView& statement)
{
    const SyntaxNodeView target = statement.child(0);
    const SyntaxNodeView source = statement.child(1);
    if (!source)
    {
        return Flow::Normal;
    }
    SyntaxKind operation = Eq;
    uint32_t operatorOffset = statement.offset();
    statement.forEachChild([&](const SyntaxNodeView&, const std::optional<SyntaxToken>& token)
    {
        if (token && operation == Eq && isAssignmentOperator(token->kind))
        {
            operation = token->kind;
            operatorOffset = token->offset;
        }
    });
    if (operation == Eq)
    {
        if (target.kind() == NameExpr && target.token(Identifier))
        {
            const std::string key = keyOf(*target.token(Identifier));
            Cell value = consume(eval(source));
            if (Cell* existing = findLocal(key))
            {
                assignTo(*existing, std::move(value));
            }
            else
            {
                bindLocal(key, std::move(value));
            }
            return Flow::Normal;
        }
        if (target.kind() == TupleExpr || target.kind() == CallExpr || target.kind() == CompoundLiteral)
        {
            const Cell value = eval(source);
            destructure(target, value);
            return Flow::Normal;
        }
        const Cell place = eval(target);
        assignTo(place, consume(eval(source)));
        return Flow::Normal;
    }
    const Cell place = eval(target);
    const Cell value = eval(source);
    currentOffset = operatorOffset;
    assignTo(place, arithmetic(*compoundOperation(operation), place, value));
    return Flow::Normal;
}

void Machine::destructure(const SyntaxNodeView& target, const Cell& input)
{
    const Cell& value = dereference(input);
    switch (target.kind())
    {
    case NameExpr:
    {
        const std::optional<SyntaxToken> name = target.token(Identifier);
        if (!name)
        {
            return;
        }
        const std::string key = keyOf(*name);
        if (Cell* existing = findLocal(key))
        {
            assignTo(*existing, consume(input));
        }
        else
        {
            bindLocal(key, consume(input));
        }
        return;
    }
    case TupleExpr:
    {
        const std::vector<SyntaxNodeView> elements = target.nodes();
        if (value->kind != ValueKind::Tuple || value->items.size() != elements.size())
        {
            panic("кортеж не подходит к образцу привязки");
        }
        for (size_t index = 0; index < elements.size(); ++index)
        {
            destructure(elements[index], value->items[index]);
        }
        return;
    }
    case CallExpr:
    {
        const std::vector<SyntaxNodeView> parts = target.nodes();
        const SyntaxNodeView arguments = target.node(ArgList);
        const std::vector<SyntaxNodeView> elements = arguments ? arguments.nodes() : std::vector<SyntaxNodeView>();
        const std::optional<SyntaxToken> head = parts.empty() ? std::nullopt : parts.front().token(Identifier);
        const std::string key = head ? keyOf(*head) : std::string();
        bool matches = false;
        if (value->kind == ValueKind::Optional)
        {
            matches = key == SOME_KEY && !value->items.empty();
        }
        else if (value->kind == ValueKind::Enum)
        {
            matches = value->decl->variants[value->variant].key == key;
        }
        if (!matches || elements.size() != value->items.size())
        {
            panic("образец привязки `" + key + "` не подошёл к значению");
        }
        for (size_t index = 0; index < elements.size(); ++index)
        {
            destructure(elements[index], value->items[index]);
        }
        return;
    }
    case CompoundLiteral:
    {
        if (value->kind != ValueKind::Struct)
        {
            panic("образец структуры не подошёл к значению");
        }
        for (const SyntaxNodeView& element : target.nodes())
        {
            if (element.kind() != LiteralElement)
            {
                continue;
            }
            const std::vector<SyntaxNodeView> values = element.nodes();
            const std::optional<SyntaxToken> label = element.token(Identifier);
            const std::optional<SyntaxToken> fieldName =
                label ? label : (values.empty() ? std::nullopt : values.front().token(Identifier));
            if (!fieldName || values.empty())
            {
                continue;
            }
            const std::string key = keyOf(*fieldName);
            for (size_t index = 0; index < value->decl->fields.size(); ++index)
            {
                if (value->decl->fields[index].key == key)
                {
                    destructure(values.front(), value->items[index]);
                }
            }
        }
        return;
    }
    case Placeholder:
        return;
    default:
        panic("образец привязки «" + std::string(kindName(target.kind())) + "» не поддерживается");
    }
}

Flow Machine::execIf(const SyntaxNodeView& statement)
{
    ScopeGuard guard(frame());
    const SyntaxNodeView condition = statement.child(0);
    const SyntaxNodeView then = statement.child(1);
    if (!then)
    {
        return Flow::Normal;
    }
    if (truth(eval(condition)))
    {
        return execBlock(then);
    }
    if (const SyntaxNodeView otherwise = statement.child(2))
    {
        return otherwise.kind() == IfStmt ? execIf(otherwise) : execBlock(otherwise);
    }
    return Flow::Normal;
}

std::optional<Flow> Machine::loopStep(Flow flow, const std::string& label)
{
    switch (flow)
    {
    case Flow::Normal:
        return std::nullopt;
    case Flow::Continue:
    case Flow::Break:
    {
        std::string& pending = frame().label;
        if (!pending.empty() && pending != label)
        {
            return flow;
        }
        pending.clear();
        return flow == Flow::Continue ? std::nullopt : std::optional<Flow>(Flow::Normal);
    }
    case Flow::Return:
        return Flow::Return;
    }
    return std::nullopt;
}

Flow Machine::execLoop(const SyntaxNodeView& statement)
{
    std::string label;
    bool afterLoop = false;
    bool afterIn = false;
    SyntaxNodeView pattern;
    SyntaxNodeView source;
    SyntaxNodeView condition;
    SyntaxNodeView body;
    statement.forEachChild([&](const SyntaxNodeView& child, const std::optional<SyntaxToken>& token)
    {
        if (token)
        {
            if (token->kind == Identifier && !afterLoop)
            {
                label = keyOf(*token);
            }
            afterLoop = afterLoop || token->kind == KwLoop;
            afterIn = afterIn || token->kind == KwIn;
            return;
        }
        if (child.kind() == Block)
        {
            body = child;
        }
        else if (statement.hasToken(KwIn) && !afterIn)
        {
            pattern = child;
        }
        else if (afterIn)
        {
            source = child;
        }
        else
        {
            condition = child;
        }
    });
    if (!body)
    {
        return Flow::Normal;
    }
    if (source)
    {
        const Cell sequence = eval(source);
        std::optional<Flow> result;
        forEachElement(sequence, [&](const Cell& element)
        {
            ScopeGuard guard(frame());
            if (pattern && !matchPattern(pattern, element, false))
            {
                panic("образец заголовка `цикл` не подошёл к элементу");
            }
            const std::optional<Flow> step = loopStep(execBlock(body), label);
            if (step)
            {
                result = step;
                return false;
            }
            return true;
        });
        return result.value_or(Flow::Normal);
    }
    for (;;)
    {
        if (condition)
        {
            ScopeGuard guard(frame());
            if (!truth(eval(condition)))
            {
                return Flow::Normal;
            }
        }
        if (const std::optional<Flow> step = loopStep(execBlock(body), label))
        {
            return *step;
        }
    }
}

void Machine::forEachElement(const Cell& input, const std::function<bool(const Cell&)>& visitor)
{
    const Cell source = dereference(input);
    switch (source->kind)
    {
    case ValueKind::Range:
    {
        const Cell from = source->items.size() > 0 ? source->items[0] : Cell();
        const Cell to = source->items.size() > 1 ? source->items[1] : Cell();
        if (!from)
        {
            panic("диапазон без начала нельзя перебрать");
        }
        if (from->kind == ValueKind::Char)
        {
            for (char32_t current = from->character;; ++current)
            {
                if (to && (current > to->character || (current == to->character && !source->inclusive)))
                {
                    return;
                }
                if (!visitor(charCell(current)))
                {
                    return;
                }
            }
        }
        const Number number = from->number != Number::Untyped ? from->number : (to ? to->number : Number::Untyped);
        WideInteger current = integerOf(from);
        const std::optional<WideInteger> last = to ? std::optional<WideInteger>(integerOf(to)) : std::nullopt;
        for (;;)
        {
            if (last)
            {
                const int order = compareIntegers(current, *last);
                if (order > 0 || (order == 0 && !source->inclusive))
                {
                    return;
                }
            }
            if (!visitor(integerCell(current, number)))
            {
                return;
            }
            const std::optional<WideInteger> following = addIntegers(current, WideInteger{false, 1});
            if (!following || (last && compareIntegers(current, *last) == 0))
            {
                return;
            }
            current = *following;
        }
    }
    case ValueKind::List:
        for (size_t index = 0; index < listLength(*source); ++index)
        {
            if (!visitor(referenceCell(listElement(*source, index))))
            {
                return;
            }
        }
        return;
    case ValueKind::Bytes:
    {
        const std::string_view bytes = textView(*source);
        for (const char byte : bytes)
        {
            if (!visitor(integerCell(unsignedInteger(static_cast<unsigned char>(byte)), Number::U8)))
            {
                return;
            }
        }
        return;
    }
    case ValueKind::Chars:
    {
        const std::string_view text = textView(*source);
        for (size_t position = 0; position < text.size();)
        {
            const unicode::Decoded decoded = unicode::decodeUtf8(text, position);
            position += decoded.length;
            if (!visitor(charCell(decoded.codePoint)))
            {
                return;
            }
        }
        return;
    }
    case ValueKind::Graphemes:
    {
        const std::string_view text = textView(*source);
        for (size_t position = 0; position < text.size();)
        {
            const size_t end = unicode::graphemeEnd(text, position);
            if (!visitor(viewCell(ValueKind::Str, source->text, source->start + position, end - position)))
            {
                return;
            }
            position = end;
        }
        return;
    }
    case ValueKind::Enumeration:
    {
        size_t number = 0;
        forEachElement(source->target, [&](const Cell& element)
        { return visitor(tupleCell({integerCell(unsignedInteger(number++), Number::Size), element})); });
        return;
    }
    case ValueKind::Struct:
    case ValueKind::Enum:
        if (const FunctionInfo* next = declarations.method(source->decl, "", "след"))
        {
            const Cell holder = referenceCell(source);
            for (;;)
            {
                const Cell item = dereference(invoke(*next, holder, {}));
                if (item->kind != ValueKind::Optional || item->items.empty())
                {
                    return;
                }
                if (!visitor(item->items.front()))
                {
                    return;
                }
            }
        }
        break;
    default:
        break;
    }
    panic("значение типа `" + valueKindName(*source) + "` нельзя перебрать в `цикл`");
}

bool Machine::matchPattern(const SyntaxNodeView& pattern, const Cell& input, bool viaReference)
{
    const bool reference = viaReference || input->kind == ValueKind::Reference;
    const Cell& value = dereference(input);
    switch (pattern.kind())
    {
    case WildcardPattern:
        return true;
    case BindingPattern:
        if (const std::optional<SyntaxToken> name = pattern.token(Identifier))
        {
            bindLocal(keyOf(*name), consume(value));
        }
        return true;
    case LiteralPattern:
    {
        const std::vector<SyntaxToken> tokens = pattern.tokens();
        size_t position = 0;
        const Cell low = patternLiteral(tokens, position);
        if (position < tokens.size() && (tokens[position].kind == DotDot || tokens[position].kind == DotDotEq))
        {
            const bool inclusive = tokens[position].kind == DotDotEq;
            ++position;
            const Cell high = patternLiteral(tokens, position);
            const int upper = compare(value, high);
            return compare(value, low) >= 0 && (inclusive ? upper <= 0 : upper < 0);
        }
        return equal(value, low);
    }
    case TuplePattern:
    {
        const std::vector<SyntaxNodeView> elements = pattern.nodes();
        if (!pattern.hasToken(Comma))
        {
            return elements.empty() ? value->kind == ValueKind::Void : matchPattern(elements.front(), value, reference);
        }
        if (value->kind != ValueKind::Tuple || value->items.size() != elements.size())
        {
            return false;
        }
        for (size_t index = 0; index < elements.size(); ++index)
        {
            if (!matchPattern(elements[index], value->items[index], reference))
            {
                return false;
            }
        }
        return true;
    }
    case OrPattern:
        for (const SyntaxNodeView& alternative : pattern.nodes())
        {
            if (matchPattern(alternative, value, reference))
            {
                return true;
            }
        }
        return false;
    case PathPattern:
        return matchPath(pattern, value, reference);
    default:
        panic("образец «" + std::string(kindName(pattern.kind())) + "» пока не исполняется интерпретатором");
    }
}

void Machine::bindPatternName(const SyntaxToken& name, const Cell& value, bool viaReference)
{
    bindLocal(keyOf(name), viaReference ? referenceCell(value) : consume(value));
}

bool Machine::matchPath(const SyntaxNodeView& pattern, const Cell& value, bool viaReference)
{
    std::vector<SyntaxToken> path;
    for (const SyntaxToken& token : pattern.tokens())
    {
        if (token.kind == Identifier)
        {
            path.push_back(token);
        }
    }
    if (path.empty())
    {
        return false;
    }
    const SyntaxNodeView arguments = pattern.node(PatternArgList);
    const SyntaxNodeView fields = pattern.node(PatternFieldList);
    if (path.size() == 1 && !arguments && !fields && !startsUppercase(path.front().text))
    {
        bindPatternName(path.front(), value, viaReference);
        return true;
    }
    const std::string key = keyOf(path.back());
    std::vector<Cell> payload;
    const TypeDecl* decl = nullptr;
    uint32_t variant = 0;
    if (value->kind == ValueKind::Optional)
    {
        if (key == NONE_KEY)
        {
            return value->items.empty();
        }
        if (key != SOME_KEY || value->items.empty())
        {
            return false;
        }
        payload = value->items;
    }
    else if (value->kind == ValueKind::Enum)
    {
        if (value->decl->variants[value->variant].key != key)
        {
            return false;
        }
        payload = value->items;
        decl = value->decl;
        variant = value->variant;
    }
    else if (value->kind == ValueKind::Struct && fields)
    {
        if (value->decl->key != key)
        {
            return false;
        }
        for (const SyntaxNodeView& field : fields.nodes())
        {
            const std::optional<SyntaxToken> name = field.token(Identifier);
            if (field.kind() != PatternField || !name)
            {
                continue;
            }
            const std::string fieldKey = keyOf(*name);
            const auto& declared = value->decl->fields;
            const auto found = std::find_if(declared.begin(), declared.end(),
                [&](const FieldDecl& candidate) { return candidate.key == fieldKey; });
            if (found == declared.end())
            {
                return false;
            }
            const Cell& item = value->items[static_cast<size_t>(found - declared.begin())];
            const std::vector<SyntaxNodeView> inner = field.nodes();
            if (inner.empty())
            {
                bindPatternName(*name, item, viaReference);
            }
            else if (!matchPattern(inner.front(), item, viaReference))
            {
                return false;
            }
        }
        return true;
    }
    else
    {
        SourceModule* module = frame().module;
        for (size_t index = 0; index + 1 < path.size(); ++index)
        {
            const auto [entry, owner] = lookup(module, keyOf(path[index]));
            if (entry == nullptr || entry->kind != EntryKind::Module)
            {
                return false;
            }
            module = entry->target;
        }
        const auto [entry, owner] = lookup(module, key);
        if (entry == nullptr || entry->kind != EntryKind::Const)
        {
            return false;
        }
        return equal(value, constant(*entry, owner));
    }
    if (arguments)
    {
        const std::vector<SyntaxNodeView> elements = arguments.nodes();
        if (elements.size() != payload.size())
        {
            return false;
        }
        for (size_t index = 0; index < elements.size(); ++index)
        {
            const std::vector<SyntaxNodeView> inner = elements[index].nodes();
            if (!inner.empty() && !matchPattern(inner.front(), payload[index], viaReference))
            {
                return false;
            }
        }
    }
    if (fields && decl != nullptr)
    {
        std::vector<std::string> names;
        if (const SyntaxNodeView list = decl->node.node(VariantList))
        {
            uint32_t position = 0;
            for (const SyntaxNodeView& declared : list.nodes())
            {
                if (declared.kind() != Variant)
                {
                    continue;
                }
                if (position++ == variant)
                {
                    for (const SyntaxNodeView& field : declared.nodes())
                    {
                        const std::optional<SyntaxToken> name = field.token(Identifier);
                        names.push_back(field.kind() == VariantField && name ? keyOf(*name) : std::string());
                    }
                }
            }
        }
        for (const SyntaxNodeView& field : fields.nodes())
        {
            const std::optional<SyntaxToken> name = field.token(Identifier);
            if (field.kind() != PatternField || !name)
            {
                continue;
            }
            const auto found = std::find(names.begin(), names.end(), keyOf(*name));
            if (found == names.end() || static_cast<size_t>(found - names.begin()) >= payload.size())
            {
                return false;
            }
            const Cell& item = payload[static_cast<size_t>(found - names.begin())];
            const std::vector<SyntaxNodeView> inner = field.nodes();
            if (inner.empty())
            {
                bindPatternName(*name, item, viaReference);
            }
            else if (!matchPattern(inner.front(), item, viaReference))
            {
                return false;
            }
        }
    }
    return true;
}

Cell Machine::patternLiteral(const std::vector<SyntaxToken>& tokens, size_t& position)
{
    const bool negative = position < tokens.size() && tokens[position].kind == Minus;
    position += negative ? 1 : 0;
    if (position >= tokens.size())
    {
        panic("образец-литерал оборван");
    }
    const Cell literalValue = literalToken(tokens[position++]);
    if (!negative)
    {
        return literalValue;
    }
    Cell negated = consume(literalValue);
    if (negated->kind == ValueKind::Integer)
    {
        negated->integer.negative = negated->integer.magnitude != 0;
    }
    else if (negated->kind == ValueKind::Float)
    {
        negated->floating = -negated->floating;
    }
    return negated;
}

Cell Machine::eval(const SyntaxNodeView& node)
{
    switch (node.kind())
    {
    case Literal:
        return literal(node);
    case NameExpr:
        return name(node);
    case FieldExpr:
        return field(node);
    case CallExpr:
        return call(node);
    case IndexExpr:
        return index(node);
    case BinaryExpr:
        return binary(node);
    case ComparisonExpr:
        return comparison(node);
    case LogicalExpr:
        return logical(node);
    case NotExpr:
        return boolCell(!truth(eval(node.child(0))));
    case MembershipExpr:
        return membership(node);
    case IsExpr:
    {
        const std::vector<SyntaxNodeView> parts = node.nodes();
        const Cell scrutinee = eval(parts.front());
        if (node.hasToken(KwNot))
        {
            const size_t level = frame().scopes.size();
            frame().scopes.emplace_back();
            const bool matched = matchPattern(parts[1], scrutinee, false);
            frame().scopes.resize(level);
            return boolCell(!matched);
        }
        return boolCell(matchPattern(parts[1], scrutinee, false));
    }
    case RangeExpr:
        return range(node);
    case CastExpr:
        return cast(node);
    case PrefixExpr:
        return prefix(node);
    case RefExpr:
    {
        const std::vector<SyntaxNodeView> parts = node.nodes();
        const SyntaxNodeView& operand = parts.back();
        const bool place = operand.kind() == NameExpr || operand.kind() == FieldExpr || operand.kind() == IndexExpr
            || operand.kind() == ParenExpr || operand.kind() == PrefixExpr;
        const Cell target = eval(operand);
        return referenceCell(place ? target : consume(target));
    }
    case ParenExpr:
    {
        const SyntaxNodeView inner = node.child(0);
        return inner ? eval(inner) : voidCell();
    }
    case TupleExpr:
    {
        std::vector<Cell> items;
        for (const SyntaxNodeView& element : node.nodes())
        {
            items.push_back(consume(eval(element)));
        }
        return tupleCell(std::move(items));
    }
    case CompoundLiteral:
        return compound(node);
    case MatchExpr:
        return match(node, nullptr);
    case FunctionExpr:
        return closureValue(node);
    case StructType:
    case EnumType:
        return synthesize(node);
    case TogetherExpr:
    {
        std::vector<Cell> items;
        if (const SyntaxNodeView arguments = node.node(ArgList))
        {
            for (const SyntaxNodeView& argument : arguments.nodes())
            {
                items.push_back(consume(eval(argument)));
            }
        }
        return tupleCell(std::move(items));
    }
    default:
        currentOffset = node.offset();
        panic("выражение «" + std::string(kindName(node.kind())) + "» пока не исполняется интерпретатором");
    }
}

Cell Machine::literal(const SyntaxNodeView& node)
{
    const std::optional<SyntaxToken> token = node.firstToken();
    if (!token)
    {
        return voidCell();
    }
    return literalToken(*token);
}

Cell Machine::literalToken(const SyntaxToken& token)
{
    if (const auto found = literals.find(token.text.data()); found != literals.end())
    {
        return found->second;
    }
    Cell value;
    switch (token.kind)
    {
    case SyntaxKind::Integer:
    {
        std::string digits;
        for (const char symbol : token.text)
        {
            if (symbol != '_')
            {
                digits += symbol;
            }
        }
        int base = 10;
        std::string_view body = digits;
        if (body.starts_with("0x"))
        {
            base = 16;
            body.remove_prefix(2);
        }
        else if (body.starts_with("0b"))
        {
            base = 2;
            body.remove_prefix(2);
        }
        uint64_t magnitude = 0;
        const std::from_chars_result parsed = std::from_chars(body.data(), body.data() + body.size(), magnitude, base);
        if (parsed.ec != std::errc())
        {
            currentOffset = token.offset;
            panic("целый литерал `" + std::string(token.text) + "` больше 2⁶⁴ − 1");
        }
        value = integerCell(unsignedInteger(magnitude), Number::Untyped);
        break;
    }
    case SyntaxKind::Float:
    {
        std::string digits;
        for (const char symbol : token.text)
        {
            if (symbol != '_')
            {
                digits += symbol;
            }
        }
        double parsed = 0;
        std::from_chars(digits.data(), digits.data() + digits.size(), parsed);
        value = floatCell(parsed, Number::Untyped);
        break;
    }
    case Char:
    {
        const std::string decoded = decodeEscapes(token.text.substr(1, token.text.size() - 2));
        value = charCell(unicode::decodeUtf8(decoded, 0).codePoint);
        break;
    }
    case Byte:
    {
        const size_t quote = token.text.find('\'');
        const std::string decoded = decodeEscapes(token.text.substr(quote + 1, token.text.size() - quote - 2));
        value =
            integerCell(unsignedInteger(static_cast<unsigned char>(decoded.empty() ? 0 : decoded.front())), Number::U8);
        break;
    }
    case String:
        value = stringCell(decodeEscapes(token.text.substr(1, token.text.size() - 2)));
        break;
    case RawString:
        value = stringCell(decodeRawString(token.text));
        break;
    case KwTrue:
        value = boolCell(true);
        break;
    case KwFalse:
        value = boolCell(false);
        break;
    default:
        currentOffset = token.offset;
        panic("литерал «" + std::string(token.text) + "» не поддерживается");
    }
    literals.emplace(token.text.data(), value);
    return value;
}

Cell Machine::name(const SyntaxNodeView& node)
{
    if (node.hasToken(KwSelf))
    {
        if (Cell* self = findLocal(std::string(SELF_KEY)))
        {
            return *self;
        }
        panic("`сам` вне метода");
    }
    if (node.hasToken(KwSelfType))
    {
        const FunctionInfo* function = frame().function;
        Value value;
        value.kind = ValueKind::Type;
        value.decl = function != nullptr ? function->owner : nullptr;
        value.name = function != nullptr ? function->builtinOwner : std::string();
        return makeCell(std::move(value));
    }
    const std::optional<SyntaxToken> token = node.token(Identifier);
    if (!token)
    {
        panic("имя без идентификатора");
    }
    const std::string& key = keyOf(*token);
    if (Cell* local = findLocal(key))
    {
        return *local;
    }
    const auto [entry, owner] = lookup(frame().module, key);
    if (entry != nullptr)
    {
        return entryValue(*entry, owner);
    }
    if (numberNamed(key)
        || std::find(BUILTIN_TYPE_NAMES.begin(), BUILTIN_TYPE_NAMES.end(), key) != BUILTIN_TYPE_NAMES.end())
    {
        Value value;
        value.kind = ValueKind::Type;
        value.name = key;
        return makeCell(std::move(value));
    }
    if (key == NONE_KEY)
    {
        return optionalCell(nullptr);
    }
    if (const auto variant = findVariant(key))
    {
        return construct(variant->first, variant->second, {});
    }
    currentOffset = token->offset;
    panic("имя `" + std::string(token->text) + "` не найдено");
}

Cell Machine::field(const SyntaxNodeView& node)
{
    const SyntaxNodeView receiverNode = node.child(0);
    const std::optional<SyntaxToken> member = node.token(Identifier);
    if (!receiverNode || !member)
    {
        panic("обращение к полю без имени");
    }
    const Cell receiver = eval(receiverNode);
    currentOffset = member->offset;
    return this->member(receiver, keyOf(*member), member->text);
}

Cell Machine::member(const Cell& receiver, const std::string& key, std::string_view display)
{
    const Cell& value = dereference(receiver);
    switch (value->kind)
    {
    case ValueKind::Module:
    {
        const auto [entry, owner] = lookup(value->module, key);
        if (entry == nullptr)
        {
            panic("в модуле `" + value->module->name + "` нет `" + std::string(display) + "`");
        }
        return entryValue(*entry, owner);
    }
    case ValueKind::Type:
    {
        if (value->decl != nullptr && value->decl->kind == DeclKind::Enum)
        {
            for (uint32_t index = 0; index < value->decl->variants.size(); ++index)
            {
                if (value->decl->variants[index].key == key)
                {
                    return construct(value->decl, index, {});
                }
            }
        }
        if (const FunctionInfo* function = declarations.method(value->decl, value->name, key))
        {
            Value result;
            result.kind = ValueKind::Function;
            result.function = function;
            return makeCell(std::move(result));
        }
        break;
    }
    case ValueKind::Struct:
    {
        const auto& fields = value->decl->fields;
        for (size_t index = 0; index < fields.size(); ++index)
        {
            if (fields[index].key == key)
            {
                return value->items[index];
            }
        }
        if (value->decl == uniqueType && !value->items.empty())
        {
            return member(value->items.front(), key, display);
        }
        break;
    }
    case ValueKind::Range:
        if (key == "от")
        {
            return value->items[0] ? value->items[0] : voidCell();
        }
        if (key == "до")
        {
            return value->items[1] ? value->items[1] : voidCell();
        }
        break;
    case ValueKind::List:
        if (key == "длина")
        {
            return integerCell(unsignedInteger(listLength(*value)), Number::Size);
        }
        break;
    case ValueKind::Bytes:
        if (key == "длина")
        {
            return integerCell(unsignedInteger(value->length), Number::Size);
        }
        break;
    case ValueKind::Vector:
        for (size_t component = 0; component < VECTOR_COMPONENTS.size() && component < value->items.size(); ++component)
        {
            if (key == VECTOR_COMPONENTS[component])
            {
                return value->items[component];
            }
        }
        break;
    default:
        break;
    }
    panic("у значения типа `" + valueKindName(*value) + "` нет поля `" + std::string(display) + "`");
}

Cell Machine::call(const SyntaxNodeView& node)
{
    const SyntaxNodeView argumentList = node.node(ArgList);
    const std::vector<SyntaxNodeView> argumentNodes =
        argumentList ? argumentList.nodes() : std::vector<SyntaxNodeView>();
    const auto evaluateArguments = [&]
    {
        std::vector<Cell> values;
        values.reserve(argumentNodes.size());
        for (const SyntaxNodeView& argument : argumentNodes)
        {
            values.push_back(eval(argument));
        }
        return values;
    };
    const SyntaxNodeView callee = node.child(0);
    if (callee.kind() == NameExpr && callee.token(Identifier))
    {
        const SyntaxToken token = *callee.token(Identifier);
        const std::string key = keyOf(token);
        if (findLocal(key) == nullptr)
        {
            const auto [entry, owner] = lookup(frame().module, key);
            if (entry == nullptr)
            {
                if (key == SOME_KEY)
                {
                    std::vector<Cell> values = evaluateArguments();
                    return optionalCell(values.empty() ? voidCell() : consume(values.front()));
                }
                if (key == ASSERT_KEY)
                {
                    std::vector<Cell> values = evaluateArguments();
                    currentOffset = node.offset();
                    if (values.empty() || !truth(values.front()))
                    {
                        panic("утверждение не выполнено");
                    }
                    return voidCell();
                }
                if (key == VECTOR_KEY)
                {
                    return vectorType(node, argumentNodes);
                }
                if (const auto variant = findVariant(key))
                {
                    return construct(variant->first, variant->second, evaluateArguments());
                }
                currentOffset = token.offset;
                panic("имя `" + std::string(token.text) + "` не найдено");
            }
            if (entry->kind == EntryKind::Function)
            {
                std::vector<Cell> values = evaluateArguments();
                currentOffset = node.offset();
                return invoke(*entry->function, nullptr, std::move(values));
            }
            if (entry->kind == EntryKind::Type)
            {
                return entryValue(*entry, owner);
            }
        }
    }
    if (callee.kind() == FieldExpr && callee.token(Identifier))
    {
        const SyntaxToken token = *callee.token(Identifier);
        const std::string key = keyOf(token);
        const Cell receiver = eval(callee.child(0));
        const Cell& target = dereference(receiver);
        if (target->kind == ValueKind::Module)
        {
            const auto [entry, owner] = lookup(target->module, key);
            if (entry != nullptr && entry->kind == EntryKind::Function)
            {
                std::vector<Cell> values = evaluateArguments();
                currentOffset = node.offset();
                return invoke(*entry->function, nullptr, std::move(values));
            }
            if (entry != nullptr && entry->kind == EntryKind::Type)
            {
                return entryValue(*entry, owner);
            }
            currentOffset = token.offset;
            panic("в модуле `" + target->module->name + "` нет функции `" + std::string(token.text) + "`");
        }
        if (target->kind == ValueKind::Type)
        {
            if (target->decl != nullptr && target->decl->kind == DeclKind::Enum)
            {
                for (uint32_t index = 0; index < target->decl->variants.size(); ++index)
                {
                    if (target->decl->variants[index].key == key)
                    {
                        return construct(target->decl, index, evaluateArguments());
                    }
                }
            }
            if (const FunctionInfo* function = declarations.method(target->decl, target->name, key))
            {
                std::vector<Cell> values = evaluateArguments();
                currentOffset = node.offset();
                return invoke(*function, nullptr, std::move(values));
            }
            currentOffset = token.offset;
            panic("у типа нет функции `" + std::string(token.text) + "`");
        }
        std::vector<Cell> values = evaluateArguments();
        currentOffset = token.offset;
        return callMethod(receiver, key, token.text, std::move(values));
    }
    const Cell function = dereference(eval(callee));
    std::vector<Cell> values = evaluateArguments();
    currentOffset = node.offset();
    if (function->kind == ValueKind::Function && function->function != nullptr)
    {
        return invoke(*function->function, nullptr, std::move(values));
    }
    if (function->kind == ValueKind::Closure)
    {
        return callClosure(*function->closure, std::move(values));
    }
    if (function->kind == ValueKind::Type)
    {
        return function;
    }
    panic("значение типа `" + valueKindName(*function) + "` нельзя вызвать");
}

Cell Machine::callMethod(
    const Cell& receiver, const std::string& key, std::string_view display, std::vector<Cell> arguments)
{
    const Cell& target = dereference(receiver);
    if (const FunctionInfo* method = methodFor(*target, key))
    {
        return invoke(*method, receiver, std::move(arguments));
    }
    if (target->kind == ValueKind::Struct && target->decl == uniqueType && !target->items.empty())
    {
        return callMethod(target->items.front(), key, display, std::move(arguments));
    }
    if (target->kind == ValueKind::Struct)
    {
        const auto& fields = target->decl->fields;
        for (size_t position = 0; position < fields.size(); ++position)
        {
            const Cell& stored = dereference(target->items[position]);
            if (fields[position].key == key && stored->kind == ValueKind::Closure)
            {
                return callClosure(*stored->closure, std::move(arguments));
            }
            if (fields[position].key == key && stored->kind == ValueKind::Function && stored->function != nullptr)
            {
                return invoke(*stored->function, nullptr, std::move(arguments));
            }
        }
    }
    bool handled = false;
    Cell result = nativeMethod(receiver, key, arguments, handled);
    if (handled)
    {
        return result;
    }
    panic("у значения типа `" + valueKindName(*target) + "` нет метода `" + std::string(display) + "`");
}

Cell Machine::nativeMethod(const Cell& receiver, const std::string& key, std::vector<Cell>& arguments, bool& handled)
{
    const Cell& target = dereference(receiver);
    handled = true;
    if ((target->kind == ValueKind::List || target->kind == ValueKind::Bytes) && key == "перечислить")
    {
        Value value;
        value.kind = ValueKind::Enumeration;
        value.target = target;
        return makeCell(std::move(value));
    }
    if ((target->kind == ValueKind::List || target->kind == ValueKind::Bytes) && key == "разделить"
        && arguments.size() == 1)
    {
        const size_t length = target->kind == ValueKind::List ? listLength(*target) : target->length;
        const size_t middle = sizeOf(arguments.front());
        if (middle > length)
        {
            panic("граница " + std::to_string(middle) + " вне среза длины " + std::to_string(length));
        }
        Cell head = copyCell(target);
        Cell tail = copyCell(target);
        head->growable = false;
        tail->growable = false;
        head->length = middle;
        tail->start += middle;
        tail->length = length - middle;
        return tupleCell({head, tail});
    }
    handled = false;
    return voidCell();
}

Cell Machine::index(const SyntaxNodeView& node)
{
    const SyntaxNodeView receiverNode = node.child(0);
    const SyntaxNodeView positionNode = node.child(1);
    if (!positionNode)
    {
        panic("индекс без значения");
    }
    const Cell receiver = dereference(eval(receiverNode));
    const Cell position = eval(positionNode);
    currentOffset = node.offset();
    const Cell& bound = dereference(position);
    const size_t length = receiver->kind == ValueKind::List ? listLength(*receiver)
        : receiver->kind == ValueKind::Bytes                ? receiver->length
                                                            : 0;
    if (receiver->kind != ValueKind::List && receiver->kind != ValueKind::Bytes)
    {
        panic("значение типа `" + valueKindName(*receiver) + "` не индексируют");
    }
    if (bound->kind == ValueKind::Range)
    {
        const auto [from, to] = sliceBounds(*bound, length);
        Cell view = copyCell(receiver);
        view->growable = false;
        view->start = receiver->start + from;
        view->length = to - from;
        return view;
    }
    const size_t offset = sizeOf(bound);
    if (offset >= length)
    {
        panic("индекс " + std::to_string(offset) + " вне среза длины " + std::to_string(length));
    }
    if (receiver->kind == ValueKind::List)
    {
        return listElement(*receiver, offset);
    }
    return integerCell(
        unsignedInteger(static_cast<unsigned char>((*receiver->text)[receiver->start + offset])), Number::U8);
}

std::pair<size_t, size_t> Machine::sliceBounds(const Value& bounds, size_t length) const
{
    const size_t from = bounds.items[0] ? sizeOf(bounds.items[0]) : 0;
    size_t to = length;
    if (bounds.items[1])
    {
        to = sizeOf(bounds.items[1]) + (bounds.inclusive ? 1 : 0);
    }
    if (from > to || to > length)
    {
        panic("границы " + std::to_string(from) + ".." + std::to_string(to) + " вне среза длины "
            + std::to_string(length));
    }
    return {from, to};
}

Cell Machine::binary(const SyntaxNodeView& node)
{
    Cell accumulator;
    std::optional<SyntaxToken> pending;
    node.forEachChild([&](const SyntaxNodeView& child, const std::optional<SyntaxToken>& token)
    {
        if (token)
        {
            pending = token;
            return;
        }
        const Cell operand = eval(child);
        if (!accumulator)
        {
            accumulator = operand;
            return;
        }
        currentOffset = pending->offset;
        accumulator = arithmetic(pending->kind, accumulator, operand);
    });
    return accumulator;
}

Cell Machine::arithmetic(SyntaxKind operation, const Cell& leftCell, const Cell& rightCell)
{
    if (operation == PlusQuestion || operation == MinusQuestion || operation == StarQuestion)
    {
        return checkedArithmetic(operation, leftCell, rightCell);
    }
    const Value& left = *dereference(leftCell);
    const Value& right = *dereference(rightCell);
    if (left.kind == ValueKind::Struct || left.kind == ValueKind::Enum)
    {
        const std::string_view traitMethod = operation == Plus ? "сложить"
            : operation == Minus                               ? "вычесть"
            : operation == Star                                ? "умножить"
            : operation == Slash                               ? "разделить"
            : operation == Percent                             ? "остаток"
                                                               : "";
        if (const FunctionInfo* method = traitMethod.empty() ? nullptr : methodFor(left, std::string(traitMethod)))
        {
            return invoke(*method, leftCell, {rightCell});
        }
    }
    if (left.kind == ValueKind::Vector || right.kind == ValueKind::Vector)
    {
        return vectorArithmetic(operation, leftCell, rightCell);
    }
    const auto symbol = [operation] { return std::string(fixedText(operation)); };
    if (left.kind == ValueKind::Integer && right.kind == ValueKind::Integer)
    {
        const bool shift = operation == Shl || operation == Shr;
        const Number number = shift || left.number != Number::Untyped ? left.number : right.number;
        const auto overflow = [&]
        {
            panic("переполнение `" + std::string(numberName(number)) + "`: " + integerText(left.integer) + " "
                + symbol() + " " + integerText(right.integer));
        };
        std::optional<WideInteger> result;
        switch (operation)
        {
        case Plus:
            result = addIntegers(left.integer, right.integer);
            break;
        case Minus:
            result = subtractIntegers(left.integer, right.integer);
            break;
        case Star:
            result = multiplyIntegers(left.integer, right.integer);
            break;
        case Slash:
        case Percent:
            if (right.integer.magnitude == 0)
            {
                panic("деление на ноль");
            }
            result = operation == Slash ? divideIntegers(left.integer, right.integer)
                                        : remainderIntegers(left.integer, right.integer);
            break;
        case PlusPercent:
        case MinusPercent:
        case StarPercent:
        case Amp:
        case KwBitOr:
        case KwBitXor:
        {
            const uint64_t a = integerBits(left.integer, number);
            const uint64_t b = integerBits(right.integer, number);
            uint64_t bits = 0;
            switch (operation)
            {
            case PlusPercent:
                bits = a + b;
                break;
            case MinusPercent:
                bits = a - b;
                break;
            case StarPercent:
                bits = a * b;
                break;
            case Amp:
                bits = a & b;
                break;
            case KwBitOr:
                bits = a | b;
                break;
            default:
                bits = a ^ b;
                break;
            }
            return integerCell(integerFromBits(bits, number), number);
        }
        case Shl:
        case Shr:
        {
            const unsigned width = numberBits(number);
            if (right.integer.negative || right.integer.magnitude >= width)
            {
                panic("сдвиг на " + integerText(right.integer) + " бит для `" + std::string(numberName(number)) + "`");
            }
            const unsigned amount = static_cast<unsigned>(right.integer.magnitude);
            const uint64_t bits = integerBits(left.integer, number);
            if (operation == Shl)
            {
                return integerCell(integerFromBits(bits << amount, number), number);
            }
            if (numberSigned(number))
            {
                const int64_t extended = static_cast<int64_t>(signExtended(bits, width));
                return integerCell(integerFromBits(static_cast<uint64_t>(extended >> amount), number), number);
            }
            return integerCell(integerFromBits(bits >> amount, number), number);
        }
        default:
            panic("операция `" + symbol() + "` не определена для целых");
        }
        if (!result || !fits(*result, number))
        {
            overflow();
        }
        return integerCell(*result, number);
    }
    const bool numeric = (left.kind == ValueKind::Float || left.kind == ValueKind::Integer)
        && (right.kind == ValueKind::Float || right.kind == ValueKind::Integer);
    if (numeric)
    {
        const double a = left.kind == ValueKind::Float ? left.floating : integerToDouble(left.integer);
        const double b = right.kind == ValueKind::Float ? right.floating : integerToDouble(right.integer);
        const Number number = left.number == Number::F32 || right.number == Number::F32 ? Number::F32 : Number::F64;
        switch (operation)
        {
        case Plus:
            return floatCell(a + b, number);
        case Minus:
            return floatCell(a - b, number);
        case Star:
            return floatCell(a * b, number);
        case Slash:
            return floatCell(a / b, number);
        case Percent:
            return floatCell(std::fmod(a, b), number);
        default:
            break;
        }
    }
    panic(
        "операция `" + symbol() + "` не определена для `" + valueKindName(left) + "` и `" + valueKindName(right) + "`");
}

Cell Machine::vectorArithmetic(SyntaxKind operation, const Cell& leftCell, const Cell& rightCell)
{
    const Cell left = dereference(leftCell);
    const Cell right = dereference(rightCell);
    const Cell& shape = left->kind == ValueKind::Vector ? left : right;
    if (left->kind == ValueKind::Vector && right->kind == ValueKind::Vector
        && left->items.size() != right->items.size())
    {
        panic("векторы разной длины: " + std::to_string(left->items.size()) + " и "
            + std::to_string(right->items.size()));
    }
    if (left->kind != ValueKind::Vector && operation != Star)
    {
        panic("скаляр слева от вектора допустим только в умножении");
    }
    Value result;
    result.kind = ValueKind::Vector;
    result.number = shape->number;
    for (size_t component = 0; component < shape->items.size(); ++component)
    {
        const Cell& a = left->kind == ValueKind::Vector ? left->items[component] : left;
        const Cell& b = right->kind == ValueKind::Vector ? right->items[component] : right;
        result.items.push_back(arithmetic(operation, a, b));
    }
    return makeCell(std::move(result));
}

Cell Machine::vectorType(const SyntaxNodeView& node, const std::vector<SyntaxNodeView>& arguments)
{
    currentOffset = node.offset();
    if (arguments.size() != 2)
    {
        panic("`Вектор` ждёт тип элемента и длину");
    }
    const std::optional<Number> number = resolveNumber(arguments[0], frame().module);
    if (!number)
    {
        panic("элемент `Вектор` — число");
    }
    Value value;
    value.kind = ValueKind::Type;
    value.name = std::string(VECTOR_KEY);
    value.number = *number;
    value.length = sizeOf(eval(arguments[1]));
    return makeCell(std::move(value));
}

Cell Machine::checkedArithmetic(SyntaxKind operation, const Cell& leftCell, const Cell& rightCell)
{
    const Cell* left = &dereference(leftCell);
    const Cell* right = &dereference(rightCell);
    for (const Cell** operand : {&left, &right})
    {
        if ((**operand)->kind == ValueKind::Optional)
        {
            if ((**operand)->items.empty())
            {
                return optionalCell(nullptr);
            }
            *operand = &dereference((**operand)->items.front());
        }
    }
    if ((*left)->kind != ValueKind::Integer || (*right)->kind != ValueKind::Integer)
    {
        panic("проверяемая арифметика определена только для целых");
    }
    const Number number = (*left)->number != Number::Untyped ? (*left)->number : (*right)->number;
    std::optional<WideInteger> result;
    switch (operation)
    {
    case PlusQuestion:
        result = addIntegers((*left)->integer, (*right)->integer);
        break;
    case MinusQuestion:
        result = subtractIntegers((*left)->integer, (*right)->integer);
        break;
    default:
        result = multiplyIntegers((*left)->integer, (*right)->integer);
        break;
    }
    if (!result || !fits(*result, number))
    {
        return optionalCell(nullptr);
    }
    return optionalCell(integerCell(*result, number));
}

Cell Machine::comparison(const SyntaxNodeView& node)
{
    SyntaxKind operation = EqEq;
    for (const GreenElement& element : node.green()->children())
    {
        if (element.node() == nullptr && isComparisonOperator(element.kind()))
        {
            operation = element.kind();
            break;
        }
    }
    const Cell left = eval(node.child(0));
    const Cell right = eval(node.child(1));
    currentOffset = node.offset();
    switch (operation)
    {
    case EqEq:
        return boolCell(equal(left, right));
    case NotEq:
        return boolCell(!equal(left, right));
    case Less:
        return boolCell(compare(left, right) < 0);
    case LessEq:
        return boolCell(compare(left, right) <= 0);
    case Greater:
        return boolCell(compare(left, right) > 0);
    default:
        return boolCell(compare(left, right) >= 0);
    }
}

Cell Machine::logical(const SyntaxNodeView& node)
{
    std::optional<bool> accumulator;
    std::optional<SyntaxKind> pending;
    bool decided = false;
    node.forEachChild([&](const SyntaxNodeView& child, const std::optional<SyntaxToken>& token)
    {
        if (token)
        {
            if (token->kind == KwAnd || token->kind == KwOr)
            {
                pending = token->kind;
            }
            return;
        }
        if (decided)
        {
            return;
        }
        if (accumulator && pending)
        {
            if ((*pending == KwAnd && !*accumulator) || (*pending == KwOr && *accumulator))
            {
                decided = true;
                return;
            }
        }
        accumulator = truth(eval(child));
    });
    return boolCell(accumulator.value_or(false));
}

Cell Machine::membership(const SyntaxNodeView& node)
{
    const std::vector<SyntaxNodeView> parts = node.nodes();
    const Cell value = eval(parts[0]);
    const Cell collection = dereference(eval(parts[1]));
    currentOffset = node.offset();
    bool found = false;
    switch (collection->kind)
    {
    case ValueKind::Range:
        found = inRange(*collection, value);
        break;
    case ValueKind::List:
        for (size_t position = 0; position < listLength(*collection) && !found; ++position)
        {
            found = equal(value, listElement(*collection, position));
        }
        break;
    case ValueKind::Bytes:
    {
        const Value& item = *dereference(value);
        if (item.kind == ValueKind::Integer && !item.integer.negative && item.integer.magnitude < 256)
        {
            found = textView(*collection).find(static_cast<char>(item.integer.magnitude)) != std::string_view::npos;
        }
        break;
    }
    default:
        panic("`в` не определено для `" + valueKindName(*collection) + "`");
    }
    return boolCell(node.hasToken(KwNot) ? !found : found);
}

bool Machine::inRange(const Value& bounds, const Cell& value) const
{
    if (bounds.items[0] && compare(value, bounds.items[0]) < 0)
    {
        return false;
    }
    if (bounds.items[1])
    {
        const int order = compare(value, bounds.items[1]);
        return order < 0 || (order == 0 && bounds.inclusive);
    }
    return true;
}

Cell Machine::range(const SyntaxNodeView& node)
{
    Cell from;
    Cell to;
    bool afterOperator = false;
    node.forEachChild([&](const SyntaxNodeView& child, const std::optional<SyntaxToken>& token)
    {
        if (token)
        {
            afterOperator = afterOperator || token->kind == DotDot || token->kind == DotDotEq;
            return;
        }
        (afterOperator ? to : from) = consume(dereference(eval(child)));
    });
    if (from && to && from->kind == ValueKind::Integer && to->kind == ValueKind::Integer)
    {
        if (from->number == Number::Untyped)
        {
            from->number = to->number;
        }
        else if (to->number == Number::Untyped)
        {
            to->number = from->number;
        }
    }
    Value value;
    value.kind = ValueKind::Range;
    value.inclusive = node.hasToken(DotDotEq);
    value.items = {from, to};
    return makeCell(std::move(value));
}

Cell Machine::cast(const SyntaxNodeView& node)
{
    const std::vector<SyntaxNodeView> parts = node.nodes();
    const Cell value = dereference(eval(parts.front()));
    const SyntaxNodeView& type = parts.back();
    currentOffset = node.offset();
    const std::optional<Number> number = resolveNumber(type, frame().module);
    if (number)
    {
        const std::string target(numberName(*number));
        WideInteger integer;
        switch (value->kind)
        {
        case ValueKind::Integer:
            if (isFloatNumber(*number))
            {
                return floatCell(integerToDouble(value->integer), *number);
            }
            integer = value->integer;
            break;
        case ValueKind::Float:
        {
            if (isFloatNumber(*number))
            {
                return floatCell(value->floating, *number);
            }
            const double truncated = std::trunc(value->floating);
            if (!std::isfinite(truncated) || std::fabs(truncated) >= 18446744073709551616.0)
            {
                panic("`как " + target + "`: " + floatText(value->floating, value->number) + " вне диапазона");
            }
            integer = WideInteger{truncated < 0, static_cast<uint64_t>(std::fabs(truncated))};
            integer.negative = integer.negative && integer.magnitude != 0;
            break;
        }
        case ValueKind::Char:
            integer = unsignedInteger(value->character);
            break;
        case ValueKind::Bool:
            integer = unsignedInteger(value->truth ? 1 : 0);
            break;
        default:
            panic("`как " + target + "` не определено для `" + valueKindName(*value) + "`");
        }
        if (!fits(integer, *number))
        {
            panic("`как " + target + "`: значение " + integerText(integer) + " не помещается");
        }
        return integerCell(integer, *number);
    }
    if (type.kind() == NameExpr && type.token(Identifier) && nameKey(type.token(Identifier)->text) == "сим")
    {
        if (value->kind == ValueKind::Char)
        {
            return value;
        }
        const WideInteger code = integerOf(value);
        if (code.negative || code.magnitude > 0x10FFFF || (code.magnitude >= 0xD800 && code.magnitude <= 0xDFFF))
        {
            panic("`как сим`: " + integerText(code) + " — не скалярное значение Юникода");
        }
        return charCell(static_cast<char32_t>(code.magnitude));
    }
    return value;
}

Cell Machine::prefix(const SyntaxNodeView& node)
{
    const std::vector<SyntaxNodeView> parts = node.nodes();
    const std::optional<SyntaxToken> operation = node.firstToken();
    if (parts.empty() || !operation)
    {
        panic("префиксное выражение без операнда");
    }
    switch (operation->kind)
    {
    case KwTry:
        return attempt(parts.front());
    case KwUnsafe:
    case KwComptime:
        return eval(parts.front());
    case Star:
        return dereference(eval(parts.front()));
    case Minus:
    {
        const Cell value = dereference(eval(parts.front()));
        currentOffset = node.offset();
        if (value->kind == ValueKind::Integer)
        {
            WideInteger negated = value->integer;
            negated.negative = !negated.negative && negated.magnitude != 0;
            if (!fits(negated, value->number))
            {
                panic("переполнение `" + std::string(numberName(value->number)) + "`: -" + integerText(value->integer));
            }
            return integerCell(negated, value->number);
        }
        if (value->kind == ValueKind::Float)
        {
            return floatCell(-value->floating, value->number);
        }
        panic("унарный минус не определён для `" + valueKindName(*value) + "`");
    }
    case KwBitNot:
    {
        const Cell value = dereference(eval(parts.front()));
        const WideInteger integer = integerOf(value);
        return integerCell(integerFromBits(~integerBits(integer, value->number), value->number), value->number);
    }
    case KwNot:
        return boolCell(!truth(eval(parts.front())));
    default:
        panic("префикс «" + std::string(operation->text) + "» пока не исполняется интерпретатором");
    }
}

Cell Machine::attempt(const SyntaxNodeView& operand)
{
    const Cell result = eval(operand);
    const Cell& value = dereference(result);
    if (value->kind == ValueKind::Enum && value->decl == resultType)
    {
        if (value->decl->variants[value->variant].key == SUCCESS_KEY)
        {
            return value->items.empty() ? voidCell() : value->items.front();
        }
        frame().returned = failure(wrapError(value->items.empty() ? voidCell() : value->items.front()));
        throw Escape{Flow::Return};
    }
    if (value->kind == ValueKind::Optional)
    {
        if (!value->items.empty())
        {
            return value->items.front();
        }
        frame().returned = optionalCell(nullptr);
        throw Escape{Flow::Return};
    }
    panic("`попытка` ждёт `Итог` или `?Т`, а получила `" + valueKindName(*value) + "`");
}

Cell Machine::compound(const SyntaxNodeView& node)
{
    const std::vector<SyntaxNodeView> parts = node.nodes();
    const SyntaxNodeView& type = parts.front();
    SourceModule* module = frame().module;
    if (type.kind() == ArrayType)
    {
        const std::vector<SyntaxNodeView> typeParts = type.nodes();
        const SyntaxNodeView element = typeParts.empty() ? SyntaxNodeView() : typeParts.back();
        std::vector<Cell> items;
        for (size_t position = 1; position < parts.size(); ++position)
        {
            const std::vector<SyntaxNodeView> values = parts[position].nodes();
            if (values.empty())
            {
                continue;
            }
            Cell item = consume(eval(values.front()));
            coerce(item, element, module);
            items.push_back(std::move(item));
        }
        return listCell(std::move(items), false);
    }
    const TypeDecl* decl = resolveDecl(type, module);
    if (decl == nullptr)
    {
        const Cell alias = dereference(eval(type));
        if (alias->kind == ValueKind::Type && alias->name == VECTOR_KEY)
        {
            Value vector;
            vector.kind = ValueKind::Vector;
            vector.number = alias->number;
            for (size_t position = 1; position < parts.size(); ++position)
            {
                const std::vector<SyntaxNodeView> values = parts[position].nodes();
                if (values.empty())
                {
                    continue;
                }
                Cell item = consume(eval(values.front()));
                if (item->number == Number::Untyped
                    && (item->kind == ValueKind::Float || item->kind == ValueKind::Integer))
                {
                    item = isFloatNumber(alias->number)
                        ? floatCell(item->kind == ValueKind::Float ? item->floating : integerToDouble(item->integer),
                              alias->number)
                        : integerCell(item->integer, alias->number);
                }
                vector.items.push_back(std::move(item));
            }
            if (vector.items.size() != alias->length)
            {
                currentOffset = node.offset();
                panic("в литерале `Вектор` " + std::to_string(vector.items.size()) + " компонент вместо "
                    + std::to_string(alias->length));
            }
            return makeCell(std::move(vector));
        }
        decl = alias->kind == ValueKind::Type ? alias->decl : nullptr;
    }
    std::vector<Local> bindings;
    if (const auto known = typeArguments.find(decl); known != typeArguments.end())
    {
        bindings = known->second;
    }
    else if (decl != nullptr && type.kind() == CallExpr && !decl->parameters.empty())
    {
        if (const SyntaxNodeView arguments = type.node(ArgList))
        {
            size_t position = 0;
            for (const SyntaxNodeView& argument : arguments.nodes())
            {
                if (position < decl->parameters.size())
                {
                    bindings.push_back(Local{decl->parameters[position++], eval(argument)});
                }
            }
        }
    }
    if (decl == nullptr || decl->kind != DeclKind::Struct)
    {
        currentOffset = type.offset();
        panic("тип литерала не найден");
    }
    const auto& fields = decl->fields;
    const auto fieldIndex = [&](const std::string& key)
    {
        const auto found = std::find_if(
            fields.begin(), fields.end(), [&](const FieldDecl& candidate) { return candidate.key == key; });
        return found == fields.end() ? std::optional<size_t>() : std::optional<size_t>(found - fields.begin());
    };
    std::vector<Cell> items(fields.size());
    size_t nextPositional = 0;
    for (size_t position = 1; position < parts.size(); ++position)
    {
        const SyntaxNodeView& element = parts[position];
        const std::vector<SyntaxNodeView> values = element.nodes();
        if (element.kind() != LiteralElement || values.empty())
        {
            continue;
        }
        std::optional<size_t> target;
        if (const std::optional<SyntaxToken> label = element.token(Identifier))
        {
            target = fieldIndex(keyOf(*label));
            if (!target)
            {
                panic("у `" + decl->display + "` нет поля `" + std::string(label->text) + "`");
            }
        }
        else if (values.front().kind() == NameExpr && values.front().token(Identifier))
        {
            target = fieldIndex(keyOf(*values.front().token(Identifier)));
        }
        if (!target)
        {
            while (nextPositional < items.size() && items[nextPositional])
            {
                ++nextPositional;
            }
            if (nextPositional >= items.size())
            {
                panic("в литерале `" + decl->display + "` больше значений, чем полей");
            }
            target = nextPositional;
        }
        Cell item = consume(eval(values.front()));
        coerce(item, fields[*target].type, decl->module, &bindings);
        items[*target] = std::move(item);
    }
    for (Cell& item : items)
    {
        if (!item)
        {
            item = voidCell();
        }
    }
    if (decl == rangeType && items.size() == 2)
    {
        Value value;
        value.kind = ValueKind::Range;
        value.items = std::move(items);
        return makeCell(std::move(value));
    }
    return structValue(decl, std::move(items));
}

Cell Machine::match(const SyntaxNodeView& node, Flow* flowOut)
{
    const std::vector<SyntaxNodeView> parts = node.nodes();
    const Cell scrutinee = eval(parts.front());
    for (size_t position = 1; position < parts.size(); ++position)
    {
        const SyntaxNodeView& arm = parts[position];
        if (arm.kind() != MatchArm)
        {
            continue;
        }
        const std::vector<SyntaxNodeView> pieces = arm.nodes();
        ScopeGuard guard(frame());
        if (!matchPattern(pieces.front(), scrutinee, false))
        {
            continue;
        }
        if (arm.hasToken(KwIf) && pieces.size() > 2 && !truth(eval(pieces[1])))
        {
            continue;
        }
        const SyntaxNodeView& body = pieces.back();
        Flow flow = Flow::Normal;
        Cell value = voidCell();
        if (body.kind() == ExprStmt)
        {
            const std::vector<SyntaxNodeView> inner = body.nodes();
            if (!inner.empty())
            {
                value = inner.front().kind() == MatchExpr ? match(inner.front(), &flow) : eval(inner.front());
            }
        }
        else if (isStatementKind(body.kind()))
        {
            flow = exec(body);
        }
        else
        {
            value = eval(body);
        }
        if (flow != Flow::Normal)
        {
            if (flowOut != nullptr)
            {
                *flowOut = flow;
                return voidCell();
            }
            throw Escape{flow};
        }
        return value;
    }
    currentOffset = node.offset();
    panic("ни одна ветвь `выбор` не подошла");
}

int Machine::compare(const Cell& leftCell, const Cell& rightCell) const
{
    const Value& left = *dereference(leftCell);
    const Value& right = *dereference(rightCell);
    if (left.kind == ValueKind::Integer && right.kind == ValueKind::Integer)
    {
        return compareIntegers(left.integer, right.integer);
    }
    const bool numeric = (left.kind == ValueKind::Float || left.kind == ValueKind::Integer)
        && (right.kind == ValueKind::Float || right.kind == ValueKind::Integer);
    if (numeric)
    {
        const double a = left.kind == ValueKind::Float ? left.floating : integerToDouble(left.integer);
        const double b = right.kind == ValueKind::Float ? right.floating : integerToDouble(right.integer);
        return a < b ? -1 : (a > b ? 1 : 0);
    }
    if (left.kind == ValueKind::Char && right.kind == ValueKind::Char)
    {
        return left.character < right.character ? -1 : (left.character > right.character ? 1 : 0);
    }
    if ((left.kind == ValueKind::Str || left.kind == ValueKind::Bytes) && left.kind == right.kind)
    {
        const int order = textView(left).compare(textView(right));
        return order < 0 ? -1 : (order > 0 ? 1 : 0);
    }
    if (left.kind == ValueKind::Bool && right.kind == ValueKind::Bool)
    {
        return static_cast<int>(left.truth) - static_cast<int>(right.truth);
    }
    const auto items = [this](const std::vector<Cell>& a, const std::vector<Cell>& b)
    {
        for (size_t position = 0; position < a.size() && position < b.size(); ++position)
        {
            if (const int order = compare(a[position], b[position]); order != 0)
            {
                return order;
            }
        }
        return a.size() < b.size() ? -1 : (a.size() > b.size() ? 1 : 0);
    };
    if (left.kind == right.kind
        && (left.kind == ValueKind::Struct || left.kind == ValueKind::Tuple || left.kind == ValueKind::Vector)
        && left.decl == right.decl)
    {
        return items(left.items, right.items);
    }
    if (left.kind == ValueKind::Enum && right.kind == ValueKind::Enum && left.decl == right.decl)
    {
        if (left.variant != right.variant)
        {
            return left.variant < right.variant ? -1 : 1;
        }
        return items(left.items, right.items);
    }
    if (left.kind == ValueKind::Optional && right.kind == ValueKind::Optional)
    {
        return items(left.items, right.items);
    }
    if (left.kind == ValueKind::List && right.kind == ValueKind::List)
    {
        const size_t length = std::min(listLength(left), listLength(right));
        for (size_t position = 0; position < length; ++position)
        {
            if (const int order = compare(listElement(left, position), listElement(right, position)); order != 0)
            {
                return order;
            }
        }
        return listLength(left) < listLength(right) ? -1 : (listLength(left) > listLength(right) ? 1 : 0);
    }
    panic("`" + valueKindName(left) + "` и `" + valueKindName(right) + "` нельзя упорядочить");
}

bool Machine::equal(const Cell& leftCell, const Cell& rightCell) const
{
    const Value& left = *dereference(leftCell);
    const Value& right = *dereference(rightCell);
    const auto sameItems = [&](const std::vector<Cell>& a, const std::vector<Cell>& b)
    {
        if (a.size() != b.size())
        {
            return false;
        }
        for (size_t position = 0; position < a.size(); ++position)
        {
            if (!a[position] || !b[position])
            {
                if (a[position] != b[position])
                {
                    return false;
                }
                continue;
            }
            if (!equal(a[position], b[position]))
            {
                return false;
            }
        }
        return true;
    };
    if (left.kind != right.kind)
    {
        const bool numeric = (left.kind == ValueKind::Float || left.kind == ValueKind::Integer)
            && (right.kind == ValueKind::Float || right.kind == ValueKind::Integer);
        return numeric && compare(leftCell, rightCell) == 0;
    }
    switch (left.kind)
    {
    case ValueKind::Void:
        return true;
    case ValueKind::Bool:
        return left.truth == right.truth;
    case ValueKind::Integer:
        return compareIntegers(left.integer, right.integer) == 0;
    case ValueKind::Float:
        return left.floating == right.floating;
    case ValueKind::Char:
        return left.character == right.character;
    case ValueKind::Str:
    case ValueKind::Bytes:
        return textView(left) == textView(right);
    case ValueKind::List:
    {
        const size_t length = listLength(left);
        if (length != listLength(right))
        {
            return false;
        }
        for (size_t position = 0; position < length; ++position)
        {
            if (!equal(listElement(left, position), listElement(right, position)))
            {
                return false;
            }
        }
        return true;
    }
    case ValueKind::Enum:
        return left.decl == right.decl && left.variant == right.variant && sameItems(left.items, right.items);
    case ValueKind::Struct:
        return left.decl == right.decl && sameItems(left.items, right.items);
    case ValueKind::Tuple:
    case ValueKind::Optional:
    case ValueKind::Vector:
        return sameItems(left.items, right.items);
    case ValueKind::Range:
        return left.inclusive == right.inclusive && sameItems(left.items, right.items);
    case ValueKind::Type:
        return left.decl == right.decl && left.name == right.name && left.number == right.number
            && left.length == right.length && sameItems(left.items, right.items);
    case ValueKind::Function:
        return left.function == right.function;
    case ValueKind::Closure:
        return left.closure == right.closure;
    default:
        return &left == &right;
    }
}

bool Machine::truth(const Cell& cell) const
{
    const Value& value = *dereference(cell);
    if (value.kind != ValueKind::Bool)
    {
        panic("ожидалось `бул`, а получено `" + valueKindName(value) + "`");
    }
    return value.truth;
}

WideInteger Machine::integerOf(const Cell& cell) const
{
    const Value& value = *dereference(cell);
    if (value.kind != ValueKind::Integer)
    {
        panic("ожидалось целое, а получено `" + valueKindName(value) + "`");
    }
    return value.integer;
}

size_t Machine::sizeOf(const Cell& cell) const
{
    const WideInteger value = integerOf(cell);
    if (value.negative)
    {
        panic("отрицательный индекс " + integerText(value));
    }
    return static_cast<size_t>(value.magnitude);
}

std::string Machine::show(const Cell& cell)
{
    const Cell& value = dereference(cell);
    switch (value->kind)
    {
    case ValueKind::Void:
        return "";
    case ValueKind::Str:
        return std::string(textView(*value));
    case ValueKind::Char:
    {
        std::string text;
        unicode::appendUtf8(text, value->character);
        return text;
    }
    case ValueKind::Integer:
        return integerText(value->integer);
    case ValueKind::Float:
        return floatText(value->floating, value->number);
    case ValueKind::Bool:
        return value->truth ? "да" : "нет";
    case ValueKind::Struct:
    case ValueKind::Enum:
    {
        const FunctionInfo* method = declarations.method(value->decl, "", "показать");
        if (method != nullptr && !method->builtin && builderType != nullptr)
        {
            Value builder;
            builder.kind = ValueKind::Builder;
            builder.builder = std::make_shared<std::string>();
            const Cell output = makeCell(std::move(builder));
            invoke(*method, referenceCell(value), {referenceCell(output)});
            return *output->builder;
        }
        return describe(value);
    }
    default:
        return describe(value);
    }
}

std::string Machine::describe(const Cell& cell)
{
    const Cell& value = dereference(cell);
    const auto joined = [&](const std::vector<Cell>& items)
    {
        std::string text;
        for (const Cell& item : items)
        {
            text += (text.empty() ? "" : ", ") + (item ? describe(item) : std::string());
        }
        return text;
    };
    switch (value->kind)
    {
    case ValueKind::Str:
        return "\"" + std::string(textView(*value)) + "\"";
    case ValueKind::Char:
        return "'" + show(value) + "'";
    case ValueKind::Enum:
    {
        const std::string name = value->decl->variants[value->variant].display;
        return value->items.empty() ? name : name + "(" + joined(value->items) + ")";
    }
    case ValueKind::Struct:
    {
        std::string text = value->decl->display + "{ ";
        for (size_t position = 0; position < value->items.size(); ++position)
        {
            text += (position == 0 ? "" : ", ") + value->decl->fields[position].display + ": "
                + describe(value->items[position]);
        }
        return text + " }";
    }
    case ValueKind::Optional:
        return value->items.empty() ? std::string(NONE_KEY) : "Есть(" + joined(value->items) + ")";
    case ValueKind::Tuple:
        return "(" + joined(value->items) + ")";
    case ValueKind::Vector:
        return "Вектор(" + joined(value->items) + ")";
    case ValueKind::List:
    {
        std::string text = "[";
        for (size_t position = 0; position < listLength(*value); ++position)
        {
            text += (position == 0 ? "" : ", ") + describe(listElement(*value, position));
        }
        return text + "]";
    }
    case ValueKind::Bytes:
    {
        std::string text = "[";
        for (const char byte : textView(*value))
        {
            text += (text.size() == 1 ? "" : ", ") + std::to_string(static_cast<unsigned char>(byte));
        }
        return text + "]";
    }
    case ValueKind::Builder:
        return "\"" + *value->builder + "\"";
    case ValueKind::Range:
        return (value->items[0] ? describe(value->items[0]) : "") + (value->inclusive ? "..=" : "..")
            + (value->items[1] ? describe(value->items[1]) : "");
    case ValueKind::Void:
        return "()";
    case ValueKind::Type:
        return typeName(value);
    case ValueKind::Function:
        return value->function != nullptr ? "функ " + value->function->display : "функ";
    case ValueKind::Closure:
        return "функ(…)";
    default:
        return show(value);
    }
}

int Machine::runEntry()
{
    const auto [entryPoint, owner] = lookup(&entryModule, std::string(ENTRY_KEY));
    if (entryPoint == nullptr || entryPoint->kind != EntryKind::Function || owner != &entryModule)
    {
        write(true, "в модуле нет функции `вход`\n");
        return FAILURE_EXIT_CODE;
    }
    try
    {
        const Cell result = dereference(invoke(*entryPoint->function, nullptr, {}));
        if (result->kind == ValueKind::Enum && result->decl == resultType
            && result->decl->variants[result->variant].key == FAILURE_KEY)
        {
            return FAILURE_EXIT_CODE;
        }
        return 0;
    }
    catch (const Panic& failure)
    {
        write(true, "паника: " + failure.message + "\n  в " + failure.location + "\n");
        return PANIC_EXIT_CODE;
    }
}

std::vector<TestOutcome> Machine::runTests()
{
    std::vector<TestOutcome> outcomes;
    std::vector<const FunctionInfo*> tests;
    for (const FunctionInfo& function : declarations.functions)
    {
        if (function.module != &entryModule || !hasAttribute(function.item, TEST_ATTRIBUTE))
        {
            continue;
        }
        const SyntaxNodeView list = function.signature.node(ParamList);
        bool parameters = false;
        if (list)
        {
            for (const SyntaxNodeView& parameter : list.nodes())
            {
                parameters = parameters || parameter.kind() == Param;
            }
        }
        if (!parameters && !function.builtin)
        {
            tests.push_back(&function);
        }
    }
    std::sort(tests.begin(), tests.end(),
        [](const FunctionInfo* left, const FunctionInfo* right) { return left->nameOffset < right->nameOffset; });
    for (const FunctionInfo* test : tests)
    {
        TestOutcome outcome;
        outcome.name = test->display;
        try
        {
            const Cell result = dereference(invoke(*test, nullptr, {}));
            outcome.passed = true;
            if (result->kind == ValueKind::Enum && result->decl == resultType
                && result->decl->variants[result->variant].key == FAILURE_KEY)
            {
                outcome.passed = false;
                const Cell& error = dereference(result->items.front());
                const bool failed = error->kind == ValueKind::Struct && error->decl == failureType;
                const Value* message = failed ? dereference(error->items.front()).get() : nullptr;
                outcome.message = message != nullptr ? std::string(textView(*message)) : show(error);
                outcome.skipped = message != nullptr && skipReasons.contains(message->text.get());
            }
        }
        catch (const Panic& failure)
        {
            outcome.passed = false;
            outcome.message = "паника: " + failure.message + " (" + failure.location + ")";
            frames.clear();
            depth = 0;
        }
        outcomes.push_back(std::move(outcome));
    }
    return outcomes;
}

int runProgram(Program& program, SourceModule& entry, const RunOptions& options)
{
    int code = 0;
    runWithStack(STACK_BYTES, [&]
    {
        Machine machine(program, entry, options);
        code = machine.runEntry();
    });
    return code;
}

std::vector<TestOutcome> runTests(Program& program, SourceModule& module, const RunOptions& options)
{
    std::vector<TestOutcome> outcomes;
    runWithStack(STACK_BYTES, [&]
    {
        Machine machine(program, module, options);
        outcomes = machine.runTests();
    });
    return outcomes;
}

}

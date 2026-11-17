#include "interp/machine.hpp"

#include "sema/naming.hpp"
#include "syntax/lexer.hpp"

#include <algorithm>
#include <array>
#include <charconv>

namespace splav
{

using SourceModule = Module;

using enum SyntaxKind;

namespace
{

constexpr size_t POINTER_BYTES = 8;
constexpr size_t SMALL_TAG_LIMIT = 256;
constexpr size_t MEDIUM_TAG_LIMIT = 65536;
constexpr std::array SCALAR_TYPE_NAMES = {"стр", "сим", "бул", "пусто", "никогда", "тип"};
constexpr std::array POINTER_LIKE = {"&", "&дин", "функ", "&функ"};

size_t roundUp(size_t value, size_t alignment)
{
    return alignment <= 1 ? value : (value + alignment - 1) / alignment * alignment;
}

size_t nextPowerOfTwo(size_t value)
{
    size_t result = 1;
    while (result < value)
    {
        result *= 2;
    }
    return result;
}

Cell typeCellNamed(std::string name, std::vector<Cell> items, bool flag)
{
    Value value;
    value.kind = ValueKind::Type;
    value.name = std::move(name);
    value.items = std::move(items);
    value.truth = flag;
    return makeCell(std::move(value));
}

Cell unknownTypeCell()
{
    return typeCellNamed("?", {}, false);
}

std::optional<size_t> attributeNumber(const SyntaxNodeView& node, std::string_view name)
{
    for (const SyntaxNodeView& attribute : node.nodes())
    {
        const std::optional<SyntaxToken> identifier = attribute.token(Identifier);
        if (attribute.kind() != Attribute || !identifier || nameKey(identifier->text) != nameKey(name))
        {
            continue;
        }
        for (const SyntaxToken& token : attribute.tokens())
        {
            if (token.kind == Integer)
            {
                size_t number = 0;
                std::from_chars(token.text.data(), token.text.data() + token.text.size(), number);
                return number;
            }
        }
        for (const SyntaxNodeView& argument : attribute.nodes())
        {
            if (const std::optional<SyntaxToken> first = argument.firstToken(); first && first->kind == Integer)
            {
                size_t number = 0;
                std::from_chars(first->text.data(), first->text.data() + first->text.size(), number);
                return number;
            }
        }
        return 0;
    }
    return std::nullopt;
}

}

std::vector<Local> Machine::bindingsOf(const Cell& type) const
{
    const Value& value = *dereference(type);
    if (const std::vector<Local>* synthesized = typeArgumentsOf(value.decl))
    {
        return *synthesized;
    }
    std::vector<Local> bindings;
    if (value.decl != nullptr)
    {
        for (size_t index = 0; index < value.decl->parameters.size() && index < value.items.size(); ++index)
        {
            bindings.push_back(Local{value.decl->parameters[index], value.items[index]});
        }
    }
    return bindings;
}

Cell Machine::typeValue(const SyntaxNodeView& node, SourceModule* module, const std::vector<Local>* bindings)
{
    switch (node.kind())
    {
    case NameExpr:
    {
        if (node.hasToken(KwSelfType))
        {
            Value value;
            value.kind = ValueKind::Type;
            value.decl = frame().function != nullptr ? frame().function->owner : nullptr;
            return value.decl != nullptr ? makeCell(std::move(value)) : unknownTypeCell();
        }
        const std::optional<SyntaxToken> name = node.token(Identifier);
        if (!name)
        {
            return unknownTypeCell();
        }
        const std::string& key = keyOf(*name);
        if (bindings != nullptr)
        {
            for (auto bound = bindings->rbegin(); bound != bindings->rend(); ++bound)
            {
                if (bound->key == key)
                {
                    return dereference(bound->cell);
                }
            }
        }
        const auto [entry, owner] = lookup(module, key);
        if (entry != nullptr && entry->kind == EntryKind::Type)
        {
            Value value;
            value.kind = ValueKind::Type;
            value.decl = entry->type;
            return makeCell(std::move(value));
        }
        if (entry != nullptr && entry->kind == EntryKind::Const)
        {
            const Cell aliased = dereference(constant(*entry, owner));
            return aliased->kind == ValueKind::Type ? aliased : unknownTypeCell();
        }
        if (numberNamed(key)
            || std::find(SCALAR_TYPE_NAMES.begin(), SCALAR_TYPE_NAMES.end(), key) != SCALAR_TYPE_NAMES.end())
        {
            return typeCellNamed(key, {}, false);
        }
        return unknownTypeCell();
    }
    case FieldExpr:
    {
        const SyntaxNodeView receiver = node.child(0);
        const std::optional<SyntaxToken> member = node.token(Identifier);
        if (!member || receiver.kind() != NameExpr || !receiver.token(Identifier))
        {
            return unknownTypeCell();
        }
        const auto [entry, owner] = lookup(module, keyOf(*receiver.token(Identifier)));
        if (entry == nullptr || entry->kind != EntryKind::Module || entry->target == nullptr)
        {
            return unknownTypeCell();
        }
        const auto [found, foundOwner] = lookup(entry->target, keyOf(*member));
        if (found == nullptr || found->kind != EntryKind::Type)
        {
            return unknownTypeCell();
        }
        Value value;
        value.kind = ValueKind::Type;
        value.decl = found->type;
        return makeCell(std::move(value));
    }
    case CallExpr:
    {
        const SyntaxNodeView callee = node.child(0);
        const SyntaxNodeView argumentList = node.node(ArgList);
        const std::vector<SyntaxNodeView> arguments =
            argumentList ? argumentList.nodes() : std::vector<SyntaxNodeView>();
        if (callee.kind() == NameExpr && callee.token(Identifier) && keyOf(*callee.token(Identifier)) == "Вектор")
        {
            return vectorType(node, arguments);
        }
        std::vector<Cell> values;
        for (const SyntaxNodeView& argument : arguments)
        {
            values.push_back(typeValue(argument, module, bindings));
        }
        const Cell head = typeValue(callee, module, bindings);
        if (head->kind == ValueKind::Type && head->decl != nullptr)
        {
            Value value;
            value.kind = ValueKind::Type;
            value.decl = head->decl;
            value.items = std::move(values);
            return makeCell(std::move(value));
        }
        if (callee.kind() == NameExpr && callee.token(Identifier))
        {
            const auto [entry, owner] = lookup(module, keyOf(*callee.token(Identifier)));
            if (entry != nullptr && entry->kind == EntryKind::Function)
            {
                const Cell result = dereference(invoke(*entry->function, nullptr, std::move(values)));
                return result->kind == ValueKind::Type ? result : unknownTypeCell();
            }
        }
        return unknownTypeCell();
    }
    case RefExpr:
    {
        const std::vector<SyntaxNodeView> parts = node.nodes();
        const SyntaxNodeView inner = parts.empty() ? SyntaxNodeView() : parts.back();
        const bool mutableAccess = node.hasToken(KwMut);
        if (inner && inner.kind() == DynType)
        {
            return typeCellNamed("&дин", {typeValue(inner.child(0), module, bindings)}, mutableAccess);
        }
        if (inner && inner.kind() == FunctionExpr)
        {
            const Cell function = typeValue(inner, module, bindings);
            return typeCellNamed("&функ", function->items, mutableAccess);
        }
        return typeCellNamed("&", {inner ? typeValue(inner, module, bindings) : unknownTypeCell()}, mutableAccess);
    }
    case DynType:
        return typeCellNamed("&дин", {typeValue(node.child(0), module, bindings)}, false);
    case OptionalType:
        return typeCellNamed("?", {typeValue(node.child(0), module, bindings)}, false);
    case SliceType:
    case SentinelType:
    {
        const std::vector<SyntaxNodeView> parts = node.nodes();
        const Cell element = parts.empty() ? unknownTypeCell() : typeValue(parts.back(), module, bindings);
        return typeCellNamed("[]", {element}, node.hasToken(KwMut));
    }
    case ArrayType:
    {
        const std::vector<SyntaxNodeView> parts = node.nodes();
        Cell array = typeCellNamed(
            "[Н]", {parts.empty() ? unknownTypeCell() : typeValue(parts.back(), module, bindings)}, false);
        if (parts.size() >= 2)
        {
            array->length = sizeOf(evaluateIn(parts.front(), *module));
        }
        return array;
    }
    case PointerType:
        return typeCellNamed("*", {typeValue(node.child(0), module, bindings)}, false);
    case TupleExpr:
    {
        std::vector<Cell> items;
        for (const SyntaxNodeView& element : node.nodes())
        {
            items.push_back(typeValue(element, module, bindings));
        }
        return typeCellNamed("()", std::move(items), false);
    }
    case FunctionExpr:
    {
        std::vector<Cell> items;
        SyntaxNodeView result;
        bool afterArrow = false;
        node.forEachChild([&](const SyntaxNodeView& child, const std::optional<SyntaxToken>& token)
        {
            if (token)
            {
                afterArrow = afterArrow || token->kind == Arrow;
                return;
            }
            if (child.kind() == FunctionParam)
            {
                const std::vector<SyntaxNodeView> parts = child.nodes();
                items.push_back(parts.empty() ? unknownTypeCell() : typeValue(parts.back(), module, bindings));
            }
            else if (afterArrow)
            {
                result = child;
            }
        });
        items.push_back(result ? typeValue(result, module, bindings) : typeCellNamed("пусто", {}, false));
        return typeCellNamed("функ", std::move(items), false);
    }
    case ParenExpr:
        return typeValue(node.child(0), module, bindings);
    default:
        return unknownTypeCell();
    }
}

std::vector<Machine::FieldLayout> Machine::fieldLayouts(const TypeDecl& decl, const std::vector<Local>& bindings)
{
    std::vector<SyntaxNodeView> fieldNodes;
    if (const SyntaxNodeView list = decl.node.node(FieldList))
    {
        for (const SyntaxNodeView& field : list.nodes())
        {
            if (field.kind() == Field && field.token(Identifier))
            {
                fieldNodes.push_back(field);
            }
        }
    }
    std::vector<FieldLayout> fields;
    const bool packed = attributeNumber(decl.node, "Упакованный").has_value();
    for (size_t index = 0; index < decl.fields.size(); ++index)
    {
        FieldLayout field;
        field.name = decl.fields[index].display;
        field.type = typeValue(decl.fields[index].type, decl.module, &bindings);
        field.layout = layoutOf(field.type);
        if (index < fieldNodes.size())
        {
            if (const std::optional<size_t> aligned = attributeNumber(fieldNodes[index], "Выравнивание"))
            {
                field.layout.align = std::max(field.layout.align, *aligned);
            }
        }
        if (packed)
        {
            field.layout.align = 1;
        }
        fields.push_back(std::move(field));
    }
    return fields;
}

Machine::Layout Machine::placeFields(std::vector<FieldLayout>& fields, size_t start, bool dense, size_t minimumAlign)
{
    std::vector<size_t> order(fields.size());
    for (size_t index = 0; index < order.size(); ++index)
    {
        order[index] = index;
    }
    if (dense)
    {
        std::stable_sort(order.begin(), order.end(),
            [&](size_t left, size_t right) { return fields[left].layout.align > fields[right].layout.align; });
    }
    size_t offset = start;
    size_t align = std::max<size_t>(1, minimumAlign);
    for (const size_t index : order)
    {
        FieldLayout& field = fields[index];
        offset = roundUp(offset, field.layout.align);
        field.offset = offset;
        offset += field.layout.size;
        align = std::max(align, field.layout.align);
    }
    return Layout{offset, align};
}

Machine::Layout Machine::layoutOf(const Cell& typeCell)
{
    const Value& type = *dereference(typeCell);
    if (type.kind != ValueKind::Type)
    {
        return Layout{};
    }
    if (type.decl != nullptr)
    {
        const TypeDecl& decl = *type.decl;
        const std::vector<Local> bindings = bindingsOf(typeCell);
        if (decl.kind == DeclKind::Struct)
        {
            std::vector<FieldLayout> fields = fieldLayouts(decl, bindings);
            if (decl.node.hasToken(KwSet))
            {
                Layout layout;
                for (const FieldLayout& field : fields)
                {
                    layout.size += field.layout.size;
                    layout.align = std::max(layout.align, field.layout.align);
                }
                return layout;
            }
            const bool dense = attributeNumber(decl.node, "Плотный").has_value();
            const size_t minimum = attributeNumber(decl.node, "Выравнивание").value_or(1);
            Layout layout = placeFields(fields, 0, dense, minimum);
            layout.size = roundUp(layout.size, layout.align);
            return layout;
        }
        if (decl.kind == DeclKind::Enum)
        {
            return enumLayout(decl, bindings).layout;
        }
        return Layout{};
    }
    const auto item = [&](size_t index) { return index < type.items.size() ? type.items[index] : unknownTypeCell(); };
    if (const std::optional<Number> number = numberNamed(type.name))
    {
        const size_t bytes = numberBits(*number) / 8;
        return Layout{bytes, bytes};
    }
    if (type.name == "ц128" || type.name == "н128")
    {
        return Layout{16, 16};
    }
    if (type.name == "в16")
    {
        return Layout{2, 2};
    }
    if (type.name == "бул")
    {
        return Layout{1, 1};
    }
    if (type.name == "сим")
    {
        return Layout{4, 4};
    }
    if (type.name == "стр" || type.name == "[]" || type.name == "&дин" || type.name == "&функ")
    {
        return Layout{2 * POINTER_BYTES, POINTER_BYTES};
    }
    if (type.name == "&" || type.name == "*" || type.name == "функ")
    {
        return Layout{POINTER_BYTES, POINTER_BYTES};
    }
    if (type.name == "[Н]")
    {
        const Layout element = layoutOf(item(0));
        return Layout{element.size * type.length, element.align};
    }
    if (type.name == "Вектор")
    {
        const size_t bytes = nextPowerOfTwo(numberBits(type.number) / 8 * type.length);
        return Layout{bytes, bytes};
    }
    if (type.name == "()")
    {
        std::vector<FieldLayout> fields;
        for (const Cell& element : type.items)
        {
            fields.push_back(FieldLayout{std::string(), element, layoutOf(element), 0});
        }
        Layout layout = placeFields(fields, 0, false, 1);
        layout.size = roundUp(layout.size, layout.align);
        return layout;
    }
    if (type.name == "?")
    {
        const Cell inner = dereference(item(0));
        if (std::find(POINTER_LIKE.begin(), POINTER_LIKE.end(), inner->name) != POINTER_LIKE.end())
        {
            return layoutOf(inner);
        }
        const Layout payload = layoutOf(inner);
        const size_t offset = roundUp(1, payload.align);
        const size_t align = std::max<size_t>(1, payload.align);
        return Layout{roundUp(offset + payload.size, align), align};
    }
    return Layout{0, 1};
}

Machine::EnumLayout Machine::enumLayout(const TypeDecl& decl, const std::vector<Local>& bindings)
{
    EnumLayout result;
    const size_t tag = decl.variants.size() <= SMALL_TAG_LIMIT ? 1 : decl.variants.size() <= MEDIUM_TAG_LIMIT ? 2 : 4;
    std::vector<std::vector<std::string>> names(decl.variants.size());
    if (const SyntaxNodeView list = decl.node.node(VariantList))
    {
        size_t position = 0;
        for (const SyntaxNodeView& variant : list.nodes())
        {
            if (variant.kind() != Variant || position >= names.size())
            {
                continue;
            }
            for (const SyntaxNodeView& field : variant.nodes())
            {
                if (field.kind() == VariantField)
                {
                    const std::optional<SyntaxToken> name = field.token(Identifier);
                    names[position].push_back(name ? normalizedName(name->text) : std::string());
                }
            }
            ++position;
        }
    }
    size_t payloadAlign = 1;
    for (size_t index = 0; index < decl.variants.size(); ++index)
    {
        std::vector<FieldLayout> fields;
        for (size_t field = 0; field < decl.variants[index].payload.size(); ++field)
        {
            const Cell type = typeValue(decl.variants[index].payload[field], decl.module, &bindings);
            const std::string name = field < names[index].size() ? names[index][field] : std::string();
            fields.push_back(FieldLayout{name, type, layoutOf(type), 0});
            payloadAlign = std::max(payloadAlign, fields.back().layout.align);
        }
        result.variants.push_back(std::move(fields));
    }
    const size_t payloadOffset = roundUp(tag, payloadAlign);
    size_t end = tag;
    for (std::vector<FieldLayout>& fields : result.variants)
    {
        const Layout placed = placeFields(fields, payloadOffset, false, 1);
        end = std::max(end, placed.size);
    }
    result.layout.align = std::max(tag, payloadAlign);
    result.layout.size = roundUp(end, result.layout.align);
    return result;
}

Cell Machine::typeInfo(const Cell& typeCell)
{
    Cell type = dereference(typeCell);
    if (type->kind == ValueKind::Tuple && std::all_of(type->items.begin(), type->items.end(), [](const Cell& item) {
        return dereference(item)->kind == ValueKind::Type;
    }))
    {
        type = typeCellNamed("()", type->items, false);
    }
    if (type->kind != ValueKind::Type)
    {
        panic("`компил_инфо` ждёт тип");
    }
    const TypeDecl* info = findType("ядро.прелюдия", "ИнфоТипа");
    const TypeDecl* structureInfo = findType("ядро.прелюдия", "ИнфоСтруктуры");
    const TypeDecl* fieldInfo = findType("ядро.прелюдия", "ИнфоПоля");
    const TypeDecl* enumerationInfo = findType("ядро.прелюдия", "ИнфоПеречисления");
    const TypeDecl* variantInfo = findType("ядро.прелюдия", "ИнфоВарианта");
    const auto size = [](size_t value) { return integerCell(unsignedInteger(value), Number::Size); };
    const auto bits = [](size_t value) { return integerCell(unsignedInteger(value), Number::U32); };
    const auto item = [&](size_t index) { return index < type->items.size() ? type->items[index] : unknownTypeCell(); };
    const auto typeList = [&](size_t from, size_t to)
    {
        std::vector<Cell> items;
        for (size_t index = from; index < to && index < type->items.size(); ++index)
        {
            items.push_back(type->items[index]);
        }
        return listCell(std::move(items), false);
    };
    const auto fieldsOf = [&](const std::vector<FieldLayout>& fields)
    {
        std::vector<Cell> items;
        for (const FieldLayout& field : fields)
        {
            items.push_back(structValue(fieldInfo, {stringCell(field.name), field.type, size(field.offset)}));
        }
        return listCell(std::move(items), false);
    };
    if (type->decl != nullptr)
    {
        const TypeDecl& decl = *type->decl;
        const std::vector<Local> bindings = bindingsOf(type);
        const Layout layout = layoutOf(type);
        if (decl.kind == DeclKind::Struct)
        {
            std::vector<FieldLayout> fields = fieldLayouts(decl, bindings);
            const bool set = decl.node.hasToken(KwSet);
            if (!set)
            {
                placeFields(fields, 0, attributeNumber(decl.node, "Плотный").has_value(), 1);
            }
            const Cell described = structValue(
                structureInfo, {stringCell(typeName(type)), size(layout.size), size(layout.align), fieldsOf(fields)});
            return enumValue(info, set ? "Набор" : "Структура", {described});
        }
        if (decl.kind == DeclKind::Enum)
        {
            const EnumLayout described = enumLayout(decl, bindings);
            std::vector<Cell> variantInfos;
            for (size_t index = 0; index < decl.variants.size(); ++index)
            {
                variantInfos.push_back(structValue(variantInfo,
                    {stringCell(decl.variants[index].display), size(index), fieldsOf(described.variants[index])}));
            }
            return enumValue(info, "Перечисление",
                {structValue(enumerationInfo,
                    {stringCell(typeName(type)), size(described.layout.size), size(described.layout.align),
                        listCell(std::move(variantInfos), false)})});
        }
        return enumValue(info, "Типаж", {stringCell(decl.display)});
    }
    if (const std::optional<Number> number = numberNamed(type->name))
    {
        if (isFloatNumber(*number))
        {
            return enumValue(info, "Вещественное", {bits(numberBits(*number))});
        }
        return enumValue(info, "Целое", {boolCell(numberSigned(*number)), bits(numberBits(*number))});
    }
    if (type->name == "ц128" || type->name == "н128")
    {
        return enumValue(info, "Целое", {boolCell(type->name == "ц128"), bits(128)});
    }
    if (type->name == "в16")
    {
        return enumValue(info, "Вещественное", {bits(16)});
    }
    if (type->name == "бул")
    {
        return enumValue(info, "Логическое", {});
    }
    if (type->name == "сим")
    {
        return enumValue(info, "Символ", {});
    }
    if (type->name == "стр")
    {
        return enumValue(info, "Строка", {});
    }
    if (type->name == "пусто")
    {
        return enumValue(info, "Пусто", {});
    }
    if (type->name == "никогда")
    {
        return enumValue(info, "Никогда", {});
    }
    if (type->name == "тип")
    {
        return enumValue(info, "ТипТипов", {});
    }
    if (type->name == "Вектор")
    {
        return enumValue(info, "КороткийВектор",
            {typeCellNamed(std::string(numberName(type->number)), {}, false), size(type->length)});
    }
    if (type->name == "&")
    {
        return enumValue(info, "Ссылка", {item(0), boolCell(type->truth)});
    }
    if (type->name == "*")
    {
        return enumValue(info, "Указатель", {item(0)});
    }
    if (type->name == "[]")
    {
        return enumValue(info, "Срез", {item(0), boolCell(type->truth)});
    }
    if (type->name == "[Н]")
    {
        return enumValue(info, "МассивФикс", {item(0), size(type->length)});
    }
    if (type->name == "?")
    {
        return enumValue(info, "Опция", {item(0)});
    }
    if (type->name == "()")
    {
        return enumValue(info, "Кортеж", {typeList(0, type->items.size())});
    }
    if (type->name == "функ" || type->name == "&функ")
    {
        const size_t parameters = type->items.empty() ? 0 : type->items.size() - 1;
        if (type->name == "функ")
        {
            return enumValue(info, "Функция", {typeList(0, parameters), item(parameters)});
        }
        return enumValue(info, "Замыкание", {typeList(0, parameters), item(parameters), boolCell(type->truth)});
    }
    if (type->name == "&дин")
    {
        return enumValue(info, "Дин", {item(0), boolCell(type->truth)});
    }
    panic("`компил_инфо`: тип `" + typeName(type) + "` неизвестен");
}

}

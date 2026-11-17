#include "sema/memory/ast.hpp"

#include <algorithm>
#include <functional>
#include <utility>

namespace splav::memory
{

namespace
{

using Field = std::pair<std::string, std::string>;

std::string literalKindText(LiteralKind kind)
{
    switch (kind)
    {
    case LiteralKind::Integer:
        return "ЦЕЛОЕ";
    case LiteralKind::Float:
        return "ВЕЩЕСТВЕННОЕ";
    case LiteralKind::Char:
        return "СИМВОЛ";
    case LiteralKind::Byte:
        return "БАЙТ";
    case LiteralKind::String:
        return "СТРОКА";
    case LiteralKind::Bool:
        return "БУЛ";
    }
    return "";
}

class Dumper
{
public:
    explicit Dumper(const Tree& owner) : tree(owner)
    {
    }

    std::string node(const Node* node) const
    {
        if (node == nullptr)
        {
            return "∅";
        }
        std::vector<Field> fields = fieldsOf(*node);
        std::sort(fields.begin(), fields.end());
        const auto [line, column] = tree.position(node->first);
        const auto [lastLine, lastColumn] = tree.position(node->lastStart);
        std::string result = kindText(node->kind) + "@" + std::to_string(line) + ":" + std::to_string(column) + "~"
            + std::to_string(lastLine) + ":" + std::to_string(lastColumn) + "(";
        bool first = true;
        for (const Field& field : fields)
        {
            result += (first ? "" : "; ") + field.first + "=" + field.second;
            first = false;
        }
        return result + ")";
    }

private:
    static std::string text(const std::string& value)
    {
        return "«" + value + "»";
    }

    static std::string optionalText(const std::optional<std::string>& value)
    {
        return value ? text(*value) : "∅";
    }

    static std::string flag(bool value)
    {
        return value ? "да" : "нет";
    }

    std::string token(const std::optional<Token>& value) const
    {
        if (!value)
        {
            return "∅";
        }
        const auto [line, column] = tree.position(value->offset);
        return "т«" + value->text + "»@" + std::to_string(line) + ":" + std::to_string(column);
    }

    std::string list(const std::vector<const Node*>& nodes) const
    {
        std::string result = "[";
        for (size_t index = 0; index < nodes.size(); ++index)
        {
            result += (index == 0 ? "" : ", ") + node(nodes[index]);
        }
        return result + "]";
    }

    std::string strings(const std::vector<std::string>& values) const
    {
        std::string result = "[";
        for (size_t index = 0; index < values.size(); ++index)
        {
            result += (index == 0 ? "" : ", ") + text(values[index]);
        }
        return result + "]";
    }

    std::string pairs(const std::vector<Labeled>& values, bool withToken) const
    {
        std::string result = "[";
        for (size_t index = 0; index < values.size(); ++index)
        {
            const Labeled& value = values[index];
            result +=
                (index == 0 ? "" : ", ") + std::string("(") + optionalText(value.label) + ", " + node(value.value);
            if (withToken)
            {
                result += ", " + token(value.token);
            }
            result += ")";
        }
        return result + "]";
    }

    std::string named(const std::vector<Labeled>& values) const
    {
        std::string result = "[";
        for (size_t index = 0; index < values.size(); ++index)
        {
            const Labeled& value = values[index];
            result += (index == 0 ? "" : ", ") + std::string("(") + optionalText(value.label) + ", " + node(value.value)
                + ", " + token(value.token) + ")";
        }
        return result + "]";
    }

    std::string operands(const std::vector<Labeled>& values) const
    {
        std::string result = "[";
        for (size_t index = 0; index < values.size(); ++index)
        {
            const Labeled& value = values[index];
            result += (index == 0 ? "" : ", ") + std::string("(") + optionalText(value.label) + ", "
                + text(value.token ? value.token->text : "") + ", " + node(value.value) + ")";
        }
        return result + "]";
    }

    std::vector<Field> fieldsOf(const Node& node) const
    {
        std::vector<Field> fields;
        const auto add = [&fields](std::string name, std::string value)
        { fields.emplace_back(std::move(name), std::move(value)); };
        const auto declaration = [&]()
        {
            add("имя", optionalText(node.name));
            add("токен_имени", token(node.nameToken));
            add("откр", flag(node.open));
            add("атрибуты", list(node.attributes));
        };
        switch (node.kind)
        {
        case Kind::Module:
            add("элементы", list(node.items));
            break;
        case Kind::Attribute:
            add("имя", optionalText(node.name));
            add("аргументы", pairs(node.labeled, false));
            break;
        case Kind::Import:
            add("путь", strings(node.path));
            add("псевдоним", optionalText(node.alias));
            add("атрибуты", list(node.attributes));
            break;
        case Kind::Include:
            add("файл", text(node.text));
            add("псевдоним", optionalText(node.alias));
            add("атрибуты", list(node.attributes));
            break;
        case Kind::Impl:
            add("параметры", list(node.ownerParameters));
            add("типаж", this->node(node.trait));
            add("тип", this->node(node.type));
            add("методы", node.hasFields ? list(node.items) : "∅");
            add("атрибуты", list(node.attributes));
            break;
        case Kind::Const:
            declaration();
            add("тип", this->node(node.type));
            add("значение", this->node(node.value));
            break;
        case Kind::Struct:
        case Kind::Set:
            declaration();
            add("параметры", list(node.ownerParameters));
            add("поля", list(node.items));
            break;
        case Kind::Enum:
            declaration();
            add("параметры", list(node.ownerParameters));
            add("варианты", list(node.items));
            break;
        case Kind::Trait:
            declaration();
            add("параметры", list(node.ownerParameters));
            add("сигнатуры", list(node.items));
            break;
        case Kind::Function:
            add("сигнатура", this->node(node.signature));
            add("тело", this->node(node.body));
            add("откр", flag(node.open));
            add("атрибуты", list(node.attributes));
            break;
        case Kind::Field:
            declaration();
            add("тип", this->node(node.type));
            break;
        case Kind::Variant:
            add("имя", optionalText(node.name));
            add("поля", node.hasFields ? pairs(node.labeled, false) : "∅");
            break;
        case Kind::Parameter:
            add("форма", text(node.op));
            add("имя", optionalText(node.name));
            add("токен_имени", token(node.nameToken));
            add("тип", this->node(node.type));
            add("компил", flag(node.comptime));
            add("изм", flag(node.mutableAccess));
            break;
        case Kind::Signature:
            add("имя", optionalText(node.name));
            add("токен_имени", token(node.nameToken));
            add("владелец", optionalText(node.owner));
            add("параметры_владельца", list(node.ownerParameters));
            add("параметры", list(node.items));
            add("требует", optionalText(node.requirement));
            add("результат", this->node(node.result));
            break;
        case Kind::Block:
            add("инструкции", list(node.items));
            break;
        case Kind::Binding:
            add("изм", flag(node.mutableAccess));
            add("имя", optionalText(node.name));
            add("токен_имени", token(node.nameToken));
            add("тип", this->node(node.type));
            add("значение", this->node(node.value));
            break;
        case Kind::Assign:
            add("оп", text(node.op));
            add("цель", this->node(node.left));
            add("значение", this->node(node.value));
            break;
        case Kind::Expression:
        case Kind::Discard:
        case Kind::Return:
        case Kind::Defer:
        case Kind::Spawn:
            add("значение", this->node(node.value));
            break;
        case Kind::Break:
        case Kind::Continue:
            add("метка", optionalText(node.label));
            break;
        case Kind::If:
            add("условие", this->node(node.condition));
            add("то", this->node(node.then));
            add("иначе", this->node(node.otherwise));
            add("компил", flag(node.comptime));
            break;
        case Kind::Loop:
            add("метка", optionalText(node.label));
            add("образец", this->node(node.pattern));
            add("источник", this->node(node.source));
            add("условие", this->node(node.condition));
            add("компил", flag(node.comptime));
            add("изм", flag(node.mutableAccess));
            add("тело", this->node(node.body));
            break;
        case Kind::Region:
            add("имя", optionalText(node.name));
            add("устройство", this->node(node.device));
            add("тело", this->node(node.body));
            break;
        case Kind::Through:
            add("значение", this->node(node.value));
            add("тело", this->node(node.body));
            break;
        case Kind::TogetherBlock:
        case Kind::UnsafeBlock:
            add("тело", this->node(node.body));
            break;
        case Kind::Literal:
            add("вид_лит", text(literalKindText(node.literal)));
            add("значение", text(node.text));
            break;
        case Kind::Paren:
            add("значение", this->node(node.value));
            break;
        case Kind::Name:
        case Kind::MutPattern:
            add("имя", optionalText(node.name));
            break;
        case Kind::Member:
            add("объект", this->node(node.object));
            add("имя", optionalText(node.name));
            add("токен_имени", token(node.nameToken));
            break;
        case Kind::Index:
            add("объект", this->node(node.object));
            add("индекс", this->node(node.index));
            break;
        case Kind::Call:
            add("функция", this->node(node.function));
            add("аргументы", list(node.items));
            break;
        case Kind::Reference:
            add("метка", optionalText(node.label));
            add("изм", flag(node.mutableAccess));
            add("операнд", this->node(node.operand));
            break;
        case Kind::Prefix:
            add("оп", text(node.op));
            add("операнд", this->node(node.operand));
            break;
        case Kind::Binary:
        case Kind::Logical:
        case Kind::Comparison:
            add("оп", text(node.op));
            add("левый", this->node(node.left));
            add("правый", this->node(node.right));
            break;
        case Kind::Membership:
            add("левый", this->node(node.left));
            add("правый", this->node(node.right));
            add("отрицание", flag(node.negated));
            break;
        case Kind::Not:
        case Kind::OptionalType:
        case Kind::DynType:
        case Kind::RawPointer:
            add("операнд", this->node(node.operand));
            break;
        case Kind::Is:
            add("значение", this->node(node.value));
            add("образец", this->node(node.pattern));
            add("отрицание", flag(node.negated));
            break;
        case Kind::Cast:
            add("значение", this->node(node.value));
            add("тип", this->node(node.type));
            break;
        case Kind::Range:
        case Kind::LiteralPattern:
            add("от", this->node(node.from));
            add("до", this->node(node.to));
            add("включительно", flag(node.inclusive));
            break;
        case Kind::CompoundLiteral:
            add("тип", this->node(node.type));
            add("элементы", pairs(node.labeled, true));
            break;
        case Kind::Tuple:
        case Kind::TuplePattern:
            add("элементы", list(node.items));
            break;
        case Kind::Match:
            add("значение", this->node(node.value));
            add("ветви", list(node.items));
            break;
        case Kind::Arm:
            add("образец", this->node(node.pattern));
            add("охрана", this->node(node.guard));
            add("тело", this->node(node.body));
            break;
        case Kind::Closure:
            add("захват", node.op.empty() ? "∅" : text(node.op));
            add("параметры", list(node.items));
            add("результат", this->node(node.result));
            add("тело", this->node(node.body));
            add("вид_тела", text(node.text));
            break;
        case Kind::FunctionType:
            add("параметры", list(node.items));
            add("результат", this->node(node.result));
            break;
        case Kind::Together:
            add("форма", node.op.empty() ? "∅" : text(node.op));
            add("аргументы", list(node.items));
            break;
        case Kind::Asm:
            add("операнды", operands(node.labeled));
            break;
        case Kind::SliceType:
        case Kind::SentinelSlice:
        case Kind::SentinelPointer:
        case Kind::ArrayType:
            add("длина", this->node(node.length));
            add("изм", flag(node.mutableAccess));
            add("операнд", this->node(node.operand));
            break;
        case Kind::StructType:
        case Kind::SetType:
            add("поля", list(node.items));
            break;
        case Kind::EnumType:
            add("варианты", list(node.items));
            break;
        case Kind::OrPattern:
            add("альтернативы", list(node.items));
            break;
        case Kind::NamePattern:
            add("путь", strings(node.path));
            add("аргументы", node.hasArguments ? pairs(node.labeled, false) : "∅");
            add("поля", node.hasFields ? named(node.named) : "∅");
            break;
        case Kind::Placeholder:
        case Kind::AnyPattern:
            break;
        }
        return fields;
    }

    const Tree& tree;
};

}

std::string kindText(Kind kind)
{
    switch (kind)
    {
    case Kind::Module:
        return "модуль";
    case Kind::Import:
        return "подключить";
    case Kind::Include:
        return "вставка";
    case Kind::Impl:
        return "реализ";
    case Kind::Const:
        return "конст";
    case Kind::Struct:
        return "структ";
    case Kind::Set:
        return "набор";
    case Kind::Enum:
        return "переч";
    case Kind::Trait:
        return "типаж";
    case Kind::Function:
        return "функция";
    case Kind::Field:
    case Kind::Member:
        return "поле";
    case Kind::Variant:
        return "вариант";
    case Kind::Signature:
        return "сигнатура";
    case Kind::Parameter:
        return "параметр";
    case Kind::Attribute:
        return "атрибут";
    case Kind::Block:
        return "блок";
    case Kind::Binding:
        return "привязка";
    case Kind::Assign:
        return "присваивание";
    case Kind::Expression:
        return "выражение";
    case Kind::Discard:
        return "отбросить";
    case Kind::Return:
        return "возврат";
    case Kind::Break:
        return "прервать";
    case Kind::Continue:
        return "продолжить";
    case Kind::If:
        return "если";
    case Kind::Loop:
        return "цикл";
    case Kind::Region:
        return "регион";
    case Kind::Through:
        return "через";
    case Kind::TogetherBlock:
        return "вместе_блок";
    case Kind::UnsafeBlock:
        return "опасно_блок";
    case Kind::Defer:
        return "отложить";
    case Kind::Spawn:
        return "запуск";
    case Kind::Literal:
        return "лит";
    case Kind::Paren:
        return "скобки";
    case Kind::Name:
        return "имя";
    case Kind::Index:
        return "индекс";
    case Kind::Call:
        return "вызов";
    case Kind::Reference:
        return "ссылка";
    case Kind::Prefix:
        return "префикс";
    case Kind::Binary:
        return "бинарное";
    case Kind::Comparison:
        return "сравнение";
    case Kind::Membership:
        return "принадлежность";
    case Kind::Logical:
        return "логическое";
    case Kind::Not:
        return "не";
    case Kind::Is:
        return "это";
    case Kind::Cast:
        return "как";
    case Kind::Range:
        return "диапазон";
    case Kind::CompoundLiteral:
        return "литерал";
    case Kind::Tuple:
        return "кортеж";
    case Kind::Match:
        return "выбор";
    case Kind::Arm:
        return "ветвь";
    case Kind::Closure:
        return "замыкание";
    case Kind::Together:
        return "вместе";
    case Kind::Placeholder:
        return "заглушка";
    case Kind::Asm:
        return "асм";
    case Kind::OptionalType:
        return "опц";
    case Kind::DynType:
        return "дин";
    case Kind::RawPointer:
        return "сырой";
    case Kind::SliceType:
        return "тип_срез";
    case Kind::SentinelSlice:
        return "тип_терм_срез";
    case Kind::SentinelPointer:
        return "тип_терм_указатель";
    case Kind::ArrayType:
        return "тип_массив";
    case Kind::FunctionType:
        return "тип_функции";
    case Kind::StructType:
        return "тип_структ";
    case Kind::SetType:
        return "тип_набор";
    case Kind::EnumType:
        return "тип_переч";
    case Kind::AnyPattern:
        return "образец_любой";
    case Kind::LiteralPattern:
        return "образец_литерал";
    case Kind::OrPattern:
        return "образец_или";
    case Kind::MutPattern:
        return "образец_изм";
    case Kind::TuplePattern:
        return "образец_кортеж";
    case Kind::NamePattern:
        return "образец_имя";
    }
    return "";
}

std::string dumpTree(const Tree& tree)
{
    return Dumper(tree).node(tree.root()) + "\n";
}

}

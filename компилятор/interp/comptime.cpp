#include "interp/comptime.hpp"

#include "interp/machine.hpp"

#include <charconv>

namespace splav
{

namespace
{

constexpr std::string_view VECTOR_NAME = "Вектор";

Cell typeCell(const TypePtr& type)
{
    if (!isKnown(type))
    {
        return nullptr;
    }
    Value value;
    value.kind = ValueKind::Type;
    switch (type->kind)
    {
    case TypeKind::Named:
        value.decl = type->decl;
        for (const TypePtr& argument : type->arguments)
        {
            Cell converted = typeCell(argument);
            if (!converted)
            {
                return nullptr;
            }
            value.items.push_back(std::move(converted));
        }
        break;
    case TypeKind::Integer:
    case TypeKind::Float:
    case TypeKind::Bool:
    case TypeKind::Char:
    case TypeKind::Str:
    case TypeKind::Void:
        value.name = type->builtin;
        break;
    case TypeKind::Vector:
    {
        const std::optional<Number> element = type->inner ? numberNamed(type->inner->builtin) : std::nullopt;
        size_t length = 0;
        const std::from_chars_result parsed =
            std::from_chars(type->builtin.data(), type->builtin.data() + type->builtin.size(), length);
        if (!element || parsed.ec != std::errc())
        {
            return nullptr;
        }
        value.name = std::string(VECTOR_NAME);
        value.number = *element;
        value.length = length;
        break;
    }
    default:
        return nullptr;
    }
    return makeCell(std::move(value));
}

class InterpreterEvaluator final : public ComptimeEvaluator
{
public:
    InterpreterEvaluator(Program& program, Module& module, Declarations& declarations)
        : machine(program, module, declarations, RunOptions{})
    {
    }

    TypePtr evaluateType(const FunctionInfo& function, const std::vector<TypePtr>& arguments) override
    {
        std::vector<Cell> cells;
        for (const TypePtr& argument : arguments)
        {
            Cell cell = typeCell(argument);
            if (!cell)
            {
                return unknownType();
            }
            cells.push_back(std::move(cell));
        }
        try
        {
            return typeOf(machine.invoke(function, nullptr, std::move(cells)));
        }
        catch (const Panic&)
        {
            return unknownType();
        }
    }

private:
    TypePtr typeOf(const Cell& cell) const
    {
        const Value& value = *dereference(cell);
        if (value.kind != ValueKind::Type)
        {
            return unknownType();
        }
        if (value.decl != nullptr)
        {
            std::vector<TypePtr> arguments;
            if (const std::vector<Local>* bindings = machine.typeArgumentsOf(value.decl))
            {
                for (const Local& binding : *bindings)
                {
                    arguments.push_back(typeOf(binding.cell));
                }
            }
            else
            {
                for (const Cell& item : value.items)
                {
                    arguments.push_back(typeOf(item));
                }
            }
            arguments.resize(value.decl->parameters.size(), unknownType());
            return namedType(value.decl, std::move(arguments));
        }
        if (value.name == VECTOR_NAME)
        {
            auto vector = std::make_shared<Type>();
            vector->kind = TypeKind::Vector;
            vector->inner = builtinType(std::string(numberName(value.number)));
            vector->builtin = std::to_string(value.length);
            return vector;
        }
        return value.name.empty() || value.name == "?" ? unknownType() : builtinType(value.name);
    }

    Machine machine;
};

}

std::unique_ptr<ComptimeEvaluator> makeComptimeEvaluator(Program& program, Module& module, Declarations& declarations)
{
    return std::make_unique<InterpreterEvaluator>(program, module, declarations);
}

}

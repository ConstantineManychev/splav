#include "sema/coverage.hpp"

#include "sema/naming.hpp"

#include <algorithm>
#include <cstddef>
#include <iterator>

namespace splav
{

namespace
{

using enum SyntaxKind;
using Row = std::vector<SyntaxNodeView>;

constexpr size_t DEPTH_LIMIT = 12;
constexpr std::string_view TUPLE_CONSTRUCTOR = "()";
constexpr std::string_view WILDCARD = "_";

std::vector<SyntaxToken> pathOf(const SyntaxNodeView& pattern)
{
    std::vector<SyntaxToken> path;
    for (const SyntaxToken& token : pattern.tokens())
    {
        if (token.kind == Identifier)
        {
            path.push_back(token);
        }
    }
    return path;
}

bool matchesAnything(const SyntaxNodeView& pattern)
{
    if (!pattern)
    {
        return true;
    }
    switch (pattern.kind())
    {
    case WildcardPattern:
    case BindingPattern:
        return true;
    case PathPattern:
    {
        const std::vector<SyntaxToken> path = pathOf(pattern);
        return path.size() == 1 && pattern.nodes().empty() && !startsUppercase(path.front().text);
    }
    default:
        return false;
    }
}

std::optional<std::string> constructorKey(const SyntaxNodeView& pattern)
{
    switch (pattern.kind())
    {
    case LiteralPattern:
    {
        const std::vector<SyntaxToken> tokens = pattern.tokens();
        if (tokens.size() == 1 && (tokens.front().kind == KwTrue || tokens.front().kind == KwFalse))
        {
            return std::string(tokens.front().text);
        }
        return std::nullopt;
    }
    case TuplePattern:
        return std::string(TUPLE_CONSTRUCTOR);
    case PathPattern:
    {
        const std::vector<SyntaxToken> path = pathOf(pattern);
        return path.empty() ? std::nullopt : std::optional<std::string>(nameKey(path.back().text));
    }
    default:
        return std::nullopt;
    }
}

Row partsOf(const SyntaxNodeView& pattern, const PatternConstructor& constructor)
{
    Row parts(constructor.types.size());
    if (pattern.kind() == TuplePattern)
    {
        const std::vector<SyntaxNodeView> elements = pattern.nodes();
        std::copy_n(elements.begin(), std::min(elements.size(), parts.size()), parts.begin());
        return parts;
    }
    if (pattern.kind() != PathPattern)
    {
        return parts;
    }
    const auto place = [&](std::string_view label) -> std::optional<size_t>
    {
        const std::string key = nameKey(label);
        const auto found = std::find(constructor.labels.begin(), constructor.labels.end(), key);
        if (found == constructor.labels.end())
        {
            return std::nullopt;
        }
        return static_cast<size_t>(std::distance(constructor.labels.begin(), found));
    };
    if (const SyntaxNodeView arguments = pattern.node(PatternArgList))
    {
        size_t position = 0;
        for (const SyntaxNodeView& argument : arguments.nodes())
        {
            const std::optional<SyntaxToken> label = argument.token(Identifier);
            const std::optional<size_t> labelled = label ? place(label->text) : std::nullopt;
            const size_t target = labelled.value_or(position);
            ++position;
            if (target < parts.size() && !argument.nodes().empty())
            {
                parts[target] = argument.nodes().front();
            }
        }
    }
    if (const SyntaxNodeView fields = pattern.node(PatternFieldList))
    {
        for (const SyntaxNodeView& field : fields.nodes())
        {
            const std::optional<SyntaxToken> name = field.token(Identifier);
            const std::optional<size_t> target = name ? place(name->text) : std::nullopt;
            if (target)
            {
                parts[*target] = field.nodes().empty() ? SyntaxNodeView() : field.nodes().front();
            }
        }
    }
    return parts;
}

std::string witness(const PatternConstructor& constructor, const std::vector<std::string>& parts)
{
    std::string joined;
    for (const std::string& part : parts)
    {
        joined += joined.empty() ? part : ", " + part;
    }
    if (constructor.key == TUPLE_CONSTRUCTOR)
    {
        return "(" + joined + ")";
    }
    return parts.empty() ? constructor.display : constructor.display + "(" + joined + ")";
}

std::vector<std::string> wildcards(size_t count)
{
    return std::vector<std::string>(count, std::string(WILDCARD));
}

bool samePattern(const SyntaxNodeView& first, const SyntaxNodeView& second)
{
    if (!first || !second || first.kind() != second.kind())
    {
        return false;
    }
    if (first.kind() == LiteralPattern)
    {
        const std::vector<SyntaxToken> left = first.tokens();
        const std::vector<SyntaxToken> right = second.tokens();
        return std::equal(left.begin(), left.end(), right.begin(), right.end(),
            [](const SyntaxToken& a, const SyntaxToken& b) { return a.kind == b.kind && a.text == b.text; });
    }
    if (first.kind() == PathPattern)
    {
        const std::vector<SyntaxToken> left = pathOf(first);
        const std::vector<SyntaxToken> right = pathOf(second);
        return !left.empty() && !right.empty() && nameKey(left.back().text) == nameKey(right.back().text)
            && first.nodes().empty() && second.nodes().empty();
    }
    return false;
}

bool opaque(const TypePtr& type)
{
    const TypePtr stripped = stripReferences(type);
    return !stripped || stripped->kind == TypeKind::Unknown || stripped->kind == TypeKind::Parameter
        || stripped->kind == TypeKind::Meta;
}

class CoverageChecker
{
public:
    explicit CoverageChecker(const ConstructorsOf& provider) : constructorsOf(provider)
    {
    }

    std::optional<std::vector<std::string>> missing(
        std::vector<Row> rows, const std::vector<TypePtr>& types, size_t depth)
    {
        if (types.empty())
        {
            return rows.empty() ? std::optional<std::vector<std::string>>(std::vector<std::string>{}) : std::nullopt;
        }
        if (depth > DEPTH_LIMIT)
        {
            return std::nullopt;
        }
        rows = expanded(std::move(rows));
        const std::optional<std::vector<PatternConstructor>> constructors = constructorsOf(types.front());
        const bool everyAnything =
            std::all_of(rows.begin(), rows.end(), [](const Row& row) { return matchesAnything(row.front()); });
        if (!constructors || everyAnything)
        {
            std::vector<Row> rest;
            for (const Row& row : rows)
            {
                if (matchesAnything(row.front()))
                {
                    rest.emplace_back(row.begin() + 1, row.end());
                }
            }
            if (rest.empty())
            {
                return opaque(types.front()) ? std::nullopt
                                             : std::optional<std::vector<std::string>>(wildcards(types.size()));
            }
            std::optional<std::vector<std::string>> tail = missing(std::move(rest), tailOf(types), depth + 1);
            if (tail)
            {
                tail->insert(tail->begin(), std::string(WILDCARD));
            }
            return tail;
        }
        for (const PatternConstructor& constructor : *constructors)
        {
            std::vector<Row> specialized = specialize(rows, constructor);
            const size_t width = constructor.types.size();
            if (specialized.empty())
            {
                std::vector<std::string> found = wildcards(types.size() - 1);
                found.insert(found.begin(), witness(constructor, wildcards(width)));
                return found;
            }
            std::optional<std::vector<std::string>> found =
                missing(std::move(specialized), widened(constructor, types), depth + 1);
            if (found)
            {
                const std::vector<std::string> inner(found->begin(), found->begin() + static_cast<ptrdiff_t>(width));
                std::vector<std::string> result{witness(constructor, inner)};
                result.insert(result.end(), found->begin() + static_cast<ptrdiff_t>(width), found->end());
                return result;
            }
        }
        return std::nullopt;
    }

    bool useful(std::vector<Row> rows, const Row& patterns, const std::vector<TypePtr>& types, size_t depth)
    {
        if (types.empty())
        {
            return rows.empty();
        }
        if (depth > DEPTH_LIMIT)
        {
            return true;
        }
        const SyntaxNodeView& head = patterns.front();
        if (head && head.kind() == OrPattern)
        {
            for (const SyntaxNodeView& alternative : head.nodes())
            {
                Row replaced = patterns;
                replaced.front() = alternative;
                if (useful(rows, replaced, types, depth))
                {
                    return true;
                }
            }
            return false;
        }
        rows = expanded(std::move(rows));
        const std::optional<std::vector<PatternConstructor>> constructors = constructorsOf(types.front());
        if (!constructors)
        {
            std::vector<Row> rest;
            for (const Row& row : rows)
            {
                if (matchesAnything(row.front()) || (!matchesAnything(head) && samePattern(row.front(), head)))
                {
                    rest.emplace_back(row.begin() + 1, row.end());
                }
            }
            return useful(std::move(rest), Row(patterns.begin() + 1, patterns.end()), tailOf(types), depth + 1);
        }
        if (matchesAnything(head))
        {
            for (const PatternConstructor& constructor : *constructors)
            {
                Row widenedPatterns(constructor.types.size());
                widenedPatterns.insert(widenedPatterns.end(), patterns.begin() + 1, patterns.end());
                if (useful(specialize(rows, constructor), widenedPatterns, widened(constructor, types), depth + 1))
                {
                    return true;
                }
            }
            return false;
        }
        const std::optional<std::string> key = constructorKey(head);
        const auto chosen = std::find_if(constructors->begin(), constructors->end(),
            [&](const PatternConstructor& constructor) { return key && constructor.key == *key; });
        if (chosen == constructors->end())
        {
            return true;
        }
        Row widenedPatterns = partsOf(head, *chosen);
        widenedPatterns.insert(widenedPatterns.end(), patterns.begin() + 1, patterns.end());
        return useful(specialize(rows, *chosen), widenedPatterns, widened(*chosen, types), depth + 1);
    }

private:
    static std::vector<Row> expanded(std::vector<Row> rows)
    {
        std::vector<Row> result;
        for (Row& row : rows)
        {
            const SyntaxNodeView head = row.front();
            if (head && head.kind() == OrPattern)
            {
                for (const SyntaxNodeView& alternative : head.nodes())
                {
                    Row replaced = row;
                    replaced.front() = alternative;
                    result.push_back(std::move(replaced));
                }
                continue;
            }
            result.push_back(std::move(row));
        }
        return result;
    }

    static std::vector<Row> specialize(const std::vector<Row>& rows, const PatternConstructor& constructor)
    {
        std::vector<Row> result;
        for (const Row& row : rows)
        {
            const SyntaxNodeView& head = row.front();
            Row specialized;
            if (matchesAnything(head))
            {
                specialized.resize(constructor.types.size());
            }
            else if (constructorKey(head) == constructor.key)
            {
                specialized = partsOf(head, constructor);
            }
            else
            {
                continue;
            }
            specialized.insert(specialized.end(), row.begin() + 1, row.end());
            result.push_back(std::move(specialized));
        }
        return result;
    }

    static std::vector<TypePtr> tailOf(const std::vector<TypePtr>& types)
    {
        return std::vector<TypePtr>(types.begin() + 1, types.end());
    }

    static std::vector<TypePtr> widened(const PatternConstructor& constructor, const std::vector<TypePtr>& types)
    {
        std::vector<TypePtr> result = constructor.types;
        result.insert(result.end(), types.begin() + 1, types.end());
        return result;
    }

    const ConstructorsOf& constructorsOf;
};

}

Coverage checkCoverage(
    const std::vector<ArmPattern>& arms, const TypePtr& scrutinee, const ConstructorsOf& constructorsOf)
{
    CoverageChecker checker(constructorsOf);
    Coverage coverage;
    std::vector<Row> unguarded;
    for (const ArmPattern& arm : arms)
    {
        if (!arm.guarded)
        {
            unguarded.push_back(Row{arm.pattern});
        }
    }
    if (std::optional<std::vector<std::string>> missing = checker.missing(unguarded, {scrutinee}, 0))
    {
        coverage.missing = missing->front();
    }
    std::vector<Row> covered;
    for (const ArmPattern& arm : arms)
    {
        if (!covered.empty() && !checker.useful(covered, Row{arm.pattern}, {scrutinee}, 0))
        {
            coverage.unreachable.push_back(arm.pattern);
        }
        if (!arm.guarded)
        {
            covered.push_back(Row{arm.pattern});
        }
    }
    return coverage;
}

}

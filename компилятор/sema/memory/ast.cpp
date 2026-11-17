#include "sema/memory/ast.hpp"

#include "sema/syntax_view.hpp"
#include "syntax/lexer.hpp"
#include "syntax/syntax_kind.hpp"

#include <algorithm>
#include <functional>

namespace splav::memory
{

namespace
{

using enum SyntaxKind;

struct Element
{
    SyntaxNodeView node;
    std::optional<SyntaxToken> token;

    bool is(SyntaxKind wanted) const
    {
        return node ? node.kind() == wanted : token->kind == wanted;
    }
};

std::vector<Element> elementsOf(const SyntaxNodeView& view)
{
    std::vector<Element> result;
    view.forEachChild([&result](const SyntaxNodeView& node, const std::optional<SyntaxToken>& token)
    { result.push_back(Element{node, token}); });
    return result;
}

std::optional<SyntaxToken> lastTokenOf(const SyntaxNodeView& view)
{
    std::optional<SyntaxToken> result;
    for (const Element& element : elementsOf(view))
    {
        if (element.node)
        {
            if (std::optional<SyntaxToken> inner = lastTokenOf(element.node))
            {
                result = inner;
            }
        }
        else
        {
            result = element.token;
        }
    }
    return result;
}

std::optional<SyntaxToken> firstTokenOf(const SyntaxNodeView& view)
{
    for (const Element& element : elementsOf(view))
    {
        if (element.node)
        {
            if (std::optional<SyntaxToken> inner = firstTokenOf(element.node))
            {
                return inner;
            }
        }
        else
        {
            return element.token;
        }
    }
    return std::nullopt;
}

std::string nameText(const SyntaxToken& token)
{
    if (token.kind == Identifier)
    {
        return normalizedName(token.text);
    }
    return std::string(token.text);
}

Token tokenOf(const SyntaxToken& token)
{
    return Token{token.offset, token.end(), nameText(token)};
}

std::string literalText(const SyntaxToken& token)
{
    if (token.kind == Byte)
    {
        const size_t quote = token.text.find('\'');
        return std::string(token.text.substr(quote == std::string_view::npos ? 0 : quote));
    }
    if (token.kind == RawString)
    {
        std::string result;
        size_t start = 0;
        while (start <= token.text.size())
        {
            const size_t end = std::min(token.text.find('\n', start), token.text.size());
            std::string_view line = token.text.substr(start, end - start);
            line.remove_prefix(std::min(line.find_first_not_of(" \t"), line.size()));
            if (line.starts_with("\\\\"))
            {
                line.remove_prefix(2);
            }
            result += (start == 0 ? "" : "\n") + std::string(line);
            start = end + 1;
        }
        return result;
    }
    return std::string(token.text);
}

LiteralKind literalKindOf(SyntaxKind kind)
{
    switch (kind)
    {
    case Integer:
        return LiteralKind::Integer;
    case Float:
        return LiteralKind::Float;
    case Char:
        return LiteralKind::Char;
    case Byte:
        return LiteralKind::Byte;
    case KwTrue:
    case KwFalse:
        return LiteralKind::Bool;
    default:
        return LiteralKind::String;
    }
}

}

class Lowering
{
public:
    explicit Lowering(Tree& owner) : tree(owner)
    {
    }

    const Node* module(const SyntaxNodeView& view)
    {
        Node& node = start(Kind::Module, view);
        node.lastStart = node.first;
        for (const SyntaxNodeView& child : view.nodes())
        {
            if (const Node* item = this->item(child))
            {
                node.items.push_back(item);
            }
        }
        return &node;
    }

private:
    Node& start(Kind kind, const SyntaxNodeView& view)
    {
        Node& node = tree.make(kind);
        if (const std::optional<SyntaxToken> first = firstTokenOf(view))
        {
            place(node, first->offset);
        }
        if (const std::optional<SyntaxToken> last = lastTokenOf(view))
        {
            node.last = last->end();
            node.lastStart = last->offset;
        }
        return node;
    }

    void place(Node& node, uint32_t offset)
    {
        node.first = offset;
        const auto [line, column] = tree.position(offset);
        node.line = line;
        node.column = column;
    }

    void finishAt(Node& node, const Node& last)
    {
        node.last = last.last;
        node.lastStart = last.lastStart;
    }

    std::vector<const Node*> attributesOf(const SyntaxNodeView& view)
    {
        std::vector<const Node*> result;
        for (const SyntaxNodeView& child : view.nodes())
        {
            if (child.kind() == Attribute)
            {
                result.push_back(attribute(child));
            }
        }
        return result;
    }

    void skipAttributes(Node& node, const SyntaxNodeView& view)
    {
        for (const Element& element : elementsOf(view))
        {
            if (element.node && element.node.kind() == Attribute)
            {
                continue;
            }
            const std::optional<SyntaxToken> first = element.node ? firstTokenOf(element.node) : element.token;
            if (first)
            {
                place(node, first->offset);
            }
            return;
        }
    }

    const Node* attribute(const SyntaxNodeView& view)
    {
        Node& node = start(Kind::Attribute, view);
        if (const std::optional<SyntaxToken> name = view.token(Identifier))
        {
            node.name = nameText(*name);
        }
        for (const SyntaxNodeView& argument : view.nodes())
        {
            Labeled labeled;
            if (argument.hasToken(Colon))
            {
                labeled.label = nameText(*argument.token(Identifier));
            }
            const std::vector<SyntaxNodeView> inner = argument.nodes();
            labeled.value = inner.empty() ? nullptr : expression(inner.back());
            node.labeled.push_back(labeled);
        }
        return &node;
    }

    const Node* item(const SyntaxNodeView& view)
    {
        Node* node = nullptr;
        switch (view.kind())
        {
        case Import:
            node = &importItem(view);
            break;
        case CInclude:
            node = &includeItem(view);
            break;
        case ImplDecl:
            node = &implItem(view);
            break;
        case ConstDecl:
            node = &constItem(view);
            break;
        case StructDecl:
            node = &structItem(view);
            break;
        case EnumDecl:
            node = &enumItem(view);
            break;
        case TraitDecl:
            node = &traitItem(view);
            break;
        case FunctionDecl:
            node = &functionItem(view);
            break;
        default:
            return nullptr;
        }
        node->attributes = attributesOf(view);
        skipAttributes(*node, view);
        return node;
    }

    Node& importItem(const SyntaxNodeView& view)
    {
        Node& node = start(Kind::Import, view);
        bool aliased = false;
        for (const SyntaxToken& token : view.tokens())
        {
            if (token.kind == KwAs)
            {
                aliased = true;
            }
            else if (token.kind == Identifier)
            {
                if (aliased)
                {
                    node.alias = nameText(token);
                }
                else
                {
                    node.path.push_back(nameText(token));
                }
            }
        }
        return node;
    }

    Node& includeItem(const SyntaxNodeView& view)
    {
        Node& node = start(Kind::Include, view);
        for (const SyntaxToken& token : view.tokens())
        {
            if (token.kind == String && node.text.empty())
            {
                node.text = std::string(token.text);
            }
            else if (token.kind == Identifier)
            {
                node.alias = nameText(token);
            }
        }
        return node;
    }

    Node& implItem(const SyntaxNodeView& view)
    {
        Node& node = start(Kind::Impl, view);
        std::vector<const Node*> types;
        for (const SyntaxNodeView& child : view.nodes())
        {
            if (child.kind() == ComptimeParamList)
            {
                node.ownerParameters = parameters(child);
            }
            else if (child.kind() == MethodList)
            {
                node.hasFields = true;
                for (const SyntaxNodeView& method : child.nodes())
                {
                    if (method.kind() == FunctionDecl)
                    {
                        Node& function = functionItem(method);
                        function.attributes = attributesOf(method);
                        skipAttributes(function, method);
                        node.items.push_back(&function);
                    }
                }
            }
            else if (child.kind() != Attribute)
            {
                types.push_back(type(child));
            }
        }
        node.trait = types.size() > 0 ? types[0] : nullptr;
        node.type = types.size() > 1 ? types[1] : nullptr;
        return node;
    }

    Node& constItem(const SyntaxNodeView& view)
    {
        Node& node = start(Kind::Const, view);
        node.open = view.hasToken(KwPub);
        const SyntaxToken name = *view.token(Identifier);
        node.name = nameText(name);
        node.nameToken = tokenOf(name);
        bool afterColon = false;
        bool afterEq = false;
        for (const Element& element : elementsOf(view))
        {
            if (element.token)
            {
                afterColon = afterColon || element.token->kind == Colon;
                afterEq = afterEq || element.token->kind == Eq;
                continue;
            }
            if (element.node.kind() == Attribute)
            {
                continue;
            }
            if (afterEq)
            {
                node.value = expression(element.node);
            }
            else if (afterColon)
            {
                node.type = type(element.node);
            }
        }
        return node;
    }

    Node& structItem(const SyntaxNodeView& view)
    {
        Node& node = start(view.hasToken(KwSet) ? Kind::Set : Kind::Struct, view);
        node.open = view.hasToken(KwPub);
        const SyntaxToken name = *view.token(Identifier);
        node.name = nameText(name);
        node.nameToken = tokenOf(name);
        if (const SyntaxNodeView parameters = view.node(ComptimeParamList))
        {
            node.ownerParameters = this->parameters(parameters);
        }
        node.items = fields(view.node(FieldList));
        return node;
    }

    Node& enumItem(const SyntaxNodeView& view)
    {
        Node& node = start(Kind::Enum, view);
        node.open = view.hasToken(KwPub);
        const SyntaxToken name = *view.token(Identifier);
        node.name = nameText(name);
        node.nameToken = tokenOf(name);
        if (const SyntaxNodeView parameters = view.node(ComptimeParamList))
        {
            node.ownerParameters = this->parameters(parameters);
        }
        node.items = variants(view.node(VariantList));
        return node;
    }

    Node& traitItem(const SyntaxNodeView& view)
    {
        Node& node = start(Kind::Trait, view);
        node.open = view.hasToken(KwPub);
        const SyntaxToken name = *view.token(Identifier);
        node.name = nameText(name);
        node.nameToken = tokenOf(name);
        if (const SyntaxNodeView parameters = view.node(ComptimeParamList))
        {
            node.ownerParameters = this->parameters(parameters);
        }
        if (const SyntaxNodeView list = view.node(SignatureList))
        {
            for (const SyntaxNodeView& child : list.nodes())
            {
                node.items.push_back(signature(child));
            }
        }
        return node;
    }

    Node& functionItem(const SyntaxNodeView& view)
    {
        Node& node = start(Kind::Function, view);
        node.open = view.hasToken(KwPub);
        node.signature = signature(view.node(Signature));
        if (const SyntaxNodeView body = view.node(Block))
        {
            node.body = block(body);
        }
        return node;
    }

    std::vector<const Node*> fields(const SyntaxNodeView& list)
    {
        std::vector<const Node*> result;
        if (!list)
        {
            return result;
        }
        for (const SyntaxNodeView& child : list.nodes())
        {
            if (child.kind() != Field)
            {
                continue;
            }
            Node& node = start(Kind::Field, child);
            node.attributes = attributesOf(child);
            skipAttributes(node, child);
            node.open = child.hasToken(KwPub);
            const SyntaxToken name = *child.token(Identifier);
            node.name = nameText(name);
            node.nameToken = tokenOf(name);
            for (const SyntaxNodeView& part : child.nodes())
            {
                if (part.kind() != Attribute)
                {
                    node.type = type(part);
                }
            }
            result.push_back(&node);
        }
        return result;
    }

    std::vector<const Node*> variants(const SyntaxNodeView& list)
    {
        std::vector<const Node*> result;
        if (!list)
        {
            return result;
        }
        for (const SyntaxNodeView& child : list.nodes())
        {
            if (child.kind() != Variant)
            {
                continue;
            }
            Node& node = start(Kind::Variant, child);
            const SyntaxToken name = *child.token(Identifier);
            node.name = nameText(name);
            node.nameToken = tokenOf(name);
            node.hasFields = child.hasToken(LParen);
            for (const SyntaxNodeView& field : child.nodes())
            {
                Labeled labeled;
                if (field.hasToken(Colon))
                {
                    labeled.label = nameText(*field.token(Identifier));
                }
                const std::vector<SyntaxNodeView> inner = field.nodes();
                labeled.value = inner.empty() ? nullptr : type(inner.back());
                node.labeled.push_back(labeled);
            }
            result.push_back(&node);
        }
        return result;
    }

    std::vector<const Node*> parameters(const SyntaxNodeView& list)
    {
        std::vector<const Node*> result;
        for (const SyntaxNodeView& child : list.nodes())
        {
            if (child.kind() == Param)
            {
                result.push_back(parameter(child));
            }
        }
        return result;
    }

    const Node* parameter(const SyntaxNodeView& view)
    {
        Node& node = start(Kind::Parameter, view);
        node.comptime = view.hasToken(KwComptime);
        node.mutableAccess = view.hasToken(KwMut);
        if (view.hasToken(KwSelf))
        {
            node.name = "сам";
            node.nameToken = tokenOf(*view.token(KwSelf));
            node.op = view.hasToken(Amp) ? (node.mutableAccess ? "&изм сам" : "&сам") : "сам";
            return &node;
        }
        if (!view.hasToken(Identifier))
        {
            node.op = "...";
            node.nameToken = tokenOf(*view.firstToken());
            return &node;
        }
        const SyntaxToken name = *view.token(Identifier);
        node.name = nameText(name);
        node.nameToken = tokenOf(name);
        node.op = view.hasToken(Ellipsis) ? "пакет" : "обычный";
        bool afterEq = false;
        for (const Element& element : elementsOf(view))
        {
            if (element.token)
            {
                afterEq = afterEq || element.token->kind == Eq;
            }
            else if (!afterEq)
            {
                node.type = type(element.node);
            }
        }
        return &node;
    }

    const Node* signature(const SyntaxNodeView& view)
    {
        Node& node = start(Kind::Signature, view);
        std::optional<SyntaxToken> first;
        std::optional<SyntaxToken> method;
        bool dotted = false;
        bool requiring = false;
        bool arrow = false;
        for (const Element& element : elementsOf(view))
        {
            if (element.node)
            {
                if (element.node.kind() == ComptimeParamList)
                {
                    node.ownerParameters = parameters(element.node);
                }
                else if (element.node.kind() == ParamList)
                {
                    node.items = parameters(element.node);
                }
                else if (arrow)
                {
                    node.result = type(element.node);
                }
                continue;
            }
            const SyntaxToken& token = *element.token;
            if (token.kind == Dot)
            {
                dotted = true;
            }
            else if (token.kind == KwRequires)
            {
                requiring = true;
            }
            else if (token.kind == Arrow)
            {
                arrow = true;
            }
            else if (token.kind == Identifier)
            {
                if (requiring)
                {
                    node.requirement = nameText(token);
                }
                else if (!first)
                {
                    first = token;
                }
                else if (dotted && !method)
                {
                    method = token;
                }
            }
        }
        if (method)
        {
            node.owner = nameText(*first);
            node.name = nameText(*method);
            node.nameToken = tokenOf(*method);
        }
        else if (first)
        {
            node.name = nameText(*first);
            node.nameToken = tokenOf(*first);
        }
        return &node;
    }

    const Node* block(const SyntaxNodeView& view)
    {
        Node& node = start(Kind::Block, view);
        for (const SyntaxNodeView& child : view.nodes())
        {
            node.items.push_back(statement(child));
        }
        return &node;
    }

    const Node* statement(const SyntaxNodeView& view)
    {
        switch (view.kind())
        {
        case BindingStmt:
            return binding(view);
        case AssignStmt:
        {
            Node& node = start(Kind::Assign, view);
            const std::vector<SyntaxNodeView> parts = view.nodes();
            node.left = expression(parts[0]);
            node.value = parts.size() > 1 ? expression(parts[1]) : nullptr;
            for (const SyntaxToken& token : view.tokens())
            {
                node.op = std::string(fixedText(token.kind));
            }
            return &node;
        }
        case ExprStmt:
            return wrapped(Kind::Expression, view);
        case DiscardStmt:
            return wrapped(Kind::Discard, view);
        case ReturnStmt:
            return wrapped(Kind::Return, view);
        case DeferStmt:
            return wrapped(Kind::Defer, view);
        case SpawnStmt:
            return wrapped(Kind::Spawn, view);
        case BreakStmt:
        case ContinueStmt:
        {
            Node& node = start(view.kind() == BreakStmt ? Kind::Break : Kind::Continue, view);
            if (const std::optional<SyntaxToken> label = view.token(Identifier))
            {
                node.label = nameText(*label);
            }
            return &node;
        }
        case IfStmt:
            return ifStatement(view);
        case LoopStmt:
            return loop(view);
        case RegionStmt:
        {
            Node& node = start(Kind::Region, view);
            if (const std::optional<SyntaxToken> name = view.token(Identifier))
            {
                node.name = nameText(*name);
            }
            for (const SyntaxNodeView& child : view.nodes())
            {
                if (child.kind() == Block)
                {
                    node.body = block(child);
                }
                else
                {
                    node.device = expression(child);
                }
            }
            return &node;
        }
        case ThroughStmt:
        {
            Node& node = start(Kind::Through, view);
            const std::vector<SyntaxNodeView> parts = view.nodes();
            node.value = expression(parts[0]);
            node.body = parts.size() > 1 ? block(parts[1]) : nullptr;
            return &node;
        }
        case TogetherStmt:
        case UnsafeStmt:
        {
            Node& node = start(view.kind() == TogetherStmt ? Kind::TogetherBlock : Kind::UnsafeBlock, view);
            node.body = block(view.node(Block));
            return &node;
        }
        default:
            return expression(view);
        }
    }

    const Node* wrapped(Kind kind, const SyntaxNodeView& view)
    {
        Node& node = start(kind, view);
        const std::vector<SyntaxNodeView> parts = view.nodes();
        node.value = parts.empty() ? nullptr : expression(parts.front());
        return &node;
    }

    const Node* binding(const SyntaxNodeView& view)
    {
        Node& node = start(Kind::Binding, view);
        node.mutableAccess = view.hasToken(KwMut);
        const SyntaxToken name = *view.token(Identifier);
        node.name = nameText(name);
        node.nameToken = tokenOf(name);
        bool afterColon = false;
        bool afterEq = false;
        for (const Element& element : elementsOf(view))
        {
            if (element.token)
            {
                afterColon = afterColon || element.token->kind == Colon;
                afterEq = afterEq || element.token->kind == Eq;
            }
            else if (afterEq)
            {
                node.value = expression(element.node);
            }
            else if (afterColon)
            {
                node.type = type(element.node);
            }
        }
        return &node;
    }

    const Node* ifStatement(const SyntaxNodeView& view)
    {
        Node& node = start(Kind::If, view);
        node.comptime = view.hasToken(KwComptime);
        place(node, view.token(KwIf)->offset);
        const std::vector<SyntaxNodeView> parts = view.nodes();
        node.condition = expression(parts[0]);
        node.then = parts.size() > 1 ? block(parts[1]) : nullptr;
        if (parts.size() > 2)
        {
            node.otherwise = parts[2].kind() == IfStmt ? ifStatement(parts[2]) : block(parts[2]);
        }
        return &node;
    }

    const Node* loop(const SyntaxNodeView& view)
    {
        Node& node = start(Kind::Loop, view);
        node.comptime = view.hasToken(KwComptime);
        node.mutableAccess = view.hasToken(KwMut);
        bool beforeLoop = true;
        for (const SyntaxToken& token : view.tokens())
        {
            if (token.kind == KwLoop)
            {
                beforeLoop = false;
            }
            else if (beforeLoop && token.kind == Identifier)
            {
                node.label = nameText(token);
            }
        }
        const bool iteration = view.hasToken(KwIn);
        for (const SyntaxNodeView& child : view.nodes())
        {
            if (child.kind() == Block)
            {
                node.body = block(child);
            }
            else if (iteration && !node.pattern)
            {
                node.pattern = loopPattern(child);
            }
            else if (iteration)
            {
                node.source = expression(child);
            }
            else
            {
                node.condition = expression(child);
            }
        }
        return &node;
    }

    const Node* loopPattern(const SyntaxNodeView& view)
    {
        if (view.kind() == WildcardPattern)
        {
            return &start(Kind::AnyPattern, view);
        }
        if (view.kind() == TuplePattern)
        {
            Node& node = start(Kind::TuplePattern, view);
            for (const SyntaxNodeView& child : view.nodes())
            {
                node.items.push_back(loopPattern(child));
            }
            return &node;
        }
        Node& node = start(Kind::NamePattern, view);
        node.path.push_back(nameText(*view.token(Identifier)));
        return &node;
    }

    const Node* expression(const SyntaxNodeView& view)
    {
        switch (view.kind())
        {
        case Literal:
        {
            Node& node = start(Kind::Literal, view);
            const SyntaxToken token = view.tokens().front();
            node.literal = literalKindOf(token.kind);
            node.text = literalText(token);
            return &node;
        }
        case NameExpr:
        {
            Node& node = start(Kind::Name, view);
            node.name = nameText(view.tokens().front());
            return &node;
        }
        case Placeholder:
            return &start(Kind::Placeholder, view);
        case ParenExpr:
        {
            Node& node = start(Kind::Paren, view);
            node.value = expression(view.nodes().front());
            return &node;
        }
        case TupleExpr:
        {
            Node& node = start(Kind::Tuple, view);
            for (const SyntaxNodeView& child : view.nodes())
            {
                node.items.push_back(expression(child));
            }
            return &node;
        }
        case FieldExpr:
        {
            Node& node = start(Kind::Member, view);
            node.object = expression(view.nodes().front());
            const SyntaxToken name = view.tokens().back();
            node.name = nameText(name);
            node.nameToken = tokenOf(name);
            return &node;
        }
        case CallExpr:
        {
            Node& node = start(Kind::Call, view);
            node.function = expression(view.nodes().front());
            for (const SyntaxNodeView& argument : view.node(ArgList).nodes())
            {
                node.items.push_back(expression(argument));
            }
            return &node;
        }
        case IndexExpr:
        {
            Node& node = start(Kind::Index, view);
            const std::vector<SyntaxNodeView> parts = view.nodes();
            node.object = expression(parts[0]);
            node.index = expression(parts[1]);
            return &node;
        }
        case RefExpr:
        {
            Node& node = start(Kind::Reference, view);
            if (const std::optional<SyntaxToken> label = view.token(Identifier))
            {
                node.label = nameText(*label);
            }
            node.mutableAccess = view.hasToken(KwMut);
            node.operand = expression(view.nodes().back());
            return &node;
        }
        case PrefixExpr:
        {
            Node& node = start(Kind::Prefix, view);
            node.op = std::string(fixedText(view.tokens().front().kind));
            node.operand = expression(view.nodes().back());
            return &node;
        }
        case BinaryExpr:
        case LogicalExpr:
            return chain(view.kind() == BinaryExpr ? Kind::Binary : Kind::Logical, view);
        case NotExpr:
        {
            Node& node = start(Kind::Not, view);
            node.operand = expression(view.nodes().back());
            return &node;
        }
        case ComparisonExpr:
        {
            Node& node = start(Kind::Comparison, view);
            const std::vector<SyntaxNodeView> parts = view.nodes();
            node.left = expression(parts[0]);
            node.right = expression(parts[1]);
            node.op = std::string(fixedText(view.tokens().front().kind));
            return &node;
        }
        case MembershipExpr:
        {
            Node& node = start(Kind::Membership, view);
            const std::vector<SyntaxNodeView> parts = view.nodes();
            node.left = expression(parts[0]);
            node.right = expression(parts[1]);
            node.negated = view.hasToken(KwNot);
            return &node;
        }
        case IsExpr:
        {
            Node& node = start(Kind::Is, view);
            const std::vector<SyntaxNodeView> parts = view.nodes();
            node.value = expression(parts[0]);
            node.pattern = pattern(parts[1]);
            node.negated = view.hasToken(KwNot);
            return &node;
        }
        case RangeExpr:
            return range(view);
        case CastExpr:
        {
            const std::vector<SyntaxNodeView> parts = view.nodes();
            const Node* value = expression(parts[0]);
            for (size_t index = 1; index < parts.size(); ++index)
            {
                Node& node = tree.make(Kind::Cast);
                place(node, value->first);
                node.value = value;
                node.type = type(parts[index]);
                finishAt(node, *node.type);
                value = &node;
            }
            return value;
        }
        case CompoundLiteral:
        {
            Node& node = start(Kind::CompoundLiteral, view);
            for (const SyntaxNodeView& child : view.nodes())
            {
                if (child.kind() != LiteralElement)
                {
                    node.type = expression(child);
                    continue;
                }
                Labeled labeled;
                if (child.hasToken(Colon))
                {
                    const SyntaxToken label = *child.token(Identifier);
                    labeled.label = nameText(label);
                    labeled.token = tokenOf(label);
                }
                labeled.value = expression(child.nodes().back());
                node.labeled.push_back(labeled);
            }
            return &node;
        }
        case MatchExpr:
            return match(view);
        case FunctionExpr:
            return functionExpression(view);
        case TogetherExpr:
        {
            Node& node = start(Kind::Together, view);
            if (const std::optional<SyntaxToken> form = view.token(Identifier))
            {
                node.op = nameText(*form);
            }
            for (const SyntaxNodeView& argument : view.node(ArgList).nodes())
            {
                node.items.push_back(expression(argument));
            }
            return &node;
        }
        case AsmExpr:
        {
            Node& node = start(Kind::Asm, view);
            for (const SyntaxNodeView& operand : view.nodes())
            {
                Labeled labeled;
                labeled.label = nameText(*operand.token(Identifier));
                labeled.token = tokenOf(*operand.token(String));
                labeled.value = expression(operand.nodes().back());
                node.labeled.push_back(labeled);
            }
            return &node;
        }
        case StructType:
        {
            Node& node = start(view.hasToken(KwSet) ? Kind::SetType : Kind::StructType, view);
            node.items = fields(view.node(FieldList));
            return &node;
        }
        case EnumType:
        {
            Node& node = start(Kind::EnumType, view);
            node.items = variants(view.node(VariantList));
            return &node;
        }
        case OptionalType:
        case DynType:
        case PointerType:
        case SliceType:
        case ArrayType:
        case SentinelType:
            return type(view);
        default:
            return &start(Kind::Placeholder, view);
        }
    }

    const Node* chain(Kind kind, const SyntaxNodeView& view)
    {
        const Node* left = nullptr;
        std::string op;
        for (const Element& element : elementsOf(view))
        {
            if (element.token)
            {
                op = std::string(fixedText(element.token->kind));
                continue;
            }
            const Node* operand = expression(element.node);
            if (!left)
            {
                left = operand;
                continue;
            }
            Node& node = tree.make(kind);
            place(node, left->first);
            node.left = left;
            node.right = operand;
            node.op = op;
            finishAt(node, *operand);
            left = &node;
        }
        return left;
    }

    const Node* range(const SyntaxNodeView& view)
    {
        Node& node = start(Kind::Range, view);
        node.inclusive = view.hasToken(DotDotEq);
        bool afterDots = false;
        for (const Element& element : elementsOf(view))
        {
            if (element.token)
            {
                afterDots = afterDots || element.token->kind == DotDot || element.token->kind == DotDotEq;
            }
            else if (afterDots)
            {
                node.to = expression(element.node);
            }
            else
            {
                node.from = expression(element.node);
            }
        }
        return &node;
    }

    const Node* match(const SyntaxNodeView& view)
    {
        Node& node = start(Kind::Match, view);
        for (const SyntaxNodeView& child : view.nodes())
        {
            if (child.kind() != MatchArm)
            {
                node.value = expression(child);
                continue;
            }
            Node& arm = start(Kind::Arm, child);
            const std::vector<SyntaxNodeView> parts = child.nodes();
            arm.pattern = pattern(parts[0]);
            const bool guarded = child.hasToken(KwIf);
            if (guarded && parts.size() > 1)
            {
                arm.guard = expression(parts[1]);
            }
            const SyntaxNodeView body = parts.back();
            arm.body = body.kind() == Block ? block(body) : statement(body);
            node.items.push_back(&arm);
        }
        return &node;
    }

    const Node* functionExpression(const SyntaxNodeView& view)
    {
        const bool closure = view.hasToken(FatArrow) || view.node(Block);
        Node& node = start(closure ? Kind::Closure : Kind::FunctionType, view);
        bool capture = false;
        bool arrow = false;
        bool fat = false;
        for (const Element& element : elementsOf(view))
        {
            if (element.token)
            {
                const SyntaxKind kind = element.token->kind;
                if (kind == LBracket)
                {
                    capture = true;
                }
                else if (kind == RBracket)
                {
                    capture = false;
                }
                else if (capture && kind == Identifier)
                {
                    node.op = nameText(*element.token);
                }
                arrow = arrow || kind == Arrow;
                fat = fat || kind == FatArrow;
                continue;
            }
            if (element.node.kind() == FunctionParam)
            {
                node.items.push_back(functionParameter(element.node));
            }
            else if (element.node.kind() == Block)
            {
                node.body = block(element.node);
                node.text = "блок";
            }
            else if (fat)
            {
                node.body = expression(element.node);
                node.text = "выражение";
            }
            else if (arrow)
            {
                node.result = type(element.node);
            }
        }
        return &node;
    }

    const Node* functionParameter(const SyntaxNodeView& view)
    {
        Node& node = start(Kind::Parameter, view);
        node.op = "обычный";
        node.mutableAccess = view.hasToken(KwMut);
        if (view.hasToken(Colon))
        {
            const SyntaxToken name = *view.token(Identifier);
            node.name = nameText(name);
            node.nameToken = tokenOf(name);
        }
        else
        {
            node.nameToken = tokenOf(*firstTokenOf(view));
        }
        const std::vector<SyntaxNodeView> parts = view.nodes();
        if (!parts.empty())
        {
            node.type = type(parts.back());
        }
        return &node;
    }

    const Node* type(const SyntaxNodeView& view)
    {
        switch (view.kind())
        {
        case OptionalType:
        case DynType:
        case PointerType:
        {
            const Kind kind = view.kind() == OptionalType ? Kind::OptionalType
                : view.kind() == DynType                  ? Kind::DynType
                                                          : Kind::RawPointer;
            Node& node = start(kind, view);
            node.operand = type(view.nodes().back());
            return &node;
        }
        case SliceType:
        case ArrayType:
        case SentinelType:
        {
            Kind kind = Kind::SliceType;
            if (view.kind() == ArrayType)
            {
                kind = Kind::ArrayType;
            }
            else if (view.kind() == SentinelType)
            {
                kind = hasStarInBrackets(view) ? Kind::SentinelPointer : Kind::SentinelSlice;
            }
            Node& node = start(kind, view);
            node.mutableAccess = view.hasToken(KwMut);
            const std::vector<SyntaxNodeView> parts = view.nodes();
            node.operand = type(parts.back());
            if (kind == Kind::ArrayType && parts.size() > 1)
            {
                node.length = expression(parts.front());
            }
            return &node;
        }
        case RefExpr:
        {
            Node& node = start(Kind::Reference, view);
            if (const std::optional<SyntaxToken> label = view.token(Identifier))
            {
                node.label = nameText(*label);
            }
            node.mutableAccess = view.hasToken(KwMut);
            node.operand = type(view.nodes().back());
            return &node;
        }
        case FunctionExpr:
            return functionExpression(view);
        case TupleExpr:
        {
            Node& node = start(Kind::Tuple, view);
            for (const SyntaxNodeView& child : view.nodes())
            {
                node.items.push_back(type(child));
            }
            return &node;
        }
        case ParenExpr:
        {
            Node& node = start(Kind::Paren, view);
            node.value = type(view.nodes().front());
            return &node;
        }
        case CallExpr:
        {
            Node& node = start(Kind::Call, view);
            node.function = type(view.nodes().front());
            for (const SyntaxNodeView& argument : view.node(ArgList).nodes())
            {
                node.items.push_back(expression(argument));
            }
            return &node;
        }
        case FieldExpr:
        {
            Node& node = start(Kind::Member, view);
            node.object = type(view.nodes().front());
            const SyntaxToken name = view.tokens().back();
            node.name = nameText(name);
            node.nameToken = tokenOf(name);
            return &node;
        }
        default:
            return expression(view);
        }
    }

    static bool hasStarInBrackets(const SyntaxNodeView& view)
    {
        for (const SyntaxToken& token : view.tokens())
        {
            if (token.kind == Star)
            {
                return true;
            }
            if (token.kind == Colon)
            {
                return false;
            }
        }
        return false;
    }

    const Node* patternLiteral(const std::vector<SyntaxToken>& tokens, size_t& index)
    {
        Node& node = tree.make(Kind::Literal);
        place(node, tokens[index].offset);
        std::string prefix;
        if (tokens[index].kind == Minus)
        {
            prefix = "-";
            ++index;
        }
        const SyntaxToken& token = tokens[index];
        node.literal = literalKindOf(token.kind);
        node.text = prefix + literalText(token);
        node.last = token.end();
        node.lastStart = token.offset;
        ++index;
        return &node;
    }

    const Node* pattern(const SyntaxNodeView& view)
    {
        switch (view.kind())
        {
        case WildcardPattern:
            return &start(Kind::AnyPattern, view);
        case LiteralPattern:
        {
            Node& node = start(Kind::LiteralPattern, view);
            const std::vector<SyntaxToken> tokens = view.tokens();
            size_t index = 0;
            node.from = patternLiteral(tokens, index);
            if (index < tokens.size())
            {
                node.inclusive = tokens[index].kind == DotDotEq;
                ++index;
                node.to = patternLiteral(tokens, index);
            }
            return &node;
        }
        case BindingPattern:
        {
            Node& node = start(Kind::MutPattern, view);
            node.name = nameText(*view.token(Identifier));
            return &node;
        }
        case TuplePattern:
        {
            const std::vector<SyntaxNodeView> parts = view.nodes();
            if (parts.size() == 1 && !view.hasToken(Comma))
            {
                return pattern(parts.front());
            }
            Node& node = start(Kind::TuplePattern, view);
            for (const SyntaxNodeView& child : parts)
            {
                node.items.push_back(pattern(child));
            }
            return &node;
        }
        case OrPattern:
        {
            Node& node = start(Kind::OrPattern, view);
            for (const SyntaxNodeView& child : view.nodes())
            {
                node.items.push_back(pattern(child));
            }
            return &node;
        }
        default:
            return pathPattern(view);
        }
    }

    const Node* pathPattern(const SyntaxNodeView& view)
    {
        Node& node = start(Kind::NamePattern, view);
        for (const SyntaxToken& token : view.tokens())
        {
            if (token.kind == Identifier)
            {
                node.path.push_back(nameText(token));
            }
        }
        if (const SyntaxNodeView arguments = view.node(PatternArgList))
        {
            node.hasArguments = true;
            for (const SyntaxNodeView& argument : arguments.nodes())
            {
                Labeled labeled;
                if (argument.hasToken(Colon))
                {
                    labeled.label = nameText(*argument.token(Identifier));
                }
                labeled.value = pattern(argument.nodes().back());
                node.labeled.push_back(labeled);
            }
        }
        if (const SyntaxNodeView fields = view.node(PatternFieldList))
        {
            node.hasFields = true;
            for (const SyntaxNodeView& field : fields.nodes())
            {
                Labeled labeled;
                const SyntaxToken name = *field.token(Identifier);
                labeled.label = nameText(name);
                labeled.token = tokenOf(name);
                const std::vector<SyntaxNodeView> inner = field.nodes();
                labeled.value = inner.empty() ? nullptr : pattern(inner.back());
                node.named.push_back(labeled);
            }
        }
        return &node;
    }

    Tree& tree;
};

Token Node::lastToken(std::string_view sourceText) const
{
    const std::string_view rest = sourceText.substr(lastStart, last - lastStart);
    return Token{lastStart, last, std::string(rest)};
}

Tree::Tree(std::string_view source, const GreenNode& root) : sourceText(source)
{
    lineStarts.push_back(0);
    for (size_t index = 0; index < source.size(); ++index)
    {
        if (source[index] == '\n')
        {
            lineStarts.push_back(static_cast<uint32_t>(index + 1));
        }
    }
    Lowering lowering(*this);
    top = lowering.module(SyntaxNodeView(&root, 0));
}

const Node* Tree::root() const
{
    return top;
}

std::string_view Tree::source() const
{
    return sourceText;
}

std::string Tree::text(const Node& node) const
{
    return std::string(sourceText.substr(node.first, node.last - node.first));
}

std::string Tree::firstLine(const Node& node) const
{
    std::string whole = text(node);
    return whole.substr(0, whole.find('\n'));
}

std::pair<uint32_t, uint32_t> Tree::position(uint32_t offset) const
{
    const auto after = std::upper_bound(lineStarts.begin(), lineStarts.end(), offset);
    const uint32_t lineStart = *(after - 1);
    uint32_t column = 1;
    for (uint32_t index = lineStart; index < offset && index < sourceText.size(); ++index)
    {
        if ((static_cast<unsigned char>(sourceText[index]) & 0xC0) != 0x80)
        {
            ++column;
        }
    }
    return {static_cast<uint32_t>(after - lineStarts.begin()), column};
}

Node& Tree::make(Kind kind)
{
    Node& node = nodes.emplace_back();
    node.kind = kind;
    return node;
}

}

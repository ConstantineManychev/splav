#include "sema/checker.hpp"

#include "sema/declarations.hpp"
#include "sema/naming.hpp"
#include "sema/syntax_view.hpp"
#include "sema/types.hpp"
#include "syntax/lexer.hpp"

#include <algorithm>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace splav
{

using SourceModule = Module;

namespace
{

using enum SyntaxKind;

struct LocalInfo
{
    TypePtr type;
};

struct TypeContext
{
    SourceModule* module = nullptr;
    std::map<std::string, TypePtr> substitution;
    TypePtr selfType;
};

enum class Denotes
{
    Value,
    Type,
    Module,
    Function,
    Header,
};

struct Denoted
{
    Denotes kind = Denotes::Value;
    TypePtr type;
    SourceModule* module = nullptr;
    const FunctionInfo* function = nullptr;
};

class Checker
{
public:
    Checker(Program& owner, SourceModule& checked, Language chosen)
        : program(owner), module(checked), language(chosen), declarations(owner, checked)
    {
    }

    void attach(const EvaluatorFactory& evaluators)
    {
        if (evaluators)
        {
            evaluator = evaluators(program, module, declarations);
        }
    }

    std::vector<Report> run()
    {
        const SyntaxNodeView root(module.root.get(), 0);
        for (const SyntaxNodeView& item : root.nodes())
        {
            checkItem(item);
        }
        std::stable_sort(reports.begin(), reports.end(),
            [](const Report& left, const Report& right) { return left.offset < right.offset; });
        return std::move(reports);
    }

private:
    std::string say(std::string_view russian, std::string_view english) const
    {
        return std::string(language == Language::Russian ? russian : english);
    }

    std::string show(const TypePtr& type) const
    {
        return typeText(type, &module, declarations.prelude);
    }

    std::string text(const SyntaxNodeView& node) const
    {
        return firstLine(module.source, node.offset(), node.end());
    }

    void report(
        uint16_t code, uint32_t offset, uint32_t length, std::string message, std::vector<ReportNote> notes = {})
    {
        reports.push_back(Report{code, offset, length, std::move(message), std::move(notes)});
    }

    void reportNode(uint16_t code, const SyntaxNodeView& node, std::string message, std::vector<ReportNote> notes = {})
    {
        const uint32_t end = node.offset() + static_cast<uint32_t>(text(node).size());
        report(code, node.offset(), end - node.offset(), std::move(message), std::move(notes));
    }

    bool isResultType(const TypePtr& type) const
    {
        return type && type->kind == TypeKind::Named && type->decl->key == RESULT_TYPE
            && type->decl->module == declarations.prelude;
    }

    TypePtr resolveType(const SyntaxNodeView& node, const TypeContext& context)
    {
        if (!node)
        {
            return unknownType();
        }
        switch (node.kind())
        {
        case NameExpr:
        {
            if (node.hasToken(KwSelfType))
            {
                return context.selfType ? context.selfType : unknownType();
            }
            const std::optional<SyntaxToken> name = node.token(Identifier);
            if (!name)
            {
                return unknownType();
            }
            const std::string key = nameKey(name->text);
            if (const auto found = context.substitution.find(key); found != context.substitution.end())
            {
                return found->second;
            }
            if (const Entry* entry = declarations.lookup(context.module, key))
            {
                if (entry->kind == EntryKind::Type)
                {
                    return namedType(entry->type, std::vector<TypePtr>(entry->type->parameters.size(), unknownType()));
                }
                if (entry->kind == EntryKind::Const)
                {
                    return aliasedType(entry->constant, context);
                }
            }
            return builtinType(key);
        }
        case FieldExpr:
        {
            if (const TypeDecl* decl = memberType(context.module, node))
            {
                return namedType(decl, std::vector<TypePtr>(decl->parameters.size(), unknownType()));
            }
            return unknownType();
        }
        case CallExpr:
            return resolveGeneric(node, context);
        case RefExpr:
        {
            const std::vector<SyntaxNodeView> parts = node.nodes();
            return referenceTo(
                parts.empty() ? unknownType() : resolveType(parts.back(), context), node.hasToken(KwMut));
        }
        case OptionalType:
        {
            const std::vector<SyntaxNodeView> parts = node.nodes();
            return optionalOf(parts.empty() ? unknownType() : resolveType(parts.back(), context));
        }
        case SliceType:
        case ArrayType:
        {
            const std::vector<SyntaxNodeView> parts = node.nodes();
            auto type = std::make_shared<Type>();
            type->kind = node.kind() == SliceType ? TypeKind::Slice : TypeKind::Array;
            type->mutableAccess = node.hasToken(KwMut);
            type->inner = parts.empty() ? unknownType() : resolveType(parts.back(), context);
            if (node.kind() == ArrayType && parts.size() >= 2)
            {
                type->builtin = text(parts.front());
            }
            return type;
        }
        case PointerType:
        {
            const std::vector<SyntaxNodeView> parts = node.nodes();
            auto type = std::make_shared<Type>();
            type->kind = TypeKind::Pointer;
            type->inner = parts.empty() ? unknownType() : resolveType(parts.back(), context);
            return type;
        }
        case TupleExpr:
        {
            auto type = std::make_shared<Type>();
            type->kind = TypeKind::Tuple;
            for (const SyntaxNodeView& element : node.nodes())
            {
                type->arguments.push_back(resolveType(element, context));
            }
            return type;
        }
        case ParenExpr:
        {
            const std::vector<SyntaxNodeView> parts = node.nodes();
            return parts.empty() ? unknownType() : resolveType(parts.front(), context);
        }
        default:
            return unknownType();
        }
    }

    TypePtr aliasedType(const SyntaxNodeView& constant, const TypeContext& context)
    {
        if (constant.hasToken(Colon) || !aliasesInProgress.insert(constant.offset()).second)
        {
            return unknownType();
        }
        SyntaxNodeView initializer;
        for (const SyntaxNodeView& part : constant.nodes())
        {
            if (part.kind() != Attribute)
            {
                initializer = part;
            }
        }
        const bool typeLike = initializer
            && (initializer.kind() == CallExpr || initializer.kind() == NameExpr || initializer.kind() == FieldExpr);
        const TypePtr type =
            typeLike ? resolveType(initializer, TypeContext{context.module, {}, nullptr}) : unknownType();
        aliasesInProgress.erase(constant.offset());
        return type;
    }

    static bool returnsType(const FunctionInfo& function)
    {
        bool afterArrow = false;
        bool result = false;
        function.signature.forEachChild([&](const SyntaxNodeView& child, const std::optional<SyntaxToken>& token)
        {
            if (token)
            {
                afterArrow = afterArrow || token->kind == Arrow;
                return;
            }
            if (afterArrow && child.kind() == NameExpr && child.token(Identifier))
            {
                result = nameKey(child.token(Identifier)->text) == "тип";
            }
        });
        return result;
    }

    TypePtr evaluateTypeCall(
        const FunctionInfo& function, const std::vector<SyntaxNodeView>& argumentNodes, const TypeContext& context)
    {
        if (!evaluator)
        {
            return unknownType();
        }
        std::vector<TypePtr> arguments;
        for (const SyntaxNodeView& argument : argumentNodes)
        {
            TypePtr type = resolveType(argument, context);
            if (!isKnown(type))
            {
                return unknownType();
            }
            arguments.push_back(std::move(type));
        }
        return evaluator->evaluateType(function, arguments);
    }

    const TypeDecl* memberType(SourceModule* context, const SyntaxNodeView& node)
    {
        const std::vector<SyntaxNodeView> parts = node.nodes();
        const std::optional<SyntaxToken> member = node.token(Identifier);
        if (parts.empty() || !member || parts.front().kind() != NameExpr)
        {
            return nullptr;
        }
        const std::optional<SyntaxToken> head = parts.front().token(Identifier);
        const Entry* entry = head ? declarations.lookup(context, nameKey(head->text)) : nullptr;
        if (entry == nullptr || entry->kind != EntryKind::Module || entry->target == nullptr)
        {
            return nullptr;
        }
        const Entry* found = declarations.lookup(entry->target, nameKey(member->text));
        return found != nullptr && found->kind == EntryKind::Type ? found->type : nullptr;
    }

    TypePtr resolveGeneric(const SyntaxNodeView& node, const TypeContext& context)
    {
        const std::vector<SyntaxNodeView> parts = node.nodes();
        if (parts.empty())
        {
            return unknownType();
        }
        const SyntaxNodeView callee = parts.front();
        const TypeDecl* decl = nullptr;
        if (callee.kind() == NameExpr)
        {
            const std::optional<SyntaxToken> name = callee.token(Identifier);
            if (name && nameKey(name->text) == "Вектор")
            {
                auto vector = std::make_shared<Type>();
                vector->kind = TypeKind::Vector;
                const SyntaxNodeView arguments = node.node(ArgList);
                const std::vector<SyntaxNodeView> values =
                    arguments ? arguments.nodes() : std::vector<SyntaxNodeView>();
                vector->inner = values.empty() ? unknownType() : resolveType(values.front(), context);
                vector->builtin = values.size() > 1 ? text(values[1]) : "_";
                return vector;
            }
            const Entry* entry = name ? declarations.lookup(context.module, nameKey(name->text)) : nullptr;
            decl = entry != nullptr && entry->kind == EntryKind::Type ? entry->type : nullptr;
            if (entry != nullptr && entry->kind == EntryKind::Function && returnsType(*entry->function))
            {
                const SyntaxNodeView arguments = node.node(ArgList);
                return evaluateTypeCall(
                    *entry->function, arguments ? arguments.nodes() : std::vector<SyntaxNodeView>(), context);
            }
        }
        else if (callee.kind() == FieldExpr)
        {
            decl = memberType(context.module, callee);
        }
        if (decl == nullptr)
        {
            return unknownType();
        }
        std::vector<TypePtr> arguments;
        std::vector<SyntaxNodeView> argumentNodes;
        if (const SyntaxNodeView list = node.node(ArgList))
        {
            argumentNodes = list.nodes();
        }
        for (size_t index = 0; index < decl->parameters.size(); ++index)
        {
            arguments.push_back(
                index < argumentNodes.size() ? resolveType(argumentNodes[index], context) : unknownType());
        }
        checkBounds(decl, arguments, argumentNodes);
        return namedType(decl, arguments);
    }

    void checkBounds(
        const TypeDecl* decl, const std::vector<TypePtr>& arguments, const std::vector<SyntaxNodeView>& nodes)
    {
        for (size_t index = 0; index < decl->bounds.size() && index < arguments.size() && index < nodes.size(); ++index)
        {
            if (!decl->bounds[index] || !isKnown(arguments[index]))
            {
                continue;
            }
            TypeContext boundContext{decl->module, {}, nullptr};
            const TypePtr bound = resolveType(decl->bounds[index], boundContext);
            if (!bound || bound->kind != TypeKind::Named || bound->decl->kind != DeclKind::Trait)
            {
                continue;
            }
            if (implements(arguments[index], bound->decl))
            {
                continue;
            }
            const std::string key = decl->key + "|" + std::to_string(index) + "|" + show(arguments[index]);
            if (!reportedBounds.insert(key).second)
            {
                continue;
            }
            reportNode(312, nodes[index],
                say("тип `", "type `") + show(arguments[index])
                    + say("` не реализует типаж `", "` does not implement trait `") + bound->decl->display
                    + say("`, которого требует параметр `", "` required by parameter `")
                    + decl->parameterDisplays[index] + say("` типа `", "` of type `") + decl->display + "`");
        }
    }

    bool implements(const TypePtr& type, const TypeDecl* trait) const
    {
        if (!isKnown(type))
        {
            return true;
        }
        if (type->kind == TypeKind::Named)
        {
            return declarations.hasImpl(trait, type->decl, "");
        }
        const bool scalar = type->kind == TypeKind::Integer || type->kind == TypeKind::Float
            || type->kind == TypeKind::Bool || type->kind == TypeKind::Char || type->kind == TypeKind::Str;
        if (!scalar)
        {
            return true;
        }
        const std::string name = type->builtin.empty() ? typeText(type, nullptr, nullptr) : type->builtin;
        if (declarations.hasImpl(trait, nullptr, name))
        {
            return true;
        }
        const bool core = trait->key == "Равенство" || trait->key == "Сравнение" || trait->key == "Показ"
            || trait->key == "Хеширование";
        if (!core)
        {
            return true;
        }
        return !(trait->key == "Хеширование" && type->kind == TypeKind::Float);
    }

    void checkItem(const SyntaxNodeView& item)
    {
        switch (item.kind())
        {
        case ConstDecl:
            checkConst(item);
            return;
        case StructDecl:
            checkRecursiveStruct(item);
            return;
        case ImplDecl:
            checkImpl(item);
            return;
        case FunctionDecl:
            checkFunction(item, nullptr);
            return;
        default:
            return;
        }
    }

    void checkConst(const SyntaxNodeView& item)
    {
        std::vector<SyntaxNodeView> parts;
        for (const SyntaxNodeView& child : item.nodes())
        {
            if (child.kind() != Attribute)
            {
                parts.push_back(child);
            }
        }
        TypeContext context{&module, {}, nullptr};
        typeContext = context;
        if (parts.size() >= 2 && item.hasToken(Colon))
        {
            const TypePtr declared = resolveType(parts[0], context);
            scopes.assign(1, {});
            const TypePtr value = infer(parts[1], declared);
            expectCompatible(value, declared, parts[1], parts[0]);
        }
    }

    void checkRecursiveStruct(const SyntaxNodeView& item)
    {
        const std::optional<SyntaxToken> name = item.token(Identifier);
        const Entry* entry = name ? declarations.lookup(&module, nameKey(name->text)) : nullptr;
        if (entry == nullptr || entry->kind != EntryKind::Type)
        {
            return;
        }
        const TypeDecl* decl = entry->type;
        TypeContext context{&module, {}, nullptr};
        for (size_t index = 0; index < decl->parameters.size(); ++index)
        {
            context.substitution[decl->parameters[index]] = parameterType(decl->parameterDisplays[index]);
        }
        for (const FieldDecl& field : decl->fields)
        {
            std::set<const TypeDecl*> visited;
            if (containsByValue(resolveType(field.type, context), decl, visited))
            {
                report(315, field.offset, field.length,
                    say("структура `", "struct `") + decl->display
                        + say("` содержит себя по значению: её размер бесконечен; ссылку дают `?&",
                            "` contains itself by value: its size is infinite; use `?&")
                        + decl->display + say("` и `Уник(", "` or `Уник(") + decl->display + ")`");
            }
        }
    }

    TypePtr parameterType(const std::string& display) const
    {
        auto type = std::make_shared<Type>();
        type->kind = TypeKind::Parameter;
        type->builtin = display;
        return type;
    }

    bool containsByValue(const TypePtr& type, const TypeDecl* target, std::set<const TypeDecl*>& visited)
    {
        if (!type)
        {
            return false;
        }
        switch (type->kind)
        {
        case TypeKind::Optional:
        case TypeKind::Array:
        case TypeKind::Vector:
            return containsByValue(type->inner, target, visited);
        case TypeKind::Tuple:
            return std::any_of(type->arguments.begin(), type->arguments.end(),
                [&](const TypePtr& element) { return containsByValue(element, target, visited); });
        case TypeKind::Named:
        {
            if (type->decl == target)
            {
                return true;
            }
            if (type->decl->kind == DeclKind::Trait || !visited.insert(type->decl).second)
            {
                return false;
            }
            TypeContext context{type->decl->module, {}, nullptr};
            for (size_t index = 0; index < type->decl->parameters.size(); ++index)
            {
                context.substitution[type->decl->parameters[index]] =
                    index < type->arguments.size() ? type->arguments[index] : unknownType();
            }
            for (const FieldDecl& field : type->decl->fields)
            {
                if (containsByValue(resolveType(field.type, context), target, visited))
                {
                    return true;
                }
            }
            for (const VariantDecl& variant : type->decl->variants)
            {
                for (const SyntaxNodeView& payload : variant.payload)
                {
                    if (containsByValue(resolveType(payload, context), target, visited))
                    {
                        return true;
                    }
                }
            }
            return false;
        }
        default:
            return false;
        }
    }

    void checkImpl(const SyntaxNodeView& item)
    {
        const ImplInfo* impl = nullptr;
        for (const ImplInfo& candidate : declarations.impls)
        {
            if (candidate.module == &module && candidate.node.offset() == item.offset())
            {
                impl = &candidate;
            }
        }
        if (const SyntaxNodeView list = item.node(MethodList))
        {
            for (const SyntaxNodeView& member : list.nodes())
            {
                if (member.kind() == FunctionDecl)
                {
                    checkFunction(member, impl != nullptr ? impl->target : nullptr);
                }
            }
        }
        if (impl == nullptr || impl->builtin || impl->trait == nullptr || item.node(MethodList))
        {
            return;
        }
        const TypeDecl* trait = impl->trait;
        const SyntaxNodeView signatures = trait->node.node(SignatureList);
        std::vector<SyntaxNodeView> required;
        if (signatures)
        {
            for (const SyntaxNodeView& signature : signatures.nodes())
            {
                if (signature.kind() == Signature)
                {
                    required.push_back(signature);
                }
            }
        }
        const bool derivable = trait->key == "Равенство" || trait->key == "Сравнение" || trait->key == "Хеширование"
            || trait->key == "Показ";
        const std::string targetName = impl->target != nullptr ? impl->target->display : impl->builtinTarget;
        bool ownMethods = !required.empty();
        for (const SyntaxNodeView& signature : required)
        {
            const std::vector<SyntaxToken> names = signature.tokens();
            const auto name = std::find_if(
                names.begin(), names.end(), [](const SyntaxToken& token) { return token.kind == Identifier; });
            if (name == names.end())
            {
                continue;
            }
            const std::string methodKey = nameKey(name->text);
            if (declarations.method(impl->target, impl->builtinTarget, methodKey) != nullptr)
            {
                continue;
            }
            ownMethods = false;
            if (derivable)
            {
                continue;
            }
            reportNode(311, item,
                say("у `", "`") + targetName + say("` нет метода `", "` has no method `") + normalizedName(name->text)
                    + say("`, которого требует типаж `", "` required by trait `") + trait->display + "`",
                {ReportNote{name->offset, static_cast<uint32_t>(name->text.size()),
                    say("метод объявлен в типаже здесь", "the method is declared in the trait here")}});
        }
        if (derivable && !ownMethods && impl->target != nullptr)
        {
            checkDerivation(item, *impl);
        }
    }

    void checkDerivation(const SyntaxNodeView& item, const ImplInfo& impl)
    {
        const TypeDecl* target = impl.target;
        TypeContext context{target->module, {}, nullptr};
        for (size_t index = 0; index < target->parameters.size(); ++index)
        {
            context.substitution[target->parameters[index]] = parameterType(target->parameterDisplays[index]);
        }
        for (const FieldDecl& field : target->fields)
        {
            const TypePtr type = resolveType(field.type, context);
            if (implements(type, impl.trait))
            {
                continue;
            }
            reportNode(313, item,
                "`" + impl.trait->display + say("` для `", "` for `") + target->display
                    + say("` не выводится: у поля `", "` cannot be derived: field `") + field.display
                    + say("` типа `", "` of type `") + show(type) + say("` его нет", "` lacks it"),
                {ReportNote{field.offset, field.length, say("поле объявлено здесь", "field declared here")}});
            return;
        }
    }

    struct FunctionScope
    {
        TypePtr result;
        std::string name;
        uint32_t nameOffset = 0;
        uint32_t nameLength = 0;
        bool open = false;
        bool known = true;
        SyntaxNodeView resultNode;
    };

    void checkFunction(const SyntaxNodeView& item, const TypeDecl* implTarget)
    {
        const SyntaxNodeView signature = item.node(Signature);
        const SyntaxNodeView body = item.node(Block);
        if (!signature)
        {
            return;
        }
        reportedBounds.clear();
        TypeContext context{&module, {}, nullptr};
        scopes.assign(1, {});
        FunctionScope function;
        regionContext = false;
        std::vector<SyntaxToken> names;
        for (const SyntaxToken& token : signature.tokens())
        {
            if (token.kind == Identifier)
            {
                names.push_back(token);
            }
        }
        const bool method = signature.hasToken(Dot);
        if (names.empty())
        {
            return;
        }
        const SyntaxToken name = method && names.size() >= 2 ? names[1] : names.front();
        function.name = normalizedName(name.text);
        function.nameOffset = name.offset;
        function.nameLength = static_cast<uint32_t>(name.text.size());
        function.open = item.hasToken(KwPub) || signature.hasToken(KwPub);
        TypePtr selfType;
        if (method)
        {
            const Entry* owner = declarations.lookup(&module, nameKey(names.front().text));
            std::vector<TypePtr> arguments;
            if (const SyntaxNodeView parameters = signature.node(ComptimeParamList))
            {
                for (const SyntaxNodeView& parameter : parameters.nodes())
                {
                    if (const std::optional<SyntaxToken> parameterName = parameter.token(Identifier))
                    {
                        const TypePtr type = parameterType(normalizedName(parameterName->text));
                        context.substitution[nameKey(parameterName->text)] = type;
                        arguments.push_back(type);
                    }
                }
            }
            selfType = owner != nullptr && owner->kind == EntryKind::Type ? namedType(owner->type, arguments)
                                                                          : builtinType(nameKey(names.front().text));
        }
        else if (implTarget != nullptr)
        {
            selfType = namedType(implTarget, std::vector<TypePtr>(implTarget->parameters.size(), unknownType()));
        }
        context.selfType = selfType;
        regionContext = requiresRegion(signature);
        bool afterArrow = false;
        signature.forEachChild([&](const SyntaxNodeView& child, const std::optional<SyntaxToken>& token)
        {
            if (token)
            {
                afterArrow = afterArrow || token->kind == Arrow;
                return;
            }
            if (child.kind() == ParamList)
            {
                declareParameters(child, context, selfType);
            }
            else if (afterArrow && !function.resultNode)
            {
                function.resultNode = child;
                function.result = resolveType(child, context);
            }
        });
        if (!function.result)
        {
            function.result = builtinType("пусто");
        }
        typeContext = context;
        functions.push_back(function);
        if (body)
        {
            checkBlock(body);
        }
        functions.pop_back();
    }

    void declareParameters(const SyntaxNodeView& list, TypeContext& context, const TypePtr& selfType)
    {
        for (const SyntaxNodeView& parameter : list.nodes())
        {
            if (parameter.kind() != Param)
            {
                continue;
            }
            if (parameter.hasToken(KwSelf))
            {
                continue;
            }
            const std::optional<SyntaxToken> name = parameter.token(Identifier);
            if (!name)
            {
                continue;
            }
            if (parameter.hasToken(KwComptime))
            {
                const TypePtr type = parameterType(normalizedName(name->text));
                context.substitution[nameKey(name->text)] = type;
                scopes.back()[nameKey(name->text)] = LocalInfo{makeMeta(type)};
                continue;
            }
            const std::vector<SyntaxNodeView> parts = parameter.nodes();
            const TypePtr type =
                parameter.hasToken(Ellipsis) || parts.empty() ? unknownType() : resolveType(parts.front(), context);
            scopes.back()[nameKey(name->text)] = LocalInfo{type};
        }
        (void)selfType;
    }

    TypePtr makeMeta(const TypePtr& inner) const
    {
        auto type = std::make_shared<Type>();
        type->kind = TypeKind::Meta;
        type->inner = inner;
        return type;
    }

    TypePtr selfValueType(const SyntaxNodeView& node) const
    {
        (void)node;
        return typeContext.selfType ? typeContext.selfType : unknownType();
    }

    void checkBlock(const SyntaxNodeView& block)
    {
        scopes.emplace_back();
        for (const SyntaxNodeView& statement : block.nodes())
        {
            checkStatement(statement);
        }
        scopes.pop_back();
    }

    void bind(const SyntaxToken& name, const TypePtr& type)
    {
        scopes.back()[nameKey(name.text)] = LocalInfo{type};
    }

    const LocalInfo* local(const std::string& key) const
    {
        for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope)
        {
            if (const auto found = scope->find(key); found != scope->end())
            {
                return &found->second;
            }
        }
        return nullptr;
    }

    static TypePtr settled(const TypePtr& type)
    {
        if (type && type->kind == TypeKind::LiteralInteger)
        {
            return builtinType("ц64");
        }
        if (type && type->kind == TypeKind::LiteralFloat)
        {
            return builtinType("в64");
        }
        return type ? type : unknownType();
    }

    void checkStatement(const SyntaxNodeView& statement)
    {
        switch (statement.kind())
        {
        case BindingStmt:
            checkBinding(statement);
            return;
        case AssignStmt:
            checkAssignment(statement);
            return;
        case ExprStmt:
            checkExpressionStatement(statement);
            return;
        case DiscardStmt:
            checkDiscard(statement);
            return;
        case ReturnStmt:
            checkReturn(statement);
            return;
        case IfStmt:
            checkIf(statement);
            return;
        case LoopStmt:
            checkLoop(statement);
            return;
        case RegionStmt:
        case ThroughStmt:
        {
            const bool saved = regionContext;
            for (const SyntaxNodeView& child : statement.nodes())
            {
                if (child.kind() == Block)
                {
                    regionContext = true;
                    checkBlock(child);
                    regionContext = saved;
                }
                else
                {
                    infer(child, unknownType());
                }
            }
            return;
        }
        case Block:
            checkBlock(statement);
            return;
        default:
            for (const SyntaxNodeView& child : statement.nodes())
            {
                if (child.kind() == Block)
                {
                    checkBlock(child);
                }
                else
                {
                    infer(child, unknownType());
                }
            }
            return;
        }
    }

    void checkBinding(const SyntaxNodeView& statement)
    {
        const std::optional<SyntaxToken> name = statement.token(Identifier);
        std::vector<SyntaxNodeView> parts = statement.nodes();
        TypePtr declared;
        size_t valueIndex = 0;
        if (statement.hasToken(Colon) && !parts.empty())
        {
            declared = resolveType(parts.front(), typeContext);
            valueIndex = 1;
        }
        TypePtr value;
        if (valueIndex < parts.size())
        {
            value = infer(parts[valueIndex], declared ? declared : unknownType());
            if (declared)
            {
                expectCompatible(value, declared, parts[valueIndex], parts.front());
            }
        }
        if (name)
        {
            bind(*name, declared ? declared : settled(value));
        }
    }

    void checkAssignment(const SyntaxNodeView& statement)
    {
        const std::vector<SyntaxNodeView> parts = statement.nodes();
        if (parts.size() < 2)
        {
            return;
        }
        const SyntaxNodeView target = parts.front();
        const bool plain = statement.hasToken(Eq);
        if (target.kind() == NameExpr && target.token(Identifier))
        {
            const std::string key = nameKey(target.token(Identifier)->text);
            if (const LocalInfo* existing = local(key))
            {
                const TypePtr type = existing->type;
                infer(parts[1], type);
                return;
            }
            const TypePtr value = infer(parts[1], unknownType());
            if (plain)
            {
                bind(*target.token(Identifier), settled(value));
            }
            return;
        }
        const TypePtr value = infer(parts[1], unknownType());
        if (!plain)
        {
            infer(target, unknownType());
            return;
        }
        destructure(target, value);
    }

    void destructure(const SyntaxNodeView& target, const TypePtr& value)
    {
        switch (target.kind())
        {
        case NameExpr:
            if (const std::optional<SyntaxToken> name = target.token(Identifier))
            {
                if (local(nameKey(name->text)) == nullptr)
                {
                    bind(*name, settled(value));
                }
            }
            return;
        case TupleExpr:
        {
            const std::vector<SyntaxNodeView> elements = target.nodes();
            const TypePtr stripped = stripReferences(value);
            for (size_t index = 0; index < elements.size(); ++index)
            {
                const bool tuple = stripped && stripped->kind == TypeKind::Tuple && index < stripped->arguments.size();
                destructure(elements[index], tuple ? stripped->arguments[index] : unknownType());
            }
            return;
        }
        case CallExpr:
            destructureVariant(target, value);
            return;
        case CompoundLiteral:
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
                const TypePtr fieldType = fieldName ? fieldOf(value, *fieldName, false) : unknownType();
                if (!values.empty())
                {
                    destructure(values.front(), fieldType);
                }
            }
            return;
        default:
            return;
        }
    }

    void destructureVariant(const SyntaxNodeView& target, const TypePtr& value)
    {
        const std::vector<SyntaxNodeView> parts = target.nodes();
        const SyntaxNodeView arguments = target.node(ArgList);
        if (parts.empty() || parts.front().kind() != NameExpr || !parts.front().token(Identifier))
        {
            return;
        }
        const std::string variant = nameKey(parts.front().token(Identifier)->text);
        const TypePtr stripped = stripReferences(value);
        std::vector<SyntaxNodeView> values = arguments ? arguments.nodes() : std::vector<SyntaxNodeView>();
        if (stripped && stripped->kind == TypeKind::Optional && variant == "Есть")
        {
            reportNode(310, target,
                say("образец привязки может не подойти: не разобрано `Ничего`",
                    "a binding pattern may fail to match: `Ничего` is not handled"));
            if (!values.empty())
            {
                destructure(values.front(), stripped->inner);
            }
            return;
        }
        if (stripped && stripped->kind == TypeKind::Named && stripped->decl->kind == DeclKind::Enum
            && stripped->decl->variants.size() > 1)
        {
            for (const VariantDecl& other : stripped->decl->variants)
            {
                if (other.key != variant)
                {
                    reportNode(310, target,
                        say("образец привязки может не подойти: не разобрано `",
                            "a binding pattern may fail to match: `")
                            + other.display + say("`", "` is not handled"));
                    break;
                }
            }
        }
        for (const SyntaxNodeView& element : values)
        {
            destructure(element, unknownType());
        }
    }

    bool isCallExpression(const SyntaxNodeView& node) const
    {
        if (node.kind() == CallExpr)
        {
            return true;
        }
        if (node.kind() == PrefixExpr && (node.hasToken(KwTry) || node.hasToken(KwUnsafe)))
        {
            const std::vector<SyntaxNodeView> parts = node.nodes();
            return !parts.empty() && isCallExpression(parts.front());
        }
        return false;
    }

    void checkExpressionStatement(const SyntaxNodeView& statement)
    {
        for (const SyntaxNodeView& child : statement.nodes())
        {
            const TypePtr type = infer(child, unknownType());
            if (!isCallExpression(child) || !isKnown(type) || type->kind == TypeKind::Void
                || type->kind == TypeKind::Never)
            {
                continue;
            }
            const std::string hint = isResultType(type)
                ? say(": его разбирает `выбор`, пробрасывает `попытка` или отбрасывает `_ =`",
                      ": handle it with `выбор`, propagate it with `попытка` or discard it with `_ =`")
                : say(": его отбрасывают явно — `_ =`", ": discard it explicitly with `_ =`");
            reportNode(306, child,
                say("значение типа `", "value of type `") + show(type) + say("` отброшено", "` is discarded") + hint);
        }
    }

    void checkDiscard(const SyntaxNodeView& statement)
    {
        const std::vector<SyntaxNodeView> parts = statement.nodes();
        const std::optional<SyntaxToken> underscore = statement.token(Underscore);
        if (parts.empty() || !underscore)
        {
            return;
        }
        const TypePtr type = infer(parts.front(), unknownType());
        if (!type || type->kind != TypeKind::Void)
        {
            return;
        }
        std::string callee;
        if (parts.front().kind() == CallExpr && !parts.front().nodes().empty())
        {
            callee = text(parts.front().nodes().front());
        }
        report(307, underscore->offset, static_cast<uint32_t>(underscore->text.size()),
            callee.empty() ? say("отбрасывать нечего: значение имеет тип `пусто`",
                                 "nothing to discard: the value has type `пусто`")
                           : say("отбрасывать нечего: `", "nothing to discard: `") + callee
                    + say("` возвращает `пусто`", "` returns `пусто`"));
    }

    void checkReturn(const SyntaxNodeView& statement)
    {
        const std::vector<SyntaxNodeView> parts = statement.nodes();
        if (parts.empty() || functions.empty())
        {
            return;
        }
        const FunctionScope& function = functions.back();
        const TypePtr value = infer(parts.front(), function.result);
        if (!function.resultNode)
        {
            return;
        }
        expectCompatible(value, function.result, parts.front(), function.resultNode,
            say("тип результата объявлен здесь", "the result type is declared here"));
    }

    bool compatible(const TypePtr& actual, const TypePtr& expected) const
    {
        if (!isKnown(actual) || !isKnown(expected) || actual->kind == TypeKind::Never)
        {
            return true;
        }
        if (actual->kind == TypeKind::LiteralInteger)
        {
            return expected->kind == TypeKind::Integer || expected->kind == TypeKind::Float;
        }
        if (actual->kind == TypeKind::LiteralFloat)
        {
            return expected->kind == TypeKind::Float;
        }
        if (sameType(actual, expected))
        {
            return true;
        }
        const bool scalar = [](const TypePtr& type)
        {
            return type->kind == TypeKind::Integer || type->kind == TypeKind::Float || type->kind == TypeKind::Bool
                || type->kind == TypeKind::Char || type->kind == TypeKind::Str;
        }(actual);
        const bool expectedScalar = expected->kind == TypeKind::Integer || expected->kind == TypeKind::Float
            || expected->kind == TypeKind::Bool || expected->kind == TypeKind::Char || expected->kind == TypeKind::Str;
        return !(scalar && expectedScalar);
    }

    void expectCompatible(const TypePtr& actual, const TypePtr& expected, const SyntaxNodeView& node,
        const SyntaxNodeView& declaredAt, std::string noteText = {})
    {
        if (compatible(actual, expected))
        {
            return;
        }
        std::vector<ReportNote> notes;
        if (declaredAt && !noteText.empty())
        {
            const uint32_t length = static_cast<uint32_t>(text(declaredAt).size());
            notes.push_back(ReportNote{declaredAt.offset(), length, std::move(noteText)});
        }
        reportNode(303, node,
            say("ожидался тип `", "expected type `") + show(expected) + say("`, а получен `", "`, found `")
                + show(actual) + "`",
            std::move(notes));
    }

    void checkIf(const SyntaxNodeView& statement)
    {
        scopes.emplace_back();
        bool thenDone = false;
        for (const SyntaxNodeView& child : statement.nodes())
        {
            if (child.kind() == Block)
            {
                checkBlock(child);
                if (!thenDone)
                {
                    thenDone = true;
                    scopes.pop_back();
                    scopes.emplace_back();
                }
            }
            else if (child.kind() == IfStmt)
            {
                checkIf(child);
            }
            else
            {
                infer(child, builtinType("бул"));
            }
        }
        scopes.pop_back();
    }

    void checkLoop(const SyntaxNodeView& statement)
    {
        scopes.emplace_back();
        const bool iteration = statement.hasToken(KwIn);
        bool afterIn = false;
        SyntaxNodeView pattern;
        statement.forEachChild([&](const SyntaxNodeView& child, const std::optional<SyntaxToken>& token)
        {
            if (token)
            {
                afterIn = afterIn || token->kind == KwIn;
                return;
            }
            if (child.kind() == Block)
            {
                checkBlock(child);
            }
            else if (iteration && !afterIn)
            {
                pattern = child;
            }
            else if (iteration)
            {
                const TypePtr element = elementOf(infer(child, unknownType()));
                if (pattern)
                {
                    bindPattern(pattern, element);
                }
            }
            else
            {
                infer(child, builtinType("бул"));
            }
        });
        scopes.pop_back();
    }

    TypePtr elementOf(const TypePtr& source)
    {
        const TypePtr stripped = stripReferences(source);
        if (!stripped)
        {
            return unknownType();
        }
        if (stripped->kind == TypeKind::Slice || stripped->kind == TypeKind::Array)
        {
            return referenceTo(stripped->inner, stripped->mutableAccess);
        }
        if (stripped->kind == TypeKind::Named && stripped->decl->key == "Диапазон" && !stripped->arguments.empty())
        {
            return stripped->arguments.front();
        }
        return unknownType();
    }

    void bindPattern(const SyntaxNodeView& pattern, const TypePtr& type)
    {
        switch (pattern.kind())
        {
        case PathPattern:
        {
            const std::vector<SyntaxToken> tokens = pattern.tokens();
            std::vector<SyntaxToken> path;
            for (const SyntaxToken& token : tokens)
            {
                if (token.kind == Identifier)
                {
                    path.push_back(token);
                }
            }
            if (path.size() == 1 && pattern.nodes().empty() && !startsUppercase(path.front().text))
            {
                bind(path.front(), settled(type));
                return;
            }
            const TypePtr stripped = stripReferences(type);
            const std::string variant = path.empty() ? std::string() : nameKey(path.back().text);
            std::vector<TypePtr> payload;
            if (stripped && stripped->kind == TypeKind::Optional && variant == "Есть")
            {
                payload.push_back(stripped->inner);
            }
            else if (stripped && stripped->kind == TypeKind::Named && stripped->decl->kind == DeclKind::Enum)
            {
                payload = variantPayload(stripped, variant);
            }
            for (const SyntaxNodeView& part : pattern.nodes())
            {
                size_t index = 0;
                for (const SyntaxNodeView& argument : part.nodes())
                {
                    const TypePtr element = index < payload.size() ? payload[index] : unknownType();
                    ++index;
                    for (const SyntaxNodeView& inner : argument.nodes())
                    {
                        bindPattern(inner, element);
                    }
                    if (argument.kind() == PatternField && argument.nodes().empty() && argument.token(Identifier))
                    {
                        bind(*argument.token(Identifier), unknownType());
                    }
                }
            }
            return;
        }
        case BindingPattern:
            if (const std::optional<SyntaxToken> name = pattern.token(Identifier))
            {
                bind(*name, settled(type));
            }
            return;
        case TuplePattern:
        {
            const TypePtr stripped = stripReferences(type);
            size_t index = 0;
            for (const SyntaxNodeView& element : pattern.nodes())
            {
                const bool tuple = stripped && stripped->kind == TypeKind::Tuple && index < stripped->arguments.size();
                bindPattern(element, tuple ? stripped->arguments[index] : unknownType());
                ++index;
            }
            return;
        }
        case OrPattern:
            for (const SyntaxNodeView& alternative : pattern.nodes())
            {
                bindPattern(alternative, type);
            }
            return;
        default:
            return;
        }
    }

    std::vector<TypePtr> variantPayload(const TypePtr& enumType, const std::string& variant)
    {
        for (const VariantDecl& declared : enumType->decl->variants)
        {
            if (declared.key != variant)
            {
                continue;
            }
            TypeContext context = contextFor(enumType);
            std::vector<TypePtr> payload;
            for (const SyntaxNodeView& node : declared.payload)
            {
                payload.push_back(resolveType(node, context));
            }
            return payload;
        }
        return {};
    }

    TypeContext contextFor(const TypePtr& named)
    {
        TypeContext context{named->decl->module, {}, named};
        for (size_t index = 0; index < named->decl->parameters.size(); ++index)
        {
            context.substitution[named->decl->parameters[index]] =
                index < named->arguments.size() ? named->arguments[index] : unknownType();
        }
        return context;
    }

    TypePtr fieldOf(const TypePtr& receiver, const SyntaxToken& member, bool checkAccess)
    {
        const TypePtr stripped = stripReferences(receiver);
        if (!stripped)
        {
            return unknownType();
        }
        const std::string key = nameKey(member.text);
        if (stripped->kind == TypeKind::Vector)
        {
            return stripped->inner;
        }
        if (stripped->kind != TypeKind::Named || stripped->decl->kind != DeclKind::Struct)
        {
            return unknownType();
        }
        for (const FieldDecl& field : stripped->decl->fields)
        {
            if (field.key != key)
            {
                continue;
            }
            if (checkAccess && !field.open && stripped->decl->module != &module)
            {
                report(205, member.offset, static_cast<uint32_t>(member.text.size()),
                    say("поле `", "field `") + field.display + say("` типа `", "` of type `") + stripped->decl->display
                        + say("` закрыто: оно объявлено без `откр` в другом модуле",
                            "` is private: it is declared without `откр` in another module"));
            }
            return resolveType(field.type, contextFor(stripped));
        }
        return unknownType();
    }

    Denoted denote(const SyntaxNodeView& node)
    {
        if (node.kind() == NameExpr)
        {
            if (node.hasToken(KwSelf))
            {
                return Denoted{Denotes::Value, selfValueType(node), nullptr, nullptr};
            }
            if (node.hasToken(KwSelfType))
            {
                return Denoted{
                    Denotes::Type, typeContext.selfType ? typeContext.selfType : unknownType(), nullptr, nullptr};
            }
            const std::optional<SyntaxToken> name = node.token(Identifier);
            if (!name)
            {
                return Denoted{Denotes::Value, unknownType(), nullptr, nullptr};
            }
            const std::string key = nameKey(name->text);
            if (const LocalInfo* found = local(key))
            {
                if (found->type && found->type->kind == TypeKind::Meta)
                {
                    return Denoted{Denotes::Type, found->type->inner, nullptr, nullptr};
                }
                return Denoted{Denotes::Value, found->type, nullptr, nullptr};
            }
            if (const Entry* entry = declarations.lookup(&module, key))
            {
                switch (entry->kind)
                {
                case EntryKind::Type:
                    return Denoted{Denotes::Type,
                        namedType(entry->type, std::vector<TypePtr>(entry->type->parameters.size(), unknownType())),
                        nullptr, nullptr};
                case EntryKind::Function:
                    return Denoted{Denotes::Function, unknownType(), nullptr, entry->function};
                case EntryKind::Module:
                    return Denoted{Denotes::Module, unknownType(), entry->target, nullptr};
                case EntryKind::Header:
                    return Denoted{Denotes::Header, unknownType(), nullptr, nullptr};
                case EntryKind::Const:
                    return Denoted{Denotes::Value, constType(entry->constant, &module), nullptr, nullptr};
                }
            }
            const TypePtr builtin = builtinType(key);
            if (builtin->kind != TypeKind::Unknown || key == "тип")
            {
                return Denoted{Denotes::Type, builtin, nullptr, nullptr};
            }
            return Denoted{Denotes::Value, unknownType(), nullptr, nullptr};
        }
        if (node.kind() == FieldExpr)
        {
            const std::vector<SyntaxNodeView> parts = node.nodes();
            const std::optional<SyntaxToken> member = node.token(Identifier);
            if (parts.empty() || !member)
            {
                return Denoted{Denotes::Value, unknownType(), nullptr, nullptr};
            }
            const Denoted receiver = denote(parts.front());
            const std::string key = nameKey(member->text);
            if (receiver.kind == Denotes::Module && receiver.module != nullptr)
            {
                const Entry* entry = declarations.lookup(receiver.module, key);
                if (entry == nullptr)
                {
                    return Denoted{Denotes::Value, unknownType(), nullptr, nullptr};
                }
                switch (entry->kind)
                {
                case EntryKind::Type:
                    return Denoted{Denotes::Type,
                        namedType(entry->type, std::vector<TypePtr>(entry->type->parameters.size(), unknownType())),
                        nullptr, nullptr};
                case EntryKind::Function:
                    return Denoted{Denotes::Function, unknownType(), nullptr, entry->function};
                case EntryKind::Const:
                    return Denoted{Denotes::Value, constType(entry->constant, receiver.module), nullptr, nullptr};
                default:
                    return Denoted{Denotes::Value, unknownType(), nullptr, nullptr};
                }
            }
            if (receiver.kind == Denotes::Type)
            {
                const TypePtr owner = receiver.type;
                if (owner && owner->kind == TypeKind::Named && owner->decl->kind == DeclKind::Enum)
                {
                    for (const VariantDecl& variant : owner->decl->variants)
                    {
                        if (variant.key == key)
                        {
                            return Denoted{Denotes::Value, owner, nullptr, nullptr};
                        }
                    }
                }
                const FunctionInfo* method = owner && owner->kind == TypeKind::Named
                    ? declarations.method(owner->decl, "", key)
                    : (owner ? declarations.method(nullptr, owner->builtin, key) : nullptr);
                if (method != nullptr)
                {
                    Denoted denoted{Denotes::Function, owner, nullptr, method};
                    return denoted;
                }
                return Denoted{Denotes::Value, unknownType(), nullptr, nullptr};
            }
            if (receiver.kind != Denotes::Value)
            {
                return Denoted{Denotes::Value, unknownType(), nullptr, nullptr};
            }
            return Denoted{Denotes::Value, fieldOf(receiver.type, *member, true), nullptr, nullptr};
        }
        if (node.kind() == CallExpr)
        {
            const std::vector<SyntaxNodeView> parts = node.nodes();
            if (!parts.empty())
            {
                const Denoted callee = denote(parts.front());
                if (callee.kind == Denotes::Type)
                {
                    return Denoted{Denotes::Type, resolveGeneric(node, typeContext), nullptr, nullptr};
                }
            }
        }
        return Denoted{Denotes::Value, infer(node, unknownType()), nullptr, nullptr};
    }

    TypePtr constType(const SyntaxNodeView& constant, SourceModule* owner)
    {
        std::vector<SyntaxNodeView> parts;
        for (const SyntaxNodeView& child : constant.nodes())
        {
            if (child.kind() != Attribute)
            {
                parts.push_back(child);
            }
        }
        if (constant.hasToken(Colon) && !parts.empty())
        {
            return resolveType(parts.front(), TypeContext{owner, {}, nullptr});
        }
        if (!parts.empty() && parts.back().kind() == Literal)
        {
            return literalType(parts.back(), unknownType());
        }
        return unknownType();
    }

    TypePtr infer(const SyntaxNodeView& node, const TypePtr& expected)
    {
        switch (node.kind())
        {
        case Literal:
            return literalType(node, expected);
        case NameExpr:
            return inferName(node, expected);
        case FieldExpr:
        {
            const Denoted denoted = denote(node);
            return denoted.kind == Denotes::Value ? denoted.type : unknownType();
        }
        case CallExpr:
            return inferCall(node, expected);
        case BinaryExpr:
            return inferBinary(node, expected);
        case ComparisonExpr:
            inferComparison(node);
            return builtinType("бул");
        case LogicalExpr:
        case NotExpr:
        case MembershipExpr:
            for (const SyntaxNodeView& child : node.nodes())
            {
                infer(child, builtinType("бул"));
            }
            return builtinType("бул");
        case IsExpr:
        {
            const std::vector<SyntaxNodeView> parts = node.nodes();
            if (!parts.empty())
            {
                const TypePtr scrutinee = infer(parts.front(), unknownType());
                if (parts.size() > 1 && !node.hasToken(KwNot))
                {
                    bindPattern(parts[1], scrutinee);
                }
            }
            return builtinType("бул");
        }
        case CastExpr:
        {
            const std::vector<SyntaxNodeView> parts = node.nodes();
            if (parts.size() >= 2)
            {
                infer(parts.front(), unknownType());
                return resolveType(parts.back(), typeContext);
            }
            return unknownType();
        }
        case PrefixExpr:
            return inferPrefix(node, expected);
        case RefExpr:
        {
            const std::vector<SyntaxNodeView> parts = node.nodes();
            if (parts.empty())
            {
                return unknownType();
            }
            const TypePtr inner = infer(
                parts.back(), expected && expected->kind == TypeKind::Reference ? expected->inner : unknownType());
            return referenceTo(inner, node.hasToken(KwMut));
        }
        case IndexExpr:
            return inferIndex(node);
        case ParenExpr:
        {
            const std::vector<SyntaxNodeView> parts = node.nodes();
            return parts.empty() ? unknownType() : infer(parts.front(), expected);
        }
        case TupleExpr:
        {
            auto tuple = std::make_shared<Type>();
            tuple->kind = TypeKind::Tuple;
            for (const SyntaxNodeView& element : node.nodes())
            {
                tuple->arguments.push_back(settled(infer(element, unknownType())));
            }
            return tuple;
        }
        case CompoundLiteral:
            return inferLiteral(node);
        case MatchExpr:
            return inferMatch(node, expected);
        case FunctionExpr:
            inferClosure(node);
            return unknownType();
        case RangeExpr:
        {
            TypePtr element = unknownType();
            for (const SyntaxNodeView& child : node.nodes())
            {
                const TypePtr type = infer(child, unknownType());
                if (element->kind == TypeKind::Unknown || element->kind == TypeKind::LiteralInteger)
                {
                    element = type;
                }
            }
            const Entry* range = declarations.lookup(&module, "Диапазон");
            if (range != nullptr && range->kind == EntryKind::Type)
            {
                return namedType(range->type, {settled(element)});
            }
            return unknownType();
        }
        default:
            for (const SyntaxNodeView& child : node.nodes())
            {
                if (child.kind() == Block)
                {
                    checkBlock(child);
                }
                else
                {
                    infer(child, unknownType());
                }
            }
            return unknownType();
        }
    }

    TypePtr literalType(const SyntaxNodeView& node, const TypePtr& expected)
    {
        const std::vector<SyntaxToken> tokens = node.tokens();
        if (tokens.empty())
        {
            return unknownType();
        }
        const SyntaxToken& token = tokens.front();
        switch (token.kind)
        {
        case Integer:
            if (expected && expected->kind == TypeKind::Integer)
            {
                checkLiteralRange(token, expected, false);
                return expected;
            }
            if (expected && expected->kind == TypeKind::Float)
            {
                return expected;
            }
            return builtinType("компил_цел");
        case Float:
            return expected && expected->kind == TypeKind::Float ? expected : builtinType("компил_вещ");
        case String:
        case RawString:
            return builtinType("стр");
        case Char:
            return builtinType("сим");
        case Byte:
            return builtinType("н8");
        case KwTrue:
        case KwFalse:
            return builtinType("бул");
        default:
            return unknownType();
        }
    }

    void checkLiteralRange(const SyntaxToken& token, const TypePtr& type, bool negative)
    {
        const std::optional<IntegerRange> range = integerRange(type->builtin);
        if (!range || range->wide)
        {
            return;
        }
        std::string digits;
        for (const char symbol : token.text)
        {
            if (symbol != '_')
            {
                digits += symbol;
            }
        }
        int base = 10;
        if (digits.starts_with("0x"))
        {
            base = 16;
            digits = digits.substr(2);
        }
        else if (digits.starts_with("0b"))
        {
            base = 2;
            digits = digits.substr(2);
        }
        uint64_t value = 0;
        bool overflow = false;
        for (const char symbol : digits)
        {
            const int digit = symbol >= '0' && symbol <= '9' ? symbol - '0'
                : symbol >= 'a' && symbol <= 'f'             ? symbol - 'a' + 10
                : symbol >= 'A' && symbol <= 'F'             ? symbol - 'A' + 10
                                                             : 0;
            const uint64_t limit = (UINT64_MAX - static_cast<uint64_t>(digit)) / static_cast<uint64_t>(base);
            if (value > limit)
            {
                overflow = true;
                break;
            }
            value = value * static_cast<uint64_t>(base) + static_cast<uint64_t>(digit);
        }
        const bool fits = !overflow && (negative ? value <= range->negativeLimit : value <= range->maximum);
        if (fits)
        {
            return;
        }
        const std::string written = (negative ? "-" : "") + std::string(token.text);
        report(305, token.offset, static_cast<uint32_t>(token.text.size()),
            say("литерал `", "literal `") + written + say("` не помещается в `", "` does not fit `") + type->builtin
                + say("`: допустимо от ", "`: the range is ") + range->minimumText + say(" до ", " to ")
                + range->maximumText);
    }

    TypePtr inferName(const SyntaxNodeView& node, const TypePtr& expected)
    {
        const std::optional<SyntaxToken> name = node.token(Identifier);
        if (name && local(nameKey(name->text)) == nullptr && startsUppercase(name->text))
        {
            const std::string key = nameKey(name->text);
            const TypePtr stripped = expected;
            if (stripped && stripped->kind == TypeKind::Optional && key == "Ничего")
            {
                return expected;
            }
            if (stripped && stripped->kind == TypeKind::Named && stripped->decl->kind == DeclKind::Enum)
            {
                for (const VariantDecl& variant : stripped->decl->variants)
                {
                    if (variant.key == key)
                    {
                        return expected;
                    }
                }
            }
        }
        const Denoted denoted = denote(node);
        return denoted.kind == Denotes::Value ? denoted.type : unknownType();
    }

    TypePtr inferCall(const SyntaxNodeView& node, const TypePtr& expected)
    {
        const std::vector<SyntaxNodeView> parts = node.nodes();
        const SyntaxNodeView arguments = node.node(ArgList);
        std::vector<SyntaxNodeView> values = arguments ? arguments.nodes() : std::vector<SyntaxNodeView>();
        if (parts.empty())
        {
            return unknownType();
        }
        const SyntaxNodeView callee = parts.front();
        if (callee.kind() == NameExpr && callee.token(Identifier)
            && local(nameKey(callee.token(Identifier)->text)) == nullptr
            && startsUppercase(callee.token(Identifier)->text))
        {
            const std::string key = nameKey(callee.token(Identifier)->text);
            const TypePtr variant = inferVariant(key, values, expected);
            if (variant)
            {
                return variant;
            }
        }
        Denoted target;
        TypePtr receiver;
        if (callee.kind() == FieldExpr)
        {
            const std::vector<SyntaxNodeView> calleeParts = callee.nodes();
            const std::optional<SyntaxToken> member = callee.token(Identifier);
            if (!calleeParts.empty() && member)
            {
                const Denoted owner = denote(calleeParts.front());
                if (owner.kind == Denotes::Value)
                {
                    const TypePtr stripped = stripReferences(owner.type);
                    const FunctionInfo* method = nullptr;
                    if (stripped && stripped->kind == TypeKind::Named)
                    {
                        method = declarations.method(stripped->decl, "", nameKey(member->text));
                    }
                    else if (stripped && !stripped->builtin.empty())
                    {
                        method = declarations.method(nullptr, stripped->builtin, nameKey(member->text));
                    }
                    if (method != nullptr)
                    {
                        return callFunction(*method, node, values, stripped, true, expected);
                    }
                    inferArguments(values);
                    return unknownType();
                }
                target = denote(callee);
                if (target.kind == Denotes::Value && target.type && target.type->kind == TypeKind::Named
                    && target.type->decl->kind == DeclKind::Enum)
                {
                    inferArguments(values);
                    return target.type;
                }
            }
        }
        else
        {
            target = denote(callee);
        }
        if (target.kind == Denotes::Function && target.function != nullptr)
        {
            receiver = target.type;
            return callFunction(*target.function, node, values, receiver, false, expected);
        }
        if (target.kind == Denotes::Type)
        {
            inferArguments(values);
            return unknownType();
        }
        inferArguments(values);
        return unknownType();
    }

    void inferArguments(const std::vector<SyntaxNodeView>& values)
    {
        for (const SyntaxNodeView& value : values)
        {
            infer(value, unknownType());
        }
    }

    TypePtr inferVariant(const std::string& key, const std::vector<SyntaxNodeView>& values, const TypePtr& expected)
    {
        if (key == "Есть")
        {
            const TypePtr inner = expected && expected->kind == TypeKind::Optional ? expected->inner : unknownType();
            const TypePtr value = values.empty() ? unknownType() : infer(values.front(), inner);
            return expected && expected->kind == TypeKind::Optional ? expected : optionalOf(settled(value));
        }
        if (expected && expected->kind == TypeKind::Named && expected->decl->kind == DeclKind::Enum)
        {
            for (const VariantDecl& variant : expected->decl->variants)
            {
                if (variant.key != key)
                {
                    continue;
                }
                const std::vector<TypePtr> payload = variantPayload(expected, key);
                for (size_t index = 0; index < values.size(); ++index)
                {
                    infer(values[index], index < payload.size() ? payload[index] : unknownType());
                }
                return expected;
            }
        }
        if (key == "Успех" || key == "Ошибка")
        {
            inferArguments(values);
            return unknownType();
        }
        return nullptr;
    }

    TypePtr callFunction(const FunctionInfo& function, const SyntaxNodeView& call,
        const std::vector<SyntaxNodeView>& values, const TypePtr& receiver, bool instance, const TypePtr& expected)
    {
        (void)expected;
        TypeContext context{function.module, {}, receiver};
        for (size_t index = 0; index < function.ownerParameters.size(); ++index)
        {
            const bool known = receiver && receiver->kind == TypeKind::Named && index < receiver->arguments.size();
            context.substitution[function.ownerParameters[index]] = known ? receiver->arguments[index] : unknownType();
        }
        std::vector<SyntaxNodeView> parameters;
        if (const SyntaxNodeView list = function.signature.node(ParamList))
        {
            for (const SyntaxNodeView& parameter : list.nodes())
            {
                if (parameter.kind() == Param && !parameter.hasToken(KwSelf))
                {
                    parameters.push_back(parameter);
                }
            }
        }
        size_t comptime = 0;
        for (const SyntaxNodeView& parameter : parameters)
        {
            comptime += parameter.hasToken(KwComptime) ? 1 : 0;
        }
        const bool explicitTypes = values.size() == parameters.size() && comptime > 0;
        size_t valueIndex = 0;
        for (const SyntaxNodeView& parameter : parameters)
        {
            const std::optional<SyntaxToken> name = parameter.token(Identifier);
            if (parameter.hasToken(KwComptime))
            {
                TypePtr bound = unknownType();
                if (explicitTypes && valueIndex < values.size())
                {
                    bound = resolveType(values[valueIndex], typeContext);
                    ++valueIndex;
                }
                if (name)
                {
                    context.substitution[nameKey(name->text)] = bound;
                }
                continue;
            }
            if (valueIndex >= values.size())
            {
                break;
            }
            if (parameter.hasToken(Ellipsis))
            {
                for (; valueIndex < values.size(); ++valueIndex)
                {
                    infer(values[valueIndex], unknownType());
                }
                break;
            }
            const std::vector<SyntaxNodeView> parts = parameter.nodes();
            const TypePtr declared = parts.empty() ? unknownType() : resolveType(parts.front(), context);
            infer(values[valueIndex], declared);
            ++valueIndex;
        }
        for (; valueIndex < values.size(); ++valueIndex)
        {
            infer(values[valueIndex], unknownType());
        }
        (void)instance;
        if (function.requiresRegion && !regionContext && !functions.empty())
        {
            const FunctionScope& enclosing = functions.back();
            reportNode(316, call,
                say("вызову `", "the call to `") + function.display
                    + say("` нужен контекст `Регион`, а функция `", "` needs a `Регион` context, but function `")
                    + enclosing.name + say("` его не требует", "` does not require it"),
                {ReportNote{enclosing.nameOffset, enclosing.nameLength,
                    enclosing.open ? say("сигнатуру `откр`-функции меняет человек: это изменение API",
                                         "a person changes the signature of an `откр` function: it is an API change")
                                   : say("исправление дописывает `требует Регион` в эту сигнатуру",
                                         "the fix adds `требует Регион` to this signature")}});
        }
        bool afterArrow = false;
        TypePtr result = builtinType("пусто");
        function.signature.forEachChild([&](const SyntaxNodeView& child, const std::optional<SyntaxToken>& token)
        {
            if (token)
            {
                afterArrow = afterArrow || token->kind == Arrow;
                return;
            }
            if (afterArrow && child.kind() != ParamList)
            {
                result = resolveType(child, context);
                afterArrow = false;
            }
        });
        return result;
    }

    TypePtr inferBinary(const SyntaxNodeView& node, const TypePtr& expected)
    {
        const std::vector<SyntaxNodeView> operands = node.nodes();
        if (operands.empty())
        {
            return unknownType();
        }
        const bool shift = node.hasToken(Shl) || node.hasToken(Shr);
        const TypePtr hint = expected && isNumeric(expected) ? expected : unknownType();
        TypePtr left = infer(operands.front(), hint);
        for (size_t index = 1; index < operands.size(); ++index)
        {
            const TypePtr anchor = isNumeric(left) ? left : hint;
            const TypePtr right = infer(operands[index], shift ? unknownType() : anchor);
            if (!shift && isNumeric(left) && isNumeric(right) && left->builtin != right->builtin)
            {
                reportConversion(left, right, operands[index]);
            }
            if (left->kind == TypeKind::LiteralInteger || left->kind == TypeKind::LiteralFloat)
            {
                left = right;
            }
            else if (!isNumeric(left))
            {
                left = unknownType();
            }
        }
        return left;
    }

    void reportConversion(const TypePtr& left, const TypePtr& right, const SyntaxNodeView& operand)
    {
        reportNode(304, operand,
            "`" + show(left) + say("` и `", "` and `") + show(right)
                + say("` — разные типы, а неявных преобразований нет: напишите `",
                    "` are different types and there are no implicit conversions: write `")
                + text(operand) + " как " + show(left) + "`");
    }

    void inferComparison(const SyntaxNodeView& node)
    {
        const std::vector<SyntaxNodeView> operands = node.nodes();
        if (operands.size() < 2)
        {
            return;
        }
        const TypePtr left = infer(operands[0], unknownType());
        const TypePtr right = infer(operands[1], isNumeric(left) ? left : unknownType());
        if (isNumeric(left) && isNumeric(right) && left->builtin != right->builtin)
        {
            reportConversion(left, right, operands[1]);
        }
    }

    TypePtr inferPrefix(const SyntaxNodeView& node, const TypePtr& expected)
    {
        const std::vector<SyntaxNodeView> parts = node.nodes();
        if (parts.empty())
        {
            return unknownType();
        }
        const SyntaxNodeView operand = parts.front();
        if (node.hasToken(KwTry))
        {
            return inferTry(node, operand);
        }
        if (node.hasToken(Minus))
        {
            if (operand.kind() == Literal && operand.token(Integer) && expected && expected->kind == TypeKind::Integer)
            {
                checkLiteralRange(*operand.token(Integer), expected, true);
                return expected;
            }
            return infer(operand, expected);
        }
        if (node.hasToken(Star))
        {
            const TypePtr inner = infer(operand, unknownType());
            return inner && (inner->kind == TypeKind::Reference || inner->kind == TypeKind::Pointer) ? inner->inner
                                                                                                     : unknownType();
        }
        return infer(operand, expected);
    }

    TypePtr inferTry(const SyntaxNodeView& node, const SyntaxNodeView& operand)
    {
        const TypePtr value = infer(operand, unknownType());
        const std::optional<SyntaxToken> keyword = node.token(KwTry);
        if (functions.empty() || !keyword)
        {
            return unknownType();
        }
        const FunctionScope& function = functions.back();
        const uint32_t length = static_cast<uint32_t>(keyword->text.size());
        if (isKnown(function.result) && !isResultType(function.result))
        {
            report(308, keyword->offset, length,
                say("`попытка` допустима только в функции, возвращающей `Итог`, а `",
                    "`попытка` is allowed only in a function returning `Итог`, but `")
                    + function.name + say("` возвращает `", "` returns `") + show(function.result) + "`");
            return isResultType(value) ? settled(value->arguments[0]) : unknownType();
        }
        if (!isResultType(value) || value->arguments.size() < 2)
        {
            return unknownType();
        }
        const TypePtr error = value->arguments[1];
        if (isResultType(function.result) && function.result->arguments.size() >= 2 && isKnown(error)
            && isKnown(function.result->arguments[1]) && !sameType(error, function.result->arguments[1]))
        {
            checkWrap(error, function.result->arguments[1], *keyword);
        }
        return value->arguments[0];
    }

    void checkWrap(const TypePtr& error, const TypePtr& target, const SyntaxToken& keyword)
    {
        size_t wrappers = 0;
        if (target->kind == TypeKind::Named && target->decl->kind == DeclKind::Enum)
        {
            for (const VariantDecl& variant : target->decl->variants)
            {
                const std::vector<TypePtr> payload = variantPayload(target, variant.key);
                wrappers += payload.size() == 1 && sameType(payload.front(), error) ? 1 : 0;
            }
        }
        if (wrappers == 1)
        {
            return;
        }
        std::vector<ReportNote> notes;
        if (target->kind == TypeKind::Named && target->decl->module == &module)
        {
            notes.push_back(ReportNote{target->decl->nameOffset, target->decl->nameLength,
                say("тип ошибки функции объявлен здесь", "the function's error type is declared here")});
        }
        report(309, keyword.offset, static_cast<uint32_t>(keyword.text.size()),
            say("ошибку типа `", "an error of type `") + show(error)
                + say("` нечем обернуть: в `", "` cannot be wrapped: `") + show(target)
                + (wrappers == 0
                        ? say("` нет варианта с такой нагрузкой", "` has no variant with such a payload")
                        : say("` несколько вариантов с такой нагрузкой", "` has several variants with such a payload")),
            std::move(notes));
    }

    TypePtr inferIndex(const SyntaxNodeView& node)
    {
        const std::vector<SyntaxNodeView> parts = node.nodes();
        if (parts.size() < 2)
        {
            return unknownType();
        }
        const TypePtr receiver = stripReferences(infer(parts[0], unknownType()));
        const TypePtr index = infer(parts[1], unknownType());
        if (receiver && receiver->kind == TypeKind::Str)
        {
            const std::string base = text(parts[0]);
            const std::string position = text(parts[1]);
            reportNode(314, node,
                say("строку не индексируют: байт даёт `", "strings are not indexed: a byte is `") + base + ".байты()["
                    + position + say("]`, символы и графемы — `", "]`, characters and graphemes are `") + base
                    + say(".символы()` и `", ".символы()` and `") + base + ".графемы()`");
            return unknownType();
        }
        if (receiver && (receiver->kind == TypeKind::Slice || receiver->kind == TypeKind::Array))
        {
            if (parts[1].kind() == RangeExpr)
            {
                auto slice = std::make_shared<Type>();
                slice->kind = TypeKind::Slice;
                slice->mutableAccess = receiver->mutableAccess;
                slice->inner = receiver->inner;
                return slice;
            }
            return receiver->inner;
        }
        (void)index;
        return unknownType();
    }

    TypePtr inferLiteral(const SyntaxNodeView& node)
    {
        const std::vector<SyntaxNodeView> parts = node.nodes();
        if (parts.empty())
        {
            return unknownType();
        }
        TypePtr type = resolveType(parts.front(), typeContext);
        if (parts.front().kind() == ArrayType && type->kind == TypeKind::Array)
        {
            for (size_t index = 1; index < parts.size(); ++index)
            {
                for (const SyntaxNodeView& value : parts[index].nodes())
                {
                    infer(value, type->inner);
                }
            }
            return type;
        }
        for (size_t index = 1; index < parts.size(); ++index)
        {
            const SyntaxNodeView& element = parts[index];
            const std::vector<SyntaxNodeView> values = element.nodes();
            if (values.empty())
            {
                continue;
            }
            const std::optional<SyntaxToken> label = element.token(Identifier);
            const std::optional<SyntaxToken> fieldName = label ? label : values.front().token(Identifier);
            const TypePtr expected = fieldName ? fieldOf(type, *fieldName, false) : unknownType();
            infer(values.front(), expected);
        }
        return type;
    }

    TypePtr inferMatch(const SyntaxNodeView& node, const TypePtr& expected)
    {
        const std::vector<SyntaxNodeView> parts = node.nodes();
        TypePtr scrutinee = unknownType();
        TypePtr result = expected && isKnown(expected) ? expected : unknownType();
        for (const SyntaxNodeView& part : parts)
        {
            if (part.kind() != MatchArm)
            {
                scrutinee = infer(part, unknownType());
                continue;
            }
            scopes.emplace_back();
            const std::vector<SyntaxNodeView> arm = part.nodes();
            for (size_t index = 0; index < arm.size(); ++index)
            {
                if (index == 0)
                {
                    bindPattern(arm[index], scrutinee);
                }
                else if (arm[index].kind() == Block)
                {
                    checkBlock(arm[index]);
                }
                else if (arm[index].kind() == ExprStmt && index + 1 == arm.size() && !arm[index].nodes().empty())
                {
                    const TypePtr value = infer(arm[index].nodes().front(), result);
                    if (!isKnown(result) && isKnown(value) && value->kind != TypeKind::Never)
                    {
                        result = settled(value);
                    }
                }
                else if (index + 1 == arm.size() && isExpressionKind(arm[index]))
                {
                    const TypePtr value = infer(arm[index], result);
                    if (!isKnown(result) && isKnown(value) && value->kind != TypeKind::Never)
                    {
                        result = settled(value);
                    }
                }
                else if (isExpressionKind(arm[index]))
                {
                    infer(arm[index], builtinType("бул"));
                }
                else
                {
                    checkStatement(arm[index]);
                }
            }
            scopes.pop_back();
        }
        return result;
    }

    static bool isExpressionKind(const SyntaxNodeView& node)
    {
        switch (node.kind())
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
            return false;
        default:
            return true;
        }
    }

    void inferClosure(const SyntaxNodeView& node)
    {
        scopes.emplace_back();
        FunctionScope closure;
        closure.result = unknownType();
        closure.name = functions.empty() ? std::string() : functions.back().name;
        closure.nameOffset = functions.empty() ? 0 : functions.back().nameOffset;
        closure.nameLength = functions.empty() ? 0 : functions.back().nameLength;
        closure.open = !functions.empty() && functions.back().open;
        functions.push_back(closure);
        for (const SyntaxNodeView& child : node.nodes())
        {
            if (child.kind() == FunctionParam)
            {
                const std::optional<SyntaxToken> name = child.token(Identifier);
                const std::vector<SyntaxNodeView> parts = child.nodes();
                if (name && child.hasToken(Colon))
                {
                    bind(*name, parts.empty() ? unknownType() : resolveType(parts.front(), typeContext));
                }
            }
            else if (child.kind() == Block)
            {
                checkBlock(child);
            }
            else if (isExpressionKind(child))
            {
                infer(child, unknownType());
            }
        }
        functions.pop_back();
        scopes.pop_back();
    }

    Program& program;
    SourceModule& module;
    Language language;
    Declarations declarations;
    std::vector<Report> reports;
    std::vector<std::map<std::string, LocalInfo>> scopes;
    std::deque<FunctionScope> functions;
    TypeContext typeContext;
    bool regionContext = false;
    std::set<std::string> reportedBounds;
    std::set<uint32_t> aliasesInProgress;
    std::unique_ptr<ComptimeEvaluator> evaluator;
};

}

std::vector<Report> checkTypes(
    Program& program, SourceModule& module, Language language, const EvaluatorFactory& evaluators)
{
    if (module.kind != ModuleKind::Source || !module.root || !module.syntax.empty())
    {
        return {};
    }
    Checker checker(program, module, language);
    checker.attach(evaluators);
    return checker.run();
}

}

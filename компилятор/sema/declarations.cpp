#include "sema/declarations.hpp"

#include "sema/naming.hpp"
#include "syntax/lexer.hpp"

#include <algorithm>
#include <optional>
#include <set>

namespace splav
{

using SourceModule = Module;

using enum SyntaxKind;

bool requiresRegion(const SyntaxNodeView& signature)
{
    bool afterRequires = false;
    for (const SyntaxToken& token : signature.tokens())
    {
        if (token.kind == KwRequires)
        {
            afterRequires = true;
        }
        else if (afterRequires && token.kind == Identifier)
        {
            return nameKey(token.text) == REGION_TYPE;
        }
    }
    return false;
}

bool hasAttribute(const SyntaxNodeView& item, std::string_view name)
{
    for (const SyntaxNodeView& child : item.nodes())
    {
        if (child.kind() != Attribute)
        {
            continue;
        }
        const std::optional<SyntaxToken> identifier = child.token(Identifier);
        if (identifier && nameKey(identifier->text) == name)
        {
            return true;
        }
    }
    return false;
}

std::string firstLine(std::string_view source, uint32_t offset, uint32_t end)
{
    std::string_view text = source.substr(offset, end - offset);
    return std::string(text.substr(0, text.find('\n')));
}

Declarations::Declarations(Program& owner, SourceModule& main) : program(owner)
{
    prelude = program.prelude();
    std::vector<SourceModule*> pending{&main};
    if (prelude != nullptr)
    {
        pending.push_back(prelude);
    }
    std::set<SourceModule*> seen;
    while (!pending.empty())
    {
        SourceModule* next = pending.back();
        pending.pop_back();
        if (next == nullptr || !seen.insert(next).second || next->kind != ModuleKind::Source || !next->root
            || !next->syntax.empty())
        {
            continue;
        }
        order.push_back(next);
        collect(*next);
        for (const auto& [key, entry] : scopes[next])
        {
            if (entry.kind == EntryKind::Module)
            {
                pending.push_back(entry.target);
            }
        }
    }
    for (SourceModule* module : order)
    {
        linkMembers(*module);
    }
}

const Entry* Declarations::lookup(SourceModule* module, const std::string& key) const
{
    if (const auto scope = scopes.find(module); scope != scopes.end())
    {
        if (const auto found = scope->second.find(key); found != scope->second.end())
        {
            return &found->second;
        }
    }
    if (prelude != nullptr && module != prelude)
    {
        if (const auto scope = scopes.find(prelude); scope != scopes.end())
        {
            if (const auto found = scope->second.find(key); found != scope->second.end())
            {
                return &found->second;
            }
        }
    }
    return nullptr;
}

const FunctionInfo* Declarations::method(
    const TypeDecl* owner, const std::string& builtinOwner, const std::string& key) const
{
    if (owner != nullptr)
    {
        if (const auto found = methods.find(owner); found != methods.end())
        {
            if (const auto method = found->second.find(key); method != found->second.end())
            {
                return method->second;
            }
        }
        return nullptr;
    }
    if (const auto found = builtinMethods.find(builtinOwner); found != builtinMethods.end())
    {
        if (const auto method = found->second.find(key); method != found->second.end())
        {
            return method->second;
        }
    }
    return nullptr;
}

bool Declarations::hasImpl(const TypeDecl* trait, const TypeDecl* target, const std::string& builtinTarget) const
{
    return std::any_of(impls.begin(), impls.end(), [&](const ImplInfo& impl) {
        return impl.trait == trait && (target != nullptr ? impl.target == target : impl.builtinTarget == builtinTarget);
    });
}

void Declarations::collect(SourceModule& module)
{
    std::map<std::string, Entry>& scope = scopes[&module];
    const SyntaxNodeView root(module.root.get(), 0);
    for (const SyntaxNodeView& item : root.nodes())
    {
        switch (item.kind())
        {
        case Import:
        {
            std::string path;
            std::optional<SyntaxToken> alias;
            bool afterAs = false;
            for (const SyntaxToken& token : item.tokens())
            {
                if (token.kind == KwAs)
                {
                    afterAs = true;
                }
                else if (token.kind == Identifier && afterAs)
                {
                    alias = token;
                }
                else if (token.kind == Identifier)
                {
                    path += (path.empty() ? "" : ".") + normalizedName(token.text);
                    alias = token;
                }
            }
            if (alias)
            {
                scope[nameKey(alias->text)] = Entry{EntryKind::Module, nullptr, nullptr, {}, program.find(path)};
            }
            break;
        }
        case CInclude:
            if (const std::optional<SyntaxToken> alias = item.token(Identifier))
            {
                scope[nameKey(alias->text)] = Entry{EntryKind::Header, nullptr, nullptr, {}, nullptr};
            }
            else if (const std::optional<SyntaxToken> file = item.token(String))
            {
                std::string header(file->text.substr(1, file->text.size() - 2));
                const size_t slash = header.find_last_of('/');
                header = header.substr(slash == std::string::npos ? 0 : slash + 1);
                scope[nameKey(header.substr(0, header.find('.')))] =
                    Entry{EntryKind::Header, nullptr, nullptr, {}, nullptr};
            }
            break;
        case ConstDecl:
            if (const std::optional<SyntaxToken> name = item.token(Identifier))
            {
                scope[nameKey(name->text)] = Entry{EntryKind::Const, nullptr, nullptr, item, nullptr};
            }
            break;
        case StructDecl:
        case EnumDecl:
        case TraitDecl:
            if (const std::optional<SyntaxToken> name = item.token(Identifier))
            {
                TypeDecl& decl = types.emplace_back();
                decl.kind = item.kind() == EnumDecl ? DeclKind::Enum
                    : item.kind() == TraitDecl      ? DeclKind::Trait
                                                    : DeclKind::Struct;
                decl.key = nameKey(name->text);
                decl.display = normalizedName(name->text);
                decl.module = &module;
                decl.node = item;
                decl.nameOffset = name->offset;
                decl.nameLength = static_cast<uint32_t>(name->text.size());
                describe(decl);
                scope[decl.key] = Entry{EntryKind::Type, &decl, nullptr, {}, nullptr};
            }
            break;
        case FunctionDecl:
        {
            const SyntaxNodeView signature = item.node(Signature);
            if (signature && !signature.hasToken(Dot))
            {
                FunctionInfo& function = addFunction(module, item, signature);
                scope[function.key] = Entry{EntryKind::Function, nullptr, &function, {}, nullptr};
            }
            break;
        }
        default:
            break;
        }
    }
}

void Declarations::describe(TypeDecl& decl)
{
    if (const SyntaxNodeView parameters = decl.node.node(ComptimeParamList))
    {
        for (const SyntaxNodeView& parameter : parameters.nodes())
        {
            const std::optional<SyntaxToken> name = parameter.token(Identifier);
            if (parameter.kind() != Param || !name)
            {
                continue;
            }
            decl.parameters.push_back(nameKey(name->text));
            decl.parameterDisplays.push_back(normalizedName(name->text));
            const std::vector<SyntaxNodeView> parts = parameter.nodes();
            decl.bounds.push_back(parts.empty() ? SyntaxNodeView() : parts.front());
        }
    }
    if (const SyntaxNodeView fields = decl.node.node(FieldList))
    {
        for (const SyntaxNodeView& field : fields.nodes())
        {
            const std::optional<SyntaxToken> name = field.token(Identifier);
            if (field.kind() != Field || !name)
            {
                continue;
            }
            FieldDecl described;
            described.key = nameKey(name->text);
            described.display = normalizedName(name->text);
            for (const SyntaxNodeView& part : field.nodes())
            {
                if (part.kind() != Attribute)
                {
                    described.type = part;
                }
            }
            described.open = field.hasToken(KwPub);
            described.offset = name->offset;
            described.length = static_cast<uint32_t>(name->text.size());
            decl.fields.push_back(described);
        }
    }
    if (const SyntaxNodeView variants = decl.node.node(VariantList))
    {
        for (const SyntaxNodeView& variant : variants.nodes())
        {
            const std::optional<SyntaxToken> name = variant.token(Identifier);
            if (variant.kind() != Variant || !name)
            {
                continue;
            }
            VariantDecl described;
            described.key = nameKey(name->text);
            described.display = normalizedName(name->text);
            for (const SyntaxNodeView& field : variant.nodes())
            {
                if (field.kind() != VariantField)
                {
                    continue;
                }
                const std::optional<SyntaxToken> label = field.token(Identifier);
                for (const SyntaxNodeView& part : field.nodes())
                {
                    described.payload.push_back(part);
                    described.labels.push_back(label ? nameKey(label->text) : std::string());
                }
            }
            decl.variants.push_back(described);
        }
    }
}

FunctionInfo& Declarations::addFunction(
    SourceModule& module, const SyntaxNodeView& item, const SyntaxNodeView& signature)
{
    FunctionInfo& function = functions.emplace_back();
    function.module = &module;
    function.item = item;
    function.signature = signature;
    function.builtin = hasAttribute(item, BUILTIN_ATTRIBUTE);
    function.open = item.hasToken(KwPub) || signature.hasToken(KwPub);
    std::vector<SyntaxToken> names;
    for (const SyntaxToken& token : signature.tokens())
    {
        if (token.kind == Identifier)
        {
            names.push_back(token);
        }
    }
    const SyntaxToken name = signature.hasToken(Dot) && names.size() >= 2 ? names[1] : names.front();
    function.key = nameKey(name.text);
    function.display = normalizedName(name.text);
    function.nameOffset = name.offset;
    function.nameLength = static_cast<uint32_t>(name.text.size());
    function.requiresRegion = requiresRegion(signature);
    return function;
}

void Declarations::linkMembers(SourceModule& module)
{
    const SyntaxNodeView root(module.root.get(), 0);
    for (const SyntaxNodeView& item : root.nodes())
    {
        if (item.kind() == FunctionDecl)
        {
            const SyntaxNodeView signature = item.node(Signature);
            if (signature && signature.hasToken(Dot))
            {
                registerMethod(module, item, signature);
            }
        }
        else if (item.kind() == ImplDecl)
        {
            registerImpl(module, item);
        }
    }
}

void Declarations::registerMethod(SourceModule& module, const SyntaxNodeView& item, const SyntaxNodeView& signature)
{
    FunctionInfo& function = addFunction(module, item, signature);
    const std::optional<SyntaxToken> owner = signature.token(Identifier);
    if (!owner)
    {
        return;
    }
    const std::string ownerKey = nameKey(owner->text);
    if (const SyntaxNodeView parameters = signature.node(ComptimeParamList))
    {
        for (const SyntaxNodeView& parameter : parameters.nodes())
        {
            if (const std::optional<SyntaxToken> name = parameter.token(Identifier))
            {
                function.ownerParameters.push_back(nameKey(name->text));
            }
        }
    }
    const Entry* entry = lookup(&module, ownerKey);
    if (entry != nullptr && entry->kind == EntryKind::Type)
    {
        function.owner = entry->type;
        methods[entry->type][function.key] = &function;
    }
    else
    {
        function.builtinOwner = ownerKey;
        builtinMethods[ownerKey][function.key] = &function;
    }
}

const TypeDecl* Declarations::typeDeclOf(SourceModule& module, const SyntaxNodeView& node, std::string& builtinName)
{
    SyntaxNodeView head = node;
    if (head.kind() == CallExpr && !head.nodes().empty())
    {
        head = head.nodes().front();
    }
    if (head.kind() == NameExpr)
    {
        const std::optional<SyntaxToken> name = head.token(Identifier);
        if (!name)
        {
            return nullptr;
        }
        const std::string key = nameKey(name->text);
        const Entry* entry = lookup(&module, key);
        if (entry != nullptr && entry->kind == EntryKind::Type)
        {
            return entry->type;
        }
        builtinName = key;
        return nullptr;
    }
    if (head.kind() == FieldExpr)
    {
        const std::vector<SyntaxNodeView> parts = head.nodes();
        const std::optional<SyntaxToken> member = head.token(Identifier);
        if (parts.empty() || !member || parts.front().kind() != NameExpr)
        {
            return nullptr;
        }
        const std::optional<SyntaxToken> moduleName = parts.front().token(Identifier);
        const Entry* entry = moduleName ? lookup(&module, nameKey(moduleName->text)) : nullptr;
        if (entry == nullptr || entry->kind != EntryKind::Module || entry->target == nullptr)
        {
            return nullptr;
        }
        const Entry* memberEntry = lookup(entry->target, nameKey(member->text));
        return memberEntry != nullptr && memberEntry->kind == EntryKind::Type ? memberEntry->type : nullptr;
    }
    return nullptr;
}

void Declarations::registerImpl(SourceModule& module, const SyntaxNodeView& item)
{
    std::vector<SyntaxNodeView> parts;
    for (const SyntaxNodeView& child : item.nodes())
    {
        if (child.kind() != Attribute && child.kind() != ComptimeParamList && child.kind() != MethodList)
        {
            parts.push_back(child);
        }
    }
    if (parts.size() < 2)
    {
        return;
    }
    ImplInfo impl;
    std::string ignored;
    impl.trait = typeDeclOf(module, parts[0], ignored);
    impl.target = typeDeclOf(module, parts[1], impl.builtinTarget);
    impl.module = &module;
    impl.node = item;
    impl.builtin = hasAttribute(item, BUILTIN_ATTRIBUTE);
    impls.push_back(impl);
    if (const SyntaxNodeView list = item.node(MethodList))
    {
        for (const SyntaxNodeView& member : list.nodes())
        {
            const SyntaxNodeView signature = member.node(Signature);
            if (member.kind() != FunctionDecl || !signature)
            {
                continue;
            }
            FunctionInfo& function = addFunction(module, member, signature);
            function.owner = impl.target;
            function.builtinOwner = impl.target == nullptr ? impl.builtinTarget : "";
            if (impl.target != nullptr && !methods[impl.target].contains(function.key))
            {
                methods[impl.target][function.key] = &function;
            }
        }
    }
}

}

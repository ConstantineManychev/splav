#include "sema/names.hpp"

#include "sema/declarations.hpp"
#include "sema/naming.hpp"

#include "sema/syntax_view.hpp"
#include "syntax/lexer.hpp"
#include "unicode/unicode.hpp"

#include <algorithm>
#include <array>
#include <deque>
#include <functional>
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

constexpr std::array BUILTIN_TYPES = {
    "ц8",
    "ц16",
    "ц32",
    "ц64",
    "ц128",
    "н8",
    "н16",
    "н32",
    "н64",
    "н128",
    "в16",
    "в32",
    "в64",
    "разм",
    "смещ",
    "компил_цел",
    "компил_вещ",
    "бул",
    "сим",
    "стр",
    "тип",
    "пусто",
    "никогда",
    "Вектор",
};
constexpr std::array BUILTIN_VALUES = {
    "Есть",
    "Ничего",
    "чтение_волатил",
    "запись_волатил",
    "утвердить",
};
constexpr size_t SIMILAR_NAME_DISTANCE = 1;

size_t editDistance(const std::u32string& left, const std::u32string& right)
{
    std::vector<size_t> previous(right.size() + 1);
    std::vector<size_t> current(right.size() + 1);
    for (size_t column = 0; column <= right.size(); ++column)
    {
        previous[column] = column;
    }
    for (size_t row = 1; row <= left.size(); ++row)
    {
        current[0] = row;
        for (size_t column = 1; column <= right.size(); ++column)
        {
            const size_t substitution = previous[column - 1] + (left[row - 1] == right[column - 1] ? 0 : 1);
            current[column] = std::min({previous[column] + 1, current[column - 1] + 1, substitution});
        }
        std::swap(previous, current);
    }
    return previous[right.size()];
}

std::string skeletonOf(std::string_view name)
{
    return unicode::encodeUtf8(unicode::skeleton(unicode::toNfc(decodedName(normalizedName(name)))));
}

std::string_view writtenScript(std::string_view name, Language language)
{
    bool latin = false;
    bool cyrillic = false;
    for (const char32_t symbol : decodedName(name))
    {
        const unicode::Script script = unicode::identifierScript(symbol);
        latin = latin || script == unicode::Script::Latin;
        cyrillic = cyrillic || script == unicode::Script::Cyrillic;
    }
    const bool russian = language == Language::Russian;
    if (latin && !cyrillic)
    {
        return russian ? "латиницей" : "in Latin script";
    }
    if (cyrillic && !latin)
    {
        return russian ? "кириллицей" : "in Cyrillic script";
    }
    return russian ? "смешанным письмом" : "in mixed scripts";
}

std::string aliasSuggestion(const std::string& name)
{
    constexpr std::u32string_view VOWELS = U"аеёиоуыэюяaeiouy";
    const std::u32string symbols = decodedName(name);
    std::u32string shortened;
    for (size_t index = 0; index < symbols.size(); ++index)
    {
        if (index == 0 || VOWELS.find(symbols[index]) == std::u32string_view::npos)
        {
            shortened += symbols[index];
        }
    }
    if (shortened.size() >= 2 && shortened != symbols)
    {
        return unicode::encodeUtf8(shortened);
    }
    return name + "М";
}

enum class SymbolKind
{
    Type,
    Enum,
    Trait,
    Const,
    Function,
    SourceModule,
    Header,
    Builtin,
};

struct Symbol
{
    SymbolKind kind = SymbolKind::Builtin;
    std::string display;
    uint32_t offset = 0;
    uint32_t length = 0;
    bool open = true;
    SourceModule* target = nullptr;
    std::string importPath;
    std::string skeleton;
};

Symbol symbolOf(SymbolKind kind, bool open)
{
    Symbol symbol;
    symbol.kind = kind;
    symbol.open = open;
    return symbol;
}

struct ModuleSymbols
{
    std::map<std::string, Symbol> names;
    std::vector<std::string> order;
    std::vector<std::string> variants;
    std::vector<Report> reports;
};

enum class LocalKind
{
    Parameter,
    Comptime,
    Binding,
};

struct Local
{
    std::string key;
    std::string display;
    uint32_t offset = 0;
    uint32_t length = 0;
    LocalKind kind = LocalKind::Binding;
    bool mutableBinding = false;
    bool used = false;
    const Local* similar = nullptr;
    std::string skeleton;
    std::u32string written;
};

struct ConstEdge
{
    std::string target;
    uint32_t offset;
    uint32_t length;
};

std::string identifierText(const SyntaxToken& token)
{
    return normalizedName(token.text);
}

std::string pathText(const SyntaxNodeView& node)
{
    std::string text;
    for (const SyntaxToken& token : node.tokens())
    {
        if (token.kind == Identifier)
        {
            text += (text.empty() ? "" : ".") + identifierText(token);
        }
        else if (token.kind == KwAs)
        {
            break;
        }
    }
    return text;
}

class Resolver
{
public:
    Resolver(Program& owner, SourceModule& checked, Language chosen) : program(owner), module(checked), language(chosen)
    {
    }

    std::vector<Report> run()
    {
        prepareModules();
        const ModuleSymbols& own = symbolsOf(module);
        reports.insert(reports.end(), own.reports.begin(), own.reports.end());
        checkFileTier(own);
        const SyntaxNodeView root(module.root.get(), 0);
        for (const SyntaxNodeView& item : root.nodes())
        {
            visitItem(item);
        }
        checkConstCycles();
        std::stable_sort(reports.begin(), reports.end(),
            [](const Report& left, const Report& right) { return left.offset < right.offset; });
        return std::move(reports);
    }

    void attach(const EvaluatorFactory& factory)
    {
        evaluators = factory;
    }

    std::vector<Respelling> spellings()
    {
        run();
        std::sort(respellings.begin(), respellings.end(),
            [](const Respelling& left, const Respelling& right) { return left.offset < right.offset; });
        respellings.erase(
            std::unique(respellings.begin(), respellings.end(),
                [](const Respelling& left, const Respelling& right) { return left.offset == right.offset; }),
            respellings.end());
        return std::move(respellings);
    }

private:
    std::string say(std::string_view russian, std::string_view english) const
    {
        return std::string(language == Language::Russian ? russian : english);
    }

    void report(
        uint16_t code, uint32_t offset, uint32_t length, std::string message, std::vector<ReportNote> notes = {})
    {
        reports.push_back(Report{code, offset, length, std::move(message), std::move(notes)});
    }

    void prepareModules()
    {
        std::vector<SourceModule*> pending{&module};
        if (SourceModule* prelude = program.prelude())
        {
            pending.push_back(prelude);
        }
        std::set<SourceModule*> seen;
        while (!pending.empty())
        {
            SourceModule* next = pending.back();
            pending.pop_back();
            if (!seen.insert(next).second || next->kind != ModuleKind::Source)
            {
                continue;
            }
            const ModuleSymbols& symbols = symbolsOf(*next);
            variants.insert(symbols.variants.begin(), symbols.variants.end());
            for (const auto& [key, symbol] : symbols.names)
            {
                if (symbol.target != nullptr)
                {
                    pending.push_back(symbol.target);
                }
            }
        }
        if (SourceModule* prelude = program.prelude())
        {
            preludeSymbols = &symbolsOf(*prelude);
        }
    }

    const ModuleSymbols& symbolsOf(SourceModule& target)
    {
        if (const auto found = cache.find(&target); found != cache.end())
        {
            return found->second;
        }
        ModuleSymbols& symbols = cache[&target];
        if (target.kind != ModuleKind::Source || !target.root)
        {
            return symbols;
        }
        const SyntaxNodeView root(target.root.get(), 0);
        for (const SyntaxNodeView& item : root.nodes())
        {
            collectDeclaration(target, item, symbols);
        }
        return symbols;
    }

    void addSymbol(ModuleSymbols& symbols, const SyntaxToken& name, Symbol symbol)
    {
        symbol.display = identifierText(name);
        symbol.skeleton = skeletonOf(symbol.display);
        symbol.offset = name.offset;
        symbol.length = static_cast<uint32_t>(name.text.size());
        const std::string key = nameKey(name.text);
        if (const auto existing = symbols.names.find(key); existing != symbols.names.end())
        {
            symbols.reports.push_back(Report{203, symbol.offset, symbol.length,
                say("имя `", "name `") + symbol.display
                    + say("` уже объявлено в этом модуле", "` is already declared in this module"),
                {ReportNote{existing->second.offset, existing->second.length,
                    say("первое объявление здесь", "first declared here")}}});
            return;
        }
        symbols.order.push_back(key);
        symbols.names.emplace(key, std::move(symbol));
    }

    void collectDeclaration(SourceModule& owner, const SyntaxNodeView& item, ModuleSymbols& symbols)
    {
        const bool open = item.hasToken(KwPub);
        switch (item.kind())
        {
        case Import:
        {
            const std::vector<SyntaxToken> tokens = item.tokens();
            std::optional<SyntaxToken> alias;
            std::optional<SyntaxToken> last;
            bool afterAs = false;
            for (const SyntaxToken& token : tokens)
            {
                if (token.kind == KwAs)
                {
                    afterAs = true;
                }
                else if (token.kind == Identifier)
                {
                    (afterAs ? alias : last) = token;
                }
            }
            if (!last)
            {
                return;
            }
            const std::string path = pathText(item);
            Symbol symbol = symbolOf(SymbolKind::SourceModule, true);
            symbol.importPath = path;
            symbol.target = program.find(path);
            if (symbol.target == nullptr && &owner == &module)
            {
                const SyntaxToken first = *item.token(Identifier);
                symbols.reports.push_back(Report{206, first.offset, last->end() - first.offset,
                    say("модуль `", "module `") + path + say("` не найден: нет файла `", "` not found: no file `")
                        + modulePathText(path) + "`",
                    {}});
            }
            addSymbol(symbols, alias ? *alias : *last, std::move(symbol));
            return;
        }
        case CInclude:
        {
            const std::optional<SyntaxToken> file = item.token(String);
            if (!file)
            {
                return;
            }
            std::string header(file->text.substr(1, file->text.size() - 2));
            Symbol symbol = symbolOf(SymbolKind::Header, true);
            symbol.target = program.header(header);
            symbol.importPath = header;
            if (const std::optional<SyntaxToken> alias = item.token(Identifier))
            {
                addSymbol(symbols, *alias, std::move(symbol));
                return;
            }
            const size_t slash = header.find_last_of('/');
            std::string stem = header.substr(slash == std::string::npos ? 0 : slash + 1);
            stem = stem.substr(0, stem.find('.'));
            const std::string_view stored = *headerNames.insert(stem).first;
            addSymbol(symbols, SyntaxToken{Identifier, stored, file->offset}, std::move(symbol));
            symbols.names[nameKey(stored)].length = static_cast<uint32_t>(file->text.size());
            return;
        }
        case ConstDecl:
            if (const std::optional<SyntaxToken> name = item.token(Identifier))
            {
                addSymbol(symbols, *name, symbolOf(SymbolKind::Const, open));
            }
            return;
        case StructDecl:
            if (const std::optional<SyntaxToken> name = item.token(Identifier))
            {
                addSymbol(symbols, *name, symbolOf(SymbolKind::Type, open));
            }
            return;
        case TraitDecl:
            if (const std::optional<SyntaxToken> name = item.token(Identifier))
            {
                addSymbol(symbols, *name, symbolOf(SymbolKind::Trait, open));
            }
            return;
        case EnumDecl:
            if (const std::optional<SyntaxToken> name = item.token(Identifier))
            {
                addSymbol(symbols, *name, symbolOf(SymbolKind::Enum, open));
                if (const SyntaxNodeView list = item.node(VariantList))
                {
                    for (const SyntaxNodeView& variant : list.nodes())
                    {
                        if (const std::optional<SyntaxToken> variantName = variant.token(Identifier))
                        {
                            symbols.variants.push_back(nameKey(variantName->text));
                        }
                    }
                }
            }
            return;
        case FunctionDecl:
        {
            const SyntaxNodeView signature = item.node(Signature);
            if (!signature || signature.hasToken(Dot))
            {
                return;
            }
            if (const std::optional<SyntaxToken> name = signature.token(Identifier))
            {
                addSymbol(symbols, *name, symbolOf(SymbolKind::Function, open || signature.hasToken(KwPub)));
            }
            return;
        }
        default:
            return;
        }
    }

    bool isPreludeName(const std::string& key) const
    {
        if (std::find(BUILTIN_TYPES.begin(), BUILTIN_TYPES.end(), key) != BUILTIN_TYPES.end()
            || std::find(BUILTIN_VALUES.begin(), BUILTIN_VALUES.end(), key) != BUILTIN_VALUES.end())
        {
            return true;
        }
        return preludeSymbols != nullptr && &module != program.prelude() && preludeSymbols->names.contains(key);
    }

    void checkFileTier(const ModuleSymbols& own)
    {
        std::map<std::string, const Symbol*> bySkeleton;
        for (const std::string& key : own.order)
        {
            const Symbol& symbol = own.names.at(key);
            if (isPreludeName(key))
            {
                report(203, symbol.offset, symbol.length,
                    say("имя `", "name `") + symbol.display
                        + say("` уже объявлено в прелюдии: её не затеняют",
                            "` is declared in the prelude, which is never shadowed"));
            }
            const auto [found, inserted] = bySkeleton.emplace(symbol.skeleton, &symbol);
            if (!inserted)
            {
                const Symbol& first = *found->second;
                reportConfusable(
                    symbol.display, symbol.offset, symbol.length, first.display, first.offset, first.length);
            }
        }
    }

    void reportConfusable(const std::string& name, uint32_t offset, uint32_t length, const std::string& other,
        uint32_t otherOffset, uint32_t otherLength)
    {
        report(208, offset, length,
            say("имя `", "name `") + name + say("` на вид неотличимо от `", "` looks identical to `") + other
                + say("`: у них один скелет UTS #39", "`: they share a UTS #39 skeleton"),
            {ReportNote{otherOffset, otherLength,
                "`" + other + say("` объявлено здесь ", "` is declared here ")
                    + std::string(writtenScript(other, language))}});
    }

    void visitItem(const SyntaxNodeView& item)
    {
        switch (item.kind())
        {
        case ConstDecl:
            if (const std::optional<SyntaxToken> name = item.token(Identifier))
            {
                currentConst = nameKey(name->text);
                constOrder.push_back(currentConst);
                constNames.emplace(currentConst, *name);
            }
            visitChildren(item);
            currentConst.clear();
            return;
        case StructDecl:
        case EnumDecl:
        case TraitDecl:
        case ImplDecl:
            withFrame([&] { visitDeclaration(item); });
            return;
        case FunctionDecl:
            withFrame([&] { visitFunction(item); });
            return;
        default:
            return;
        }
    }

    template <typename Body>
    void withFrame(Body&& body)
    {
        std::vector<std::vector<Local*>> savedScopes = std::move(scopes);
        std::vector<std::string> savedLabels = std::move(labels);
        scopes.assign(1, {});
        labels.clear();
        body();
        closeScope();
        scopes = std::move(savedScopes);
        labels = std::move(savedLabels);
    }

    void visitDeclaration(const SyntaxNodeView& item)
    {
        for (const SyntaxNodeView& child : item.nodes())
        {
            switch (child.kind())
            {
            case Attribute:
                break;
            case ComptimeParamList:
                declareParameters(child);
                break;
            case SignatureList:
            case MethodList:
                for (const SyntaxNodeView& member : child.nodes())
                {
                    if (member.kind() == Signature)
                    {
                        withScope([&] { visitSignature(member); });
                    }
                    else if (member.kind() == FunctionDecl)
                    {
                        withScope([&] { visitFunction(member); });
                    }
                }
                break;
            default:
                visit(child);
                break;
            }
        }
    }

    template <typename Body>
    void withScope(Body&& body)
    {
        scopes.emplace_back();
        body();
        closeScope();
    }

    void visitFunction(const SyntaxNodeView& function)
    {
        for (const SyntaxNodeView& child : function.nodes())
        {
            if (child.kind() == Signature)
            {
                visitSignature(child);
            }
            else if (child.kind() == Block)
            {
                visit(child);
            }
        }
    }

    void visitSignature(const SyntaxNodeView& signature)
    {
        const bool method = signature.hasToken(Dot);
        bool ownerSeen = false;
        typeOnly = true;
        signature.forEachChild([&](const SyntaxNodeView& child, const std::optional<SyntaxToken>& token)
        {
            if (token && token->kind == Identifier && method && !ownerSeen)
            {
                ownerSeen = true;
                resolveName(*token);
            }
            if (!child)
            {
                return;
            }
            if (child.kind() == ComptimeParamList)
            {
                declareParameters(child);
            }
            else if (child.kind() == ParamList)
            {
                declareParameters(child);
            }
            else if (child.kind() != Attribute)
            {
                visit(child);
            }
        });
        typeOnly = false;
    }

    void declareParameters(const SyntaxNodeView& list)
    {
        const bool savedTypeOnly = typeOnly;
        typeOnly = true;
        for (const SyntaxNodeView& parameter : list.nodes())
        {
            if (parameter.kind() != Param)
            {
                continue;
            }
            for (const SyntaxNodeView& part : parameter.nodes())
            {
                visit(part);
            }
            if (const std::optional<SyntaxToken> name = parameter.token(Identifier))
            {
                declare(*name, parameter.hasToken(KwComptime) ? LocalKind::Comptime : LocalKind::Parameter,
                    parameter.hasToken(KwMut));
            }
        }
        typeOnly = savedTypeOnly;
    }

    Local* findLocal(const std::string& key, bool comptimeOnly) const
    {
        for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope)
        {
            for (auto local = scope->rbegin(); local != scope->rend(); ++local)
            {
                if ((*local)->key == key && (!comptimeOnly || (*local)->kind == LocalKind::Comptime))
                {
                    return *local;
                }
            }
        }
        return nullptr;
    }

    Local* declare(const SyntaxToken& name, LocalKind kind, bool mutableBinding)
    {
        Local& local = storage.emplace_back();
        local.key = nameKey(name.text);
        local.display = identifierText(name);
        local.offset = name.offset;
        local.length = static_cast<uint32_t>(name.text.size());
        local.kind = kind;
        local.mutableBinding = mutableBinding;
        local.used = kind != LocalKind::Binding;
        local.skeleton = skeletonOf(local.display);
        local.written = decodedName(local.display);
        for (const std::vector<Local*>& scope : scopes)
        {
            for (const Local* visible : scope)
            {
                if (visible->key == local.key)
                {
                    report(203, local.offset, local.length,
                        say("имя `", "name `") + local.display
                            + say("` уже объявлено в этой функции: затенения нет",
                                "` is already declared in this function: there is no shadowing"),
                        {ReportNote{visible->offset, visible->length,
                            visible->kind == LocalKind::Binding
                                ? say("первое объявление — привязка", "first declared as a binding")
                                : say("первое объявление — параметр", "first declared as a parameter")}});
                }
                else if (visible->skeleton == local.skeleton)
                {
                    reportConfusable(
                        local.display, local.offset, local.length, visible->display, visible->offset, visible->length);
                }
                else if (kind == LocalKind::Binding && local.similar == nullptr
                    && editDistance(visible->written, local.written) <= SIMILAR_NAME_DISTANCE)
                {
                    local.similar = visible;
                }
            }
        }
        scopes.back().push_back(&local);
        return &local;
    }

    void closeScope()
    {
        for (const Local* local : scopes.back())
        {
            if (local->used)
            {
                continue;
            }
            std::vector<ReportNote> notes;
            if (local->similar != nullptr)
            {
                notes.push_back(ReportNote{local->similar->offset, local->similar->length,
                    say("похожее имя `", "similar name `") + local->similar->display
                        + say("` объявлено здесь", "` is declared here")});
            }
            report(202, local->offset, local->length,
                say("привязка `", "binding `") + local->display + say("` не использована", "` is never used"),
                std::move(notes));
        }
        scopes.pop_back();
    }

    const Symbol* fileSymbol(const std::string& key)
    {
        const ModuleSymbols& own = symbolsOf(module);
        if (const auto found = own.names.find(key); found != own.names.end())
        {
            return &found->second;
        }
        return nullptr;
    }

    void noteSpelling(const SyntaxToken& token, const std::string& declared)
    {
        if (token.text.empty() || token.text.front() == '`' || declared.empty())
        {
            return;
        }
        if (identifierText(token) != declared)
        {
            respellings.push_back(Respelling{token.offset, static_cast<uint32_t>(token.text.size()), declared});
        }
    }

    std::optional<const Symbol*> resolveName(const SyntaxToken& token)
    {
        const std::string key = nameKey(token.text);
        if (Local* local = findLocal(key, typeOnly))
        {
            local->used = true;
            noteSpelling(token, local->display);
            return nullptr;
        }
        if (const Symbol* symbol = fileSymbol(key))
        {
            noteConstReference(key, token, *symbol);
            noteSpelling(token, symbol->display);
            return symbol;
        }
        if (isPreludeName(key) || (startsUppercase(token.text) && variants.contains(key)))
        {
            if (preludeSymbols != nullptr)
            {
                if (const auto found = preludeSymbols->names.find(key); found != preludeSymbols->names.end())
                {
                    noteSpelling(token, found->second.display);
                }
            }
            return nullptr;
        }
        report(201, token.offset, static_cast<uint32_t>(token.text.size()),
            say("имя `", "name `") + identifierText(token) + say("` не найдено", "` not found"));
        return std::nullopt;
    }

    void noteConstReference(const std::string& key, const SyntaxToken& token, const Symbol& symbol)
    {
        if (!currentConst.empty() && symbol.kind == SymbolKind::Const)
        {
            constEdges[currentConst].push_back(ConstEdge{key, token.offset, static_cast<uint32_t>(token.text.size())});
        }
    }

    void visitChildren(const SyntaxNodeView& node)
    {
        for (const SyntaxNodeView& child : node.nodes())
        {
            if (child.kind() != Attribute)
            {
                visit(child);
            }
        }
    }

    void visit(const SyntaxNodeView& node)
    {
        switch (node.kind())
        {
        case NameExpr:
            if (const std::optional<SyntaxToken> name = node.token(Identifier))
            {
                resolveName(*name);
            }
            return;
        case Placeholder:
            report(209, node.offset(), node.end() - node.offset(),
                say("`_` — не значение: он стоит только в образце и слева от `=`",
                    "`_` is not a value: it appears only in patterns and on the left of `=`"));
            return;
        case FieldExpr:
            visitField(node);
            return;
        case Block:
            withScope([&] { visitChildren(node); });
            return;
        case BindingStmt:
            visitBinding(node);
            return;
        case AssignStmt:
            visitAssignment(node);
            return;
        case IfStmt:
            visitIf(node);
            return;
        case IsExpr:
            visitIs(node);
            return;
        case MatchExpr:
            visitMatch(node);
            return;
        case LoopStmt:
            visitLoop(node);
            return;
        case BreakStmt:
        case ContinueStmt:
            checkLabel(node);
            return;
        case FunctionExpr:
            visitClosure(node);
            return;
        case AsmOperand:
            for (const SyntaxNodeView& child : node.nodes())
            {
                if (child.kind() != Placeholder)
                {
                    visit(child);
                }
            }
            return;
        case Attribute:
            return;
        default:
            visitChildren(node);
            return;
        }
    }

    void visitField(const SyntaxNodeView& node)
    {
        const std::vector<SyntaxNodeView> children = node.nodes();
        const std::optional<SyntaxToken> member = node.token(Identifier);
        if (children.empty() || children.front().kind() != NameExpr || !member)
        {
            visitChildren(node);
            return;
        }
        const std::optional<SyntaxToken> receiver = children.front().token(Identifier);
        if (!receiver)
        {
            visitChildren(node);
            return;
        }
        const std::string key = nameKey(receiver->text);
        if (Local* local = findLocal(key, typeOnly))
        {
            local->used = true;
            noteSpelling(*receiver, local->display);
            const Symbol* shadowed = fileSymbol(key);
            if (shadowed != nullptr
                && (shadowed->kind == SymbolKind::SourceModule || shadowed->kind == SymbolKind::Header))
            {
                reportShadowedModule(*receiver, *local, *shadowed);
            }
            return;
        }
        const std::optional<const Symbol*> resolved = resolveName(*receiver);
        if (resolved && *resolved != nullptr)
        {
            checkModuleMember(**resolved, *member);
        }
    }

    void checkModuleMember(const Symbol& owner, const SyntaxToken& member)
    {
        if (owner.kind != SymbolKind::SourceModule || owner.target == nullptr)
        {
            return;
        }
        const ModuleSymbols& target = symbolsOf(*owner.target);
        const auto found = target.names.find(nameKey(member.text));
        const uint32_t length = static_cast<uint32_t>(member.text.size());
        if (found == target.names.end())
        {
            report(201, member.offset, length,
                say("в модуле `", "module `") + owner.importPath + say("` нет имени `", "` has no name `")
                    + identifierText(member) + "`");
        }
        else
        {
            noteSpelling(member, found->second.display);
        }
        if (found != target.names.end() && !found->second.open && owner.target != &module)
        {
            report(205, member.offset, length,
                say("имя `", "name `") + found->second.display + say("` модуля `", "` of module `") + owner.importPath
                    + say("` закрыто: оно объявлено без `откр`", "` is private: it is declared without `откр`"));
        }
    }

    void reportShadowedModule(const SyntaxToken& receiver, const Local& local, const Symbol& shadowed)
    {
        const bool parameter = local.kind != LocalKind::Binding;
        const std::string kind = parameter ? say("параметром", "by parameter") : say("привязкой", "by binding");
        const std::string declared = parameter ? say("параметр объявлен здесь", "parameter declared here")
                                               : say("привязка объявлена здесь", "binding declared here");
        report(210, receiver.offset, static_cast<uint32_t>(receiver.text.size()),
            say("модуль `", "module `") + shadowed.display + say("` затенён ", "` is shadowed ") + kind + " `"
                + local.display + "`",
            {ReportNote{local.offset, local.length,
                declared
                    + say("; модуль подключают под псевдонимом: `подключить ",
                        "; import the module under an alias: `подключить ")
                    + shadowed.importPath + " как " + aliasSuggestion(shadowed.display) + "`"}});
    }

    void visitBinding(const SyntaxNodeView& node)
    {
        visitChildren(node);
        if (const std::optional<SyntaxToken> name = node.token(Identifier))
        {
            declare(*name, LocalKind::Binding, node.hasToken(KwMut));
        }
    }

    void visitAssignment(const SyntaxNodeView& node)
    {
        const std::vector<SyntaxNodeView> children = node.nodes();
        if (children.size() < 2)
        {
            visitChildren(node);
            return;
        }
        const bool plain = node.hasToken(Eq);
        for (size_t index = 1; index < children.size(); ++index)
        {
            visit(children[index]);
        }
        assignTarget(children.front(), plain);
    }

    void assignTarget(const SyntaxNodeView& target, bool plain)
    {
        switch (target.kind())
        {
        case NameExpr:
        {
            const std::optional<SyntaxToken> name = target.token(Identifier);
            if (!name)
            {
                return;
            }
            if (Local* local = findLocal(nameKey(name->text), false))
            {
                local->used = true;
                noteSpelling(*name, local->display);
                if (!local->mutableBinding)
                {
                    const bool parameter = local->kind != LocalKind::Binding;
                    report(204, name->offset, static_cast<uint32_t>(name->text.size()),
                        (parameter ? say("параметр `", "parameter `") : say("привязка `", "binding `")) + local->display
                            + say("` неизменяем", "` is immutable") + (parameter ? say("", "") : say("а", ""))
                            + say(
                                ": присваивают только привязке с `изм`", ": only a binding with `изм` can be assigned"),
                        {ReportNote{local->offset, local->length,
                            parameter ? say("объявлен здесь без `изм`", "declared here without `изм`")
                                      : say("объявлена здесь без `изм`", "declared here without `изм`")}});
                }
                return;
            }
            if (plain)
            {
                declare(*name, LocalKind::Binding, false);
            }
            else
            {
                resolveName(*name);
            }
            return;
        }
        case TupleExpr:
        case ParenExpr:
            for (const SyntaxNodeView& element : target.nodes())
            {
                if (element.kind() != Placeholder)
                {
                    assignTarget(element, plain);
                }
            }
            return;
        case CallExpr:
            for (const SyntaxNodeView& part : target.nodes())
            {
                if (part.kind() != ArgList)
                {
                    visit(part);
                    continue;
                }
                for (const SyntaxNodeView& argument : part.nodes())
                {
                    if (argument.kind() != Placeholder)
                    {
                        assignTarget(argument, plain);
                    }
                }
            }
            return;
        case CompoundLiteral:
        {
            const std::vector<SyntaxNodeView> parts = target.nodes();
            for (const SyntaxNodeView& part : parts)
            {
                if (part.kind() != LiteralElement)
                {
                    visit(part);
                    continue;
                }
                const std::vector<SyntaxNodeView> values = part.nodes();
                if (values.empty())
                {
                    continue;
                }
                if (values.front().kind() == NameExpr || values.front().kind() == TupleExpr
                    || values.front().kind() == CompoundLiteral)
                {
                    assignTarget(values.front(), plain);
                }
                else if (values.front().kind() != Placeholder)
                {
                    visit(values.front());
                }
            }
            return;
        }
        default:
            visit(target);
            return;
        }
    }

    bool referencesLocals(const SyntaxNodeView& node) const
    {
        if (node.kind() == NameExpr)
        {
            const std::optional<SyntaxToken> name = node.token(Identifier);
            return node.hasToken(KwSelf) || (name && findLocal(nameKey(name->text), false) != nullptr);
        }
        for (const SyntaxNodeView& child : node.nodes())
        {
            if (referencesLocals(child))
            {
                return true;
            }
        }
        return false;
    }

    std::optional<bool> comptimeDecision(const SyntaxNodeView& node)
    {
        const SyntaxNodeView condition = node.child(0);
        if (!node.hasToken(KwComptime) || !evaluators || !condition || referencesLocals(condition))
        {
            return std::nullopt;
        }
        if (!evaluator)
        {
            comptimeDeclarations = std::make_unique<Declarations>(program, module);
            evaluator = evaluators(program, module, *comptimeDeclarations);
        }
        return evaluator ? evaluator->evaluateCondition(condition, module) : std::nullopt;
    }

    void visitIf(const SyntaxNodeView& node)
    {
        if (const std::optional<bool> decided = comptimeDecision(node))
        {
            visit(node.child(0));
            if (const SyntaxNodeView chosen = *decided ? node.child(1) : node.child(2))
            {
                visit(chosen);
            }
            return;
        }
        const std::vector<SyntaxNodeView> children = node.nodes();
        size_t index = 0;
        withScope([&]
        {
            for (; index < children.size(); ++index)
            {
                visit(children[index]);
                if (children[index].kind() == Block)
                {
                    ++index;
                    break;
                }
            }
        });
        for (; index < children.size(); ++index)
        {
            visit(children[index]);
        }
    }

    void visitIs(const SyntaxNodeView& node)
    {
        const std::vector<SyntaxNodeView> children = node.nodes();
        if (children.empty())
        {
            return;
        }
        visit(children.front());
        const bool negated = node.hasToken(KwNot);
        for (size_t index = 1; index < children.size(); ++index)
        {
            pattern(children[index], false, !negated);
        }
    }

    void visitMatch(const SyntaxNodeView& node)
    {
        for (const SyntaxNodeView& child : node.nodes())
        {
            if (child.kind() != MatchArm)
            {
                visit(child);
                continue;
            }
            withScope([&]
            {
                const std::vector<SyntaxNodeView> parts = child.nodes();
                for (size_t index = 0; index < parts.size(); ++index)
                {
                    if (index == 0)
                    {
                        pattern(parts[index], false, true);
                    }
                    else
                    {
                        visit(parts[index]);
                    }
                }
            });
        }
    }

    void visitLoop(const SyntaxNodeView& node)
    {
        std::optional<SyntaxToken> label;
        bool beforeLoop = true;
        bool iteration = node.hasToken(KwIn);
        bool afterIn = false;
        SyntaxNodeView loopPattern;
        std::vector<SyntaxNodeView> rest;
        node.forEachChild([&](const SyntaxNodeView& child, const std::optional<SyntaxToken>& token)
        {
            if (token)
            {
                if (token->kind == KwLoop)
                {
                    beforeLoop = false;
                }
                else if (token->kind == Identifier && beforeLoop)
                {
                    label = token;
                }
                else if (token->kind == KwIn)
                {
                    afterIn = true;
                }
                return;
            }
            if (iteration && !afterIn)
            {
                loopPattern = child;
            }
            else
            {
                rest.push_back(child);
            }
        });
        for (const SyntaxNodeView& child : rest)
        {
            if (iteration && child.kind() != Block)
            {
                visit(child);
            }
        }
        withScope([&]
        {
            if (loopPattern)
            {
                pattern(loopPattern, node.hasToken(KwMut), true);
            }
            if (label)
            {
                labels.push_back(nameKey(label->text));
            }
            for (const SyntaxNodeView& child : rest)
            {
                if (!iteration || child.kind() == Block)
                {
                    visit(child);
                }
            }
            if (label)
            {
                labels.pop_back();
            }
        });
    }

    void checkLabel(const SyntaxNodeView& node)
    {
        visitChildren(node);
        const std::optional<SyntaxToken> label = node.token(Identifier);
        if (!label || std::find(labels.begin(), labels.end(), nameKey(label->text)) != labels.end())
        {
            return;
        }
        report(211, label->offset, static_cast<uint32_t>(label->text.size()),
            say("метка `", "label `") + identifierText(*label)
                + say("` не найдена: ею не помечен ни один объемлющий цикл", "` not found: no enclosing loop has it"));
    }

    void visitClosure(const SyntaxNodeView& node)
    {
        const bool hasBody = node.hasToken(FatArrow) || node.node(Block);
        std::vector<std::string> savedLabels = std::move(labels);
        labels.clear();
        withScope([&]
        {
            for (const SyntaxNodeView& child : node.nodes())
            {
                if (child.kind() != FunctionParam)
                {
                    visit(child);
                    continue;
                }
                for (const SyntaxNodeView& part : child.nodes())
                {
                    visit(part);
                }
                const std::optional<SyntaxToken> name = child.token(Identifier);
                if (hasBody && name && child.hasToken(Colon))
                {
                    declare(*name, LocalKind::Parameter, child.hasToken(KwMut));
                }
            }
        });
        labels = std::move(savedLabels);
    }

    void pattern(const SyntaxNodeView& node, bool mutableBinding, bool binds)
    {
        switch (node.kind())
        {
        case BindingPattern:
            if (const std::optional<SyntaxToken> name = node.token(Identifier); name && binds)
            {
                declare(*name, LocalKind::Binding, true);
            }
            return;
        case PathPattern:
        {
            const std::vector<SyntaxToken> tokens = node.tokens();
            std::vector<SyntaxToken> path;
            for (const SyntaxToken& token : tokens)
            {
                if (token.kind == Identifier)
                {
                    path.push_back(token);
                }
            }
            const std::vector<SyntaxNodeView> parts = node.nodes();
            if (path.size() == 1 && parts.empty() && !startsUppercase(path.front().text))
            {
                if (binds)
                {
                    declare(path.front(), LocalKind::Binding, mutableBinding);
                }
                return;
            }
            if (!path.empty())
            {
                resolvePatternPath(path);
            }
            for (const SyntaxNodeView& part : parts)
            {
                patternArguments(part, mutableBinding, binds);
            }
            return;
        }
        case TuplePattern:
            for (const SyntaxNodeView& element : node.nodes())
            {
                pattern(element, mutableBinding, binds);
            }
            return;
        case OrPattern:
        {
            bool first = true;
            for (const SyntaxNodeView& alternative : node.nodes())
            {
                pattern(alternative, mutableBinding, binds && first);
                first = false;
            }
            return;
        }
        default:
            visitChildren(node);
            return;
        }
    }

    void resolvePatternPath(const std::vector<SyntaxToken>& path)
    {
        const std::optional<const Symbol*> resolved = resolveName(path.front());
        if (path.size() > 1 && resolved && *resolved != nullptr)
        {
            checkModuleMember(**resolved, path[1]);
        }
    }

    void patternArguments(const SyntaxNodeView& node, bool mutableBinding, bool binds)
    {
        for (const SyntaxNodeView& argument : node.nodes())
        {
            if (argument.kind() == PatternField)
            {
                const std::vector<SyntaxNodeView> inner = argument.nodes();
                if (inner.empty())
                {
                    if (const std::optional<SyntaxToken> name = argument.token(Identifier); name && binds)
                    {
                        declare(*name, LocalKind::Binding, mutableBinding);
                    }
                }
                for (const SyntaxNodeView& child : inner)
                {
                    pattern(child, mutableBinding, binds);
                }
                continue;
            }
            if (argument.kind() == PatternArg)
            {
                for (const SyntaxNodeView& child : argument.nodes())
                {
                    pattern(child, mutableBinding, binds);
                }
                continue;
            }
            pattern(argument, mutableBinding, binds);
        }
    }

    void checkConstCycles()
    {
        std::map<std::string, int> state;
        std::vector<std::string> stack;
        std::set<std::set<std::string>> reported;
        std::function<void(const std::string&)> walk = [&](const std::string& current)
        {
            state[current] = 1;
            stack.push_back(current);
            for (const ConstEdge& edge : constEdges[current])
            {
                if (state[edge.target] == 0)
                {
                    walk(edge.target);
                }
                else if (state[edge.target] == 1)
                {
                    const auto start = std::find(stack.begin(), stack.end(), edge.target);
                    std::vector<std::string> cycle(start, stack.end());
                    if (reported.insert(std::set<std::string>(cycle.begin(), cycle.end())).second)
                    {
                        reportCycle(cycle);
                    }
                }
            }
            stack.pop_back();
            state[current] = 2;
        };
        for (const std::string& name : constOrder)
        {
            if (state[name] == 0)
            {
                walk(name);
            }
        }
    }

    void reportCycle(const std::vector<std::string>& cycle)
    {
        std::string path;
        std::vector<ReportNote> notes;
        for (size_t index = 0; index < cycle.size(); ++index)
        {
            const std::string& from = cycle[index];
            const std::string& to = cycle[(index + 1) % cycle.size()];
            const std::string fromName = identifierText(constNames[from]);
            const std::string toName = identifierText(constNames[to]);
            path += "`" + fromName + "` → ";
            for (const ConstEdge& edge : constEdges[from])
            {
                if (edge.target == to)
                {
                    notes.push_back(ReportNote{edge.offset, edge.length,
                        "`" + fromName + say("` вычисляется через `", "` is computed from `") + toName + "`"});
                    break;
                }
            }
        }
        const SyntaxToken& first = constNames[cycle.front()];
        path += "`" + identifierText(first) + "`";
        report(207, first.offset, static_cast<uint32_t>(first.text.size()),
            say("цикл между константами: ", "cycle between constants: ") + path, std::move(notes));
    }

    Program& program;
    SourceModule& module;
    Language language;
    std::vector<Report> reports;
    std::map<const SourceModule*, ModuleSymbols> cache;
    const ModuleSymbols* preludeSymbols = nullptr;
    std::set<std::string> variants;
    std::set<std::string> headerNames;
    std::deque<Local> storage;
    std::vector<std::vector<Local*>> scopes;
    std::vector<std::string> labels;
    bool typeOnly = false;
    std::string currentConst;
    std::vector<std::string> constOrder;
    std::map<std::string, SyntaxToken> constNames;
    std::map<std::string, std::vector<ConstEdge>> constEdges;
    std::vector<Respelling> respellings;
    EvaluatorFactory evaluators;
    std::unique_ptr<Declarations> comptimeDeclarations;
    std::unique_ptr<ComptimeEvaluator> evaluator;
};

}

std::vector<Report> resolveNames(
    Program& program, SourceModule& module, Language language, const EvaluatorFactory& evaluators)
{
    if (module.kind != ModuleKind::Source || !module.root || !module.syntax.empty())
    {
        return {};
    }
    Resolver resolver(program, module, language);
    resolver.attach(evaluators);
    return resolver.run();
}

std::vector<Respelling> canonicalSpellings(Program& program, SourceModule& module)
{
    if (module.kind != ModuleKind::Source || !module.root || !module.syntax.empty())
    {
        return {};
    }
    return Resolver(program, module, Language::Russian).spellings();
}

}

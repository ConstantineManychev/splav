#include "format/formatter.hpp"

#include <algorithm>
#include <optional>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace splav
{

namespace
{

using enum SyntaxKind;

size_t displayWidth(std::string_view text)
{
    size_t width = 0;
    for (const char symbol : text)
    {
        width += (static_cast<unsigned char>(symbol) & 0xC0) != 0x80 ? 1 : 0;
    }
    return width;
}

bool isOneOf(SyntaxKind kind, std::initializer_list<SyntaxKind> kinds)
{
    return std::find(kinds.begin(), kinds.end(), kind) != kinds.end();
}

bool isComment(SyntaxKind kind)
{
    return isOneOf(kind, {LineComment, DocComment, ModuleDocComment, BlockComment});
}

class Printer
{
public:
    struct Comment
    {
        std::string text;
        int newlinesBefore;
        bool endsLine;
    };

    struct Snapshot
    {
        size_t lineCount;
        std::string current;
        int currentIndent;
        int indent;
        int pending;
        int sourceNewlines;
        std::vector<Comment> comments;
        bool lineHasContent;
        bool anyOutput;
        bool allowBlank;
        bool forceBlank;
        bool breakRequired;
        bool commentEndedLine;
        bool continuation;
    };

    int indent = 0;

    void space()
    {
        pending = std::max(pending, SPACE);
    }

    void line(bool keepBlank)
    {
        pending = LINE;
        allowBlank = keepBlank;
        forceBlank = false;
        continuation = false;
    }

    void blankLine()
    {
        pending = LINE;
        allowBlank = true;
        forceBlank = true;
        continuation = false;
    }

    void continuationLine()
    {
        pending = LINE;
        allowBlank = false;
        forceBlank = false;
        continuation = true;
    }

    void sourceNewline()
    {
        ++sourceNewlines;
    }

    void comment(std::string_view text, bool endsLine)
    {
        comments.push_back(Comment{std::string(text), sourceNewlines, endsLine});
        sourceNewlines = 0;
    }

    void write(std::string_view text)
    {
        emitComments();
        if (anyOutput)
        {
            if (breakRequired || pending == LINE)
            {
                const bool keepBlank = allowBlank || (breakRequired && commentEndedLine);
                startLine(forceBlank || (keepBlank && sourceNewlines >= 2), continuation ? indent + 1 : indent);
            }
            else if (pending == SPACE)
            {
                current += ' ';
            }
        }
        current += text;
        settle();
    }

    void rawString(std::string_view text)
    {
        emitComments();
        if (lineHasContent)
        {
            startLine(false, indent + 1);
        }
        else if (anyOutput && (breakRequired || pending == LINE))
        {
            startLine(forceBlank || (allowBlank && sourceNewlines >= 2), indent);
        }
        size_t lineStart = 0;
        bool first = true;
        while (lineStart <= text.size())
        {
            size_t lineEnd = text.find('\n', lineStart);
            lineEnd = lineEnd == std::string_view::npos ? text.size() : lineEnd;
            std::string_view rawLine = text.substr(lineStart, lineEnd - lineStart);
            rawLine.remove_prefix(std::min(rawLine.find_first_not_of(" \t"), rawLine.size()));
            if (!first)
            {
                startLine(false, currentIndent);
            }
            current += rawLine;
            first = false;
            lineStart = lineEnd + 1;
        }
        settle();
        breakRequired = true;
    }

    void pad(size_t count)
    {
        current.append(count, ' ');
    }

    int lineIndent() const
    {
        return currentIndent;
    }

    size_t lineNumber() const
    {
        return lines.size();
    }

    size_t column() const
    {
        return displayWidth(current);
    }

    bool hasPendingComments() const
    {
        return !comments.empty();
    }

    size_t widthOfLine(size_t index) const
    {
        const std::string& text = index < lines.size() ? lines[index] : current;
        size_t widest = 0;
        size_t lineStart = 0;
        while (lineStart <= text.size())
        {
            size_t lineEnd = text.find('\n', lineStart);
            lineEnd = lineEnd == std::string::npos ? text.size() : lineEnd;
            widest = std::max(widest, displayWidth(std::string_view(text).substr(lineStart, lineEnd - lineStart)));
            lineStart = lineEnd + 1;
        }
        return widest;
    }

    Snapshot snapshot() const
    {
        return Snapshot{lines.size(), current, currentIndent, indent, pending, sourceNewlines, comments, lineHasContent,
            anyOutput, allowBlank, forceBlank, breakRequired, commentEndedLine, continuation};
    }

    void restore(const Snapshot& saved)
    {
        lines.resize(saved.lineCount);
        current = saved.current;
        currentIndent = saved.currentIndent;
        indent = saved.indent;
        pending = saved.pending;
        sourceNewlines = saved.sourceNewlines;
        comments = saved.comments;
        lineHasContent = saved.lineHasContent;
        anyOutput = saved.anyOutput;
        allowBlank = saved.allowBlank;
        forceBlank = saved.forceBlank;
        breakRequired = saved.breakRequired;
        commentEndedLine = saved.commentEndedLine;
        continuation = saved.continuation;
    }

    std::string finish()
    {
        emitComments();
        if (!anyOutput)
        {
            return {};
        }
        std::string output;
        for (const std::string& text : lines)
        {
            output += text;
            output += '\n';
        }
        output += current;
        output += '\n';
        return output;
    }

private:
    static constexpr int NONE = 0;
    static constexpr int SPACE = 1;
    static constexpr int LINE = 2;

    std::vector<std::string> lines;
    std::string current;
    std::vector<Comment> comments;
    int currentIndent = 0;
    int pending = NONE;
    int sourceNewlines = 0;
    bool lineHasContent = false;
    bool anyOutput = false;
    bool allowBlank = false;
    bool forceBlank = false;
    bool breakRequired = false;
    bool commentEndedLine = false;
    bool continuation = false;

    void startLine(bool blank, int level)
    {
        lines.push_back(std::move(current));
        if (blank)
        {
            lines.emplace_back();
        }
        current.assign(static_cast<size_t>(level) * FORMAT_INDENT_WIDTH, ' ');
        currentIndent = level;
        lineHasContent = false;
    }

    void settle()
    {
        lineHasContent = true;
        anyOutput = true;
        pending = NONE;
        sourceNewlines = 0;
        allowBlank = false;
        forceBlank = false;
        breakRequired = false;
        commentEndedLine = false;
        continuation = false;
    }

    void emitComments()
    {
        bool afterComment = false;
        for (const Comment& entry : comments)
        {
            if (entry.newlinesBefore == 0 && lineHasContent && !breakRequired)
            {
                current += ' ';
            }
            else if (anyOutput)
            {
                startLine(forceBlank || ((allowBlank || afterComment) && entry.newlinesBefore >= 2), indent);
                forceBlank = false;
            }
            afterComment = true;
            current += entry.text;
            lineHasContent = true;
            anyOutput = true;
            if (entry.endsLine)
            {
                breakRequired = true;
                commentEndedLine = true;
            }
            else
            {
                pending = std::max(pending, SPACE);
            }
        }
        comments.clear();
    }
};

class IndentScope
{
public:
    IndentScope(Printer*& target, int level) : printer(target), saved(target->indent)
    {
        printer->indent = level;
    }

    ~IndentScope()
    {
        printer->indent = saved;
    }

    IndentScope(const IndentScope&) = delete;
    IndentScope& operator=(const IndentScope&) = delete;

private:
    Printer*& printer;
    int saved;
};

class NestingScope
{
public:
    explicit NestingScope(int& counter) : nesting(counter)
    {
        ++nesting;
    }

    ~NestingScope()
    {
        --nesting;
    }

    NestingScope(const NestingScope&) = delete;
    NestingScope& operator=(const NestingScope&) = delete;

private:
    int& nesting;
};

class Formatter
{
public:
    std::string run(const GreenNode& root)
    {
        if (root.kind() == Manifest)
        {
            Cursor cursor(*this, root);
            while (!cursor.done())
            {
                formatTopLevel(cursor.take());
            }
            cursor.finish();
        }
        else
        {
            formatModule(root);
        }
        return main.finish();
    }

private:
    class Cursor
    {
    public:
        Cursor(Formatter& owner, const GreenNode& node) : formatter(owner), children(node.children())
        {
            peekNext();
        }

        bool done() const
        {
            return next >= children.size();
        }

        SyntaxKind kind() const
        {
            return done() ? EndOfFile : children[next].kind();
        }

        bool at(SyntaxKind expected) const
        {
            return kind() == expected;
        }

        const GreenElement& peek() const
        {
            return children[next];
        }

        int newlinesBefore() const
        {
            return newlines;
        }

        bool newlineBefore(SyntaxKind closer) const
        {
            for (size_t index = fed; index < children.size() && children[index].kind() != closer; ++index)
            {
                if (children[index].kind() == Newline)
                {
                    return true;
                }
            }
            return false;
        }

        const GreenElement& take()
        {
            feedTrivia(next);
            const GreenElement& element = children[next];
            fed = next + 1;
            peekNext();
            return element;
        }

        void finish()
        {
            feedTrivia(children.size());
            fed = children.size();
            next = children.size();
        }

    private:
        Formatter& formatter;
        const std::vector<GreenElement>& children;
        size_t fed = 0;
        size_t next = 0;
        int newlines = 0;

        void peekNext()
        {
            next = fed;
            newlines = 0;
            while (next < children.size() && isTrivia(children[next].kind()))
            {
                newlines += children[next].kind() == Newline ? 1 : 0;
                ++next;
            }
        }

        void feedTrivia(size_t end)
        {
            for (size_t index = fed; index < end; ++index)
            {
                const SyntaxKind triviaKind = children[index].kind();
                if (triviaKind == Newline)
                {
                    formatter.out->sourceNewline();
                }
                else if (isComment(triviaKind))
                {
                    formatter.out->comment(children[index].token()->text, triviaKind != BlockComment);
                }
            }
        }
    };

    struct ListRecord
    {
        const GreenNode* node;
        size_t line;
        int nesting;
        bool flat;
    };

    Printer main;
    Printer* out = &main;
    std::unordered_set<const GreenNode*> expanded;
    std::vector<ListRecord> lists;
    int listNesting = 0;

    void token(Cursor& cursor)
    {
        const GreenElement& element = cursor.take();
        writeToken(*element.token());
    }

    void writeToken(const GreenToken& leaf)
    {
        if (leaf.kind == RawString)
        {
            out->rawString(leaf.text);
        }
        else
        {
            out->write(leaf.text);
        }
    }

    bool tokenIf(Cursor& cursor, SyntaxKind expected)
    {
        if (!cursor.at(expected))
        {
            return false;
        }
        token(cursor);
        return true;
    }

    void element(const GreenElement& child)
    {
        if (const GreenToken* leaf = child.token())
        {
            writeToken(*leaf);
        }
        else
        {
            formatNode(*child.node());
        }
    }

    void next(Cursor& cursor)
    {
        element(cursor.take());
    }

    static bool isBlockItem(const GreenElement& item)
    {
        const GreenNode* node = item.node();
        if (node == nullptr)
        {
            return false;
        }
        if (isOneOf(node->kind(), {StructDecl, EnumDecl, TraitDecl}))
        {
            return true;
        }
        if (node->kind() != FunctionDecl && node->kind() != ImplDecl)
        {
            return false;
        }
        return std::any_of(node->children().begin(), node->children().end(),
            [](const GreenElement& child) { return child.kind() == Block || child.kind() == MethodList; });
    }

    void formatModule(const GreenNode& node)
    {
        Cursor cursor(*this, node);
        const GreenElement* previous = nullptr;
        while (!cursor.done())
        {
            const GreenElement& upcoming = cursor.peek();
            if (previous != nullptr)
            {
                if (isBlockItem(*previous) || isBlockItem(upcoming))
                {
                    out->blankLine();
                }
                else
                {
                    out->line(true);
                }
            }
            formatTopLevel(cursor.take());
            previous = &upcoming;
        }
        cursor.finish();
    }

    void formatTopLevel(const GreenElement& item)
    {
        for (;;)
        {
            const Printer::Snapshot saved = out->snapshot();
            lists.clear();
            listNesting = 0;
            element(item);
            const GreenNode* candidate = overflowCandidate(saved.lineCount);
            if (candidate == nullptr)
            {
                return;
            }
            expanded.insert(candidate);
            out->restore(saved);
        }
    }

    const GreenNode* overflowCandidate(size_t firstLine) const
    {
        for (size_t line = firstLine; line <= out->lineNumber(); ++line)
        {
            if (out->widthOfLine(line) <= FORMAT_LINE_WIDTH)
            {
                continue;
            }
            const ListRecord* chosen = nullptr;
            for (const ListRecord& record : lists)
            {
                if (!record.flat || record.line != line)
                {
                    continue;
                }
                const bool shallower = chosen == nullptr || record.nesting < chosen->nesting;
                const bool parameters = chosen != nullptr && record.nesting == chosen->nesting
                    && record.node->kind() == ParamList && chosen->node->kind() != ParamList;
                if (shallower || parameters)
                {
                    chosen = &record;
                }
            }
            if (chosen != nullptr)
            {
                return chosen->node;
            }
        }
        return nullptr;
    }

    void formatNode(const GreenNode& node)
    {
        switch (node.kind())
        {
        case Import:
        case CInclude:
        case ConstDecl:
        case StructDecl:
        case EnumDecl:
        case TraitDecl:
        case ImplDecl:
        case FunctionDecl:
            formatDeclaration(node);
            break;
        case Signature:
            formatSignature(node);
            break;
        case ComptimeParamList:
        case ParamList:
        case ArgList:
        case TupleExpr:
        case PatternArgList:
            formatBracketNode(node);
            break;
        case TuplePattern:
            if (isGrouping(node))
            {
                formatTight(node);
            }
            else
            {
                formatBracketNode(node);
            }
            break;
        case PatternFieldList:
            formatBraceNode(node);
            break;
        case Param:
        case FunctionParam:
        case AttributeArgument:
        case LiteralElement:
        case VariantField:
        case PatternArg:
        case PatternField:
        case Field:
            formatLabelled(node);
            break;
        case Attribute:
        case Variant:
            formatWithList(node);
            break;
        case FieldList:
        case VariantList:
        case SignatureList:
        case MethodList:
            formatBody(node);
            break;
        case Block:
            formatBlock(node, false);
            break;
        case BindingStmt:
        case AssignStmt:
        case ExprStmt:
        case ReturnStmt:
        case BreakStmt:
        case ContinueStmt:
        case SpawnStmt:
        case DiscardStmt:
        case DeferStmt:
        case LogicalExpr:
        case ComparisonExpr:
        case MembershipExpr:
        case IsExpr:
        case BinaryExpr:
        case CastExpr:
        case NotExpr:
        case OrPattern:
        case BindingPattern:
        case DynType:
            formatSpaced(node);
            break;
        case IfStmt:
            formatIf(node);
            break;
        case LoopStmt:
        case RegionStmt:
        case ThroughStmt:
        case TogetherStmt:
        case UnsafeStmt:
            formatHeaderWithBlock(node);
            break;
        case PrefixExpr:
            formatPrefix(node);
            break;
        case RefExpr:
            formatReference(node);
            break;
        case SliceType:
        case ArrayType:
        case SentinelType:
            formatBracketType(node);
            break;
        case CompoundLiteral:
            formatCompoundLiteral(node);
            break;
        case MatchExpr:
            formatMatch(node);
            break;
        case FunctionExpr:
            formatFunctionExpression(node);
            break;
        case AsmExpr:
        case AsmOperand:
        case TogetherExpr:
            formatWithList(node);
            break;
        case StructType:
        case EnumType:
            formatTight(node);
            break;
        case PathPattern:
            formatTight(node);
            break;
        default:
            formatTight(node);
        }
    }

    void formatTight(const GreenNode& node)
    {
        Cursor cursor(*this, node);
        while (!cursor.done())
        {
            next(cursor);
        }
    }

    void formatSpaced(const GreenNode& node)
    {
        Cursor cursor(*this, node);
        bool afterOperator = false;
        bool first = true;
        while (!cursor.done())
        {
            if (!first && !cursor.at(Colon))
            {
                if (afterOperator && cursor.newlinesBefore() > 0)
                {
                    out->continuationLine();
                }
                else
                {
                    out->space();
                }
            }
            afterOperator = cursor.peek().token() != nullptr;
            first = false;
            next(cursor);
        }
    }

    void formatAttributes(Cursor& cursor)
    {
        while (cursor.at(Attribute))
        {
            next(cursor);
            out->line(false);
        }
    }

    void formatDeclaration(const GreenNode& node)
    {
        Cursor cursor(*this, node);
        formatAttributes(cursor);
        bool first = true;
        SyntaxKind previous = EndOfFile;
        while (!cursor.done())
        {
            const SyntaxKind current = cursor.kind();
            const bool tight = first || current == Dot || previous == Dot || current == Colon
                || isOneOf(current, {ComptimeParamList, FieldList, VariantList, SignatureList, MethodList, Block});
            if (!tight)
            {
                out->space();
            }
            if (current == Block)
            {
                formatBlock(*cursor.take().node(), false);
            }
            else
            {
                next(cursor);
            }
            previous = current;
            first = false;
        }
    }

    void formatSignature(const GreenNode& node)
    {
        Cursor cursor(*this, node);
        bool first = true;
        SyntaxKind previous = EndOfFile;
        while (!cursor.done())
        {
            const SyntaxKind current = cursor.kind();
            const bool tight =
                first || current == Dot || previous == Dot || current == ComptimeParamList || current == ParamList;
            if (!tight)
            {
                out->space();
            }
            next(cursor);
            previous = current;
            first = false;
        }
    }

    void formatLabelled(const GreenNode& node)
    {
        Cursor cursor(*this, node);
        formatAttributes(cursor);
        while (!cursor.done())
        {
            const SyntaxKind current = cursor.kind();
            if (current == Eq)
            {
                out->space();
            }
            next(cursor);
            if (isOneOf(current, {KwComptime, KwMut, KwPub, Colon, Eq}))
            {
                out->space();
            }
        }
    }

    void formatWithList(const GreenNode& node)
    {
        Cursor cursor(*this, node);
        while (!cursor.done())
        {
            if (cursor.at(LParen))
            {
                formatList(cursor, &node, RParen, false);
            }
            else
            {
                next(cursor);
            }
        }
    }

    void formatBracketNode(const GreenNode& node)
    {
        Cursor cursor(*this, node);
        formatList(cursor, &node, RParen, false);
    }

    void formatBraceNode(const GreenNode& node)
    {
        Cursor cursor(*this, node);
        formatList(cursor, &node, RBrace, true);
    }

    void formatList(Cursor& cursor, const GreenNode* identity, SyntaxKind closer, bool padded)
    {
        const bool forced = expanded.contains(identity);
        token(cursor);
        const bool broken = !forced && cursor.newlineBefore(closer);
        const int base = out->lineIndent();
        lists.push_back(ListRecord{identity, out->lineNumber(), listNesting, !broken && !forced});
        const NestingScope nesting(listNesting);
        if (cursor.at(closer))
        {
            if (cursor.newlinesBefore() > 0 && out->hasPendingComments())
            {
                out->line(false);
            }
            token(cursor);
            return;
        }
        const bool singleTuple =
            isOneOf(identity->kind(), {TupleExpr, TuplePattern}) && hasSingleElementWithComma(*identity);
        bool hugClose = true;
        {
            const IndentScope scope(out, forced || broken ? base + 1 : out->indent);
            const bool wholeLines = closer == RBrace && broken;
            if (forced || wholeLines || (broken && cursor.newlinesBefore() > 0))
            {
                out->line(false);
            }
            else if (padded)
            {
                out->space();
            }
            while (!cursor.done() && !cursor.at(closer))
            {
                next(cursor);
                const bool hadComma = cursor.at(Comma);
                if (hadComma)
                {
                    cursor.take();
                }
                const bool last = cursor.at(closer) || cursor.done();
                if (!last)
                {
                    out->write(",");
                    if (forced || (broken && cursor.newlinesBefore() > 0))
                    {
                        out->line(false);
                    }
                    else
                    {
                        out->space();
                    }
                    continue;
                }
                hugClose = !forced && !wholeLines && !(broken && cursor.newlinesBefore() > 0);
                if (!hugClose || singleTuple)
                {
                    out->write(",");
                }
            }
        }
        if (!hugClose)
        {
            const IndentScope scope(out, base);
            out->line(false);
            token(cursor);
            return;
        }
        if (padded)
        {
            out->space();
        }
        token(cursor);
    }

    static bool isGrouping(const GreenNode& node)
    {
        return std::none_of(node.children().begin(), node.children().end(),
            [](const GreenElement& child) { return child.kind() == Comma; });
    }

    static bool hasSingleElementWithComma(const GreenNode& node)
    {
        size_t elements = 0;
        size_t commas = 0;
        for (const GreenElement& child : node.children())
        {
            if (child.kind() == Comma)
            {
                ++commas;
            }
            else if (!isTrivia(child.kind()) && !isOneOf(child.kind(), {LParen, RParen}))
            {
                ++elements;
            }
        }
        return elements == 1 && commas >= 1;
    }

    void formatBody(const GreenNode& node)
    {
        Cursor cursor(*this, node);
        const int base = out->lineIndent();
        {
            const IndentScope scope(out, base);
            out->line(false);
            token(cursor);
        }
        {
            const IndentScope scope(out, base + 1);
            bool first = true;
            const GreenElement* previous = nullptr;
            while (!cursor.done() && !cursor.at(RBrace))
            {
                const GreenElement& upcoming = cursor.peek();
                if (!first && (isBlockItem(*previous) || isBlockItem(upcoming)))
                {
                    out->blankLine();
                }
                else
                {
                    out->line(!first);
                }
                next(cursor);
                previous = &upcoming;
                first = false;
            }
        }
        const IndentScope scope(out, base);
        out->line(false);
        token(cursor);
    }

    static bool isEmptyBlock(const GreenNode& block)
    {
        return std::none_of(block.children().begin(), block.children().end(),
            [](const GreenElement& child) { return !isOneOf(child.kind(), {LBrace, RBrace, Whitespace, Newline}); });
    }

    void formatBlock(const GreenNode& node, bool inlineWhenEmpty)
    {
        Cursor cursor(*this, node);
        if (inlineWhenEmpty && isEmptyBlock(node))
        {
            out->space();
            token(cursor);
            token(cursor);
            return;
        }
        const int base = out->lineIndent();
        {
            const IndentScope scope(out, base);
            out->line(false);
            token(cursor);
        }
        {
            const IndentScope scope(out, base + 1);
            bool first = true;
            while (!cursor.done() && !cursor.at(RBrace))
            {
                out->line(!first);
                next(cursor);
                first = false;
            }
        }
        const IndentScope scope(out, base);
        out->line(false);
        token(cursor);
    }

    void formatIf(const GreenNode& node)
    {
        Cursor cursor(*this, node);
        if (tokenIf(cursor, KwComptime))
        {
            out->space();
        }
        token(cursor);
        out->space();
        next(cursor);
        formatBlock(*cursor.take().node(), false);
        if (!cursor.at(KwElse))
        {
            return;
        }
        out->line(false);
        token(cursor);
        if (cursor.at(IfStmt))
        {
            out->space();
            formatIf(*cursor.take().node());
        }
        else
        {
            formatBlock(*cursor.take().node(), false);
        }
    }

    void formatHeaderWithBlock(const GreenNode& node)
    {
        Cursor cursor(*this, node);
        bool first = true;
        while (!cursor.done())
        {
            const SyntaxKind current = cursor.kind();
            if (current == Block)
            {
                formatBlock(*cursor.take().node(), false);
                continue;
            }
            if (!first && current != Colon)
            {
                out->space();
            }
            next(cursor);
            first = false;
        }
    }

    void formatPrefix(const GreenNode& node)
    {
        Cursor cursor(*this, node);
        const SyntaxKind sign = cursor.kind();
        token(cursor);
        if (isKeyword(sign))
        {
            out->space();
        }
        next(cursor);
    }

    void formatReference(const GreenNode& node)
    {
        Cursor cursor(*this, node);
        token(cursor);
        if (tokenIf(cursor, Identifier))
        {
            out->space();
        }
        if (tokenIf(cursor, KwMut))
        {
            out->space();
        }
        next(cursor);
    }

    void formatBracketType(const GreenNode& node)
    {
        Cursor cursor(*this, node);
        while (!cursor.done())
        {
            const SyntaxKind current = cursor.kind();
            next(cursor);
            if (current == KwMut)
            {
                out->space();
            }
        }
    }

    void formatCompoundLiteral(const GreenNode& node)
    {
        Cursor cursor(*this, node);
        const bool arrayLike = isOneOf(cursor.kind(), {ArrayType, SliceType, SentinelType});
        next(cursor);
        formatList(cursor, &node, RBrace, !arrayLike);
    }

    void formatFunctionExpression(const GreenNode& node)
    {
        Cursor cursor(*this, node);
        while (!cursor.done())
        {
            const SyntaxKind current = cursor.kind();
            if (current == LParen)
            {
                formatList(cursor, &node, RParen, false);
            }
            else if (current == Block)
            {
                formatBlock(*cursor.take().node(), false);
            }
            else if (current == Arrow || current == FatArrow)
            {
                out->space();
                token(cursor);
                out->space();
            }
            else
            {
                next(cursor);
                if (current == KwExtern || current == String || current == RawString)
                {
                    out->space();
                }
            }
        }
    }

    std::optional<size_t> armHeadWidth(const GreenNode& arm)
    {
        Printer trial;
        Printer* const saved = out;
        const size_t recorded = lists.size();
        out = &trial;
        Cursor cursor(*this, arm);
        formatArmHead(cursor);
        out = saved;
        lists.resize(recorded);
        if (trial.lineNumber() != 0 || trial.hasPendingComments())
        {
            return std::nullopt;
        }
        return trial.column();
    }

    void formatArmHead(Cursor& cursor)
    {
        next(cursor);
        if (cursor.at(KwIf))
        {
            out->space();
            token(cursor);
            out->space();
            next(cursor);
        }
    }

    void formatArm(const GreenNode& arm, size_t alignWidth)
    {
        Cursor cursor(*this, arm);
        const size_t linesBefore = out->lineNumber();
        formatArmHead(cursor);
        const size_t indentColumns = static_cast<size_t>(out->lineIndent()) * FORMAT_INDENT_WIDTH;
        const size_t width = out->column() - std::min(out->column(), indentColumns);
        if (out->lineNumber() == linesBefore + 1 && width < alignWidth)
        {
            out->pad(alignWidth - width);
        }
        out->space();
        token(cursor);
        if (cursor.at(Block))
        {
            formatBlock(*cursor.take().node(), true);
        }
        else
        {
            out->space();
            next(cursor);
        }
    }

    void formatMatch(const GreenNode& node)
    {
        Cursor cursor(*this, node);
        token(cursor);
        out->space();
        next(cursor);
        const int base = out->lineIndent();
        {
            const IndentScope scope(out, base);
            out->line(false);
            token(cursor);
        }
        size_t alignWidth = 0;
        for (const GreenElement& child : node.children())
        {
            if (child.kind() == MatchArm)
            {
                alignWidth = std::max(alignWidth, armHeadWidth(*child.node()).value_or(0));
            }
        }
        {
            const IndentScope scope(out, base + 1);
            bool first = true;
            while (!cursor.done() && !cursor.at(RBrace))
            {
                out->line(!first);
                formatArm(*cursor.take().node(), alignWidth);
                first = false;
            }
        }
        const IndentScope scope(out, base);
        out->line(false);
        token(cursor);
    }
};

}

std::string format(const GreenNode& root)
{
    return Formatter().run(root);
}

}

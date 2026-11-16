#include "syntax/parser.hpp"

#include "syntax/lexer.hpp"

#include <algorithm>
#include <optional>
#include <utility>

namespace splav
{

namespace
{

using enum SyntaxKind;

struct ParserToken
{
    SyntaxKind kind;
    SyntaxKind treeKind;
    uint32_t offset;
    uint32_t length;
};

struct ExpressionFlags
{
    bool noLiteral = false;
    bool noMembership = false;
};

constexpr ExpressionFlags HEADER{true, false};
constexpr ExpressionFlags LOOP_CONDITION{true, true};

bool isOneOf(SyntaxKind kind, std::initializer_list<SyntaxKind> kinds)
{
    return std::find(kinds.begin(), kinds.end(), kind) != kinds.end();
}

bool endsStatement(SyntaxKind kind)
{
    return isLiteralToken(kind)
        || isOneOf(kind,
            {Identifier, KwSelf, KwSelfType, KwTrue, KwFalse, KwReturn, KwBreak, KwContinue, RParen, RBracket, RBrace});
}

bool isOpening(SyntaxKind kind)
{
    return isOneOf(kind, {LParen, LBracket, LBrace});
}

bool isClosing(SyntaxKind kind)
{
    return isOneOf(kind, {RParen, RBracket, RBrace});
}

bool isAssignment(SyntaxKind kind)
{
    return isOneOf(kind,
        {Eq, PlusEq, MinusEq, StarEq, SlashEq, PercentEq, PlusPercentEq, MinusPercentEq, StarPercentEq, ShlEq, ShrEq,
            AmpEq, BitOrEq, BitXorEq});
}

bool isComparison(SyntaxKind kind)
{
    return isOneOf(kind, {EqEq, NotEq, Less, LessEq, Greater, GreaterEq});
}

bool isAdditive(SyntaxKind kind)
{
    return isOneOf(kind, {Plus, Minus, PlusPercent, MinusPercent, PlusQuestion, MinusQuestion, KwBitOr, KwBitXor});
}

bool isMultiplicative(SyntaxKind kind)
{
    return isOneOf(kind, {Star, Slash, Percent, StarPercent, StarQuestion, Shl, Shr, Amp});
}

bool isValuePrefix(SyntaxKind kind)
{
    return isOneOf(kind, {Minus, KwBitNot, KwTry, KwUnsafe, KwComptime, Star, Amp});
}

bool startsOperand(SyntaxKind kind)
{
    return isLiteralToken(kind)
        || isOneOf(kind,
            {Identifier, KwSelf, KwSelfType, KwTrue, KwFalse, LParen, LBracket, Amp, Star, Minus, KwBitNot, KwTry,
                KwUnsafe, KwComptime, Question, KwDyn, KwMatch, KwFn, KwExtern, KwTogether, KwAsm, KwStruct, KwSet,
                KwEnum});
}

bool startsItem(SyntaxKind kind)
{
    return isOneOf(kind,
        {LBracket, KwImport, KwInclude, KwImpl, KwPub, KwConst, KwStruct, KwSet, KwEnum, KwTrait, KwFn, KwExtern});
}

bool followsRegionLabel(SyntaxKind kind)
{
    return isOneOf(kind, {Identifier, KwMut, KwDyn, Question, KwFn, KwExtern, KwSelfType});
}

std::vector<ParserToken> filterTokens(const std::vector<Token>& tokens)
{
    std::vector<ParserToken> result;
    std::vector<SyntaxKind> brackets;
    for (size_t index = 0; index < tokens.size(); ++index)
    {
        const Token& token = tokens[index];
        if (token.kind == Newline)
        {
            if (!brackets.empty() && brackets.back() != LBrace)
            {
                continue;
            }
            if (result.empty() || !endsStatement(result.back().treeKind))
            {
                continue;
            }
            if (result.back().kind == RBrace)
            {
                size_t next = index + 1;
                while (isTrivia(tokens[next].kind))
                {
                    ++next;
                }
                if (tokens[next].kind == KwElse)
                {
                    continue;
                }
            }
            result.push_back(ParserToken{StatementEnd, StatementEnd, token.offset, 0});
            continue;
        }
        if (isTrivia(token.kind))
        {
            continue;
        }
        if (isOpening(token.kind))
        {
            brackets.push_back(token.kind);
        }
        else if (isClosing(token.kind) && !brackets.empty())
        {
            brackets.pop_back();
        }
        result.push_back(
            ParserToken{token.kind == RawString ? String : token.kind, token.kind, token.offset, token.length});
    }
    return result;
}

class Parser
{
public:
    Parser(std::string_view text, std::vector<Token> lexed)
        : source(text), raw(std::move(lexed)), tokens(filterTokens(raw))
    {
    }

    Parse run(ParseMode mode, std::optional<Diagnostic> lexicalError)
    {
        builder.startNode(mode == ParseMode::Module ? Module : mode == ParseMode::Fragment ? Fragment : Manifest);
        switch (mode)
        {
        case ParseMode::Module:
            parseModule();
            break;
        case ParseMode::Fragment:
            parseFragment();
            break;
        case ParseMode::Manifest:
            parseManifest();
            break;
        }
        expectEndOfFile();
        if (failed)
        {
            emitRemainder();
        }
        else
        {
            flushTrivia();
        }
        Parse result;
        result.root = builder.finish();
        if (lexicalError)
        {
            result.diagnostics.push_back(std::move(*lexicalError));
        }
        else if (error)
        {
            result.diagnostics.push_back(std::move(*error));
        }
        return result;
    }

private:
    class DepthScope
    {
    public:
        explicit DepthScope(Parser& owner) : parser(owner), saved(owner.depth)
        {
        }

        ~DepthScope()
        {
            parser.depth = saved;
        }

        DepthScope(const DepthScope&) = delete;
        DepthScope& operator=(const DepthScope&) = delete;

    private:
        Parser& parser;
        int saved;
    };

    std::string_view source;
    std::vector<Token> raw;
    std::vector<ParserToken> tokens;
    GreenBuilder builder;
    std::optional<Diagnostic> error;
    size_t position = 0;
    size_t rawCursor = 0;
    uint32_t emitted = 0;
    int depth = 0;
    bool failed = false;
    bool inType = false;

    SyntaxKind kind(size_t ahead = 0) const
    {
        if (failed)
        {
            return EndOfFile;
        }
        return tokens[std::min(position + ahead, tokens.size() - 1)].kind;
    }

    bool at(SyntaxKind expected) const
    {
        return kind() == expected;
    }

    bool until(SyntaxKind closing) const
    {
        return !failed && kind() != closing;
    }

    void fail(Message message, SyntaxKind token = Error)
    {
        if (failed)
        {
            return;
        }
        const ParserToken& current = tokens[position];
        Diagnostic diagnostic;
        diagnostic.code = codeOf(message);
        diagnostic.message = message;
        diagnostic.token = token;
        diagnostic.offset = current.offset;
        if (current.kind == StatementEnd)
        {
            diagnostic.ending = Ending::Line;
        }
        else if (current.kind == EndOfFile)
        {
            diagnostic.ending = Ending::File;
        }
        else
        {
            diagnostic.anchorLength = current.length;
        }
        error = std::move(diagnostic);
        failed = true;
    }

    void deepen()
    {
        if (!failed && ++depth > MAX_NESTING)
        {
            fail(Message::NestingTooDeep);
        }
    }

    void flushTrivia()
    {
        while (rawCursor < raw.size() && isTrivia(raw[rawCursor].kind))
        {
            const Token& trivia = raw[rawCursor];
            builder.token(trivia.kind, source.substr(trivia.offset, trivia.length));
            emitted = trivia.offset + trivia.length;
            ++rawCursor;
        }
    }

    void emitRemainder()
    {
        builder.startNode(Error);
        for (; rawCursor < raw.size(); ++rawCursor)
        {
            const Token& token = raw[rawCursor];
            const uint32_t begin = std::max(token.offset, emitted);
            const uint32_t end = token.offset + token.length;
            if (begin < end)
            {
                builder.token(begin == token.offset ? token.kind : Error, source.substr(begin, end - begin));
            }
        }
        builder.finishNode();
    }

    void start(SyntaxKind node)
    {
        if (!failed)
        {
            flushTrivia();
        }
        builder.startNode(node);
    }

    size_t checkpoint()
    {
        if (!failed)
        {
            flushTrivia();
        }
        return builder.checkpoint();
    }

    void startAt(size_t mark, SyntaxKind node)
    {
        builder.startNodeAt(mark, node);
    }

    void finish()
    {
        builder.finishNode();
    }

    void bump()
    {
        if (failed)
        {
            return;
        }
        const ParserToken& token = tokens[position];
        flushTrivia();
        builder.token(token.treeKind, source.substr(token.offset, token.length));
        emitted = token.offset + token.length;
        if (raw[rawCursor].offset + raw[rawCursor].length <= emitted)
        {
            ++rawCursor;
        }
        ++position;
    }

    bool accept(SyntaxKind expected)
    {
        if (!at(expected))
        {
            return false;
        }
        bump();
        return true;
    }

    bool expect(SyntaxKind expected)
    {
        if (accept(expected))
        {
            return true;
        }
        if (expected == Identifier && isKeyword(kind()))
        {
            fail(Message::KeywordAsName, kind());
        }
        else
        {
            fail(Message::ExpectedToken, expected);
        }
        return false;
    }

    void expectEndOfFile()
    {
        if (!failed && !at(EndOfFile))
        {
            fail(Message::ExpectedToken, EndOfFile);
        }
    }

    void skipEnds()
    {
        while (kind() == StatementEnd)
        {
            ++position;
        }
    }

    bool blockAhead()
    {
        if (failed)
        {
            return false;
        }
        size_t probe = position;
        while (tokens[probe].kind == StatementEnd)
        {
            ++probe;
        }
        if (tokens[probe].kind != LBrace)
        {
            return false;
        }
        position = probe;
        return true;
    }

    void endStatement()
    {
        if (at(StatementEnd))
        {
            skipEnds();
        }
        else if (!at(RBrace) && !at(EndOfFile))
        {
            fail(Message::ExpectedNewline);
        }
    }

    void splitStarQuestion()
    {
        if (failed || tokens[position].kind != StarQuestion)
        {
            return;
        }
        const ParserToken whole = tokens[position];
        tokens[position] = ParserToken{Star, Star, whole.offset, 1};
        tokens.insert(tokens.begin() + static_cast<std::ptrdiff_t>(position) + 1,
            ParserToken{Question, Question, whole.offset + 1, 1});
    }

    void parseModule()
    {
        skipEnds();
        while (!at(EndOfFile))
        {
            parseItem();
            skipEnds();
        }
    }

    void parseFragment()
    {
        skipEnds();
        while (!at(EndOfFile))
        {
            if (startsItem(kind()))
            {
                parseItem();
            }
            else
            {
                parseStatement();
            }
            skipEnds();
        }
    }

    void parseManifest()
    {
        skipEnds();
        const size_t mark = checkpoint();
        start(NameExpr);
        expect(Identifier);
        finish();
        startAt(mark, CompoundLiteral);
        parseLiteralBody();
        finish();
        skipEnds();
    }

    void parseAttributes()
    {
        while (at(LBracket))
        {
            start(Attribute);
            bump();
            expect(Identifier);
            if (accept(LParen))
            {
                while (until(RParen))
                {
                    start(AttributeArgument);
                    if (at(Identifier) && kind(1) == Colon)
                    {
                        bump();
                        bump();
                    }
                    parseExpression();
                    finish();
                    if (!accept(Comma))
                    {
                        break;
                    }
                }
                expect(RParen);
            }
            expect(RBracket);
            finish();
            skipEnds();
        }
    }

    void parseItem()
    {
        const size_t mark = checkpoint();
        parseAttributes();
        switch (kind())
        {
        case KwImport:
            startAt(mark, Import);
            bump();
            expect(Identifier);
            while (accept(Dot))
            {
                expect(Identifier);
            }
            if (accept(KwAs))
            {
                expect(Identifier);
            }
            break;
        case KwInclude:
            startAt(mark, CInclude);
            bump();
            expect(String);
            if (accept(KwAs))
            {
                expect(Identifier);
            }
            if (accept(KwLink))
            {
                expect(String);
            }
            break;
        case KwImpl:
            startAt(mark, ImplDecl);
            bump();
            if (at(LParen))
            {
                parseComptimeParams();
            }
            parseType();
            expect(KwFor);
            parseType();
            if (blockAhead())
            {
                parseMethodList();
            }
            break;
        default:
            parseDeclaration(mark);
        }
        finish();
        endStatement();
    }

    void parseDeclaration(size_t mark)
    {
        switch (at(KwPub) ? kind(1) : kind())
        {
        case KwConst:
            startAt(mark, ConstDecl);
            accept(KwPub);
            bump();
            expect(Identifier);
            if (accept(Colon))
            {
                parseType();
            }
            expect(Eq);
            parseExpression();
            break;
        case KwStruct:
        case KwSet:
            startAt(mark, StructDecl);
            accept(KwPub);
            bump();
            expect(Identifier);
            if (at(LParen))
            {
                parseComptimeParams();
            }
            parseFieldList();
            break;
        case KwEnum:
            startAt(mark, EnumDecl);
            accept(KwPub);
            bump();
            expect(Identifier);
            if (at(LParen))
            {
                parseComptimeParams();
            }
            parseVariantList();
            break;
        case KwTrait:
            startAt(mark, TraitDecl);
            accept(KwPub);
            bump();
            expect(Identifier);
            if (at(LParen))
            {
                parseComptimeParams();
            }
            parseSignatureList();
            break;
        case KwFn:
        case KwExtern:
            startAt(mark, FunctionDecl);
            accept(KwPub);
            parseSignature();
            if (blockAhead())
            {
                parseBlock();
            }
            break;
        default:
            startAt(mark, Error);
            accept(KwPub);
            fail(Message::ExpectedDeclaration);
        }
    }

    void parseSignatureList()
    {
        if (!blockAhead())
        {
            fail(Message::ExpectedBlock);
            return;
        }
        start(SignatureList);
        bump();
        skipEnds();
        while (until(RBrace))
        {
            parseSignature();
            endStatement();
        }
        expect(RBrace);
        finish();
    }

    void parseFieldList()
    {
        if (!blockAhead())
        {
            fail(Message::ExpectedBlock);
            return;
        }
        start(FieldList);
        bump();
        skipEnds();
        while (until(RBrace))
        {
            const size_t mark = checkpoint();
            parseAttributes();
            startAt(mark, Field);
            accept(KwPub);
            expect(Identifier);
            expect(Colon);
            parseType();
            finish();
            endStatement();
        }
        expect(RBrace);
        finish();
    }

    void parseVariantList()
    {
        if (!blockAhead())
        {
            fail(Message::ExpectedBlock);
            return;
        }
        start(VariantList);
        bump();
        skipEnds();
        while (until(RBrace))
        {
            start(Variant);
            expect(Identifier);
            if (accept(LParen))
            {
                while (until(RParen))
                {
                    start(VariantField);
                    if (at(Identifier) && kind(1) == Colon)
                    {
                        bump();
                        bump();
                    }
                    parseType();
                    finish();
                    if (!accept(Comma))
                    {
                        break;
                    }
                }
                expect(RParen);
            }
            finish();
            endStatement();
        }
        expect(RBrace);
        finish();
    }

    void parseMethodList()
    {
        start(MethodList);
        expect(LBrace);
        skipEnds();
        while (until(RBrace))
        {
            const size_t mark = checkpoint();
            parseAttributes();
            startAt(mark, FunctionDecl);
            parseSignature();
            if (blockAhead())
            {
                parseBlock();
            }
            finish();
            endStatement();
        }
        expect(RBrace);
        finish();
    }

    void parseComptimeParams()
    {
        start(ComptimeParamList);
        expect(LParen);
        while (until(RParen))
        {
            start(Param);
            expect(KwComptime);
            expect(Identifier);
            expect(Colon);
            parseType();
            if (accept(Eq))
            {
                parseExpression();
            }
            finish();
            if (!accept(Comma))
            {
                break;
            }
        }
        expect(RParen);
        finish();
    }

    void parseSignature()
    {
        start(Signature);
        if (accept(KwExtern))
        {
            expect(String);
        }
        expect(KwFn);
        expect(Identifier);
        if (accept(Dot))
        {
            expect(Identifier);
            const size_t mark = checkpoint();
            parseParams();
            startAt(mark, ParamList);
            finish();
        }
        else
        {
            const size_t mark = checkpoint();
            const bool allComptime = parseParams();
            if (allComptime && at(Dot))
            {
                startAt(mark, ComptimeParamList);
                finish();
                bump();
                expect(Identifier);
                const size_t methodMark = checkpoint();
                parseParams();
                startAt(methodMark, ParamList);
                finish();
            }
            else
            {
                startAt(mark, ParamList);
                finish();
                if (at(Dot))
                {
                    fail(Message::OnlyComptimeParams);
                }
            }
        }
        if (accept(KwRequires))
        {
            expect(Identifier);
        }
        if (accept(Arrow))
        {
            parseType();
        }
        finish();
    }

    bool parseParams()
    {
        expect(LParen);
        bool allComptime = true;
        while (until(RParen))
        {
            start(Param);
            if (accept(Ellipsis))
            {
                allComptime = false;
            }
            else if (at(KwSelf) || (at(Amp) && (kind(1) == KwSelf || kind(2) == KwSelf)))
            {
                accept(Amp);
                accept(KwMut);
                expect(KwSelf);
                allComptime = false;
            }
            else
            {
                const bool comptime = accept(KwComptime);
                allComptime = comptime && allComptime;
                accept(KwMut);
                expect(Identifier);
                expect(Colon);
                accept(Ellipsis);
                parseType();
            }
            finish();
            if (!accept(Comma))
            {
                break;
            }
        }
        expect(RParen);
        return allComptime;
    }

    void parseBlock()
    {
        const DepthScope scope(*this);
        deepen();
        start(Block);
        expect(LBrace);
        skipEnds();
        while (until(RBrace))
        {
            parseStatement();
        }
        expect(RBrace);
        finish();
    }

    void requireBlock()
    {
        if (!blockAhead())
        {
            fail(Message::ExpectedBlock);
            return;
        }
        parseBlock();
    }

    void parseStatement()
    {
        const SyntaxKind current = kind();
        if (current == KwMut)
        {
            start(BindingStmt);
            bump();
            expect(Identifier);
            if (accept(Colon))
            {
                parseType();
                if (accept(Eq))
                {
                    parseExpression();
                }
            }
            else
            {
                expect(Eq);
                parseExpression();
            }
            finish();
        }
        else if (current == Identifier && kind(1) == Colon)
        {
            if (kind(2) == KwLoop)
            {
                parseLoop();
            }
            else
            {
                start(BindingStmt);
                bump();
                bump();
                parseType();
                expect(Eq);
                parseExpression();
                finish();
            }
        }
        else if (current == KwIf)
        {
            parseIf(false);
        }
        else if (current == KwComptime && kind(1) == KwIf)
        {
            parseIf(true);
        }
        else if (current == KwLoop)
        {
            parseLoop();
        }
        else if (current == KwRegion)
        {
            start(RegionStmt);
            bump();
            if (accept(Identifier) && accept(KwOn))
            {
                parseExpression(HEADER);
            }
            requireBlock();
            finish();
        }
        else if (current == KwThrough)
        {
            start(ThroughStmt);
            bump();
            parseExpression(HEADER);
            requireBlock();
            finish();
        }
        else if ((current == KwTogether || current == KwUnsafe) && kind(1) == LBrace)
        {
            start(current == KwTogether ? TogetherStmt : UnsafeStmt);
            bump();
            parseBlock();
            finish();
        }
        else if (current == KwDefer)
        {
            start(DeferStmt);
            bump();
            parseExpression();
            finish();
        }
        else
        {
            parseSimple();
        }
        endStatement();
    }

    void parseSimple()
    {
        const SyntaxKind current = kind();
        if (current == KwSpawn)
        {
            start(SpawnStmt);
            bump();
            parseExpression();
            finish();
        }
        else if (current == KwReturn)
        {
            start(ReturnStmt);
            bump();
            if (!at(StatementEnd) && !at(RBrace) && !at(EndOfFile))
            {
                parseExpression();
            }
            finish();
        }
        else if (current == KwBreak || current == KwContinue)
        {
            start(current == KwBreak ? BreakStmt : ContinueStmt);
            bump();
            accept(Identifier);
            finish();
        }
        else if (current == Underscore && kind(1) == Eq)
        {
            start(DiscardStmt);
            bump();
            bump();
            parseExpression();
            finish();
        }
        else
        {
            const size_t mark = checkpoint();
            parseExpression();
            if (isAssignment(kind()))
            {
                startAt(mark, AssignStmt);
                bump();
                parseExpression();
            }
            else
            {
                startAt(mark, ExprStmt);
            }
            finish();
        }
    }

    void parseIf(bool comptime)
    {
        start(IfStmt);
        if (comptime)
        {
            bump();
        }
        expect(KwIf);
        parseExpression(HEADER);
        requireBlock();
        if (accept(KwElse))
        {
            const DepthScope scope(*this);
            if (at(KwIf))
            {
                deepen();
                parseIf(false);
            }
            else if (at(KwComptime) && kind(1) == KwIf)
            {
                deepen();
                parseIf(true);
            }
            else
            {
                requireBlock();
            }
        }
        finish();
    }

    void parseLoop()
    {
        start(LoopStmt);
        if (at(Identifier))
        {
            bump();
            expect(Colon);
        }
        expect(KwLoop);
        if (until(LBrace))
        {
            if (isIteration())
            {
                accept(KwComptime);
                accept(KwMut);
                parseLoopPattern();
                expect(KwIn);
                parseExpression(HEADER);
            }
            else
            {
                parseExpression(LOOP_CONDITION);
            }
        }
        requireBlock();
        finish();
    }

    bool isIteration() const
    {
        const SyntaxKind current = kind();
        if (current == KwComptime || current == KwMut)
        {
            return true;
        }
        if (current == Identifier || current == Underscore)
        {
            return kind(1) == KwIn;
        }
        if (current != LParen)
        {
            return false;
        }
        int nesting = 0;
        for (size_t probe = position;; ++probe)
        {
            const SyntaxKind probed = tokens[probe].kind;
            if (isOpening(probed))
            {
                ++nesting;
            }
            else if (isClosing(probed))
            {
                --nesting;
                if (nesting == 0)
                {
                    return tokens[probe + 1].kind == KwIn;
                }
            }
            else if (probed == EndOfFile)
            {
                return false;
            }
        }
    }

    void parseLoopPattern()
    {
        const DepthScope scope(*this);
        deepen();
        if (at(Underscore))
        {
            start(WildcardPattern);
            bump();
            finish();
        }
        else if (at(LParen))
        {
            start(TuplePattern);
            bump();
            parseLoopPattern();
            while (accept(Comma))
            {
                if (at(RParen))
                {
                    break;
                }
                parseLoopPattern();
            }
            expect(RParen);
            finish();
        }
        else
        {
            start(PathPattern);
            expect(Identifier);
            finish();
        }
    }

    void parseExpression(ExpressionFlags flags = {})
    {
        const DepthScope scope(*this);
        deepen();
        parseLogical(flags, KwOr);
    }

    void parseLogical(ExpressionFlags flags, SyntaxKind connective)
    {
        const size_t mark = checkpoint();
        parseLogicalOperand(flags, connective);
        if (!at(connective))
        {
            return;
        }
        startAt(mark, LogicalExpr);
        while (accept(connective))
        {
            parseLogicalOperand(flags, connective);
        }
        finish();
    }

    void parseLogicalOperand(ExpressionFlags flags, SyntaxKind connective)
    {
        if (connective == KwOr)
        {
            parseLogical(flags, KwAnd);
        }
        else
        {
            parseNegation(flags);
        }
    }

    void parseNegation(ExpressionFlags flags)
    {
        if (!at(KwNot))
        {
            parseComparison(flags);
            return;
        }
        const DepthScope scope(*this);
        deepen();
        start(NotExpr);
        bump();
        parseNegation(flags);
        finish();
    }

    void parseComparison(ExpressionFlags flags)
    {
        const size_t mark = checkpoint();
        parseRange(flags);
        if (isComparison(kind()) || (at(KwIn) && !flags.noMembership))
        {
            startAt(mark, at(KwIn) ? MembershipExpr : ComparisonExpr);
            bump();
            parseRange(flags);
        }
        else if (at(KwNot) && kind(1) == KwIn && !flags.noMembership)
        {
            startAt(mark, MembershipExpr);
            bump();
            bump();
            parseRange(flags);
        }
        else if (at(KwNot) && kind(1) == KwIs)
        {
            startAt(mark, IsExpr);
            bump();
            bump();
            parseSinglePattern(flags.noLiteral);
        }
        else if (at(KwIs))
        {
            startAt(mark, IsExpr);
            bump();
            parseSinglePattern(flags.noLiteral);
        }
        else
        {
            return;
        }
        finish();
        const bool membership = at(KwIn) || (at(KwNot) && kind(1) == KwIn);
        const bool negatedPattern = at(KwNot) && kind(1) == KwIs;
        if (isComparison(kind()) || at(KwIs) || negatedPattern || (membership && !flags.noMembership))
        {
            fail(Message::NonAssociativeComparison);
        }
    }

    void parseRange(ExpressionFlags flags)
    {
        const size_t mark = checkpoint();
        if (at(DotDot) || at(DotDotEq))
        {
            const bool inclusive = at(DotDotEq);
            start(RangeExpr);
            bump();
            if (startsOperand(kind()))
            {
                parseAdditive(flags);
            }
            else if (inclusive)
            {
                fail(Message::ExpectedRangeBound);
            }
            finish();
            return;
        }
        parseAdditive(flags);
        if (at(DotDot))
        {
            startAt(mark, RangeExpr);
            bump();
            if (startsOperand(kind()))
            {
                parseAdditive(flags);
            }
            finish();
        }
        else if (at(DotDotEq))
        {
            startAt(mark, RangeExpr);
            bump();
            parseAdditive(flags);
            finish();
        }
    }

    void parseAdditive(ExpressionFlags flags)
    {
        const size_t mark = checkpoint();
        parseMultiplicative(flags);
        if (!isAdditive(kind()))
        {
            return;
        }
        startAt(mark, BinaryExpr);
        while (isAdditive(kind()))
        {
            bump();
            parseMultiplicative(flags);
        }
        finish();
    }

    void parseMultiplicative(ExpressionFlags flags)
    {
        const size_t mark = checkpoint();
        parseCast(flags.noLiteral);
        if (!isMultiplicative(kind()))
        {
            return;
        }
        startAt(mark, BinaryExpr);
        while (isMultiplicative(kind()))
        {
            bump();
            parseCast(flags.noLiteral);
        }
        finish();
    }

    void parseCast(bool noLiteral)
    {
        const size_t mark = checkpoint();
        parsePrefix(noLiteral);
        if (!at(KwAs))
        {
            return;
        }
        startAt(mark, CastExpr);
        while (accept(KwAs))
        {
            parseType();
        }
        finish();
    }

    void parsePrefix(bool noLiteral)
    {
        const DepthScope scope(*this);
        size_t opened = 0;
        for (;;)
        {
            splitStarQuestion();
            if (!isValuePrefix(kind()))
            {
                break;
            }
            deepen();
            if (failed)
            {
                break;
            }
            if (at(Amp))
            {
                start(RefExpr);
                parseReference();
            }
            else
            {
                start(PrefixExpr);
                bump();
            }
            ++opened;
        }
        const size_t mark = checkpoint();
        parseTypeLevel();
        if (at(LBrace) && !noLiteral && !inType)
        {
            startAt(mark, CompoundLiteral);
            parseLiteralBody();
            finish();
        }
        for (; opened > 0; --opened)
        {
            finish();
        }
    }

    void parseReference()
    {
        bump();
        if (at(Identifier) && isRegionLabel())
        {
            bump();
        }
        accept(KwMut);
    }

    bool isRegionLabel() const
    {
        const SyntaxKind next = kind(1);
        if (followsRegionLabel(next))
        {
            return true;
        }
        if (next != LBracket)
        {
            return false;
        }
        const SyntaxKind inside = kind(2);
        return inside == RBracket || inside == Colon || (inside == Underscore && kind(3) == RBracket)
            || (inside == Star && kind(3) == Colon);
    }

    void parseType()
    {
        const bool saved = inType;
        inType = true;
        parseTypeLevel();
        inType = saved;
    }

    void parseTypeLevel()
    {
        const DepthScope scope(*this);
        deepen();
        size_t opened = 0;
        for (;;)
        {
            splitStarQuestion();
            const SyntaxKind current = kind();
            if (!isOneOf(current, {Question, KwDyn, Star, Amp, LBracket}))
            {
                break;
            }
            deepen();
            if (failed)
            {
                break;
            }
            if (current == Amp)
            {
                start(RefExpr);
                parseReference();
            }
            else if (current == LBracket)
            {
                parseBracketPrefix();
            }
            else
            {
                start(current == Question ? OptionalType : current == KwDyn ? DynType : PointerType);
                bump();
            }
            ++opened;
        }
        parsePostfix();
        for (; opened > 0; --opened)
        {
            finish();
        }
    }

    void parseBracketPrefix()
    {
        const size_t mark = checkpoint();
        bump();
        SyntaxKind node = ArrayType;
        if (accept(RBracket))
        {
            node = SliceType;
        }
        else if (at(Colon) || (at(Star) && kind(1) == Colon))
        {
            node = SentinelType;
            accept(Star);
            expect(Colon);
            parseExpression();
            expect(RBracket);
        }
        else if (at(Underscore) && kind(1) == RBracket)
        {
            bump();
            bump();
        }
        else
        {
            parseExpression();
            expect(RBracket);
        }
        accept(KwMut);
        startAt(mark, node);
    }

    void parsePostfix()
    {
        const DepthScope scope(*this);
        const size_t mark = checkpoint();
        parsePrimary();
        for (;;)
        {
            if (!isOneOf(kind(), {Dot, LParen, LBracket}))
            {
                return;
            }
            deepen();
            if (failed)
            {
                return;
            }
            if (at(Dot))
            {
                startAt(mark, FieldExpr);
                bump();
                expect(Identifier);
            }
            else if (at(LParen))
            {
                startAt(mark, CallExpr);
                parseArguments();
            }
            else
            {
                startAt(mark, IndexExpr);
                bump();
                parseExpression();
                expect(RBracket);
            }
            finish();
        }
    }

    void parseArguments()
    {
        const bool saved = inType;
        inType = false;
        start(ArgList);
        expect(LParen);
        while (until(RParen))
        {
            parseExpression();
            if (!accept(Comma))
            {
                break;
            }
        }
        expect(RParen);
        finish();
        inType = saved;
    }

    void parseLiteralBody()
    {
        expect(LBrace);
        skipEnds();
        while (until(RBrace))
        {
            start(LiteralElement);
            if (at(Identifier) && kind(1) == Colon)
            {
                bump();
                bump();
            }
            parseExpression();
            finish();
            if (!accept(Comma) && !at(StatementEnd))
            {
                break;
            }
            skipEnds();
        }
        skipEnds();
        expect(RBrace);
    }

    void parsePrimary()
    {
        const SyntaxKind current = kind();
        if (isLiteralToken(current) || current == KwTrue || current == KwFalse)
        {
            start(Literal);
            bump();
            finish();
        }
        else if (current == Identifier || current == KwSelf || current == KwSelfType)
        {
            start(NameExpr);
            bump();
            finish();
        }
        else if (current == Underscore)
        {
            start(Placeholder);
            bump();
            finish();
        }
        else if (current == LParen)
        {
            parseParenthesized();
        }
        else if (current == KwMatch)
        {
            parseMatch();
        }
        else if (current == KwFn || current == KwExtern)
        {
            parseFunctionExpression();
        }
        else if (current == KwTogether)
        {
            start(TogetherExpr);
            bump();
            if (accept(Dot))
            {
                expect(Identifier);
            }
            parseArguments();
            finish();
        }
        else if (current == KwAsm)
        {
            parseAsm();
        }
        else if (current == KwStruct || current == KwSet)
        {
            start(StructType);
            bump();
            parseFieldList();
            finish();
        }
        else if (current == KwEnum)
        {
            start(EnumType);
            bump();
            parseVariantList();
            finish();
        }
        else
        {
            fail(Message::ExpectedExpression);
        }
    }

    void parseParenthesized()
    {
        const bool saved = inType;
        inType = false;
        const size_t mark = checkpoint();
        bump();
        parseExpression();
        SyntaxKind node = ParenExpr;
        if (accept(Comma))
        {
            node = TupleExpr;
            while (until(RParen))
            {
                parseExpression();
                if (!accept(Comma))
                {
                    break;
                }
            }
        }
        expect(RParen);
        startAt(mark, node);
        finish();
        inType = saved;
    }

    void parseAsm()
    {
        start(AsmExpr);
        bump();
        expect(LParen);
        expect(String);
        while (accept(Comma))
        {
            if (at(RParen))
            {
                break;
            }
            start(AsmOperand);
            expect(Identifier);
            expect(LParen);
            expect(String);
            expect(Comma);
            parseExpression();
            expect(RParen);
            finish();
        }
        expect(RParen);
        finish();
    }

    void parseFunctionExpression()
    {
        start(FunctionExpr);
        if (accept(KwExtern))
        {
            expect(String);
        }
        expect(KwFn);
        if (accept(LBracket))
        {
            expect(Identifier);
            expect(RBracket);
        }
        expect(LParen);
        while (until(RParen))
        {
            start(FunctionParam);
            accept(KwMut);
            if (at(Identifier) && kind(1) == Colon)
            {
                bump();
                bump();
            }
            if (!accept(Ellipsis))
            {
                parseType();
            }
            finish();
            if (!accept(Comma))
            {
                break;
            }
        }
        expect(RParen);
        if (accept(Arrow))
        {
            parseType();
        }
        if (!inType)
        {
            if (accept(FatArrow))
            {
                parseExpression();
            }
            else if (blockAhead())
            {
                parseBlock();
            }
        }
        finish();
    }

    void parseMatch()
    {
        const bool saved = inType;
        inType = false;
        start(MatchExpr);
        bump();
        parseExpression(HEADER);
        if (!blockAhead())
        {
            fail(Message::ExpectedBlock);
        }
        expect(LBrace);
        skipEnds();
        while (until(RBrace))
        {
            start(MatchArm);
            parsePattern(false);
            if (accept(KwIf))
            {
                parseExpression();
            }
            expect(FatArrow);
            if (at(LBrace))
            {
                parseBlock();
            }
            else
            {
                parseSimple();
            }
            finish();
            endStatement();
        }
        expect(RBrace);
        finish();
        inType = saved;
    }

    void parsePattern(bool noLiteral)
    {
        const size_t mark = checkpoint();
        parseSinglePattern(noLiteral);
        if (!at(KwOr))
        {
            return;
        }
        startAt(mark, OrPattern);
        while (accept(KwOr))
        {
            parseSinglePattern(noLiteral);
        }
        finish();
    }

    void parsePatternLiteral()
    {
        accept(Minus);
        if (isLiteralToken(kind()) || at(KwTrue) || at(KwFalse))
        {
            bump();
        }
        else
        {
            fail(Message::ExpectedLiteral);
        }
    }

    void parseSinglePattern(bool noLiteral)
    {
        const DepthScope scope(*this);
        deepen();
        const SyntaxKind current = kind();
        const bool negativeNumber = current == Minus && (kind(1) == Integer || kind(1) == Float);
        if (current == Underscore)
        {
            start(WildcardPattern);
            bump();
            finish();
        }
        else if (isLiteralToken(current) || current == KwTrue || current == KwFalse || negativeNumber)
        {
            start(LiteralPattern);
            parsePatternLiteral();
            if (at(DotDot) || at(DotDotEq))
            {
                bump();
                parsePatternLiteral();
            }
            finish();
        }
        else if (current == KwMut)
        {
            start(BindingPattern);
            bump();
            expect(Identifier);
            finish();
        }
        else if (current == LParen)
        {
            start(TuplePattern);
            bump();
            parsePattern(false);
            while (accept(Comma))
            {
                if (at(RParen))
                {
                    break;
                }
                parsePattern(false);
            }
            expect(RParen);
            finish();
        }
        else
        {
            parsePathPattern(noLiteral);
        }
    }

    void parsePathPattern(bool noLiteral)
    {
        start(PathPattern);
        expect(Identifier);
        while (accept(Dot))
        {
            expect(Identifier);
        }
        if (at(LParen))
        {
            start(PatternArgList);
            bump();
            while (until(RParen))
            {
                start(PatternArg);
                if (at(Identifier) && kind(1) == Colon)
                {
                    bump();
                    bump();
                }
                parsePattern(false);
                finish();
                if (!accept(Comma))
                {
                    break;
                }
            }
            expect(RParen);
            finish();
        }
        else if (at(LBrace) && !noLiteral)
        {
            start(PatternFieldList);
            bump();
            skipEnds();
            while (until(RBrace))
            {
                start(PatternField);
                expect(Identifier);
                if (accept(Colon))
                {
                    parsePattern(false);
                }
                finish();
                if (!accept(Comma) && !at(StatementEnd))
                {
                    break;
                }
                skipEnds();
            }
            expect(RBrace);
            finish();
        }
        finish();
    }
};

}

Parse parse(std::string_view source, ParseMode mode)
{
    Lexed lexed = lex(source);
    return Parser(source, std::move(lexed.tokens)).run(mode, std::move(lexed.error));
}

}

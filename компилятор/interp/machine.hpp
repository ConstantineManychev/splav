#pragma once

#include "interp/interpreter.hpp"
#include "interp/value.hpp"
#include "sema/declarations.hpp"

#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace splav
{

struct Panic
{
    std::string message;
    std::string location;
};

enum class Flow : uint8_t
{
    Normal,
    Return,
    Break,
    Continue,
};

struct Escape
{
    Flow flow;
};

struct Local
{
    std::string key;
    Cell cell;
};

struct Scope
{
    std::vector<Local> locals;
    std::vector<SyntaxNodeView> defers;
};

struct Frame
{
    const FunctionInfo* function = nullptr;
    Module* module = nullptr;
    const TypeDecl* errorType = nullptr;
    std::vector<Scope> scopes;
    Cell returned;
    std::string label;
};

struct Parameter
{
    std::string key;
    SyntaxNodeView type;
    bool self = false;
    bool reference = false;
    bool comptime = false;
    bool variadic = false;
};

struct Prepared
{
    std::vector<Parameter> parameters;
    SyntaxNodeView result;
    SyntaxNodeView body;
    const TypeDecl* error = nullptr;
    size_t runtimeCount = 0;
    size_t comptimeCount = 0;
};

class Machine;

struct Call
{
    Machine& machine;
    const FunctionInfo& function;
    Cell receiver;
    std::vector<Cell>& arguments;

    const Value& argument(size_t index) const;
    const Cell& argumentCell(size_t index) const;
};

using Builtin = Cell (*)(Call& call);

void registerBuiltins(std::unordered_map<std::string, Builtin>& table);

class Machine
{
public:
    Machine(Program& owner, Module& main, const RunOptions& chosen);
    ~Machine();
    Machine(const Machine&) = delete;
    Machine& operator=(const Machine&) = delete;

    int runEntry();
    std::vector<TestOutcome> runTests();

    Cell invoke(const FunctionInfo& function, Cell receiver, std::vector<Cell> arguments);
    [[noreturn]] void panic(const std::string& message) const;
    void write(bool error, std::string_view text) const;
    std::string show(const Cell& value);
    bool equal(const Cell& left, const Cell& right) const;
    std::string describe(const Cell& value);

    Cell enumValue(const TypeDecl* decl, std::string_view variant, std::vector<Cell> payload) const;
    Cell structValue(const TypeDecl* decl, std::vector<Cell> items) const;
    Cell success(Cell payload) const;
    Cell failure(Cell error) const;
    const TypeDecl* findType(std::string_view module, std::string_view key) const;

    Program& program;
    Module& entryModule;
    RunOptions options;
    Declarations declarations;
    const TypeDecl* resultType = nullptr;
    const TypeDecl* arrayType = nullptr;
    const TypeDecl* rangeType = nullptr;
    const TypeDecl* graphemesType = nullptr;
    const TypeDecl* charsType = nullptr;
    const TypeDecl* builderType = nullptr;
    const TypeDecl* failureType = nullptr;
    const TypeDecl* inputErrorType = nullptr;
    const TypeDecl* fileType = nullptr;
    const TypeDecl* regionType = nullptr;
    const TypeDecl* arenaType = nullptr;
    const TypeDecl* uniqueType = nullptr;
    std::map<int32_t, std::FILE*> files;
    std::set<const std::string*> skipReasons;
    int32_t nextDescriptor = 3;

private:
    Frame& frame();
    const std::string& keyOf(const SyntaxToken& token);
    std::string locationOf(const Module* module, uint32_t offset) const;

    Cell* findLocal(const std::string& key);
    void bindLocal(const std::string& key, Cell cell);
    Cell consume(const Cell& cell) const;
    void assignTo(const Cell& place, Cell value) const;

    std::pair<const Entry*, Module*> lookup(Module* module, const std::string& key) const;
    const TypeDecl* resolveDecl(const SyntaxNodeView& node, Module* module) const;
    std::optional<Number> resolveNumber(const SyntaxNodeView& node, Module* module) const;
    std::optional<Number> numberOf(const SyntaxNodeView& type, Module* module, const std::vector<Local>* bindings);
    void coerce(
        const Cell& cell, const SyntaxNodeView& type, Module* module, const std::vector<Local>* bindings = nullptr);
    const Prepared& prepare(const FunctionInfo& function);
    std::vector<Local> inferTypeArguments(const Prepared& signature, const std::vector<Cell>& arguments);
    Cell typeOf(const Cell& value) const;
    std::string typeName(const Cell& value) const;
    Cell synthesize(const SyntaxNodeView& node);
    Cell closureValue(const SyntaxNodeView& node);
    Cell callClosure(const Closure& closure, std::vector<Cell> arguments);
    Cell defaultValue(const SyntaxNodeView& type, Module* module);
    Cell wrapError(const Cell& error);
    std::optional<std::pair<const TypeDecl*, uint32_t>> findVariant(const std::string& key) const;
    Cell constant(const Entry& entry, Module* owner);
    Cell entryValue(const Entry& entry, Module* owner);
    const FunctionInfo* methodFor(const Value& value, const std::string& key) const;

    Flow execBlock(const SyntaxNodeView& block);
    Flow exec(const SyntaxNodeView& statement);
    Flow execBinding(const SyntaxNodeView& statement);
    Flow execAssign(const SyntaxNodeView& statement);
    Flow execIf(const SyntaxNodeView& statement);
    Flow execLoop(const SyntaxNodeView& statement);
    void runDefers(std::vector<SyntaxNodeView> defers);
    void destructure(const SyntaxNodeView& target, const Cell& value);
    std::optional<Flow> loopStep(Flow flow, const std::string& label);
    void forEachElement(const Cell& source, const std::function<bool(const Cell&)>& visitor);

    bool matchPattern(const SyntaxNodeView& pattern, const Cell& value, bool viaReference);
    bool matchPath(const SyntaxNodeView& pattern, const Cell& value, bool viaReference);
    void bindPatternName(const SyntaxToken& name, const Cell& value, bool viaReference);
    Cell patternLiteral(const std::vector<SyntaxToken>& tokens, size_t& index);

    Cell eval(const SyntaxNodeView& node);
    Cell literal(const SyntaxNodeView& node);
    Cell literalToken(const SyntaxToken& token);
    Cell name(const SyntaxNodeView& node);
    Cell field(const SyntaxNodeView& node);
    Cell member(const Cell& receiver, const std::string& key, std::string_view display);
    Cell call(const SyntaxNodeView& node);
    Cell callMethod(
        const Cell& receiver, const std::string& key, std::string_view display, std::vector<Cell> arguments);
    Cell nativeMethod(const Cell& receiver, const std::string& key, std::vector<Cell>& arguments, bool& handled);
    Cell construct(const TypeDecl* decl, uint32_t variant, std::vector<Cell> arguments);
    Cell index(const SyntaxNodeView& node);
    Cell binary(const SyntaxNodeView& node);
    Cell arithmetic(SyntaxKind operation, const Cell& left, const Cell& right);
    Cell vectorArithmetic(SyntaxKind operation, const Cell& left, const Cell& right);
    Cell vectorType(const SyntaxNodeView& node, const std::vector<SyntaxNodeView>& arguments);
    Cell checkedArithmetic(SyntaxKind operation, const Cell& left, const Cell& right);
    Cell comparison(const SyntaxNodeView& node);
    Cell logical(const SyntaxNodeView& node);
    Cell membership(const SyntaxNodeView& node);
    Cell range(const SyntaxNodeView& node);
    Cell cast(const SyntaxNodeView& node);
    Cell prefix(const SyntaxNodeView& node);
    Cell attempt(const SyntaxNodeView& operand);
    Cell compound(const SyntaxNodeView& node);
    Cell match(const SyntaxNodeView& node, Flow* flow);
    int compare(const Cell& left, const Cell& right) const;
    bool truth(const Cell& value) const;
    WideInteger integerOf(const Cell& value) const;
    size_t sizeOf(const Cell& value) const;
    bool inRange(const Value& range, const Cell& value) const;
    std::pair<size_t, size_t> sliceBounds(const Value& range, size_t length) const;

    std::unordered_map<std::string, Builtin> builtins;
    std::unordered_map<const FunctionInfo*, Builtin> builtinCache;
    std::deque<Frame> frames;
    std::unordered_map<const char*, std::string> keys;
    std::unordered_map<const char*, Cell> literals;
    std::map<std::pair<Module*, uint32_t>, Cell> constants;
    std::unordered_map<const FunctionInfo*, Prepared> prepared;
    std::deque<TypeDecl> synthesizedTypes;
    std::map<std::string, const TypeDecl*> instances;
    std::unordered_map<const TypeDecl*, std::vector<Local>> typeArguments;
    std::unordered_map<std::string, std::vector<std::pair<const TypeDecl*, uint32_t>>> variants;
    Module* currentModule = nullptr;
    uint32_t currentOffset = 0;
    size_t depth = 0;
};

}

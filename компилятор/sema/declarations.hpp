#pragma once

#include "sema/program.hpp"
#include "sema/syntax_view.hpp"
#include "sema/types.hpp"

#include <deque>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace splav
{

constexpr std::string_view RESULT_TYPE = "Итог";
constexpr std::string_view REGION_TYPE = "Регион";
constexpr std::string_view BUILTIN_ATTRIBUTE = "Встроено";

struct FunctionInfo
{
    std::string key;
    std::string display;
    Module* module = nullptr;
    SyntaxNodeView item;
    SyntaxNodeView signature;
    const TypeDecl* owner = nullptr;
    std::string builtinOwner;
    std::vector<std::string> ownerParameters;
    bool requiresRegion = false;
    bool open = false;
    bool builtin = false;
    uint32_t nameOffset = 0;
    uint32_t nameLength = 0;
};

struct ImplInfo
{
    const TypeDecl* trait = nullptr;
    const TypeDecl* target = nullptr;
    std::string builtinTarget;
    Module* module = nullptr;
    SyntaxNodeView node;
    bool builtin = false;
};

enum class EntryKind
{
    Type,
    Function,
    Const,
    Module,
    Header,
};

struct Entry
{
    EntryKind kind = EntryKind::Const;
    const TypeDecl* type = nullptr;
    const FunctionInfo* function = nullptr;
    SyntaxNodeView constant;
    Module* target = nullptr;
};

bool requiresRegion(const SyntaxNodeView& signature);
bool hasAttribute(const SyntaxNodeView& item, std::string_view name);
std::string firstLine(std::string_view source, uint32_t offset, uint32_t end);

class Declarations
{
public:
    Declarations(Program& owner, Module& main);

    const Entry* lookup(Module* module, const std::string& key) const;
    const FunctionInfo* method(const TypeDecl* owner, const std::string& builtinOwner, const std::string& key) const;
    bool hasImpl(const TypeDecl* trait, const TypeDecl* target, const std::string& builtinTarget) const;

    Program& program;
    Module* prelude = nullptr;
    std::vector<Module*> order;
    std::map<Module*, std::map<std::string, Entry>> scopes;
    std::deque<TypeDecl> types;
    std::deque<FunctionInfo> functions;
    std::vector<ImplInfo> impls;
    std::map<const TypeDecl*, std::map<std::string, const FunctionInfo*>> methods;
    std::map<std::string, std::map<std::string, const FunctionInfo*>> builtinMethods;

private:
    void collect(Module& module);
    void describe(TypeDecl& decl);
    FunctionInfo& addFunction(Module& module, const SyntaxNodeView& item, const SyntaxNodeView& signature);
    void linkMembers(Module& module);
    void registerMethod(Module& module, const SyntaxNodeView& item, const SyntaxNodeView& signature);
    const TypeDecl* typeDeclOf(Module& module, const SyntaxNodeView& node, std::string& builtinName);
    void registerImpl(Module& module, const SyntaxNodeView& item);
};

}

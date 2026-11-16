#pragma once

#include "syntax/syntax_kind.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace splav
{

class GreenNode;

struct GreenToken
{
    SyntaxKind kind;
    std::string text;
};

class GreenElement
{
public:
    explicit GreenElement(std::shared_ptr<const GreenNode> child);
    explicit GreenElement(std::shared_ptr<const GreenToken> leaf);

    const GreenNode* node() const;
    const GreenToken* token() const;
    SyntaxKind kind() const;
    uint32_t textLength() const;

private:
    std::shared_ptr<const GreenNode> nodePointer;
    std::shared_ptr<const GreenToken> tokenPointer;
};

class GreenNode
{
public:
    GreenNode(SyntaxKind nodeKind, std::vector<GreenElement> nodeChildren);

    SyntaxKind kind() const;
    uint32_t textLength() const;
    const std::vector<GreenElement>& children() const;
    std::string text() const;
    void appendText(std::string& output) const;

private:
    SyntaxKind nodeKind;
    uint32_t length;
    std::vector<GreenElement> nodeChildren;
};

class GreenBuilder
{
public:
    void startNode(SyntaxKind kind);
    void startNodeAt(size_t checkpoint, SyntaxKind kind);
    void finishNode();
    void token(SyntaxKind kind, std::string_view text);
    size_t checkpoint() const;
    size_t openNodes() const;
    std::shared_ptr<const GreenNode> finish();

private:
    struct Open
    {
        SyntaxKind kind;
        size_t firstChild;
    };

    struct TokenKey
    {
        SyntaxKind kind;
        std::string text;

        bool operator==(const TokenKey& other) const = default;
    };

    struct TokenKeyHash
    {
        size_t operator()(const TokenKey& key) const;
    };

    std::vector<Open> open;
    std::vector<GreenElement> children;
    std::unordered_map<TokenKey, std::shared_ptr<const GreenToken>, TokenKeyHash> cache;
};

std::string dumpTree(const GreenNode& root);

}

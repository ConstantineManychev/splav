#pragma once

#include "syntax/green.hpp"

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace splav
{

struct SyntaxToken
{
    SyntaxKind kind;
    std::string_view text;
    uint32_t offset;

    uint32_t end() const;
};

class SyntaxNodeView
{
public:
    SyntaxNodeView() = default;
    SyntaxNodeView(const GreenNode* node, uint32_t offset);

    explicit operator bool() const;
    SyntaxKind kind() const;
    uint32_t offset() const;
    uint32_t end() const;
    const GreenNode* green() const;

    std::vector<SyntaxNodeView> nodes() const;
    std::vector<SyntaxToken> tokens() const;
    SyntaxNodeView node(SyntaxKind kind) const;
    SyntaxNodeView child(size_t index) const;
    std::optional<SyntaxToken> token(SyntaxKind kind) const;
    std::optional<SyntaxToken> firstToken() const;
    bool hasToken(SyntaxKind kind) const;

    template <typename Visitor>
    void forEachChild(Visitor&& visitor) const
    {
        uint32_t position = start;
        for (const GreenElement& element : greenNode->children())
        {
            if (const GreenNode* child = element.node())
            {
                visitor(SyntaxNodeView(child, position), std::optional<SyntaxToken>());
            }
            else if (!isTrivia(element.kind()))
            {
                visitor(SyntaxNodeView(),
                    std::optional<SyntaxToken>(SyntaxToken{element.kind(), element.token()->text, position}));
            }
            position += element.textLength();
        }
    }

private:
    const GreenNode* greenNode = nullptr;
    uint32_t start = 0;
};

}

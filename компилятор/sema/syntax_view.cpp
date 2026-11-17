#include "sema/syntax_view.hpp"

namespace splav
{

uint32_t SyntaxToken::end() const
{
    return offset + static_cast<uint32_t>(text.size());
}

SyntaxNodeView::SyntaxNodeView(const GreenNode* node, uint32_t offset) : greenNode(node), start(offset)
{
}

SyntaxNodeView::operator bool() const
{
    return greenNode != nullptr;
}

SyntaxKind SyntaxNodeView::kind() const
{
    return greenNode->kind();
}

uint32_t SyntaxNodeView::offset() const
{
    return start;
}

uint32_t SyntaxNodeView::end() const
{
    return start + greenNode->textLength();
}

const GreenNode* SyntaxNodeView::green() const
{
    return greenNode;
}

std::vector<SyntaxNodeView> SyntaxNodeView::nodes() const
{
    std::vector<SyntaxNodeView> result;
    forEachChild([&result](const SyntaxNodeView& child, const std::optional<SyntaxToken>&)
    {
        if (child)
        {
            result.push_back(child);
        }
    });
    return result;
}

std::vector<SyntaxToken> SyntaxNodeView::tokens() const
{
    std::vector<SyntaxToken> result;
    forEachChild([&result](const SyntaxNodeView&, const std::optional<SyntaxToken>& token)
    {
        if (token)
        {
            result.push_back(*token);
        }
    });
    return result;
}

SyntaxNodeView SyntaxNodeView::node(SyntaxKind wanted) const
{
    uint32_t position = start;
    for (const GreenElement& element : greenNode->children())
    {
        if (const GreenNode* child = element.node(); child != nullptr && child->kind() == wanted)
        {
            return SyntaxNodeView(child, position);
        }
        position += element.textLength();
    }
    return SyntaxNodeView();
}

SyntaxNodeView SyntaxNodeView::child(size_t index) const
{
    uint32_t position = start;
    for (const GreenElement& element : greenNode->children())
    {
        if (const GreenNode* found = element.node(); found != nullptr && index-- == 0)
        {
            return SyntaxNodeView(found, position);
        }
        position += element.textLength();
    }
    return SyntaxNodeView();
}

std::optional<SyntaxToken> SyntaxNodeView::token(SyntaxKind wanted) const
{
    uint32_t position = start;
    for (const GreenElement& element : greenNode->children())
    {
        if (element.node() == nullptr && element.kind() == wanted)
        {
            return SyntaxToken{wanted, element.token()->text, position};
        }
        position += element.textLength();
    }
    return std::nullopt;
}

std::optional<SyntaxToken> SyntaxNodeView::firstToken() const
{
    std::optional<SyntaxToken> found;
    forEachChild([&found](const SyntaxNodeView& child, const std::optional<SyntaxToken>& token)
    {
        if (found)
        {
            return;
        }
        found = child ? child.firstToken() : token;
    });
    return found;
}

bool SyntaxNodeView::hasToken(SyntaxKind wanted) const
{
    for (const GreenElement& element : greenNode->children())
    {
        if (element.node() == nullptr && element.kind() == wanted)
        {
            return true;
        }
    }
    return false;
}

}

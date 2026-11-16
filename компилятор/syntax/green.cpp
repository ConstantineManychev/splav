#include "syntax/green.hpp"

#include <functional>

namespace splav
{

namespace
{

constexpr size_t MAX_CACHED_TOKEN_LENGTH = 32;

void appendEscaped(std::string& output, std::string_view text)
{
    for (const char symbol : text)
    {
        switch (symbol)
        {
        case '\n':
            output += "\\n";
            break;
        case '\r':
            output += "\\r";
            break;
        case '\t':
            output += "\\t";
            break;
        case '"':
            output += "\\\"";
            break;
        case '\\':
            output += "\\\\";
            break;
        default:
            output += symbol;
        }
    }
}

void dumpElement(std::string& output, const GreenElement& element, uint32_t offset, size_t depth)
{
    output.append(depth * 2, ' ');
    output += kindName(element.kind());
    output += "@" + std::to_string(offset) + ".." + std::to_string(offset + element.textLength());
    if (const GreenToken* token = element.token())
    {
        output += " \"";
        appendEscaped(output, token->text);
        output += "\"\n";
        return;
    }
    output += "\n";
    uint32_t childOffset = offset;
    for (const GreenElement& child : element.node()->children())
    {
        dumpElement(output, child, childOffset, depth + 1);
        childOffset += child.textLength();
    }
}

}

GreenElement::GreenElement(std::shared_ptr<const GreenNode> child) : nodePointer(std::move(child))
{
}

GreenElement::GreenElement(std::shared_ptr<const GreenToken> leaf) : tokenPointer(std::move(leaf))
{
}

const GreenNode* GreenElement::node() const
{
    return nodePointer.get();
}

const GreenToken* GreenElement::token() const
{
    return tokenPointer.get();
}

SyntaxKind GreenElement::kind() const
{
    return nodePointer ? nodePointer->kind() : tokenPointer->kind;
}

uint32_t GreenElement::textLength() const
{
    return nodePointer ? nodePointer->textLength() : static_cast<uint32_t>(tokenPointer->text.size());
}

GreenNode::GreenNode(SyntaxKind kind, std::vector<GreenElement> elements)
    : nodeKind(kind), length(0), nodeChildren(std::move(elements))
{
    for (const GreenElement& child : nodeChildren)
    {
        length += child.textLength();
    }
}

SyntaxKind GreenNode::kind() const
{
    return nodeKind;
}

uint32_t GreenNode::textLength() const
{
    return length;
}

const std::vector<GreenElement>& GreenNode::children() const
{
    return nodeChildren;
}

std::string GreenNode::text() const
{
    std::string output;
    output.reserve(length);
    appendText(output);
    return output;
}

void GreenNode::appendText(std::string& output) const
{
    for (const GreenElement& child : nodeChildren)
    {
        if (const GreenToken* token = child.token())
        {
            output += token->text;
        }
        else
        {
            child.node()->appendText(output);
        }
    }
}

size_t GreenBuilder::TokenKeyHash::operator()(const TokenKey& key) const
{
    return std::hash<std::string>()(key.text) * 31 + static_cast<size_t>(key.kind);
}

void GreenBuilder::startNode(SyntaxKind kind)
{
    open.push_back(Open{kind, children.size()});
}

void GreenBuilder::startNodeAt(size_t checkpoint, SyntaxKind kind)
{
    open.push_back(Open{kind, checkpoint});
}

void GreenBuilder::finishNode()
{
    const Open current = open.back();
    open.pop_back();
    std::vector<GreenElement> own(
        std::make_move_iterator(children.begin() + static_cast<std::ptrdiff_t>(current.firstChild)),
        std::make_move_iterator(children.end()));
    children.erase(children.begin() + static_cast<std::ptrdiff_t>(current.firstChild), children.end());
    children.emplace_back(std::make_shared<const GreenNode>(current.kind, std::move(own)));
}

void GreenBuilder::token(SyntaxKind kind, std::string_view text)
{
    if (text.size() > MAX_CACHED_TOKEN_LENGTH)
    {
        children.emplace_back(std::make_shared<const GreenToken>(GreenToken{kind, std::string(text)}));
        return;
    }
    TokenKey key{kind, std::string(text)};
    auto found = cache.find(key);
    if (found == cache.end())
    {
        auto created = std::make_shared<const GreenToken>(GreenToken{kind, key.text});
        found = cache.emplace(std::move(key), std::move(created)).first;
    }
    children.emplace_back(found->second);
}

size_t GreenBuilder::checkpoint() const
{
    return children.size();
}

size_t GreenBuilder::openNodes() const
{
    return open.size();
}

std::shared_ptr<const GreenNode> GreenBuilder::finish()
{
    while (open.size() > 1)
    {
        finishNode();
    }
    const Open root = open.back();
    open.pop_back();
    auto node = std::make_shared<const GreenNode>(root.kind, std::move(children));
    children.clear();
    return node;
}

std::string dumpTree(const GreenNode& root)
{
    std::string output;
    output.append(kindName(root.kind()));
    output += "@0.." + std::to_string(root.textLength()) + "\n";
    uint32_t offset = 0;
    for (const GreenElement& child : root.children())
    {
        dumpElement(output, child, offset, 1);
        offset += child.textLength();
    }
    return output;
}

}

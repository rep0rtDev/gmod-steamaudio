// src/util/KeyValues.cpp
#include "util/KeyValues.h"

#include <cctype>

namespace sa {

namespace {

struct Token {
    enum Kind { End, Open, Close, String, Condition } kind = End;
    std::string text;
    bool quoted = false;
};

class Lexer {
public:
    Lexer(const char* begin, const char* end, size_t maxToken) : m_p(begin), m_end(end), m_maxToken(maxToken) {}

    Token Next()
    {
        SkipSpaceAndComments();
        Token t;
        if (m_p >= m_end)
            return t;
        const char c = *m_p;
        if (c == '{') {
            ++m_p;
            t.kind = Token::Open;
            return t;
        }
        if (c == '}') {
            ++m_p;
            t.kind = Token::Close;
            return t;
        }
        if (c == '"') {
            ++m_p;
            t.kind = Token::String;
            t.quoted = true;
            while (m_p < m_end && *m_p != '"' && *m_p != '\n') {
                if (*m_p == '\\' && m_p + 1 < m_end && (m_p[1] == '"' || m_p[1] == '\\')) {
                    ++m_p;
                }
                Append(t.text, *m_p++);
            }
            if (m_p < m_end && *m_p == '"')
                ++m_p;
            return t;
        }
        if (c == '[') {
            t.kind = Token::Condition;
            ++m_p;
            while (m_p < m_end && *m_p != ']' && *m_p != '\n')
                Append(t.text, *m_p++);
            if (m_p < m_end && *m_p == ']')
                ++m_p;
            return t;
        }
        t.kind = Token::String;
        while (m_p < m_end && !std::isspace(static_cast<unsigned char>(*m_p)) && *m_p != '{' && *m_p != '}' &&
               *m_p != '"' && *m_p != '\0')
            Append(t.text, *m_p++);
        return t;
    }

private:
    void Append(std::string& s, char c)
    {
        if (s.size() < m_maxToken)
            s.push_back(c);
        else
            m_overflowed = true;
    }

    void SkipSpaceAndComments()
    {
        for (;;) {
            while (m_p < m_end && (std::isspace(static_cast<unsigned char>(*m_p)) || *m_p == '\0'))
                ++m_p;
            if (m_p + 1 < m_end && m_p[0] == '/' && m_p[1] == '/') {
                while (m_p < m_end && *m_p != '\n')
                    ++m_p;
                continue;
            }
            return;
        }
    }

    const char* m_p;
    const char* m_end;
    size_t m_maxToken;
    bool m_overflowed = false;

public:
    bool Overflowed() const { return m_overflowed; }
};

class Parser {
public:
    Parser(Lexer& lexer, KvDocument& out, const KvLimits& limits) : m_lex(lexer), m_out(out), m_limits(limits) {}

    void Run()
    {
        ParseBlock(m_out.roots, 0);
    }

private:
    // Parses "key value" / "key { ... }" pairs into `into` until a closing
    // brace (depth > 0) or end of input.
    void ParseBlock(std::vector<KvNode>& into, size_t depth)
    {
        for (;;) {
            Token key = m_lex.Next();
            if (key.kind == Token::End) {
                if (depth > 0)
                    m_out.truncated = true;
                return;
            }
            if (key.kind == Token::Close) {
                if (depth == 0) {
                    m_out.truncated = true; // stray brace at top level: ignore
                    continue;
                }
                return;
            }
            if (key.kind == Token::Open) {
                // Block without a key: consume it so the structure stays balanced.
                std::vector<KvNode> discard;
                if (depth + 1 <= m_limits.maxDepth)
                    ParseBlock(discard, depth + 1);
                else
                    SkipBlock();
                m_out.truncated = true;
                continue;
            }
            if (key.kind == Token::Condition)
                continue;

            if (!key.quoted && !key.text.empty() && key.text[0] == '#') {
                Token target = m_lex.Next();
                if (target.kind == Token::String) {
                    const std::string directive = KvLower(key.text);
                    if (directive == "#include" || directive == "#base")
                        m_out.includes.push_back(target.text);
                }
                continue;
            }

            Token value = m_lex.Next();
            // A condition may sit between key and value: "key" [$WIN32] "value"
            std::string condition;
            if (value.kind == Token::Condition) {
                condition = value.text;
                value = m_lex.Next();
            }
            if (value.kind == Token::End) {
                m_out.truncated = true;
                return;
            }
            if (value.kind == Token::Close) {
                // Dangling key: keep as empty leaf.
                AddLeaf(into, key.text, std::string(), condition);
                if (depth == 0)
                    continue;
                return;
            }
            if (value.kind == Token::Open) {
                if (m_out.nodes >= m_limits.maxNodes) {
                    m_out.truncated = true;
                    SkipBlock();
                    continue;
                }
                KvNode node;
                node.key = key.text;
                node.isBlock = true;
                node.condition = condition;
                ++m_out.nodes;
                if (depth + 1 > m_limits.maxDepth) {
                    m_out.truncated = true;
                    SkipBlock();
                } else {
                    ParseBlock(node.children, depth + 1);
                }
                into.push_back(std::move(node));
                continue;
            }
            KvNode* leaf = AddLeaf(into, key.text, value.text, condition);
            // Trailing condition after the value: "key" "value" [$X360]
            Token trailing = PeekCondition();
            if (trailing.kind == Token::Condition && leaf)
                leaf->condition = trailing.text;
        }
    }

    // Consumes a condition token if one follows; otherwise pushes the token
    // back by re-lexing from its start.
    Token PeekCondition()
    {
        Lexer saved = m_lex;
        Token t = m_lex.Next();
        if (t.kind == Token::Condition)
            return t;
        m_lex = saved;
        Token none;
        return none;
    }

    KvNode* AddLeaf(std::vector<KvNode>& into, const std::string& key, const std::string& value,
                    const std::string& condition)
    {
        if (m_out.nodes >= m_limits.maxNodes) {
            m_out.truncated = true;
            return nullptr;
        }
        KvNode node;
        node.key = key;
        node.value = value;
        node.condition = condition;
        ++m_out.nodes;
        into.push_back(std::move(node));
        return &into.back();
    }

    void SkipBlock()
    {
        int depth = 1;
        while (depth > 0) {
            Token t = m_lex.Next();
            if (t.kind == Token::End)
                return;
            if (t.kind == Token::Open)
                ++depth;
            else if (t.kind == Token::Close)
                --depth;
        }
    }

    Lexer& m_lex;
    KvDocument& m_out;
    const KvLimits& m_limits;
};

} // namespace

std::string KvLower(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool KvKeyEquals(const std::string& a, const char* b)
{
    size_t i = 0;
    for (; i < a.size() && b[i] != '\0'; ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return i == a.size() && b[i] == '\0';
}

const KvNode* KvNode::Find(const char* name) const
{
    for (const KvNode& child : children)
        if (KvKeyEquals(child.key, name))
            return &child;
    return nullptr;
}

std::string KvNode::Get(const char* name, const std::string& fallback) const
{
    for (const KvNode& child : children)
        if (!child.isBlock && KvKeyEquals(child.key, name))
            return child.value;
    return fallback;
}

bool ParseKeyValues(const char* data, size_t size, KvDocument& out, const KvLimits& limits)
{
    out = KvDocument{};
    if (!data) {
        out.error = "null input";
        return false;
    }
    // Skip a UTF-8 BOM.
    if (size >= 3 && static_cast<unsigned char>(data[0]) == 0xEF && static_cast<unsigned char>(data[1]) == 0xBB &&
        static_cast<unsigned char>(data[2]) == 0xBF) {
        data += 3;
        size -= 3;
    }
    Lexer lexer(data, data + size, limits.maxTokenLength);
    Parser parser(lexer, out, limits);
    parser.Run();
    if (lexer.Overflowed())
        out.truncated = true;
    if (out.roots.empty() && out.includes.empty()) {
        out.error = out.truncated ? "unbalanced or truncated KeyValues" : "no key/value pairs";
        return false;
    }
    return true;
}

bool ParseKeyValues(const std::string& text, KvDocument& out, const KvLimits& limits)
{
    return ParseKeyValues(text.data(), text.size(), out, limits);
}

} // namespace sa

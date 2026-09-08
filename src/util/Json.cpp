// src/util/Json.cpp
#include "Json.h"

#include <cmath>
#include <cstdlib>
#include <cerrno>
#include <limits>
#include <fstream>
#include <sstream>

namespace sa {

namespace {

const JsonValue& NullValue()
{
    static const JsonValue kNull;
    return kNull;
}

const std::string& EmptyString()
{
    static const std::string kEmpty;
    return kEmpty;
}

const std::vector<JsonValue>& EmptyArray()
{
    static const std::vector<JsonValue> kEmpty;
    return kEmpty;
}

const std::map<std::string, JsonValue>& EmptyObject()
{
    static const std::map<std::string, JsonValue> kEmpty;
    return kEmpty;
}

class Parser {
public:
    explicit Parser(const std::string& text) : m_text(text) {}

    JsonParseResult Run()
    {
        JsonParseResult result;
        SkipWhitespace();
        if (!ParseValue(result.value)) {
            result.ok = false;
            result.error = m_error;
            result.line = m_line;
            result.column = m_column;
            return result;
        }
        SkipWhitespace();
        if (m_pos != m_text.size()) {
            Fail("trailing characters after JSON value");
            result.ok = false;
            result.error = m_error;
            result.line = m_line;
            result.column = m_column;
            return result;
        }
        result.ok = true;
        return result;
    }

private:
    const std::string& m_text;
    size_t m_pos = 0;
    size_t m_line = 1;
    size_t m_column = 1;
    size_t m_depth = 0;
    std::string m_error;

    bool AtEnd() const { return m_pos >= m_text.size(); }
    char Peek() const { return AtEnd() ? '\0' : m_text[m_pos]; }
    char PeekAt(size_t offset) const
    {
        return (m_pos + offset < m_text.size()) ? m_text[m_pos + offset] : '\0';
    }

    char Advance()
    {
        const char c = m_text[m_pos++];
        if (c == '\n') {
            ++m_line;
            m_column = 1;
        } else {
            ++m_column;
        }
        return c;
    }

    void Fail(const std::string& message)
    {
        if (m_error.empty())
            m_error = message;
    }

    void SkipWhitespace()
    {
        for (;;) {
            while (!AtEnd() && (Peek() == ' ' || Peek() == '\t' || Peek() == '\n' || Peek() == '\r'))
                Advance();
            if (Peek() == '/' && PeekAt(1) == '/') {
                while (!AtEnd() && Peek() != '\n')
                    Advance();
                continue;
            }
            if (Peek() == '/' && PeekAt(1) == '*') {
                Advance();
                Advance();
                while (!AtEnd() && !(Peek() == '*' && PeekAt(1) == '/'))
                    Advance();
                if (!AtEnd()) {
                    Advance();
                    Advance();
                }
                continue;
            }
            break;
        }
    }

    bool ParseValue(JsonValue& out)
    {
        if (m_depth >= 64) {
            Fail("JSON nesting limit exceeded");
            return false;
        }
        struct DepthGuard {
            size_t& depth;
            ~DepthGuard() { --depth; }
        } guard{m_depth};
        ++m_depth;
        switch (Peek()) {
        case '{': return ParseObject(out);
        case '[': return ParseArray(out);
        case '"': {
            std::string s;
            if (!ParseString(s))
                return false;
            out = JsonValue::MakeString(std::move(s));
            return true;
        }
        case 't':
            if (MatchLiteral("true")) {
                out = JsonValue::MakeBool(true);
                return true;
            }
            Fail("invalid literal");
            return false;
        case 'f':
            if (MatchLiteral("false")) {
                out = JsonValue::MakeBool(false);
                return true;
            }
            Fail("invalid literal");
            return false;
        case 'n':
            if (MatchLiteral("null")) {
                out = JsonValue::MakeNull();
                return true;
            }
            Fail("invalid literal");
            return false;
        default:
            if (Peek() == '-' || (Peek() >= '0' && Peek() <= '9'))
                return ParseNumber(out);
            Fail(AtEnd() ? "unexpected end of input" : "unexpected character");
            return false;
        }
    }

    bool MatchLiteral(const char* literal)
    {
        size_t i = 0;
        while (literal[i] != '\0') {
            if (PeekAt(i) != literal[i])
                return false;
            ++i;
        }
        for (size_t k = 0; k < i; ++k)
            Advance();
        return true;
    }

    bool ParseNumber(JsonValue& out)
    {
        const size_t start = m_pos;
        if (Peek() == '-')
            Advance();
        if (!(Peek() >= '0' && Peek() <= '9')) {
            Fail("invalid number");
            return false;
        }
        while (Peek() >= '0' && Peek() <= '9')
            Advance();
        if (Peek() == '.') {
            Advance();
            if (!(Peek() >= '0' && Peek() <= '9')) {
                Fail("invalid fraction");
                return false;
            }
            while (Peek() >= '0' && Peek() <= '9')
                Advance();
        }
        if (Peek() == 'e' || Peek() == 'E') {
            Advance();
            if (Peek() == '+' || Peek() == '-')
                Advance();
            if (!(Peek() >= '0' && Peek() <= '9')) {
                Fail("invalid exponent");
                return false;
            }
            while (Peek() >= '0' && Peek() <= '9')
                Advance();
        }
        const std::string token = m_text.substr(start, m_pos - start);
        const double value = std::strtod(token.c_str(), nullptr);
        if (!std::isfinite(value)) {
            Fail("number is outside the finite range");
            return false;
        }
        out = JsonValue::MakeNumber(value);
        return true;
    }

    static void AppendUtf8(std::string& out, uint32_t cp)
    {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool ParseHex4(uint32_t& out)
    {
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = Peek();
            uint32_t digit;
            if (c >= '0' && c <= '9')
                digit = static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f')
                digit = static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                digit = static_cast<uint32_t>(c - 'A' + 10);
            else {
                Fail("invalid \\u escape");
                return false;
            }
            out = (out << 4) | digit;
            Advance();
        }
        return true;
    }

    bool ParseString(std::string& out)
    {
        if (Peek() != '"') {
            Fail("expected string");
            return false;
        }
        Advance();
        for (;;) {
            if (AtEnd()) {
                Fail("unterminated string");
                return false;
            }
            const char c = Advance();
            if (c == '"')
                return true;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (AtEnd()) {
                Fail("unterminated escape");
                return false;
            }
            const char e = Advance();
            switch (e) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                uint32_t cp;
                if (!ParseHex4(cp))
                    return false;
                if (cp >= 0xD800 && cp <= 0xDBFF && Peek() == '\\' && PeekAt(1) == 'u') {
                    Advance();
                    Advance();
                    uint32_t low;
                    if (!ParseHex4(low))
                        return false;
                    if (low >= 0xDC00 && low <= 0xDFFF)
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                }
                AppendUtf8(out, cp);
                break;
            }
            default:
                Fail("invalid escape sequence");
                return false;
            }
        }
    }

    bool ParseArray(JsonValue& out)
    {
        Advance(); // '['
        std::vector<JsonValue> items;
        SkipWhitespace();
        if (Peek() == ']') {
            Advance();
            out = JsonValue::MakeArray(std::move(items));
            return true;
        }
        for (;;) {
            SkipWhitespace();
            JsonValue item;
            if (!ParseValue(item))
                return false;
            items.push_back(std::move(item));
            SkipWhitespace();
            if (Peek() == ',') {
                Advance();
                SkipWhitespace();
                if (Peek() == ']') { // tolerate trailing comma
                    Advance();
                    break;
                }
                continue;
            }
            if (Peek() == ']') {
                Advance();
                break;
            }
            Fail("expected ',' or ']' in array");
            return false;
        }
        out = JsonValue::MakeArray(std::move(items));
        return true;
    }

    bool ParseObject(JsonValue& out)
    {
        Advance(); // '{'
        std::map<std::string, JsonValue> members;
        SkipWhitespace();
        if (Peek() == '}') {
            Advance();
            out = JsonValue::MakeObject(std::move(members));
            return true;
        }
        for (;;) {
            SkipWhitespace();
            std::string key;
            if (!ParseString(key))
                return false;
            SkipWhitespace();
            if (Peek() != ':') {
                Fail("expected ':' after object key");
                return false;
            }
            Advance();
            SkipWhitespace();
            JsonValue value;
            if (!ParseValue(value))
                return false;
            members[key] = std::move(value);
            SkipWhitespace();
            if (Peek() == ',') {
                Advance();
                SkipWhitespace();
                if (Peek() == '}') { // tolerate trailing comma
                    Advance();
                    break;
                }
                continue;
            }
            if (Peek() == '}') {
                Advance();
                break;
            }
            Fail("expected ',' or '}' in object");
            return false;
        }
        out = JsonValue::MakeObject(std::move(members));
        return true;
    }
};

} // namespace

JsonValue JsonValue::MakeBool(bool v)
{
    JsonValue j;
    j.m_type = Type::Bool;
    j.m_bool = v;
    return j;
}

JsonValue JsonValue::MakeNumber(double v)
{
    JsonValue j;
    j.m_type = Type::Number;
    j.m_number = v;
    return j;
}

JsonValue JsonValue::MakeString(std::string v)
{
    JsonValue j;
    j.m_type = Type::String;
    j.m_string = std::move(v);
    return j;
}

JsonValue JsonValue::MakeArray(std::vector<JsonValue> v)
{
    JsonValue j;
    j.m_type = Type::Array;
    j.m_array = std::move(v);
    return j;
}

JsonValue JsonValue::MakeObject(std::map<std::string, JsonValue> v)
{
    JsonValue j;
    j.m_type = Type::Object;
    j.m_object = std::move(v);
    return j;
}

bool JsonValue::AsBool(bool fallback) const
{
    if (m_type == Type::Bool)
        return m_bool;
    if (m_type == Type::Number)
        return m_number != 0.0;
    return fallback;
}

double JsonValue::AsNumber(double fallback) const
{
    if (m_type == Type::Number)
        return std::isfinite(m_number) ? m_number : fallback;
    if (m_type == Type::Bool)
        return m_bool ? 1.0 : 0.0;
    if (m_type == Type::String) {
        char* end = nullptr;
        const double v = std::strtod(m_string.c_str(), &end);
        if (end && end != m_string.c_str() && std::isfinite(v))
            return v;
    }
    return fallback;
}

int JsonValue::AsInt(int fallback) const
{
    if (m_type == Type::String) {
        // Accept hex strings such as "0x1F".
        char* end = nullptr;
        errno = 0;
        const long long v = std::strtoll(m_string.c_str(), &end, 0);
        if (end && end != m_string.c_str() && errno != ERANGE &&
            v >= std::numeric_limits<int>::min() && v <= std::numeric_limits<int>::max())
            return static_cast<int>(v);
        return fallback;
    }
    if (m_type != Type::Number && m_type != Type::Bool)
        return fallback;
    const double d = AsNumber(static_cast<double>(fallback));
    if (!std::isfinite(d) || d < std::numeric_limits<int>::min() || d > std::numeric_limits<int>::max())
        return fallback;
    return static_cast<int>(std::lround(d));
}

const std::string& JsonValue::AsString() const
{
    return m_type == Type::String ? m_string : EmptyString();
}

std::string JsonValue::AsString(const std::string& fallback) const
{
    return m_type == Type::String ? m_string : fallback;
}

const std::vector<JsonValue>& JsonValue::AsArray() const
{
    return m_type == Type::Array ? m_array : EmptyArray();
}

const std::map<std::string, JsonValue>& JsonValue::AsObject() const
{
    return m_type == Type::Object ? m_object : EmptyObject();
}

const JsonValue& JsonValue::operator[](const std::string& key) const
{
    if (m_type != Type::Object)
        return NullValue();
    const auto it = m_object.find(key);
    return it == m_object.end() ? NullValue() : it->second;
}

bool JsonValue::Has(const std::string& key) const
{
    return m_type == Type::Object && m_object.find(key) != m_object.end();
}

const JsonValue& JsonValue::At(size_t index) const
{
    if (m_type != Type::Array || index >= m_array.size())
        return NullValue();
    return m_array[index];
}

size_t JsonValue::Size() const
{
    if (m_type == Type::Array)
        return m_array.size();
    if (m_type == Type::Object)
        return m_object.size();
    return 0;
}

JsonParseResult ParseJson(const std::string& text)
{
    Parser parser(text);
    return parser.Run();
}

JsonParseResult ParseJsonFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        JsonParseResult r;
        r.ok = false;
        r.error = "cannot open file: " + path;
        return r;
    }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size < 0 || size > (16 << 20)) {
        JsonParseResult result;
        result.error = "JSON file size is invalid: " + path;
        return result;
    }
    in.seekg(0);
    std::string text(static_cast<size_t>(size), '\0');
    if (size > 0 && !in.read(text.data(), size)) {
        JsonParseResult result;
        result.error = "cannot read file: " + path;
        return result;
    }
    return ParseJson(text);
}

} // namespace sa

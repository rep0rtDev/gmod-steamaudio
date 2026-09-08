// src/util/Json.h
//
// Minimal dependency-free JSON reader used for the external signature
// configuration file (steamaudio_signatures.json). Supports the full JSON
// grammar (objects, arrays, strings with escapes, numbers, true/false/null)
// plus // and /* */ comments for convenience.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace sa {

class JsonValue {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    JsonValue() = default;
    static JsonValue MakeNull() { return JsonValue(); }
    static JsonValue MakeBool(bool v);
    static JsonValue MakeNumber(double v);
    static JsonValue MakeString(std::string v);
    static JsonValue MakeArray(std::vector<JsonValue> v);
    static JsonValue MakeObject(std::map<std::string, JsonValue> v);

    Type GetType() const { return m_type; }
    bool IsNull() const { return m_type == Type::Null; }
    bool IsBool() const { return m_type == Type::Bool; }
    bool IsNumber() const { return m_type == Type::Number; }
    bool IsString() const { return m_type == Type::String; }
    bool IsArray() const { return m_type == Type::Array; }
    bool IsObject() const { return m_type == Type::Object; }

    bool AsBool(bool fallback = false) const;
    double AsNumber(double fallback = 0.0) const;
    int AsInt(int fallback = 0) const;
    const std::string& AsString() const;
    std::string AsString(const std::string& fallback) const;
    const std::vector<JsonValue>& AsArray() const;
    const std::map<std::string, JsonValue>& AsObject() const;

    // Object member lookup; returns a static null value when missing.
    const JsonValue& operator[](const std::string& key) const;
    const JsonValue& Get(const std::string& key) const { return (*this)[key]; }
    bool Has(const std::string& key) const;
    // Array element lookup; returns a static null value when out of range.
    const JsonValue& At(size_t index) const;
    size_t Size() const;

private:
    Type m_type = Type::Null;
    bool m_bool = false;
    double m_number = 0.0;
    std::string m_string;
    std::vector<JsonValue> m_array;
    std::map<std::string, JsonValue> m_object;
};

struct JsonParseResult {
    bool ok = false;
    JsonValue value;
    std::string error;
    size_t line = 0;
    size_t column = 0;
};

JsonParseResult ParseJson(const std::string& text);

// Convenience: reads the file and parses it. `result.ok` is false on I/O error.
JsonParseResult ParseJsonFile(const std::string& path);

} // namespace sa

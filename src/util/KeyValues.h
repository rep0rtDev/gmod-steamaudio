// src/util/KeyValues.h
//
// Parser for Valve KeyValues text (VMT materials, surfaceproperties.txt,
// manifests, gameinfo.txt). Produces a plain tree; keys compare
// case-insensitively as in the engine. Accepts `//` comments, quoted and bare
// tokens, `[$CONDITION]` suffixes (recorded and otherwise ignored) and the
// `#include` / `#base` directives (collected, not followed). Never throws and
// never reads outside the supplied range; malformed input yields as much of
// the tree as could be recovered.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace sa {

struct KvNode {
    std::string key;
    std::string value;     // leaf value; empty for blocks
    std::string condition; // conditional suffix without brackets ("$WIN32", "!$X360"), if any
    bool isBlock = false;
    std::vector<KvNode> children;

    // First child whose key matches case-insensitively, or nullptr.
    const KvNode* Find(const char* name) const;
    // Value of the first matching leaf child, or `fallback`.
    std::string Get(const char* name, const std::string& fallback) const;
    bool Has(const char* name) const { return Find(name) != nullptr; }
};

struct KvDocument {
    std::vector<KvNode> roots;
    std::vector<std::string> includes; // #include / #base targets in order
    size_t nodes = 0;
    bool truncated = false; // hit a node/depth cap or an unbalanced brace
    std::string error;
};

struct KvLimits {
    size_t maxNodes = 1u << 18;
    size_t maxDepth = 32;
    size_t maxTokenLength = 4096;
};

// Parses `size` bytes. Returns false only when nothing usable was produced.
bool ParseKeyValues(const char* data, size_t size, KvDocument& out, const KvLimits& limits = KvLimits{});
bool ParseKeyValues(const std::string& text, KvDocument& out, const KvLimits& limits = KvLimits{});

bool KvKeyEquals(const std::string& a, const char* b);
std::string KvLower(std::string s);

} // namespace sa

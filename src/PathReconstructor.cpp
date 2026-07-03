#include "PathReconstructor.h"

#include <unordered_set>

std::wstring PathReconstructor::Resolve(uint64_t startFrn, const std::unordered_map<uint64_t, UsnNode>& nodes,
                                         std::vector<std::wstring>* diagnostics) {
    auto cached = m_cache.find(startFrn);
    if (cached != m_cache.end()) {
        return cached->second;
    }

    // Walk upward collecting the chain of (frn, name) not yet cached, stopping at a cached
    // ancestor, the volume root (self-referencing parent), or a missing/cyclic parent.
    std::vector<uint64_t> chainFrns;
    std::vector<std::wstring> chainNames;
    std::unordered_set<uint64_t> visited;

    uint64_t current = startFrn;
    std::wstring basePath;
    bool corrupt = false;
    bool missingParent = false;

    const size_t kMaxDepth = 1024;

    for (;;) {
        auto cacheIt = m_cache.find(current);
        if (cacheIt != m_cache.end()) {
            basePath = cacheIt->second;
            break;
        }

        auto nodeIt = nodes.find(current);
        if (nodeIt == nodes.end()) {
            basePath = m_driveRoot;
            missingParent = true;
            break;
        }

        if (!visited.insert(current).second || chainFrns.size() > kMaxDepth) {
            basePath = m_driveRoot;
            corrupt = true;
            break;
        }

        const UsnNode& node = nodeIt->second;
        if (node.parentFrn == current) {
            // Self-referencing parent marks the volume root; it contributes no name segment.
            basePath = m_driveRoot;
            m_cache[current] = basePath;
            break;
        }

        chainFrns.push_back(current);
        chainNames.push_back(node.name);
        current = node.parentFrn;
    }

    std::wstring path = basePath;
    for (size_t i = chainFrns.size(); i-- > 0;) {
        if (!path.empty() && path.back() != L'\\') {
            path += L'\\';
        }
        path += chainNames[i];
        m_cache[chainFrns[i]] = path;
    }

    if (diagnostics != nullptr) {
        if (corrupt) {
            diagnostics->push_back(L"Detected a cyclic or overly deep parent chain; path may be incomplete.");
        } else if (missingParent) {
            diagnostics->push_back(L"Parent record missing from index; showing best-effort path.");
        }
    }

    return path;
}

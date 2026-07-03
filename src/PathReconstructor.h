#pragma once
// Reconstructs full paths from an NTFS file-reference-number parent chain, with memoized
// caching and safety against corrupt/cyclic data.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// One node of the file reference graph built from USN/MFT enumeration.
struct UsnNode {
    uint64_t parentFrn = 0;
    std::wstring name;
    uint32_t attributes = 0;
};

class PathReconstructor {
public:
    explicit PathReconstructor(std::wstring driveRoot) : m_driveRoot(std::move(driveRoot)) {}

    // Resolves the full path for frn by walking parent references in 'nodes'. Results (and all
    // ancestors visited along the way) are cached so repeated lookups are O(1) amortized.
    // On corrupt/cyclic/missing data, returns a best-effort partial path and appends a note to
    // 'diagnostics' if provided; never throws or loops forever.
    std::wstring Resolve(uint64_t frn, const std::unordered_map<uint64_t, UsnNode>& nodes,
                          std::vector<std::wstring>* diagnostics = nullptr);

    void Clear() { m_cache.clear(); }
    size_t CacheSize() const { return m_cache.size(); }

private:
    std::wstring m_driveRoot;
    std::unordered_map<uint64_t, std::wstring> m_cache;
};

// Scheme CD music/sound ripper
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/scheme-cd-ripper

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include "internal.h"

namespace cdrip::detail {
namespace {

constexpr int kMinMatchLen = 6;
constexpr double kMinMatchRatio = 0.6;
constexpr size_t kMinCandidateLen = 6;

struct TitleItem {
    std::string normalized;
};

std::string normalize_album_title(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    bool last_space = false;
    for (unsigned char ch : input) {
        if (ch < 0x80) {
            if (std::isalnum(ch)) {
                out.push_back(static_cast<char>(std::tolower(ch)));
                last_space = false;
            } else {
                if (!last_space) {
                    out.push_back(' ');
                    last_space = true;
                }
            }
        } else {
            out.push_back(static_cast<char>(ch));
            last_space = false;
        }
    }
    return trim(out);
}

int longest_common_substring_len(const std::string& a, const std::string& b) {
    if (a.empty() || b.empty()) return 0;
    std::vector<int> prev(b.size() + 1, 0);
    std::vector<int> curr(b.size() + 1, 0);
    int best = 0;
    for (size_t i = 1; i <= a.size(); ++i) {
        for (size_t j = 1; j <= b.size(); ++j) {
            if (a[i - 1] == b[j - 1]) {
                curr[j] = prev[j - 1] + 1;
                if (curr[j] > best) best = curr[j];
            } else {
                curr[j] = 0;
            }
        }
        std::swap(prev, curr);
    }
    return best;
}

bool is_similar_title(const std::string& a, const std::string& b) {
    const size_t min_len = std::min(a.size(), b.size());
    if (min_len == 0) return false;
    const int lcs = longest_common_substring_len(a, b);
    if (lcs < kMinMatchLen) return false;
    const double ratio = static_cast<double>(lcs) / static_cast<double>(min_len);
    return ratio >= kMinMatchRatio;
}

std::string longest_common_substring_all(const std::vector<std::string>& values) {
    if (values.empty()) return {};
    if (values.size() == 1) return values.front();
    size_t base_index = 0;
    for (size_t i = 1; i < values.size(); ++i) {
        if (values[i].size() < values[base_index].size()) {
            base_index = i;
        }
    }
    const std::string& base = values[base_index];
    if (base.empty()) return {};
    for (size_t len = base.size(); len > 0; --len) {
        for (size_t start = 0; start + len <= base.size(); ++start) {
            std::string sub = base.substr(start, len);
            const std::string trimmed = trim(sub);
            if (trimmed.empty()) continue;
            bool found_in_all = true;
            for (size_t i = 0; i < values.size(); ++i) {
                if (i == base_index) continue;
                if (values[i].find(sub) == std::string::npos) {
                    found_in_all = false;
                    break;
                }
            }
            if (found_in_all) return trimmed;
        }
    }
    return {};
}

class DisjointSet {
public:
    explicit DisjointSet(size_t n) : parent_(n), rank_(n, 0) {
        for (size_t i = 0; i < n; ++i) parent_[i] = static_cast<int>(i);
    }

    int find(int x) {
        if (parent_[x] != x) parent_[x] = find(parent_[x]);
        return parent_[x];
    }

    void unite(int a, int b) {
        a = find(a);
        b = find(b);
        if (a == b) return;
        if (rank_[a] < rank_[b]) std::swap(a, b);
        parent_[b] = a;
        if (rank_[a] == rank_[b]) ++rank_[a];
    }

private:
    std::vector<int> parent_;
    std::vector<int> rank_;
};

}  // namespace

std::vector<std::string> extract_album_title_candidates(
    const std::vector<const CdRipCddbEntry*>& entries) {

    std::vector<TitleItem> items;
    items.reserve(entries.size());
    for (const auto* entry : entries) {
        if (!entry) continue;
        const std::string title = trim(album_tag(entry, "ALBUM"));
        if (title.empty()) continue;
        const std::string normalized = normalize_album_title(title);
        if (normalized.empty()) continue;
        items.push_back({normalized});
    }

    if (items.empty()) return {};

    DisjointSet dsu(items.size());
    for (size_t i = 0; i < items.size(); ++i) {
        for (size_t j = i + 1; j < items.size(); ++j) {
            if (is_similar_title(items[i].normalized, items[j].normalized)) {
                dsu.unite(static_cast<int>(i), static_cast<int>(j));
            }
        }
    }

    std::vector<std::vector<size_t>> groups(items.size());
    for (size_t i = 0; i < items.size(); ++i) {
        const int root = dsu.find(static_cast<int>(i));
        groups[static_cast<size_t>(root)].push_back(i);
    }

    std::vector<std::string> candidates;
    for (const auto& group : groups) {
        if (group.empty()) continue;
        std::vector<std::string> normalized;
        normalized.reserve(group.size());
        for (size_t idx : group) {
            normalized.push_back(items[idx].normalized);
        }
        std::string candidate = trim(longest_common_substring_all(normalized));
        if (candidate.size() < kMinCandidateLen) continue;
        candidates.push_back(candidate);
    }

    std::sort(candidates.begin(), candidates.end(),
        [](const std::string& a, const std::string& b) {
            if (a.size() != b.size()) return a.size() > b.size();
            return a < b;
        });

    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
    return candidates;
}

}  // namespace cdrip::detail

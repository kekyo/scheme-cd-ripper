// Scheme CD music/sound ripper
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/scheme-cd-ripper

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "internal.h"

namespace cdrip::detail {
namespace {

constexpr int kMinMatchLen = 6;
constexpr double kMinMatchRatio = 0.6;
constexpr size_t kMinCandidateLen = 6;

struct TitleItem {
    std::string normalized;
    std::vector<std::string> tokens;
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

std::vector<std::string> split_tokens(const std::string& normalized) {
    std::vector<std::string> tokens;
    size_t pos = 0;
    while (pos < normalized.size()) {
        while (pos < normalized.size() && normalized[pos] == ' ') ++pos;
        if (pos >= normalized.size()) break;
        size_t end = pos;
        while (end < normalized.size() && normalized[end] != ' ') ++end;
        if (end > pos) {
            tokens.push_back(normalized.substr(pos, end - pos));
        }
        pos = end;
    }
    return tokens;
}

bool is_stopword(const std::string& token) {
    static const std::unordered_set<std::string> kStopwords = {
        "a", "an", "and", "are", "at", "best", "by", "cd", "collection", "compilation",
        "complete", "disc", "discs", "edition", "for", "from", "greatest", "history",
        "hits", "in", "live", "mix", "of", "on", "or", "part", "pt", "remix", "series",
        "selection", "set", "side", "sides", "special", "the", "to", "version", "versions",
        "vol", "volume", "vols", "volumes", "with", "without"
    };
    return kStopwords.find(token) != kStopwords.end();
}

bool is_numeric_token(const std::string& token) {
    if (token.empty()) return false;
    for (unsigned char ch : token) {
        if (std::isdigit(ch)) return true;
    }
    static const std::unordered_set<std::string> kRoman = {
        "i", "ii", "iii", "iv", "v", "vi", "vii", "viii", "ix", "x",
        "xi", "xii", "xiii", "xiv", "xv", "xvi", "xvii", "xviii", "xix", "xx"
    };
    return kRoman.find(token) != kRoman.end();
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
        items.push_back({normalized, split_tokens(normalized)});
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
        std::unordered_map<std::string, size_t> freq;
        freq.reserve(group.size() * 4);
        for (size_t idx : group) {
            std::unordered_set<std::string> seen;
            for (const auto& token : items[idx].tokens) {
                if (token.empty()) continue;
                if (seen.insert(token).second) {
                    ++freq[token];
                }
            }
        }

        const size_t common_threshold = (group.size() * 3 + 4) / 5;
        std::unordered_set<std::string> common_tokens;
        common_tokens.reserve(freq.size());
        for (const auto& it : freq) {
            if (it.second >= common_threshold) {
                common_tokens.insert(it.first);
            }
        }

        double best_score = -1e9;
        size_t best_index = group.front();
        size_t best_numeric = 0;
        size_t best_length = 0;

        for (size_t idx : group) {
            std::unordered_set<std::string> unique_tokens;
            for (const auto& token : items[idx].tokens) {
                if (!token.empty()) unique_tokens.insert(token);
            }
            size_t specific_tokens = 0;
            size_t numeric_tokens = 0;
            size_t stop_tokens = 0;
            for (const auto& token : unique_tokens) {
                const bool stop = is_stopword(token);
                if (stop) {
                    ++stop_tokens;
                }
                if (!stop && common_tokens.find(token) == common_tokens.end()) {
                    ++specific_tokens;
                }
                if (is_numeric_token(token)) {
                    ++numeric_tokens;
                }
            }
            const double score =
                static_cast<double>(specific_tokens) * 10.0 +
                static_cast<double>(numeric_tokens) * 2.0 -
                static_cast<double>(stop_tokens) * 3.0 +
                static_cast<double>(items[idx].normalized.size()) * 0.05;
            const size_t length = items[idx].normalized.size();
            if (score > best_score + 1e-6 ||
                (std::fabs(score - best_score) <= 1e-6 &&
                 (numeric_tokens > best_numeric ||
                  (numeric_tokens == best_numeric && length > best_length)))) {
                best_score = score;
                best_index = idx;
                best_numeric = numeric_tokens;
                best_length = length;
            }
        }

        const std::string candidate = trim(items[best_index].normalized);
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

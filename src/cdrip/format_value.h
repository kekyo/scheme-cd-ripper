#pragma once

// Scheme CD music/sound ripper
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/scheme-cd-ripper

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace cdrip::detail {

inline std::string format_truncate_on_newline(const std::string& s) {
    const size_t pos_ctrl = s.find_first_of("\r\n");
    const size_t pos_lit_n = s.find("\\n");
    const size_t pos_lit_r = s.find("\\r");

    size_t pos = std::string::npos;

    if (pos_ctrl != std::string::npos) pos = pos_ctrl;
    if (pos_lit_n != std::string::npos) pos = (pos == std::string::npos) ? pos_lit_n : std::min(pos, pos_lit_n);
    if (pos_lit_r != std::string::npos) pos = (pos == std::string::npos) ? pos_lit_r : std::min(pos, pos_lit_r);
    if (pos == std::string::npos) return s;
    return s.substr(0, pos);
}

inline bool format_char_in_set(const char* chars, char ch) {
    for (const char* p = chars; *p != '\0'; ++p) {
        if (*p == ch) return true;
    }
    return false;
}

inline std::string format_safe_string(const std::string& s) {
    constexpr const char kTrailingTrimChars[] = ".,;|~/\\^";
    constexpr const char kReplaceChars[] = ".:;|/\\^";

    std::string out = format_truncate_on_newline(s);
    while (!out.empty() && format_char_in_set(kTrailingTrimChars, out.back())) {
        out.pop_back();
    }
    for (char& ch : out) {
        if (format_char_in_set(kReplaceChars, ch)) {
            ch = '_';
        }
    }
    return out;
}

class Formattable {
public:
    virtual ~Formattable() = default;
    virtual std::string toString(const std::string& format) const = 0;
};

class StringValue : public Formattable {
public:
    explicit StringValue(std::string value)
        : value_(std::move(value)) {}

    std::string toString(const std::string& format) const override {
        if (format == "n") {
            return format_safe_string(value_);
        }
        return value_;
    }

private:
    std::string value_;
};

class NumericValue : public Formattable {
public:
    NumericValue(int value, std::string raw)
        : value_(value), raw_(std::move(raw)) {}

    std::string toString(const std::string& format) const override {
        if (!format.empty() && format.back() == 'd') {
            std::string width_text = format.substr(0, format.size() - 1);
            try {
                int width = std::stoi(width_text);
                if (width > 0) {
                    std::ostringstream oss;
                    oss << std::setw(width) << std::setfill('0') << value_;
                    return oss.str();
                }
            } catch (...) {
                // Fall through to raw value.
            }
        }
        return raw_;
    }

private:
    int value_;
    std::string raw_;
};

using FormatTagMap = std::map<std::string, std::unique_ptr<Formattable>>;

struct FormatSegment {
    std::string key;
    std::string format;
};

enum class FormatOperator {
    kJoinPath,
    kJoinSpace,
};

struct FormatExpression {
    std::vector<FormatSegment> segments;
    std::vector<FormatOperator> operators;
};

inline bool is_format_operator(char ch) {
    return ch == '/' || ch == '+';
}

inline FormatOperator format_operator_from_char(char ch) {
    switch (ch) {
    case '+':
        return FormatOperator::kJoinSpace;
    default:
        return FormatOperator::kJoinPath;
    }
}

inline std::string format_key_upper(const std::string& s) {
    std::string r;
    r.reserve(s.size());
    for (unsigned char c : s) {
        r.push_back(static_cast<char>(std::toupper(c)));
    }
    return r;
}

inline FormatSegment parse_format_segment(const std::string& token) {
    FormatSegment segment;
    auto colon = token.find(':');
    if (colon != std::string::npos) {
        segment.key = format_key_upper(token.substr(0, colon));
        segment.format = token.substr(colon + 1);
    } else {
        segment.key = format_key_upper(token);
    }
    return segment;
}

inline FormatExpression parse_format_expression(const std::string& token) {
    FormatExpression expr;
    size_t start = 0;
    for (size_t i = 0; i < token.size(); ++i) {
        if (is_format_operator(token[i])) {
            expr.segments.push_back(parse_format_segment(token.substr(start, i - start)));
            expr.operators.push_back(format_operator_from_char(token[i]));
            start = i + 1;
        }
    }
    expr.segments.push_back(parse_format_segment(token.substr(start)));
    return expr;
}

inline std::string format_token_expression(
    const FormatExpression& expr,
    const FormatTagMap& tags) {

    std::string out;
    bool has_output = false;
    for (size_t i = 0; i < expr.segments.size(); ++i) {
        const auto& segment = expr.segments[i];
        std::string value;
        if (!segment.key.empty()) {
            auto it = tags.find(segment.key);
            if (it != tags.end() && it->second) {
                value = it->second->toString(segment.format);
            }
        }
        if (value.empty()) continue;
        if (has_output && i > 0) {
            FormatOperator op = expr.operators[i - 1];
            switch (op) {
            case FormatOperator::kJoinPath:
                out += "/";
                break;
            case FormatOperator::kJoinSpace:
                out += " ";
                break;
            }
        }
        out += value;
        has_output = true;
    }
    return out;
}

}  // namespace cdrip::detail

#pragma once

// Scheme CD music/sound ripper
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/scheme-cd-ripper

#include <iomanip>
#include <sstream>
#include <string>
#include <utility>

namespace cdrip::detail {

class Formattable {
public:
    virtual ~Formattable() = default;
    virtual std::string toString(const std::string& format) const = 0;
};

class StringValue : public Formattable {
public:
    explicit StringValue(std::string value)
        : value_(std::move(value)) {}

    std::string toString(const std::string& /*format*/) const override {
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

}  // namespace cdrip::detail

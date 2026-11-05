// TorqueScript lexer (plan §1.2 Track B, W1.6). See lexer.hpp for the token set.
#include "fuse/remaster/tscript/lexer.hpp"

#include <cstdio>

namespace fuse::remaster::tscript {
namespace {

bool is_alpha(char c) noexcept { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }
bool is_alnum(char c) noexcept { return is_alpha(c) || is_digit(c); }
bool is_hex(char c) noexcept { return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

// Longest first.
constexpr std::string_view kOperators[] = {
    "!$=", "<<=", ">>=", "-->", "$=", "==", "!=", "<=", ">=", "&&", "||", "<<", ">>", "++", "--", "+=", "-=",
    "*=",  "/=",  "%=",  "&=",  "|=", "^=", "->", "::", "+",  "-",  "*",  "/",  "%",  "!",  "~",  "&",
    "|",   "^",   "<",   ">",   "=",  "?",  ":",  ";",  ",",  ".",  "(",  ")",  "[",  "]",  "{",  "}",
};

class Lexer {
public:
    Lexer(std::string_view src, std::string_view file, std::vector<Diagnostic>& diags)
        : src_(src), file_(file), diags_(diags) {}

    std::vector<Token> run() {
        std::vector<Token> out;
        out.reserve(src_.size() / 4 + 16);
        if (src_.starts_with("\xEF\xBB\xBF")) {
            pos_ = 3;
            col_ = 1;
        }
        for (;;) {
            skip_space_and_comments();
            if (pos_ >= src_.size()) break;
            if (auto t = next(); t.kind != TokKind::End) out.push_back(t);
        }
        Token end;
        end.kind = TokKind::End;
        end.line = line_;
        end.col = col_;
        end.begin = end.end = static_cast<std::uint32_t>(pos_);
        out.push_back(end);
        return out;
    }

private:
    [[nodiscard]] char peek(std::size_t ahead = 0) const noexcept {
        return pos_ + ahead < src_.size() ? src_[pos_ + ahead] : '\0';
    }
    void advance(std::size_t n = 1) noexcept {
        for (std::size_t i = 0; i < n && pos_ < src_.size(); ++i) {
            if (src_[pos_] == '\n') {
                ++line_;
                col_ = 1;
            } else {
                ++col_;
            }
            ++pos_;
        }
    }
    void error(std::uint32_t line, std::uint32_t col, std::string msg) {
        diags_.push_back(Diagnostic{std::string(file_), line, col, std::move(msg)});
    }

    void skip_space_and_comments() {
        while (pos_ < src_.size()) {
            const char c = src_[pos_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v') {
                advance();
            } else if (c == '/' && peek(1) == '/') {
                while (pos_ < src_.size() && src_[pos_] != '\n') advance();
            } else if (c == '/' && peek(1) == '*') {
                const auto l = line_, co = col_;
                advance(2);
                while (pos_ < src_.size() && !(src_[pos_] == '*' && peek(1) == '/')) advance();
                if (pos_ >= src_.size()) {
                    error(l, co, "unterminated /* comment");
                    return;
                }
                advance(2);
            } else {
                return;
            }
        }
    }

    Token make(TokKind k, std::size_t begin, std::uint32_t line, std::uint32_t col) const {
        Token t;
        t.kind = k;
        t.line = line;
        t.col = col;
        t.begin = static_cast<std::uint32_t>(begin);
        t.end = static_cast<std::uint32_t>(pos_);
        t.text = src_.substr(begin, pos_ - begin);
        return t;
    }

    // [A-Za-z_][A-Za-z0-9_]* ( '::' [A-Za-z_][A-Za-z0-9_]* )*
    void scan_qualified_name() {
        while (is_alnum(peek())) advance();
        while (peek() == ':' && peek(1) == ':' && is_alpha(peek(2))) {
            advance(2);
            while (is_alnum(peek())) advance();
        }
    }

    Token next() {
        const std::size_t begin = pos_;
        const auto line = line_, col = col_;
        const char c = src_[pos_];

        if (is_alpha(c)) {
            scan_qualified_name();
            const auto word = src_.substr(begin, pos_ - begin);
            // foreach$ / switch$ (a '$' not starting a global variable)
            if ((word == "foreach" || word == "switch") && peek() == '$' && !is_alpha(peek(1))) advance();
            return make(TokKind::Ident, begin, line, col);
        }
        if ((c == '%' || c == '$') && is_alpha(peek(1))) {
            advance();
            scan_qualified_name();
            return make(c == '%' ? TokKind::LocalVar : TokKind::GlobalVar, begin, line, col);
        }
        if (is_digit(c) || (c == '.' && is_digit(peek(1)))) return number(begin, line, col);
        if (c == '"' || c == '\'') return string(c, begin, line, col);

        for (const auto op : kOperators) {
            if (src_.substr(pos_).starts_with(op)) {
                advance(op.size());
                return make(TokKind::Punct, begin, line, col);
            }
        }
        if (c == '@') {
            advance();
            return make(TokKind::Punct, begin, line, col);
        }
        const auto uc = static_cast<unsigned char>(c);
        char buf[64];
        if (uc >= 0x20 && uc < 0x7F) {
            std::snprintf(buf, sizeof buf, "unexpected character '%c'", c);
        } else {
            std::snprintf(buf, sizeof buf, "unexpected byte 0x%02X", static_cast<unsigned>(uc));
        }
        error(line, col, buf);
        advance();
        return Token{};
    }

    Token number(std::size_t begin, std::uint32_t line, std::uint32_t col) {
        if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X') && is_hex(peek(2))) {
            advance(2);
            while (is_hex(peek())) advance();
            return make(TokKind::Int, begin, line, col);
        }
        bool is_float = false;
        while (is_digit(peek())) advance();
        if (peek() == '.' && is_digit(peek(1))) {
            is_float = true;
            advance();
            while (is_digit(peek())) advance();
        }
        if ((peek() == 'e' || peek() == 'E') &&
            (is_digit(peek(1)) || ((peek(1) == '+' || peek(1) == '-') && is_digit(peek(2))))) {
            is_float = true;
            advance(2);
            while (is_digit(peek())) advance();
        }
        return make(is_float ? TokKind::Float : TokKind::Int, begin, line, col);
    }

    Token string(char quote, std::size_t begin, std::uint32_t line, std::uint32_t col) {
        advance(); // opening quote
        const std::size_t body = pos_;
        while (pos_ < src_.size() && src_[pos_] != quote) {
            if (src_[pos_] == '\\' && pos_ + 1 < src_.size()) {
                advance(2);
            } else if (src_[pos_] == '\n') {
                break;
            } else {
                advance();
            }
        }
        const std::size_t body_end = pos_;
        if (pos_ >= src_.size() || src_[pos_] != quote) {
            error(line, col, quote == '"' ? "unterminated string" : "unterminated tagged string");
        } else {
            advance(); // closing quote
        }
        Token t = make(quote == '"' ? TokKind::String : TokKind::Tagged, begin, line, col);
        t.text = src_.substr(body, body_end - body);
        return t;
    }

    std::string_view src_;
    std::string_view file_;
    std::vector<Diagnostic>& diags_;
    std::size_t pos_ = 0;
    std::uint32_t line_ = 1;
    std::uint32_t col_ = 1;
};

} // namespace

std::vector<Token> lex(std::string_view source, std::string_view file, std::vector<Diagnostic>& diags) {
    return Lexer(source, file, diags).run();
}

} // namespace fuse::remaster::tscript

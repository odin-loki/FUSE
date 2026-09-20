#include <fuse/compat/ts.hpp>

#include <cctype>
#include <cmath>
#include <fstream>
#include <ios>
#include <iterator>
#include <locale>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fuse::compat {
namespace {

enum class TokenKind {
    Eof,
    Number,
    String,
    Ident,
    Local,
    Global,
    Eq,
    EqEq,
    BangEq,
    Lt,
    Gt,
    Plus,
    Minus,
    Star,
    Slash,
    LParen,
    RParen,
    LBrace,
    RBrace,
    Comma,
    Semicolon,
    KwFunction,
    KwIf,
    KwElse,
    KwReturn,
};

struct Token {
    TokenKind kind = TokenKind::Eof;
    std::string text;
    double number = 0;
    int line = 1;
    int column = 1;
};

struct ScriptError {
    std::string message;
    int line = 1;
    int column = 1;
};

[[nodiscard]] const char* tokenKindName(TokenKind kind) {
    switch (kind) {
        case TokenKind::Eof:
            return "end of file";
        case TokenKind::Number:
            return "number";
        case TokenKind::String:
            return "string";
        case TokenKind::Ident:
            return "identifier";
        case TokenKind::Local:
            return "local";
        case TokenKind::Global:
            return "global";
        case TokenKind::Eq:
            return "'='";
        case TokenKind::EqEq:
            return "'=='";
        case TokenKind::BangEq:
            return "'!='";
        case TokenKind::Lt:
            return "'<'";
        case TokenKind::Gt:
            return "'>'";
        case TokenKind::Plus:
            return "'+'";
        case TokenKind::Minus:
            return "'-'";
        case TokenKind::Star:
            return "'*'";
        case TokenKind::Slash:
            return "'/'";
        case TokenKind::LParen:
            return "'('";
        case TokenKind::RParen:
            return "')'";
        case TokenKind::LBrace:
            return "'{'";
        case TokenKind::RBrace:
            return "'}'";
        case TokenKind::Comma:
            return "','";
        case TokenKind::Semicolon:
            return "';'";
        case TokenKind::KwFunction:
            return "'function'";
        case TokenKind::KwIf:
            return "'if'";
        case TokenKind::KwElse:
            return "'else'";
        case TokenKind::KwReturn:
            return "'return'";
    }
    return "token";
}

class Lexer {
public:
    explicit Lexer(std::string_view source) : m_source(source) {}

    Token next() {
        skipTrivia();
        m_startLine = m_line;
        m_startColumn = m_column;
        if (m_pos >= m_source.size()) {
            return make(TokenKind::Eof);
        }

        const char c = peek();
        if (c == '%') {
            return lexPrefixed(TokenKind::Local, '%');
        }
        if (c == '$') {
            return lexPrefixed(TokenKind::Global, '$');
        }
        if (c == '"') {
            return lexString();
        }
        if (std::isdigit(static_cast<unsigned char>(c)) ||
            (c == '.' && m_pos + 1 < m_source.size() &&
             std::isdigit(static_cast<unsigned char>(m_source[m_pos + 1])))) {
            return lexNumber();
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            return lexIdent();
        }

        advance();
        switch (c) {
            case '=':
                if (match('=')) {
                    return make(TokenKind::EqEq);
                }
                return make(TokenKind::Eq);
            case '!':
                if (match('=')) {
                    return make(TokenKind::BangEq);
                }
                error("unexpected '!'");
            case '<':
                return make(TokenKind::Lt);
            case '>':
                return make(TokenKind::Gt);
            case '+':
                return make(TokenKind::Plus);
            case '-':
                return make(TokenKind::Minus);
            case '*':
                return make(TokenKind::Star);
            case '/':
                return make(TokenKind::Slash);
            case '(':
                return make(TokenKind::LParen);
            case ')':
                return make(TokenKind::RParen);
            case '{':
                return make(TokenKind::LBrace);
            case '}':
                return make(TokenKind::RBrace);
            case ',':
                return make(TokenKind::Comma);
            case ';':
                return make(TokenKind::Semicolon);
            default:
                error(std::string("unexpected character '") + c + "'");
        }
    }

private:
    [[nodiscard]] char peek() const {
        return m_pos < m_source.size() ? m_source[m_pos] : '\0';
    }

    char advance() {
        const char c = m_source[m_pos++];
        if (c == '\n') {
            ++m_line;
            m_column = 1;
        } else {
            ++m_column;
        }
        return c;
    }

    bool match(char expected) {
        if (peek() != expected) {
            return false;
        }
        advance();
        return true;
    }

    void skipTrivia() {
        while (m_pos < m_source.size()) {
            const char c = peek();
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                advance();
                continue;
            }
            if (c == '/' && m_pos + 1 < m_source.size()) {
                const char next = m_source[m_pos + 1];
                if (next == '/') {
                    advance();
                    advance();
                    while (m_pos < m_source.size() && peek() != '\n') {
                        advance();
                    }
                    continue;
                }
                if (next == '*') {
                    const int commentLine = m_line;
                    const int commentColumn = m_column;
                    advance();
                    advance();
                    bool closed = false;
                    while (m_pos < m_source.size()) {
                        if (peek() == '*' && m_pos + 1 < m_source.size() &&
                            m_source[m_pos + 1] == '/') {
                            advance();
                            advance();
                            closed = true;
                            break;
                        }
                        advance();
                    }
                    if (!closed) {
                        throw ScriptError{"unterminated block comment", commentLine, commentColumn};
                    }
                    continue;
                }
            }
            break;
        }
    }

    Token lexPrefixed(TokenKind kind, char prefix) {
        advance();
        if (!(std::isalpha(static_cast<unsigned char>(peek())) || peek() == '_')) {
            error(std::string("expected identifier after '") + prefix + "'");
        }
        std::string name(1, prefix);
        name.push_back(advance());
        while (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_') {
            name.push_back(advance());
        }
        Token token = make(kind);
        token.text = std::move(name);
        return token;
    }

    Token lexIdent() {
        std::string name;
        name.push_back(advance());
        while (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_') {
            name.push_back(advance());
        }
        Token token = make(TokenKind::Ident);
        token.text = name;
        if (name == "function") {
            token.kind = TokenKind::KwFunction;
        } else if (name == "if") {
            token.kind = TokenKind::KwIf;
        } else if (name == "else") {
            token.kind = TokenKind::KwElse;
        } else if (name == "return") {
            token.kind = TokenKind::KwReturn;
        }
        return token;
    }

    Token lexNumber() {
        std::string text;
        while (std::isdigit(static_cast<unsigned char>(peek()))) {
            text.push_back(advance());
        }
        if (peek() == '.') {
            text.push_back(advance());
            while (std::isdigit(static_cast<unsigned char>(peek()))) {
                text.push_back(advance());
            }
        }
        if (peek() == 'e' || peek() == 'E') {
            text.push_back(advance());
            if (peek() == '+' || peek() == '-') {
                text.push_back(advance());
            }
            if (!std::isdigit(static_cast<unsigned char>(peek()))) {
                error("expected exponent digits");
            }
            while (std::isdigit(static_cast<unsigned char>(peek()))) {
                text.push_back(advance());
            }
        }
        Token token = make(TokenKind::Number);
        token.text = text;
        try {
            token.number = std::stod(text);
        } catch (const std::exception&) {
            error("invalid number literal");
        }
        return token;
    }

    Token lexString() {
        const int line = m_line;
        const int column = m_column;
        advance();
        std::string text;
        while (m_pos < m_source.size() && peek() != '"') {
            if (peek() == '\n') {
                throw ScriptError{"unterminated string", line, column};
            }
            if (peek() == '\\') {
                advance();
                if (m_pos >= m_source.size()) {
                    throw ScriptError{"unterminated string", line, column};
                }
                const char esc = advance();
                switch (esc) {
                    case 'n':
                        text.push_back('\n');
                        break;
                    case 't':
                        text.push_back('\t');
                        break;
                    case 'r':
                        text.push_back('\r');
                        break;
                    case '\\':
                        text.push_back('\\');
                        break;
                    case '"':
                        text.push_back('"');
                        break;
                    default:
                        text.push_back(esc);
                        break;
                }
                continue;
            }
            text.push_back(advance());
        }
        if (peek() != '"') {
            throw ScriptError{"unterminated string", line, column};
        }
        advance();
        Token token = make(TokenKind::String);
        token.text = std::move(text);
        return token;
    }

    Token make(TokenKind kind) const {
        Token token;
        token.kind = kind;
        token.line = m_startLine;
        token.column = m_startColumn;
        return token;
    }

    [[noreturn]] void error(const std::string& message) const {
        throw ScriptError{message, m_startLine, m_startColumn};
    }

    std::string_view m_source;
    std::size_t m_pos = 0;
    int m_line = 1;
    int m_column = 1;
    int m_startLine = 1;
    int m_startColumn = 1;
};

enum class ExprKind {
    Number,
    String,
    Var,
    Assign,
    Binary,
    Unary,
    Call,
};

struct Expr {
    ExprKind kind = ExprKind::Number;
    int line = 1;
    int column = 1;
    double number = 0;
    std::string text;
    TokenKind op = TokenKind::Eof;
    std::unique_ptr<Expr> left;
    std::unique_ptr<Expr> right;
    std::vector<std::unique_ptr<Expr>> args;
};

enum class StmtKind {
    Expr,
    If,
    Block,
    Function,
    Return,
};

struct Stmt {
    StmtKind kind = StmtKind::Expr;
    int line = 1;
    int column = 1;
    std::unique_ptr<Expr> expr;
    std::unique_ptr<Stmt> thenBranch;
    std::unique_ptr<Stmt> elseBranch;
    std::string name;
    std::vector<std::string> params;
    std::vector<std::unique_ptr<Stmt>> stmts;
};

[[nodiscard]] std::unique_ptr<Expr> makeExpr(ExprKind kind, const Token& at) {
    auto expr = std::make_unique<Expr>();
    expr->kind = kind;
    expr->line = at.line;
    expr->column = at.column;
    return expr;
}

[[nodiscard]] std::unique_ptr<Stmt> makeStmt(StmtKind kind, const Token& at) {
    auto stmt = std::make_unique<Stmt>();
    stmt->kind = kind;
    stmt->line = at.line;
    stmt->column = at.column;
    return stmt;
}

class Parser {
public:
    explicit Parser(std::string_view source) : m_lexer(source) {
        m_current = m_lexer.next();
    }

    std::vector<std::unique_ptr<Stmt>> parseProgram() {
        std::vector<std::unique_ptr<Stmt>> stmts;
        while (!check(TokenKind::Eof)) {
            stmts.push_back(parseStatement());
        }
        return stmts;
    }

private:
    [[nodiscard]] bool check(TokenKind kind) const {
        return m_current.kind == kind;
    }

    Token advance() {
        Token previous = m_current;
        if (m_current.kind != TokenKind::Eof) {
            m_current = m_lexer.next();
        }
        return previous;
    }

    bool match(TokenKind kind) {
        if (!check(kind)) {
            return false;
        }
        advance();
        return true;
    }

    Token consume(TokenKind kind, const char* message) {
        if (check(kind)) {
            return advance();
        }
        error(m_current, message);
    }

    [[noreturn]] void error(const Token& token, const std::string& message) const {
        throw ScriptError{message + " (got " + tokenKindName(token.kind) + ")", token.line, token.column};
    }

    std::unique_ptr<Stmt> parseStatement() {
        if (check(TokenKind::KwFunction)) {
            return parseFunction();
        }
        if (check(TokenKind::KwIf)) {
            return parseIf();
        }
        if (check(TokenKind::KwReturn)) {
            return parseReturn();
        }
        if (check(TokenKind::LBrace)) {
            return parseBlock();
        }
        auto stmt = makeStmt(StmtKind::Expr, m_current);
        stmt->expr = parseExpression();
        consume(TokenKind::Semicolon, "expected ';' after expression");
        return stmt;
    }

    std::unique_ptr<Stmt> parseFunction() {
        const Token kw = consume(TokenKind::KwFunction, "expected 'function'");
        if (!check(TokenKind::Ident)) {
            error(m_current, "expected function name");
        }
        const Token name = advance();
        consume(TokenKind::LParen, "expected '(' after function name");
        std::vector<std::string> params;
        if (!check(TokenKind::RParen)) {
            do {
                if (!(check(TokenKind::Local) || check(TokenKind::Global) || check(TokenKind::Ident))) {
                    error(m_current, "expected parameter name");
                }
                params.push_back(advance().text);
            } while (match(TokenKind::Comma));
        }
        consume(TokenKind::RParen, "expected ')' after parameter list");
        auto stmt = makeStmt(StmtKind::Function, kw);
        stmt->name = name.text;
        stmt->params = std::move(params);
        stmt->thenBranch = parseBlock();
        return stmt;
    }

    std::unique_ptr<Stmt> parseIf() {
        const Token kw = consume(TokenKind::KwIf, "expected 'if'");
        consume(TokenKind::LParen, "expected '(' after 'if'");
        auto cond = parseExpression();
        consume(TokenKind::RParen, "expected ')' after condition");
        auto stmt = makeStmt(StmtKind::If, kw);
        stmt->expr = std::move(cond);
        stmt->thenBranch = parseBlock();
        if (match(TokenKind::KwElse)) {
            stmt->elseBranch = parseBlock();
        }
        return stmt;
    }

    std::unique_ptr<Stmt> parseReturn() {
        const Token kw = consume(TokenKind::KwReturn, "expected 'return'");
        auto stmt = makeStmt(StmtKind::Return, kw);
        if (!check(TokenKind::Semicolon)) {
            stmt->expr = parseExpression();
        }
        consume(TokenKind::Semicolon, "expected ';' after return");
        return stmt;
    }

    std::unique_ptr<Stmt> parseBlock() {
        const Token open = consume(TokenKind::LBrace, "expected '{'");
        auto stmt = makeStmt(StmtKind::Block, open);
        while (!check(TokenKind::RBrace) && !check(TokenKind::Eof)) {
            stmt->stmts.push_back(parseStatement());
        }
        consume(TokenKind::RBrace, "expected '}' after block");
        return stmt;
    }

    std::unique_ptr<Expr> parseExpression() {
        return parseAssignment();
    }

    std::unique_ptr<Expr> parseAssignment() {
        auto expr = parseComparison();
        if (check(TokenKind::Eq)) {
            const Token eq = advance();
            if (expr->kind != ExprKind::Var) {
                error(eq, "assignment target must be a variable");
            }
            auto assign = makeExpr(ExprKind::Assign, eq);
            assign->text = expr->text;
            assign->right = parseAssignment();
            return assign;
        }
        return expr;
    }

    std::unique_ptr<Expr> parseComparison() {
        auto expr = parseTerm();
        while (check(TokenKind::EqEq) || check(TokenKind::BangEq) || check(TokenKind::Lt) ||
               check(TokenKind::Gt)) {
            const Token op = advance();
            auto binary = makeExpr(ExprKind::Binary, op);
            binary->op = op.kind;
            binary->left = std::move(expr);
            binary->right = parseTerm();
            expr = std::move(binary);
        }
        return expr;
    }

    std::unique_ptr<Expr> parseTerm() {
        auto expr = parseFactor();
        while (check(TokenKind::Plus) || check(TokenKind::Minus)) {
            const Token op = advance();
            auto binary = makeExpr(ExprKind::Binary, op);
            binary->op = op.kind;
            binary->left = std::move(expr);
            binary->right = parseFactor();
            expr = std::move(binary);
        }
        return expr;
    }

    std::unique_ptr<Expr> parseFactor() {
        auto expr = parseUnary();
        while (check(TokenKind::Star) || check(TokenKind::Slash)) {
            const Token op = advance();
            auto binary = makeExpr(ExprKind::Binary, op);
            binary->op = op.kind;
            binary->left = std::move(expr);
            binary->right = parseUnary();
            expr = std::move(binary);
        }
        return expr;
    }

    std::unique_ptr<Expr> parseUnary() {
        if (check(TokenKind::Minus) || check(TokenKind::Plus)) {
            const Token op = advance();
            auto unary = makeExpr(ExprKind::Unary, op);
            unary->op = op.kind;
            unary->right = parseUnary();
            return unary;
        }
        return parsePrimary();
    }

    std::unique_ptr<Expr> parsePrimary() {
        if (check(TokenKind::Number)) {
            const Token token = advance();
            auto expr = makeExpr(ExprKind::Number, token);
            expr->number = token.number;
            return expr;
        }
        if (check(TokenKind::String)) {
            const Token token = advance();
            auto expr = makeExpr(ExprKind::String, token);
            expr->text = token.text;
            return expr;
        }
        if (check(TokenKind::Local) || check(TokenKind::Global) || check(TokenKind::Ident)) {
            const Token token = advance();
            if (token.kind == TokenKind::Ident && match(TokenKind::LParen)) {
                auto call = makeExpr(ExprKind::Call, token);
                call->text = token.text;
                if (!check(TokenKind::RParen)) {
                    do {
                        call->args.push_back(parseExpression());
                    } while (match(TokenKind::Comma));
                }
                consume(TokenKind::RParen, "expected ')' after arguments");
                return call;
            }
            auto var = makeExpr(ExprKind::Var, token);
            var->text = token.text;
            return var;
        }
        if (match(TokenKind::LParen)) {
            auto expr = parseExpression();
            consume(TokenKind::RParen, "expected ')' after expression");
            return expr;
        }
        error(m_current, "expected expression");
    }

    Lexer m_lexer;
    Token m_current;
};

enum class ValueKind { Number, String };

struct Value {
    ValueKind kind = ValueKind::String;
    double number = 0;
    std::string text;
};

[[nodiscard]] Value makeNumber(double n) {
    Value value;
    value.kind = ValueKind::Number;
    value.number = n;
    return value;
}

[[nodiscard]] Value makeString(std::string text) {
    Value value;
    value.kind = ValueKind::String;
    value.text = std::move(text);
    return value;
}

[[nodiscard]] std::string formatNumber(double n) {
    if (!std::isfinite(n)) {
        if (std::isnan(n)) {
            return "nan";
        }
        return n > 0 ? "inf" : "-inf";
    }
    if (n == 0.0) {
        return "0";
    }
    const double truncated = std::trunc(n);
    if (n == truncated && std::fabs(n) < 1e15) {
        return std::to_string(static_cast<long long>(n));
    }
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::defaultfloat << n;
    return out.str();
}

[[nodiscard]] std::string toString(const Value& value) {
    if (value.kind == ValueKind::Number) {
        return formatNumber(value.number);
    }
    return value.text;
}

[[nodiscard]] bool looksNumeric(const std::string& text, double& out) {
    if (text.empty()) {
        out = 0;
        return true;
    }
    std::size_t idx = 0;
    try {
        out = std::stod(text, &idx);
    } catch (const std::exception&) {
        return false;
    }
    while (idx < text.size() && std::isspace(static_cast<unsigned char>(text[idx]))) {
        ++idx;
    }
    return idx == text.size();
}

[[nodiscard]] double toNumber(const Value& value) {
    if (value.kind == ValueKind::Number) {
        return value.number;
    }
    double n = 0;
    if (!looksNumeric(value.text, n)) {
        return 0;
    }
    return n;
}

[[nodiscard]] bool isTruthy(const Value& value) {
    if (value.kind == ValueKind::Number) {
        return value.number != 0.0;
    }
    return !value.text.empty();
}

[[nodiscard]] bool bothNumeric(const Value& a, const Value& b, double& left, double& right) {
    if (a.kind == ValueKind::Number && b.kind == ValueKind::Number) {
        left = a.number;
        right = b.number;
        return true;
    }
    double aNum = 0;
    double bNum = 0;
    const bool aOk = a.kind == ValueKind::Number || looksNumeric(a.text, aNum);
    const bool bOk = b.kind == ValueKind::Number || looksNumeric(b.text, bNum);
    if (!aOk || !bOk) {
        return false;
    }
    left = a.kind == ValueKind::Number ? a.number : aNum;
    right = b.kind == ValueKind::Number ? b.number : bNum;
    return true;
}

struct ReturnSignal {
    Value value;
};

struct FunctionDef {
    std::vector<std::string> params;
    const Stmt* body = nullptr;
};

class Interpreter {
public:
    explicit Interpreter(Dialect dialect) : m_dialect(dialect) {}

    ExecResult run(std::string_view source, std::string_view chunkName) {
        m_chunkName = chunkName.empty() ? std::string("<eval>") : std::string(chunkName);
        ExecResult result;
        result.dialect = m_dialect;
        try {
            Parser parser(source);
            auto program = parser.parseProgram();
            m_scopes.emplace_back();
            for (const auto& stmt : program) {
                exec(*stmt);
            }
            result.ok = true;
            result.output = m_output;
        } catch (const ScriptError& err) {
            result.ok = false;
            result.output = m_output;
            result.error = formatError(err);
        } catch (const ReturnSignal&) {
            result.ok = true;
            result.output = m_output;
        }
        return result;
    }

private:
    [[nodiscard]] std::string formatError(const ScriptError& err) const {
        std::ostringstream out;
        out << m_chunkName << ':' << err.line << ':' << err.column << ": " << err.message;
        return out.str();
    }

    [[noreturn]] void fail(const Expr& expr, const std::string& message) const {
        throw ScriptError{message, expr.line, expr.column};
    }

    [[noreturn]] void fail(const Stmt& stmt, const std::string& message) const {
        throw ScriptError{message, stmt.line, stmt.column};
    }

    void exec(const Stmt& stmt) {
        switch (stmt.kind) {
            case StmtKind::Expr:
                eval(*stmt.expr);
                break;
            case StmtKind::If: {
                if (isTruthy(eval(*stmt.expr))) {
                    exec(*stmt.thenBranch);
                } else if (stmt.elseBranch) {
                    exec(*stmt.elseBranch);
                }
                break;
            }
            case StmtKind::Block:
                for (const auto& child : stmt.stmts) {
                    exec(*child);
                }
                break;
            case StmtKind::Function:
                m_functions[stmt.name] = FunctionDef{stmt.params, stmt.thenBranch.get()};
                break;
            case StmtKind::Return:
                throw ReturnSignal{stmt.expr ? eval(*stmt.expr) : makeString("")};
        }
    }

    Value eval(const Expr& expr) {
        switch (expr.kind) {
            case ExprKind::Number:
                return makeNumber(expr.number);
            case ExprKind::String:
                return makeString(expr.text);
            case ExprKind::Var:
                return getVar(expr.text);
            case ExprKind::Assign: {
                const Value value = eval(*expr.right);
                setVar(expr.text, value);
                return value;
            }
            case ExprKind::Unary: {
                const Value right = eval(*expr.right);
                if (expr.op == TokenKind::Minus) {
                    return makeNumber(-toNumber(right));
                }
                return makeNumber(toNumber(right));
            }
            case ExprKind::Binary:
                return evalBinary(expr);
            case ExprKind::Call:
                return evalCall(expr);
        }
        fail(expr, "internal: unknown expression");
    }

    Value evalBinary(const Expr& expr) {
        const Value left = eval(*expr.left);
        const Value right = eval(*expr.right);
        switch (expr.op) {
            case TokenKind::Plus:
                return makeNumber(toNumber(left) + toNumber(right));
            case TokenKind::Minus:
                return makeNumber(toNumber(left) - toNumber(right));
            case TokenKind::Star:
                return makeNumber(toNumber(left) * toNumber(right));
            case TokenKind::Slash: {
                const double denom = toNumber(right);
                if (denom == 0.0) {
                    fail(expr, "division by zero");
                }
                return makeNumber(toNumber(left) / denom);
            }
            case TokenKind::EqEq:
                return makeNumber(valuesEqual(left, right) ? 1 : 0);
            case TokenKind::BangEq:
                return makeNumber(valuesEqual(left, right) ? 0 : 1);
            case TokenKind::Lt:
                return makeNumber(valuesLess(left, right) ? 1 : 0);
            case TokenKind::Gt:
                return makeNumber(valuesLess(right, left) ? 1 : 0);
            default:
                fail(expr, "internal: unknown operator");
        }
    }

    [[nodiscard]] static bool valuesEqual(const Value& left, const Value& right) {
        double a = 0;
        double b = 0;
        if (bothNumeric(left, right, a, b)) {
            return a == b;
        }
        return toString(left) == toString(right);
    }

    [[nodiscard]] static bool valuesLess(const Value& left, const Value& right) {
        double a = 0;
        double b = 0;
        if (bothNumeric(left, right, a, b)) {
            return a < b;
        }
        return toString(left) < toString(right);
    }

    Value evalCall(const Expr& expr) {
        std::vector<Value> args;
        args.reserve(expr.args.size());
        for (const auto& arg : expr.args) {
            args.push_back(eval(*arg));
        }

        if (expr.text == "echo") {
            std::string printed;
            for (const auto& arg : args) {
                printed += toString(arg);
            }
            m_output += printed;
            return makeString(printed);
        }

        const auto it = m_functions.find(expr.text);
        if (it == m_functions.end()) {
            fail(expr, "unknown function '" + expr.text + "'");
        }
        const FunctionDef& fn = it->second;
        if (args.size() != fn.params.size()) {
            fail(expr,
                 "function '" + expr.text + "' expected " + std::to_string(fn.params.size()) +
                     " argument(s)");
        }

        Scope scope;
        for (std::size_t i = 0; i < fn.params.size(); ++i) {
            scope.locals[fn.params[i]] = args[i];
        }
        m_scopes.push_back(std::move(scope));
        Value ret = makeString("");
        try {
            if (fn.body != nullptr) {
                exec(*fn.body);
            }
        } catch (const ReturnSignal& signal) {
            ret = signal.value;
        } catch (...) {
            m_scopes.pop_back();
            throw;
        }
        m_scopes.pop_back();
        return ret;
    }

    [[nodiscard]] Value getVar(const std::string& name) const {
        if (!name.empty() && name[0] == '$') {
            const auto it = m_globals.find(name);
            return it == m_globals.end() ? makeString("") : it->second;
        }
        if (m_scopes.empty()) {
            return makeString("");
        }
        const auto it = m_scopes.back().locals.find(name);
        return it == m_scopes.back().locals.end() ? makeString("") : it->second;
    }

    void setVar(const std::string& name, const Value& value) {
        if (!name.empty() && name[0] == '$') {
            m_globals[name] = value;
            return;
        }
        if (m_scopes.empty()) {
            m_scopes.emplace_back();
        }
        m_scopes.back().locals[name] = value;
    }

    struct Scope {
        std::unordered_map<std::string, Value> locals;
    };

    Dialect m_dialect;
    std::string m_chunkName;
    std::string m_output;
    std::vector<Scope> m_scopes;
    std::unordered_map<std::string, Value> m_globals;
    std::unordered_map<std::string, FunctionDef> m_functions;
};

} // namespace

ExecResult eval(Dialect dialect, std::string_view source, std::string_view chunkName) {
    Interpreter interpreter(dialect);
    return interpreter.run(source, chunkName);
}

ExecResult evalFile(Dialect dialect, const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    ExecResult result;
    result.dialect = dialect;
    if (!in) {
        result.ok = false;
        result.error = "failed to open '" + path.generic_string() + "'";
        return result;
    }
    const std::string source((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::string chunkName = path.generic_string();
    return eval(dialect, source, chunkName);
}

} // namespace fuse::compat

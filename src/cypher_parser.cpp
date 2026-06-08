#include "cypher_parser.h"
#include "graph_store.h"
#include "serializer.h"
#include <cctype>
#include <algorithm>
#include <sstream>

namespace graphstore {

// ==================== Lexer ====================

Lexer::Lexer(const std::string& input) : input_(input) {}

char Lexer::Current() const {
    if (pos_ >= input_.size()) return '\0';
    return input_[pos_];
}

char Lexer::Advance() {
    if (pos_ >= input_.size()) return '\0';
    char c = input_[pos_++];
    if (c == '\n') { line_++; column_ = 1; }
    else column_++;
    return c;
}

bool Lexer::Match(char expected) {
    if (Current() == expected) { Advance(); return true; }
    return false;
}

bool Lexer::IsAlpha(char c) const { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool Lexer::IsDigit(char c) const { return std::isdigit(static_cast<unsigned char>(c)); }
bool Lexer::IsAlnum(char c) const { return IsAlpha(c) || IsDigit(c); }

Token Lexer::MakeToken(Token::Type type, const std::string& text) {
    return Token{type, text, line_, column_};
}

void Lexer::SkipWhitespace() {
    while (std::isspace(static_cast<unsigned char>(Current()))) Advance();
}

void Lexer::SkipComment() {
    if (Current() == '/' && pos_ + 1 < input_.size() && input_[pos_ + 1] == '/') {
        while (Current() != '\n' && Current() != '\0') Advance();
    }
}

Token Lexer::ReadIdentifierOrKeyword() {
    size_t start = pos_;
    while (IsAlnum(Current())) Advance();
    std::string text = input_.substr(start, pos_ - start);
    // Convert to uppercase for keyword lookup
    std::string upper;
    upper.resize(text.size());
    std::transform(text.begin(), text.end(), upper.begin(), [](unsigned char c) {
        return static_cast<unsigned char>(std::toupper(c));
    });
    
    // 关键字映射
    static const std::unordered_map<std::string, Token::Type> keywords = {
        {"MATCH", Token::Type::MATCH}, {"OPTIONAL", Token::Type::OPTIONAL_TYPE},
        {"WHERE", Token::Type::WHERE}, {"RETURN", Token::Type::RETURN},
        {"WITH", Token::Type::WITH}, {"UNWIND", Token::Type::UNWIND},
        {"AS", Token::Type::AS}, {"CREATE", Token::Type::CREATE},
        {"MERGE", Token::Type::MERGE}, {"SET", Token::Type::SET},
        {"DELETE", Token::Type::DELETE_TYPE}, {"DETACH", Token::Type::DETACH},
        {"REMOVE", Token::Type::REMOVE}, {"ON", Token::Type::ON},
        {"ORDER", Token::Type::ORDER}, {"BY", Token::Type::BY},
        {"ASC", Token::Type::ASC}, {"DESC", Token::Type::DESC},
        {"SKIP", Token::Type::SKIP}, {"LIMIT", Token::Type::LIMIT},
        {"AND", Token::Type::AND}, {"OR", Token::Type::OR},
        {"NOT", Token::Type::NOT}, {"IN", Token::Type::IN_TYPE},
        {"IS", Token::Type::IS}, {"NULL", Token::Type::NULL_},
        {"TRUE", Token::Type::TRUE_TYPE}, {"FALSE", Token::Type::FALSE_TYPE},
        {"DISTINCT", Token::Type::DISTINCT},
        {"COUNT", Token::Type::COUNT}, {"SUM", Token::Type::SUM},
        {"AVG", Token::Type::AVG}, {"MIN", Token::Type::MIN},
        {"MAX", Token::Type::MAX}, {"EXISTS", Token::Type::EXISTS},
        {"COLLECT", Token::Type::COLLECT},
        {"CASE", Token::Type::CASE}, {"WHEN", Token::Type::WHEN},
        {"THEN", Token::Type::THEN}, {"ELSE", Token::Type::ELSE},
        {"END", Token::Type::END}
    };
    
    auto it = keywords.find(upper);
    if (it != keywords.end()) return MakeToken(it->second, text);
    return MakeToken(Token::Type::IDENTIFIER, text);
}

Token Lexer::ReadString() {
    char quote = Advance();  // ' 或 "
    size_t start = pos_;
    while (Current() != quote && Current() != '\0') {
        if (Current() == '\\') Advance();  // 转义
        Advance();
    }
    std::string text = input_.substr(start, pos_ - start);
    if (Current() == quote) Advance();
    return MakeToken(Token::Type::STRING, text);
}

Token Lexer::ReadNumber() {
    size_t start = pos_;
    bool is_float = false;
    
    while (IsDigit(Current())) Advance();
    if (Current() == '.' && pos_ + 1 < input_.size() && input_[pos_ + 1] != '.') {
        is_float = true;
        Advance();
        while (IsDigit(Current())) Advance();
    }
    if (Current() == 'e' || Current() == 'E') {
        is_float = true;
        Advance();
        if (Current() == '+' || Current() == '-') Advance();
        while (IsDigit(Current())) Advance();
    }
    
    std::string text = input_.substr(start, pos_ - start);
    return MakeToken(is_float ? Token::Type::FLOAT : Token::Type::INTEGER, text);
}

Token Lexer::ReadParameter() {
    Advance();  // $
    size_t start = pos_;
    if (IsAlpha(Current())) {
        while (IsAlnum(Current())) Advance();
    } else if (IsDigit(Current())) {
        while (IsDigit(Current())) Advance();
    }
    return MakeToken(Token::Type::PARAMETER, input_.substr(start, pos_ - start));
}

Token Lexer::NextTokenFromInput() {
    SkipWhitespace();
    SkipComment();
    SkipWhitespace();
    
    if (pos_ >= input_.size()) return MakeToken(Token::Type::EOF_, "");
    
    char c = Current();
    size_t line = line_, col = column_;
    
    // 标识符
    if (IsAlpha(c)) return ReadIdentifierOrKeyword();
    
    // 字符串
    if (c == '\'' || c == '"') return ReadString();
    
    // 数字
    if (IsDigit(c)) return ReadNumber();
    
    // 参数
    if (c == '$') return ReadParameter();
    
    // 符号
    auto make_sym = [&](Token::Type t, const std::string& s) -> Token {
        for (size_t i = 0; i < s.size(); ++i) Advance();
        return MakeToken(t, s);
    };
    
    switch (c) {
        case '(': return make_sym(Token::Type::LPAREN, "(");
        case ')': return make_sym(Token::Type::RPAREN, ")");
        case '[': return make_sym(Token::Type::LBRACKET, "[");
        case ']': return make_sym(Token::Type::RBRACKET, "]");
        case '{': return make_sym(Token::Type::LBRACE, "{");
        case '}': return make_sym(Token::Type::RBRACE, "}");
        case ':': return make_sym(Token::Type::COLON, ":");
        case ',': return make_sym(Token::Type::COMMA, ",");
        case ';': return make_sym(Token::Type::SEMICOLON, ";");
        case '.':
            if (pos_ + 1 < input_.size() && input_[pos_ + 1] == '.') {
                return make_sym(Token::Type::ELLIPSIS, "..");
            }
            return make_sym(Token::Type::DOT, ".");
        case '+': return make_sym(Token::Type::PLUS, "+");
        case '*': return make_sym(Token::Type::STAR, "*");
        case '/': return make_sym(Token::Type::SLASH, "/");
        case '%': return make_sym(Token::Type::PERCENT, "%");
        case '|': return make_sym(Token::Type::PIPE, "|");
        case '-':
            if (pos_ + 1 < input_.size() && input_[pos_ + 1] == '>') {
                return make_sym(Token::Type::ARROW_RIGHT, "->");
            }
            return make_sym(Token::Type::MINUS, "-");
        case '<':
            if (pos_ + 1 < input_.size() && input_[pos_ + 1] == '=') {
                return make_sym(Token::Type::LE, "<=");
            }
            if (pos_ + 1 < input_.size() && input_[pos_ + 1] == '-') {
                return make_sym(Token::Type::ARROW_LEFT, "<-");
            }
            return make_sym(Token::Type::LT, "<");
        case '>':
            if (pos_ + 1 < input_.size() && input_[pos_ + 1] == '=') {
                return make_sym(Token::Type::GE, ">=");
            }
            return make_sym(Token::Type::GT, ">");
        case '=':
            if (pos_ + 1 < input_.size() && input_[pos_ + 1] == '~') {
                Advance(); Advance();
                return MakeToken(Token::Type::ASSIGN, "=~");
            }
            return make_sym(Token::Type::EQ, "=");
        case '!':
            if (pos_ + 1 < input_.size() && input_[pos_ + 1] == '=') {
                return make_sym(Token::Type::NE, "!=");
            }
            break;
    }
    
    Advance();
    return MakeToken(Token::Type::INVALID, std::string(1, c));
}

Token Lexer::NextToken() {
    if (!lookahead_.empty()) {
        Token tok = lookahead_.front();
        lookahead_.erase(lookahead_.begin());
        return tok;
    }
    return NextTokenFromInput();
}

Token Lexer::PeekToken(size_t offset) {
    while (lookahead_.size() <= offset) {
        lookahead_.push_back(NextTokenFromInput());
    }
    return lookahead_[offset];
}

bool Lexer::IsAtEnd() const {
    return pos_ >= input_.size();
}

// ==================== Parser ====================

Parser::Parser(const std::string& input) : lexer_(input) {
    Advance();
}

void Parser::Advance() {
    current_ = lexer_.NextToken();
}

bool Parser::Match(Token::Type type) {
    if (Check(type)) { Advance(); return true; }
    return false;
}

bool Parser::Check(Token::Type type) const {
    return current_.type == type;
}

Token Parser::Consume(Token::Type type, const std::string& message) {
    if (Check(type)) {
        Token tok = current_;
        Advance();
        return tok;
    }
    Error(message + " (got " + current_.text + ")");
    return Token{Token::Type::INVALID, ""};
}

void Parser::Error(const std::string& message) {
    if (error_.empty()) {
        error_ = "Line " + std::to_string(current_.line) + ":" + 
                 std::to_string(current_.column) + " - " + message;
    }
}

std::optional<CypherQuery> Parser::ParseQuery() {
    try {
        return ParseCypherQuery();
    } catch (...) {
        return std::nullopt;
    }
}

CypherQuery Parser::ParseCypherQuery() {
    CypherQuery query;
    
    // 解析多个子句
    while (!Check(Token::Type::EOF_)) {
        if (Check(Token::Type::MATCH) || Check(Token::Type::OPTIONAL_TYPE)) {
            query.match_clauses.push_back(ParseMatchClause());
            // WHERE 紧跟 MATCH
            if (Check(Token::Type::WHERE)) {
                query.where = ParseWhereClause();
            }
        } else if (Check(Token::Type::WITH)) {
            query.with_clauses.push_back(std::make_shared<WithClause>(ParseWithClause()));
        } else if (Check(Token::Type::UNWIND)) {
            query.unwind_clauses.push_back(std::make_shared<UnwindClause>(ParseUnwindClause()));
        } else if (Check(Token::Type::CREATE)) {
            query.create = ParseCreateClause();
        } else if (Check(Token::Type::MERGE)) {
            query.merge = ParseMergeClause();
        } else if (Check(Token::Type::SET)) {
            query.set = ParseSetClause();
        } else if (Check(Token::Type::DELETE_TYPE) || Check(Token::Type::DETACH)) {
            query.del = ParseDeleteClause();
        } else if (Check(Token::Type::REMOVE)) {
            query.remove = ParseRemoveClause();
        } else if (Check(Token::Type::RETURN)) {
            query.ret = ParseReturnClause();
            break;  // RETURN 通常是最后一个子句
        } else {
            Error("Unexpected token: " + current_.text);
            break;
        }
    }
    
    // 解析 ORDER BY, SKIP, LIMIT（在 RETURN 之后）
    if (Check(Token::Type::ORDER)) {
        Advance();  // ORDER
        Consume(Token::Type::BY, "Expected BY after ORDER");
        do {
            auto expr = ParseExpression();
            bool asc = true;
            if (Match(Token::Type::ASC)) asc = true;
            else if (Match(Token::Type::DESC)) asc = false;
            query.order_by.push_back({expr, asc});
        } while (Match(Token::Type::COMMA));
    }
    
    if (Match(Token::Type::SKIP)) {
        auto tok = Consume(Token::Type::INTEGER, "Expected integer after SKIP");
        query.skip = std::stoull(tok.text);
    }
    
    if (Match(Token::Type::LIMIT)) {
        auto tok = Consume(Token::Type::INTEGER, "Expected integer after LIMIT");
        query.limit = std::stoull(tok.text);
    }
    
    if (!error_.empty()){
        throw std::runtime_error(error_);
    }
    return query;
}

MatchClause Parser::ParseMatchClause() {
    MatchClause match;
    if (Match(Token::Type::OPTIONAL_TYPE)) {
        match.optional = true;
        Consume(Token::Type::MATCH, "Expected MATCH after OPTIONAL");
    } else {
        Consume(Token::Type::MATCH, "Expected MATCH");
    }
    
    do {
        match.patterns.push_back(ParsePathPattern());
    } while (Match(Token::Type::COMMA));
    
    // WHERE 紧跟 MATCH
    if (Check(Token::Type::WHERE)) {
        // 这里不解析 WHERE，它会在 ParseCypherQuery 中处理
    }
    
    return match;
}

WhereClause Parser::ParseWhereClause() {
    Consume(Token::Type::WHERE, "Expected WHERE");
    return WhereClause{ParseExpression()};
}

ReturnClause Parser::ParseReturnClause() {
    Consume(Token::Type::RETURN, "Expected RETURN");
    ReturnClause ret;
    
    if (Match(Token::Type::DISTINCT)) ret.distinct = true;
    
    if (Match(Token::Type::STAR)) {
        ret.all = true;
    } else {
        do {
            ReturnClause::ReturnItem item;
            item.expression = ParseExpression();
            if (Match(Token::Type::AS)) {
                item.alias = ParseIdentifier();
            }
            ret.items.push_back(item);
        } while (Match(Token::Type::COMMA));
    }
    
    return ret;
}

CreateClause Parser::ParseCreateClause() {
    Consume(Token::Type::CREATE, "Expected CREATE");
    CreateClause create;
    do {
        create.patterns.push_back(ParsePathPattern());
    } while (Match(Token::Type::COMMA));
    return create;
}

MergeClause Parser::ParseMergeClause() {
    Consume(Token::Type::MERGE, "Expected MERGE");
    MergeClause merge;
    do {
        merge.patterns.push_back(ParsePathPattern());
    } while (Match(Token::Type::COMMA));
    for (;;) {
        if (!Check(Token::Type::ON)) break;
        Advance();
        if (Match(Token::Type::MATCH)) {
            merge.on_match = std::move(ParseSetClause());
        } else if (Match(Token::Type::CREATE)) {
            merge.on_create = std::move(ParseSetClause());
        } else {
            break;
        }
    }
    return merge;
}

SetClause Parser::ParseSetClause() {
    Consume(Token::Type::SET, "Expected SET");
    SetClause set;
    do {
        SetClause::SetItem item;
        // Parse target without consuming comparison operators
        auto target = ParseAddSubExpression();
        
        if (Match(Token::Type::COLON)) {
            // SET n:Label (or n:Label1:Label2)
            item.add_label = true;
            item.target = target;
            item.label = ParseIdentifier();
            while (Match(Token::Type::COLON)) {
                set.items.push_back(std::move(item));
                item = SetClause::SetItem{};
                item.add_label = true;
                item.target = target;
                item.label = ParseIdentifier();
            }
        } else {
            // SET n.prop = value
            Consume(Token::Type::EQ, "Expected =");
            item.target = target;
            item.value = ParseExpression();
        }
        set.items.push_back(item);
    } while (Match(Token::Type::COMMA));
    return set;
}

DeleteClause Parser::ParseDeleteClause() {
    DeleteClause del;
    if (Match(Token::Type::DETACH)) del.detach = true;
    Consume(Token::Type::DELETE_TYPE, "Expected DELETE");
    do {
        del.expressions.push_back(ParseExpression());
    } while (Match(Token::Type::COMMA));
    return del;
}

RemoveClause Parser::ParseRemoveClause() {
    Consume(Token::Type::REMOVE, "Expected REMOVE");
    RemoveClause remove;
    do {
        RemoveClause::RemoveItem item;
        auto target = ParseExpression();
        if (Match(Token::Type::COLON)) {
            item.is_label = true;
            item.label = ParseIdentifier();
        }
        item.target = target;
        remove.items.push_back(item);
    } while (Match(Token::Type::COMMA));
    return remove;
}

WithClause Parser::ParseWithClause() {
    Consume(Token::Type::WITH, "Expected WITH");
    WithClause with;
    if (Match(Token::Type::DISTINCT)) with.distinct = true;
    do {
        ReturnClause::ReturnItem item;
        item.expression = ParseExpression();
        if (Match(Token::Type::AS)) item.alias = ParseIdentifier();
        with.items.push_back(item);
    } while (Match(Token::Type::COMMA));
    
    if (Check(Token::Type::WHERE)) {
        with.where = ParseExpression();
    }
    return with;
}

UnwindClause Parser::ParseUnwindClause() {
    Consume(Token::Type::UNWIND, "Expected UNWIND");
    UnwindClause unwind;
    unwind.expression = ParseExpression();
    Consume(Token::Type::AS, "Expected AS");
    unwind.variable = ParseIdentifier();
    return unwind;
}

// ==================== 表达式解析（优先级 climbing）====================

std::shared_ptr<Expression> Parser::ParseExpression() {
    return ParseOrExpression();
}

std::shared_ptr<Expression> Parser::ParseOrExpression() {
    auto left = ParseAndExpression();
    while (Match(Token::Type::OR)) {
        auto expr = std::make_shared<Expression>();
        expr->type = Expression::Type::LOGICAL;
        expr->logical_op = LogicalOp::OR;
        expr->left = left;
        expr->right = ParseAndExpression();
        left = expr;
    }
    return left;
}

std::shared_ptr<Expression> Parser::ParseAndExpression() {
    auto left = ParseNotExpression();
    while (Match(Token::Type::AND)) {
        auto expr = std::make_shared<Expression>();
        expr->type = Expression::Type::LOGICAL;
        expr->logical_op = LogicalOp::AND;
        expr->left = left;
        expr->right = ParseNotExpression();
        left = expr;
    }
    return left;
}

std::shared_ptr<Expression> Parser::ParseNotExpression() {
    if (Match(Token::Type::NOT)) {
        auto expr = std::make_shared<Expression>();
        expr->type = Expression::Type::LOGICAL;
        expr->logical_op = LogicalOp::NOT;
        expr->left = ParseComparisonExpression();
        return expr;
    }
    return ParseComparisonExpression();
}

std::shared_ptr<Expression> Parser::ParseComparisonExpression() {
    auto left = ParseAddSubExpression();
    
    // 检查各种比较操作符
    if (Check(Token::Type::EQ) || Check(Token::Type::NE) || 
        Check(Token::Type::LT) || Check(Token::Type::LE) ||
        Check(Token::Type::GT) || Check(Token::Type::GE)) {
        auto expr = std::make_shared<Expression>();
        expr->type = Expression::Type::COMPARISON;
        expr->comp_op = ParseComparisonOp();
        expr->left = left;
        expr->right = ParseAddSubExpression();
        return expr;
    }
    
    // IS [NOT] NULL
    if (Match(Token::Type::IS)) {
        auto expr = std::make_shared<Expression>();
        expr->type = Expression::Type::COMPARISON;
        expr->left = left;
        if (Match(Token::Type::NOT)) {
            expr->comp_op = ComparisonOp::IS_NOT_NULL;
        } else {
            expr->comp_op = ComparisonOp::IS_NULL;
        }
        Consume(Token::Type::NULL_, "Expected NULL");
        return expr;
    }
    
    // STARTS WITH, ENDS WITH, CONTAINS
    if (Check(Token::Type::IDENTIFIER)) {
        std::string id = current_.text;
        if (id == "STARTS") {
            Advance();
            Consume(Token::Type::WITH, "Expected WITH");
            auto expr = std::make_shared<Expression>();
            expr->type = Expression::Type::COMPARISON;
            expr->comp_op = ComparisonOp::STARTS_WITH;
            expr->left = left;
            expr->right = ParseAddSubExpression();
            return expr;
        } else if (id == "ENDS") {
            Advance();
            Consume(Token::Type::WITH, "Expected WITH");
            auto expr = std::make_shared<Expression>();
            expr->type = Expression::Type::COMPARISON;
            expr->comp_op = ComparisonOp::ENDS_WITH;
            expr->left = left;
            expr->right = ParseAddSubExpression();
            return expr;
        } else if (id == "CONTAINS") {
            Advance();
            auto expr = std::make_shared<Expression>();
            expr->type = Expression::Type::COMPARISON;
            expr->comp_op = ComparisonOp::CONTAINS;
            expr->left = left;
            expr->right = ParseAddSubExpression();
            return expr;
        } else if (id == "IN") {
            Advance();
            auto expr = std::make_shared<Expression>();
            expr->type = Expression::Type::COMPARISON;
            expr->comp_op = ComparisonOp::CYPHER_IN;
            expr->left = left;
            expr->right = ParsePrimaryExpression();
            return expr;
        }
    }
    
    return left;
}

std::shared_ptr<Expression> Parser::ParseAddSubExpression() {
    auto left = ParseMulDivExpression();
    while (Check(Token::Type::PLUS) || Check(Token::Type::MINUS)) {
        // 简化：不实现算术表达式，直接返回
        break;
    }
    return left;
}

std::shared_ptr<Expression> Parser::ParseMulDivExpression() {
    return ParseUnaryExpression();
}

std::shared_ptr<Expression> Parser::ParseUnaryExpression() {
    return ParsePrimaryExpression();
}

std::shared_ptr<Expression> Parser::ParsePrimaryExpression() {
    // 字面量
    if (Check(Token::Type::INTEGER) || Check(Token::Type::FLOAT) ||
        Check(Token::Type::STRING) || Check(Token::Type::TRUE_TYPE) ||
        Check(Token::Type::FALSE_TYPE) || Check(Token::Type::NULL_)) {
        return ParseLiteral();
    }
    
    // 参数
    if (Check(Token::Type::PARAMETER)) {
        auto expr = std::make_shared<Expression>();
        expr->type = Expression::Type::PARAMETER;
        expr->param_name = current_.text;
        Advance();
        return expr;
    }
    
    // 函数调用 / 聚合函数
    if (Check(Token::Type::IDENTIFIER) || Check(Token::Type::COUNT) ||
        Check(Token::Type::SUM) || Check(Token::Type::AVG) ||
        Check(Token::Type::MIN) || Check(Token::Type::MAX) ||
        Check(Token::Type::COLLECT) || Check(Token::Type::EXISTS)) {
        std::string name = current_.text;
        // 预读检查是否是函数调用（后面跟左括号）
        if (lexer_.PeekToken(0).type == Token::Type::LPAREN) {
            return ParseFunctionCall();
        }
        return ParsePropertyOrVariable();
    }
    
    // 列表
    if (Check(Token::Type::LBRACKET)) {
        return ParseListExpression();
    }
    
    // CASE
    if (Check(Token::Type::CASE)) {
        return ParseCaseExpression();
    }
    
    // 括号表达式
    if (Match(Token::Type::LPAREN)) {
        auto expr = ParseExpression();
        Consume(Token::Type::RPAREN, "Expected )");
        return expr;
    }
    
    // 模式表达式 (用于 EXISTS)
    if (Check(Token::Type::LPAREN) && lexer_.PeekToken(0).type == Token::Type::IDENTIFIER) {
        // 可能是模式，简化处理
    }
    
    Error("Unexpected expression: " + current_.text);
    return std::make_shared<Expression>();
}

std::shared_ptr<Expression> Parser::ParseLiteral() {
    auto expr = std::make_shared<Expression>();
    expr->type = Expression::Type::LITERAL;
    
    if (Check(Token::Type::INTEGER)) {
        expr->literal_value = std::stoll(current_.text);
        Advance();
    } else if (Check(Token::Type::FLOAT)) {
        expr->literal_value = std::stod(current_.text);
        Advance();
    } else if (Check(Token::Type::STRING)) {
        expr->literal_value = current_.text;
        Advance();
    } else if (Check(Token::Type::TRUE_TYPE)) {
        expr->literal_value = true;
        Advance();
    } else if (Check(Token::Type::FALSE_TYPE)) {
        expr->literal_value = false;
        Advance();
    } else if (Check(Token::Type::NULL_)) {
        expr->literal_value = std::monostate{};
        Advance();
    }
    
    return expr;
}

std::shared_ptr<Expression> Parser::ParsePropertyOrVariable() {
    auto expr = std::make_shared<Expression>();
    std::string name = ParseIdentifier();
    
    if (Match(Token::Type::DOT)) {
        // 属性访问: n.name
        expr->type = Expression::Type::PROPERTY;
        expr->variable_name = name;
        expr->property_name = ParseIdentifier();
    } else {
        // 变量: n
        expr->type = Expression::Type::VARIABLE;
        expr->variable_name = name;
    }
    
    return expr;
}

std::shared_ptr<Expression> Parser::ParseFunctionCall() {
    auto expr = std::make_shared<Expression>();
    std::string name = ParseIdentifier();
    // uppercase function names
    std::string upper;
    upper.resize(name.size());
    std::transform(name.begin(), name.end(), upper.begin(), [](unsigned char c) {
        return static_cast<unsigned char>(std::toupper(c));
    });
    
    if (IsAggregationFunction(upper)) {
        expr->type = Expression::Type::AGGREGATION;
        expr->agg_func = upper;
    } else {
        expr->type = Expression::Type::FUNCTION_CALL;
        expr->function_name = upper;
    }
    
    Consume(Token::Type::LPAREN, "Expected (");
    
    if (Match(Token::Type::DISTINCT)) {
        expr->distinct = true;
    }
    
    if (!Check(Token::Type::RPAREN)) {
        do {
            expr->args.push_back(ParseExpression());
        } while (Match(Token::Type::COMMA));
    }
    
    Consume(Token::Type::RPAREN, "Expected )");
    return expr;
}

std::shared_ptr<Expression> Parser::ParseListExpression() {
    auto expr = std::make_shared<Expression>();
    expr->type = Expression::Type::LIST;
    Consume(Token::Type::LBRACKET, "Expected [");
    
    if (!Check(Token::Type::RBRACKET)) {
        do {
            expr->args.push_back(ParseExpression());
        } while (Match(Token::Type::COMMA));
    }
    
    Consume(Token::Type::RBRACKET, "Expected ]");
    return expr;
}

std::shared_ptr<Expression> Parser::ParseCaseExpression() {
    auto expr = std::make_shared<Expression>();
    expr->type = Expression::Type::FUNCTION_CALL;
    expr->function_name = "CASE";
    Consume(Token::Type::CASE, "Expected CASE");
    
    // Simple CASE: CASE expr WHEN val1 THEN res1 WHEN ...
    if (!Check(Token::Type::WHEN) && !Check(Token::Type::ELSE) && !Check(Token::Type::END)) {
        expr->case_is_simple = true;
        expr->args.push_back(ParseExpression());
    }
    
    while (Match(Token::Type::WHEN)) {
        expr->args.push_back(ParseExpression());
        Consume(Token::Type::THEN, "Expected THEN");
        expr->args.push_back(ParseExpression());
    }
    
    if (Match(Token::Type::ELSE)) {
        expr->args.push_back(ParseExpression());
    }
    
    Consume(Token::Type::END, "Expected END");
    return expr;
}

// ==================== 模式解析 ====================

PathPattern Parser::ParsePathPattern() {
    PathPattern path;
    
    // 起始节点
    path.nodes.push_back(ParseNodePattern());
    
    // 关系和后续节点
    while (Check(Token::Type::MINUS) || Check(Token::Type::ARROW_LEFT) ||
           Check(Token::Type::ARROW_RIGHT) || Check(Token::Type::LPAREN)) {
        
        if (Check(Token::Type::ARROW_LEFT) || Check(Token::Type::ARROW_RIGHT) ||
            Check(Token::Type::MINUS)) {
            path.relationships.push_back(ParseRelationshipPattern());
            path.nodes.push_back(ParseNodePattern());
        } else {
            break;
        }
    }
    
    return path;
}

NodePattern Parser::ParseNodePattern() {
    Consume(Token::Type::LPAREN, "Expected (");
    NodePattern node;
    
    if (Check(Token::Type::IDENTIFIER)) {
        node.variable = ParseIdentifier();
    }
    
    if (Check(Token::Type::COLON)) {
        node.labels = ParseLabels();
    }
    
    if (Check(Token::Type::LBRACE)) {
        node.properties = ParseProperties();
    }
    
    Consume(Token::Type::RPAREN, "Expected )");
    return node;
}

RelationshipPattern Parser::ParseRelationshipPattern() {
    RelationshipPattern rel;
    
    // 方向
    if (Check(Token::Type::ARROW_LEFT)) {
        rel.direction = Direction::IN_DIR;
        Advance();
    } else if (Check(Token::Type::MINUS)) {
        Advance();
        if (Check(Token::Type::ARROW_RIGHT)) {
            rel.direction = Direction::OUT_DIR;
            Advance();
        } else {
            rel.direction = Direction::BOTH;
        }
    }
    
    // 关系详情 [rel:TYPE {props}]
    if (Check(Token::Type::LBRACKET)) {
        Consume(Token::Type::LBRACKET, "Expected [");
        
        if (Check(Token::Type::IDENTIFIER)) {
            rel.variable = ParseIdentifier();
        }
        
        if (Check(Token::Type::COLON)) {
            rel.types = ParseLabels();  // 复用标签解析
        }
        
        if (Check(Token::Type::STAR)) {
            // 变长关系 *1..5
            rel.variable_length = true;
            Advance();
            if (Check(Token::Type::INTEGER)) {
                size_t val = std::stoull(current_.text);
                rel.min_hops = val;
                rel.max_hops = val;   // *N 即恰好 N 跳
                Advance();
            }
            if (Check(Token::Type::ELLIPSIS)) {
                if (!rel.min_hops) rel.min_hops = 1;  // *..5 从 1 开始
                Advance();
                if (Check(Token::Type::INTEGER)) {
                    rel.max_hops = std::stoull(current_.text);
                    Advance();
                } else {
                    rel.max_hops.reset();  // *2.. 无上限
                }
            }
        }
        
        if (Check(Token::Type::LBRACE)) {
            rel.properties = ParseProperties();
        }
        
        Consume(Token::Type::RBRACKET, "Expected ]");
    }
    
    // 处理剩余的方向符号
    if (rel.direction == Direction::BOTH && Check(Token::Type::ARROW_RIGHT)) {
        rel.direction = Direction::OUT_DIR;
        Advance();
    }
    
    // 消费关系模式后的尾随 MINUS（用于 -- 和 <-[:REL]- 等情况）
    if (Check(Token::Type::MINUS)) {
        Advance();
    }
    
    return rel;
}

// ==================== 辅助方法 ====================

std::string Parser::ParseIdentifier() {
    if (Check(Token::Type::IDENTIFIER) || Check(Token::Type::COUNT) ||
        Check(Token::Type::SUM) || Check(Token::Type::AVG) ||
        Check(Token::Type::MIN) || Check(Token::Type::MAX) ||
        Check(Token::Type::COLLECT) || Check(Token::Type::EXISTS)) {
        std::string name = current_.text;
        Advance();
        return name;
    }
    Error("Expected identifier, got: " + current_.text);
    return "";
}

std::vector<std::string> Parser::ParseLabels() {
    std::vector<std::string> labels;
    do {
        Consume(Token::Type::COLON, "Expected :");
        labels.push_back(ParseIdentifier());
    } while (Check(Token::Type::COLON));
    return labels;
}

std::unordered_map<std::string, CypherValue> Parser::ParseProperties() {
    Consume(Token::Type::LBRACE, "Expected {");
    std::unordered_map<std::string, CypherValue> props;
    
    if (!Check(Token::Type::RBRACE)) {
        do {
            std::string key = ParseIdentifier();
            Consume(Token::Type::COLON, "Expected :");
            props[key] = ParseCypherValue();
        } while (Match(Token::Type::COMMA));
    }
    
    Consume(Token::Type::RBRACE, "Expected }");
    return props;
}

CypherValue Parser::ParseCypherValue() {
    if (Check(Token::Type::INTEGER)) {
        auto val = std::stoll(current_.text);
        Advance();
        return val;
    } else if (Check(Token::Type::FLOAT)) {
        auto val = std::stod(current_.text);
        Advance();
        return val;
    } else if (Check(Token::Type::STRING)) {
        auto val = current_.text;
        Advance();
        return val;
    } else if (Check(Token::Type::TRUE_TYPE)) {
        Advance();
        return true;
    } else if (Check(Token::Type::FALSE_TYPE)) {
        Advance();
        return false;
    } else if (Check(Token::Type::NULL_)) {
        Advance();
        return std::monostate{};
    } else if (Check(Token::Type::LBRACKET)) {
        // 列表
        Advance();
        std::vector<CypherValue> list;
        if (!Check(Token::Type::RBRACKET)) {
            do {
                list.push_back(ParseCypherValue());
            } while (Match(Token::Type::COMMA));
        }
        Consume(Token::Type::RBRACKET, "Expected ]");
        // 简化：不返回列表类型
        return std::monostate{};
    }
    
    Error("Expected value");
    return std::monostate{};
}

ComparisonOp Parser::ParseComparisonOp() {
    if (Match(Token::Type::EQ)) return ComparisonOp::EQ;
    if (Match(Token::Type::NE)) return ComparisonOp::NE;
    if (Match(Token::Type::LT)) return ComparisonOp::LT;
    if (Match(Token::Type::LE)) return ComparisonOp::LE;
    if (Match(Token::Type::GT)) return ComparisonOp::GT;
    if (Match(Token::Type::GE)) return ComparisonOp::GE;
    Error("Expected comparison operator");
    return ComparisonOp::EQ;
}

bool Parser::IsAggregationFunction(const std::string& name) const {
    std::string upper;
    upper.resize(name.size());
    std::transform(name.begin(), name.end(), upper.begin(), [](unsigned char c) {
        return static_cast<unsigned char>(std::toupper(c));
    });
    return upper == "COUNT" || upper == "SUM" || upper == "AVG" || 
           upper == "MIN" || upper == "MAX" || upper == "COLLECT";
}

// ==================== QueryExecutor 简化实现 ====================

QueryExecutor::QueryExecutor(GraphStore* db) : db_(db) {
    read_tx_id_ = db_->BeginTransaction(true);
    read_ts_ = db_->ResolveReadTs(read_tx_id_);
    write_ts_ = 0;
}

QueryExecutor::~QueryExecutor() {
    if (read_tx_id_ != 0) {
        db_->Commit(read_tx_id_);
        read_tx_id_ = 0;
        read_ts_ = 0;
    }
    if (write_ts_ != 0) {
        db_->Rollback(write_ts_);
        write_ts_ = 0;
    }
}

TxId QueryExecutor::GetWriteTx() {
    if (write_ts_ == 0) {
        write_ts_ = db_->BeginTransaction(false);
    }
    return write_ts_;
}

void QueryExecutor::BeginTx() {
    explicit_tx_ = true;
}

Status QueryExecutor::CommitTx() {
    if (write_ts_ != 0) {
        Status s = db_->Commit(write_ts_);
        write_ts_ = 0;
        return s;
    }
    return Status::OK;
}

void QueryExecutor::RollbackTx() {
    if (write_ts_ != 0) {
        db_->Rollback(write_ts_);
        write_ts_ = 0;
    }
    explicit_tx_ = false;
}

QueryExecutor::ResultSet QueryExecutor::Execute(const CypherQuery& query) {
    BuildRelVars(query);
    // MERGE 和 CREATE 是独立操作
    if (query.merge) return ExecuteMerge(query);
    if (query.create) return ExecuteCreate(query);
    
    // SET/DELETE/REMOVE 需要先 MATCH
    if (query.set) return ExecuteSet(query);
    if (query.del) return ExecuteDelete(query);
    if (query.remove) return ExecuteRemove(query);
    
    // WITH 管道处理：MATCH → WITH* → RETURN
    if (!query.with_clauses.empty()) {
        std::vector<ResultRow> rows;
        if (!query.match_clauses.empty() && !query.match_clauses[0].patterns.empty()) {
            rows = MatchPattern(query.match_clauses[0].patterns[0], {});
        }
        for (const auto& wc : query.with_clauses) {
            rows = ApplyWithProjection(*wc, rows);
        }
        return ApplyReturn(query.ret, rows);
    }
    
    // 纯 MATCH + RETURN
    return ExecuteMatch(query);
}

QueryExecutor::ResultSet QueryExecutor::ExecuteMatch(const CypherQuery& query) {
    ResultSet result;
    std::vector<ResultRow> rows;
    
    // 简化：只处理第一个 MATCH 子句的第一个模式
    if (!query.match_clauses.empty() && !query.match_clauses[0].patterns.empty()) {
        const auto& pattern = query.match_clauses[0].patterns[0];
        rows = MatchPattern(pattern, rows);
    }
    
    // 应用 WHERE
    if (query.where) {
        rows = ApplyWhere(*query.where->condition, rows);
    }
    
    // 应用 ORDER BY, SKIP, LIMIT（在 RETURN 之前）
    if (!query.order_by.empty()) {
        rows = ApplyOrderBy(query.order_by, rows);
    }
    if (query.skip || query.limit) {
        rows = ApplySkipLimit(query.skip, query.limit, rows);
    }
    
    // 应用 RETURN
    result = ApplyReturn(query.ret, rows);
    
    return result;
}

std::vector<QueryExecutor::ResultRow> QueryExecutor::MatchPattern(
    const PathPattern& pattern, const std::vector<ResultRow>& input) {
    std::vector<ResultRow> result;
    
    if (pattern.nodes.empty()) return result;
    (void)input;  // cross-join with input 暂不实现
    
    // 使用 GetEffTs() 直接作为读取时间戳（read_ts_ 是 tx_id，但写入事务的 write_ver 可直接用于版本比较）
    TxId eff_ts = GetEffTs();
    
    // 显式事务中设置 search_roots_ 以读取自身写入
    Transaction* txn = nullptr;
    if (explicit_tx_ && write_ts_ != 0) {
        txn = db_->GetTransaction(write_ts_);
        if (txn) db_->SetSearchRoots(txn);
    }
    
    // 匹配第一个节点
    {
        const auto& node = pattern.nodes[0];
        {
            auto vertices = node.labels.empty()
                ? db_->GetAllVertices(eff_ts)
                : db_->GetVerticesByLabel(eff_ts, static_cast<LabelId>(Fnv1a(node.labels[0])));
            for (auto vid : vertices) {
                ResultRow row;
                if (!node.variable.empty())
                    row.bindings[node.variable] = static_cast<int64_t>(vid);
                bool match = true;
                for (const auto& [key, val] : node.properties) {
                    auto prop = db_->GetVertexProperty(eff_ts, vid, key);
                    if (!prop) { match = false; break; }
                    if (std::holds_alternative<std::string>(val)) {
                        if (*prop != std::get<std::string>(val)) { match = false; break; }
                    }
                }
                if (match) result.push_back(row);
            }
        }
    }
    
    // 遍历边关系链：(node1)-[rel1]->(node2)-[rel2]->(node3)...
    for (size_t i = 0; i < pattern.relationships.size() && i + 1 < pattern.nodes.size(); ++i) {
        const auto& rel = pattern.relationships[i];
        const auto& next_node = pattern.nodes[i + 1];
        
        std::vector<ResultRow> next_result;
        
        for (const auto& row : result) {
            // 从当前绑定中找源顶点
            VertexId src = 0;
            if (i < pattern.nodes.size()) {
                const auto& cur_node = pattern.nodes[i];
                auto it = row.bindings.find(cur_node.variable);
                if (it == row.bindings.end()) continue;
                src = static_cast<VertexId>(std::get<int64_t>(it->second));
            }
            
            // 遍历边
            auto edges = TraverseEdges(src, rel);
            
            for (const auto& entry : edges) {
                VertexId target = entry.target_id;
                
                // 匹配目标节点
                bool node_match = true;
                if (!next_node.labels.empty()) {
                    auto v = db_->GetVertex(eff_ts, target);
                    if (!v || v->label != static_cast<LabelId>(Fnv1a(next_node.labels[0])))
                        node_match = false;
                }
                if (node_match) {
                    for (const auto& [key, val] : next_node.properties) {
                        auto prop = db_->GetVertexProperty(eff_ts, target, key);
                        if (!prop) { node_match = false; break; }
                        if (std::holds_alternative<std::string>(val)) {
                            if (*prop != std::get<std::string>(val)) { node_match = false; break; }
                        }
                    }
                }
                if (!node_match) continue;
                
                ResultRow new_row = row;
                if (!next_node.variable.empty())
                    new_row.bindings[next_node.variable] = static_cast<int64_t>(target);
                if (!rel.variable.empty()) {
                    if (rel.variable_length) {
                        // 变长路径：绑定为列表
                        auto lst = std::make_shared<CypherList>();
                        for (auto eid : entry.edges)
                            lst->values.push_back(static_cast<int64_t>(eid));
                        new_row.bindings[rel.variable] = lst;
                    } else {
                        new_row.bindings[rel.variable] = static_cast<int64_t>(entry.edge_id);
                    }
                }
                next_result.push_back(new_row);
            }
        }
        
        result = next_result;
    }
    
    if (txn) db_->ResetSearchRoots();
    return result;
}

bool QueryExecutor::MatchNode(const NodePattern& node, VertexId id, ResultRow& row) {
    (void)node; (void)id; (void)row;
    return true;
}

bool QueryExecutor::MatchRelationship(const RelationshipPattern& rel, EdgeId id,
                                       VertexId from, VertexId to, ResultRow& row) {
    (void)rel; (void)id; (void)from; (void)to; (void)row;
    return true;
}

std::vector<QueryExecutor::BfsEntry> QueryExecutor::TraverseEdges(
    VertexId src, const RelationshipPattern& rel) {
    std::vector<BfsEntry> result;
    TxId eff_ts = GetEffTs();
    
    int max_hops = static_cast<int>(rel.max_hops.value_or(1));
    if (rel.variable_length && !rel.max_hops) max_hops = 10;
    
    // 非变长单跳: 直接查边
    if (!rel.variable_length && max_hops == 1) {
        auto handle_edge = [&](EdgeId eid, VertexId neighbor) {
            result.push_back({eid, neighbor, {eid}, {src, neighbor}});
        };
        if (rel.direction == Direction::OUT_DIR || rel.direction == Direction::BOTH) {
            for (auto eid : db_->GetOutEdges(eff_ts, src)) {
                auto edge = db_->GetEdge(eff_ts, eid);
                if (!edge) continue;
                if (!rel.types.empty()) {
                    bool ok = false;
                    for (const auto& t : rel.types)
                        if (static_cast<LabelId>(Fnv1a(t)) == edge->label) { ok = true; break; }
                    if (!ok) continue;
                }
                handle_edge(eid, edge->dst);
            }
        }
        if (rel.direction == Direction::IN_DIR || rel.direction == Direction::BOTH) {
            for (auto eid : db_->GetInEdges(eff_ts, src)) {
                auto edge = db_->GetEdge(eff_ts, eid);
                if (!edge) continue;
                if (!rel.types.empty()) {
                    bool ok = false;
                    for (const auto& t : rel.types)
                        if (static_cast<LabelId>(Fnv1a(t)) == edge->label) { ok = true; break; }
                    if (!ok) continue;
                }
                handle_edge(eid, edge->src);
            }
        }
        return result;
    }
    
    // 变长边 BFS — 收集所有路径
    struct State { VertexId vid; int depth; std::vector<EdgeId> edges; std::vector<VertexId> vertices; };
    std::vector<State> queue;
    // 起始状态：从 src 出发
    queue.push_back({src, 0, {}, {src}});
    size_t head = 0;
    int min_hops = static_cast<int>(rel.min_hops.value_or(1));
    
    while (head < queue.size()) {
        auto [cur, depth, cur_edges, cur_verts] = queue[head++];
        if (depth >= max_hops) continue;
        
        // 遍历边的 lambda
        auto try_edge = [&](EdgeId eid, const Edge& edge) {
            VertexId neighbor = (edge.src == cur) ? edge.dst : edge.src;
            int nd = depth + 1;
            
            auto new_edges = cur_edges;
            new_edges.push_back(eid);
            auto new_verts = cur_verts;
            new_verts.push_back(neighbor);
            
            if (nd >= min_hops && nd <= max_hops)
                result.push_back({eid, neighbor, new_edges, new_verts});
            if (nd < max_hops) {
                queue.push_back({neighbor, nd, new_edges, new_verts});
            }
        };
        
        auto traverse_direction = [&](const std::vector<EdgeId>& edges) {
            for (auto eid : edges) {
                auto edge = db_->GetEdge(eff_ts, eid);
                if (!edge) continue;
                if (!rel.types.empty()) {
                    bool ok = false;
                    for (const auto& t : rel.types)
                        if (static_cast<LabelId>(Fnv1a(t)) == edge->label) { ok = true; break; }
                    if (!ok) continue;
                }
                try_edge(eid, *edge);
            }
        };
        
        if (rel.direction == Direction::OUT_DIR || rel.direction == Direction::BOTH) {
            traverse_direction(db_->GetOutEdges(eff_ts, cur));
        }
        if (rel.direction == Direction::IN_DIR || rel.direction == Direction::BOTH) {
            traverse_direction(db_->GetInEdges(eff_ts, cur));
        }
    }
    
    return result;
}

TxId QueryExecutor::GetEffTs() const {
    TxId eff = read_ts_;
    if (explicit_tx_ && write_ts_ != 0) {
        auto* txn = db_->GetTransaction(write_ts_);
        // write_ver 是写事务的版本号，可直接用于版本比较（>= 读取时间戳即可见）
        if (txn && txn->write_ver > eff) eff = txn->write_ver;
    }
    return eff;
}

CypherValue QueryExecutor::Evaluate(const Expression& expr, const ResultRow& row) {
    switch (expr.type) {
        case Expression::Type::LITERAL:
            return expr.literal_value;
        case Expression::Type::VARIABLE:
            if (row.bindings.count(expr.variable_name)) {
                return row.bindings.at(expr.variable_name);
            }
            return std::monostate{};
        case Expression::Type::PROPERTY: {
            auto it = row.bindings.find(expr.variable_name);
            if (it == row.bindings.end()) return std::monostate{};
            uint64_t id = static_cast<uint64_t>(std::get<int64_t>(it->second));
            TxId eff_ts = GetEffTs();
            if (rel_vars_.count(expr.variable_name)) {
                auto prop = db_->GetEdgeProperty(eff_ts, id, expr.property_name);
                if (prop) return *prop;
            } else {
                auto prop = db_->GetVertexProperty(eff_ts, static_cast<VertexId>(id), expr.property_name);
                if (prop) return *prop;
            }
            return std::monostate{};
        }
        case Expression::Type::COMPARISON: {
            auto left = Evaluate(*expr.left, row);
            auto right = Evaluate(*expr.right, row);
            
            // IN 操作符：左值是否在右值列表中
            if (expr.comp_op == ComparisonOp::CYPHER_IN) {
                if (expr.right->type == Expression::Type::LIST) {
                    for (const auto& arg : expr.right->args) {
                        auto item = Evaluate(*arg, row);
                        if (left == item) return true;
                    }
                }
                return false;
            }
            
            // 统一为 string 或 int64
            auto normalize = [](const CypherValue& v) -> std::variant<std::monostate, int64_t, std::string> {
                if (std::holds_alternative<int64_t>(v)) return std::get<int64_t>(v);
                if (std::holds_alternative<std::string>(v)) return std::get<std::string>(v);
                if (std::holds_alternative<bool>(v)) return static_cast<int64_t>(std::get<bool>(v) ? 1 : 0);
                if (std::holds_alternative<double>(v)) return static_cast<int64_t>(std::get<double>(v));
                return std::monostate{};
            };
            
            auto lv = normalize(left);
            auto rv = normalize(right);
            if (std::holds_alternative<std::monostate>(lv) || std::holds_alternative<std::monostate>(rv))
                return false;
            
            if (std::holds_alternative<int64_t>(lv) && std::holds_alternative<int64_t>(rv)) {
                int64_t l = std::get<int64_t>(lv), r = std::get<int64_t>(rv);
                switch (expr.comp_op) {
                    case ComparisonOp::EQ: return l == r;
                    case ComparisonOp::NE: return l != r;
                    case ComparisonOp::LT: return l < r;
                    case ComparisonOp::LE: return l <= r;
                    case ComparisonOp::GT: return l > r;
                    case ComparisonOp::GE: return l >= r;
                    default: return false;
                }
            }
            
            if (std::holds_alternative<std::string>(lv) && std::holds_alternative<std::string>(rv)) {
                const std::string& l = std::get<std::string>(lv);
                const std::string& r = std::get<std::string>(rv);
                switch (expr.comp_op) {
                    case ComparisonOp::EQ: return l == r;
                    case ComparisonOp::NE: return l != r;
                    case ComparisonOp::LT: return l < r;
                    case ComparisonOp::LE: return l <= r;
                    case ComparisonOp::GT: return l > r;
                    case ComparisonOp::GE: return l >= r;
                    case ComparisonOp::STARTS_WITH: return l.rfind(r, 0) == 0;
                    case ComparisonOp::ENDS_WITH: return l.size() >= r.size() && 
                        l.compare(l.size() - r.size(), r.size(), r) == 0;
                    case ComparisonOp::CONTAINS: return l.find(r) != std::string::npos;
                    default: return false;
                }
            }
            
            // 跨类型：string ↔ int64 — 尝试转 int64
            auto to_int = [](const auto& v) -> std::optional<int64_t> {
                if constexpr (std::is_same_v<std::decay_t<decltype(v)>, int64_t>) return v;
                else {
                    try { return std::stoll(v); } catch (...) { return std::nullopt; }
                }
            };
            if (std::holds_alternative<int64_t>(lv) && std::holds_alternative<std::string>(rv)) {
                auto ri = to_int(std::get<std::string>(rv));
                if (!ri) return false;
                int64_t l = std::get<int64_t>(lv), r = *ri;
                switch (expr.comp_op) {
                    case ComparisonOp::EQ: return l == r;
                    case ComparisonOp::NE: return l != r;
                    case ComparisonOp::LT: return l < r;
                    case ComparisonOp::LE: return l <= r;
                    case ComparisonOp::GT: return l > r;
                    case ComparisonOp::GE: return l >= r;
                    default: return false;
                }
            }
            if (std::holds_alternative<std::string>(lv) && std::holds_alternative<int64_t>(rv)) {
                auto li = to_int(std::get<std::string>(lv));
                if (!li) return false;
                int64_t l = *li, r = std::get<int64_t>(rv);
                switch (expr.comp_op) {
                    case ComparisonOp::EQ: return l == r;
                    case ComparisonOp::NE: return l != r;
                    case ComparisonOp::LT: return l < r;
                    case ComparisonOp::LE: return l <= r;
                    case ComparisonOp::GT: return l > r;
                    case ComparisonOp::GE: return l >= r;
                    default: return false;
                }
            }
            
            return false;
        }
        case Expression::Type::LOGICAL: {
            if (expr.logical_op == LogicalOp::NOT) {
                return !EvaluateBoolean(*expr.left, row);
            }
            bool left = EvaluateBoolean(*expr.left, row);
            bool right = EvaluateBoolean(*expr.right, row);
            if (expr.logical_op == LogicalOp::AND) return left && right;
            if (expr.logical_op == LogicalOp::OR) return left || right;
            return false;
        }
        case Expression::Type::LIST: {
            auto lst = std::make_shared<CypherList>();
            for (const auto& arg : expr.args)
                lst->values.push_back(Evaluate(*arg, row));
            return lst;
        }
        case Expression::Type::AGGREGATION: {
            // 返回内部表达式的值，供 ApplyReturn 聚合
            if (!expr.args.empty())
                return Evaluate(*expr.args[0], row);
            return static_cast<int64_t>(0);
        }
        case Expression::Type::FUNCTION_CALL: {
            if (expr.function_name == "EXISTS") {
                if (expr.args.empty()) return false;
                auto val = Evaluate(*expr.args[0], row);
                return !std::holds_alternative<std::monostate>(val);
            }
            if (expr.function_name == "CASE") {
                if (expr.args.empty()) return std::monostate{};
                size_t start = expr.case_is_simple ? 1 : 0;
                CypherValue case_val;
                if (expr.case_is_simple) case_val = Evaluate(*expr.args[0], row);
                for (size_t i = start; i + 1 < expr.args.size(); i += 2) {
                    if (expr.case_is_simple) {
                        auto when_val = Evaluate(*expr.args[i], row);
                        if (case_val == when_val) return Evaluate(*expr.args[i + 1], row);
                    } else {
                        if (EvaluateBoolean(*expr.args[i], row))
                            return Evaluate(*expr.args[i + 1], row);
                    }
                }
                // ELSE
                if (expr.args.size() % 2 != start)
                    return Evaluate(*expr.args.back(), row);
                return std::monostate{};
            }
            return std::monostate{};
        }
        default:
            return std::monostate{};
    }
}

bool QueryExecutor::EvaluateBoolean(const Expression& expr, const ResultRow& row) {
    auto val = Evaluate(expr, row);
    if (std::holds_alternative<bool>(val)) return std::get<bool>(val);
    return false;
}

std::vector<QueryExecutor::ResultRow> QueryExecutor::ApplyWhere(
    const Expression& condition, const std::vector<ResultRow>& rows) {
    std::vector<ResultRow> result;
    for (const auto& row : rows) {
        if (EvaluateBoolean(condition, row)) {
            result.push_back(row);
        }
    }
    return result;
}

QueryExecutor::ResultSet QueryExecutor::ApplyReturn(
    const ReturnClause& ret, const std::vector<ResultRow>& rows) {
    ResultSet result;
    
    if (ret.all) {
        // RETURN *: 返回所有绑定变量
        if (!rows.empty()) {
            for (const auto& [name, val] : rows[0].bindings) {
                result.columns.push_back(name);
            }
        }
        result.rows = rows;
        return result;
    }
    
    // 检查是否有聚合表达式
    bool has_aggregation = false;
    for (const auto& item : ret.items) {
        if (item.expression->type == Expression::Type::AGGREGATION) {
            has_aggregation = true;
            break;
        }
    }
    
    if (has_aggregation) {
        // 聚合模式：对全体行计算聚合值，返回单行
        for (const auto& item : ret.items) {
            std::string key = item.alias.empty() ?
                (item.expression->variable_name.empty() ? item.expression->agg_func : item.expression->variable_name) : item.alias;
            result.columns.push_back(key);
        }
        
        ResultRow agg_row;
        for (const auto& item : ret.items) {
            std::string key = item.alias.empty() ?
                (item.expression->variable_name.empty() ? item.expression->agg_func : item.expression->variable_name) : item.alias;
            
            if (item.expression->type == Expression::Type::AGGREGATION) {
                const auto& func = item.expression->agg_func;
                if (func == "COUNT") {
                    agg_row.bindings[key] = static_cast<int64_t>(rows.size());
                } else if (func == "SUM") {
                    int64_t sum = 0;
                    for (const auto& row : rows) {
                        auto val = Evaluate(*item.expression, row);
                        if (std::holds_alternative<int64_t>(val)) sum += std::get<int64_t>(val);
                    }
                    agg_row.bindings[key] = sum;
                } else if (func == "AVG") {
                    int64_t sum = 0, count = 0;
                    for (const auto& row : rows) {
                        auto val = Evaluate(*item.expression, row);
                        if (std::holds_alternative<int64_t>(val)) { sum += std::get<int64_t>(val); count++; }
                    }
                    if (count > 0)
                        agg_row.bindings[key] = static_cast<double>(sum) / count;
                    else
                        agg_row.bindings[key] = std::monostate{};
                } else if (func == "MIN") {
                    CypherValue min_val;
                    bool first = true;
                    for (const auto& row : rows) {
                        auto val = Evaluate(*item.expression, row);
                        if (std::holds_alternative<std::monostate>(val)) continue;
                        if (first) { min_val = val; first = false; continue; }
                        auto normalize = [](const CypherValue& v) -> std::variant<std::monostate, int64_t, std::string> {
                            if (std::holds_alternative<int64_t>(v)) return std::get<int64_t>(v);
                            if (std::holds_alternative<std::string>(v)) return std::get<std::string>(v);
                            return std::monostate{};
                        };
                        auto lv = normalize(min_val), rv = normalize(val);
                        if (std::holds_alternative<int64_t>(lv) && std::holds_alternative<int64_t>(rv))
                            if (std::get<int64_t>(rv) < std::get<int64_t>(lv)) min_val = val;
                        if (std::holds_alternative<std::string>(lv) && std::holds_alternative<std::string>(rv))
                            if (std::get<std::string>(rv) < std::get<std::string>(lv)) min_val = val;
                    }
                    agg_row.bindings[key] = first ? std::monostate{} : min_val;
                } else if (func == "MAX") {
                    CypherValue max_val;
                    bool first = true;
                    for (const auto& row : rows) {
                        auto val = Evaluate(*item.expression, row);
                        if (std::holds_alternative<std::monostate>(val)) continue;
                        if (first) { max_val = val; first = false; continue; }
                        auto normalize = [](const CypherValue& v) -> std::variant<std::monostate, int64_t, std::string> {
                            if (std::holds_alternative<int64_t>(v)) return std::get<int64_t>(v);
                            if (std::holds_alternative<std::string>(v)) return std::get<std::string>(v);
                            return std::monostate{};
                        };
                        auto lv = normalize(max_val), rv = normalize(val);
                        if (std::holds_alternative<int64_t>(lv) && std::holds_alternative<int64_t>(rv))
                            if (std::get<int64_t>(rv) > std::get<int64_t>(lv)) max_val = val;
                        if (std::holds_alternative<std::string>(lv) && std::holds_alternative<std::string>(rv))
                            if (std::get<std::string>(rv) > std::get<std::string>(lv)) max_val = val;
                    }
                    agg_row.bindings[key] = first ? std::monostate{} : max_val;
                } else {
                    agg_row.bindings[key] = std::monostate{};
                }
            } else {
                // 非聚合列取第一行值
                if (!rows.empty())
                    agg_row.bindings[key] = Evaluate(*item.expression, rows[0]);
                else
                    agg_row.bindings[key] = std::monostate{};
            }
        }
        result.rows.push_back(agg_row);
        return result;
    }
    
    // 非聚合：逐行求值
    for (const auto& item : ret.items) {
        result.columns.push_back(item.alias.empty() ? 
            (item.expression->variable_name + "." + item.expression->property_name) : item.alias);
    }
    
    for (const auto& row : rows) {
        ResultRow new_row;
        for (const auto& item : ret.items) {
            auto val = Evaluate(*item.expression, row);
            std::string key = item.alias.empty() ? 
                item.expression->variable_name : item.alias;
            new_row.bindings[key] = val;
        }
        result.rows.push_back(new_row);
    }
    
    return result;
}

std::vector<QueryExecutor::ResultRow> QueryExecutor::ApplyWithProjection(
    const WithClause& with, const std::vector<ResultRow>& rows) {
    if (with.items.empty()) return rows;
    std::vector<ResultRow> projected;
    for (const auto& row : rows) {
        ResultRow new_row;
        for (const auto& item : with.items) {
            auto val = Evaluate(*item.expression, row);
            std::string key = item.alias.empty() ?
                item.expression->variable_name : item.alias;
            new_row.bindings[key] = val;
        }
        projected.push_back(new_row);
    }
    if (with.where) {
        projected = ApplyWhere(*with.where, projected);
    }
    if (with.distinct) {
        std::vector<ResultRow> deduped;
        for (auto& row : projected) {
            bool dup = false;
            for (auto& existing : deduped) {
                if (row.bindings == existing.bindings) { dup = true; break; }
            }
            if (!dup) deduped.push_back(std::move(row));
        }
        projected = std::move(deduped);
    }
    return projected;
}

std::vector<QueryExecutor::ResultRow> QueryExecutor::ApplyOrderBy(
    const std::vector<OrderByItem>& order_by, const std::vector<ResultRow>& rows) {
    if (order_by.empty() || rows.size() <= 1) return rows;
    
    // 预计算排序键
    struct SortRow {
        const ResultRow* row;
        CypherValue key;
    };
    std::vector<SortRow> sorted;
    sorted.reserve(rows.size());
    for (const auto& row : rows) {
        sorted.push_back({&row, Evaluate(*order_by[0].expression, row)});
    }
    
    // 比较函数（支持 int64、string、跨类型）
    auto less = [&](const SortRow& a, const SortRow& b) -> bool {
        const auto& l = a.key;
        const auto& r = b.key;
        
        auto to_int = [](const CypherValue& v) -> std::optional<int64_t> {
            if (std::holds_alternative<int64_t>(v)) return std::get<int64_t>(v);
            if (std::holds_alternative<std::string>(v)) {
                try { return std::stoll(std::get<std::string>(v)); } catch (...) {}
            }
            return std::nullopt;
        };
        
        auto li = to_int(l), ri = to_int(r);
        if (li && ri) return order_by[0].ascending ? (*li < *ri) : (*li > *ri);
        
        if (std::holds_alternative<std::string>(l) && std::holds_alternative<std::string>(r)) {
            return order_by[0].ascending
                ? (std::get<std::string>(l) < std::get<std::string>(r))
                : (std::get<std::string>(l) > std::get<std::string>(r));
        }
        
        return false;
    };
    
    std::sort(sorted.begin(), sorted.end(), less);
    
    std::vector<ResultRow> result;
    result.reserve(sorted.size());
    for (const auto& sr : sorted)
        result.push_back(*sr.row);
    return result;
}

std::vector<QueryExecutor::ResultRow> QueryExecutor::ApplySkipLimit(
    const std::optional<size_t>& skip, const std::optional<size_t>& limit,
    const std::vector<ResultRow>& rows) {
    size_t start = skip.value_or(0);
    size_t end = rows.size();
    if (limit) end = std::min(end, start + *limit);
    if (start >= rows.size()) return {};
    return std::vector<ResultRow>(rows.begin() + start, rows.begin() + end);
}

QueryExecutor::ResultSet QueryExecutor::ExecuteCreate(const CypherQuery& query) {
    ResultSet result;
    if (!query.create) return result;
    
    TxId write_ts = GetWriteTx();
    
    for (const auto& pattern : query.create->patterns) {
        if (pattern.nodes.empty()) continue;
        
        ResultRow row;
        // 1) 创建所有节点，记录 vid 映射
        std::unordered_map<std::string, VertexId> node_vids;
        for (size_t ni = 0; ni < pattern.nodes.size(); ni++) {
            const auto& np = pattern.nodes[ni];
            
            LabelId label = 0;
            if (!np.labels.empty())
                label = static_cast<LabelId>(Fnv1a(np.labels[0]));
            
            Properties props;
            for (const auto& [k, v] : np.properties) {
                if (std::holds_alternative<std::string>(v))
                    props.Set(k, std::get<std::string>(v));
            }
            
            VertexId vid = db_->AddVertex(write_ts, label, props);
            if (vid == 0) continue;
            if (!np.variable.empty()) {
                node_vids[np.variable] = vid;
                row.bindings[np.variable] = static_cast<int64_t>(vid);
            }
        }
        
        // 2) 创建所有关系边
        for (size_t ri = 0; ri < pattern.relationships.size() && ri + 1 < pattern.nodes.size(); ri++) {
            const auto& rel = pattern.relationships[ri];
            const auto& src_node = pattern.nodes[ri];
            const auto& dst_node = pattern.nodes[ri + 1];
            
            VertexId src_vid = 0, dst_vid = 0;
            auto sit = node_vids.find(src_node.variable);
            auto dit = node_vids.find(dst_node.variable);
            if (sit == node_vids.end() || dit == node_vids.end()) continue;
            src_vid = sit->second;
            dst_vid = dit->second;
            
            // 方向：IN_DIR 时交换 src/dst
            if (rel.direction == Direction::IN_DIR)
                std::swap(src_vid, dst_vid);
            
            LabelId edge_label = 0;
            if (!rel.types.empty())
                edge_label = static_cast<LabelId>(Fnv1a(rel.types[0]));
            
            Properties edge_props;
            for (const auto& [k, v] : rel.properties) {
                if (std::holds_alternative<std::string>(v))
                    edge_props.Set(k, std::get<std::string>(v));
            }
            
            EdgeId eid = db_->AddEdge(write_ts, src_vid, dst_vid, edge_label, edge_props);
            if (eid > 0 && !rel.variable.empty())
                row.bindings[rel.variable] = static_cast<int64_t>(eid);
        }
        
        if (!row.bindings.empty())
            result.rows.push_back(row);
    }
    
    if (!explicit_tx_) {
        db_->Commit(write_ts);
        write_ts_ = 0;
    }
    
    if (!result.rows.empty()) {
        for (const auto& [var, val] : result.rows[0].bindings) {
            result.columns.push_back(var);
        }
    }
    
    return result;
}

static VertexId FindVertexByPattern(GraphStore* db, TxId read_ts,
                                      const NodePattern& np) {
    LabelId label = 0;
    if (!np.labels.empty())
        label = static_cast<LabelId>(Fnv1a(np.labels[0]));
    
    if (np.properties.empty()) {
        // 无属性约束：按标签查找
        if (!np.labels.empty()) {
            auto vids = db->GetVerticesByLabel(read_ts, label);
            return vids.empty() ? 0 : vids[0];
        }
        return 0;
    }
    
    // 用属性索引查找：取第一个属性作为索引查询，然后在内存中过滤剩余属性
    auto it = np.properties.begin();
    if (!std::holds_alternative<std::string>(it->second))
        return 0;
    const std::string& first_key = it->first;
    const std::string& first_val = std::get<std::string>(it->second);
    
    auto candidates = db->GetVerticesByProperty(read_ts, label, first_key, first_val);
    for (auto vid : candidates) {
        bool match = true;
        for (const auto& [k, v] : np.properties) {
            if (k == first_key) continue;
            auto prop = db->GetVertexProperty(read_ts, vid, k);
            if (!prop) { match = false; break; }
            if (std::holds_alternative<std::string>(v) && *prop != std::get<std::string>(v))
                { match = false; break; }
        }
        if (match) return vid;
    }
    return 0;
}

static EdgeId FindEdgeBetween(GraphStore* db, TxId read_ts,
                               VertexId src, VertexId dst,
                               const RelationshipPattern& rel) {
    LabelId edge_label = 0;
    if (!rel.types.empty())
        edge_label = static_cast<LabelId>(Fnv1a(rel.types[0]));
    
    auto out_edges = db->GetOutEdges(read_ts, src);
    for (auto eid : out_edges) {
        auto edge = db->GetEdge(read_ts, eid);
        if (!edge || edge->dst != dst) continue;
        if (!rel.types.empty() && edge->label != edge_label) continue;
        
        bool props_match = true;
        for (const auto& [k, v] : rel.properties) {
            auto prop = db->GetEdgeProperty(read_ts, eid, k);
            if (!prop) { props_match = false; break; }
            if (std::holds_alternative<std::string>(v) && *prop != std::get<std::string>(v))
                { props_match = false; break; }
        }
        if (props_match) return eid;
    }
    return 0;
}

QueryExecutor::ResultSet QueryExecutor::ExecuteMerge(const CypherQuery& query) {
    ResultSet result;
    if (!query.merge) return result;
    
    TxId write_ts = GetWriteTx();
    bool created = false;
    
    auto apply_set = [&](ResultRow& row, const SetClause& sc) {
        for (const auto& item : sc.items) {
            if (item.add_label) {
                auto it = row.bindings.find(item.target->variable_name);
                if (it == row.bindings.end()) continue;
                VertexId vid = static_cast<VertexId>(std::get<int64_t>(it->second));
                LabelId label_id = static_cast<LabelId>(Fnv1a(item.label));
                db_->AddVertexLabel(write_ts, vid, label_id);
            } else if (item.target && item.target->type == Expression::Type::PROPERTY) {
                auto it = row.bindings.find(item.target->variable_name);
                if (it == row.bindings.end()) continue;
                VertexId vid = static_cast<VertexId>(std::get<int64_t>(it->second));
                auto val = Evaluate(*item.value, row);
                if (std::holds_alternative<std::string>(val)) {
                    db_->SetVertexProperty(write_ts, vid, item.target->property_name,
                                           std::get<std::string>(val));
                }
            }
        }
    };
    
    for (const auto& pattern : query.merge->patterns) {
        if (pattern.nodes.empty()) continue;
        
        // 1) 先尝试 MATCH 整个模式
        auto matched = MatchPattern(pattern, {});
        
        if (!matched.empty()) {
            if (query.merge->on_match.has_value()) {
                for (auto& row : matched) {
                    apply_set(row, *query.merge->on_match);
                }
            }
            result.rows.insert(result.rows.end(), matched.begin(), matched.end());
            continue;
        }
        
        // 2) 逐节点 MERGE：MATCH 已存在 → 重用；不存在 → CREATE
        ResultRow row;
        std::unordered_map<std::string, VertexId> node_vids;
        
        for (size_t ni = 0; ni < pattern.nodes.size(); ni++) {
            const auto& np = pattern.nodes[ni];
            
            LabelId label = 0;
            if (!np.labels.empty())
                label = static_cast<LabelId>(Fnv1a(np.labels[0]));
            
            // 尝试 MATCH 该节点
            VertexId existing = FindVertexByPattern(db_, write_ts, np);
            
            if (existing != 0) {
                node_vids[np.variable] = existing;
                row.bindings[np.variable] = static_cast<int64_t>(existing);
            } else {
                // 不存在 → CREATE
                Properties props;
                for (const auto& [k, v] : np.properties) {
                    if (std::holds_alternative<std::string>(v))
                        props.Set(k, std::get<std::string>(v));
                }
                VertexId vid = db_->AddVertex(write_ts, label, props);
                if (vid == 0) continue;
                if (!np.variable.empty()) {
                    node_vids[np.variable] = vid;
                    row.bindings[np.variable] = static_cast<int64_t>(vid);
                }
            }
        }
        
        // 3) 逐关系 MERGE：在已解析的两端节点间寻找已存在的边
        for (size_t ri = 0; ri < pattern.relationships.size() && ri + 1 < pattern.nodes.size(); ri++) {
            const auto& rel = pattern.relationships[ri];
            const auto& src_node = pattern.nodes[ri];
            const auto& dst_node = pattern.nodes[ri + 1];
            
            VertexId src_vid = 0, dst_vid = 0;
            auto sit = node_vids.find(src_node.variable);
            auto dit = node_vids.find(dst_node.variable);
            if (sit == node_vids.end() || dit == node_vids.end()) continue;
            src_vid = sit->second;
            dst_vid = dit->second;
            
            if (rel.direction == Direction::IN_DIR)
                std::swap(src_vid, dst_vid);
            
            // 尝试 MATCH 该边
            EdgeId existing_eid = FindEdgeBetween(db_, write_ts, src_vid, dst_vid, rel);
            
            if (existing_eid != 0) {
                if (!rel.variable.empty())
                    row.bindings[rel.variable] = static_cast<int64_t>(existing_eid);
            } else {
                // 不存在 → CREATE
                LabelId edge_label = 0;
                if (!rel.types.empty())
                    edge_label = static_cast<LabelId>(Fnv1a(rel.types[0]));
                
                Properties edge_props;
                for (const auto& [k, v] : rel.properties) {
                    if (std::holds_alternative<std::string>(v))
                        edge_props.Set(k, std::get<std::string>(v));
                }
                
                EdgeId eid = db_->AddEdge(write_ts, src_vid, dst_vid, edge_label, edge_props);
                if (eid > 0 && !rel.variable.empty())
                    row.bindings[rel.variable] = static_cast<int64_t>(eid);
            }
        }
        
        created = true;
        if (!row.bindings.empty()) {
            if (query.merge->on_create.has_value()) {
                apply_set(row, *query.merge->on_create);
            }
            result.rows.push_back(row);
        }
    }
    
    if (created && !explicit_tx_) {
        db_->Commit(write_ts);
        write_ts_ = 0;
    }
    
    if (!result.rows.empty()) {
        for (const auto& [var, val] : result.rows[0].bindings) {
            result.columns.push_back(var);
        }
    }
    
    return result;
}

QueryExecutor::ResultSet QueryExecutor::ExecuteSet(const CypherQuery& query) {
    ResultSet result;
    
    // 先 MATCH
    std::vector<ResultRow> rows;
    if (!query.match_clauses.empty() && !query.match_clauses[0].patterns.empty()) {
        rows = MatchPattern(query.match_clauses[0].patterns[0], {});
        if (query.where) {
            rows = ApplyWhere(*query.where->condition, rows);
        }
    }
    
    if (rows.empty()) return result;
    
    TxId write_ts = GetWriteTx();
    
    for (auto& row : rows) {
        for (const auto& item : query.set->items) {
            if (item.add_label) {
                auto it = row.bindings.find(item.target->variable_name);
                if (it == row.bindings.end()) continue;
                VertexId vid = static_cast<VertexId>(std::get<int64_t>(it->second));
                LabelId label_id = static_cast<LabelId>(Fnv1a(item.label));
                db_->AddVertexLabel(write_ts, vid, label_id);
            } else if (item.target && item.target->type == Expression::Type::PROPERTY) {
                // SET n.prop = value
                auto it = row.bindings.find(item.target->variable_name);
                if (it == row.bindings.end()) continue;
                VertexId vid = static_cast<VertexId>(std::get<int64_t>(it->second));
                
                auto val = Evaluate(*item.value, row);
                if (std::holds_alternative<std::string>(val)) {
                    db_->SetVertexProperty(write_ts, vid, item.target->property_name,
                                           std::get<std::string>(val));
                }
            }
        }
    }
    
    if (!explicit_tx_) {
        db_->Commit(write_ts);
        write_ts_ = 0;
    }
    
    // 重新 MATCH 以获得更新后的结果用于 RETURN
    std::vector<ResultRow> updated;
    if (!query.match_clauses.empty() && !query.match_clauses[0].patterns.empty()) {
        updated = MatchPattern(query.match_clauses[0].patterns[0], {});
        if (query.where) {
            updated = ApplyWhere(*query.where->condition, updated);
        }
    }
    result = ApplyReturn(query.ret, updated);
    
    return result;
}

QueryExecutor::ResultSet QueryExecutor::ExecuteDelete(const CypherQuery& query) {
    ResultSet result;
    
    // 先 MATCH
    std::vector<ResultRow> rows;
    if (!query.match_clauses.empty() && !query.match_clauses[0].patterns.empty()) {
        rows = MatchPattern(query.match_clauses[0].patterns[0], {});
        if (query.where) {
            rows = ApplyWhere(*query.where->condition, rows);
        }
    }
    
    if (rows.empty()) return result;
    
    TxId write_ts = GetWriteTx();
    
    for (const auto& row : rows) {
        for (const auto& expr : query.del->expressions) {
            if (expr->type == Expression::Type::VARIABLE) {
                auto it = row.bindings.find(expr->variable_name);
                if (it == row.bindings.end()) continue;
                uint64_t id = static_cast<uint64_t>(std::get<int64_t>(it->second));
                
                if (rel_vars_.count(expr->variable_name))
                    db_->RemoveEdge(write_ts, id);
                else
                    db_->RemoveVertex(write_ts, static_cast<VertexId>(id));
            }
        }
    }
    
    if (!explicit_tx_) {
        db_->Commit(write_ts);
        write_ts_ = 0;
    }
    
    result = ApplyReturn(query.ret, {});
    
    return result;
}

QueryExecutor::ResultSet QueryExecutor::ExecuteRemove(const CypherQuery& query) {
    ResultSet result;
    
    // 先 MATCH
    std::vector<ResultRow> rows;
    if (!query.match_clauses.empty() && !query.match_clauses[0].patterns.empty()) {
        rows = MatchPattern(query.match_clauses[0].patterns[0], {});
        if (query.where) {
            rows = ApplyWhere(*query.where->condition, rows);
        }
    }
    
    if (rows.empty()) return result;
    
    TxId write_ts = GetWriteTx();
    
    for (const auto& row : rows) {
        for (const auto& item : query.remove->items) {
            if (item.is_label) {
                auto it = row.bindings.find(item.target->variable_name);
                if (it == row.bindings.end()) continue;
                VertexId vid = static_cast<VertexId>(std::get<int64_t>(it->second));
                LabelId label_id = static_cast<LabelId>(Fnv1a(item.label));
                db_->RemoveVertexLabel(write_ts, vid, label_id);
            } else if (item.target->type == Expression::Type::PROPERTY) {
                auto it = row.bindings.find(item.target->variable_name);
                if (it == row.bindings.end()) continue;
                uint64_t id = static_cast<uint64_t>(std::get<int64_t>(it->second));
                if (rel_vars_.count(item.target->variable_name))
                    db_->RemoveEdgeProperty(write_ts, static_cast<EdgeId>(id), item.target->property_name);
                else
                    db_->RemoveVertexProperty(write_ts, static_cast<VertexId>(id), item.target->property_name);
            }
        }
    }
    
    if (!explicit_tx_) {
        db_->Commit(write_ts);
        write_ts_ = 0;
    }
    
    result = ApplyReturn(query.ret, rows);
    
    return result;
}

void QueryExecutor::BuildRelVars(const CypherQuery& query) {
    rel_vars_.clear();
    for (const auto& mc : query.match_clauses) {
        for (const auto& pat : mc.patterns) {
            for (const auto& rel : pat.relationships) {
                if (!rel.variable.empty()) rel_vars_.insert(rel.variable);
            }
        }
    }
}

} // namespace graphstore

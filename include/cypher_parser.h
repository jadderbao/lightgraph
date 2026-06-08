#ifndef CYPHER_PARSER_H
#define CYPHER_PARSER_H

#include "common.h"
#include <string>
#include <vector>
#include <memory>
#include <variant>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace graphstore {

class GraphStore;
// ==================== Cypher AST 节点 ====================

// 值类型（用于 WHERE 子句和属性值）
struct CypherList;
using CypherValue = std::variant<std::monostate, int64_t, double, bool, std::string, std::shared_ptr<CypherList>>;
struct CypherList { std::vector<CypherValue> values; };

// 比较操作符
enum class ComparisonOp {
    EQ, NE, LT, LE, GT, GE,
    STARTS_WITH, ENDS_WITH, CONTAINS,
    IS_NULL, IS_NOT_NULL, CYPHER_IN
};

// 逻辑操作符
enum class LogicalOp { AND, OR, NOT };

// 方向
enum class Direction { OUT_DIR, IN_DIR, BOTH };

// -------------------- 表达式 --------------------
struct Expression {
    enum class Type {
        LITERAL,        // 字面量: 42, "hello"
        PROPERTY,       // 属性访问: n.name
        VARIABLE,       // 变量: n
        PARAMETER,      // 参数: $param
        COMPARISON,     // 比较: n.age > 30
        LOGICAL,        // 逻辑: AND, OR, NOT
        FUNCTION_CALL,  // 函数: count(n), exists(n.name)
        LIST,           // 列表: [1, 2, 3]
        PATTERN,        // 模式: (a)-[:KNOWS]->(b)
        AGGREGATION     // 聚合: COUNT, SUM, AVG, MIN, MAX
    };
    
    Type type;
    CypherValue literal_value;                    // LITERAL
    std::string variable_name;                    // VARIABLE / PROPERTY 的变量名
    std::string property_name;                    // PROPERTY 的属性名
    std::string param_name;                       // PARAMETER
    std::string function_name;                    // FUNCTION_CALL
    ComparisonOp comp_op;                         // COMPARISON
    LogicalOp logical_op;                         // LOGICAL
    std::vector<std::shared_ptr<Expression>> args; // 函数参数 / 逻辑操作数 / 列表元素
    std::shared_ptr<Expression> left;             // 二元操作左操作数
    std::shared_ptr<Expression> right;            // 二元操作右操作数
    std::string agg_func;                         // AGGREGATION: COUNT, SUM, etc.
    bool distinct = false;                        // COUNT(DISTINCT n)
    bool case_is_simple = false;                  // CASE expr WHEN ...
};

// -------------------- 模式元素 --------------------
struct NodePattern {
    std::string variable;           // 变量名，如 "n"
    std::vector<std::string> labels; // 标签，如 ["Person"]
    std::unordered_map<std::string, CypherValue> properties; // {name: "Alice", age: 30}
};

struct RelationshipPattern {
    std::string variable;
    std::vector<std::string> types;  // [:KNOWS], [:FRIEND|COLLEAGUE]
    std::unordered_map<std::string, CypherValue> properties;
    Direction direction = Direction::BOTH;
    std::optional<size_t> min_hops;  // *1..5 中的 1
    std::optional<size_t> max_hops;  // *1..5 中的 5
    bool variable_length = false;    // 是否有 * 变长
};

struct PathPattern {
    std::vector<NodePattern> nodes;
    std::vector<RelationshipPattern> relationships;
};

// -------------------- 子句 --------------------
struct MatchClause {
    bool optional = false;           // OPTIONAL MATCH
    std::vector<PathPattern> patterns;
};

struct WhereClause {
    std::shared_ptr<Expression> condition;
};

struct ReturnClause {
    struct ReturnItem {
        std::shared_ptr<Expression> expression;
        std::string alias;           // AS 别名
    };
    std::vector<ReturnItem> items;
    bool distinct = false;
    bool all = false;                // RETURN *
    std::optional<std::shared_ptr<Expression>> order_by;
    bool ascending = true;
    std::optional<size_t> skip;
    std::optional<size_t> limit;
};

struct CreateClause {
    std::vector<PathPattern> patterns;
};

struct SetClause {
    struct SetItem {
        std::shared_ptr<Expression> target;   // n.name
        std::shared_ptr<Expression> value;      // "Alice"
        bool add_label = false;               // SET n:Person
        std::string label;                    // 新增标签
    };
    std::vector<SetItem> items;
};

struct MergeClause {
    std::vector<PathPattern> patterns;
    std::optional<SetClause> on_match;     // ON MATCH SET ...
    std::optional<SetClause> on_create;    // ON CREATE SET ...
};

struct DeleteClause {
    std::vector<std::shared_ptr<Expression>> expressions;
    bool detach = false;             // DETACH DELETE
};

struct RemoveClause {
    struct RemoveItem {
        std::shared_ptr<Expression> target;
        bool is_label = false;
        std::string label;
    };
    std::vector<RemoveItem> items;
};

struct WithClause {
    std::vector<ReturnClause::ReturnItem> items;  // 支持多投影列
    bool distinct = false;                        // WITH DISTINCT
    std::shared_ptr<Expression> where;
};

struct UnwindClause {
    std::shared_ptr<Expression> expression;  // [1, 2, 3]
    std::string variable;                    // AS x
};

struct OrderByItem {
    std::shared_ptr<Expression> expression;
    bool ascending = true;
};

// -------------------- 完整查询 --------------------
struct CypherQuery {
    std::vector<MatchClause> match_clauses;
    std::vector<std::shared_ptr<WithClause>> with_clauses;
    std::vector<std::shared_ptr<UnwindClause>> unwind_clauses;
    std::optional<WhereClause> where;
    std::optional<CreateClause> create;
    std::optional<MergeClause> merge;
    std::optional<SetClause> set;
    std::optional<DeleteClause> del;
    std::optional<RemoveClause> remove;
    ReturnClause ret;
    std::vector<OrderByItem> order_by;
    std::optional<size_t> skip;
    std::optional<size_t> limit;
};

// ==================== 词法单元 ====================
struct Token {
    enum class Type {
        // 关键字
        MATCH, OPTIONAL_TYPE, WHERE, RETURN, WITH, UNWIND, AS,
        CREATE, MERGE, SET, DELETE_TYPE, DETACH, REMOVE, ON,
        ORDER, BY, ASC, DESC, SKIP, LIMIT,
        AND, OR, NOT, IN_TYPE, IS, NULL_, TRUE_TYPE, FALSE_TYPE,
        DISTINCT, COUNT, SUM, AVG, MIN, MAX, EXISTS, COLLECT,
        CASE, WHEN, THEN, ELSE, END,
        // 符号
        LPAREN, RPAREN, LBRACKET, RBRACKET, LBRACE, RBRACE,
        COLON, COMMA, DOT, SEMICOLON,
        ARROW_RIGHT, ARROW_LEFT, DASH, PIPE,
        STAR, SLASH, PERCENT, PLUS, MINUS,
        EQ, NE, LT, LE, GT, GE, ASSIGN,
        ELLIPSIS,                        // ..
        // 字面量
        IDENTIFIER, STRING, INTEGER, FLOAT, PARAMETER,
        // 特殊
        EOF_, INVALID
    };
    
    Type type;
    std::string text;
    size_t line = 1;
    size_t column = 1;
};

// ==================== 词法分析器 ====================
class Lexer {
public:
    explicit Lexer(const std::string& input);
    
    Token NextToken();
    Token PeekToken(size_t offset = 0);
    bool IsAtEnd() const;
    
private:
    std::string input_;
    size_t pos_ = 0;
    size_t line_ = 1;
    size_t column_ = 1;
    std::vector<Token> lookahead_;
    
    Token NextTokenFromInput(); // read from input directly, no lookahead check
    void SkipWhitespace();
    void SkipComment();
    Token ReadIdentifierOrKeyword();
    Token ReadString();
    Token ReadNumber();
    Token ReadParameter();
    Token MakeToken(Token::Type type, const std::string& text);
    char Current() const;
    char Advance();
    bool Match(char expected);
    bool IsAlpha(char c) const;
    bool IsDigit(char c) const;
    bool IsAlnum(char c) const;
};

// ==================== 语法分析器 ====================
class Parser {
public:
    explicit Parser(const std::string& input);
    
    // 解析入口
    std::optional<CypherQuery> ParseQuery();
    
    // 错误信息
    std::string GetError() const { return error_; }
    
private:
    Lexer lexer_;
    Token current_;
    std::string error_;
    
    void Advance();
    bool Match(Token::Type type);
    bool Check(Token::Type type) const;
    Token Consume(Token::Type type, const std::string& message);
    void Error(const std::string& message);
    
    // 子句解析
    CypherQuery ParseCypherQuery();
    MatchClause ParseMatchClause();
    WhereClause ParseWhereClause();
    ReturnClause ParseReturnClause();
    CreateClause ParseCreateClause();
    MergeClause ParseMergeClause();
    SetClause ParseSetClause();
    DeleteClause ParseDeleteClause();
    RemoveClause ParseRemoveClause();
    WithClause ParseWithClause();
    UnwindClause ParseUnwindClause();
    
    // 表达式解析（优先级 climbing）
    std::shared_ptr<Expression> ParseExpression();
    std::shared_ptr<Expression> ParseOrExpression();
    std::shared_ptr<Expression> ParseAndExpression();
    std::shared_ptr<Expression> ParseNotExpression();
    std::shared_ptr<Expression> ParseComparisonExpression();
    std::shared_ptr<Expression> ParseAddSubExpression();
    std::shared_ptr<Expression> ParseMulDivExpression();
    std::shared_ptr<Expression> ParseUnaryExpression();
    std::shared_ptr<Expression> ParsePrimaryExpression();
    std::shared_ptr<Expression> ParseLiteral();
    std::shared_ptr<Expression> ParsePropertyOrVariable();
    std::shared_ptr<Expression> ParseFunctionCall();
    std::shared_ptr<Expression> ParseListExpression();
    std::shared_ptr<Expression> ParseCaseExpression();
    std::shared_ptr<Expression> ParsePatternExpression();
    
    // 模式解析
    PathPattern ParsePathPattern();
    NodePattern ParseNodePattern();
    RelationshipPattern ParseRelationshipPattern();
    Direction ParseDirection();
    
    // 辅助
    std::unordered_map<std::string, CypherValue> ParseProperties();
    CypherValue ParseCypherValue();
    std::string ParseIdentifier();
    std::vector<std::string> ParseLabels();
    ComparisonOp ParseComparisonOp();
    bool IsAggregationFunction(const std::string& name) const;
};

// ==================== 查询执行器 ====================
class QueryExecutor {
public:
    explicit QueryExecutor(GraphStore* db);
    
    // 执行查询，返回结果集
    struct ResultRow {
        std::unordered_map<std::string, CypherValue> bindings;
    };
    struct ResultSet {
        std::vector<std::string> columns;
        std::vector<ResultRow> rows;
    };
    
    ~QueryExecutor();
    
    ResultSet Execute(const CypherQuery& query);
    
    // 显式事务控制
    void BeginTx();
    Status CommitTx();
    void RollbackTx();
    
private:
    GraphStore* db_;
    TxId read_tx_id_;       // 只读事务 ID
    TxId read_ts_;          // 解析后的读取时间戳
    TxId write_ts_ = 0;
    bool explicit_tx_ = false;
    
    TxId GetWriteTx();
    TxId GetEffTs() const;
    
    ResultSet ExecuteMatch(const CypherQuery& query);
    ResultSet ExecuteCreate(const CypherQuery& query);
    ResultSet ExecuteMerge(const CypherQuery& query);
    ResultSet ExecuteSet(const CypherQuery& query);
    ResultSet ExecuteDelete(const CypherQuery& query);
    ResultSet ExecuteRemove(const CypherQuery& query);
    
    // 模式匹配
    std::vector<ResultRow> MatchPattern(const PathPattern& pattern, 
                                          const std::vector<ResultRow>& input);
    bool MatchNode(const NodePattern& node, VertexId id, ResultRow& row);
    bool MatchRelationship(const RelationshipPattern& rel, EdgeId id, 
                           VertexId from, VertexId to, ResultRow& row);
    
    // v5: BFS 边遍历，支持变长路径 [*min..max]
    // 返回完整路径信息
    struct BfsEntry {
        EdgeId edge_id;                    // 最后一条边
        VertexId target_id;                // 目标顶点
        std::vector<EdgeId> edges;         // 完整边序列（src→...→target）
        std::vector<VertexId> vertices;    // 完整顶点序列（包含 src 和 target）
    };
    std::vector<BfsEntry> TraverseEdges(VertexId src, const RelationshipPattern& rel);
    
    // 表达式求值
    CypherValue Evaluate(const Expression& expr, const ResultRow& row);
    bool EvaluateBoolean(const Expression& expr, const ResultRow& row);
    
    // 过滤
    std::vector<ResultRow> ApplyWhere(const Expression& condition,
                                      const std::vector<ResultRow>& rows);
    
    // 投影
    ResultSet ApplyReturn(const ReturnClause& ret,
                          const std::vector<ResultRow>& rows);
    std::vector<ResultRow> ApplyWithProjection(const WithClause& with,
                                               const std::vector<ResultRow>& rows);
    
    // 排序/分页
    std::vector<ResultRow> ApplyOrderBy(const std::vector<OrderByItem>& order_by,
                                        const std::vector<ResultRow>& rows);
    std::vector<ResultRow> ApplySkipLimit(const std::optional<size_t>& skip,
                                          const std::optional<size_t>& limit,
                                          const std::vector<ResultRow>& rows);
    
    void BuildRelVars(const CypherQuery& query);
    std::unordered_set<std::string> rel_vars_;
};

} // namespace graphstore
#endif // CYPHER_PARSER_H

#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include "cypher_parser.h"
#include "graph_store.h"

using namespace graphstore;

static void print_help() {
    std::cout << "Cypher Parser CLI Tool" << std::endl;
    std::cout << "Usage: cypher_parser <path>               Open database in interactive shell" << std::endl;
    std::cout << "       cypher_parser <command> [args...]" << std::endl;
    std::cout << std::endl;
    std::cout << "Commands:" << std::endl;
    std::cout << "  <path>                      Open database in interactive shell (CRUD)" << std::endl;
    std::cout << "  tokenize <query...>         Tokenize a Cypher query" << std::endl;
    std::cout << "  parse <query...>            Parse and show AST structure" << std::endl;
    std::cout << "  exec <path> <query...>      Execute query against a database" << std::endl;
    std::cout << "  help                        Show this help" << std::endl;
}

// ---- Tokenize ----
static const char* token_type_name(Token::Type t) {
    switch (t) {
        case Token::Type::MATCH: return "MATCH";
        case Token::Type::OPTIONAL_TYPE: return "OPTIONAL";
        case Token::Type::WHERE: return "WHERE";
        case Token::Type::RETURN: return "RETURN";
        case Token::Type::WITH: return "WITH";
        case Token::Type::UNWIND: return "UNWIND";
        case Token::Type::AS: return "AS";
        case Token::Type::CREATE: return "CREATE";
        case Token::Type::MERGE: return "MERGE";
        case Token::Type::SET: return "SET";
        case Token::Type::DELETE_TYPE: return "DELETE";
        case Token::Type::DETACH: return "DETACH";
        case Token::Type::REMOVE: return "REMOVE";
        case Token::Type::ON: return "ON";
        case Token::Type::ORDER: return "ORDER";
        case Token::Type::BY: return "BY";
        case Token::Type::ASC: return "ASC";
        case Token::Type::DESC: return "DESC";
        case Token::Type::SKIP: return "SKIP";
        case Token::Type::LIMIT: return "LIMIT";
        case Token::Type::AND: return "AND";
        case Token::Type::OR: return "OR";
        case Token::Type::NOT: return "NOT";
        case Token::Type::IN_TYPE: return "IN";
        case Token::Type::IS: return "IS";
        case Token::Type::NULL_: return "NULL";
        case Token::Type::TRUE_TYPE: return "TRUE";
        case Token::Type::FALSE_TYPE: return "FALSE";
        case Token::Type::DISTINCT: return "DISTINCT";
        case Token::Type::COUNT: return "COUNT";
        case Token::Type::SUM: return "SUM";
        case Token::Type::AVG: return "AVG";
        case Token::Type::MIN: return "MIN";
        case Token::Type::MAX: return "MAX";
        case Token::Type::EXISTS: return "EXISTS";
        case Token::Type::COLLECT: return "COLLECT";
        case Token::Type::CASE: return "CASE";
        case Token::Type::WHEN: return "WHEN";
        case Token::Type::THEN: return "THEN";
        case Token::Type::ELSE: return "ELSE";
        case Token::Type::END: return "END";
        case Token::Type::LPAREN: return "(";
        case Token::Type::RPAREN: return ")";
        case Token::Type::LBRACKET: return "[";
        case Token::Type::RBRACKET: return "]";
        case Token::Type::LBRACE: return "{";
        case Token::Type::RBRACE: return "}";
        case Token::Type::COLON: return ":";
        case Token::Type::COMMA: return ",";
        case Token::Type::DOT: return ".";
        case Token::Type::SEMICOLON: return ";";
        case Token::Type::ARROW_RIGHT: return "->";
        case Token::Type::ARROW_LEFT: return "<-";
        case Token::Type::DASH: return "-";
        case Token::Type::PIPE: return "|";
        case Token::Type::STAR: return "*";
        case Token::Type::SLASH: return "/";
        case Token::Type::PERCENT: return "%";
        case Token::Type::PLUS: return "+";
        case Token::Type::MINUS: return "-";
        case Token::Type::EQ: return "=";
        case Token::Type::NE: return "!=";
        case Token::Type::LT: return "<";
        case Token::Type::LE: return "<=";
        case Token::Type::GT: return ">";
        case Token::Type::GE: return ">=";
        case Token::Type::ASSIGN: return ":=";
        case Token::Type::ELLIPSIS: return "..";
        case Token::Type::IDENTIFIER: return "IDENTIFIER";
        case Token::Type::STRING: return "STRING";
        case Token::Type::INTEGER: return "INTEGER";
        case Token::Type::FLOAT: return "FLOAT";
        case Token::Type::PARAMETER: return "PARAMETER";
        case Token::Type::EOF_: return "EOF";
        case Token::Type::INVALID: return "INVALID";
    }
    return "?";
}

static void cmd_tokenize(const std::string& input) {
    Lexer lex(input);
    int idx = 0;
    while (true) {
        Token t = lex.NextToken();
        if (t.type == Token::Type::EOF_) break;
        std::cout << "[" << idx << "] " << token_type_name(t.type)
                  << "  text='" << t.text << "'"
                  << "  Ln:" << t.line << " Col:" << t.column << std::endl;
        ++idx;
    }
}

// ---- Parse (AST dump) ----
static void print_indent(int depth) {
    for (int i = 0; i < depth; ++i) std::cout << "  ";
}

static void dump_value(const CypherValue& val) {
    std::visit([](const auto& v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::monostate>)
            std::cout << "null";
        else if constexpr (std::is_same_v<T, int64_t>)
            std::cout << v;
        else if constexpr (std::is_same_v<T, double>)
            std::cout << v;
        else if constexpr (std::is_same_v<T, bool>)
            std::cout << (v ? "true" : "false");
        else if constexpr (std::is_same_v<T, std::string>)
            std::cout << "'" << v << "'";
        else
            std::cout << "?";
    }, val);
}

static void dump_expression(const Expression& expr, int depth) {
    switch (expr.type) {
        case Expression::Type::LITERAL:
            print_indent(depth); std::cout << "LITERAL: "; dump_value(expr.literal_value); std::cout << std::endl;
            break;
        case Expression::Type::PROPERTY:
            print_indent(depth); std::cout << "PROPERTY: " << expr.variable_name << "." << expr.property_name << std::endl;
            break;
        case Expression::Type::VARIABLE:
            print_indent(depth); std::cout << "VARIABLE: " << expr.variable_name << std::endl;
            break;
        case Expression::Type::PARAMETER:
            print_indent(depth); std::cout << "PARAMETER: $" << expr.param_name << std::endl;
            break;
        case Expression::Type::COMPARISON: {
            const char* op = "?";
            switch (expr.comp_op) {
                case ComparisonOp::EQ: op = "="; break;
                case ComparisonOp::NE: op = "!="; break;
                case ComparisonOp::LT: op = "<"; break;
                case ComparisonOp::LE: op = "<="; break;
                case ComparisonOp::GT: op = ">"; break;
                case ComparisonOp::GE: op = ">="; break;
                case ComparisonOp::STARTS_WITH: op = "STARTS_WITH"; break;
                case ComparisonOp::ENDS_WITH: op = "ENDS_WITH"; break;
                case ComparisonOp::CONTAINS: op = "CONTAINS"; break;
                case ComparisonOp::IS_NULL: op = "IS_NULL"; break;
                case ComparisonOp::IS_NOT_NULL: op = "IS_NOT_NULL"; break;
                case ComparisonOp::CYPHER_IN: op = "IN"; break;
            }
            print_indent(depth); std::cout << "COMPARISON (" << op << ")" << std::endl;
            dump_expression(*expr.left, depth + 1);
            dump_expression(*expr.right, depth + 1);
            break;
        }
        case Expression::Type::LOGICAL: {
            const char* op = "?";
            switch (expr.logical_op) {
                case LogicalOp::AND: op = "AND"; break;
                case LogicalOp::OR: op = "OR"; break;
                case LogicalOp::NOT: op = "NOT"; break;
            }
            print_indent(depth); std::cout << "LOGICAL (" << op << ")" << std::endl;
            for (const auto& arg : expr.args) {
                dump_expression(*arg, depth + 1);
            }
            break;
        }
        case Expression::Type::FUNCTION_CALL:
            print_indent(depth); std::cout << "FUNCTION: " << expr.function_name << std::endl;
            for (const auto& arg : expr.args) {
                dump_expression(*arg, depth + 1);
            }
            break;
        case Expression::Type::LIST:
            print_indent(depth); std::cout << "LIST" << std::endl;
            for (const auto& arg : expr.args) {
                dump_expression(*arg, depth + 1);
            }
            break;
        case Expression::Type::AGGREGATION:
            print_indent(depth); std::cout << "AGGREGATION: " << expr.agg_func
                                          << (expr.distinct ? " (DISTINCT)" : "") << std::endl;
            for (const auto& arg : expr.args) {
                dump_expression(*arg, depth + 1);
            }
            break;
        case Expression::Type::PATTERN:
            print_indent(depth); std::cout << "PATTERN" << std::endl;
            break;
    }
}

static void dump_properties(const std::unordered_map<std::string, CypherValue>& props, int depth) {
    if (props.empty()) return;
    print_indent(depth); std::cout << "properties:" << std::endl;
    for (const auto& [k, v] : props) {
        print_indent(depth + 1); std::cout << k << ": "; dump_value(v); std::cout << std::endl;
    }
}

static void dump_node(const NodePattern& node, int depth) {
    print_indent(depth);
    std::cout << "NODE";
    if (!node.variable.empty()) std::cout << " " << node.variable;
    if (!node.labels.empty()) {
        std::cout << " :";
        for (size_t i = 0; i < node.labels.size(); ++i) {
            if (i > 0) std::cout << ":";
            std::cout << node.labels[i];
        }
    }
    std::cout << std::endl;
    dump_properties(node.properties, depth + 1);
}

static void dump_rel(const RelationshipPattern& rel, int depth) {
    print_indent(depth);
    std::cout << "REL";
    if (!rel.variable.empty()) std::cout << " " << rel.variable;
    if (!rel.types.empty()) {
        std::cout << " :";
        for (size_t i = 0; i < rel.types.size(); ++i) {
            if (i > 0) std::cout << "|";
            std::cout << rel.types[i];
        }
    }
    switch (rel.direction) {
        case Direction::OUT_DIR: std::cout << " ->"; break;
        case Direction::IN_DIR: std::cout << " <-"; break;
        case Direction::BOTH: std::cout << " --"; break;
    }
    if (rel.variable_length) {
        std::cout << " *";
        if (rel.min_hops.has_value() || rel.max_hops.has_value()) {
            std::cout << "[";
            if (rel.min_hops.has_value()) std::cout << rel.min_hops.value();
            std::cout << "..";
            if (rel.max_hops.has_value()) std::cout << rel.max_hops.value();
            std::cout << "]";
        }
    }
    std::cout << std::endl;
    dump_properties(rel.properties, depth + 1);
}

static void dump_path(const PathPattern& path, int depth) {
    print_indent(depth); std::cout << "PATH:" << std::endl;
    for (const auto& node : path.nodes) {
        dump_node(node, depth + 1);
    }
    for (const auto& rel : path.relationships) {
        dump_rel(rel, depth + 1);
    }
}

static void dump_match(const MatchClause& match, int depth) {
    print_indent(depth); std::cout << (match.optional ? "OPTIONAL_MATCH" : "MATCH") << std::endl;
    for (const auto& pattern : match.patterns) {
        dump_path(pattern, depth + 1);
    }
}

static void dump_return(const ReturnClause& ret, int depth) {
    print_indent(depth); std::cout << "RETURN";
    if (ret.distinct) std::cout << " (DISTINCT)";
    if (ret.all) std::cout << " *";
    std::cout << std::endl;
    for (const auto& item : ret.items) {
        print_indent(depth + 1);
        if (!item.alias.empty()) std::cout << item.alias << " = ";
        std::cout << std::endl;
        dump_expression(*item.expression, depth + 2);
    }
}

static void dump_query(const CypherQuery& q) {
    std::cout << "=== Cypher Query AST ===" << std::endl;
    for (const auto& match : q.match_clauses) {
        dump_match(match, 0);
    }
    if (q.where.has_value()) {
        std::cout << "  WHERE:" << std::endl;
        dump_expression(*q.where->condition, 2);
    }
    for (const auto& w : q.with_clauses) {
        std::cout << "  WITH:" << std::endl;
    }
    for (const auto& u : q.unwind_clauses) {
        std::cout << "  UNWIND: " << u->variable << std::endl;
    }
    if (q.create.has_value()) {
        std::cout << "  CREATE:" << std::endl;
        for (const auto& pattern : q.create->patterns) {
            dump_path(pattern, 2);
        }
    }
    if (q.merge.has_value()) {
        std::cout << "  MERGE:" << std::endl;
        for (const auto& pattern : q.merge->patterns) {
            dump_path(pattern, 2);
        }
    }
    if (q.set.has_value()) {
        std::cout << "  SET:" << std::endl;
        for (const auto& item : q.set->items) {
            print_indent(2);
            if (item.add_label) {
                std::cout << "ADD LABEL: " << item.label << std::endl;
            } else {
                std::cout << "SET PROPERTY:" << std::endl;
                dump_expression(*item.target, 3);
                dump_expression(*item.value, 3);
            }
        }
    }
    if (q.del.has_value()) {
        print_indent(1); std::cout << "DELETE" << (q.del->detach ? " (DETACH)" : "") << std::endl;
        for (const auto& expr : q.del->expressions) {
            dump_expression(*expr, 2);
        }
    }
    if (q.remove.has_value()) {
        std::cout << "  REMOVE:" << std::endl;
        for (const auto& item : q.remove->items) {
            print_indent(2);
            if (item.is_label) {
                std::cout << "REMOVE LABEL: " << item.label << std::endl;
            } else {
                std::cout << "REMOVE PROPERTY:" << std::endl;
                dump_expression(*item.target, 3);
            }
        }
    }
    if (!q.ret.items.empty() || q.ret.all) {
        dump_return(q.ret, 1);
    }
    if (!q.order_by.empty()) {
        std::cout << "  ORDER BY:" << std::endl;
        for (const auto& ob : q.order_by) {
            print_indent(2); std::cout << (ob.ascending ? "ASC" : "DESC") << std::endl;
            dump_expression(*ob.expression, 3);
        }
    }
    if (q.skip.has_value()) {
        std::cout << "  SKIP: " << q.skip.value() << std::endl;
    }
    if (q.limit.has_value()) {
        std::cout << "  LIMIT: " << q.limit.value() << std::endl;
    }
}

static void cmd_parse(const std::string& input) {
    Parser parser(input);
    auto query = parser.ParseQuery();
    if (!query) {
        std::cerr << "Parse error: " << parser.GetError() << std::endl;
        return;
    }
    dump_query(*query);
}

// ---- Exec ----
static void print_result(const QueryExecutor::ResultSet& result) {
    if (result.columns.empty() && result.rows.empty()) {
        std::cout << "(empty result)" << std::endl;
        return;
    }
    for (size_t i = 0; i < result.columns.size(); ++i) {
        if (i > 0) std::cout << "\t";
        std::cout << result.columns[i];
    }
    std::cout << std::endl;
    for (const auto& row : result.rows) {
        for (size_t i = 0; i < result.columns.size(); ++i) {
            if (i > 0) std::cout << "\t";
            auto it = row.bindings.find(result.columns[i]);
            if (it != row.bindings.end()) {
                std::visit([](const auto& v) {
                    using T = std::decay_t<decltype(v)>;
                    if constexpr (std::is_same_v<T, std::monostate>)
                        std::cout << "null";
                    else if constexpr (std::is_same_v<T, int64_t>)
                        std::cout << v;
                    else if constexpr (std::is_same_v<T, double>)
                        std::cout << v;
                    else if constexpr (std::is_same_v<T, bool>)
                        std::cout << (v ? "true" : "false");
                    else if constexpr (std::is_same_v<T, std::string>)
                        std::cout << v;
                    else
                        std::cout << "?";
                }, it->second);
            } else {
                std::cout << "null";
            }
        }
        std::cout << std::endl;
    }
    std::cout << "(" << result.rows.size() << " rows)" << std::endl;
}

static void cmd_exec(const std::string& path, const std::string& input) {
    GraphStore db;
    Status s = db.Init(path);
    if (!IsOk(s)) {
        std::cerr << "Error: Failed to open database: " << path << std::endl;
        return;
    }
    Parser parser(input);
    auto query = parser.ParseQuery();
    if (!query) {
        std::cerr << "Parse error: " << parser.GetError() << std::endl;
        db.Close();
        return;
    }
    QueryExecutor exec(&db);
    try {
        auto result = exec.Execute(*query);
        print_result(result);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }
    db.Close();
}

// ---- REPL (interactive shell) ----
static void shell_open(GraphStore& db, bool& has_db, const std::string& path) {
    if (has_db) {
        db.Close();
        has_db = false;
    }
    Status s = db.Init(path);
    if (!IsOk(s)) {
        std::cerr << "Error: Failed to open database: " << path << std::endl;
        return;
    }
    has_db = true;
    TxId rtx = db.BeginTransaction(true);
    std::cout << "Connected to: " << path << std::endl;
    std::cout << "  Vertices: " << db.GetVertexCount(rtx)
              << ", Edges: " << db.GetEdgeCount(rtx) << std::endl;
    db.Commit(rtx);
}

static void shell_close(GraphStore& db, bool& has_db) {
    if (!has_db) {
        std::cout << "  No database is open." << std::endl;
        return;
    }
    db.Close();
    has_db = false;
    std::cout << "Database closed." << std::endl;
}

static void cmd_repl(const std::string& init_path) {
    GraphStore db;
    bool has_db = false;

    if (!init_path.empty()) {
        shell_open(db, has_db, init_path);
    }

    std::cout << "Cypher Parser Interactive Shell" << std::endl;
    std::cout << "Commands: .open <path>  .close  .exit  .help  .tables  .tokenize  .parse" << std::endl;
    std::string line;

    auto do_tokenize = [&](const std::string& input) {
        Lexer lex(input);
        int idx = 0;
        while (true) {
            Token t = lex.NextToken();
            if (t.type == Token::Type::EOF_) break;
            std::cout << "  [" << idx << "] " << token_type_name(t.type)
                      << " '" << t.text << "'" << std::endl;
            ++idx;
        }
    };
    auto do_parse = [&](const std::string& input) {
        Parser parser(input);
        auto query = parser.ParseQuery();
        if (!query) {
            std::cout << "  Parse error: " << parser.GetError() << std::endl;
            return;
        }
        dump_query(*query);
    };
    auto do_exec = [&](const std::string& input) {
        Parser parser(input);
        auto query = parser.ParseQuery();
        if (!query) {
            std::cout << "  Parse error: " << parser.GetError() << std::endl;
            return;
        }
        QueryExecutor exec(&db);
        try {
            auto result = exec.Execute(*query);
            print_result(result);
        } catch (const std::exception& e) {
            std::cerr << "  Error: " << e.what() << std::endl;
        }
    };
    auto do_tables = [&]() {
        TxId rtx = db.BeginTransaction(true);
        std::cout << "  Vertices: " << db.GetVertexCount(rtx)
                  << ", Edges: " << db.GetEdgeCount(rtx) << std::endl;
        auto all = db.GetAllVertices(rtx);
        std::unordered_set<LabelId> seen;
        for (auto vid : all) {
            auto v = db.GetVertex(rtx, vid);
            if (v) seen.insert(v->label);
        }
        if (!seen.empty()) {
            std::cout << "  Labels:";
            for (auto l : seen) std::cout << " " << l;
            std::cout << std::endl;
        }
        db.Commit(rtx);
    };

    while (true) {
        std::cout << "cypher> ";
        if (!std::getline(std::cin, line)) break;
        if (line.empty()) continue;

        if (line == ".exit" || line == ".quit" || line == "exit" || line == "quit") break;

        if (line == ".help" || line == "help") {
            std::cout << "  Cypher queries: MATCH, CREATE, MERGE, SET, DELETE, REMOVE, etc." << std::endl;
            std::cout << "  Special commands:" << std::endl;
            std::cout << "    .open <path>       Open a database file" << std::endl;
            std::cout << "    .close             Close the current database" << std::endl;
            std::cout << "    .cls               Clear the screen" << std::endl;
            std::cout << "    .exit              Exit the shell" << std::endl;
            std::cout << "    .help              Show this help" << std::endl;
            std::cout << "    .tables            Show database statistics" << std::endl;
            std::cout << "    .tokenize <query>  Show tokens without executing" << std::endl;
            std::cout << "    .parse <query>     Show AST without executing" << std::endl;
            continue;
        }

        if (line.substr(0, 5) == ".open") {
            std::string rest = line.size() > 5 ? line.substr(6) : "";
            if (rest.empty()) {
                std::cout << "  Usage: .open <path>" << std::endl;
            } else {
                shell_open(db, has_db, rest);
            }
            continue;
        }

        if (line == ".close") {
            shell_close(db, has_db);
            continue;
        }

        if (line == ".cls") {
#ifdef _WIN32
            std::system("cls");
#else
            std::system("clear");
#endif
            continue;
        }

        if (line == ".tables") {
            if (!has_db) { std::cout << "  No database open. Use .open <path>" << std::endl; continue; }
            do_tables();
            continue;
        }

        if (line.substr(0, 9) == ".tokenize" || line.substr(0, 9) == ".tokenise") {
            std::string rest = line.size() > 9 ? line.substr(10) : "";
            if (!rest.empty()) do_tokenize(rest);
            continue;
        }
        if (line.substr(0, 6) == ".parse") {
            std::string rest = line.size() > 6 ? line.substr(7) : "";
            if (!rest.empty()) do_parse(rest);
            continue;
        }

        // default: treat as Cypher query
        if (!has_db) {
            std::cout << "  No database open. Use .open <path> first" << std::endl;
            continue;
        }
        do_exec(line);
    }
    std::cout << "Bye." << std::endl;
    db.Close();
}

int main(int argc, char* argv[]) {
    auto is_cmd = [](const std::string& s) {
        return s == "tokenize" || s == "parse" || s == "exec" || s == "repl"
            || s == "help" || s == "--help" || s == "-h";
    };

    if (argc < 2) {
        cmd_repl("");
        return 0;
    }

    std::string first = argv[1];

    // plain path → open interactive shell
    if (argc == 2 && !is_cmd(first)) {
        cmd_repl(first);
        return 0;
    }

    if (first == "help" || first == "--help" || first == "-h") {
        print_help();
        return 0;
    }

    if (first == "tokenize") {
        if (argc < 3) { std::cerr << "Usage: cypher_parser tokenize <query...>" << std::endl; return 1; }
        std::ostringstream oss;
        for (int i = 2; i < argc; ++i) {
            if (i > 2) oss << " ";
            oss << argv[i];
        }
        cmd_tokenize(oss.str());
    } else if (first == "parse") {
        if (argc < 3) { std::cerr << "Usage: cypher_parser parse <query...>" << std::endl; return 1; }
        std::ostringstream oss;
        for (int i = 2; i < argc; ++i) {
            if (i > 2) oss << " ";
            oss << argv[i];
        }
        cmd_parse(oss.str());
    } else if (first == "exec") {
        if (argc < 4) { std::cerr << "Usage: cypher_parser exec <path> <query...>" << std::endl; return 1; }
        std::ostringstream oss;
        for (int i = 3; i < argc; ++i) {
            if (i > 3) oss << " ";
            oss << argv[i];
        }
        cmd_exec(argv[2], oss.str());
    } else if (first == "repl") {
        if (argc < 3) { std::cerr << "Usage: cypher_parser repl <path>" << std::endl; return 1; }
        cmd_repl(argv[2]);
    } else {
        std::cerr << "Unknown command: " << first << std::endl;
        print_help();
        return 1;
    }
    return 0;
}

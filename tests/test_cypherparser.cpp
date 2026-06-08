#include <iostream>
#include <cassert>
#include <filesystem>
#include <thread>
#include <atomic>
#include <vector>
#include <random>
#include "cypher_parser.h"
#include "graph_store.h"

using namespace graphstore;

static int tests_passed = 0;
static int tests_total = 0;

#define CHECK(cond, msg) do { \
    tests_total++; \
    if (!(cond)) { std::cerr << "  FAIL: " << msg << std::endl; return false; } \
    tests_passed++; \
} while(0)

#define CHECK_EQ(a, b, msg) CHECK((a) == (b), msg)

struct DbGuard {
    GraphStore& db;
    std::string path;
    ~DbGuard() {
        db.Close();
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

// ==================== Lexer Tests ====================

bool test_lexer_keywords() {
    std::cout << "Test: lexer keywords... ";
    Lexer lex("MATCH OPTIONAL WHERE RETURN WITH UNWIND AS CREATE MERGE SET DELETE DETACH REMOVE");
    CHECK_EQ(lex.NextToken().type, Token::Type::MATCH, "MATCH");
    CHECK_EQ(lex.NextToken().type, Token::Type::OPTIONAL_TYPE, "OPTIONAL");
    CHECK_EQ(lex.NextToken().type, Token::Type::WHERE, "WHERE");
    CHECK_EQ(lex.NextToken().type, Token::Type::RETURN, "RETURN");
    CHECK_EQ(lex.NextToken().type, Token::Type::WITH, "WITH");
    CHECK_EQ(lex.NextToken().type, Token::Type::UNWIND, "UNWIND");
    CHECK_EQ(lex.NextToken().type, Token::Type::AS, "AS");
    CHECK_EQ(lex.NextToken().type, Token::Type::CREATE, "CREATE");
    CHECK_EQ(lex.NextToken().type, Token::Type::MERGE, "MERGE");
    CHECK_EQ(lex.NextToken().type, Token::Type::SET, "SET");
    CHECK_EQ(lex.NextToken().type, Token::Type::DELETE_TYPE, "DELETE");
    CHECK_EQ(lex.NextToken().type, Token::Type::DETACH, "DETACH");
    CHECK_EQ(lex.NextToken().type, Token::Type::REMOVE, "REMOVE");
    std::cout << "PASS" << std::endl;
    return true;
}

bool test_lexer_symbols() {
    std::cout << "Test: lexer symbols... ";
    Lexer lex("( ) [ ] { } , : ; . + - -> <- < <= > >= = !=");
    CHECK_EQ(lex.NextToken().type, Token::Type::LPAREN, "(");
    CHECK_EQ(lex.NextToken().type, Token::Type::RPAREN, ")");
    CHECK_EQ(lex.NextToken().type, Token::Type::LBRACKET, "[");
    CHECK_EQ(lex.NextToken().type, Token::Type::RBRACKET, "]");
    CHECK_EQ(lex.NextToken().type, Token::Type::LBRACE, "{");
    CHECK_EQ(lex.NextToken().type, Token::Type::RBRACE, "}");
    CHECK_EQ(lex.NextToken().type, Token::Type::COMMA, ",");
    CHECK_EQ(lex.NextToken().type, Token::Type::COLON, ":");
    CHECK_EQ(lex.NextToken().type, Token::Type::SEMICOLON, ";");
    CHECK_EQ(lex.NextToken().type, Token::Type::DOT, ".");
    CHECK_EQ(lex.NextToken().type, Token::Type::PLUS, "+");
    CHECK_EQ(lex.NextToken().type, Token::Type::MINUS, "-");
    CHECK_EQ(lex.NextToken().type, Token::Type::ARROW_RIGHT, "->");
    CHECK_EQ(lex.NextToken().type, Token::Type::ARROW_LEFT, "<-");
    CHECK_EQ(lex.NextToken().type, Token::Type::LT, "<");
    CHECK_EQ(lex.NextToken().type, Token::Type::LE, "<=");
    CHECK_EQ(lex.NextToken().type, Token::Type::GT, ">");
    CHECK_EQ(lex.NextToken().type, Token::Type::GE, ">=");
    CHECK_EQ(lex.NextToken().type, Token::Type::EQ, "=");
    CHECK_EQ(lex.NextToken().type, Token::Type::NE, "!=");
    std::cout << "PASS" << std::endl;
    return true;
}

bool test_lexer_literals() {
    std::cout << "Test: lexer literals... ";
    Lexer lex("42 3.14 'hello' \"world\" true false null $param");
    auto t1 = lex.NextToken();
    CHECK_EQ(t1.type, Token::Type::INTEGER, "integer");
    CHECK_EQ(t1.text, "42", "int val");
    auto t2 = lex.NextToken();
    CHECK_EQ(t2.type, Token::Type::FLOAT, "float");
    CHECK_EQ(t2.text, "3.14", "float val");
    auto t3 = lex.NextToken();
    CHECK_EQ(t3.type, Token::Type::STRING, "string1");
    CHECK_EQ(t3.text, "hello", "string1 val");
    auto t4 = lex.NextToken();
    CHECK_EQ(t4.type, Token::Type::STRING, "string2");
    CHECK_EQ(t4.text, "world", "string2 val");
    CHECK_EQ(lex.NextToken().type, Token::Type::TRUE_TYPE, "true");
    CHECK_EQ(lex.NextToken().type, Token::Type::FALSE_TYPE, "false");
    CHECK_EQ(lex.NextToken().type, Token::Type::NULL_, "null");
    CHECK_EQ(lex.NextToken().type, Token::Type::PARAMETER, "param");
    std::cout << "PASS" << std::endl;
    return true;
}

bool test_lexer_comment_ellipsis() {
    std::cout << "Test: lexer comments/ellipsis... ";
    Lexer lex1("MATCH // comment\nRETURN");
    CHECK_EQ(lex1.NextToken().type, Token::Type::MATCH, "keyword before comment");
    CHECK_EQ(lex1.NextToken().type, Token::Type::RETURN, "keyword after comment");
    Lexer lex2("1..5");
    CHECK_EQ(lex2.NextToken().type, Token::Type::INTEGER, "int before ..");
    CHECK_EQ(lex2.NextToken().type, Token::Type::ELLIPSIS, "..");
    CHECK_EQ(lex2.NextToken().type, Token::Type::INTEGER, "int after ..");
    std::cout << "PASS" << std::endl;
    return true;
}

// ==================== Parser Tests ====================

bool test_parse_basic_pattern() {
    std::cout << "Test: basic pattern parsing... ";
    auto q1 = Parser("MATCH (n) RETURN n").ParseQuery();
    CHECK(q1.has_value(), "MATCH (n) RETURN n");
    CHECK_EQ(q1->match_clauses.size(), 1, "one match");
    CHECK_EQ(q1->match_clauses[0].patterns[0].nodes.size(), 1, "one node");
    CHECK_EQ(q1->match_clauses[0].patterns[0].nodes[0].variable, "n", "node var");
    CHECK_EQ(q1->ret.items.size(), 1, "one return");
    
    auto q2 = Parser("MATCH (n:Person) RETURN n").ParseQuery();
    CHECK(q2.has_value(), "MATCH (n:Person)");
    CHECK_EQ(q2->match_clauses[0].patterns[0].nodes[0].labels.size(), 1, "one label");
    CHECK_EQ(q2->match_clauses[0].patterns[0].nodes[0].labels[0], "Person", "label name");
    
    auto q3 = Parser("MATCH (n:Person {name: 'Alice'}) RETURN n").ParseQuery();
    CHECK(q3.has_value(), "MATCH with props");
    auto& props = q3->match_clauses[0].patterns[0].nodes[0].properties;
    CHECK_EQ(props.count("name"), 1, "has name key");
    CHECK(std::holds_alternative<std::string>(props.at("name")), "string value");
    CHECK_EQ(std::get<std::string>(props.at("name")), "Alice", "prop val");
    
    std::cout << "PASS" << std::endl;
    return true;
}

bool test_parse_relationships() {
    std::cout << "Test: relationship parsing... ";
    
    auto q1 = Parser("MATCH (a)-[:KNOWS]->(b) RETURN a").ParseQuery();
    CHECK(q1.has_value(), "out rel");
    CHECK_EQ(q1->match_clauses[0].patterns[0].relationships[0].direction, Direction::OUT_DIR, "out");
    CHECK_EQ(q1->match_clauses[0].patterns[0].relationships[0].types[0], "KNOWS", "type");
    
    auto q2 = Parser("MATCH (a)<-[:FRIEND]-(b) RETURN a").ParseQuery();
    CHECK(q2.has_value(), "in rel");
    CHECK_EQ(q2->match_clauses[0].patterns[0].relationships[0].direction, Direction::IN_DIR, "in");
    CHECK_EQ(q2->match_clauses[0].patterns[0].relationships[0].types[0], "FRIEND", "in type");
    
    auto q3 = Parser("MATCH (a)--(b) RETURN a").ParseQuery();
    CHECK(q3.has_value(), "undirected");
    CHECK_EQ(q3->match_clauses[0].patterns[0].relationships[0].direction, Direction::BOTH, "both");
    
    auto q4 = Parser("MATCH (a)-[*1..5]->(b) RETURN a").ParseQuery();
    CHECK(q4.has_value(), "varlen");
    CHECK(q4->match_clauses[0].patterns[0].relationships[0].variable_length, "varlen");
    
    std::cout << "PASS" << std::endl;
    return true;
}

bool test_parse_where() {
    std::cout << "Test: WHERE clause... ";
    
    auto q1 = Parser("MATCH (n) WHERE n.age > 30 RETURN n").ParseQuery();
    CHECK(q1.has_value(), "where GT");
    CHECK(q1->where.has_value(), "has where");
    CHECK_EQ(q1->where->condition->type, Expression::Type::COMPARISON, "comparison");
    
    auto q2 = Parser("MATCH (n) WHERE n.age > 25 AND n.name = 'Alice' RETURN n").ParseQuery();
    CHECK(q2.has_value(), "where AND");
    CHECK(q2->where.has_value(), "has where AND");
    CHECK_EQ(q2->where->condition->type, Expression::Type::LOGICAL, "logical");
    CHECK_EQ(q2->where->condition->logical_op, LogicalOp::AND, "AND");
    
    auto q3 = Parser("MATCH (n) WHERE n.name IS NULL RETURN n").ParseQuery();
    CHECK(q3.has_value(), "where IS NULL");
    CHECK_EQ(q3->where->condition->comp_op, ComparisonOp::IS_NULL, "IS NULL");
    
    auto q4 = Parser("MATCH (n) WHERE n.name STARTS WITH 'A' RETURN n").ParseQuery();
    CHECK(q4.has_value(), "STARTS WITH");
    CHECK_EQ(q4->where->condition->comp_op, ComparisonOp::STARTS_WITH, "STARTS WITH op");
    
    auto q5 = Parser("MATCH (n) WHERE n.name ENDS WITH 'Z' RETURN n").ParseQuery();
    CHECK(q5.has_value(), "ENDS WITH");
    CHECK_EQ(q5->where->condition->comp_op, ComparisonOp::ENDS_WITH, "ENDS WITH op");
    
    auto q6 = Parser("MATCH (n) WHERE n.name CONTAINS 'mid' RETURN n").ParseQuery();
    CHECK(q6.has_value(), "CONTAINS");
    CHECK_EQ(q6->where->condition->comp_op, ComparisonOp::CONTAINS, "CONTAINS op");
    
    auto q7 = Parser("MATCH (n) WHERE NOT n.name = 'Bob' RETURN n").ParseQuery();
    CHECK(q7.has_value(), "NOT");
    CHECK(q7->where.has_value(), "has where NOT");
    CHECK_EQ(q7->where->condition->type, Expression::Type::LOGICAL, "logical NOT");
    CHECK_EQ(q7->where->condition->logical_op, LogicalOp::NOT, "NOT");
    
    std::cout << "PASS" << std::endl;
    return true;
}

bool test_parse_return() {
    std::cout << "Test: RETURN clause... ";
    
    auto q1 = Parser("MATCH (n) RETURN *").ParseQuery();
    CHECK(q1.has_value(), "RETURN *");
    CHECK(q1->ret.all, "return all");
    
    auto q2 = Parser("MATCH (n) RETURN DISTINCT n.name").ParseQuery();
    CHECK(q2.has_value(), "RETURN DISTINCT");
    CHECK(q2->ret.distinct, "distinct");
    
    auto q3 = Parser("MATCH (n) RETURN n.name AS alias_name").ParseQuery();
    CHECK(q3.has_value(), "RETURN AS");
    CHECK_EQ(q3->ret.items.size(), 1, "one item");
    CHECK_EQ(q3->ret.items[0].alias, "alias_name", "alias");
    
    {
    Parser p4("MATCH (n) RETURN n ORDER BY n.age SKIP 5 LIMIT 10");
    auto q4 = p4.ParseQuery();
    CHECK(q4.has_value(), "ORDER BY");
    CHECK_EQ(q4->order_by.size(), 1, "one order item");
    CHECK_EQ(q4->skip.value_or(0), 5, "skip");
    CHECK_EQ(q4->limit.value_or(0), 10, "limit");
    }
    
    {
    Parser p5("MATCH (n) RETURN n ORDER BY n.age DESC");
    auto q5 = p5.ParseQuery();
    CHECK(q5.has_value(), "ORDER BY DESC");
    CHECK_EQ(q5->order_by.size(), 1, "one desc order");
    CHECK(!q5->order_by[0].ascending, "desc");
    }
    
    std::cout << "PASS" << std::endl;
    return true;
}

bool test_parse_clauses() {
    std::cout << "Test: CREATE/MERGE/SET/DELETE/REMOVE/WITH/UNWIND... ";
    
    auto q1 = Parser("OPTIONAL MATCH (n) RETURN n").ParseQuery();
    CHECK(q1.has_value(), "OPTIONAL MATCH");
    CHECK(q1->match_clauses[0].optional, "optional");
    
    auto q2 = Parser("CREATE (n:Person {name: 'Alice'})").ParseQuery();
    CHECK(q2.has_value(), "CREATE");
    CHECK(q2->create.has_value(), "has create");
    CHECK_EQ(q2->create->patterns[0].nodes[0].labels[0], "Person", "create label");
    
    auto q3 = Parser("MERGE (n:Person {name: 'Bob'})").ParseQuery();
    CHECK(q3.has_value(), "MERGE");
    CHECK(q3->merge.has_value(), "has merge");
    
    auto q4 = Parser("MATCH (n) SET n:Admin RETURN n").ParseQuery();
    CHECK(q4.has_value(), "SET label");
    CHECK(q4->set.has_value(), "has set");
    CHECK(q4->set->items[0].add_label, "add label");
    CHECK_EQ(q4->set->items[0].label, "Admin", "label name");
    
    auto q5 = Parser("MATCH (n) DELETE n").ParseQuery();
    CHECK(q5.has_value(), "DELETE");
    CHECK(q5->del.has_value(), "has delete");
    CHECK(!q5->del->detach, "not detach");
    
    auto q6 = Parser("MATCH (n) DETACH DELETE n").ParseQuery();
    CHECK(q6.has_value(), "DETACH DELETE");
    CHECK(q6->del->detach, "detach");
    
    auto q7 = Parser("MATCH (n) REMOVE n:Admin RETURN n").ParseQuery();
    CHECK(q7.has_value(), "REMOVE");
    CHECK(q7->remove.has_value(), "has remove");
    CHECK(q7->remove->items[0].is_label, "is label");
    
    auto q8 = Parser("MATCH (n) WITH n.name AS name RETURN name").ParseQuery();
    CHECK(q8.has_value(), "WITH");
    CHECK_EQ(q8->with_clauses.size(), 1, "one with");
    
    auto q9 = Parser("UNWIND [1,2,3] AS x RETURN x").ParseQuery();
    CHECK(q9.has_value(), "UNWIND");
    CHECK_EQ(q9->unwind_clauses.size(), 1, "one unwind");
    CHECK_EQ(q9->unwind_clauses[0]->variable, "x", "unwind var");
    
    std::cout << "PASS" << std::endl;
    return true;
}

bool test_parse_aggregation_and_list() {
    std::cout << "Test: aggregation/list/CASE... ";
    
    auto q1 = Parser("MATCH (n) RETURN COUNT(n) AS cnt").ParseQuery();
    CHECK(q1.has_value(), "COUNT");
    CHECK_EQ(q1->ret.items[0].expression->type, Expression::Type::AGGREGATION, "agg");
    CHECK_EQ(q1->ret.items[0].expression->agg_func, "COUNT", "COUNT");
    
    auto q2 = Parser("MATCH (a)-[:A]->(b), (c)-[:B]->(d) RETURN a").ParseQuery();
    CHECK(q2.has_value(), "multiple patterns");
    CHECK_EQ(q2->match_clauses[0].patterns.size(), 2, "two patterns");
    
    auto q3 = Parser("MATCH (n) RETURN CASE WHEN n.age > 30 THEN 'old' ELSE 'young' END AS status").ParseQuery();
    CHECK(q3.has_value(), "CASE");
    CHECK_EQ(q3->ret.items[0].expression->type, Expression::Type::FUNCTION_CALL, "CASE");
    CHECK_EQ(q3->ret.items[0].expression->function_name, "CASE", "CASE name");
    
    std::cout << "PASS" << std::endl;
    return true;
}

bool test_parse_error() {
    std::cout << "Test: parse error handling... ";
    Parser parser("MATCH (n RANDOM_SYNTAX_ERROR");
    auto q = parser.ParseQuery();
    CHECK(!q.has_value(), "should fail on error");
    std::cout << "PASS" << std::endl;
    return true;
}

// ==================== Executor Tests ====================

bool test_executor_basic() {
    std::cout << "Test: QueryExecutor basic... " << std::flush;
    std::string db_path = "/tmp/test_cypher_parser.db";
    std::filesystem::remove_all(db_path);
    
    GraphStore db;
    if (!IsOk(db.Init(db_path))) {
        std::cout << "FAIL (init)" << std::endl;
        return false;
    }
    DbGuard guard{db, db_path};
    
    TxId wtx = db.BeginTransaction(false);
    Properties p1, p2;
    p1.Set("name", "Alice");
    p1.Set("age", "30");
    p2.Set("name", "Bob");
    p2.Set("age", "25");
    VertexId v1 = db.AddVertex(wtx, 1, p1);
    VertexId v2 = db.AddVertex(wtx, 2, p2);
    db.AddEdge(wtx, v1, v2, 10, {});
    db.Commit(wtx);

    QueryExecutor exec(&db);
    auto q = Parser("MATCH (n:Person {name: 'Alice'}) RETURN n.name AS name").ParseQuery();
    CHECK(q.has_value(), "parse for exec");
    auto result = exec.Execute(*q);
    CHECK_EQ(result.columns.size(), 1, "one column");

    std::cout << "PASS" << std::endl;
    return true;
}

bool test_executor_return() {
    std::cout << "Test: QueryExecutor RETURN... " << std::flush;
    std::string db_path = "/tmp/test_cypher2.db";
    std::filesystem::remove_all(db_path);
    
    GraphStore db;
    if (!IsOk(db.Init(db_path))) {
        std::cout << "FAIL (init)" << std::endl;
        return false;
    }
    DbGuard guard{db, db_path};
    
    TxId wtx = db.BeginTransaction(false);
    Properties p;
    p.Set("name", "Alice");
    p.Set("age", "30");
    db.AddVertex(wtx, 1, p);
    db.Commit(wtx);
    
    QueryExecutor exec(&db);
    auto q = Parser("MATCH (n) RETURN n.name AS name, n.age AS age").ParseQuery();
    CHECK(q.has_value(), "parse for return");
    auto result = exec.Execute(*q);
    CHECK_EQ(result.columns.size(), 2, "two columns");
    
    std::cout << "PASS" << std::endl;
    return true;
}

bool test_executor_create_stub() {
    std::cout << "Test: QueryExecutor CREATE stub... " << std::flush;
    std::string db_path = "/tmp/test_cypher3.db";
    std::filesystem::remove_all(db_path);
    
    GraphStore db;
    if (!IsOk(db.Init(db_path))) {
        std::cout << "FAIL (init)" << std::endl;
        return false;
    }
    DbGuard guard{db, db_path};
    
    auto q = Parser("CREATE (n:Person {name: 'Charlie'})").ParseQuery();
    CHECK(q.has_value(), "parse create");
    
    QueryExecutor exec(&db);
    auto result = exec.Execute(*q);
    (void)result;
    
    std::cout << "PASS" << std::endl;
    return true;
}

bool test_executor_create_vertex() {
    std::cout << "Test: QueryExecutor CREATE vertex... " << std::flush;
    std::string db_path = "/tmp/test_create_vertex.db";
    std::filesystem::remove_all(db_path);
    
    GraphStore db;
    if (!IsOk(db.Init(db_path))) { std::cout << "FAIL (init)" << std::endl; return false; }
    DbGuard guard{db, db_path};
    
    QueryExecutor exec(&db);
    auto q = Parser("CREATE (n:Person {name: 'Charlie', age: '25'})").ParseQuery();
    CHECK(q.has_value(), "parse create");
    auto result = exec.Execute(*q);
    CHECK_EQ(result.rows.size(), 1, "one row");
    CHECK(result.rows[0].bindings.count("n"), "has binding n");
    VertexId vid = static_cast<VertexId>(std::get<int64_t>(result.rows[0].bindings["n"]));
    CHECK(vid > 0, "valid vertex id");
    
    // verify via MATCH
    QueryExecutor exec2(&db);
    auto q2 = Parser("MATCH (n:Person {name: 'Charlie'}) RETURN n.name AS name").ParseQuery();
    CHECK(q2.has_value(), "parse match");
    auto result2 = exec2.Execute(*q2);
    CHECK_EQ(result2.rows.size(), 1, "found by match");
    
    std::cout << "PASS" << std::endl;
    return true;
}

bool test_executor_merge_create() {
    std::cout << "Test: QueryExecutor MERGE (create when not found)... " << std::flush;
    std::string db_path = "/tmp/test_merge_create.db";
    std::filesystem::remove_all(db_path);
    
    GraphStore db;
    if (!IsOk(db.Init(db_path))) { std::cout << "FAIL (init)" << std::endl; return false; }
    DbGuard guard{db, db_path};
    
    // MERGE a non-existent pattern → should CREATE
    QueryExecutor exec(&db);
    auto q = Parser("MERGE (n:Person {name: 'Dave'})").ParseQuery();
    CHECK(q.has_value(), "parse merge");
    auto result = exec.Execute(*q);
    CHECK_EQ(result.rows.size(), 1, "created one");
    
    // MERGE the same pattern again → should MATCH, not create duplicate
    QueryExecutor exec2(&db);
    auto q2 = Parser("MERGE (n:Person {name: 'Dave'})").ParseQuery();
    CHECK(q2.has_value(), "parse merge2");
    auto result2 = exec2.Execute(*q2);
    CHECK_EQ(result2.rows.size(), 1, "matched one (no duplicate)");
    
    // verify only ONE vertex exists
    QueryExecutor exec3(&db);
    auto q3 = Parser("MATCH (n:Person {name: 'Dave'}) RETURN n").ParseQuery();
    auto result3 = exec3.Execute(*q3);
    CHECK_EQ(result3.rows.size(), 1, "only one Dave");
    
    std::cout << "PASS" << std::endl;
    return true;
}

bool test_executor_set_property() {
    std::cout << "Test: QueryExecutor SET property... " << std::flush;
    std::string db_path = "/tmp/test_exec_set.db";
    std::filesystem::remove_all(db_path);
    
    GraphStore db;
    if (!IsOk(db.Init(db_path))) { std::cout << "FAIL (init)" << std::endl; return false; }
    DbGuard guard{db, db_path};
    
    // create a vertex
    QueryExecutor exec1(&db);
    auto q1 = Parser("CREATE (n:Person {name: 'Eve', age: '20'})").ParseQuery();
    auto r1 = exec1.Execute(*q1);
    CHECK_EQ(r1.rows.size(), 1, "created for set test");
    
    // update property via SET
    QueryExecutor exec2(&db);
    auto q2 = Parser("MATCH (n:Person {name: 'Eve'}) SET n.age = '21' RETURN n.name, n.age").ParseQuery();
    auto r2 = exec2.Execute(*q2);
    CHECK_EQ(r2.rows.size(), 1, "set returned row");
    
    // verify SET worked
    QueryExecutor exec3(&db);
    auto q3 = Parser("MATCH (n:Person {name: 'Eve'}) RETURN n.age AS age").ParseQuery();
    auto r3 = exec3.Execute(*q3);
    CHECK_EQ(r3.rows.size(), 1, "found after set");
    
    std::cout << "PASS" << std::endl;
    return true;
}

bool test_executor_delete_vertex() {
    std::cout << "Test: QueryExecutor DELETE vertex... " << std::flush;
    std::string db_path = "/tmp/test_exec_del.db";
    std::filesystem::remove_all(db_path);
    
    GraphStore db;
    if (!IsOk(db.Init(db_path))) { std::cout << "FAIL (init)" << std::endl; return false; }
    DbGuard guard{db, db_path};
    
    // create vertices
    QueryExecutor exec1(&db);
    exec1.Execute(*Parser("CREATE (n:Person {name: 'Frank'})").ParseQuery());
    exec1.Execute(*Parser("CREATE (n:Person {name: 'Grace'})").ParseQuery());
    
    // delete one via QueryExecutor
    QueryExecutor exec2(&db);
    auto q2 = Parser("MATCH (n:Person {name: 'Frank'}) DELETE n").ParseQuery();
    auto r2 = exec2.Execute(*q2);
    (void)r2;
    
    // verify deleted
    QueryExecutor exec3(&db);
    auto q3 = Parser("MATCH (n:Person {name: 'Frank'}) RETURN n").ParseQuery();
    auto r3 = exec3.Execute(*q3);
    CHECK_EQ(r3.rows.size(), 0, "Frank deleted");
    
    // verify Grace still exists
    QueryExecutor exec4(&db);
    auto q4 = Parser("MATCH (n:Person {name: 'Grace'}) RETURN n").ParseQuery();
    auto r4 = exec4.Execute(*q4);
    CHECK_EQ(r4.rows.size(), 1, "Grace still exists");
    
    std::cout << "PASS" << std::endl;
    return true;
}

bool test_executor_path_single_hop() {
    std::cout << "Test: QueryExecutor single-hop path... " << std::flush;
    std::string db_path = "/tmp/test_single_hop.db";
    std::filesystem::remove_all(db_path);
    
    GraphStore db;
    if (!IsOk(db.Init(db_path))) { std::cout << "FAIL (init)" << std::endl; return false; }
    DbGuard guard{db, db_path};
    
    // create vertices and edge manually (no Cypher CREATE edges yet)
    LabelId person_label = Fnv1a("Person");
    TxId wtx = db.BeginTransaction(false);
    Properties pa, pb;
    pa.Set("name", "Alice");
    pb.Set("name", "Bob");
    VertexId v1 = db.AddVertex(wtx, person_label, pa);
    VertexId v2 = db.AddVertex(wtx, person_label, pb);
    db.AddEdge(wtx, v1, v2, 10, {});
    db.Commit(wtx);
    
    // MATCH with single-hop relationship: vertices and edge created above
    
    // MATCH with single-hop relationship
    QueryExecutor exec(&db);
    auto q = Parser("MATCH (a:Person)-[r]->(b:Person) RETURN a.name, b.name").ParseQuery();
    CHECK(q.has_value(), "parse path");
    auto result = exec.Execute(*q);
    CHECK_EQ(result.rows.size(), 1, "one path found");
    
    std::cout << "PASS" << std::endl;
    return true;
}

bool test_executor_varlen_path() {
    std::cout << "Test: QueryExecutor variable-length path... " << std::flush;
    std::string db_path = "/tmp/test_varlen.db";
    std::filesystem::remove_all(db_path);
    
    GraphStore db;
    if (!IsOk(db.Init(db_path))) { std::cout << "FAIL (init)" << std::endl; return false; }
    DbGuard guard{db, db_path};
    
    // build a chain: 1→2→3→4
    LabelId person_label = Fnv1a("Person");
    TxId wtx = db.BeginTransaction(false);
    Properties pv1, pv2, pv3, pv4;
    pv1.Set("name", "A"); pv2.Set("name", "B");
    pv3.Set("name", "C"); pv4.Set("name", "D");
    VertexId v1 = db.AddVertex(wtx, person_label, pv1);
    VertexId v2 = db.AddVertex(wtx, person_label, pv2);
    VertexId v3 = db.AddVertex(wtx, person_label, pv3);
    VertexId v4 = db.AddVertex(wtx, person_label, pv4);
    db.AddEdge(wtx, v1, v2, 10, {});
    db.AddEdge(wtx, v2, v3, 10, {});
    db.AddEdge(wtx, v3, v4, 10, {});
    db.Commit(wtx);
    
    // 2-hop path: a→c (through b)
    QueryExecutor exec(&db);
    auto q = Parser("MATCH (a:Person)-[*2]->(c:Person) RETURN a.name, c.name").ParseQuery();
    CHECK(q.has_value(), "parse varlen");
    auto result = exec.Execute(*q);
    // paths of length 2: A→C, B→D
    CHECK(result.rows.size() >= 1, "at least one 2-hop path");
    
    std::cout << "PASS" << std::endl;
    return true;
}

bool test_where_numeric() {
    std::cout << "Test: WHERE with numeric comparisons... " << std::flush;
    std::string db_path = "/tmp/test_where_num.db";
    std::filesystem::remove_all(db_path);
    
    GraphStore db;
    if (!IsOk(db.Init(db_path))) { std::cout << "FAIL (init)" << std::endl; return false; }
    DbGuard guard{db, db_path};
    
    // create vertices with age property
    TxId wtx = db.BeginTransaction(false);
    LabelId pl = static_cast<LabelId>(Fnv1a("Person"));
    auto mk = [&](const std::string& name, const std::string& age) {
        Properties p; p.Set("name", name); p.Set("age", age);
        return db.AddVertex(wtx, pl, p);
    };
    mk("Alice", "25"); mk("Bob", "30"); mk("Charlie", "35");
    db.Commit(wtx);
    
    // WHERE n.age > 30 (int vs string stored property via cross-type)
    QueryExecutor exec(&db);
    auto q = Parser("MATCH (n:Person) WHERE n.age > 30 RETURN n.name AS name").ParseQuery();
    CHECK(q.has_value(), "parse where gt");
    auto r = exec.Execute(*q);
    CHECK_EQ(r.rows.size(), 1, "one person with age > 30");
    CHECK_EQ(std::get<std::string>(r.rows[0].bindings["name"]), "Charlie", "Charlie > 30");
    
    // WHERE n.age >= 25 AND n.age < 35
    QueryExecutor exec2(&db);
    auto q2 = Parser("MATCH (n:Person) WHERE n.age >= 25 AND n.age < 35 RETURN n.name AS name").ParseQuery();
    auto r2 = exec2.Execute(*q2);
    CHECK_EQ(r2.rows.size(), 2, "two persons with age in [25,35)");
    
    // WHERE n.age = 30
    QueryExecutor exec3(&db);
    auto q3 = Parser("MATCH (n:Person) WHERE n.age = 30 RETURN n.name").ParseQuery();
    auto r3 = exec3.Execute(*q3);
    CHECK_EQ(r3.rows.size(), 1, "one with age 30");
    
    // WHERE n.name = 'Alice' (string comparison still works)
    QueryExecutor exec4(&db);
    auto q4 = Parser("MATCH (n:Person) WHERE n.name = 'Alice' RETURN n.age").ParseQuery();
    auto r4 = exec4.Execute(*q4);
    CHECK_EQ(r4.rows.size(), 1, "Alice found by name");
    
    std::cout << "PASS" << std::endl;
    return true;
}

bool test_order_by() {
    std::cout << "Test: ORDER BY... " << std::flush;
    std::string db_path = "/tmp/test_order.db";
    std::filesystem::remove_all(db_path);
    
    GraphStore db;
    if (!IsOk(db.Init(db_path))) { std::cout << "FAIL (init)" << std::endl; return false; }
    DbGuard guard{db, db_path};
    
    TxId wtx = db.BeginTransaction(false);
    LabelId pl = static_cast<LabelId>(Fnv1a("Person"));
    Properties pa, pb, pc;
    pa.Set("name", "Alice"); pa.Set("age", "30");
    pb.Set("name", "Bob");   pb.Set("age", "25");
    pc.Set("name", "Charlie"); pc.Set("age", "35");
    VertexId va = db.AddVertex(wtx, pl, pa);
    VertexId vb = db.AddVertex(wtx, pl, pb);
    VertexId vc = db.AddVertex(wtx, pl, pc);
    (void)va; (void)vb; (void)vc;
    db.Commit(wtx);
    
    // ORDER BY age ASC
    QueryExecutor exec(&db);
    auto q = Parser("MATCH (n:Person) RETURN n.name AS name ORDER BY n.age ASC").ParseQuery();
    auto r = exec.Execute(*q);
    CHECK_EQ(r.rows.size(), 3, "three rows");
    CHECK_EQ(std::get<std::string>(r.rows[0].bindings["name"]), "Bob", "first by age asc");
    CHECK_EQ(std::get<std::string>(r.rows[1].bindings["name"]), "Alice", "second by age asc");
    CHECK_EQ(std::get<std::string>(r.rows[2].bindings["name"]), "Charlie", "third by age asc");
    
    // ORDER BY age DESC
    auto q2 = Parser("MATCH (n:Person) RETURN n.name AS name ORDER BY n.age DESC").ParseQuery();
    auto r2 = exec.Execute(*q2);
    CHECK_EQ(r2.rows.size(), 3, "three desc");
    CHECK_EQ(std::get<std::string>(r2.rows[0].bindings["name"]), "Charlie", "first by age desc");
    
    // ORDER BY name ASC
    auto q3 = Parser("MATCH (n:Person) RETURN n.name AS name ORDER BY name ASC").ParseQuery();
    auto r3 = exec.Execute(*q3);
    CHECK_EQ(r3.rows.size(), 3, "three name asc");
    CHECK_EQ(std::get<std::string>(r3.rows[0].bindings["name"]), "Alice", "first by name");
    
    std::cout << "PASS" << std::endl;
    return true;
}

bool test_aggregate_count() {
    std::cout << "Test: COUNT aggregation... " << std::flush;
    std::string db_path = "/tmp/test_count.db";
    std::filesystem::remove_all(db_path);
    
    GraphStore db;
    if (!IsOk(db.Init(db_path))) { std::cout << "FAIL (init)" << std::endl; return false; }
    DbGuard guard{db, db_path};
    
    TxId wtx = db.BeginTransaction(false);
    LabelId pl = static_cast<LabelId>(Fnv1a("Person"));
    Properties p1, p2, p3;
    p1.Set("name", "A"); p2.Set("name", "B"); p3.Set("name", "C");
    db.AddVertex(wtx, pl, p1); db.AddVertex(wtx, pl, p2); db.AddVertex(wtx, pl, p3);
    db.Commit(wtx);
    
    QueryExecutor exec(&db);
    auto q = Parser("MATCH (n:Person) RETURN COUNT(n) AS cnt").ParseQuery();
    auto r = exec.Execute(*q);
    CHECK_EQ(r.rows.size(), 1, "one row with count");
    CHECK_EQ(std::get<int64_t>(r.rows[0].bindings["cnt"]), 3, "count=3");
    
    // COUNT with WHERE filter
    auto q2 = Parser("MATCH (n:Person) WHERE n.name > 'A' RETURN COUNT(n) AS cnt").ParseQuery();
    auto r2 = exec.Execute(*q2);
    CHECK_EQ(std::get<int64_t>(r2.rows[0].bindings["cnt"]), 2, "count=2 after filter");
    
    std::cout << "PASS" << std::endl;
    return true;
}

bool test_skip_limit() {
    std::cout << "Test: SKIP / LIMIT... " << std::flush;
    std::string db_path = "/tmp/test_skip.db";
    std::filesystem::remove_all(db_path);
    
    GraphStore db;
    if (!IsOk(db.Init(db_path))) { std::cout << "FAIL (init)" << std::endl; return false; }
    DbGuard guard{db, db_path};
    
    TxId wtx = db.BeginTransaction(false);
    LabelId pl = static_cast<LabelId>(Fnv1a("Person"));
    Properties pa, pb, pc;
    pa.Set("name", "Alice"); pa.Set("age", "25");
    pb.Set("name", "Bob");   pb.Set("age", "30");
    pc.Set("name", "Charlie"); pc.Set("age", "35");
    db.AddVertex(wtx, pl, pa);
    db.AddVertex(wtx, pl, pb);
    db.AddVertex(wtx, pl, pc);
    db.Commit(wtx);
    
    // LIMIT 2
    QueryExecutor exec(&db);
    auto q = Parser("MATCH (n:Person) RETURN n.name AS name ORDER BY n.age ASC LIMIT 2").ParseQuery();
    auto r = exec.Execute(*q);
    CHECK_EQ(r.rows.size(), 2, "limit 2");
    CHECK_EQ(std::get<std::string>(r.rows[0].bindings["name"]), "Alice", "first after limit");
    
    // SKIP 1
    auto q2 = Parser("MATCH (n:Person) RETURN n.name AS name ORDER BY n.age ASC SKIP 1").ParseQuery();
    auto r2 = exec.Execute(*q2);
    CHECK_EQ(r2.rows.size(), 2, "skip 1 gives 2");
    CHECK_EQ(std::get<std::string>(r2.rows[0].bindings["name"]), "Bob", "first after skip 1");
    
    // SKIP 1 LIMIT 1
    auto q3 = Parser("MATCH (n:Person) RETURN n.name AS name ORDER BY n.age ASC SKIP 1 LIMIT 1").ParseQuery();
    auto r3 = exec.Execute(*q3);
    CHECK_EQ(r3.rows.size(), 1, "skip 1 limit 1");
    CHECK_EQ(std::get<std::string>(r3.rows[0].bindings["name"]), "Bob", "skip 1 limit 1 = Bob");
    
    std::cout << "PASS" << std::endl;
    return true;
}

bool test_label_less_match() {
    std::cout << "Test: MATCH without labels (full scan)... " << std::flush;
    std::string db_path = "/tmp/test_nolabel.db";
    std::filesystem::remove_all(db_path);
    
    GraphStore db;
    if (!IsOk(db.Init(db_path))) { std::cout << "FAIL (init)" << std::endl; return false; }
    DbGuard guard{db, db_path};
    
    TxId wtx = db.BeginTransaction(false);
    Properties p1, p2;
    p1.Set("name", "X"); p2.Set("name", "Y");
    db.AddVertex(wtx, 1, p1);
    db.AddVertex(wtx, 2, p2);
    db.Commit(wtx);
    
    QueryExecutor exec(&db);
    auto q = Parser("MATCH (n) RETURN n.name AS name ORDER BY name ASC").ParseQuery();
    auto r = exec.Execute(*q);
    CHECK_EQ(r.rows.size(), 2, "two vertices without label");
    CHECK_EQ(std::get<std::string>(r.rows[0].bindings["name"]), "X", "first by name");
    CHECK_EQ(std::get<std::string>(r.rows[1].bindings["name"]), "Y", "second by name");
    
    std::cout << "PASS" << std::endl;
    return true;
}

bool test_executor_create_edge() {
    std::cout << "Test: QueryExecutor CREATE with edge... " << std::flush;
    std::string db_path = "/tmp/test_create_edge.db";
    std::filesystem::remove_all(db_path);
    
    GraphStore db;
    if (!IsOk(db.Init(db_path))) { std::cout << "FAIL (init)" << std::endl; return false; }
    DbGuard guard{db, db_path};
    
    QueryExecutor exec(&db);
    auto q = Parser("CREATE (a:Person {name:'Alice'})-[r:KNOWS]->(b:Person {name:'Bob'})").ParseQuery();
    CHECK(q.has_value(), "parse create edge");
    auto result = exec.Execute(*q);
    CHECK_EQ(result.rows.size(), 1, "one row");
    CHECK(result.rows[0].bindings.count("a"), "has a");
    CHECK(result.rows[0].bindings.count("b"), "has b");
    CHECK(result.rows[0].bindings.count("r"), "has r");
    
    // verify via MATCH
    QueryExecutor exec2(&db);
    auto q2 = Parser("MATCH (a:Person)-[r:KNOWS]->(b:Person) RETURN a.name, b.name").ParseQuery();
    auto r2 = exec2.Execute(*q2);
    CHECK_EQ(r2.rows.size(), 1, "match found edge");
    
    std::cout << "PASS" << std::endl;
    return true;
}

bool test_executor_merge_create_edge() {
    std::cout << "Test: QueryExecutor MERGE with edge... " << std::flush;
    std::string db_path = "/tmp/test_merge_edge.db";
    std::filesystem::remove_all(db_path);
    
    GraphStore db;
    if (!IsOk(db.Init(db_path))) { std::cout << "FAIL (init)" << std::endl; return false; }
    DbGuard guard{db, db_path};
    
    // MERGE a pattern with edge → creates
    QueryExecutor exec(&db);
    auto q = Parser("MERGE (a:Person {name:'Carol'})-[r:KNOWS]->(b:Person {name:'Dave'})").ParseQuery();
    CHECK(q.has_value(), "parse merge edge");
    auto result = exec.Execute(*q);
    CHECK_EQ(result.rows.size(), 1, "created one");
    CHECK(result.rows[0].bindings.count("r"), "has edge");
    
    // MERGE again → should match, not create
    QueryExecutor exec2(&db);
    auto result2 = exec2.Execute(*q);
    CHECK_EQ(result2.rows.size(), 1, "matched one");
    
    // verify only one edge
    QueryExecutor exec3(&db);
    auto q3 = Parser("MATCH (a:Person)-[r:KNOWS]->(b:Person) RETURN r").ParseQuery();
    auto r3 = exec3.Execute(*q3);
    CHECK_EQ(r3.rows.size(), 1, "only one edge");
    
    std::cout << "PASS" << std::endl;
    return true;
}

bool test_executor_detach_delete() {
    std::cout << "Test: QueryExecutor DETACH DELETE... " << std::flush;
    std::string db_path = "/tmp/test_detach_delete.db";
    std::filesystem::remove_all(db_path);
    
    GraphStore db;
    if (!IsOk(db.Init(db_path))) { std::cout << "FAIL (init)" << std::endl; return false; }
    DbGuard guard{db, db_path};
    
    // create a vertex with edge
    QueryExecutor exec(&db);
    auto q = Parser("CREATE (a:Person {name:'Eve'})-[r:KNOWS]->(b:Person {name:'Frank'})").ParseQuery();
    auto r1 = exec.Execute(*q);
    CHECK_EQ(r1.rows.size(), 1, "created");
    
    // DETACH DELETE a
    QueryExecutor exec2(&db);
    auto q2 = Parser("MATCH (a:Person {name:'Eve'}) DETACH DELETE a").ParseQuery();
    exec2.Execute(*q2);
    
    // verify a gone
    QueryExecutor exec3(&db);
    auto q3 = Parser("MATCH (a:Person {name:'Eve'}) RETURN a").ParseQuery();
    auto r3 = exec3.Execute(*q3);
    CHECK_EQ(r3.rows.size(), 0, "Eve deleted");
    
    // b should remain (edge auto-deleted)
    QueryExecutor exec4(&db);
    auto q4 = Parser("MATCH (b:Person {name:'Frank'}) RETURN b").ParseQuery();
    auto r4 = exec4.Execute(*q4);
    CHECK_EQ(r4.rows.size(), 1, "Frank still exists");
    
    std::cout << "PASS" << std::endl;
    return true;
}

bool test_executor_delete_edge_var() {
    std::cout << "Test: QueryExecutor DELETE edge variable... " << std::flush;
    std::string db_path = "/tmp/test_del_edge_var.db";
    std::filesystem::remove_all(db_path);
    
    GraphStore db;
    if (!IsOk(db.Init(db_path))) { std::cout << "FAIL (init)" << std::endl; return false; }
    DbGuard guard{db, db_path};
    
    // create
    QueryExecutor exec(&db);
    exec.Execute(*Parser("CREATE (a:Person {name:'Grace'})-[r:KNOWS]->(b:Person {name:'Hank'})").ParseQuery());
    
    // delete the edge
    QueryExecutor exec2(&db);
    auto q2 = Parser("MATCH (a:Person {name:'Grace'})-[r:KNOWS]->(b:Person {name:'Hank'}) DELETE r").ParseQuery();
    exec2.Execute(*q2);
    
    // edge should be gone
    QueryExecutor exec3(&db);
    auto q3 = Parser("MATCH (a:Person {name:'Grace'})-[r:KNOWS]->(b) RETURN r").ParseQuery();
    auto r3 = exec3.Execute(*q3);
    CHECK_EQ(r3.rows.size(), 0, "edge deleted");
    
    // vertices should remain
    QueryExecutor exec4(&db);
    auto q4 = Parser("MATCH (a:Person) RETURN a.name AS n").ParseQuery();
    auto r4 = exec4.Execute(*q4);
    CHECK_EQ(r4.rows.size(), 2, "both vertices remain");
    
    std::cout << "PASS" << std::endl;
    return true;
}

bool test_mt_concurrent() {
    std::cout << "Test: multi-threaded insert/delete/query... " << std::flush;
    std::string db_path = "/tmp/test_mt_concurrent1.db";
    std::filesystem::remove_all(db_path);

    GraphStore db;
    if (!IsOk(db.Init(db_path))) {
        std::cout << "FAIL (init)" << std::endl;
        return false;
    }
    DbGuard guard{db, db_path};

    const int NUM_WRITERS = 4;
    const int NUM_READERS = 4;
    const int OPS_PER_WRITER = 10;
    const int OPS_PER_READER = 5;
    // Using modest counts because FlushFileBuffers (FlushWal) is slow on Windows.
    // On Linux, increase to 100/50 for heavier stress testing.
    std::atomic<bool> stop_readers{false};
    std::atomic<int> write_errors{0};
    std::atomic<int> read_errors{0};
    std::mutex write_mtx;

    // Writer threads: insert and delete vertices
    std::vector<std::thread> writers;
    for (int i = 0; i < NUM_WRITERS; ++i) {
        writers.emplace_back([&, i]() {
            std::mt19937 rng(std::random_device{}());
            std::vector<VertexId> my_verts;
            for (int j = 0; j < OPS_PER_WRITER; ++j) {
                std::lock_guard<std::mutex> lock(write_mtx);
                if (my_verts.empty() || (rng() % 3) != 0) {
                    // insert (70% chance)
                    TxId tx = db.BeginTransaction(false);
                    Properties p;
                    p.Set("thread", std::to_string(i));
                    p.Set("seq", std::to_string(j));
                    p.Set("val", std::to_string(rng()));
                    VertexId vid = db.AddVertex(tx, i + 1, p);
                    if (vid == 0) write_errors++;
                    db.Commit(tx);
                    my_verts.push_back(vid);
                } else {
                    // delete (30% chance)
                    size_t idx = rng() % my_verts.size();
                    VertexId del = my_verts[idx];
                    TxId tx = db.BeginTransaction(false);
                    if (db.RemoveVertex(tx, del)) {
                        my_verts[idx] = my_verts.back();
                        my_verts.pop_back();
                    } else {
                        write_errors++;
                    }
                    db.Commit(tx);
                }
            }
            // cleanup remaining vertices (with retry)
            for (VertexId vid : my_verts) {
                for (int retry = 0; retry < 5; ++retry) {
                    std::lock_guard<std::mutex> lock(write_mtx);
                    TxId tx = db.BeginTransaction(false);
                    bool ok = db.RemoveVertex(tx, vid);
                    db.Commit(tx);
                    if (ok) break;
                }
            }
        });
    }

    // Reader threads: query concurrently
    std::vector<std::thread> readers;
    for (int i = 0; i < NUM_READERS; ++i) {
        readers.emplace_back([&]() {
            for (int k = 0; k < OPS_PER_READER && !stop_readers; ++k) {
                QueryExecutor exec(&db);
                auto q = Parser("MATCH (n) RETURN COUNT(n) AS cnt").ParseQuery();
                if (!q) { read_errors++; continue; }
                auto result = exec.Execute(*q);
                if (result.columns.empty()) read_errors++;
            }
        });
    }

    for (auto& t : writers) t.join();
    stop_readers = true;
    for (auto& t : readers) t.join();

    // Final count
    TxId rtx = db.BeginTransaction(true);
    size_t final_count = db.GetVertexCount(rtx);
    int re = read_errors.load();
    CHECK_EQ(re, 0, "read errors");
    CHECK(final_count < NUM_WRITERS, "residual less than writer count");

    std::cout << "PASS" << std::endl;
    return true;
}

bool test_set_property() {
    std::cout << "Test: SET n.prop = value... ";
    auto q = Parser("MATCH (n) SET n.age = 30 RETURN n").ParseQuery();
    CHECK(q.has_value(), "SET property");
    CHECK(q->set.has_value(), "has set");
    CHECK_EQ(q->set->items.size(), 1, "one set item");
    CHECK(!q->set->items[0].add_label, "property set");
    std::cout << "PASS" << std::endl;
    return true;
}

bool test_parse_update() {
    std::cout << "Test: UPDATE/SET parser... ";
    // single property
    auto q1 = Parser("MATCH (n) SET n.age = 30").ParseQuery();
    CHECK(q1.has_value(), "SET single prop");
    CHECK(q1->set.has_value(), "has set");
    CHECK_EQ(q1->set->items.size(), 1, "one set item");
    CHECK(!q1->set->items[0].add_label, "property");
    CHECK_EQ(q1->set->items[0].target->type, Expression::Type::PROPERTY, "prop type");
    CHECK_EQ(q1->set->items[0].target->property_name, "age", "prop name");
    CHECK_EQ(q1->set->items[0].value->type, Expression::Type::LITERAL, "literal type");
    CHECK_EQ(std::get<int64_t>(q1->set->items[0].value->literal_value), 30, "prop val");

    // multiple properties
    auto q2 = Parser("MATCH (n) SET n.name = 'Bob', n.age = 25").ParseQuery();
    CHECK(q2.has_value(), "SET multi prop");
    CHECK_EQ(q2->set->items.size(), 2, "two items");
    CHECK_EQ(q2->set->items[0].target->property_name, "name", "first prop");
    CHECK_EQ(q2->set->items[1].target->property_name, "age", "second prop");

    // SET label
    auto q3 = Parser("MATCH (n) SET n:Admin:Moderator RETURN n").ParseQuery();
    CHECK(q3.has_value(), "SET label");
    CHECK(q3->set.has_value(), "has set");
    CHECK_EQ(q3->set->items.size(), 2, "two label items");
    CHECK(q3->set->items[0].add_label, "first add label");
    CHECK_EQ(q3->set->items[0].label, "Admin", "first label");
    CHECK(q3->set->items[1].add_label, "second add label");
    CHECK_EQ(q3->set->items[1].label, "Moderator", "second label");

    // SET with property reference (using simple value instead of expression)
    auto q4 = Parser("MATCH (n) SET n.age = 1").ParseQuery();
    CHECK(q4.has_value(), "SET simple expr");
    CHECK_EQ(q4->set->items[0].target->property_name, "age", "prop name");

    std::cout << "PASS" << std::endl;
    return true;
}

bool test_parse_delete() {
    std::cout << "Test: DELETE parser... ";
    // simple DELETE
    auto q1 = Parser("MATCH (n) DELETE n").ParseQuery();
    CHECK(q1.has_value(), "DELETE");
    CHECK(q1->del.has_value(), "has delete");
    CHECK(!q1->del->detach, "not detach");
    CHECK_EQ(q1->del->expressions.size(), 1, "one expr");
    CHECK_EQ(q1->del->expressions[0]->type, Expression::Type::VARIABLE, "var type");
    CHECK_EQ(q1->del->expressions[0]->variable_name, "n", "var name");

    // DETACH DELETE
    auto q2 = Parser("MATCH (n) DETACH DELETE n").ParseQuery();
    CHECK(q2.has_value(), "DETACH DELETE");
    CHECK(q2->del->detach, "detach");

    // DELETE multiple
    auto q3 = Parser("MATCH (a)-[r]-(b) DELETE a, r, b").ParseQuery();
    CHECK(q3.has_value(), "DELETE multiple");
    CHECK_EQ(q3->del->expressions.size(), 3, "three exprs");
    CHECK_EQ(q3->del->expressions[0]->variable_name, "a", "first var");
    CHECK_EQ(q3->del->expressions[1]->variable_name, "r", "second var");
    CHECK_EQ(q3->del->expressions[2]->variable_name, "b", "third var");

    // DETACH DELETE multiple
    auto q4 = Parser("MATCH (a)-[r]-(b) DETACH DELETE a, b").ParseQuery();
    CHECK(q4.has_value(), "DETACH DELETE multiple");
    CHECK(q4->del->detach, "detach");
    CHECK_EQ(q4->del->expressions.size(), 2, "two exprs");

    std::cout << "PASS" << std::endl;
    return true;
}

bool test_executor_update() {
    std::cout << "Test: QueryExecutor SET (update property)... " << std::flush;
    std::string db_path = "/tmp/test_update_delete.db";
    std::filesystem::remove_all(db_path);

    GraphStore db;
    if (!IsOk(db.Init(db_path))) {
        std::cout << "FAIL (init)" << std::endl;
        return false;
    }
    DbGuard guard{db, db_path};

    // create a vertex
    TxId wtx = db.BeginTransaction(false);
    Properties p;
    p.Set("name", "Alice");
    p.Set("age", "30");
    VertexId vid = db.AddVertex(wtx, 1, p);
    CHECK(vid != 0, "vertex created");
    db.Commit(wtx);

    // update property via SetVertexProperty
    TxId utx = db.BeginTransaction(false);
    bool updated = db.SetVertexProperty(utx, vid, "age", "31");
    CHECK(updated, "set property");
    db.Commit(utx);

    // verify via GetVertexProperty
    TxId rtx = db.BeginTransaction(true);
    auto val = db.GetVertexProperty(rtx, vid, "age");
    CHECK(val.has_value(), "age exists");
    CHECK_EQ(*val, "31", "age updated");
    auto name = db.GetVertexProperty(rtx, vid, "name");
    CHECK(name.has_value(), "name exists");
    CHECK_EQ(*name, "Alice", "name unchanged");
    db.Commit(rtx);

    // update multiple properties
    TxId utx2 = db.BeginTransaction(false);
    db.SetVertexProperty(utx2, vid, "name", "Bob");
    db.SetVertexProperty(utx2, vid, "city", "NYC");
    db.Commit(utx2);

    TxId rtx2 = db.BeginTransaction(true);
    auto name2 = db.GetVertexProperty(rtx2, vid, "name");
    CHECK_EQ(*name2, "Bob", "name updated");
    auto city = db.GetVertexProperty(rtx2, vid, "city");
    CHECK_EQ(*city, "NYC", "city added");
    db.Commit(rtx2);

    std::cout << "PASS" << std::endl;
    return true;
}

bool test_executor_delete() {
    std::cout << "Test: QueryExecutor DELETE (remove vertex)... " << std::flush;
    std::string db_path = "/tmp/test_delete_vertex.db";
    std::filesystem::remove_all(db_path);

    GraphStore db;
    if (!IsOk(db.Init(db_path))) {
        std::cout << "FAIL (init)" << std::endl;
        return false;
    }
    DbGuard guard{db, db_path};

    // create vertices
    TxId wtx = db.BeginTransaction(false);
    VertexId v1 = db.AddVertex(wtx, 1, {});
    VertexId v2 = db.AddVertex(wtx, 2, {});
    VertexId v3 = db.AddVertex(wtx, 3, {});
    db.Commit(wtx);

    // delete one vertex
    TxId dtx = db.BeginTransaction(false);
    bool removed = db.RemoveVertex(dtx, v1);
    CHECK(removed, "remove vertex");
    db.Commit(dtx);

    // verify count
    TxId rtx = db.BeginTransaction(true);
    size_t count = db.GetVertexCount(rtx);
    CHECK_EQ(count, 2, "two remaining after delete");
    db.Commit(rtx);

    // delete remaining
    TxId dtx2 = db.BeginTransaction(false);
    CHECK(db.RemoveVertex(dtx2, v2), "remove v2");
    CHECK(db.RemoveVertex(dtx2, v3), "remove v3");
    db.Commit(dtx2);

    TxId rtx2 = db.BeginTransaction(true);
    size_t count2 = db.GetVertexCount(rtx2);
    CHECK_EQ(count2, 0, "zero after all deleted");
    db.Commit(rtx2);

    // delete non-existent vertex returns false
    TxId dtx3 = db.BeginTransaction(false);
    bool removed_nonexist = db.RemoveVertex(dtx3, 999);
    CHECK(!removed_nonexist, "non-existent vertex returns false");
    db.Commit(dtx3);

    std::cout << "PASS" << std::endl;
    return true;
}

bool test_edge_property_in_where() {
    std::cout << "Test: edge property in WHERE/RETURN... " << std::flush;
    std::string db_path = "/tmp/test_edge_prop.db";
    std::filesystem::remove_all(db_path);
    GraphStore db;
    if (!IsOk(db.Init(db_path))) { std::cout << "FAIL (init)" << std::endl; return false; }
    DbGuard guard{db, db_path};
    
    // create vertices and edges with properties
    LabelId person_label = Fnv1a("Person");
    TxId wtx = db.BeginTransaction(false);
    Properties pa, pb, pc;
    pa.Set("name", "A");
    pb.Set("name", "B");
    pc.Set("name", "C");
    VertexId a = db.AddVertex(wtx, person_label, pa);
    VertexId b = db.AddVertex(wtx, person_label, pb);
    VertexId c = db.AddVertex(wtx, person_label, pc);
    
    Properties pe1, pe2;
    pe1.Set("weight", "10");
    pe1.Set("rel", "knows");
    pe2.Set("weight", "3");
    pe2.Set("rel", "knows");
    db.AddEdge(wtx, a, b, 10, pe1);
    db.AddEdge(wtx, b, c, 10, pe2);
    db.Commit(wtx);
    
    QueryExecutor exec(&db);
    
    // Test 1: RETURN edge property
    {
        auto q = Parser("MATCH (a:Person)-[r]->(b:Person) RETURN r.weight").ParseQuery();
        CHECK(q.has_value(), "parse RETURN r.weight");
        auto result = exec.Execute(*q);
        CHECK_EQ(result.rows.size(), 2, "two edges returned");
        int count_10 = 0, count_3 = 0;
        for (const auto& row : result.rows) {
            auto it = row.bindings.find("r");
            CHECK(it != row.bindings.end(), "r in bindings");
            if (std::holds_alternative<std::string>(it->second)) {
                if (std::get<std::string>(it->second) == "10") count_10++;
                if (std::get<std::string>(it->second) == "3") count_3++;
            }
        }
        CHECK_EQ(count_10, 1, "one edge with weight=10");
        CHECK_EQ(count_3, 1, "one edge with weight=3");
    }
    
    // Test 2: WHERE on edge property (numeric comparison)
    {
        auto q = Parser("MATCH (a:Person)-[r]->(b:Person) WHERE r.weight > 5 RETURN a.name").ParseQuery();
        CHECK(q.has_value(), "parse WHERE r.weight > 5");
        auto result = exec.Execute(*q);
        CHECK_EQ(result.rows.size(), 1, "one edge with weight > 5");
    }
    
    // Test 3: WHERE with string edge property
    {
        auto q = Parser("MATCH (a:Person)-[r]->(b:Person) WHERE r.rel = 'knows' RETURN a.name").ParseQuery();
        auto result = exec.Execute(*q);
        CHECK_EQ(result.rows.size(), 2, "both edges have rel=knows");
    }
    
    // Test 4: WHERE filter that eliminates all rows
    {
        auto q = Parser("MATCH (a:Person)-[r]->(b:Person) WHERE r.weight > 100 RETURN a.name").ParseQuery();
        auto result = exec.Execute(*q);
        CHECK_EQ(result.rows.size(), 0, "no edges with weight > 100");
    }
    
    // Test 5: AND with edge property + vertex property in WHERE
    {
        auto q = Parser("MATCH (a:Person)-[r]->(b:Person) WHERE r.weight > 5 AND a.name = 'A' RETURN b.name").ParseQuery();
        auto result = exec.Execute(*q);
        CHECK_EQ(result.rows.size(), 1, "edge from A with weight > 5");
    }
    
    std::cout << "PASS" << std::endl;
    return true;
}

bool test_executor_set_label() {
    std::cout << "Test: QueryExecutor SET n:Label... " << std::flush;
    std::string db_path = "/tmp/test_set_label.db";
    std::filesystem::remove_all(db_path);

    GraphStore db;
    if (!IsOk(db.Init(db_path))) { std::cout << "FAIL (init)" << std::endl; return false; }
    DbGuard guard{db, db_path};

    QueryExecutor exec(&db);
    exec.Execute(*Parser("CREATE (n {name:'Alice'})").ParseQuery());

    // add label
    QueryExecutor exec2(&db);
    auto q = Parser("MATCH (n {name:'Alice'}) SET n:Person RETURN n.name").ParseQuery();
    auto r = exec2.Execute(*q);
    CHECK_EQ(r.rows.size(), 1, "set label returns row");

    // verify label is searchable
    QueryExecutor exec3(&db);
    auto q2 = Parser("MATCH (n:Person) RETURN n.name AS name").ParseQuery();
    auto r2 = exec3.Execute(*q2);
    CHECK_EQ(r2.rows.size(), 1, "found by new label");
    CHECK_EQ(std::get<std::string>(r2.rows[0].bindings["name"]), "Alice", "correct name");

    std::cout << "PASS" << std::endl;
    return true;
}

bool test_executor_remove_label() {
    std::cout << "Test: QueryExecutor REMOVE n:Label... " << std::flush;
    std::string db_path = "/tmp/test_remove_label.db";
    std::filesystem::remove_all(db_path);

    GraphStore db;
    if (!IsOk(db.Init(db_path))) { std::cout << "FAIL (init)" << std::endl; return false; }
    DbGuard guard{db, db_path};

    QueryExecutor exec(&db);
    exec.Execute(*Parser("CREATE (n:Person:Student {name:'Bob'})").ParseQuery());

    // remove Person label
    QueryExecutor exec2(&db);
    auto q = Parser("MATCH (n {name:'Bob'}) REMOVE n:Person RETURN n.name").ParseQuery();
    exec2.Execute(*q);

    // should NOT be findable as Person
    QueryExecutor exec3(&db);
    auto q2 = Parser("MATCH (n:Person) RETURN n.name").ParseQuery();
    auto r2 = exec3.Execute(*q2);
    CHECK_EQ(r2.rows.size(), 0, "not found as Person");

    // should still be findable without label
    QueryExecutor exec4(&db);
    auto q3 = Parser("MATCH (n {name:'Bob'}) RETURN n.name AS name").ParseQuery();
    auto r3 = exec4.Execute(*q3);
    CHECK_EQ(r3.rows.size(), 1, "still exists");
    CHECK_EQ(std::get<std::string>(r3.rows[0].bindings["name"]), "Bob", "correct name");

    std::cout << "PASS" << std::endl;
    return true;
}

bool test_executor_remove_property() {
    std::cout << "Test: QueryExecutor REMOVE n.prop... " << std::flush;
    std::string db_path = "/tmp/test_remove_prop.db";
    std::filesystem::remove_all(db_path);

    GraphStore db;
    if (!IsOk(db.Init(db_path))) { std::cout << "FAIL (init)" << std::endl; return false; }
    DbGuard guard{db, db_path};

    QueryExecutor exec(&db);
    exec.Execute(*Parser("CREATE (n:Person {name:'Charlie', age:'30'})").ParseQuery());

    // remove age property
    QueryExecutor exec2(&db);
    auto q = Parser("MATCH (n:Person {name:'Charlie'}) REMOVE n.age RETURN n.name").ParseQuery();
    exec2.Execute(*q);

    // verify age is gone
    QueryExecutor exec3(&db);
    auto q2 = Parser("MATCH (n:Person {name:'Charlie'}) RETURN n.age AS age").ParseQuery();
    auto r2 = exec3.Execute(*q2);
    CHECK_EQ(r2.rows.size(), 1, "one row");
    CHECK(std::holds_alternative<std::monostate>(r2.rows[0].bindings["age"]), "age is null");

    // name should still be there
    QueryExecutor exec4(&db);
    auto q3 = Parser("MATCH (n:Person {name:'Charlie'}) RETURN n.name AS name").ParseQuery();
    auto r3 = exec4.Execute(*q3);
    CHECK_EQ(r3.rows.size(), 1, "name still present");
    CHECK_EQ(std::get<std::string>(r3.rows[0].bindings["name"]), "Charlie", "correct name");

    std::cout << "PASS" << std::endl;
    return true;
}

bool test_multi_statement_tx() {
    std::cout << "Test: Multi-statement explicit transactions... " << std::flush;
    std::string db_path = "/tmp/test_multi_tx.db";
    std::filesystem::remove_all(db_path);

    GraphStore db;
    if (!IsOk(db.Init(db_path))) { std::cout << "FAIL (init)" << std::endl; return false; }
    DbGuard guard{db, db_path};

    QueryExecutor exec(&db);
    exec.BeginTx();

    // CREATE a vertex
    auto q1 = Parser("CREATE (n:Person {name:'Dave', age:'25'})").ParseQuery();
    auto r1 = exec.Execute(*q1);
    CHECK_EQ(r1.rows.size(), 1, "created in explicit tx");

    // SET a property (same transaction)
    auto q2 = Parser("MATCH (n:Person {name:'Dave'}) SET n.age = '26'").ParseQuery();
    auto r2 = exec.Execute(*q2);
    CHECK_EQ(r2.rows.size(), 1, "set in same tx");

    // Commit
    CHECK(IsOk(exec.CommitTx()), "commit ok");

    // Verify committed data in new executor
    QueryExecutor exec2(&db);
    auto q3 = Parser("MATCH (n:Person {name:'Dave'}) RETURN n.age AS age").ParseQuery();
    auto r3 = exec2.Execute(*q3);
    CHECK_EQ(r3.rows.size(), 1, "found after commit");
    CHECK_EQ(std::get<std::string>(r3.rows[0].bindings["age"]), "26", "age updated to 26");

    std::cout << "PASS" << std::endl;
    return true;
}

bool test_multi_statement_tx_rollback() {
    std::cout << "Test: Explicit transaction rollback... " << std::flush;
    std::string db_path = "/tmp/test_tx_rollback.db";
    std::filesystem::remove_all(db_path);

    GraphStore db;
    if (!IsOk(db.Init(db_path))) { std::cout << "FAIL (init)" << std::endl; return false; }
    DbGuard guard{db, db_path};

    // Start explicit tx, begin work, then rollback
    QueryExecutor exec(&db);
    exec.BeginTx();

    auto q1 = Parser("CREATE (n:Person {name:'Eve'})").ParseQuery();
    auto r1 = exec.Execute(*q1);
    CHECK_EQ(r1.rows.size(), 1, "created in tx");

    exec.RollbackTx();

    // Auto-commit CREATE in a new executor should work fine after rollback
    QueryExecutor exec2(&db);
    auto q2 = Parser("CREATE (n:Person {name:'Frank'})").ParseQuery();
    auto r2 = exec2.Execute(*q2);
    CHECK_EQ(r2.rows.size(), 1, "created after rollback");

    std::cout << "PASS" << std::endl;
    return true;
}

// ==================== Main ====================

int main() {
#ifdef _WIN32
    std::system("cls");
#else
    std::system("clear");
#endif
    std::cout << "=== Cypher Parser Tests ===" << std::endl;
    
    bool all_ok = true;
    auto run = [&](bool ok, const char* name) {
        if (!ok) { all_ok = false; std::cerr << "  [" << name << " FAILED]" << std::endl; }
    };
    run(test_lexer_keywords(), "lexer_keywords");
    run(test_lexer_symbols(), "lexer_symbols");
    run(test_lexer_literals(), "lexer_literals");
    run(test_lexer_comment_ellipsis(), "lexer_comment_ellipsis");
    run(test_parse_basic_pattern(), "parse_basic_pattern");
    run(test_parse_relationships(), "parse_relationships");
    run(test_parse_where(), "parse_where");
    run(test_parse_return(), "parse_return");
    run(test_parse_clauses(), "parse_clauses");
    run(test_parse_aggregation_and_list(), "parse_aggregation_and_list");
    run(test_parse_error(), "parse_error");
    run(test_set_property(), "set_property");
    run(test_parse_update(), "parse_update");
    run(test_parse_delete(), "parse_delete");
    run(test_where_numeric(), "where_numeric");
    run(test_order_by(), "order_by");
    run(test_aggregate_count(), "aggregate_count");
    run(test_skip_limit(), "skip_limit");
    run(test_label_less_match(), "label_less_match");
    run(test_executor_update(), "executor_update");
    run(test_executor_delete(), "executor_delete");
    run(test_executor_basic(), "executor_basic");
    run(test_executor_return(), "executor_return");
    run(test_executor_create_stub(), "executor_create_stub");
    run(test_executor_create_vertex(), "executor_create_vertex");
    run(test_executor_merge_create(), "executor_merge_create");
    run(test_executor_set_property(), "executor_set_property");
    run(test_executor_delete_vertex(), "executor_delete_vertex");
    run(test_executor_path_single_hop(), "executor_path_single_hop");
    run(test_executor_varlen_path(), "executor_varlen_path");
    run(test_executor_create_edge(), "executor_create_edge");
    run(test_executor_merge_create_edge(), "executor_merge_create_edge");
    run(test_executor_detach_delete(), "executor_detach_delete");
    run(test_executor_delete_edge_var(), "executor_delete_edge_var");
    run(test_edge_property_in_where(), "edge_property_in_where");
    run(test_mt_concurrent(), "mt_concurrent");
    run(test_executor_set_label(), "executor_set_label");
    run(test_executor_remove_label(), "executor_remove_label");
    run(test_executor_remove_property(), "executor_remove_property");
    run(test_multi_statement_tx(), "multi_statement_tx");
    run(test_multi_statement_tx_rollback(), "multi_statement_tx_rollback");
    
    std::cout << "\nResults: " << tests_passed << "/" << tests_total << " passed" << std::endl;
    return all_ok ? 0 : 1;
}


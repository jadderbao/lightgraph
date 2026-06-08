#include <iostream>
#include <string>
#include <vector>
#include <filesystem>
#include <sstream>
#include <iomanip>
#include "graph_store.h"
#include "cypher_parser.h"
#include "version.h"

using namespace graphstore;

static void print_version() {
    std::cout << "LightGraph v" << Version::String << std::endl;
}

static void print_help() {
    std::cout << "Graph Database Admin Tool v" << Version::String << std::endl;
    std::cout << "Usage: db_admin <command> [args...]" << std::endl;
    std::cout << std::endl;
    std::cout << "Commands:" << std::endl;
    std::cout << "  init <path> [size_mb]        Initialize a new graph database (default 64MB)" << std::endl;
    std::cout << "  info <path>                  Show database statistics" << std::endl;
    std::cout << "  query <path> <cypher...>     Execute a Cypher query" << std::endl;
    std::cout << "  repl <path>                  Interactive REPL mode" << std::endl;
    std::cout << "  backup <path> <dest>         Backup database to destination" << std::endl;
    std::cout << "  compact <path>               Run garbage collection and compact" << std::endl;
    std::cout << "  dump <path>                  Dump all vertices and edges" << std::endl;
    std::cout << "  help                         Show this help" << std::endl;
}

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

static bool open_db(GraphStore& db, const std::string& path, size_t map_size = 64ULL << 20) {
    Status s = db.Init(path, map_size);
    if (!IsOk(s)) {
        std::cerr << "Error: Failed to open database at '" << path << "'" << std::endl;
        return false;
    }
    return true;
}

static void cmd_init(const std::string& path, size_t map_size) {
    GraphStore db;
    if (!open_db(db, path, map_size)) return;
    std::cout << "Database initialized: " << path;
    if (map_size != 64ULL << 20)
        std::cout << " (" << (map_size >> 20) << "MB)";
    std::cout << std::endl;
    db.Close();
}

static void cmd_info(const std::string& path) {
    GraphStore db;
    if (!open_db(db, path)) return;
    TxId rtx = db.BeginTransaction(true);
    std::cout << "Database: " << path << std::endl;
    std::cout << "  Vertices: " << db.GetVertexCount(rtx) << std::endl;
    std::cout << "  Edges:    " << db.GetEdgeCount(rtx) << std::endl;
    db.Commit(rtx);
    db.Close();
}

static void cmd_query(const std::string& path, const std::string& cypher) {
    GraphStore db;
    if (!open_db(db, path)) return;
    auto parsed = Parser(cypher).ParseQuery();
    if (!parsed) {
        std::cerr << "Error: Failed to parse query" << std::endl;
        db.Close();
        return;
    }
    QueryExecutor exec(&db);
    try {
        auto result = exec.Execute(*parsed);
        print_result(result);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }
    db.Close();
}

static void cmd_repl(const std::string& path) {
    GraphStore db;
    if (!open_db(db, path)) return;
    std::cout << "GraphDB REPL v" << Version::String << " — type 'exit' or 'quit' to quit, 'help' for help" << std::endl;
    std::string line;
    while (true) {
        std::cout << "> ";
        if (!std::getline(std::cin, line)) break;
        if (line.empty()) continue;
        if (line == "exit" || line == "quit") break;
        if (line == "help") {
            std::cout << "Enter any Cypher query." << std::endl;
            std::cout << "  MATCH (n) RETURN n" << std::endl;
            std::cout << "  CREATE (n:Person {name: 'Alice'})" << std::endl;
            std::cout << "  MATCH (n) WHERE n.age > 30 RETURN n.name" << std::endl;
            std::cout << "Commands: exit, quit, help" << std::endl;
            continue;
        }
        auto parsed = Parser(line).ParseQuery();
        if (!parsed) {
            std::cout << "Parse error" << std::endl;
            continue;
        }
        QueryExecutor exec(&db);
        try {
            auto result = exec.Execute(*parsed);
            print_result(result);
        } catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << std::endl;
        }
    }
    db.Close();
}

static void cmd_backup(const std::string& path, const std::string& dest) {
    GraphStore db;
    if (!open_db(db, path)) return;
    Status s = db.Backup(std::filesystem::path(dest));
    if (IsOk(s)) {
        std::cout << "Backup created: " << dest << std::endl;
    } else {
        std::cerr << "Error: Backup failed" << std::endl;
    }
    db.Close();
}

static void cmd_compact(const std::string& path) {
    GraphStore db;
    if (!open_db(db, path)) return;
    Status s = db.Compact();
    if (IsOk(s)) {
        std::cout << "Compaction completed" << std::endl;
    } else {
        std::cerr << "Error: Compaction failed" << std::endl;
    }
    db.Close();
}

static void cmd_dump(const std::string& path) {
    GraphStore db;
    if (!open_db(db, path)) return;
    TxId rtx = db.BeginTransaction(true);
    auto all_vertices = db.GetAllVertices(rtx);
    std::cout << "=== Vertices (" << all_vertices.size() << ") ===" << std::endl;
    for (auto vid : all_vertices) {
        auto v = db.GetVertex(rtx, vid);
        if (!v) continue;
        std::cout << "  [" << vid << "] label=" << v->label;
        auto name = db.GetVertexProperty(rtx, vid, "name");
        if (name) std::cout << " name='" << *name << "'";
        std::cout << " out=" << v->out_degree << " in=" << v->in_degree;
        std::cout << std::endl;
        auto out_edges = db.GetOutEdges(rtx, vid);
        for (auto eid : out_edges) {
            auto e = db.GetEdge(rtx, eid);
            if (!e) continue;
            std::cout << "    -[" << eid << " label=" << e->label << "]-> " << e->dst;
            auto since = db.GetEdgeProperty(rtx, eid, "since");
            if (since) std::cout << " (since=" << *since << ")";
            std::cout << std::endl;
        }
    }
    db.Commit(rtx);
    db.Close();
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_help();
        return 1;
    }
    std::string cmd = argv[1];
    if (cmd == "help" || cmd == "--help" || cmd == "-h") {
        print_help();
        return 0;
    }
    if (cmd == "--version" || cmd == "-V") {
        print_version();
        return 0;
    }
    if (cmd == "init") {
        if (argc < 3) { std::cerr << "Usage: db_admin init <path> [size_mb]" << std::endl; return 1; }
        size_t map_size = 64ULL << 20;
        if (argc >= 4) {
            try { map_size = static_cast<size_t>(std::stoll(argv[3])) << 20; }
            catch (...) { std::cerr << "Error: invalid size_mb '" << argv[3] << "'" << std::endl; return 1; }
        }
        cmd_init(argv[2], map_size);
    } else if (cmd == "info") {
        if (argc < 3) { std::cerr << "Usage: db_admin info <path>" << std::endl; return 1; }
        cmd_info(argv[2]);
    } else if (cmd == "query") {
        if (argc < 4) { std::cerr << "Usage: db_admin query <path> <cypher...>" << std::endl; return 1; }
        std::ostringstream oss;
        for (int i = 3; i < argc; ++i) {
            if (i > 3) oss << " ";
            oss << argv[i];
        }
        cmd_query(argv[2], oss.str());
    } else if (cmd == "repl") {
        if (argc < 3) { std::cerr << "Usage: db_admin repl <path>" << std::endl; return 1; }
        cmd_repl(argv[2]);
    } else if (cmd == "backup") {
        if (argc < 4) { std::cerr << "Usage: db_admin backup <path> <dest>" << std::endl; return 1; }
        cmd_backup(argv[2], argv[3]);
    } else if (cmd == "compact") {
        if (argc < 3) { std::cerr << "Usage: db_admin compact <path>" << std::endl; return 1; }
        cmd_compact(argv[2]);
    } else if (cmd == "dump") {
        if (argc < 3) { std::cerr << "Usage: db_admin dump <path>" << std::endl; return 1; }
        cmd_dump(argv[2]);
    } else {
        std::cerr << "Unknown command: " << cmd << std::endl;
        print_help();
        return 1;
    }
    return 0;
}

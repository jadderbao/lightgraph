#include "graph_store.h"
#include "cypher_parser.h"
#include <iostream>

using namespace graphstore;

int main() {
    std::string db_path = "/tmp/debug_varlen.db";
    std::filesystem::remove_all(db_path);
    
    GraphStore db;
    auto s = db.Init(db_path);
    if (!IsOk(s)) { std::cerr << "init failed\n"; return 1; }
    
    // Test: create vertex via executor
    {
        QueryExecutor exec(&db);
        auto q = Parser("CREATE (n:Person {name: 'Charlie', age: '25'})").ParseQuery();
        if (!q) { std::cerr << "parse failed\n"; return 1; }
        auto result = exec.Execute(*q);
        std::cout << "CREATE rows: " << result.rows.size() << std::endl;
        if (!result.rows.empty()) {
            for (const auto& [k, v] : result.rows[0].bindings) {
                std::cout << "  binding: " << k << " = ";
                if (std::holds_alternative<int64_t>(v)) std::cout << std::get<int64_t>(v);
                else if (std::holds_alternative<std::string>(v)) std::cout << std::get<std::string>(v);
                else std::cout << "other";
                std::cout << std::endl;
            }
        }
    }
    
    // Try MATCH in a new executor
    {
        QueryExecutor exec2(&db);
        auto q2 = Parser("MATCH (n:Person {name: 'Charlie'}) RETURN n.name AS name").ParseQuery();
        if (!q2) { std::cerr << "parse q2 failed\n"; return 1; }
        auto result2 = exec2.Execute(*q2);
        std::cout << "MATCH rows: " << result2.rows.size() << std::endl;
    }
    
    // Also try raw API to verify
    {
        TxId rtx = db.BeginTransaction(true);
        auto vertices = db.GetAllVertices(rtx);
        std::cout << "Raw GetAllVertices: " << vertices.size() << std::endl;
        auto label_vertices = db.GetVerticesByLabel(rtx, Fnv1a("Person"));
        std::cout << "Raw GetVerticesByLabel(Person): " << label_vertices.size() << std::endl;
        for (auto vid : label_vertices) {
            auto v = db.GetVertex(rtx, vid);
            if (v) std::cout << "  Vertex " << vid << " label=" << v->label << std::endl;
            auto prop = db.GetVertexProperty(rtx, vid, "name");
            if (prop) std::cout << "  name = " << *prop << std::endl;
        }
        db.Commit(rtx);
    }
    
    // Check if the issue is with QueryExecutor's destructor closing the read tx
    std::cout << "Done." << std::endl;
    return 0;
}

#include "graph_store.h"
#include "cypher_parser.h"
#include <iostream>

using namespace graphstore;

int main() {
    std::string db_path = "/tmp/debug_v2.db";
    std::filesystem::remove_all(db_path);
    
    GraphStore db;
    auto s = db.Init(db_path);
    if (!IsOk(s)) { std::cerr << "init failed\n"; return 1; }
    
    // Test 1: Create via executor
    std::cout << "=== Test 1: Create via executor ===\n";
    {
        QueryExecutor exec(&db);
        auto q = Parser("CREATE (n:Person {name:'X', age:'1'})").ParseQuery();
        auto r = exec.Execute(*q);
        std::cout << "CREATE rows:" << r.rows.size() << " has_n:" << r.rows[0].bindings.count("n") << "\n";
    }
    
    // Test 2: Raw read
    std::cout << "=== Test 2: Raw read after create ===\n";
    {
        TxId rtx = db.BeginTransaction(true);
        auto all = db.GetAllVertices(rtx);
        std::cout << "GetAllVertices:" << all.size() << "\n";
        for (auto v : all) {
            auto vt = db.GetVertex(rtx, v);
            std::cout << " vid:" << v << " label:" << (vt ? vt->label : -1) << "\n";
        }
        db.Commit(rtx);
    }
    
    // Test 3: Create same data via raw API
    std::cout << "=== Test 3: Create via raw API ===\n";
    {
        TxId wtx = db.BeginTransaction(false);
        Properties p; p.Set("name", "Y"); p.Set("age", "2");
        auto vid = db.AddVertex(wtx, Fnv1a("Person"), p);
        std::cout << "AddVertex result:" << vid << "\n";
        db.Commit(wtx);
    }
    
    // Test 4: Raw read after raw create
    std::cout << "=== Test 4: Raw read after raw create ===\n";
    {
        TxId rtx = db.BeginTransaction(true);
        auto all = db.GetAllVertices(rtx);
        std::cout << "GetAllVertices:" << all.size() << "\n";
        for (auto v : all) {
            auto vt = db.GetVertex(rtx, v);
            auto nm = db.GetVertexProperty(rtx, v, "name");
            std::cout << " vid:" << v << " label:" << (vt ? vt->label : -1) 
                      << " name:" << (nm ? *nm : "NULL") << "\n";
        }
        db.Commit(rtx);
    }
    
    // Test 5: Create via executor (second time)
    std::cout << "=== Test 5: Second create via executor ===\n";
    {
        QueryExecutor exec2(&db);
        auto q2 = Parser("CREATE (n:Person {name:'Z', age:'3'})").ParseQuery();
        auto r2 = exec2.Execute(*q2);
        std::cout << "CREATE rows:" << r2.rows.size() << "\n";
    }
    
    // Test 6: Read all
    std::cout << "=== Test 6: Read all after everything ===\n";
    {
        TxId rtx = db.BeginTransaction(true);
        auto all = db.GetAllVertices(rtx);
        std::cout << "GetAllVertices:" << all.size() << "\n";
        for (auto v : all) {
            auto vt = db.GetVertex(rtx, v);
            auto nm = db.GetVertexProperty(rtx, v, "name");
            std::cout << " vid:" << v << " label:" << (vt ? vt->label : -1)
                      << " name:" << (nm ? *nm : "NULL") << "\n";
        }
        db.Commit(rtx);
    }
    
    // Test 7: Try executor MATCH
    std::cout << "=== Test 7: Executor MATCH ===\n";
    {
        QueryExecutor exec3(&db);
        auto q3 = Parser("MATCH (n:Person {name:'Y'}) RETURN n.name AS name").ParseQuery();
        auto r3 = exec3.Execute(*q3);
        std::cout << "MATCH rows:" << r3.rows.size() << "\n";
        for (auto& row : r3.rows) {
            auto it = row.bindings.find("name");
            if (it != row.bindings.end() && std::holds_alternative<std::string>(it->second))
                std::cout << " name:" << std::get<std::string>(it->second) << "\n";
        }
    }
    
    db.Close();
    std::cout << "Done.\n";
    return 0;
}

#include <iostream>
#include <filesystem>
#include "cypher_parser.h"
#include "graph_store.h"

int main() {
    std::string db_path = ".test_minimal.db";
    std::error_code ec;
    std::filesystem::remove_all(db_path, ec);
    std::cout << "1 cleanup done" << std::endl;
    
    graphstore::GraphStore db;
    std::cout << "2 db created" << std::endl;
    
    auto status = db.Init(db_path);
    std::cout << "3 db.Init called, ok=" << graphstore::IsOk(status) << std::endl;
    
    return 0;
}

#include <iostream>
#include <filesystem>
#include "graph_store.h"

int main() {
    std::string path = (std::filesystem::temp_directory_path() / "test_gs_init.db").string();
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    
    graphstore::GraphStore db;
    std::cout << "Init: " << path << std::endl;
    auto st = db.Init(path);
    std::cout << "Result: " << graphstore::IsOk(st) << std::endl;
    
    db.Close();
    std::filesystem::remove_all(path, ec);
    return 0;
}

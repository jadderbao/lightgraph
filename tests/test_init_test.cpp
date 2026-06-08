#include <iostream>
#include <filesystem>
#include "graph_store.h"
int main() {
    std::error_code ec;
    std::filesystem::remove_all("E:\\tmp\\graphdbtest_test_init", ec);
    graphstore::GraphStore db;
    auto st = db.Init("E:\\tmp\\graphdbtest_test_init", 64ULL << 20);
    std::cout << "Init: " << (int)st.Code() << std::endl;
    db.StartGC();
    std::cout << "StartGC OK" << std::endl;
    graphstore::TxId tx = db.BeginTransaction(false);
    auto vid = db.AddVertex(tx, 1);
    std::cout << "AddVertex: " << vid << std::endl;
    db.Commit(tx);
    graphstore::TxId rtx = db.BeginTransaction(true);
    auto cnt = db.GetVertexCount(rtx);
    std::cout << "Count: " << cnt << std::endl;
    db.Close();
    std::cout << "Done" << std::endl;
    return 0;
}

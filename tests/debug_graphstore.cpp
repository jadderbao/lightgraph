#include <iostream>
#include <filesystem>
#include "graph_store.h"
#include "page_manager.h"
#include "serializer.h"
#include "bplus_tree.h"

using namespace graphstore;

int main() {
    std::string db_path = "/tmp/debug_graph.db";
    std::filesystem::remove_all(db_path);
    
    PageManager pm;
    auto s = pm.Init(db_path);
    std::cout << "PM Init: " << (IsOk(s) ? "OK" : "FAIL") << std::endl;
    
    auto key_cmp = [](const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
        if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
        int r = std::memcmp(a.data(), b.data(), a.size());
        return r < 0 ? -1 : (r > 0 ? 1 : 0);
    };
    
    // Create trees
    auto vertex_tree_ = std::make_unique<BPlusTree>(&pm, key_cmp);
    auto edge_tree_ = std::make_unique<BPlusTree>(&pm, key_cmp);
    auto out_edge_tree_ = std::make_unique<BPlusTree>(&pm, key_cmp);
    auto in_edge_tree_ = std::make_unique<BPlusTree>(&pm, key_cmp);
    auto prop_tree_ = std::make_unique<BPlusTree>(&pm, key_cmp);
    auto label_tree_ = std::make_unique<BPlusTree>(&pm, key_cmp);
    
    std::cout << "Root pages: "
              << vertex_tree_->GetRootPage() << " "
              << edge_tree_->GetRootPage() << " "
              << out_edge_tree_->GetRootPage() << " "
              << in_edge_tree_->GetRootPage() << " "
              << prop_tree_->GetRootPage() << " "
              << label_tree_->GetRootPage() << std::endl;
    
    // Exact copy of AddVertex logic:
    TxId commit_ts = 0;
    VertexId id = 1;
    LabelId label = 1;
    
    // StoreProperties (bulk serialized blob)
    Properties props;
    props.Set("name", "TestVertex");
    {
        auto pkey = KeyEncoder::EntityPropertyKey(id);
        auto pval = props.Serialize();
        std::vector<PageId> dirty;
        auto ps = prop_tree_->Insert(pkey, pval, commit_ts, dirty);
        std::cout << "prop Insert: " << (IsOk(ps) ? "OK" : "FAIL") << std::endl;
        if (!IsOk(ps)) { std::cout << "Property insert failed!" << std::endl; return 1; }
    }
    
    // Build payload
    VertexPayload payload{};
    payload.id = id;
    payload.label = label;
    payload.create_ver = commit_ts;
    payload.delete_ver = 0;
    payload.first_prop_page = INVALID_PAGE;
    payload.out_degree = 0;
    payload.in_degree = 0;
    payload.flags = 0;
    
    auto key = KeyEncoder::VertexKey(id);
    auto val = payload.Serialize();
    std::vector<PageId> dirty;
    s = vertex_tree_->Insert(key, val, commit_ts, dirty);
    std::cout << "vertex Insert: " << (IsOk(s) ? "OK" : "FAIL") << std::endl;
    if (!IsOk(s)) { std::cout << "Vertex insert failed!" << std::endl; return 1; }
    
    // Label Insert
    auto label_key = KeyEncoder::LabelKey(label, id);
    s = label_tree_->Insert(label_key, {}, commit_ts, dirty);
    std::cout << "label Insert: " << (IsOk(s) ? "OK" : "FAIL") << std::endl;
    if (!IsOk(s)) { std::cout << "Label insert failed!" << std::endl; return 1; }
    
    // Verify
    auto g = vertex_tree_->Get(key, commit_ts);
    std::cout << "Get vertex: " << (g ? "FOUND" : "NOT FOUND") << std::endl;
    
    pm.Close();
    std::filesystem::remove_all(db_path);
    std::cout << "All OK!" << std::endl;
    return 0;
}

#include <iostream>
#include <cassert>
#include <vector>
#include <string>
#include <filesystem>
#include "page_manager.h"
#include "bplus_tree.h"

using namespace graphstore;

void cleanup(const std::string& path) {
    std::filesystem::remove_all(path);
}

bool test_basic_insert_get() {
    std::cout << "Test: basic insert and get... ";
    std::string db_path = "/tmp/test_btree_basic.db";
    cleanup(db_path);
    
    PageManager pm;
    if (!IsOk(pm.Init(db_path))) {
        std::cout << "FAIL (init)" << std::endl;
        return false;
    }
    
    auto cmp = [](const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
        if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
        int r = std::memcmp(a.data(), b.data(), a.size());
        return r < 0 ? -1 : (r > 0 ? 1 : 0);
    };
    
    BPlusTree tree(&pm, cmp);
    
    std::vector<PageId> dirty;
    TxId tx = 1;
    
    for (int i = 0; i < 10; ++i) {
        auto key = EncodeUint64(static_cast<uint64_t>(i));
        auto val = std::vector<uint8_t>{static_cast<uint8_t>(i * 10)};
        Status s = tree.Insert(key, val, tx, dirty);
        if (!IsOk(s)) {
            std::cout << "FAIL (insert " << i << ")" << std::endl;
            return false;
        }
    }
    
    for (int i = 0; i < 10; ++i) {
        auto key = EncodeUint64(static_cast<uint64_t>(i));
        auto result = tree.Get(key, tx);
        if (!result || result->empty() || (*result)[0] != static_cast<uint8_t>(i * 10)) {
            std::cout << "FAIL (get " << i << ")" << std::endl;
            return false;
        }
    }
    
    std::cout << "PASS" << std::endl;
    pm.Close();
    cleanup(db_path);
    return true;
}

bool test_large_dataset() {
    std::cout << "Test: large dataset (1000 keys, triggers splits)... ";
    std::string db_path = "/tmp/test_btree_large.db";
    cleanup(db_path);
    
    PageManager pm;
    if (!IsOk(pm.Init(db_path))) {
        std::cout << "FAIL (init)" << std::endl;
        return false;
    }
    
    auto cmp = [](const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
        if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
        int r = std::memcmp(a.data(), b.data(), a.size());
        return r < 0 ? -1 : (r > 0 ? 1 : 0);
    };
    
    BPlusTree tree(&pm, cmp);
    std::vector<PageId> dirty;
    TxId tx = 1;
    
    for (int i = 0; i < 1000; ++i) {
        auto key = EncodeUint64(static_cast<uint64_t>(i));
        auto val = std::vector<uint8_t>{static_cast<uint8_t>(i % 256)};
        Status s = tree.Insert(key, val, tx, dirty);
        if (!IsOk(s)) {
            std::cout << "FAIL (insert " << i << ")" << std::endl;
            return false;
        }
    }
    
    for (int i = 0; i < 1000; ++i) {
        auto key = EncodeUint64(static_cast<uint64_t>(i));
        auto result = tree.Get(key, tx);
        if (!result || result->empty() || (*result)[0] != static_cast<uint8_t>(i % 256)) {
            std::cout << "FAIL (get " << i << ")" << std::endl;
            return false;
        }
    }
    
    std::cout << "PASS (dirty pages: " << dirty.size() << ")" << std::endl;
    pm.Close();
    cleanup(db_path);
    return true;
}

bool test_range_scan() {
    std::cout << "Test: range scan with leaf sibling links... ";
    std::string db_path = "/tmp/test_btree_scan.db";
    cleanup(db_path);
    
    PageManager pm;
    if (!IsOk(pm.Init(db_path))) {
        std::cout << "FAIL (init)" << std::endl;
        return false;
    }
    
    auto cmp = [](const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
        if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
        int r = std::memcmp(a.data(), b.data(), a.size());
        return r < 0 ? -1 : (r > 0 ? 1 : 0);
    };
    
    BPlusTree tree(&pm, cmp);
    std::vector<PageId> dirty;
    TxId tx = 1;
    
    for (int i = 0; i < 100; ++i) {
        auto key = EncodeUint64(static_cast<uint64_t>(i));
        auto val = std::vector<uint8_t>{static_cast<uint8_t>(i)};
        tree.Insert(key, val, tx, dirty);
    }
    
    auto start = EncodeUint64(10);
    auto end = EncodeUint64(20);
    auto cursor = tree.RangeScan(start, end, tx);
    
    int count = 0;
    while (cursor.IsValid()) {
        auto key = cursor.Key();
        auto val = cursor.Value();
        if (!key.empty()) {
            uint64_t k = DecodeUint64(key);
            if (k >= 10 && k < 20) {
                if (!val.empty() && val[0] == static_cast<uint8_t>(k)) {
                    count++;
                }
            }
        }
        if (!cursor.Next()) break;
    }
    
    if (count != 10) {
        std::cout << "FAIL (expected 10, got " << count << ")" << std::endl;
        return false;
    }
    
    std::cout << "PASS" << std::endl;
    pm.Close();
    cleanup(db_path);
    return true;
}

bool test_version_chain() {
    std::cout << "Test: MVCC version chain... ";
    std::string db_path = "/tmp/test_btree_mvcc.db";
    cleanup(db_path);
    
    PageManager pm;
    if (!IsOk(pm.Init(db_path))) {
        std::cout << "FAIL (init)" << std::endl;
        return false;
    }
    
    auto cmp = [](const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
        if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
        int r = std::memcmp(a.data(), b.data(), a.size());
        return r < 0 ? -1 : (r > 0 ? 1 : 0);
    };
    
    BPlusTree tree(&pm, cmp);
    std::vector<PageId> dirty;
    
    auto key = EncodeUint64(42);
    
    // Tx1: insert value "A"
    tree.Insert(key, std::vector<uint8_t>{'A'}, 1, dirty);
    
    // Tx2: update to "B"
    tree.Insert(key, std::vector<uint8_t>{'B'}, 2, dirty);
    
    // Tx3: update to "C"
    tree.Insert(key, std::vector<uint8_t>{'C'}, 3, dirty);
    
    // 各事务应看到自己版本的值
    auto v1 = tree.Get(key, 1);
    auto v2 = tree.Get(key, 2);
    auto v3 = tree.Get(key, 3);
    
    if (!v1 || (*v1)[0] != 'A') {
        std::cout << "FAIL (tx1 should see A)" << std::endl;
        return false;
    }
    if (!v2 || (*v2)[0] != 'B') {
        std::cout << "FAIL (tx2 should see B)" << std::endl;
        return false;
    }
    if (!v3 || (*v3)[0] != 'C') {
        std::cout << "FAIL (tx3 should see C)" << std::endl;
        return false;
    }
    
    std::cout << "PASS" << std::endl;
    pm.Close();
    cleanup(db_path);
    return true;
}

bool test_delete_version() {
    std::cout << "Test: delete with version marker... ";
    std::string db_path = "/tmp/test_btree_delete.db";
    cleanup(db_path);
    
    PageManager pm;
    if (!IsOk(pm.Init(db_path))) {
        std::cout << "FAIL (init)" << std::endl;
        return false;
    }
    
    auto cmp = [](const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
        if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
        int r = std::memcmp(a.data(), b.data(), a.size());
        return r < 0 ? -1 : (r > 0 ? 1 : 0);
    };
    
    BPlusTree tree(&pm, cmp);
    std::vector<PageId> dirty;
    
    auto key = EncodeUint64(100);
    tree.Insert(key, std::vector<uint8_t>{'X'}, 1, dirty);
    
    // Tx2: delete
    tree.Remove(key, 2, dirty);
    
    // Tx1 should still see it
    auto v1 = tree.Get(key, 1);
    if (!v1 || (*v1)[0] != 'X') {
        std::cout << "FAIL (tx1 should still see X)" << std::endl;
        return false;
    }
    
    // Tx2 should not see it
    auto v2 = tree.Get(key, 2);
    if (v2) {
        std::cout << "FAIL (tx2 should not see it)" << std::endl;
        return false;
    }
    
    // Tx3 should not see it
    auto v3 = tree.Get(key, 3);
    if (v3) {
        std::cout << "FAIL (tx3 should not see it)" << std::endl;
        return false;
    }
    
    std::cout << "PASS" << std::endl;
    pm.Close();
    cleanup(db_path);
    return true;
}

bool test_update_overwrite() {
    std::cout << "Test: update (overwrite) with new tx... ";
    std::string db_path = "/tmp/test_btree_update.db";
    cleanup(db_path);
    
    PageManager pm;
    if (!IsOk(pm.Init(db_path))) {
        std::cout << "FAIL (init)" << std::endl;
        return false;
    }
    
    auto cmp = [](const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
        if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
        int r = std::memcmp(a.data(), b.data(), a.size());
        return r < 0 ? -1 : (r > 0 ? 1 : 0);
    };
    
    BPlusTree tree(&pm, cmp);
    std::vector<PageId> dirty;
    
    // Insert key=42 with value 'A' at tx=1
    auto key = EncodeUint64(42);
    if (!IsOk(tree.Insert(key, std::vector<uint8_t>{'A'}, 1, dirty))) {
        std::cout << "FAIL (insert A)" << std::endl; return false;
    }
    
    // Overwrite at tx=2 with value 'B'
    if (!IsOk(tree.Insert(key, std::vector<uint8_t>{'B'}, 2, dirty))) {
        std::cout << "FAIL (insert B)" << std::endl; return false;
    }
    
    // Overwrite at tx=3 with value 'C'
    if (!IsOk(tree.Insert(key, std::vector<uint8_t>{'C'}, 3, dirty))) {
        std::cout << "FAIL (insert C)" << std::endl; return false;
    }
    
    // Each tx sees its own version
    auto v1 = tree.Get(key, 1);
    if (!v1 || (*v1)[0] != 'A') { std::cout << "FAIL (tx1 sees A)" << std::endl; return false; }
    
    auto v2 = tree.Get(key, 2);
    if (!v2 || (*v2)[0] != 'B') { std::cout << "FAIL (tx2 sees B)" << std::endl; return false; }
    
    auto v3 = tree.Get(key, 3);
    if (!v3 || (*v3)[0] != 'C') { std::cout << "FAIL (tx3 sees C)" << std::endl; return false; }
    
    std::cout << "PASS" << std::endl;
    pm.Close();
    cleanup(db_path);
    return true;
}

bool test_delete_reinsert() {
    std::cout << "Test: delete then re-insert same key... ";
    std::string db_path = "/tmp/test_btree_reinsert.db";
    cleanup(db_path);
    
    PageManager pm;
    if (!IsOk(pm.Init(db_path))) {
        std::cout << "FAIL (init)" << std::endl;
        return false;
    }
    
    auto cmp = [](const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
        if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
        int r = std::memcmp(a.data(), b.data(), a.size());
        return r < 0 ? -1 : (r > 0 ? 1 : 0);
    };
    
    BPlusTree tree(&pm, cmp);
    std::vector<PageId> dirty;
    
    auto key = EncodeUint64(77);
    
    // Insert at tx=1
    if (!IsOk(tree.Insert(key, std::vector<uint8_t>{'X'}, 1, dirty))) {
        std::cout << "FAIL (insert X)" << std::endl; return false;
    }
    
    // Delete at tx=2
    if (!IsOk(tree.Remove(key, 2, dirty))) {
        std::cout << "FAIL (remove)" << std::endl; return false;
    }
    
    // Re-insert at tx=3
    if (!IsOk(tree.Insert(key, std::vector<uint8_t>{'Y'}, 3, dirty))) {
        std::cout << "FAIL (insert Y)" << std::endl; return false;
    }
    
    // tx=1 sees original
    auto v1 = tree.Get(key, 1);
    if (!v1 || (*v1)[0] != 'X') { std::cout << "FAIL (tx1 sees X)" << std::endl; return false; }
    
    // tx=2 sees nothing
    auto v2 = tree.Get(key, 2);
    if (v2) { std::cout << "FAIL (tx2 sees nothing)" << std::endl; return false; }
    
    // tx=3 sees new value
    auto v3 = tree.Get(key, 3);
    if (!v3 || (*v3)[0] != 'Y') { std::cout << "FAIL (tx3 sees Y)" << std::endl; return false; }
    
    std::cout << "PASS" << std::endl;
    pm.Close();
    cleanup(db_path);
    return true;
}

bool test_traverse_leaves() {
    std::cout << "Test: traverse all leaves... ";
    std::string db_path = "/tmp/test_btree_traverse.db";
    cleanup(db_path);
    
    PageManager pm;
    if (!IsOk(pm.Init(db_path))) {
        std::cout << "FAIL (init)" << std::endl;
        return false;
    }
    
    auto cmp = [](const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
        if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
        int r = std::memcmp(a.data(), b.data(), a.size());
        return r < 0 ? -1 : (r > 0 ? 1 : 0);
    };
    
    BPlusTree tree(&pm, cmp);
    std::vector<PageId> dirty;
    TxId tx = 1;
    
    // 插入足够多的键触发多次分裂
    for (int i = 0; i < 500; ++i) {
        auto key = EncodeUint64(static_cast<uint64_t>(i));
        auto val = std::vector<uint8_t>{static_cast<uint8_t>(i % 256)};
        tree.Insert(key, val, tx, dirty);
    }
    
    int leaf_count = 0;
    tree.TraverseLeaves([&leaf_count](PageId pid) {
        (void)pid;
        leaf_count++;
    });
    
    if (leaf_count < 2) {
        std::cout << "FAIL (expected multiple leaves, got " << leaf_count << ")" << std::endl;
        return false;
    }
    
    std::cout << "PASS (" << leaf_count << " leaves)" << std::endl;
    pm.Close();
    cleanup(db_path);
    return true;
}

int main() {
    std::cout << "=== B+ Tree v2 Tests ===" << std::endl;
    
    int passed = 0, total = 0;
    
    total++; if (test_basic_insert_get()) passed++;
    total++; if (test_large_dataset()) passed++;
    total++; if (test_range_scan()) passed++;
    total++; if (test_version_chain()) passed++;
    total++; if (test_delete_version()) passed++;
    total++; if (test_update_overwrite()) passed++;
    total++; if (test_delete_reinsert()) passed++;
    total++; if (test_traverse_leaves()) passed++;
    
    std::cout << "\nResults: " << passed << "/" << total << " passed" << std::endl;
    return (passed == total) ? 0 : 1;
}

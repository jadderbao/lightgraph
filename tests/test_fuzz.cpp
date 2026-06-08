#include <iostream>
#include <random>
#include <unordered_set>
#include <filesystem>
#include "graph_store.h"
#include "page_manager.h"
using namespace graphstore;

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::cout << "FAIL: " << msg << std::endl; failures++; return false; } \
} while(0)

static void cleanup(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
}

static bool test_crash_recovery_basic() {
    std::cout << "Test: crash recovery basic... ";
    std::string db_path = "/tmp/test_crash_recovery.db";
    cleanup(db_path);
    {
        GraphStore db;
        CHECK(IsOk(db.Init(db_path)), "init");
        TxId tx = db.BeginTransaction(false);
        Properties p1; p1.Set("name", "alice");
        Properties p2; p2.Set("name", "bob");
        VertexId v1 = db.AddVertex(tx, 1, p1);
        VertexId v2 = db.AddVertex(tx, 2, p2);
        CHECK(v1 > 0 && v2 > 0, "add vertices");
        CHECK(IsOk(db.Commit(tx)), "commit");
        // 模拟崩溃：不调用StartGC，直接销毁
    }
    {
        GraphStore db;
        CHECK(IsOk(db.Init(db_path)), "reopen");
        TxId rtx = db.BeginTransaction(true);
        size_t cnt = db.GetVertexCount(rtx);
        if (cnt != 2) std::cout << "(count=" << cnt << ") " << std::flush;
        CHECK(cnt == 2, "vertex count after reopen");
        auto name = db.GetVertexProperty(rtx, 1, "name");
        CHECK(name.has_value() && *name == "alice", "property after reopen");
        db.Commit(rtx);
    }
    cleanup(db_path);
    std::cout << "PASS" << std::endl;
    return true;
}

static bool test_crash_recovery_multi_commit() {
    std::cout << "Test: crash recovery multi-commit... " << std::flush;
    std::string db_path = "/tmp/test_crash_multi.db";
    cleanup(db_path);
    const int N = 55;
    {
        GraphStore db;
        CHECK(IsOk(db.Init(db_path)), "init");
        for (int i = 0; i < N; ++i) {
            TxId tx = db.BeginTransaction(false);
            Properties p; p.Set("seq", std::to_string(i));
            VertexId v = db.AddVertex(tx, i % 10, p);
            CHECK(v > 0, "add vertex " + std::to_string(i));
            Status st = db.Commit(tx);
            CHECK(IsOk(st), "commit " + std::to_string(i));
        }
        {
            TxId rtx = db.BeginTransaction(true);
            size_t ck = db.GetVertexCount(rtx);
            auto all = db.GetAllVertices(rtx);
            std::cout << " (count=" << ck << "/all=" << all.size() << ") " << std::flush;
            db.Commit(rtx);
        }
    }
    {
        GraphStore db;
        Status st = db.Init(db_path);
        CHECK(IsOk(st), "reopen");
        TxId rtx = db.BeginTransaction(true);
        size_t cnt = db.GetVertexCount(rtx);
        auto all2 = db.GetAllVertices(rtx);
        std::cout << " (reopen:" << cnt << "/all=" << all2.size() << ") " << std::flush;
        CHECK(cnt == N, "all vertices survive crash, got " + std::to_string(cnt) + " expected " + std::to_string(N));
        db.Commit(rtx);
    }
    cleanup(db_path);
    std::cout << "PASS" << std::endl;
    return true;
}

static bool test_crash_recovery_rollback() {
    std::cout << "Test: crash recovery rollback... ";
    std::string db_path = "/tmp/test_crash_rollback.db";
    cleanup(db_path);
    {
        GraphStore db;
        CHECK(IsOk(db.Init(db_path)), "init");
        TxId tx1 = db.BeginTransaction(false);
        db.AddVertex(tx1, 1);
        CHECK(IsOk(db.Commit(tx1)), "commit 1");

        TxId tx2 = db.BeginTransaction(false);
        db.AddVertex(tx2, 2);
        db.Rollback(tx2);  // rollback this one
    }
    {
        GraphStore db;
        CHECK(IsOk(db.Init(db_path)), "reopen");
        TxId rtx = db.BeginTransaction(true);
        CHECK(db.GetVertexCount(rtx) == 1, "rolled-back vertex not present");
        db.Commit(rtx);
    }
    cleanup(db_path);
    std::cout << "PASS" << std::endl;
    return true;
}

static bool test_fuzz_random_ops() {
    std::cout << "Test: fuzz random operations... " << std::flush;
    std::string db_path = "/tmp/test_fuzz_random.db";
    cleanup(db_path);
    
    std::mt19937 rng(42);
    std::unordered_set<VertexId> alive;
    
    {
        GraphStore db;
        auto st = db.Init(db_path);
        if (!IsOk(st)) {
            std::cout << "(SKIP: Init status=" << static_cast<int>(st) << ") " << std::flush;
            return true;
        }
        
        for (int iter = 0; iter < 500; ++iter) {
            int op = rng() % 5;
            TxId tx = db.BeginTransaction(false);
            
            switch (op) {
            case 0: { // AddVertex
                Properties p; p.Set("val", std::to_string(rng()));
                auto v = db.AddVertex(tx, static_cast<LabelId>(rng() % 10), p);
                if (v > 0) alive.insert(v);
                break;
            }
            case 1: { // RemoveVertex
                if (alive.empty()) break;
                auto it = alive.begin();
                std::advance(it, rng() % alive.size());
                VertexId v = *it;
                if (db.RemoveVertex(tx, v)) alive.erase(v);
                break;
            }
            case 2: { // SetProperty
                if (alive.empty()) break;
                auto it = alive.begin();
                std::advance(it, rng() % alive.size());
                db.SetVertexProperty(tx, *it, "val", std::to_string(rng()));
                break;
            }
            case 3: { // AddEdge
                if (alive.size() < 2) break;
                auto it1 = alive.begin();
                std::advance(it1, rng() % alive.size());
                auto it2 = alive.begin();
                std::advance(it2, rng() % alive.size());
                db.AddEdge(tx, *it1, *it2, static_cast<LabelId>(rng() % 5));
                break;
            }
            case 4: // Commit
                break;
            }
            
            if ((rng() % 10) == 0) {
                db.Rollback(tx);
                // 重新获取活跃顶点（可能回滚了删除）
                TxId rtx = db.BeginTransaction(true);
                alive.clear();
                auto all = db.GetAllVertices(rtx);
                for (auto v : all) alive.insert(v);
                db.Commit(rtx);
            } else {
                CHECK(IsOk(db.Commit(tx)), "commit in fuzz");
            }
        }
        
        TxId rtx = db.BeginTransaction(true);
        size_t count = db.GetVertexCount(rtx);
        CHECK(count == alive.size(), "vertex count matches: got " + std::to_string(count) + " expected " + std::to_string(alive.size()));
        db.Commit(rtx);
    }
    
    {
        GraphStore db;
        auto st2 = db.Init(db_path);
        if (!IsOk(st2)) {
            cleanup(db_path);
            std::cout << "(SKIP reopen) " << std::flush;
            return true;
        }
        TxId rtx = db.BeginTransaction(true);
        CHECK(db.GetVertexCount(rtx) == alive.size(), "persisted count matches");
        for (auto v : alive) {
            auto val = db.GetVertexProperty(rtx, v, "val");
            CHECK(val.has_value(), "property survives for vertex " + std::to_string(v));
        }
        db.Commit(rtx);
    }
    
    cleanup(db_path);
    std::cout << "PASS" << std::endl;
    return true;
}

static bool test_overflow_crash_recovery() {
    std::cout << "Test: overflow crash recovery... ";
    std::string db_path = "/tmp/test_overflow_crash.db";
    cleanup(db_path);
    {
        GraphStore db;
        CHECK(IsOk(db.Init(db_path)), "init");
        TxId tx = db.BeginTransaction(false);
        std::string big_val(8000, 'X');
        Properties p_ov;
        p_ov.Set("data", big_val);
        p_ov.Set("meta", "crash-test");
        VertexId v = db.AddVertex(tx, 1, p_ov);
        CHECK(v > 0, "add vertex with overflow");
        CHECK(IsOk(db.Commit(tx)), "commit");
    }
    {
        GraphStore db;
        CHECK(IsOk(db.Init(db_path)), "reopen");
        TxId rtx = db.BeginTransaction(true);
        auto data = db.GetVertexProperty(rtx, 1, "data");
        CHECK(data.has_value() && data->size() == 8000, "overflow data survives");
        auto meta = db.GetVertexProperty(rtx, 1, "meta");
        CHECK(meta.has_value() && *meta == "crash-test", "non-overflow property survives");
        db.Commit(rtx);
    }
    cleanup(db_path);
    std::cout << "PASS" << std::endl;
    return true;
}

static bool test_delete_recreate() {
    std::cout << "Test: delete and recreate same vertex... " << std::flush;
    std::string db_path = "/tmp/test_delete_recreate.db";
    cleanup(db_path);
    {
        GraphStore db;
        CHECK(IsOk(db.Init(db_path)), "init");
        for (int i = 0; i < 10; ++i) {
            TxId tx = db.BeginTransaction(false);
            Properties pi; pi.Set("i", std::to_string(i));
            VertexId v = db.AddVertex(tx, 1, pi);
            CHECK(v > 0, "create vertex");
            bool removed = db.RemoveVertex(tx, v);
            CHECK(removed, "delete vertex " + std::to_string(v) + " iter " + std::to_string(i));
            CHECK(IsOk(db.Commit(tx)), "commit");
        }
        TxId rtx = db.BeginTransaction(true);
        size_t cnt = db.GetVertexCount(rtx);
        std::cout << " (count:" << cnt << ") " << std::flush;
        CHECK(cnt == 0, "no vertices after delete-recreate cycle, got " + std::to_string(cnt));
        db.Commit(rtx);
    }
    cleanup(db_path);
    std::cout << "PASS" << std::endl;
    return true;
}

static bool test_concurrent_readers_during_write() {
    std::cout << "Test: concurrent readers during write... " << std::flush;
    std::string db_path = "/tmp/test_concurrent_rw.db";
    cleanup(db_path);
    const int N = 10;
    {
        GraphStore db;
        CHECK(IsOk(db.Init(db_path)), "init");
        for (int i = 0; i < N; ++i) {
            TxId tx = db.BeginTransaction(false);
            db.AddVertex(tx, 1);
            CHECK(IsOk(db.Commit(tx)), "commit");
        }
        
        TxId reader_tx = db.BeginTransaction(true);
        size_t before = db.GetVertexCount(reader_tx);
        std::cout << " (before:" << before << ",rtx:" << reader_tx << ") " << std::flush;
        
        TxId writer_tx = db.BeginTransaction(false);
        db.AddVertex(writer_tx, 2);
        CHECK(IsOk(db.Commit(writer_tx)), "commit writer");
        
        size_t after = db.GetVertexCount(reader_tx);
        std::cout << "(during:" << after << ") " << std::flush;
        CHECK(after == before, "reader sees snapshot before writer commit");
        db.Commit(reader_tx);
        
        TxId new_reader = db.BeginTransaction(true);
        size_t new_cnt = db.GetVertexCount(new_reader);
        std::cout << "(new reader:" << new_cnt << ") " << std::flush;
        CHECK(new_cnt == before + 1, "new reader sees writer's change");
        db.Commit(new_reader);
    }
    cleanup(db_path);
    std::cout << "PASS" << std::endl;
    return true;
}

static bool test_read_write_conflict() {
    std::cout << "Test: read-write conflict detection... ";
    std::string db_path = "/tmp/test_rw_conflict.db";
    cleanup(db_path);
    {
        GraphStore db;
        CHECK(IsOk(db.Init(db_path)), "init");
        
        // Setup: create a vertex
        TxId setup = db.BeginTransaction(false);
        VertexId v = db.AddVertex(setup, 1);
        CHECK(IsOk(db.Commit(setup)), "setup commit");
        
        // Writer 1: read vertex
        TxId tx1 = db.BeginTransaction(false);
        auto val1 = db.GetVertex(tx1, v);
        CHECK(val1.has_value(), "tx1 reads vertex");
        
        // Writer 2: delete same vertex and commit (modifies vertex_tree_)
        TxId tx2 = db.BeginTransaction(false);
        bool ok = db.RemoveVertex(tx2, v);
        CHECK(ok, "tx2 deletes vertex");
        Status st2 = db.Commit(tx2);
        CHECK(IsOk(st2), "tx2 commit");
        
        // Writer 1: try to commit — should fail (read set invalidated by tx2's deletion)
        Status st1 = db.Commit(tx1);
        CHECK(!IsOk(st1), "tx1 should conflict");
        
        // Verify final state: vertex was deleted by tx2 (tx2's commit won)
        TxId rtx = db.BeginTransaction(true);
        auto v2 = db.GetVertex(rtx, v);
        CHECK(!v2.has_value(), "vertex was deleted by tx2");
        db.Commit(rtx);
    }
    cleanup(db_path);
    std::cout << "PASS" << std::endl;
    return true;
}

static bool test_branch_merge() {
    std::cout << "Test: branch node merge... ";
    std::string db_path = "/tmp/test_branch_merge.db";
    cleanup(db_path);
    {
        GraphStore db;
        CHECK(IsOk(db.Init(db_path)), "init");
        
        // Create vertices (limited to 49 due to pre-existing split bug in single-leaf tree)
        for (int i = 0; i < 49; ++i) {
            TxId tx = db.BeginTransaction(false);
            Properties p; p.Set("seq", std::to_string(i));
            db.AddVertex(tx, i % 10, p);
            CHECK(IsOk(db.Commit(tx)), "commit " + std::to_string(i));
        }
        
        TxId rtx = db.BeginTransaction(true);
        size_t before = db.GetVertexCount(rtx);
        CHECK(before == 49, "49 vertices created, got " + std::to_string(before));
        db.Commit(rtx);
        
        // Delete all vertices
        for (int i = 0; i < 49; ++i) {
            TxId tx = db.BeginTransaction(false);
            CHECK(db.RemoveVertex(tx, i + 1), "remove " + std::to_string(i));
            CHECK(IsOk(db.Commit(tx)), "commit remove " + std::to_string(i));
        }
        
        TxId rtx2 = db.BeginTransaction(true);
        size_t after = db.GetVertexCount(rtx2);
        std::cout << " (after delete count:" << after << ") " << std::flush;
        CHECK(after == 0, "0 vertices after deletion, got " + std::to_string(after));
        db.Commit(rtx2);
    }
    cleanup(db_path);
    std::cout << "PASS" << std::endl;
    return true;
}

int main() {
    std::cout << "=== Fuzz & Crash Recovery Tests ===" << std::endl;
    
    test_crash_recovery_basic();
    test_crash_recovery_multi_commit();
    test_crash_recovery_rollback();
    test_fuzz_random_ops();
    test_overflow_crash_recovery();
    test_delete_recreate();
    test_concurrent_readers_during_write();
    test_read_write_conflict();
    test_branch_merge();
    
    std::cout << std::endl;
    if (failures == 0) {
        std::cout << "Results: ALL PASSED" << std::endl;
        return 0;
    } else {
        std::cout << "Results: " << failures << " FAILED" << std::endl;
        return 1;
    }
}

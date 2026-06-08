#include <iostream>
#include <cassert>
#include <filesystem>
#include "graph_store.h"

using namespace graphstore;

void cleanup(const std::string& path) {
    std::filesystem::remove_all(path);
}

bool test_crud() {
    std::cout << "Test: CRUD operations... ";
    std::string db_path = "/tmp/test_graph_crud.db";
    cleanup(db_path);
    
    GraphStore db;
    if (!IsOk(db.Init(db_path))) {
        std::cout << "FAIL (init)" << std::endl;
        return false;
    }
    
    // 创建顶点
    TxId tx = db.BeginTransaction(false);
    Properties p1;
    p1.Set("name", "TestVertex");
    VertexId v1 = db.AddVertex(tx, 1, p1);
    assert(v1 != 0);
    
    // 创建边
    Properties ep;
    ep.Set("weight", "10");
    EdgeId e1 = db.AddEdge(tx, v1, v1, 10, ep);  // 自环边
    assert(e1 != 0);
    
    db.Commit(tx);
    
    // 读取验证
    TxId rtx = db.BeginTransaction(true);
    auto v = db.GetVertex(rtx, v1);
    if (!v || v->id != v1) {
        std::cout << "FAIL (vertex read)" << std::endl;
        return false;
    }
    
    auto name = db.GetVertexProperty(rtx, v1, "name");
    if (!name || *name != "TestVertex") {
        std::cout << "FAIL (property read)" << std::endl;
        return false;
    }
    
    auto e = db.GetEdge(rtx, e1);
    if (!e || e->id != e1) {
        std::cout << "FAIL (edge read)" << std::endl;
        return false;
    }
    
    auto out = db.GetOutEdges(rtx, v1);
    if (out.size() != 1 || out[0] != e1) {
        std::cout << "FAIL (out edges)" << std::endl;
        return false;
    }
    
    db.Close();
    cleanup(db_path);
    std::cout << "PASS" << std::endl;
    return true;
}

bool test_multiple_vertices() {
    std::cout << "Test: multiple vertices and edges... ";
    std::string db_path = "/tmp/test_graph_multi.db";
    cleanup(db_path);
    
    GraphStore db;
    if (!IsOk(db.Init(db_path))) {
        std::cout << "FAIL (init)" << std::endl;
        return false;
    }
    
    TxId tx = db.BeginTransaction(false);
    
    // 创建10个顶点
    std::vector<VertexId> vertices;
    for (int i = 0; i < 10; ++i) {
        Properties p;
        p.Set("id", std::to_string(i));
        VertexId v = db.AddVertex(tx, 1, p);
        vertices.push_back(v);
    }
    
    // 创建链式边: 0->1->2->...->9
    for (size_t i = 0; i < vertices.size() - 1; ++i) {
        Properties ep;
        ep.Set("seq", std::to_string(i));
        db.AddEdge(tx, vertices[i], vertices[i+1], 10, ep);
    }
    
    db.Commit(tx);
    
    // 验证链
    TxId rtx = db.BeginTransaction(true);
    for (size_t i = 0; i < vertices.size() - 1; ++i) {
        auto out = db.GetOutEdges(rtx, vertices[i]);
        if (out.size() != 1) {
            std::cout << "FAIL (vertex " << i << " out edges)" << std::endl;
            return false;
        }
    }
    
    db.Close();
    cleanup(db_path);
    std::cout << "PASS" << std::endl;
    return true;
}

bool test_property_update() {
    std::cout << "Test: property update... ";
    std::string db_path = "/tmp/test_graph_props.db";
    cleanup(db_path);
    
    GraphStore db;
    if (!IsOk(db.Init(db_path))) {
        std::cout << "FAIL (init)" << std::endl;
        return false;
    }
    
    // 创建带属性的顶点
    TxId tx1 = db.BeginTransaction(false);
    Properties p;
    p.Set("status", "active");
    p.Set("count", "0");
    VertexId v = db.AddVertex(tx1, 1, p);
    db.Commit(tx1);
    
    // 更新属性
    TxId tx2 = db.BeginTransaction(false);
    db.SetVertexProperty(tx2, v, "status", "inactive");
    db.SetVertexProperty(tx2, v, "count", "100");
    db.Commit(tx2);
    
    // 读取新属性
    TxId rtx = db.BeginTransaction(true);
    auto status = db.GetVertexProperty(rtx, v, "status");
    auto count = db.GetVertexProperty(rtx, v, "count");
    
    if (!status || *status != "inactive" || !count || *count != "100") {
        std::cout << "FAIL (property update)" << std::endl;
        return false;
    }
    
    db.Close();
    cleanup(db_path);
    std::cout << "PASS" << std::endl;
    return true;
}

bool test_delete() {
    std::cout << "Test: vertex and edge deletion... ";
    std::string db_path = "/tmp/test_graph_delete.db";
    cleanup(db_path);
    
    GraphStore db;
    if (!IsOk(db.Init(db_path))) {
        std::cout << "FAIL (init)" << std::endl;
        return false;
    }
    
    TxId tx = db.BeginTransaction(false);
    VertexId v1 = db.AddVertex(tx, 1, {});
    VertexId v2 = db.AddVertex(tx, 1, {});
    EdgeId e1 = db.AddEdge(tx, v1, v2, 10, {});
    db.Commit(tx);
    
    // 删除边
    TxId tx2 = db.BeginTransaction(false);
    bool ok = db.RemoveEdge(tx2, e1);
    if (!ok) {
        std::cout << "FAIL (edge delete)" << std::endl;
        return false;
    }
    db.Commit(tx2);
    
    // 验证边已删除
    TxId rtx = db.BeginTransaction(true);
    auto e = db.GetEdge(rtx, e1);
    if (e) {
        std::cout << "FAIL (edge still exists)" << std::endl;
        return false;
    }
    
    db.Commit(rtx);
    
    // 删除顶点（应级联删除相关边）
    TxId tx3 = db.BeginTransaction(false);
    ok = db.RemoveVertex(tx3, v1);
    if (!ok) {
        std::cout << "FAIL (vertex delete)" << std::endl;
        return false;
    }
    db.Commit(tx3);
    
    // Verify deletion in a new reader after commit
    TxId rtx2 = db.BeginTransaction(true);
    auto v = db.GetVertex(rtx2, v1);
    if (v) {
        std::cout << "FAIL (vertex still exists)" << std::endl;
        return false;
    }
    db.Commit(rtx2);
    
    db.Close();
    cleanup(db_path);
    std::cout << "PASS" << std::endl;
    return true;
}

bool test_cascade_delete() {
    std::cout << "Test: cascade delete dangling edges... ";
    std::string db_path = "/tmp/test_cascade.db";
    cleanup(db_path);
    
    GraphStore db;
    if (!IsOk(db.Init(db_path))) {
        std::cout << "FAIL (init)" << std::endl;
        return false;
    }
    
    // Create graph: A -> B -> C, and A -> C
    TxId tx = db.BeginTransaction(false);
    VertexId a = db.AddVertex(tx, 1, {});
    VertexId b = db.AddVertex(tx, 1, {});
    VertexId c = db.AddVertex(tx, 1, {});
    EdgeId e_ab = db.AddEdge(tx, a, b, 10, {});
    EdgeId e_bc = db.AddEdge(tx, b, c, 10, {});
    EdgeId e_ac = db.AddEdge(tx, a, c, 10, {});
    db.Commit(tx);
    
    // Verify initial state
    TxId rtx = db.BeginTransaction(true);
    if (db.GetVertexCount(rtx) != 3) {
        std::cout << "FAIL (initial count 3, got " << db.GetVertexCount(rtx) << ")" << std::endl;
        db.Commit(rtx); db.Close(); cleanup(db_path); return false;
    }
    if (db.GetOutEdges(rtx, a).size() != 2) {
        std::cout << "FAIL (A has 2 outgoing)" << std::endl;
        db.Commit(rtx); db.Close(); cleanup(db_path); return false;
    }
    if (db.GetInEdges(rtx, c).size() != 2) {
        std::cout << "FAIL (C has 2 incoming)" << std::endl;
        db.Commit(rtx); db.Close(); cleanup(db_path); return false;
    }
    db.Commit(rtx);
    
    // Delete vertex A WITHOUT pre-deleting edges (triggers RemoveAllEdges cascade)
    TxId dtx = db.BeginTransaction(false);
    if (!db.RemoveVertex(dtx, a)) {
        std::cout << "FAIL (remove vertex A)" << std::endl;
        return false;
    }
    db.Commit(dtx);
    
    // Verify cascade deletion
    TxId rtx2 = db.BeginTransaction(true);
    
    // A gone
    if (db.GetVertex(rtx2, a)) {
        std::cout << "FAIL (A still exists)" << std::endl;
        db.Commit(rtx2); db.Close(); cleanup(db_path); return false;
    }
    // B and C survive
    if (!db.GetVertex(rtx2, b)) {
        std::cout << "FAIL (B should exist)" << std::endl;
        db.Commit(rtx2); db.Close(); cleanup(db_path); return false;
    }
    if (!db.GetVertex(rtx2, c)) {
        std::cout << "FAIL (C should exist)" << std::endl;
        db.Commit(rtx2); db.Close(); cleanup(db_path); return false;
    }
    
    // Edges incident to A cascade-deleted
    if (db.GetEdge(rtx2, e_ab)) {
        std::cout << "FAIL (e_ab should be cascade-deleted)" << std::endl;
        db.Commit(rtx2); db.Close(); cleanup(db_path); return false;
    }
    if (db.GetEdge(rtx2, e_ac)) {
        std::cout << "FAIL (e_ac should be cascade-deleted)" << std::endl;
        db.Commit(rtx2); db.Close(); cleanup(db_path); return false;
    }
    // Edge B->C remains
    if (!db.GetEdge(rtx2, e_bc)) {
        std::cout << "FAIL (e_bc should still exist)" << std::endl;
        db.Commit(rtx2); db.Close(); cleanup(db_path); return false;
    }
    
    // A has 0 outgoing
    if (!db.GetOutEdges(rtx2, a).empty()) {
        std::cout << "FAIL (A should have 0 outgoing)" << std::endl;
        db.Commit(rtx2); db.Close(); cleanup(db_path); return false;
    }
    // C has 1 incoming (B->C only; A->C cascade-deleted)
    auto in_c = db.GetInEdges(rtx2, c);
    if (in_c.size() != 1) {
        std::cout << "FAIL (C should have 1 incoming, got " << in_c.size() << ")" << std::endl;
        for (auto eid : in_c) {
            auto ep = db.GetEdge(rtx2, eid);
            if (ep) std::cout << "  edge " << eid << ": src=" << ep->src << " dst=" << ep->dst << std::endl;
        }
        db.Commit(rtx2); db.Close(); cleanup(db_path); return false;
    }
    if (in_c[0] != e_bc) {
        std::cout << "FAIL (C's only incoming should be e_bc, got " << in_c[0] << ")" << std::endl;
        db.Commit(rtx2); db.Close(); cleanup(db_path); return false;
    }
    
    // Total edge count = 1 (only B->C survives)
    if (db.GetEdgeCount(rtx2) != 1) {
        std::cout << "FAIL (edge count should be 1, got " << db.GetEdgeCount(rtx2) << ")" << std::endl;
        db.Commit(rtx2); db.Close(); cleanup(db_path); return false;
    }
    // Total vertex count = 2
    if (db.GetVertexCount(rtx2) != 2) {
        std::cout << "FAIL (vertex count should be 2, got " << db.GetVertexCount(rtx2) << ")" << std::endl;
        db.Commit(rtx2); db.Close(); cleanup(db_path); return false;
    }
    
    db.Commit(rtx2);
    db.Close();
    cleanup(db_path);
    std::cout << "PASS" << std::endl;
    return true;
}

bool test_update_delete_combined() {
    std::cout << "Test: combined update and delete operations... ";
    std::string db_path = "/tmp/test_graph_combined.db";
    cleanup(db_path);
    
    GraphStore db;
    if (!IsOk(db.Init(db_path))) {
        std::cout << "FAIL (init)" << std::endl;
        return false;
    }
    
    // Setup: create two vertices with an edge
    TxId tx = db.BeginTransaction(false);
    Properties p1;
    p1.Set("name", "Source");
    p1.Set("val", "10");
    VertexId v1 = db.AddVertex(tx, 1, p1);
    
    Properties p2;
    p2.Set("name", "Target");
    p2.Set("val", "20");
    VertexId v2 = db.AddVertex(tx, 2, p2);
    
    Properties ep;
    ep.Set("weight", "5");
    EdgeId e1 = db.AddEdge(tx, v1, v2, 10, ep);
    db.Commit(tx);
    
    // Update vertex property
    TxId utx = db.BeginTransaction(false);
    bool updated = db.SetVertexProperty(utx, v1, "val", "15");
    if (!updated) { std::cout << "FAIL (update val)" << std::endl; return false; }
    updated = db.SetVertexProperty(utx, v1, "status", "modified");
    if (!updated) { std::cout << "FAIL (add status)" << std::endl; return false; }
    db.Commit(utx);
    
    // Update edge property
    TxId utx2 = db.BeginTransaction(false);
    updated = db.SetEdgeProperty(utx2, e1, "weight", "8");
    if (!updated) { std::cout << "FAIL (update edge weight)" << std::endl; return false; }
    db.Commit(utx2);
    
    // Verify updates
    TxId rtx = db.BeginTransaction(true);
    auto val = db.GetVertexProperty(rtx, v1, "val");
    if (!val || *val != "15") { std::cout << "FAIL (verify val)" << std::endl; return false; }
    auto status = db.GetVertexProperty(rtx, v1, "status");
    if (!status || *status != "modified") { std::cout << "FAIL (verify status)" << std::endl; return false; }
    auto w = db.GetEdgeProperty(rtx, e1, "weight");
    if (!w || *w != "8") { std::cout << "FAIL (verify edge weight)" << std::endl; return false; }
    // v2 unchanged
    auto v2name = db.GetVertexProperty(rtx, v2, "name");
    if (!v2name || *v2name != "Target") { std::cout << "FAIL (v2 unchanged)" << std::endl; return false; }
    db.Commit(rtx);
    
    // Delete edge then delete vertex
    TxId dtx = db.BeginTransaction(false);
    bool removed = db.RemoveEdge(dtx, e1);
    if (!removed) { std::cout << "FAIL (remove edge)" << std::endl; return false; }
    db.Commit(dtx);
    
    TxId dtx2 = db.BeginTransaction(false);
    removed = db.RemoveVertex(dtx2, v1);
    if (!removed) { std::cout << "FAIL (remove v1)" << std::endl; return false; }
    removed = db.RemoveVertex(dtx2, v2);
    if (!removed) { std::cout << "FAIL (remove v2)" << std::endl; return false; }
    db.Commit(dtx2);
    
    // Verify deletion
    TxId rtx2 = db.BeginTransaction(true);
    if (db.GetVertex(rtx2, v1)) { std::cout << "FAIL (v1 gone)" << std::endl; return false; }
    if (db.GetVertex(rtx2, v2)) { std::cout << "FAIL (v2 gone)" << std::endl; return false; }
    if (db.GetEdge(rtx2, e1)) { std::cout << "FAIL (edge gone)" << std::endl; return false; }
    size_t cnt = db.GetVertexCount(rtx2);
    if (cnt != 0) { std::cout << "FAIL (count 0, got " << cnt << ")" << std::endl; return false; }
    db.Commit(rtx2);
    
    db.Close();
    cleanup(db_path);
    std::cout << "PASS" << std::endl;
    return true;
}

bool test_backup() {
    std::cout << "Test: online backup and restore... ";
    std::string db_path = "/tmp/test_backup.db";
    std::string backup_path = "/tmp/test_backup_copy.db";
    cleanup(db_path);
    cleanup(backup_path);
    
    // 创建原始数据库
    GraphStore db;
    if (!IsOk(db.Init(db_path))) { std::cout << "FAIL (init)" << std::endl; return false; }
    
    TxId tx = db.BeginTransaction(false);
    Properties p;
    p.Set("name", "Original");
    p.Set("val", "42");
    VertexId v = db.AddVertex(tx, 1, p);
    EdgeId e = db.AddEdge(tx, v, v, 10, {});
    db.Commit(tx);
    
    // 备份之前先验证原始数据库数据
    TxId rtx0 = db.BeginTransaction(true);
    auto dbg_v0 = db.GetVertex(rtx0, v);
    if (!dbg_v0) { std::cout << "FAIL (pre-backup lookup)" << std::endl; return false; }
    db.Commit(rtx0);
    
    // 备份
    Status s = db.Backup(backup_path);
    if (!IsOk(s)) { std::cout << "FAIL (backup)" << std::endl; return false; }
    db.Close();
    
    // 从备份恢复
    GraphStore restored;
    if (!IsOk(restored.Init(backup_path))) { std::cout << "FAIL (restore init)" << std::endl; return false; }
    
    TxId rtx = restored.BeginTransaction(true);
    auto rv = restored.GetVertex(rtx, v);
    if (!rv || rv->id != v) { std::cout << "FAIL (vertex mismatch)" << std::endl; restored.Close(); cleanup(backup_path); return false; }
    
    auto name = restored.GetVertexProperty(rtx, v, "name");
    if (!name || *name != "Original") { std::cout << "FAIL (prop mismatch)" << std::endl; restored.Close(); cleanup(backup_path); return false; }
    
    auto val = restored.GetVertexProperty(rtx, v, "val");
    if (!val || *val != "42") { std::cout << "FAIL (val mismatch)" << std::endl; restored.Close(); cleanup(backup_path); return false; }
    
    auto re = restored.GetEdge(rtx, e);
    if (!re) { std::cout << "FAIL (edge missing in backup)" << std::endl; restored.Close(); cleanup(backup_path); return false; }
    
    restored.Commit(rtx);
    restored.Close();
    
    cleanup(backup_path);
    cleanup(db_path);
    std::cout << "PASS" << std::endl;
    return true;
}

bool test_large_properties() {
    std::cout << "Test: large properties (overflow)... ";
    std::string db_path = "/tmp/test_large_props.db";
    cleanup(db_path);
    
    GraphStore db;
    if (!IsOk(db.Init(db_path))) { std::cout << "FAIL (init)" << std::endl; return false; }
    
    // Test 1: property value > 3000 bytes (overflow threshold)
    {
        TxId tx = db.BeginTransaction(false);
        Properties p;
        std::string big_val(5000, 'X');
        p.Set("big", big_val);
        p.Set("small", "hello");
        VertexId v = db.AddVertex(tx, 1, p);
        assert(v != 0);
        db.Commit(tx);
        
        TxId rtx = db.BeginTransaction(true);
        auto got = db.GetVertexProperty(rtx, v, "big");
        if (!got) {
            std::cout << "FAIL (big val null, expected size=" << big_val.size() << ")" << std::endl;
            return false;
        }
        if (got->size() != big_val.size()) {
            std::cout << "FAIL (big val size mismatch: expected=" << big_val.size() << " got=" << got->size() << ")" << std::endl;
            return false;
        }
        if (*got != big_val) { std::cout << "FAIL (big val content mismatch)" << std::endl; return false; }
        auto small = db.GetVertexProperty(rtx, v, "small");
        if (!small || *small != "hello") { std::cout << "FAIL (small val with big)" << std::endl; return false; }
        db.Commit(rtx);
    }
    
    // Test 2: property value > 10KB
    {
        TxId tx = db.BeginTransaction(false);
        Properties p;
        std::string big_val(15000, 'Y');
        p.Set("huge", big_val);
        VertexId v = db.AddVertex(tx, 2, p);
        db.Commit(tx);
        
        TxId rtx = db.BeginTransaction(true);
        auto got = db.GetVertexProperty(rtx, v, "huge");
        if (!got || *got != big_val) { std::cout << "FAIL (15KB val)" << std::endl; return false; }
        db.Commit(rtx);
    }
    
    // Test 3: update large property value
    {
        TxId tx = db.BeginTransaction(false);
        Properties p;
        p.Set("data", std::string(4000, 'A'));
        VertexId v = db.AddVertex(tx, 3, p);
        db.Commit(tx);
        
        // update to even larger
        TxId tx2 = db.BeginTransaction(false);
        std::string bigger(8000, 'B');
        bool ok = db.SetVertexProperty(tx2, v, "data", bigger);
        if (!ok) { std::cout << "FAIL (set large)" << std::endl; return false; }
        db.Commit(tx2);
        
        TxId rtx = db.BeginTransaction(true);
        auto got = db.GetVertexProperty(rtx, v, "data");
        if (!got || *got != bigger) { std::cout << "FAIL (read updated large)" << std::endl; return false; }
        db.Commit(rtx);
    }
    
    // Test 4: multiple large properties on same vertex
    {
        TxId tx = db.BeginTransaction(false);
        Properties p;
        p.Set("a", std::string(3001, 'M'));
        p.Set("b", std::string(3001, 'N'));
        p.Set("c", "normal");
        VertexId v = db.AddVertex(tx, 4, p);
        db.Commit(tx);
        
        TxId rtx = db.BeginTransaction(true);
        auto a = db.GetVertexProperty(rtx, v, "a");
        auto b = db.GetVertexProperty(rtx, v, "b");
        auto c = db.GetVertexProperty(rtx, v, "c");
        if (!a || a->size() != 3001) { std::cout << "FAIL (multi large a)" << std::endl; return false; }
        if (!b || b->size() != 3001) { std::cout << "FAIL (multi large b)" << std::endl; return false; }
        if (!c || *c != "normal") { std::cout << "FAIL (multi large c)" << std::endl; return false; }
        db.Commit(rtx);
    }
    
    // Test 5: edge with large property
    {
        TxId tx = db.BeginTransaction(false);
        VertexId a = db.AddVertex(tx, 5, {});
        VertexId b = db.AddVertex(tx, 5, {});
        Properties ep;
        std::string big_edge(7000, 'E');
        ep.Set("desc", big_edge);
        EdgeId e = db.AddEdge(tx, a, b, 10, ep);
        db.Commit(tx);
        
        TxId rtx = db.BeginTransaction(true);
        auto got = db.GetEdgeProperty(rtx, e, "desc");
        if (!got || *got != big_edge) { std::cout << "FAIL (edge large prop)" << std::endl; return false; }
        db.Commit(rtx);
    }
    
    db.Close();
    cleanup(db_path);
    std::cout << "PASS" << std::endl;
    return true;
}

int main() {
    std::cout << "=== GraphStore Tests ===" << std::endl;
    
    int passed = 0, total = 0;
    
    total++; if (test_crud()) passed++;
    total++; if (test_multiple_vertices()) passed++;
    total++; if (test_property_update()) passed++;
    total++; if (test_delete()) passed++;
    total++; if (test_cascade_delete()) passed++;
    total++; if (test_update_delete_combined()) passed++;
    total++; if (test_backup()) passed++;
    total++; if (test_large_properties()) passed++;
    
    std::cout << "\nResults: " << passed << "/" << total << " passed" << std::endl;
    return (passed == total) ? 0 : 1;
}

#include <iostream>
#include <cassert>
#include "graph_store.h"

using namespace graphstore;

void example_cleanup(const std::string& path) {
    std::filesystem::remove_all(path);
}

int main() {
    std::cout << "=== GraphStore Basic Example ===" << std::endl;
    
    std::string db_path = "/tmp/my_graph.db";
    example_cleanup(db_path);
    
    // 1. 初始化数据库
    GraphStore db;
    Status s = db.Init(db_path, 1ULL << 28);  // 256MB
    if (!IsOk(s)) {
        std::cerr << "Failed to init database" << std::endl;
        return 1;
    }
    std::cout << "Database initialized" << std::endl;
    
    // 2. 开始写事务
    TxId tx = db.BeginTransaction(false);  // 写事务
    std::cout << "Started transaction: " << tx << std::endl;
    
    // 3. 创建顶点（人物）
    Properties person_props;
    person_props.Set("name", "Alice");
    person_props.Set("age", "30");
    VertexId alice = db.AddVertex(tx, 1, person_props);  // label=1 表示 Person
    std::cout << "Created vertex Alice (id=" << alice << ")" << std::endl;
    
    Properties bob_props;
    bob_props.Set("name", "Bob");
    bob_props.Set("age", "25");
    VertexId bob = db.AddVertex(tx, 1, bob_props);
    std::cout << "Created vertex Bob (id=" << bob << ")" << std::endl;
    
    Properties charlie_props;
    charlie_props.Set("name", "Charlie");
    charlie_props.Set("age", "35");
    VertexId charlie = db.AddVertex(tx, 1, charlie_props);
    std::cout << "Created vertex Charlie (id=" << charlie << ")" << std::endl;
    
    // 4. 创建边（关系）
    Properties friend_props;
    friend_props.Set("since", "2020");
    friend_props.Set("strength", "5");
    EdgeId e1 = db.AddEdge(tx, alice, bob, 10, friend_props);  // label=10 表示 FRIEND
    std::cout << "Created edge Alice->Bob (id=" << e1 << ")" << std::endl;
    
    Properties knows_props;
    knows_props.Set("since", "2019");
    EdgeId e2 = db.AddEdge(tx, bob, charlie, 11, knows_props);  // label=11 表示 KNOWS
    std::cout << "Created edge Bob->Charlie (id=" << e2 << ")" << std::endl;
    
    EdgeId e3 = db.AddEdge(tx, alice, charlie, 10, friend_props);
    std::cout << "Created edge Alice->Charlie (id=" << e3 << ")" << std::endl;
    
    // 5. 提交事务
    s = db.Commit(tx);
    if (!IsOk(s)) {
        std::cerr << "Failed to commit" << std::endl;
        return 1;
    }
    std::cout << "Transaction committed" << std::endl;
    
    // 6. 开始读事务查询
    TxId rtx = db.BeginTransaction(true);  // 只读事务
    std::cout << "\nStarted read transaction: " << rtx << std::endl;
    
    // 查询顶点
    auto alice_v = db.GetVertex(rtx, alice);
    if (alice_v) {
        std::cout << "Alice: label=" << alice_v->label 
                  << ", out_degree=" << alice_v->out_degree
                  << ", in_degree=" << alice_v->in_degree << std::endl;
    }
    
    // 查询属性
    auto name = db.GetVertexProperty(rtx, alice, "name");
    if (name) {
        std::cout << "Alice's name: " << *name << std::endl;
    }
    
    auto age = db.GetVertexProperty(rtx, bob, "age");
    if (age) {
        std::cout << "Bob's age: " << *age << std::endl;
    }
    
    // 查询出边
    std::cout << "\nAlice's outgoing edges:" << std::endl;
    auto out_edges = db.GetOutEdges(rtx, alice);
    for (auto eid : out_edges) {
        auto edge = db.GetEdge(rtx, eid);
        if (edge) {
            auto since = db.GetEdgeProperty(rtx, eid, "since");
            std::cout << "  Edge " << eid << ": " << edge->src << " -> " << edge->dst;
            if (since) std::cout << " (since: " << *since << ")";
            std::cout << std::endl;
        }
    }
    
    // 查询入边
    std::cout << "\nCharlie's incoming edges:" << std::endl;
    auto in_edges = db.GetInEdges(rtx, charlie);
    for (auto eid : in_edges) {
        auto edge = db.GetEdge(rtx, eid);
        if (edge) {
            std::cout << "  Edge " << eid << ": " << edge->src << " -> " << edge->dst << std::endl;
        }
    }
    
    // 按标签查询顶点
    std::cout << "\nAll Person vertices (label=1):" << std::endl;
    auto persons = db.GetVerticesByLabel(rtx, 1);
    for (auto vid : persons) {
        auto v = db.GetVertex(rtx, vid);
        if (v) {
            auto vname = db.GetVertexProperty(rtx, vid, "name");
            std::cout << "  Vertex " << vid;
            if (vname) std::cout << ": " << *vname;
            std::cout << std::endl;
        }
    }
    
    // 7. 关闭数据库
    db.Close();
    example_cleanup(db_path);
    std::cout << "\nDatabase closed" << std::endl;
    
    // 8. 演示更新属性
    std::cout << "\n=== (2) Update Demo ===" << std::endl;
    {
        GraphStore db2;
        if (!IsOk(db2.Init(db_path, 1ULL << 28))) {
            std::cerr << "Failed to init database for update demo" << std::endl;
            return 1;
        }
        // 先创建顶点
        TxId wtx = db2.BeginTransaction(false);
        Properties p;
        p.Set("name", "Demo");
        p.Set("version", "1");
        VertexId vid = db2.AddVertex(wtx, 1, p);
        db2.Commit(wtx);

        // 读取原始值
        TxId rtx = db2.BeginTransaction(true);
        auto name_before = db2.GetVertexProperty(rtx, vid, "name");
        auto ver_before = db2.GetVertexProperty(rtx, vid, "version");
        std::cout << "Before update: name=" << (name_before ? *name_before : "?")
                  << ", version=" << (ver_before ? *ver_before : "?") << std::endl;
        db2.Commit(rtx);

        // 更新属性
        TxId utx = db2.BeginTransaction(false);
        db2.SetVertexProperty(utx, vid, "name", "UpdatedDemo");
        db2.SetVertexProperty(utx, vid, "version", "2");
        db2.SetVertexProperty(utx, vid, "status", "active");
        db2.Commit(utx);

        // 读取更新后
        TxId rtx2 = db2.BeginTransaction(true);
        auto name_after = db2.GetVertexProperty(rtx2, vid, "name");
        auto ver_after = db2.GetVertexProperty(rtx2, vid, "version");
        auto status = db2.GetVertexProperty(rtx2, vid, "status");
        std::cout << "After update: name=" << (name_after ? *name_after : "?")
                  << ", version=" << (ver_after ? *ver_after : "?")
                  << ", status=" << (status ? *status : "?") << std::endl;
        db2.Commit(rtx2);
        db2.Close();
    }
    
    // 9. 演示删除顶点/边
    std::cout << "\n=== (3) Delete Demo ===" << std::endl;
    {
        GraphStore db3;
        if (!IsOk(db3.Init(db_path, 1ULL << 28))) {
            std::cerr << "Failed to init database for delete demo" << std::endl;
            return 1;
        }
        // 创建顶点和边
        TxId wtx = db3.BeginTransaction(false);
        VertexId va = db3.AddVertex(wtx, 1, {});
        VertexId vb = db3.AddVertex(wtx, 1, {});
        EdgeId e = db3.AddEdge(wtx, va, vb, 10, {});
        db3.Commit(wtx);

        TxId rtx = db3.BeginTransaction(true);
        std::cout << "Before delete: vertices=" << db3.GetVertexCount(rtx)
                  << ", edges from " << va << " = " << db3.GetOutEdges(rtx, va).size() << std::endl;
        db3.Commit(rtx);

        // 删除边
        TxId dtx = db3.BeginTransaction(false);
        db3.RemoveEdge(dtx, e);
        db3.Commit(dtx);

        // 删除顶点
        TxId dtx2 = db3.BeginTransaction(false);
        db3.RemoveVertex(dtx2, va);
        db3.Commit(dtx2);

        TxId rtx2 = db3.BeginTransaction(true);
        std::cout << "After delete: vertices=" << db3.GetVertexCount(rtx2)
                  << ", va still exists=" << db3.GetVertex(rtx2, va).has_value() << std::endl;
        db3.Commit(rtx2);
        db3.Close();
    }
    
    // 10. 清理
    example_cleanup(db_path);
    std::cout << "\nAll demos completed" << std::endl;
    
    return 0;
}

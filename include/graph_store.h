#ifndef GRAPH_STORE_H
#define GRAPH_STORE_H

#include "common.h"
#include "page_manager.h"
#include "bplus_tree.h"
#include "serializer.h"
#include <thread>
#include <atomic>
#include <unordered_set>

namespace graphstore {

struct Transaction {
    TxId     id;
    TxId     read_ts;       // 读快照时间戳
    TxId     write_ver;     // 写入版本（全局唯一事务ID，提交时patching为commit_ts）
    bool     is_write;
    bool     committed;
    std::vector<PageId> dirty_pages;
    
    // v4: OCC读集（key→old_value序列化+树索引）
    enum : uint8_t {
        TREE_VERTEX = 0,
        TREE_EDGE,
        TREE_OUT_EDGE,
        TREE_IN_EDGE,
        TREE_PROP,
        TREE_LABEL,
        TREE_PROP_INDEX,
    };
    struct ReadEntry {
        std::vector<uint8_t> key;
        std::vector<uint8_t> old_value;
        uint8_t tree_index = TREE_VERTEX;
    };
    std::vector<ReadEntry> read_set_;
    
    struct WriteEntry {
        std::vector<uint8_t> key;
        std::vector<uint8_t> old_value;
        uint8_t tree_index = TREE_VERTEX;
    };
    std::vector<WriteEntry> write_set_;
    
    // v5: 每棵树COW根跟踪（用于CAS提交）
    struct TreeRoot {
        PageId snapshot_root = INVALID_PAGE;
        PageId cow_root = INVALID_PAGE;
    };
    std::unordered_map<uint8_t, TreeRoot> tree_roots_;
    
    TreeRoot& GetOrCreateTreeRoot(uint8_t tree_idx, PageId committed_root) {
        auto it = tree_roots_.find(tree_idx);
        if (it == tree_roots_.end()) {
            TreeRoot tr;
            tr.snapshot_root = committed_root;
            tr.cow_root = committed_root;
            it = tree_roots_.emplace(tree_idx, tr).first;
        }
        return it->second;
    }
    
    Transaction(TxId tx_id, TxId read_ts, bool write)
        : id(tx_id), read_ts(read_ts), write_ver(read_ts), is_write(write), committed(false) {}
};

struct Vertex {
    VertexId id;
    LabelId  label;
    Properties props;
    uint32_t out_degree;
    uint32_t in_degree;
};

struct Edge {
    EdgeId   id;
    VertexId src;
    VertexId dst;
    LabelId  label;
    Properties props;
};

// v3: 死锁检测 - 等待图节点
struct WaitForEntry {
    TxId waiting_tx;
    TxId holding_tx;
};

class GraphStore {
public:
    GraphStore();
    ~GraphStore();
    
    Status Init(const std::string& path, size_t map_size = 64ULL << 20);
    Status Close();
    
    // v3: 事务管理（OCC模式）
    TxId BeginTransaction(bool read_only = false);
    Status Commit(TxId tx_id);
    void Rollback(TxId tx_id);
    
    // v3: 显式加锁（悲观模式，可选）
    bool TryLockVertex(TxId tx, VertexId id);
    bool TryLockEdge(TxId tx, EdgeId id);
    void UnlockAll(TxId tx);
    
    // 顶点操作
    VertexId AddVertex(TxId tx, LabelId label, const Properties& props = {});
    bool RemoveVertex(TxId tx, VertexId id);
    bool SetVertexProperty(TxId tx, VertexId id, const std::string& key, const std::string& val);
    bool AddVertexLabel(TxId tx, VertexId id, LabelId label);
    bool RemoveVertexLabel(TxId tx, VertexId id, LabelId label);
    bool RemoveVertexProperty(TxId tx, VertexId id, const std::string& key);
    
    // 边操作
    EdgeId AddEdge(TxId tx, VertexId src, VertexId dst, LabelId label, const Properties& props = {});
    bool RemoveEdge(TxId tx, EdgeId id);
    bool SetEdgeProperty(TxId tx, EdgeId id, const std::string& key, const std::string& val);
    bool RemoveEdgeProperty(TxId tx, EdgeId id, const std::string& key);
    
    // 查询
    std::optional<Vertex> GetVertex(TxId read_ts, VertexId id) const;
    std::optional<Edge>   GetEdge(TxId read_ts, EdgeId id) const;
    std::vector<EdgeId>   GetOutEdges(TxId read_ts, VertexId src) const;
    std::vector<EdgeId>   GetInEdges(TxId read_ts, VertexId dst) const;
    std::vector<VertexId> GetAllVertices(TxId read_ts) const;
    std::vector<VertexId> GetVerticesByLabel(TxId read_ts, LabelId label) const;
    
    // 属性查询
    std::optional<std::string> GetVertexProperty(TxId read_ts, VertexId id, const std::string& key) const;
    std::optional<std::string> GetEdgeProperty(TxId read_ts, EdgeId id, const std::string& key) const;
    
    // v4: 属性索引查询
    std::vector<VertexId> GetVerticesByProperty(TxId read_ts, LabelId label,
                                                 const std::string& key,
                                                 const std::string& value) const;
    
    // 事务内读自己的写入（设置search_root使读操作穿透到COW树）
    void SetSearchRoots(Transaction* txn) const;
    void ResetSearchRoots() const;
    
    // v6: 批量导入
    struct BulkVertexInput {
        LabelId label = 0;
        Properties props;
    };
    struct BulkEdgeInput {
        VertexId src = 0;
        VertexId dst = 0;
        LabelId label = 0;
        Properties props;
    };
    struct BulkInsertOutput {
        std::vector<VertexId> vertex_ids;
        std::vector<EdgeId> edge_ids;
    };
    BulkInsertOutput BulkInsert(TxId tx,
        const std::vector<BulkVertexInput>& vertices,
        const std::vector<BulkEdgeInput>& edges);
    BulkInsertOutput ImportCSV(TxId tx,
        const std::string& vertices_csv,
        const std::string& edges_csv);
    
    // 统计
    size_t GetVertexCount(TxId read_ts) const;
    size_t GetEdgeCount(TxId read_ts) const;
    
    // 维护
    Status Checkpoint();
    Status Compact();     // 手动触发GC
    Status Backup(const std::filesystem::path& backup_path);
    
    // GC控制
    void StartGC();
    void StopGC();
    
    // v3: 死锁检测
    bool HasDeadlock() const;
    
    // 事务查询（公开以便外部读取事务元信息）
    Transaction* GetTransaction(TxId tx_id);
    const Transaction* GetTransaction(TxId tx_id) const;
    
    TxId ResolveReadTs(TxId tx_or_ts) const;
    
private:
    std::unique_ptr<PageManager> pm_;
    
    std::unique_ptr<BPlusTree> vertex_tree_;
    std::unique_ptr<BPlusTree> edge_tree_;
    std::unique_ptr<BPlusTree> out_edge_tree_;
    std::unique_ptr<BPlusTree> in_edge_tree_;
    std::unique_ptr<BPlusTree> prop_tree_;
    std::unique_ptr<BPlusTree> label_tree_;
    std::unique_ptr<BPlusTree> prop_index_tree_;  // v4: 属性→实体倒排索引
    
    // 事务管理
    std::atomic<TxId> global_tx_id_{1000000};
    std::atomic<TxId> global_commit_ts_{0};
    std::mutex commit_mutex_;        // 序列化meta页写入
    std::set<TxId>    active_readers_;
    mutable std::mutex        readers_mutex_;
    
    // v3: 活跃事务表（OCC验证用）
    std::unordered_map<TxId, std::unique_ptr<Transaction>> active_txns_;
    mutable std::mutex txns_mutex_;  // const getters 也需要访问
    
    // v3: 等待图（死锁检测）
    std::vector<WaitForEntry> wait_for_graph_;
    mutable std::mutex wait_mutex_;
    
    // v3: 行级锁表
    std::unordered_map<uint64_t, TxId> vertex_locks_;  // vertex_id -> tx_id
    std::unordered_map<uint64_t, TxId> edge_locks_;    // edge_id -> tx_id
    std::mutex lock_mutex_;
    
    // ID生成器
    std::atomic<VertexId> next_vertex_id_{1};
    std::atomic<EdgeId>   next_edge_id_{1};
    
    // GC
    std::atomic<bool> gc_running_{false};
    std::thread gc_thread_;
    
    // 辅助方法
    Status InitTrees();
    void RegisterReader(TxId read_ts);
    void UnregisterReader(TxId read_ts);
    TxId GetMinReadTs() const;
    
    // v3: OCC验证
    bool ValidateReadSet(Transaction* tx);
    bool ValidateWriteSet(Transaction* tx);
    
    // v5: 按索引获取树指针
    BPlusTree* GetTreeByIndex(uint8_t tree_idx) const;

    // 批量边删除（避免 O(E) 内存占用）
    void RemoveAllEdges(TxId tx, VertexId id);
    
    // v3: 死锁检测（DFS）
    bool DetectCycle(TxId start, std::unordered_set<TxId>& visited, 
                     std::unordered_set<TxId>& rec_stack) const;
    
    // 属性存储
    Status StoreProperties(TxId tx_id, uint64_t owner_id, const Properties& props);
    Properties LoadProperties(TxId read_ts, uint64_t owner_id) const;
    // v4: 属性索引辅助
    void UpdatePropIndex(Transaction* txn, TxId write_ver, LabelId label,
                         uint64_t entity_id, const std::string& key,
                         const std::string& old_val, const std::string& new_val);
    
    // 可见性检查
    bool IsVisible(const VertexPayload& payload, TxId read_ts) const;
    bool IsVisible(const EdgePayload& payload, TxId read_ts) const;
    
    // 实时度数计算（避免版本链膨胀）
    uint32_t CountOutEdges(TxId read_ts, VertexId id) const;
    uint32_t CountInEdges(TxId read_ts, VertexId id) const;

    // GC
    void GCLoop();
    void CollectGarbage(TxId before_ts);
};

} // namespace graphstore
#endif // GRAPH_STORE_H

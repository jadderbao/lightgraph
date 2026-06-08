#include "graph_store.h"
#include <iostream>
#include <algorithm>
#include <future>
#include <fstream>

namespace graphstore {

GraphStore::GraphStore() = default;

GraphStore::~GraphStore() {
    StopGC();
    Close();
}

Status GraphStore::Init(const std::string& path, size_t map_size) {
    pm_ = std::make_unique<PageManager>();
    Status s = pm_->Init(path, map_size);
    if (!IsOk(s)) return s;
    
    s = InitTrees();
    if (!IsOk(s)) return s;
    
    return Status::OK;
}

Status GraphStore::Close() {
    StopGC();
    if (pm_) {
        pm_->FlushAll();
        pm_->Close();
        pm_.reset();
    }
    vertex_tree_.reset();
    edge_tree_.reset();
    out_edge_tree_.reset();
    in_edge_tree_.reset();
    prop_tree_.reset();
    label_tree_.reset();
    return Status::OK;
}

Status GraphStore::InitTrees() {
    auto* meta = pm_->GetMetaPage();
    global_commit_ts_ = meta->global_commit_ts;
    
    auto key_cmp = [](const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
        if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
        int r = std::memcmp(a.data(), b.data(), a.size());
        return r < 0 ? -1 : (r > 0 ? 1 : 0);
    };
    
    if (meta->vertex_root == INVALID_PAGE) {
        vertex_tree_ = std::make_unique<BPlusTree>(pm_.get(), key_cmp);
        edge_tree_ = std::make_unique<BPlusTree>(pm_.get(), key_cmp);
        out_edge_tree_ = std::make_unique<BPlusTree>(pm_.get(), key_cmp);
        in_edge_tree_ = std::make_unique<BPlusTree>(pm_.get(), key_cmp);
        prop_tree_ = std::make_unique<BPlusTree>(pm_.get(), key_cmp);
        label_tree_ = std::make_unique<BPlusTree>(pm_.get(), key_cmp);
        prop_index_tree_ = std::make_unique<BPlusTree>(pm_.get(), key_cmp);
        meta->vertex_root = vertex_tree_->GetRootPage();
        meta->edge_root = edge_tree_->GetRootPage();
        meta->out_edge_root = out_edge_tree_->GetRootPage();
        meta->in_edge_root = in_edge_tree_->GetRootPage();
        meta->prop_root = prop_tree_->GetRootPage();
        meta->label_root = label_tree_->GetRootPage();
        meta->prop_index_root = prop_index_tree_->GetRootPage();
    } else {
        vertex_tree_ = std::make_unique<BPlusTree>(pm_.get(), meta->vertex_root, key_cmp);
        edge_tree_ = std::make_unique<BPlusTree>(pm_.get(), meta->edge_root, key_cmp);
        out_edge_tree_ = std::make_unique<BPlusTree>(pm_.get(), meta->out_edge_root, key_cmp);
        in_edge_tree_ = std::make_unique<BPlusTree>(pm_.get(), meta->in_edge_root, key_cmp);
        prop_tree_ = std::make_unique<BPlusTree>(pm_.get(), meta->prop_root, key_cmp);
        label_tree_ = std::make_unique<BPlusTree>(pm_.get(), meta->label_root, key_cmp);
        prop_index_tree_ = std::make_unique<BPlusTree>(pm_.get(), meta->prop_index_root, key_cmp);
    }
    
    return Status::OK;
}

TxId GraphStore::BeginTransaction(bool read_only) {
    TxId tx_id = global_tx_id_.fetch_add(1, std::memory_order_relaxed);
    TxId read_ts = global_commit_ts_.load(std::memory_order_acquire);
    
    auto tx = std::make_unique<Transaction>(tx_id, read_ts, !read_only);
    if (!read_only) {
        tx->write_ver = tx_id;
    }
    {
        std::lock_guard<std::mutex> lock(txns_mutex_);
        active_txns_[tx_id] = std::move(tx);
    }
    
    RegisterReader(read_ts);
    return tx_id;
}

Status GraphStore::Commit(TxId tx_id) {
    Transaction* tx = nullptr;
    {
        std::lock_guard<std::mutex> tlock(txns_mutex_);
        auto it = active_txns_.find(tx_id);
        if (it == active_txns_.end()) return Status::INVALID_ARGUMENT;
        tx = it->second.get();
    }
    
    if (!tx->is_write) {
        tx->committed = true;
        UnregisterReader(tx->read_ts);
        {
            std::lock_guard<std::mutex> tlock(txns_mutex_);
            active_txns_.erase(tx_id);
        }
        return Status::OK;
    }
    
    if (!ValidateReadSet(tx) || !ValidateWriteSet(tx)) {
        Rollback(tx_id);
        return Status::TX_CONFLICT;
    }
    
    TxId commit_ts = global_commit_ts_.fetch_add(1, std::memory_order_acq_rel) + 1;
    pm_->SetGlobalCommitTs(commit_ts);
    
    // Phase 1: 版本节点patching（write_ver→commit_ts）
    for (PageId pid : tx->dirty_pages) {
        if (pid != 0) vertex_tree_->PatchVersion(pid, tx->write_ver, commit_ts);
    }
    
    // Phase 2: CAS发布每棵树的COW根（使新数据对读者可见）
    for (auto& [tree_idx, tr] : tx->tree_roots_) {
        auto* tree = GetTreeByIndex(tree_idx);
        if (!tree) continue;
        bool pub_ok = tree->PublishRoot(tr.snapshot_root, tr.cow_root);
        if (!pub_ok) {
            Rollback(tx_id);
            return Status::TX_CONFLICT;
        }
    }
    
    // Phase 3: 持久化脏数据页
    for (PageId pid : tx->dirty_pages) {
        if (pid != 0) pm_->FlushPage(pid);
    }
    
    // Phase 4: 串行化更新meta页根指针
    {
        std::lock_guard<std::mutex> lock(commit_mutex_);
        auto* meta = pm_->GetMetaPage();
        meta->vertex_root = vertex_tree_->GetRootPage();
        meta->edge_root = edge_tree_->GetRootPage();
        meta->out_edge_root = out_edge_tree_->GetRootPage();
        meta->in_edge_root = in_edge_tree_->GetRootPage();
        meta->prop_root = prop_tree_->GetRootPage();
        meta->label_root = label_tree_->GetRootPage();
        meta->prop_index_root = prop_index_tree_ ? prop_index_tree_->GetRootPage() : INVALID_PAGE;
        meta->num_pages = pm_->GetNumPages();
        pm_->FlushPage(0);
    }
    
    tx->committed = true;
    UnregisterReader(tx->read_ts);
    UnlockAll(tx_id);
    
    {
        std::lock_guard<std::mutex> tlock(txns_mutex_);
        active_txns_.erase(tx_id);
    }
    
    return Status::OK;
}

void GraphStore::Rollback(TxId tx_id) {
    Transaction* tx = nullptr;
    {
        std::lock_guard<std::mutex> tlock(txns_mutex_);
        auto it = active_txns_.find(tx_id);
        if (it != active_txns_.end()) tx = it->second.get();
    }
    
    if (tx) {
        UnlockAll(tx_id);
        UnregisterReader(tx->read_ts);
        {
            std::lock_guard<std::mutex> tlock(txns_mutex_);
            active_txns_.erase(tx_id);
        }
    }
}

bool GraphStore::TryLockVertex(TxId tx, VertexId id) {
    std::lock_guard<std::mutex> lock(lock_mutex_);
    auto it = vertex_locks_.find(id);
    if (it != vertex_locks_.end() && it->second != tx) {
        std::lock_guard<std::mutex> wlock(wait_mutex_);
        wait_for_graph_.push_back({tx, it->second});
        return false;
    }
    vertex_locks_[id] = tx;
    return true;
}

bool GraphStore::TryLockEdge(TxId tx, EdgeId id) {
    std::lock_guard<std::mutex> lock(lock_mutex_);
    auto it = edge_locks_.find(id);
    if (it != edge_locks_.end() && it->second != tx) {
        std::lock_guard<std::mutex> wlock(wait_mutex_);
        wait_for_graph_.push_back({tx, it->second});
        return false;
    }
    edge_locks_[id] = tx;
    return true;
}

void GraphStore::UnlockAll(TxId tx) {
    std::lock_guard<std::mutex> lock(lock_mutex_);
    for (auto it = vertex_locks_.begin(); it != vertex_locks_.end(); ) {
        if (it->second == tx) it = vertex_locks_.erase(it);
        else ++it;
    }
    for (auto it = edge_locks_.begin(); it != edge_locks_.end(); ) {
        if (it->second == tx) it = edge_locks_.erase(it);
        else ++it;
    }
    {
        std::lock_guard<std::mutex> wlock(wait_mutex_);
        wait_for_graph_.erase(
            std::remove_if(wait_for_graph_.begin(), wait_for_graph_.end(),
                [tx](const WaitForEntry& e) { return e.waiting_tx == tx; }),
            wait_for_graph_.end());
    }
}

bool GraphStore::ValidateReadSet(Transaction* tx) {
    TxId current_commit = global_commit_ts_.load(std::memory_order_acquire);
    for (const auto& entry : tx->read_set_) {
        // 跳过本事务写入的 key（写集中的条目不验证）
        bool in_write = false;
        for (const auto& we : tx->write_set_) {
            if (we.key == entry.key && we.tree_index == entry.tree_index) {
                in_write = true; break;
            }
        }
        if (in_write) continue;
        
        BPlusTree* tree = nullptr;
        switch (entry.tree_index) {
            case Transaction::TREE_VERTEX:     tree = vertex_tree_.get(); break;
            case Transaction::TREE_EDGE:       tree = edge_tree_.get(); break;
            case Transaction::TREE_OUT_EDGE:   tree = out_edge_tree_.get(); break;
            case Transaction::TREE_IN_EDGE:    tree = in_edge_tree_.get(); break;
            case Transaction::TREE_PROP:       tree = prop_tree_.get(); break;
            case Transaction::TREE_LABEL:      tree = label_tree_.get(); break;
            case Transaction::TREE_PROP_INDEX: tree = prop_index_tree_.get(); break;
            default: continue;
        }
        
        auto current = tree->Get(entry.key, current_commit);
        std::vector<uint8_t> current_val = current.value_or(std::vector<uint8_t>{});
        if (current_val != entry.old_value) {
            return false;
        }
    }
    return true;
}

bool GraphStore::ValidateWriteSet(Transaction* tx) {
    TxId current_commit = global_commit_ts_.load(std::memory_order_acquire);
    for (const auto& entry : tx->write_set_) {
        BPlusTree* tree = nullptr;
        switch (entry.tree_index) {
            case Transaction::TREE_VERTEX:     tree = vertex_tree_.get(); break;
            case Transaction::TREE_EDGE:       tree = edge_tree_.get(); break;
            case Transaction::TREE_OUT_EDGE:   tree = out_edge_tree_.get(); break;
            case Transaction::TREE_IN_EDGE:    tree = in_edge_tree_.get(); break;
            case Transaction::TREE_PROP:       tree = prop_tree_.get(); break;
            case Transaction::TREE_LABEL:      tree = label_tree_.get(); break;
            case Transaction::TREE_PROP_INDEX: tree = prop_index_tree_.get(); break;
            default: continue;
        }
        
        // 读当前已提交的值（排除本事务未提交的修改）
        auto current = tree->Get(entry.key, current_commit);
        std::vector<uint8_t> current_val = current.value_or(std::vector<uint8_t>{});
        if (current_val != entry.old_value) {
            return false;
        }
    }
    return true;
}

bool GraphStore::HasDeadlock() const {
    std::lock_guard<std::mutex> lock(wait_mutex_);
    std::unordered_set<TxId> all_txs;
    for (const auto& e : wait_for_graph_) {
        all_txs.insert(e.waiting_tx);
        all_txs.insert(e.holding_tx);
    }
    
    for (TxId start : all_txs) {
        std::unordered_set<TxId> visited, rec_stack;
        if (DetectCycle(start, visited, rec_stack)) return true;
    }
    return false;
}

BPlusTree* GraphStore::GetTreeByIndex(uint8_t tree_idx) const {
    switch (tree_idx) {
        case Transaction::TREE_VERTEX:     return vertex_tree_.get();
        case Transaction::TREE_EDGE:       return edge_tree_.get();
        case Transaction::TREE_OUT_EDGE:   return out_edge_tree_.get();
        case Transaction::TREE_IN_EDGE:    return in_edge_tree_.get();
        case Transaction::TREE_PROP:       return prop_tree_.get();
        case Transaction::TREE_LABEL:      return label_tree_.get();
        case Transaction::TREE_PROP_INDEX: return prop_index_tree_.get();
        default: return nullptr;
    }
}

bool GraphStore::DetectCycle(TxId start, std::unordered_set<TxId>& visited, 
                              std::unordered_set<TxId>& rec_stack) const {
    visited.insert(start);
    rec_stack.insert(start);
    
    for (const auto& e : wait_for_graph_) {
        if (e.waiting_tx == start) {
            if (rec_stack.count(e.holding_tx)) return true;
            if (!visited.count(e.holding_tx)) {
                if (DetectCycle(e.holding_tx, visited, rec_stack)) return true;
            }
        }
    }
    
    rec_stack.erase(start);
    return false;
}

void GraphStore::RegisterReader(TxId read_ts) {
    std::lock_guard<std::mutex> lock(readers_mutex_);
    active_readers_.insert(read_ts);
}

void GraphStore::UnregisterReader(TxId read_ts) {
    std::lock_guard<std::mutex> lock(readers_mutex_);
    active_readers_.erase(read_ts);
}

TxId GraphStore::GetMinReadTs() const {
    std::lock_guard<std::mutex> lock(readers_mutex_);
    if (active_readers_.empty()) return global_commit_ts_.load(std::memory_order_acquire);
    return *active_readers_.begin();
}

Transaction* GraphStore::GetTransaction(TxId tx_id) {
    std::lock_guard<std::mutex> tlock(txns_mutex_);
    auto it = active_txns_.find(tx_id);
    return (it != active_txns_.end()) ? it->second.get() : nullptr;
}

const Transaction* GraphStore::GetTransaction(TxId tx_id) const {
    std::lock_guard<std::mutex> tlock(txns_mutex_);
    auto it = active_txns_.find(tx_id);
    return (it != active_txns_.end()) ? it->second.get() : nullptr;
}

// 在只读事务中跟踪读取的 key 及其当前值
static void TrackRead(Transaction* tx, const std::vector<uint8_t>& key,
                      const std::optional<std::vector<uint8_t>>& value,
                      uint8_t tree_index = Transaction::TREE_VERTEX) {
    if (!tx || !tx->is_write) return;
    Transaction::ReadEntry entry;
    entry.key = key;
    entry.old_value = value.value_or(std::vector<uint8_t>{});
    entry.tree_index = tree_index;
    tx->read_set_.push_back(std::move(entry));
}

// 跟踪写集：记录要写入的 key 及其写前快照值
static void TrackWrite(Transaction* tx, const std::vector<uint8_t>& key,
                       const std::optional<std::vector<uint8_t>>& old_val,
                       uint8_t tree_index = Transaction::TREE_VERTEX) {
    if (!tx) return;
    Transaction::WriteEntry entry;
    entry.key = key;
    entry.old_value = old_val.value_or(std::vector<uint8_t>{});
    entry.tree_index = tree_index;
    tx->write_set_.push_back(std::move(entry));
}

VertexId GraphStore::AddVertex(TxId tx, LabelId label, const Properties& props) {
    auto* txn = GetTransaction(tx);
    if (!txn) return 0;
    TxId write_ver = txn->write_ver;
    
    VertexId id = next_vertex_id_.fetch_add(1, std::memory_order_relaxed);
    
    VertexPayload payload{};
    payload.id = id;
    payload.label = label;
    payload.create_ver = write_ver;
    payload.delete_ver = 0;
    payload.first_prop_page = INVALID_PAGE;
    payload.out_degree = 0;
    payload.in_degree = 0;
    payload.flags = 0;
    
    std::vector<PageId> dirty;
    
    if (!props.data.empty()) {
        Status s = StoreProperties(tx, id, props);
        if (!IsOk(s)) return 0;
        auto& pi_tr = txn->GetOrCreateTreeRoot(Transaction::TREE_PROP_INDEX, prop_index_tree_->GetCommittedRoot());
        for (const auto& [k, v] : props.data) {
            auto idx_key = KeyEncoder::PropIndexKey(label, k, v, id);
            PageId pi_new_root;
            prop_index_tree_->Insert(idx_key, {}, write_ver, pi_tr.cow_root, pi_new_root, dirty);
            pi_tr.cow_root = pi_new_root;
        }
    }
    
    auto key = KeyEncoder::VertexKey(id);
    auto val = payload.Serialize();
    
    auto& v_tr = txn->GetOrCreateTreeRoot(Transaction::TREE_VERTEX, vertex_tree_->GetCommittedRoot());
    PageId v_new_root;
    Status s = vertex_tree_->Insert(key, val, write_ver, v_tr.cow_root, v_new_root, dirty);
    v_tr.cow_root = v_new_root;
    if (!IsOk(s)) return 0;
    
    auto& l_tr = txn->GetOrCreateTreeRoot(Transaction::TREE_LABEL, label_tree_->GetCommittedRoot());
    PageId l_new_root;
    auto label_key = KeyEncoder::LabelKey(label, id);
    label_tree_->Insert(label_key, {}, write_ver, l_tr.cow_root, l_new_root, dirty);
    l_tr.cow_root = l_new_root;
    
    TrackWrite(txn, key, std::nullopt, Transaction::TREE_VERTEX);
    txn->dirty_pages.insert(txn->dirty_pages.end(), dirty.begin(), dirty.end());
    return id;
}

bool GraphStore::RemoveVertex(TxId tx, VertexId id) {
    auto* txn = GetTransaction(tx);
    if (!txn) return false;
    TxId write_ver = txn->write_ver;
    SetSearchRoots(txn);
    
    auto key = KeyEncoder::VertexKey(id);
    
    auto val = vertex_tree_->Get(key, write_ver);
    if (!val) {
        ResetSearchRoots();
        return false;
    }
    
    auto payload = VertexPayload::Deserialize(*val);
    if (!IsVisible(payload, write_ver)) return false;
    
    // Clean up property index entries before deletion
    {
        auto props = LoadProperties(write_ver, id);
        for (const auto& [k, v] : props.data) {
            UpdatePropIndex(txn, write_ver, payload.label, id, k, v, "");
        }
    }
    
    RemoveAllEdges(tx, id);
    
    std::vector<PageId> dirty;
    auto& v_tr = txn->GetOrCreateTreeRoot(Transaction::TREE_VERTEX, vertex_tree_->GetCommittedRoot());
    PageId v_new_root;
    vertex_tree_->Remove(key, write_ver, v_tr.cow_root, v_new_root, dirty);
    v_tr.cow_root = v_new_root;
    
    // Clean up label_tree_ entry
    {
        std::vector<PageId> label_dirty;
        auto& l_tr = txn->GetOrCreateTreeRoot(Transaction::TREE_LABEL, label_tree_->GetCommittedRoot());
        PageId l_new_root;
        label_tree_->Remove(KeyEncoder::LabelKey(payload.label, id), write_ver, l_tr.cow_root, l_new_root, label_dirty);
        l_tr.cow_root = l_new_root;
        txn->dirty_pages.insert(txn->dirty_pages.end(), label_dirty.begin(), label_dirty.end());
    }
    
    auto old_vertex = vertex_tree_->Get(key, txn->read_ts);
    TrackWrite(txn, key, old_vertex, Transaction::TREE_VERTEX);
    txn->dirty_pages.insert(txn->dirty_pages.end(), dirty.begin(), dirty.end());
    ResetSearchRoots();
    return true;
}

void GraphStore::UpdatePropIndex(Transaction* txn, TxId write_ver, LabelId label,
                                  uint64_t entity_id, const std::string& key,
                                  const std::string& old_val, const std::string& new_val) {
    std::vector<PageId> dirty;
    auto& pi_tr = txn->GetOrCreateTreeRoot(Transaction::TREE_PROP_INDEX, prop_index_tree_->GetCommittedRoot());
    if (!old_val.empty()) {
        auto old_idx_key = KeyEncoder::PropIndexKey(label, key, old_val, entity_id);
        PageId pi_new_root;
        prop_index_tree_->Remove(old_idx_key, write_ver, pi_tr.cow_root, pi_new_root, dirty);
        pi_tr.cow_root = pi_new_root;
    }
    if (!new_val.empty()) {
        auto new_idx_key = KeyEncoder::PropIndexKey(label, key, new_val, entity_id);
        PageId pi_new_root;
        prop_index_tree_->Insert(new_idx_key, {}, write_ver, pi_tr.cow_root, pi_new_root, dirty);
        pi_tr.cow_root = pi_new_root;
    }
    txn->dirty_pages.insert(txn->dirty_pages.end(), dirty.begin(), dirty.end());
}

bool GraphStore::SetVertexProperty(TxId tx, VertexId id, const std::string& key, const std::string& val) {
    auto* txn = GetTransaction(tx);
    if (!txn) return false;
    TxId write_ver = txn->write_ver;
    SetSearchRoots(txn);
    
    auto old_val_opt = GetVertexProperty(write_ver, id, key);
    auto props = LoadProperties(write_ver, id);
    props.Set(key, val);
    
    auto prop_key = KeyEncoder::EntityPropertyKey(id);
    auto old_prop_val = prop_tree_->Get(prop_key, txn->read_ts);
    Status s = StoreProperties(tx, id, props);
    if (!IsOk(s)) return false;
    
    TrackWrite(txn, prop_key, old_prop_val, Transaction::TREE_PROP);
    
    auto v = GetVertex(write_ver, id);
    if (v) {
        UpdatePropIndex(txn, write_ver, v->label, id, key,
                        old_val_opt.value_or(""), val);
    }
    
    ResetSearchRoots();
    return true;
}

bool GraphStore::AddVertexLabel(TxId tx, VertexId id, LabelId label) {
    auto* txn = GetTransaction(tx);
    if (!txn) return false;
    TxId write_ver = txn->write_ver;
    SetSearchRoots(txn);
    std::vector<PageId> dirty;
    auto key = KeyEncoder::LabelKey(label, id);
    TrackWrite(txn, key, std::nullopt, Transaction::TREE_LABEL);
    auto& l_tr = txn->GetOrCreateTreeRoot(Transaction::TREE_LABEL, label_tree_->GetCommittedRoot());
    PageId l_new_root;
    label_tree_->Insert(key, {}, write_ver, l_tr.cow_root, l_new_root, dirty);
    l_tr.cow_root = l_new_root;
    txn->dirty_pages.insert(txn->dirty_pages.end(), dirty.begin(), dirty.end());
    ResetSearchRoots();
    return true;
}

bool GraphStore::RemoveVertexLabel(TxId tx, VertexId id, LabelId label) {
    auto* txn = GetTransaction(tx);
    if (!txn) return false;
    TxId write_ver = txn->write_ver;
    SetSearchRoots(txn);
    std::vector<PageId> dirty;
    auto key = KeyEncoder::LabelKey(label, id);
    TrackWrite(txn, key, label_tree_->Get(key, txn->read_ts), Transaction::TREE_LABEL);
    auto& l_tr = txn->GetOrCreateTreeRoot(Transaction::TREE_LABEL, label_tree_->GetCommittedRoot());
    PageId l_new_root;
    label_tree_->Remove(key, write_ver, l_tr.cow_root, l_new_root, dirty);
    l_tr.cow_root = l_new_root;
    txn->dirty_pages.insert(txn->dirty_pages.end(), dirty.begin(), dirty.end());
    ResetSearchRoots();
    return true;
}

bool GraphStore::RemoveVertexProperty(TxId tx, VertexId id, const std::string& key) {
    auto* txn = GetTransaction(tx);
    if (!txn) return false;
    TxId write_ver = txn->write_ver;
    SetSearchRoots(txn);

    auto old_val_opt = GetVertexProperty(write_ver, id, key);
    auto props = LoadProperties(write_ver, id);
    props.Remove(key);

    auto prop_key = KeyEncoder::EntityPropertyKey(id);
    auto old_prop_val = prop_tree_->Get(prop_key, txn->read_ts);
    Status s = StoreProperties(tx, id, props);
    if (!IsOk(s)) return false;

    TrackWrite(txn, prop_key, old_prop_val, Transaction::TREE_PROP);

    auto v = GetVertex(write_ver, id);
    if (v) {
        std::vector<PageId> index_dirty;
        if (old_val_opt) {
            auto old_idx_key = KeyEncoder::PropIndexKey(v->label, key, *old_val_opt, id);
            auto& pi_tr = txn->GetOrCreateTreeRoot(Transaction::TREE_PROP_INDEX, prop_index_tree_->GetCommittedRoot());
            PageId pi_new_root;
            prop_index_tree_->Remove(old_idx_key, write_ver, pi_tr.cow_root, pi_new_root, index_dirty);
            pi_tr.cow_root = pi_new_root;
        }
        txn->dirty_pages.insert(txn->dirty_pages.end(), index_dirty.begin(), index_dirty.end());
    }
    ResetSearchRoots();
    return true;
}

EdgeId GraphStore::AddEdge(TxId tx, VertexId src, VertexId dst, LabelId label, const Properties& props) {
    auto* txn = GetTransaction(tx);
    if (!txn) return 0;
    TxId write_ver = txn->write_ver;
    SetSearchRoots(txn);
    
    auto src_v = vertex_tree_->Get(KeyEncoder::VertexKey(src), write_ver);
    auto dst_v = vertex_tree_->Get(KeyEncoder::VertexKey(dst), write_ver);
    if (!src_v || !dst_v) { ResetSearchRoots(); return 0; }
    
    EdgeId id = next_edge_id_.fetch_add(1, std::memory_order_relaxed);
    
    EdgePayload payload{};
    payload.id = id;
    payload.src = src;
    payload.dst = dst;
    payload.label = label;
    payload.create_ver = write_ver;
    payload.delete_ver = 0;
    payload.first_prop_page = INVALID_PAGE;
    payload.flags = 0;
    
    if (!props.data.empty()) {
        StoreProperties(tx, static_cast<uint64_t>(id) | (1ULL << 63), props);
    }
    
    std::vector<PageId> dirty;
    
    auto edge_key = KeyEncoder::EdgeKey(id);
    auto& e_tr = txn->GetOrCreateTreeRoot(Transaction::TREE_EDGE, edge_tree_->GetCommittedRoot());
    PageId e_new_root;
    edge_tree_->Insert(edge_key, payload.Serialize(), write_ver, e_tr.cow_root, e_new_root, dirty);
    e_tr.cow_root = e_new_root;
    
    auto out_key = KeyEncoder::OutEdgeKey(src, id);
    auto out_val = EncodeUint64(id);
    auto& o_tr = txn->GetOrCreateTreeRoot(Transaction::TREE_OUT_EDGE, out_edge_tree_->GetCommittedRoot());
    PageId o_new_root;
    out_edge_tree_->Insert(out_key, out_val, write_ver, o_tr.cow_root, o_new_root, dirty);
    o_tr.cow_root = o_new_root;
    
    auto in_key = KeyEncoder::InEdgeKey(dst, id);
    auto in_val = EncodeUint64(id);
    auto& i_tr = txn->GetOrCreateTreeRoot(Transaction::TREE_IN_EDGE, in_edge_tree_->GetCommittedRoot());
    PageId i_new_root;
    in_edge_tree_->Insert(in_key, in_val, write_ver, i_tr.cow_root, i_new_root, dirty);
    i_tr.cow_root = i_new_root;
    
    TrackWrite(txn, edge_key, std::nullopt, Transaction::TREE_EDGE);
    txn->dirty_pages.insert(txn->dirty_pages.end(), dirty.begin(), dirty.end());
    ResetSearchRoots();
    return id;
}

GraphStore::BulkInsertOutput GraphStore::BulkInsert(TxId tx,
    const std::vector<BulkVertexInput>& vertices,
    const std::vector<BulkEdgeInput>& edges) {
    auto* txn = GetTransaction(tx);
    if (!txn) return {};
    TxId write_ver = txn->write_ver;
    SetSearchRoots(txn);
    
    BulkInsertOutput result;
    
    VertexId base_vid = next_vertex_id_.fetch_add(
        static_cast<VertexId>(vertices.size()), std::memory_order_relaxed);
    result.vertex_ids.reserve(vertices.size());
    for (size_t i = 0; i < vertices.size(); i++)
        result.vertex_ids.push_back(base_vid + static_cast<VertexId>(i));
    
    EdgeId base_eid = next_edge_id_.fetch_add(
        static_cast<EdgeId>(edges.size()), std::memory_order_relaxed);
    result.edge_ids.reserve(edges.size());
    for (size_t i = 0; i < edges.size(); i++)
        result.edge_ids.push_back(base_eid + static_cast<EdgeId>(i));
    
    auto& v_tr = txn->GetOrCreateTreeRoot(Transaction::TREE_VERTEX, vertex_tree_->GetCommittedRoot());
    auto& l_tr = txn->GetOrCreateTreeRoot(Transaction::TREE_LABEL, label_tree_->GetCommittedRoot());
    auto& pi_tr = txn->GetOrCreateTreeRoot(Transaction::TREE_PROP_INDEX, prop_index_tree_->GetCommittedRoot());
    
    for (size_t i = 0; i < vertices.size(); i++) {
        VertexId id = base_vid + static_cast<VertexId>(i);
        const auto& input = vertices[i];
        
        VertexPayload payload{};
        payload.id = id;
        payload.label = input.label;
        payload.create_ver = write_ver;
        payload.delete_ver = 0;
        payload.first_prop_page = INVALID_PAGE;
        payload.out_degree = 0;
        payload.in_degree = 0;
        payload.flags = 0;
        
        std::vector<PageId> dirty;
        
        if (!input.props.data.empty()) {
            StoreProperties(tx, id, input.props);
            for (const auto& [k, v] : input.props.data) {
                auto idx_key = KeyEncoder::PropIndexKey(input.label, k, v, id);
                PageId pi_new_root;
                prop_index_tree_->Insert(idx_key, {}, write_ver, pi_tr.cow_root, pi_new_root, dirty);
                pi_tr.cow_root = pi_new_root;
            }
            txn->dirty_pages.insert(txn->dirty_pages.end(), dirty.begin(), dirty.end());
            dirty.clear();
        }
        
        auto vkey = KeyEncoder::VertexKey(id);
        PageId v_new_root;
        vertex_tree_->Insert(vkey, payload.Serialize(), write_ver, v_tr.cow_root, v_new_root, dirty);
        v_tr.cow_root = v_new_root;
        txn->dirty_pages.insert(txn->dirty_pages.end(), dirty.begin(), dirty.end());
        dirty.clear();
        
        auto lkey = KeyEncoder::LabelKey(input.label, id);
        PageId l_new_root;
        label_tree_->Insert(lkey, {}, write_ver, l_tr.cow_root, l_new_root, dirty);
        l_tr.cow_root = l_new_root;
        txn->dirty_pages.insert(txn->dirty_pages.end(), dirty.begin(), dirty.end());
        TrackWrite(txn, vkey, std::nullopt, Transaction::TREE_VERTEX);
    }
    
    // Re-fetch search roots to include vertices just inserted above
    SetSearchRoots(txn);
    
    auto& e_tr = txn->GetOrCreateTreeRoot(Transaction::TREE_EDGE, edge_tree_->GetCommittedRoot());
    auto& o_tr = txn->GetOrCreateTreeRoot(Transaction::TREE_OUT_EDGE, out_edge_tree_->GetCommittedRoot());
    auto& i_tr = txn->GetOrCreateTreeRoot(Transaction::TREE_IN_EDGE, in_edge_tree_->GetCommittedRoot());
    
    for (size_t i = 0; i < edges.size(); i++) {
        EdgeId id = base_eid + static_cast<EdgeId>(i);
        const auto& input = edges[i];
        
        auto src_v = vertex_tree_->Get(KeyEncoder::VertexKey(input.src), write_ver);
        auto dst_v = vertex_tree_->Get(KeyEncoder::VertexKey(input.dst), write_ver);
        if (!src_v || !dst_v) { ResetSearchRoots(); return {}; }
        
        EdgePayload payload{};
        payload.id = id;
        payload.src = input.src;
        payload.dst = input.dst;
        payload.label = input.label;
        payload.create_ver = write_ver;
        payload.delete_ver = 0;
        payload.first_prop_page = INVALID_PAGE;
        payload.flags = 0;
        
        std::vector<PageId> dirty;
        
        if (!input.props.data.empty()) {
            StoreProperties(tx, static_cast<uint64_t>(id) | (1ULL << 63), input.props);
        }
        
        PageId e_new_root;
        edge_tree_->Insert(KeyEncoder::EdgeKey(id), payload.Serialize(), write_ver, e_tr.cow_root, e_new_root, dirty);
        e_tr.cow_root = e_new_root;
        PageId o_new_root;
        out_edge_tree_->Insert(KeyEncoder::OutEdgeKey(input.src, id),
                                EncodeUint64(id), write_ver, o_tr.cow_root, o_new_root, dirty);
        o_tr.cow_root = o_new_root;
        PageId i_new_root;
        in_edge_tree_->Insert(KeyEncoder::InEdgeKey(input.dst, id),
                              EncodeUint64(id), write_ver, i_tr.cow_root, i_new_root, dirty);
        i_tr.cow_root = i_new_root;
        
        txn->dirty_pages.insert(txn->dirty_pages.end(), dirty.begin(), dirty.end());
        TrackWrite(txn, KeyEncoder::EdgeKey(id), std::nullopt, Transaction::TREE_EDGE);
    }
    
    ResetSearchRoots();
    return result;
}

GraphStore::BulkInsertOutput GraphStore::ImportCSV(TxId tx,
    const std::string& vertices_csv,
    const std::string& edges_csv) {
    BulkInsertOutput result;
    std::vector<BulkVertexInput> vertices;
    std::vector<BulkEdgeInput> edges;
    
    auto parse_props = [](const std::string& part) -> Properties {
        Properties p;
        if (part.empty()) return p;
        size_t eq = part.find('=');
        if (eq != std::string::npos) {
            p.Set(part.substr(0, eq), part.substr(eq + 1));
        }
        return p;
    };
    
    auto split_line = [](const std::string& line) -> std::vector<std::string> {
        std::vector<std::string> cols;
        size_t start = 0, end;
        while ((end = line.find(',', start)) != std::string::npos) {
            cols.push_back(line.substr(start, end - start));
            start = end + 1;
        }
        cols.push_back(line.substr(start));
        return cols;
    };
    
    if (!vertices_csv.empty()) {
        std::ifstream vf(vertices_csv);
        std::string line;
        bool header = true;
        while (std::getline(vf, line)) {
            if (line.empty()) continue;
            if (header) { header = false; continue; }
            auto cols = split_line(line);
            if (cols.size() < 2) continue;
            BulkVertexInput v;
            v.label = static_cast<LabelId>(std::stoul(cols[1]));
            if (cols.size() >= 3) {
                size_t start = 0, end;
                const std::string& props_str = cols[2];
                while ((end = props_str.find('|', start)) != std::string::npos) {
                    auto p = parse_props(props_str.substr(start, end - start));
                    for (auto& [k, val] : p.data) v.props.Set(k, val);
                    start = end + 1;
                }
                if (start < props_str.size()) {
                    auto p = parse_props(props_str.substr(start));
                    for (auto& [k, val] : p.data) v.props.Set(k, val);
                }
            }
            vertices.push_back(std::move(v));
        }
    }
    
    if (!edges_csv.empty()) {
        std::ifstream ef(edges_csv);
        std::string line;
        bool header = true;
        while (std::getline(ef, line)) {
            if (line.empty()) continue;
            if (header) { header = false; continue; }
            auto cols = split_line(line);
            if (cols.size() < 4) continue;
            BulkEdgeInput e;
            e.src = static_cast<VertexId>(std::stoul(cols[1]));
            e.dst = static_cast<VertexId>(std::stoul(cols[2]));
            e.label = static_cast<LabelId>(std::stoul(cols[3]));
            if (cols.size() >= 5) {
                size_t start = 0, end;
                const std::string& props_str = cols[4];
                while ((end = props_str.find('|', start)) != std::string::npos) {
                    auto p = parse_props(props_str.substr(start, end - start));
                    for (auto& [k, val] : p.data) e.props.Set(k, val);
                    start = end + 1;
                }
                if (start < props_str.size()) {
                    auto p = parse_props(props_str.substr(start));
                    for (auto& [k, val] : p.data) e.props.Set(k, val);
                }
            }
            edges.push_back(std::move(e));
        }
    }
    
    result = BulkInsert(tx, vertices, edges);
    return result;
}

bool GraphStore::RemoveEdge(TxId tx, EdgeId id) {
    auto* txn = GetTransaction(tx);
    if (!txn) return false;
    TxId write_ver = txn->write_ver;
    SetSearchRoots(txn);
    
    auto key = KeyEncoder::EdgeKey(id);
    
    auto val = edge_tree_->Get(key, write_ver);
    if (!val) { ResetSearchRoots(); return false; }
    
    auto payload = EdgePayload::Deserialize(*val);
    if (!IsVisible(payload, write_ver)) return false;
    
    std::vector<PageId> dirty;
    auto& e_tr = txn->GetOrCreateTreeRoot(Transaction::TREE_EDGE, edge_tree_->GetCommittedRoot());
    PageId e_new_root;
    edge_tree_->Remove(key, write_ver, e_tr.cow_root, e_new_root, dirty);
    e_tr.cow_root = e_new_root;
    
    auto& o_tr = txn->GetOrCreateTreeRoot(Transaction::TREE_OUT_EDGE, out_edge_tree_->GetCommittedRoot());
    PageId o_new_root;
    out_edge_tree_->Remove(KeyEncoder::OutEdgeKey(payload.src, id), write_ver, o_tr.cow_root, o_new_root, dirty);
    o_tr.cow_root = o_new_root;
    
    auto& i_tr = txn->GetOrCreateTreeRoot(Transaction::TREE_IN_EDGE, in_edge_tree_->GetCommittedRoot());
    PageId i_new_root;
    in_edge_tree_->Remove(KeyEncoder::InEdgeKey(payload.dst, id), write_ver, i_tr.cow_root, i_new_root, dirty);
    i_tr.cow_root = i_new_root;
    
    auto old_edge = edge_tree_->Get(key, txn->read_ts);
    TrackWrite(txn, key, old_edge, Transaction::TREE_EDGE);
    txn->dirty_pages.insert(txn->dirty_pages.end(), dirty.begin(), dirty.end());
    ResetSearchRoots();
    return true;
}

bool GraphStore::SetEdgeProperty(TxId tx, EdgeId id, const std::string& key, const std::string& val) {
    auto* txn = GetTransaction(tx);
    if (!txn) return false;
    TxId write_ver = txn->write_ver;
    SetSearchRoots(txn);
    
    uint64_t owner_id = static_cast<uint64_t>(id) | (1ULL << 63);
    
    auto old_val_opt = GetEdgeProperty(write_ver, id, key);
    
    auto props = LoadProperties(write_ver, owner_id);
    props.Set(key, val);
    
    auto prop_key = KeyEncoder::EntityPropertyKey(owner_id);
    auto old_prop_val = prop_tree_->Get(prop_key, txn->read_ts);
    Status s = StoreProperties(tx, owner_id, props);
    if (!IsOk(s)) return false;
    
    TrackWrite(txn, prop_key, old_prop_val, Transaction::TREE_PROP);
    
    ResetSearchRoots();
    return true;
}

bool GraphStore::RemoveEdgeProperty(TxId tx, EdgeId id, const std::string& key) {
    auto* txn = GetTransaction(tx);
    if (!txn) return false;
    TxId write_ver = txn->write_ver;
    SetSearchRoots(txn);

    uint64_t owner_id = static_cast<uint64_t>(id) | (1ULL << 63);
    auto props = LoadProperties(write_ver, owner_id);
    props.Remove(key);

    auto prop_key = KeyEncoder::EntityPropertyKey(owner_id);
    auto old_prop_val = prop_tree_->Get(prop_key, txn->read_ts);
    Status s = StoreProperties(tx, owner_id, props);
    if (!IsOk(s)) return false;

    TrackWrite(txn, prop_key, old_prop_val, Transaction::TREE_PROP);
    ResetSearchRoots();
    return true;
}

TxId GraphStore::ResolveReadTs(TxId tx_or_ts) const {
    std::lock_guard<std::mutex> lock(txns_mutex_);
    auto it = active_txns_.find(tx_or_ts);
    if (it != active_txns_.end()) {
        // 写事务返回 max(read_ts, write_ver) 以支持写内读自身
        TxId resolved = it->second->read_ts;
        if (it->second->is_write && it->second->write_ver > resolved)
            resolved = it->second->write_ver;
        return resolved;
    }
    return tx_or_ts;
}

    std::optional<Vertex> GraphStore::GetVertex(TxId tx_or_ts, VertexId id) const {
    TxId resolved_ts = ResolveReadTs(tx_or_ts);
    auto key = KeyEncoder::VertexKey(id);
    auto val = vertex_tree_->Get(key, resolved_ts);
    if (!val) {
        // 即使不存在也记录读取（用于冲突检测）
        auto* non_const_this = const_cast<GraphStore*>(this);
        auto* tx = non_const_this->GetTransaction(tx_or_ts);
        TrackRead(tx, key, std::nullopt);
        return std::nullopt;
    }
    
    auto payload = VertexPayload::Deserialize(*val);
    if (!IsVisible(payload, resolved_ts)) {
        auto* non_const_this = const_cast<GraphStore*>(this);
        auto* tx = non_const_this->GetTransaction(tx_or_ts);
        TrackRead(tx, key, std::nullopt);
        return std::nullopt;
    }
    
    // 跟踪读取
    auto* non_const_this = const_cast<GraphStore*>(this);
    auto* tx = non_const_this->GetTransaction(tx_or_ts);
    TrackRead(tx, key, val);
    
    Vertex v;
    v.id = payload.id;
    v.label = payload.label;
    v.out_degree = CountOutEdges(resolved_ts, id);
    v.in_degree = CountInEdges(resolved_ts, id);
    v.props = LoadProperties(resolved_ts, id);
    
    // Track prop_tree_ read
    auto prop_key = KeyEncoder::EntityPropertyKey(id);
    auto prop_val = prop_tree_->Get(prop_key, resolved_ts);
    TrackRead(tx, prop_key, prop_val, Transaction::TREE_PROP);
    
    // Track degree index range reads
    auto out_start = KeyEncoder::OutEdgeKey(id, 0);
    TrackRead(tx, out_start, std::nullopt, Transaction::TREE_OUT_EDGE);
    auto in_start = KeyEncoder::InEdgeKey(id, 0);
    TrackRead(tx, in_start, std::nullopt, Transaction::TREE_IN_EDGE);
    
    return v;
}

std::optional<Edge> GraphStore::GetEdge(TxId tx_or_ts, EdgeId id) const {
    TxId original = tx_or_ts;
    tx_or_ts = ResolveReadTs(tx_or_ts);
    auto key = KeyEncoder::EdgeKey(id);
    auto val = edge_tree_->Get(key, tx_or_ts);
    if (!val) return std::nullopt;
    
    auto payload = EdgePayload::Deserialize(*val);
    if (!IsVisible(payload, tx_or_ts)) return std::nullopt;
    
    // Track edge_tree_ read
    auto* non_const_this = const_cast<GraphStore*>(this);
    auto* tx = non_const_this->GetTransaction(original);
    TrackRead(tx, key, val, Transaction::TREE_EDGE);
    
    Edge e;
    e.id = payload.id;
    e.src = payload.src;
    e.dst = payload.dst;
    e.label = payload.label;
    e.props = LoadProperties(tx_or_ts, static_cast<uint64_t>(id) | (1ULL << 63));
    
    // Track prop_tree_ read
    auto prop_key = KeyEncoder::EntityPropertyKey(static_cast<uint64_t>(id) | (1ULL << 63));
    auto prop_val = prop_tree_->Get(prop_key, tx_or_ts);
    TrackRead(tx, prop_key, prop_val, Transaction::TREE_PROP);
    
    return e;
}

uint32_t GraphStore::CountOutEdges(TxId read_ts, VertexId id) const {
    read_ts = ResolveReadTs(read_ts);
    auto start = KeyEncoder::OutEdgeKey(id, 0);
    auto end = KeyEncoder::OutEdgeKey(id, std::numeric_limits<EdgeId>::max());
    uint32_t count = 0;
    auto cursor = out_edge_tree_->RangeScan(start, end, read_ts);
    while (cursor.IsValid()) {
        auto key = cursor.Key();
        if (key.size() < sizeof(uint64_t)) break;
        if (std::memcmp(key.data(), start.data(), sizeof(uint64_t)) != 0) break;
        if (!cursor.Value().empty()) count++;
        if (!cursor.Next()) break;
    }
    return count;
}

uint32_t GraphStore::CountInEdges(TxId read_ts, VertexId id) const {
    read_ts = ResolveReadTs(read_ts);
    auto start = KeyEncoder::InEdgeKey(id, 0);
    auto end = KeyEncoder::InEdgeKey(id, std::numeric_limits<EdgeId>::max());
    uint32_t count = 0;
    auto cursor = in_edge_tree_->RangeScan(start, end, read_ts);
    while (cursor.IsValid()) {
        auto key = cursor.Key();
        if (key.size() < sizeof(uint64_t)) break;
        if (std::memcmp(key.data(), start.data(), sizeof(uint64_t)) != 0) break;
        if (!cursor.Value().empty()) count++;
        if (!cursor.Next()) break;
    }
    return count;
}

void GraphStore::SetSearchRoots(Transaction* txn) const {
    if (!txn) return;
    for (auto& [tree_idx, tr] : txn->tree_roots_) {
        auto* tree = const_cast<GraphStore*>(this)->GetTreeByIndex(tree_idx);
        if (tree) tree->SetSearchRoot(tr.cow_root);
    }
}

void GraphStore::ResetSearchRoots() const {
    auto set_invalid = [](auto& tree) { if (tree) tree->ResetSearchRoot(); };
    set_invalid(vertex_tree_);
    set_invalid(edge_tree_);
    set_invalid(out_edge_tree_);
    set_invalid(in_edge_tree_);
    set_invalid(prop_tree_);
    set_invalid(label_tree_);
    set_invalid(prop_index_tree_);
}

void GraphStore::RemoveAllEdges(TxId tx, VertexId id) {
    auto* txn = GetTransaction(tx);
    if (!txn) return;
    TxId write_ver = txn->write_ver;
    
    auto remove_edges_in_range = [&](BPlusTree* tree, BPlusTree* other_tree,
        const std::vector<uint8_t>& start, const std::vector<uint8_t>& end) {
        uint8_t tree_idx = (tree == out_edge_tree_.get()) ? Transaction::TREE_OUT_EDGE : Transaction::TREE_IN_EDGE;
        uint8_t other_idx = (other_tree == out_edge_tree_.get()) ? Transaction::TREE_OUT_EDGE : Transaction::TREE_IN_EDGE;
        auto& e_tr = txn->GetOrCreateTreeRoot(Transaction::TREE_EDGE, edge_tree_->GetCommittedRoot());
        auto& t_tr = txn->GetOrCreateTreeRoot(tree_idx, tree->GetCommittedRoot());
        auto& o_tr = txn->GetOrCreateTreeRoot(other_idx, other_tree->GetCommittedRoot());
        auto cursor = tree->RangeScan(start, end, write_ver);
        while (cursor.IsValid()) {
            auto key = cursor.Key();
            if (key.size() < sizeof(uint64_t)) break;
            if (std::memcmp(key.data(), start.data(), sizeof(uint64_t)) != 0) break;
            auto val = cursor.Value();
            if (!val.empty()) {
                EdgeId eid = DecodeUint64(val);
                
                auto edge_key = KeyEncoder::EdgeKey(eid);
                auto edge_val = edge_tree_->Get(edge_key, write_ver);
                if (edge_val) {
                    auto payload = EdgePayload::Deserialize(*edge_val);
                    if (IsVisible(payload, write_ver)) {
                        std::vector<PageId> dirty;
                        PageId e_new_root;
                        edge_tree_->Remove(edge_key, write_ver, e_tr.cow_root, e_new_root, dirty);
                        e_tr.cow_root = e_new_root;
                        auto old_edge_val = edge_tree_->Get(edge_key, txn->read_ts);
                        TrackWrite(txn, edge_key, old_edge_val, Transaction::TREE_EDGE);
                        
                        auto other_key = (tree == out_edge_tree_.get()) ?
                            KeyEncoder::InEdgeKey(payload.dst, eid) :
                            KeyEncoder::OutEdgeKey(payload.src, eid);
                        PageId o_new_root;
                        other_tree->Remove(other_key, write_ver, o_tr.cow_root, o_new_root, dirty);
                        o_tr.cow_root = o_new_root;
                        
                        txn->dirty_pages.insert(txn->dirty_pages.end(), dirty.begin(), dirty.end());
                    }
                }
                
                std::vector<PageId> dirty;
                PageId t_new_root;
                tree->Remove(key, write_ver, t_tr.cow_root, t_new_root, dirty);
                t_tr.cow_root = t_new_root;
                txn->dirty_pages.insert(txn->dirty_pages.end(), dirty.begin(), dirty.end());
            }
            if (!cursor.Next()) break;
        }
    };
    
    {
        auto start = KeyEncoder::OutEdgeKey(id, 0);
        auto end = KeyEncoder::OutEdgeKey(id, std::numeric_limits<EdgeId>::max());
        remove_edges_in_range(out_edge_tree_.get(), in_edge_tree_.get(), start, end);
    }
    {
        auto start = KeyEncoder::InEdgeKey(id, 0);
        auto end = KeyEncoder::InEdgeKey(id, std::numeric_limits<EdgeId>::max());
        remove_edges_in_range(in_edge_tree_.get(), out_edge_tree_.get(), start, end);
    }
}

std::vector<EdgeId> GraphStore::GetOutEdges(TxId read_ts, VertexId src) const {
    read_ts = ResolveReadTs(read_ts);
    std::vector<EdgeId> result;
    
    auto start = KeyEncoder::OutEdgeKey(src, 0);
    auto end = KeyEncoder::OutEdgeKey(src, std::numeric_limits<EdgeId>::max());
    
    auto cursor = out_edge_tree_->RangeScan(start, end, read_ts);
    while (cursor.IsValid()) {
        auto key = cursor.Key();
        if (key.size() < sizeof(uint64_t)) break;
        if (std::memcmp(key.data(), start.data(), sizeof(uint64_t)) != 0) break;
        auto val = cursor.Value();
        if (!val.empty()) {
            EdgeId eid = DecodeUint64(val);
            auto edge = GetEdge(read_ts, eid);
            if (edge) result.push_back(eid);
        }
        if (!cursor.Next()) break;
    }
    
    return result;
}

std::vector<EdgeId> GraphStore::GetInEdges(TxId read_ts, VertexId dst) const {
    read_ts = ResolveReadTs(read_ts);
    std::vector<EdgeId> result;
    
    auto start = KeyEncoder::InEdgeKey(dst, 0);
    auto end = KeyEncoder::InEdgeKey(dst, std::numeric_limits<EdgeId>::max());
    
    auto cursor = in_edge_tree_->RangeScan(start, end, read_ts);
    while (cursor.IsValid()) {
        auto key = cursor.Key();
        if (key.size() < sizeof(uint64_t)) break;
        if (std::memcmp(key.data(), start.data(), sizeof(uint64_t)) != 0) break;
        auto val = cursor.Value();
        if (!val.empty()) {
            EdgeId eid = DecodeUint64(val);
            auto edge = GetEdge(read_ts, eid);
            if (edge) result.push_back(eid);
        }
        if (!cursor.Next()) break;
    }
    
    return result;
}

std::vector<VertexId> GraphStore::GetAllVertices(TxId read_ts) const {
    read_ts = ResolveReadTs(read_ts);
    std::vector<VertexId> result;
    std::vector<uint8_t> empty;
    auto cursor = vertex_tree_->PrefixScan(empty, read_ts);
    while (cursor.IsValid()) {
        auto key = cursor.Key();
        if (key.size() >= sizeof(uint64_t)) {
            VertexId vid = DecodeUint64(std::vector<uint8_t>(key.begin(), key.end()));
            auto v = GetVertex(read_ts, vid);
            if (v) result.push_back(vid);
        }
        if (!cursor.Next()) break;
    }
    return result;
}

std::vector<VertexId> GraphStore::GetVerticesByLabel(TxId read_ts, LabelId label) const {
    read_ts = ResolveReadTs(read_ts);
    std::vector<VertexId> result;
    
    auto start = KeyEncoder::LabelKey(label, 0);
    auto end = KeyEncoder::LabelKey(label, std::numeric_limits<uint64_t>::max());
    
    auto cursor = label_tree_->RangeScan(start, end, read_ts);
    while (cursor.IsValid()) {
        auto key = cursor.Key();
        if (key.size() < sizeof(uint64_t)) break;
        if (std::memcmp(key.data(), start.data(), sizeof(uint64_t)) != 0) break;
        if (key.size() >= sizeof(uint64_t) * 2) {
            // 检查 label key 未被删除（MVCC）
            if (!label_tree_->Get(key, read_ts).has_value()) {
                if (!cursor.Next()) break;
                continue;
            }
            VertexId vid = DecodeUint64(std::vector<uint8_t>(key.begin() + sizeof(uint64_t), key.end()));
            auto v = GetVertex(read_ts, vid);
            if (v) result.push_back(vid);
        }
        if (!cursor.Next()) break;
    }
    
    return result;
}

std::optional<std::string> GraphStore::GetVertexProperty(TxId read_ts, VertexId id, const std::string& key) const {
    read_ts = ResolveReadTs(read_ts);
    auto props = LoadProperties(read_ts, id);
    auto it = props.data.find(key);
    if (it == props.data.end()) return std::nullopt;
    return it->second;
}

std::optional<std::string> GraphStore::GetEdgeProperty(TxId read_ts, EdgeId id, const std::string& key) const {
    read_ts = ResolveReadTs(read_ts);
    auto props = LoadProperties(read_ts, static_cast<uint64_t>(id) | (1ULL << 63));
    auto it = props.data.find(key);
    if (it == props.data.end()) return std::nullopt;
    return it->second;
}

std::vector<VertexId> GraphStore::GetVerticesByProperty(TxId read_ts, LabelId label,
                                                         const std::string& key,
                                                         const std::string& value) const {
    read_ts = ResolveReadTs(read_ts);
    std::vector<VertexId> result;
    if (!prop_index_tree_) return result;
    
    auto prefix = KeyEncoder::PropIndexExactPrefix(label, key, value);
    auto cursor = prop_index_tree_->PrefixScan(prefix, read_ts);
    
    // 属性索引键格式: [label:8][key\0][value\0][entity_id:8]
    size_t label_key_value_size = prefix.size();
    
    while (cursor.IsValid()) {
        auto k = cursor.Key();
        if (k.size() < label_key_value_size + sizeof(uint64_t)) break;
        if (std::memcmp(k.data(), prefix.data(), label_key_value_size) != 0) break;
        
        // 解码 entity_id（从键末尾8字节）
        std::vector<uint8_t> eid_buf(k.end() - sizeof(uint64_t), k.end());
        VertexId vid = DecodeUint64(eid_buf);
        
        // 验证顶点仍然可见
        auto v = GetVertex(read_ts, vid);
        if (v) result.push_back(vid);
        
        if (!cursor.Next()) break;
    }
    
    return result;
}

size_t GraphStore::GetVertexCount(TxId read_ts) const {
    read_ts = ResolveReadTs(read_ts);
    size_t count = 0;
    std::vector<uint8_t> empty;
    
    auto cursor = vertex_tree_->PrefixScan(empty, read_ts);
    while (cursor.IsValid()) {
        auto key = cursor.Key();
        if (!key.empty()) {
            auto val = vertex_tree_->Get(key, read_ts);
            if (val) {
                auto payload = VertexPayload::Deserialize(*val);
                if (IsVisible(payload, read_ts)) count++;
            }
        }
        if (!cursor.Next()) break;
    }
    return count;
}

size_t GraphStore::GetEdgeCount(TxId read_ts) const {
    read_ts = ResolveReadTs(read_ts);
    size_t count = 0;
    std::vector<uint8_t> empty;
    auto cursor = edge_tree_->PrefixScan(empty, read_ts);
    while (cursor.IsValid()) {
        auto key = cursor.Key();
        if (!key.empty()) {
            auto val = edge_tree_->Get(key, read_ts);
            if (val) {
                auto payload = EdgePayload::Deserialize(*val);
                if (IsVisible(payload, read_ts)) count++;
            }
        }
        if (!cursor.Next()) break;
    }
    return count;
}

Status GraphStore::Checkpoint() {
    return pm_->FlushAll();
}

Status GraphStore::Backup(const std::filesystem::path& backup_path) {
    std::lock_guard<std::mutex> lock(commit_mutex_);
    
    // 刷脏到文件
    auto s = pm_->FlushAll();
    if (!IsOk(s)) return s;
    
    // 复制数据文件
    std::error_code ec;
    std::filesystem::copy(pm_->GetDbPath(), backup_path,
        std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) return Status::IO_ERROR;
    
    return Status::OK;
}

Status GraphStore::Compact() {
    TxId min_read = GetMinReadTs();
    CollectGarbage(min_read);
    return Status::OK;
}

void GraphStore::StartGC() {
    gc_running_ = true;
    gc_thread_ = std::thread([this]() { this->GCLoop(); });
}

void GraphStore::StopGC() {
    gc_running_ = false;
    if (gc_thread_.joinable()) gc_thread_.join();
}

void GraphStore::GCLoop() {
    while (gc_running_) {
        TxId min_read = GetMinReadTs();
        TxId current_commit = global_commit_ts_.load(std::memory_order_acquire);
        
        if (min_read < current_commit) {
            CollectGarbage(min_read);
        }
        
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
}

void GraphStore::CollectGarbage(TxId before_ts) {
    struct TreeJob {
        BPlusTree* tree;
        std::vector<PageId> dirty;
    };
    
    auto compact_tree = [before_ts](TreeJob& job) {
        job.tree->TraverseLeaves([&job, before_ts](PageId pid) {
            job.tree->CompactPage(pid, before_ts, job.dirty);
        });
    };
    
    std::vector<TreeJob> jobs = {
        {vertex_tree_.get()},
        {edge_tree_.get()},
        {out_edge_tree_.get()},
        {in_edge_tree_.get()},
        {label_tree_.get()},
        {prop_tree_.get()},
    };
    if (prop_index_tree_) {
        jobs.push_back({prop_index_tree_.get()});
    }
    
    std::vector<std::future<void>> futures;
    for (auto& job : jobs) {
        futures.push_back(std::async(std::launch::async, compact_tree, std::ref(job)));
    }
    for (auto& f : futures) {
        f.get();
    }
}

Status GraphStore::StoreProperties(TxId tx_id, uint64_t owner_id, const Properties& props) {
    auto* txn = GetTransaction(tx_id);
    if (!txn) return Status::INVALID_ARGUMENT;
    TxId write_ver = txn->write_ver;
    std::vector<PageId> dirty;
    auto& p_tr = txn->GetOrCreateTreeRoot(Transaction::TREE_PROP, prop_tree_->GetCommittedRoot());
    
    auto key = KeyEncoder::EntityPropertyKey(owner_id);
    auto val = props.Serialize();
    PageId p_new_root;
    Status s = prop_tree_->Insert(key, val, write_ver, p_tr.cow_root, p_new_root, dirty);
    p_tr.cow_root = p_new_root;
    
    txn->dirty_pages.insert(txn->dirty_pages.end(), dirty.begin(), dirty.end());
    return Status::OK;
}

Properties GraphStore::LoadProperties(TxId read_ts, uint64_t owner_id) const {
    auto key = KeyEncoder::EntityPropertyKey(owner_id);
    auto val = prop_tree_->Get(key, read_ts);
    if (!val) return {};
    return Properties::Deserialize(*val);
}

bool GraphStore::IsVisible(const VertexPayload& payload, TxId read_ts) const {
    return true;
}

bool GraphStore::IsVisible(const EdgePayload& payload, TxId read_ts) const {
    return true;
}

} // namespace graphstore

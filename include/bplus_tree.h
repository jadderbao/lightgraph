#ifndef BPLUS_TREE_H
#define BPLUS_TREE_H

#include "common.h"
#include "page_manager.h"
#include <functional>

namespace graphstore {

using KeyCompare = std::function<int(const std::vector<uint8_t>&, const std::vector<uint8_t>&)>;

// v3: 溢出页头（放在PageHeader之后）
struct OverflowHeader {
    PageId   next_overflow;
    uint32_t data_len;      // 本页数据长度
    uint32_t total_len;     // 链总长度
    uint16_t data_offset;   // 数据在页内的偏移
    static constexpr size_t SIZE = 14;
};

// v3: 版本节点支持溢出页链
struct VersionNodeEx {
    TxId     version;
    bool     is_delete;
    uint8_t  pad[3];
    uint32_t data_len;      // 总数据长度
    PageId   first_overflow; // v3: 溢出页链首页（0=内联）
    uint16_t older_offset;
    static constexpr size_t HEADER_SIZE = 24;
};

static_assert(sizeof(VersionNodeEx) == VersionNodeEx::HEADER_SIZE, "VersionNodeEx size");

class BTreeCursor {
public:
    BTreeCursor(PageManager* pm, PageId leaf_page, uint16_t slot_idx, TxId read_ts);
    bool IsValid() const;
    bool Next();
    std::vector<uint8_t> Key() const;
    std::vector<uint8_t> Value() const;
    
private:
    PageManager* pm_;
    PageId       current_page_;
    uint16_t     slot_idx_;
    TxId         read_ts_;
};

class BPlusTree {
    friend class BTreeCursor;    
public:
    BPlusTree(PageManager* pm, PageId root_page, KeyCompare cmp);
    BPlusTree(PageManager* pm, KeyCompare cmp);
    
    Status Insert(const std::vector<uint8_t>& key, 
                  const std::vector<uint8_t>& value, 
                  TxId tx_id,
                  PageId base_root,
                  PageId& new_root,
                  std::vector<PageId>& dirty_pages);
    Status Insert(const std::vector<uint8_t>& key, 
                  const std::vector<uint8_t>& value, 
                  TxId tx_id,
                  std::vector<PageId>& dirty_pages) {
        PageId base = GetCommittedRoot(), new_root;
        Status s = Insert(key, value, tx_id, base, new_root, dirty_pages);
        if (IsOk(s)) SetRootForInit(new_root);
        return s;
    }
    Status Remove(const std::vector<uint8_t>& key, 
                  TxId tx_id,
                  PageId base_root,
                  PageId& new_root,
                  std::vector<PageId>& dirty_pages);
    Status Remove(const std::vector<uint8_t>& key, 
                  TxId tx_id,
                  std::vector<PageId>& dirty_pages) {
        PageId base = GetCommittedRoot(), new_root;
        Status s = Remove(key, tx_id, base, new_root, dirty_pages);
        if (IsOk(s)) SetRootForInit(new_root);
        return s;
    }
    std::optional<std::vector<uint8_t>> Get(
        const std::vector<uint8_t>& key, 
        TxId read_ts) const;
    std::optional<std::vector<uint8_t>> Get(
        const std::vector<uint8_t>& key,
        TxId read_ts,
        PageId search_root) const;
    
    BTreeCursor RangeScan(
        const std::vector<uint8_t>& start_key,
        const std::vector<uint8_t>& end_key,
        TxId read_ts) const;
    
    // v3: 前缀扫描（用于属性树）
    BTreeCursor PrefixScan(
        const std::vector<uint8_t>& prefix,
        TxId read_ts) const;
    
    PageId GetCommittedRoot() const { 
        PageId sr = search_root_.load(std::memory_order_acquire);
        return (sr != INVALID_PAGE) ? sr : root_page_.load(std::memory_order_acquire);
    }
    PageId GetRootPage() const { return root_page_.load(std::memory_order_acquire); }
    void SetRootForInit(PageId root) { root_page_.store(root, std::memory_order_release); }
    bool PublishRoot(PageId expected, PageId desired) {
        return root_page_.compare_exchange_strong(expected, desired,
            std::memory_order_acq_rel, std::memory_order_acquire);
    }
    void SetSearchRoot(PageId root) { search_root_.store(root, std::memory_order_release); }
    void ResetSearchRoot() { search_root_.store(INVALID_PAGE, std::memory_order_release); }
    PageId GetSearchRoot() const { return search_root_.load(std::memory_order_acquire); }
    void PatchVersion(PageId pid, TxId old_ver, TxId new_ver);
    
    void TraverseLeaves(std::function<void(PageId)> callback) const;
    
    // v3: GC - 压缩页内空间，回收旧版本
    Status CompactPage(PageId leaf_id, TxId min_read_ts, std::vector<PageId>& dirty);
    void TryMergeLeaf(PageId leaf_id, TxId min_read_ts, std::vector<PageId>& dirty);
    size_t GetPageFragmentation(PageId leaf_id) const;
    
private:
    PageManager* pm_;
    mutable std::atomic<PageId> root_page_{INVALID_PAGE};
    mutable std::atomic<PageId> search_root_{INVALID_PAGE};
    KeyCompare   cmp_;
    
    struct Slot {
        uint16_t key_offset;
        uint16_t key_len;
        uint16_t val_offset;
        uint16_t val_len;
    };
    static constexpr size_t SLOT_SIZE = 8;
    static constexpr size_t BTREE_ORDER = 160;
    
    PageId FindLeaf(const std::vector<uint8_t>& key) const;
    PageId FindLeaf(const std::vector<uint8_t>& key, std::vector<PageId>& path) const;
    PageId FindLeaf(const std::vector<uint8_t>& key, std::vector<PageId>& path, PageId root) const;
    int    SearchInLeaf(const Page* page, const std::vector<uint8_t>& key) const;
    int    SearchInBranch(const Page* page, const std::vector<uint8_t>& key) const;
    
    const VersionNodeEx* FindVisibleVersion(const Page* page, uint16_t val_offset, TxId read_ts) const;
    std::vector<uint8_t> ReadVersionData(const Page* page, const VersionNodeEx* node) const;
    
    Status DoInsertIntoLeaf(PageId leaf_id, const std::vector<uint8_t>& key,
                            const std::vector<uint8_t>& value, TxId tx_id,
                            std::vector<PageId>& dirty_pages);
    Status DoRemoveFromLeaf(PageId leaf_id, int slot_idx, TxId tx_id);
    Status SplitLeaf(PageId leaf_id, std::vector<PageId>& dirty_pages,
                     std::vector<uint8_t>& out_split_key, PageId& out_right_page);
    Status InsertIntoParent(const std::vector<PageId>& path, 
                            std::vector<uint8_t> split_key,
                            PageId left_child, PageId right_child,
                            PageId& current_root,
                            std::vector<PageId>& dirty_pages);
    Status SplitBranch(PageId branch_id, std::vector<PageId>& dirty_pages,
                       const std::vector<uint8_t>& split_key,
                       PageId left_child, PageId right_child,
                       std::vector<uint8_t>& out_mid_key,
                       PageId& out_right_sibling);
    
    Slot* GetSlotArray(Page* page);
    const Slot* GetSlotArray(const Page* page) const;
    std::vector<uint8_t> GetKeyFromSlot(const Page* page, const Slot& slot) const;
    PageId CopyPage(PageId old_id, std::vector<PageId>& dirty_pages);
    void RedirectChildParent(PageId child_id, PageId new_child,
                              const std::vector<uint8_t>& search_key,
                              std::vector<PageId>& dirty);
    
};

} // namespace graphstore
#endif // BPLUS_TREE_H

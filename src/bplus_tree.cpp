#include "bplus_tree.h"
#include <iostream>
#include <cstring>
#include <limits>

namespace graphstore {

BTreeCursor::BTreeCursor(PageManager* pm, PageId leaf_page, uint16_t slot_idx, TxId read_ts)
    : pm_(pm), current_page_(leaf_page), slot_idx_(slot_idx), read_ts_(read_ts) {}

bool BTreeCursor::IsValid() const {
    return current_page_ != INVALID_PAGE && pm_ != nullptr;
}

bool BTreeCursor::Next() {
    if (!IsValid()) return false;
    slot_idx_++;
    auto* page = pm_->GetPage(current_page_);
    if (!page) { current_page_ = INVALID_PAGE; return false; }
    if (slot_idx_ >= page->Header()->num_keys) {
        PageId next = page->Header()->next_sibling;
        if (next == INVALID_PAGE || next == 0) {
            current_page_ = INVALID_PAGE;
            return false;
        }
        current_page_ = next;
        slot_idx_ = 0;
    }
    return true;
}

std::vector<uint8_t> BTreeCursor::Key() const {
    if (!IsValid()) return {};
    auto* page = pm_->GetPage(current_page_);
    if (!page) return {};
    auto* slots = reinterpret_cast<const BPlusTree::Slot*>(page->Body());
    if (slot_idx_ >= page->Header()->num_keys) return {};
    const auto& slot = slots[slot_idx_];
    return std::vector<uint8_t>(page->data + slot.key_offset, 
                                 page->data + slot.key_offset + slot.key_len);
}

std::vector<uint8_t> BTreeCursor::Value() const {
    if (!IsValid()) return {};
    auto* page = pm_->GetPage(current_page_);
    if (!page) return {};
    auto* slots = reinterpret_cast<const BPlusTree::Slot*>(page->Body());
    if (slot_idx_ >= page->Header()->num_keys) return {};
    const auto& slot = slots[slot_idx_];
    auto* vnode = reinterpret_cast<const VersionNodeEx*>(page->data + slot.val_offset);
    
    if (vnode->first_overflow != 0 && vnode->first_overflow != INVALID_PAGE) {
        return pm_->ReadOverflowChain(vnode->first_overflow);
    }
    return std::vector<uint8_t>(page->data + slot.val_offset + VersionNodeEx::HEADER_SIZE,
                                 page->data + slot.val_offset + VersionNodeEx::HEADER_SIZE + vnode->data_len);
}

BPlusTree::BPlusTree(PageManager* pm, PageId root_page, KeyCompare cmp)
    : pm_(pm), cmp_(cmp) {
    root_page_.store(root_page, std::memory_order_relaxed);
}

BPlusTree::BPlusTree(PageManager* pm, KeyCompare cmp)
    : pm_(pm), cmp_(cmp) {
    PageId rp = pm->AllocatePage(PageType::BTREE_LEAF);
    root_page_.store(rp, std::memory_order_release);
    auto* root = pm->GetPage(rp);
    root->Header()->num_keys = 0;
    root->Header()->next_sibling = INVALID_PAGE;
    root->Header()->prev_sibling = INVALID_PAGE;
}

Status BPlusTree::Insert(const std::vector<uint8_t>& key,
                         const std::vector<uint8_t>& value,
                         TxId tx_id,
                         PageId base_root,
                         PageId& new_root,
                         std::vector<PageId>& dirty_pages) {
    PageId current_root = base_root;
    
    if (current_root == INVALID_PAGE) {
        current_root = pm_->AllocatePage(PageType::BTREE_LEAF);
        dirty_pages.push_back(current_root);
        auto* r = pm_->GetPage(current_root);
        r->Header()->num_keys = 0;
        r->Header()->next_sibling = INVALID_PAGE;
        r->Header()->prev_sibling = INVALID_PAGE;
    }
    
    std::vector<PageId> path;
    PageId leaf_id = FindLeaf(key, path, current_root);
    if (leaf_id == INVALID_PAGE) return Status::ERROR;
    
    // 读取原始叶子的兄弟链接（在 CopyPage 之前）
    auto* old_lf_page = pm_->GetPage(leaf_id);
    PageId old_prev = old_lf_page->Header()->prev_sibling;
    PageId old_next = old_lf_page->Header()->next_sibling;
    
    // COW: 复制叶子页
    PageId cow_leaf = CopyPage(leaf_id, dirty_pages);
    
    // COW 相邻叶子的兄弟链，确保游标遍历时能找到所有 COW 副本
    PageId cow_prev = old_prev;
    PageId cow_next = old_next;
    
    if (old_prev != INVALID_PAGE) {
        bool is_dirty = false;
        for (auto d : dirty_pages) { if (d == old_prev) { is_dirty = true; break; } }
        if (!is_dirty) cow_prev = CopyPage(old_prev, dirty_pages);
        auto* cp = pm_->GetPage(cow_prev);
        cp->Header()->next_sibling = cow_leaf;
        auto* cl = pm_->GetPage(cow_leaf);
        cl->Header()->prev_sibling = cow_prev;
    }
    
    if (old_next != INVALID_PAGE) {
        bool is_dirty = false;
        for (auto d : dirty_pages) { if (d == old_next) { is_dirty = true; break; } }
        if (!is_dirty) cow_next = CopyPage(old_next, dirty_pages);
        auto* cp = pm_->GetPage(cow_next);
        cp->Header()->prev_sibling = cow_leaf;
        auto* cl = pm_->GetPage(cow_leaf);
        cl->Header()->next_sibling = cow_next;
    }
    
    // COW 祖先链：沿路径向上复制分支页，并更新子指针
    if (path.size() > 1) {
        PageId current_child = cow_leaf;
        for (int i = static_cast<int>(path.size()) - 2; i >= 0; --i) {
            PageId old_branch = path[i];
            bool already_cow = false;
            for (auto d : dirty_pages) { if (d == old_branch) { already_cow = true; break; } }
            PageId cow_branch = already_cow ? old_branch : CopyPage(old_branch, dirty_pages);
            if (i == 0) current_root = cow_branch;
            
            auto* bp = pm_->GetPage(cow_branch);
            auto* bh = bp->Header();
            auto* bslots = reinterpret_cast<Slot*>(bp->Body());
            for (int s = 0; s < bh->num_keys; ++s) {
                auto* cp = reinterpret_cast<PageId*>(bp->data + bslots[s].val_offset);
                if (cp[0] == path[i + 1]) cp[0] = current_child;
                if (cp[1] == path[i + 1]) cp[1] = current_child;
                if (i == static_cast<int>(path.size()) - 2) {
                    if (old_prev != INVALID_PAGE && cow_prev != old_prev) {
                        if (cp[0] == old_prev) cp[0] = cow_prev;
                        if (cp[1] == old_prev) cp[1] = cow_prev;
                    }
                    if (old_next != INVALID_PAGE && cow_next != old_next) {
                        if (cp[0] == old_next) cp[0] = cow_next;
                        if (cp[1] == old_next) cp[1] = cow_next;
                    }
                }
            }
            
            path[i] = cow_branch;
            current_child = cow_branch;
        }
    }
    
    // 尝试插入
    Status s = DoInsertIntoLeaf(cow_leaf, key, value, tx_id, dirty_pages);
    if (IsOk(s)) {
        if (path.size() == 1) current_root = cow_leaf;
        new_root = current_root;
        return Status::OK;
    }
    
    // 页满，分裂叶子
    std::vector<uint8_t> split_key;
    PageId right_page;
    s = SplitLeaf(cow_leaf, dirty_pages, split_key, right_page);
    if (!IsOk(s)) return s;
    
    // 将新键插入到分裂后的正确叶子页
    if (cmp_(key, split_key) < 0) {
        s = DoInsertIntoLeaf(cow_leaf, key, value, tx_id, dirty_pages);
    } else {
        s = DoInsertIntoLeaf(right_page, key, value, tx_id, dirty_pages);
    }
    if (!IsOk(s)) return s;
    
    // 向上传播
    if (path.size() == 1) {
        // 根就是叶子，提升为新分支根
        PageId branch_root = pm_->AllocatePage(PageType::BTREE_BRANCH);
        dirty_pages.push_back(branch_root);
        auto* nr = pm_->GetPage(branch_root);
        nr->Header()->num_keys = 1;
        nr->Header()->free_start = PageHeader::SIZE + nr->Header()->num_keys * static_cast<uint16_t>(SLOT_SIZE);
        
        auto* ns = reinterpret_cast<Slot*>(nr->Body());
        uint16_t koff = nr->Allocate(split_key.size());
        std::memcpy(nr->data + koff, split_key.data(), split_key.size());
        uint16_t voff = nr->Allocate(sizeof(PageId) * 2);
        auto* cp = reinterpret_cast<PageId*>(nr->data + voff);
        cp[0] = cow_leaf;
        cp[1] = right_page;
        ns[0].key_offset = koff;
        ns[0].key_len = static_cast<uint16_t>(split_key.size());
        ns[0].val_offset = voff;
        ns[0].val_len = sizeof(PageId) * 2;
        current_root = branch_root;
    } else {
        // 插入父分支页
        s = InsertIntoParent(path, split_key, cow_leaf, right_page, current_root, dirty_pages);
        if (!IsOk(s)) return s;
    }
    new_root = current_root;
    return Status::OK;
}

Status BPlusTree::Remove(const std::vector<uint8_t>& key,
                         TxId tx_id,
                         PageId base_root,
                         PageId& new_root,
                         std::vector<PageId>& dirty_pages) {
    PageId current_root = base_root;
    if (current_root == INVALID_PAGE) return Status::NOT_FOUND;
    
    std::vector<PageId> path;
    PageId leaf_id = FindLeaf(key, path, current_root);
    if (leaf_id == INVALID_PAGE) return Status::NOT_FOUND;
    
    auto* old_lf_page = pm_->GetPage(leaf_id);
    PageId old_prev = old_lf_page->Header()->prev_sibling;
    PageId old_next = old_lf_page->Header()->next_sibling;
    
    PageId cow_leaf = CopyPage(leaf_id, dirty_pages);
    
    PageId cow_prev = old_prev;
    PageId cow_next = old_next;
    
    if (old_prev != INVALID_PAGE) {
        bool is_dirty = false;
        for (auto d : dirty_pages) { if (d == old_prev) { is_dirty = true; break; } }
        if (!is_dirty) cow_prev = CopyPage(old_prev, dirty_pages);
        auto* cp = pm_->GetPage(cow_prev);
        cp->Header()->next_sibling = cow_leaf;
        auto* cl = pm_->GetPage(cow_leaf);
        cl->Header()->prev_sibling = cow_prev;
    }
    
    if (old_next != INVALID_PAGE) {
        bool is_dirty = false;
        for (auto d : dirty_pages) { if (d == old_next) { is_dirty = true; break; } }
        if (!is_dirty) cow_next = CopyPage(old_next, dirty_pages);
        auto* cp = pm_->GetPage(cow_next);
        cp->Header()->prev_sibling = cow_leaf;
        auto* cl = pm_->GetPage(cow_leaf);
        cl->Header()->next_sibling = cow_next;
    }
    
    if (path.size() > 1) {
        PageId current_child = cow_leaf;
        for (int i = static_cast<int>(path.size()) - 2; i >= 0; --i) {
            PageId old_branch = path[i];
            bool already_cow = false;
            for (auto d : dirty_pages) { if (d == old_branch) { already_cow = true; break; } }
            PageId cow_branch = already_cow ? old_branch : CopyPage(old_branch, dirty_pages);
            if (i == 0) current_root = cow_branch;
            
            auto* bp = pm_->GetPage(cow_branch);
            auto* bh = bp->Header();
            auto* bslots = reinterpret_cast<Slot*>(bp->Body());
            for (int s = 0; s < bh->num_keys; ++s) {
                auto* cp = reinterpret_cast<PageId*>(bp->data + bslots[s].val_offset);
                if (cp[0] == path[i + 1]) cp[0] = current_child;
                if (cp[1] == path[i + 1]) cp[1] = current_child;
                if (i == static_cast<int>(path.size()) - 2) {
                    if (old_prev != INVALID_PAGE && cow_prev != old_prev) {
                        if (cp[0] == old_prev) cp[0] = cow_prev;
                        if (cp[1] == old_prev) cp[1] = cow_prev;
                    }
                    if (old_next != INVALID_PAGE && cow_next != old_next) {
                        if (cp[0] == old_next) cp[0] = cow_next;
                        if (cp[1] == old_next) cp[1] = cow_next;
                    }
                }
            }
            
            current_child = cow_branch;
        }
    }
    
    if (path.size() == 1) current_root = cow_leaf;
    
    auto* page = pm_->GetPage(cow_leaf);
    int idx = SearchInLeaf(page, key);
    if (idx < 0) return Status::NOT_FOUND;
    
    Status s = DoRemoveFromLeaf(cow_leaf, idx, tx_id);
    if (IsOk(s)) {
        new_root = current_root;
        return Status::OK;
    }
    
    // 页满，分裂叶子后重试
    std::vector<uint8_t> split_key;
    PageId right_page;
    s = SplitLeaf(cow_leaf, dirty_pages, split_key, right_page);
    if (!IsOk(s)) return s;
    
    if (cmp_(key, split_key) < 0) {
        auto* left_page_ptr = pm_->GetPage(cow_leaf);
        int left_idx = SearchInLeaf(left_page_ptr, key);
        if (left_idx < 0) return Status::NOT_FOUND;
        s = DoRemoveFromLeaf(cow_leaf, left_idx, tx_id);
    } else {
        auto* right_page_ptr = pm_->GetPage(right_page);
        int right_idx = SearchInLeaf(right_page_ptr, key);
        if (right_idx < 0) return Status::NOT_FOUND;
        s = DoRemoveFromLeaf(right_page, right_idx, tx_id);
    }
    if (!IsOk(s)) return s;
    
    // 向上传播
    if (path.size() == 1) {
        PageId branch_root = pm_->AllocatePage(PageType::BTREE_BRANCH);
        dirty_pages.push_back(branch_root);
        auto* nr = pm_->GetPage(branch_root);
        nr->Header()->num_keys = 1;
        nr->Header()->free_start = PageHeader::SIZE + nr->Header()->num_keys * static_cast<uint16_t>(SLOT_SIZE);
        
        auto* ns = reinterpret_cast<Slot*>(nr->Body());
        uint16_t koff = nr->Allocate(split_key.size());
        std::memcpy(nr->data + koff, split_key.data(), split_key.size());
        uint16_t voff = nr->Allocate(sizeof(PageId) * 2);
        auto* cp = reinterpret_cast<PageId*>(nr->data + voff);
        cp[0] = cow_leaf;
        cp[1] = right_page;
        ns[0].key_offset = koff;
        ns[0].key_len = static_cast<uint16_t>(split_key.size());
        ns[0].val_offset = voff;
        ns[0].val_len = sizeof(PageId) * 2;
        current_root = branch_root;
    } else {
        s = InsertIntoParent(path, split_key, cow_leaf, right_page, current_root, dirty_pages);
        if (!IsOk(s)) return s;
    }
    
    new_root = current_root;
    return Status::OK;
}

std::optional<std::vector<uint8_t>> BPlusTree::Get(
    const std::vector<uint8_t>& key, TxId read_ts) const {
    PageId leaf_id = FindLeaf(key);
    if (leaf_id == INVALID_PAGE) return std::nullopt;
    
    auto* page = pm_->GetPage(leaf_id);
    int idx = SearchInLeaf(page, key);
    if (idx < 0) return std::nullopt;
    
    auto* slots = reinterpret_cast<const Slot*>(page->Body());
    auto* vnode = FindVisibleVersion(page, slots[idx].val_offset, read_ts);
    if (!vnode || vnode->is_delete) return std::nullopt;
    return ReadVersionData(page, vnode);
}

std::optional<std::vector<uint8_t>> BPlusTree::Get(
    const std::vector<uint8_t>& key, TxId read_ts, PageId search_root) const {
    if (search_root == INVALID_PAGE)
        return Get(key, read_ts);
    std::vector<PageId> path;
    PageId leaf_id = FindLeaf(key, path, search_root);
    if (leaf_id == INVALID_PAGE) return std::nullopt;
    auto* page = pm_->GetPage(leaf_id);
    int idx = SearchInLeaf(page, key);
    if (idx < 0) return std::nullopt;
    auto* slots = reinterpret_cast<const Slot*>(page->Body());
    auto* vnode = FindVisibleVersion(page, slots[idx].val_offset, read_ts);
    if (!vnode || vnode->is_delete) return std::nullopt;
    return ReadVersionData(page, vnode);
}

BTreeCursor BPlusTree::RangeScan(
    const std::vector<uint8_t>& start_key,
    const std::vector<uint8_t>& /*end_key*/,
    TxId read_ts) const {
    PageId leaf_id = FindLeaf(start_key);
    if (leaf_id == INVALID_PAGE) return BTreeCursor(pm_, INVALID_PAGE, 0, read_ts);
    
    auto* page = pm_->GetPage(leaf_id);
    int idx = SearchInLeaf(page, start_key);
    if (idx < 0) idx = 0;
    
    auto* slots = reinterpret_cast<const Slot*>(page->Body());
    while (idx < page->Header()->num_keys) {
        auto k = GetKeyFromSlot(page, slots[idx]);
        if (cmp_(k, start_key) >= 0) break;
        idx++;
    }
    
    return BTreeCursor(pm_, leaf_id, static_cast<uint16_t>(idx), read_ts);
}

BTreeCursor BPlusTree::PrefixScan(
    const std::vector<uint8_t>& prefix,
    TxId read_ts) const {
    return RangeScan(prefix, std::vector<uint8_t>(prefix.size(), 0xFF), read_ts);
}

void BPlusTree::TraverseLeaves(std::function<void(PageId)> callback) const {
    PageId rp = root_page_.load(std::memory_order_consume);
    if (rp == INVALID_PAGE) return;
    PageId current = rp;
    while (true) {
        auto* page = pm_->GetPage(current);
        if (!page) break;
        if (page->Header()->type == PageType::BTREE_LEAF) break;
        auto* slots = reinterpret_cast<const Slot*>(page->Body());
        if (page->Header()->num_keys == 0) break;
        auto* cp = reinterpret_cast<const PageId*>(page->data + slots[0].val_offset);
        current = cp[0];
    }
    while (current != INVALID_PAGE && current != 0) {
        callback(current);
        auto* page = pm_->GetPage(current);
        if (!page) break;
        current = page->Header()->next_sibling;
    }
}

void BPlusTree::RedirectChildParent(PageId child_id, PageId new_child,
                                     const std::vector<uint8_t>& search_key,
                                     std::vector<PageId>& dirty) {
    std::vector<PageId> path;
    FindLeaf(search_key, path);
    if (path.size() < 2) return;
    PageId parent_id = path[path.size() - 2];
    auto* parent = pm_->GetPage(parent_id);
    if (!parent || parent->Header()->type != PageType::BTREE_BRANCH) return;
    auto* ps = reinterpret_cast<Slot*>(parent->Body());
    for (int i = 0; i < parent->Header()->num_keys; ++i) {
        auto* cp = reinterpret_cast<PageId*>(parent->data + ps[i].val_offset);
        if (cp[0] == child_id) { cp[0] = new_child; dirty.push_back(parent_id); break; }
        if (cp[1] == child_id) { cp[1] = new_child; dirty.push_back(parent_id); break; }
    }
}

Status BPlusTree::CompactPage(PageId leaf_id, TxId min_read_ts, std::vector<PageId>& dirty) {
    auto* page = pm_->GetPage(leaf_id);
    if (!page || page->Header()->type != PageType::BTREE_LEAF) 
        return Status::INVALID_ARGUMENT;
    
    auto backup = std::make_unique<uint8_t[]>(PAGE_SIZE);
    std::memcpy(backup.get(), page->data, PAGE_SIZE);
    auto* old_h = reinterpret_cast<const PageHeader*>(backup.get());
    auto* old_slots = reinterpret_cast<const Slot*>(backup.get() + PageHeader::SIZE);
    
    PageId next = old_h->next_sibling;
    PageId prev = old_h->prev_sibling;
    
    page->Header()->num_keys = 0;
    page->Header()->free_start = PageHeader::SIZE;
    page->Header()->free_end = PAGE_SIZE;
    std::memset(page->data + PageHeader::SIZE, 0, PAGE_SIZE - PageHeader::SIZE);
    
    auto* slots = reinterpret_cast<Slot*>(page->Body());
    int count = 0;
    
    for (int i = 0; i < old_h->num_keys; ++i) {
        auto& os = old_slots[i];
        auto* visible = FindVisibleVersion(reinterpret_cast<const Page*>(backup.get()), os.val_offset, min_read_ts);
        if (!visible) continue;
        
        uint16_t koff = page->Allocate(os.key_len);
        std::memcpy(page->data + koff, backup.get() + os.key_offset, os.key_len);
        
        size_t ver_size = VersionNodeEx::HEADER_SIZE + visible->data_len;
        uint16_t voff = page->Allocate(ver_size);
        auto* vn = reinterpret_cast<VersionNodeEx*>(page->data + voff);
        vn->version = visible->version;
        vn->is_delete = visible->is_delete;
        vn->pad[0] = vn->pad[1] = vn->pad[2] = 0;
        vn->data_len = visible->data_len;
        vn->first_overflow = visible->first_overflow;
        vn->older_offset = 0;
        
        if (visible->first_overflow == 0 || visible->first_overflow == INVALID_PAGE) {
            std::memcpy(page->data + voff + VersionNodeEx::HEADER_SIZE,
                       reinterpret_cast<const uint8_t*>(visible) + VersionNodeEx::HEADER_SIZE,
                       visible->data_len);
        }
        
        slots[count].key_offset = koff;
        slots[count].key_len = os.key_len;
        slots[count].val_offset = voff;
        slots[count].val_len = static_cast<uint16_t>(ver_size);
        count++;
    }
    
    page->Header()->num_keys = static_cast<uint16_t>(count);
    page->Header()->free_start = PageHeader::SIZE + count * static_cast<uint16_t>(SLOT_SIZE);
    page->Header()->next_sibling = next;
    page->Header()->prev_sibling = prev;
    page->UpdateChecksum();
    dirty.push_back(leaf_id);
    
    if (count > 0 && count < BTREE_ORDER / 4) {
        TryMergeLeaf(leaf_id, min_read_ts, dirty);
    }
    return Status::OK;
}

void BPlusTree::TryMergeLeaf(PageId leaf_id, TxId min_read_ts, std::vector<PageId>& dirty) {
    auto* page = pm_->GetPage(leaf_id);
    if (!page) return;
    
    PageId right_id = page->Header()->next_sibling;
    if (right_id == INVALID_PAGE || right_id == 0) return;
    
    auto* right_page = pm_->GetPage(right_id);
    if (!right_page || right_page->Header()->type != PageType::BTREE_LEAF) return;
    
    std::vector<uint8_t> right_first_key;
    if (right_page->Header()->num_keys > 0) {
        auto* rslots = reinterpret_cast<const Slot*>(right_page->Body());
        right_first_key = GetKeyFromSlot(right_page, rslots[0]);
    }
    
    auto right_backup = std::make_unique<uint8_t[]>(PAGE_SIZE);
    std::memcpy(right_backup.get(), right_page->data, PAGE_SIZE);
    auto* right_old_h = reinterpret_cast<const PageHeader*>(right_backup.get());
    auto* right_old_slots = reinterpret_cast<const Slot*>(right_backup.get() + PageHeader::SIZE);
    
    for (int i = 0; i < right_old_h->num_keys; ++i) {
        auto& os = right_old_slots[i];
        auto* visible = FindVisibleVersion(reinterpret_cast<const Page*>(right_backup.get()),
                                           os.val_offset, min_read_ts);
        if (!visible) continue;
        
        if (!page->HasSpace(SLOT_SIZE + os.key_len + VersionNodeEx::HEADER_SIZE + visible->data_len)) {
            return;
        }
        
        uint16_t koff = page->Allocate(os.key_len);
        std::memcpy(page->data + koff, right_backup.get() + os.key_offset, os.key_len);
        
        size_t ver_size = VersionNodeEx::HEADER_SIZE + visible->data_len;
        uint16_t voff = page->Allocate(ver_size);
        auto* vn = reinterpret_cast<VersionNodeEx*>(page->data + voff);
        vn->version = visible->version;
        vn->is_delete = visible->is_delete;
        vn->pad[0] = vn->pad[1] = vn->pad[2] = 0;
        vn->data_len = visible->data_len;
        vn->first_overflow = visible->first_overflow;
        vn->older_offset = 0;
        
        if (visible->first_overflow == 0 || visible->first_overflow == INVALID_PAGE) {
            std::memcpy(page->data + voff + VersionNodeEx::HEADER_SIZE,
                       reinterpret_cast<const uint8_t*>(visible) + VersionNodeEx::HEADER_SIZE,
                       visible->data_len);
        }
        
        auto* slots = reinterpret_cast<Slot*>(page->Body());
        int ni = page->Header()->num_keys;
        slots[ni].key_offset = koff;
        slots[ni].key_len = os.key_len;
        slots[ni].val_offset = voff;
        slots[ni].val_len = static_cast<uint16_t>(ver_size);
        page->Header()->num_keys++;
    }
    
    page->Header()->free_start = PageHeader::SIZE + page->Header()->num_keys * static_cast<uint16_t>(SLOT_SIZE);
    
    PageId next_right = right_old_h->next_sibling;
    page->Header()->next_sibling = next_right;
    if (next_right != INVALID_PAGE && next_right != 0) {
        auto* next_page = pm_->GetPage(next_right);
        if (next_page) next_page->Header()->prev_sibling = leaf_id;
    }
    
    right_page->Header()->num_keys = 0;
    pm_->FreePage(right_id, min_read_ts);
    
    if (!right_first_key.empty()) {
        RedirectChildParent(right_id, leaf_id, right_first_key, dirty);
    }
}

size_t BPlusTree::GetPageFragmentation(PageId leaf_id) const {
    auto* page = pm_->GetPage(leaf_id);
    if (!page) return 0;
    return page->GetFragmentation();
}

PageId BPlusTree::FindLeaf(const std::vector<uint8_t>& key) const {
    std::vector<PageId> dummy;
    return FindLeaf(key, dummy);
}

PageId BPlusTree::FindLeaf(const std::vector<uint8_t>& key, std::vector<PageId>& path) const {
    PageId sr = search_root_.load(std::memory_order_consume);
    PageId root = (sr != INVALID_PAGE) ? sr : root_page_.load(std::memory_order_consume);
    return FindLeaf(key, path, root);
}

PageId BPlusTree::FindLeaf(const std::vector<uint8_t>& key, std::vector<PageId>& path, PageId root) const {
    path.clear();
    PageId current = root;
    while (current != INVALID_PAGE) {
        path.push_back(current);
        auto* page = pm_->GetPage(current);
        if (!page) return INVALID_PAGE;
        auto* h = page->Header();
        if (h->type == PageType::BTREE_LEAF) return current;
        
        int idx = SearchInBranch(page, key);
        auto* slots = reinterpret_cast<const Slot*>(page->Body());
        
        if (idx >= 0) {
            // SearchInBranch returns the rightmost slot with slot.key ≤ key.
            // For that slot, key ≥ slot.key, so descend to cp[1] (right child).
            auto* cp = reinterpret_cast<const PageId*>(page->data + slots[idx].val_offset);
            current = cp[1];
        } else if (h->num_keys > 0) {
            // key < slot[0].key → go to leftmost child
            auto* cp = reinterpret_cast<const PageId*>(page->data + slots[0].val_offset);
            current = cp[0];
        } else {
            return INVALID_PAGE;
        }
    }
    return INVALID_PAGE;
}

int BPlusTree::SearchInLeaf(const Page* page, const std::vector<uint8_t>& key) const {
    auto* h = page->Header();
    auto* slots = reinterpret_cast<const Slot*>(page->Body());
    int left = 0, right = h->num_keys - 1;
    while (left <= right) {
        int mid = left + (right - left) / 2;
        auto cur_key = GetKeyFromSlot(page, slots[mid]);
        int cmp = cmp_(cur_key, key);
        if (cmp == 0) return mid;
        if (cmp < 0) left = mid + 1;
        else right = mid - 1;
    }
    return -1;
}

int BPlusTree::SearchInBranch(const Page* page, const std::vector<uint8_t>& key) const {
    auto* h = page->Header();
    auto* slots = reinterpret_cast<const Slot*>(page->Body());
    int left = 0, right = h->num_keys - 1;
    int result = -1;
    while (left <= right) {
        int mid = left + (right - left) / 2;
        auto cur_key = GetKeyFromSlot(page, slots[mid]);
        int cmp = cmp_(cur_key, key);
        if (cmp <= 0) { result = mid; left = mid + 1; }
        else right = mid - 1;
    }
    return result;
}

const VersionNodeEx* BPlusTree::FindVisibleVersion(const Page* page, uint16_t val_offset, TxId read_ts) const {
    uint16_t cur_off = val_offset;
    while (cur_off != 0 && cur_off >= PageHeader::SIZE) {
        auto* vnode = reinterpret_cast<const VersionNodeEx*>(page->data + cur_off);
        if (vnode->version <= read_ts) {
            if (vnode->is_delete) return nullptr;
            return vnode;
        }
        cur_off = vnode->older_offset;
    }
    return nullptr;
}

std::vector<uint8_t> BPlusTree::ReadVersionData(const Page* page, const VersionNodeEx* node) const {
    if (node->first_overflow != 0 && node->first_overflow != INVALID_PAGE) {
        return pm_->ReadOverflowChain(node->first_overflow);
    }
    return std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(node) + VersionNodeEx::HEADER_SIZE,
                                 reinterpret_cast<const uint8_t*>(node) + VersionNodeEx::HEADER_SIZE + node->data_len);
}

Status BPlusTree::DoInsertIntoLeaf(PageId leaf_id, const std::vector<uint8_t>& key,
                                    const std::vector<uint8_t>& value, TxId tx_id,
                                    std::vector<PageId>& dirty_pages) {
    auto* page = pm_->GetPage(leaf_id);
    auto* h = page->Header();
    auto* slots = reinterpret_cast<Slot*>(page->Body());
    
    int existing = SearchInLeaf(page, key);
    
    bool use_overflow = false;
    PageId overflow_first = INVALID_PAGE;
    size_t inline_size = value.size();
    
    if (value.size() > 3000) {
        use_overflow = true;
        inline_size = 0;
        overflow_first = pm_->AllocateOverflowChain(value, dirty_pages);
    }
    
    size_t ver_size = VersionNodeEx::HEADER_SIZE + inline_size;
    size_t need = ver_size + (existing < 0 ? SLOT_SIZE + key.size() : 0);
    if (!page->HasSpace(need)) return Status::ERROR;
    
    uint16_t new_off = page->Allocate(ver_size);
    auto* new_vn = reinterpret_cast<VersionNodeEx*>(page->data + new_off);
    new_vn->version = tx_id;
    new_vn->is_delete = false;
    new_vn->pad[0] = new_vn->pad[1] = new_vn->pad[2] = 0;
    new_vn->data_len = static_cast<uint32_t>(value.size());
    new_vn->first_overflow = use_overflow ? overflow_first : INVALID_PAGE;
    new_vn->older_offset = 0;
    
    if (!use_overflow) {
        std::memcpy(page->data + new_off + VersionNodeEx::HEADER_SIZE, value.data(), value.size());
    }
    
    if (existing >= 0) {
        auto& slot = slots[existing];
        new_vn->older_offset = slot.val_offset;
        slot.val_offset = new_off;
        slot.val_len = static_cast<uint16_t>(ver_size);
        h->free_start = PageHeader::SIZE + h->num_keys * static_cast<uint16_t>(SLOT_SIZE);
    } else {
        int insert_pos = 0;
        for (int i = 0; i < h->num_keys; ++i) {
            auto cur_key = GetKeyFromSlot(page, slots[i]);
            if (cmp_(cur_key, key) > 0) break;
            insert_pos = i + 1;
        }
        for (int i = h->num_keys; i > insert_pos; --i) slots[i] = slots[i-1];
        
        uint16_t koff = page->Allocate(key.size());
        std::memcpy(page->data + koff, key.data(), key.size());
        
        slots[insert_pos].key_offset = koff;
        slots[insert_pos].key_len = static_cast<uint16_t>(key.size());
        slots[insert_pos].val_offset = new_off;
        slots[insert_pos].val_len = static_cast<uint16_t>(ver_size);
        h->num_keys++;
        h->free_start = PageHeader::SIZE + h->num_keys * static_cast<uint16_t>(SLOT_SIZE);
    }
    return Status::OK;
}

Status BPlusTree::DoRemoveFromLeaf(PageId leaf_id, int slot_idx, TxId tx_id) {
    auto* page = pm_->GetPage(leaf_id);
    auto* h = page->Header();
    auto* slots = reinterpret_cast<Slot*>(page->Body());
    if (slot_idx < 0 || slot_idx >= h->num_keys) return Status::INVALID_ARGUMENT;
    
    auto& slot = slots[slot_idx];
    
    // 如果最新版本是同一事务写入的，原地标记 is_delete 即可，无需分配新墓碑
    auto* latest = reinterpret_cast<VersionNodeEx*>(page->data + slot.val_offset);
    if (latest->version == tx_id) {
        latest->is_delete = true;
        latest->data_len = 0;
        latest->first_overflow = INVALID_PAGE;
        slot.val_len = static_cast<uint16_t>(VersionNodeEx::HEADER_SIZE);
        h->free_start = PageHeader::SIZE + h->num_keys * static_cast<uint16_t>(SLOT_SIZE);
        return Status::OK;
    }
    
    size_t ver_size = VersionNodeEx::HEADER_SIZE;
    if (!page->HasSpace(ver_size)) return Status::ERROR;
    
    uint16_t new_off = page->Allocate(ver_size);
    if (new_off == 0) return Status::ERROR;
    auto* new_vn = reinterpret_cast<VersionNodeEx*>(page->data + new_off);
    new_vn->version = tx_id;
    new_vn->is_delete = true;
    new_vn->pad[0] = new_vn->pad[1] = new_vn->pad[2] = 0;
    new_vn->data_len = 0;
    new_vn->first_overflow = INVALID_PAGE;
    new_vn->older_offset = slot.val_offset;
    
    slot.val_offset = new_off;
    slot.val_len = static_cast<uint16_t>(ver_size);
    h->free_start = PageHeader::SIZE + h->num_keys * static_cast<uint16_t>(SLOT_SIZE);
    return Status::OK;
}

Status BPlusTree::SplitLeaf(PageId leaf_id, std::vector<PageId>& dirty_pages,
                            std::vector<uint8_t>& out_split_key, PageId& out_right_page) {
    auto* old_page = pm_->GetPage(leaf_id);
    auto* old_h = old_page->Header();
    auto* old_slots = reinterpret_cast<Slot*>(old_page->Body());
    
    PageId new_leaf = pm_->AllocatePage(PageType::BTREE_LEAF);
    dirty_pages.push_back(new_leaf);
    auto* new_page = pm_->GetPage(new_leaf);
    new_page->Header()->num_keys = 0;
    new_page->Header()->next_sibling = old_h->next_sibling;
    new_page->Header()->prev_sibling = leaf_id;
    
    old_h->next_sibling = new_leaf;
    
    // 更新原右兄弟的 prev（此时 next_sibling 已在 dirty_pages 中由 Insert 的兄弟 COW 保证）
    if (new_page->Header()->next_sibling != INVALID_PAGE) {
        auto* next_page = pm_->GetPage(new_page->Header()->next_sibling);
        if (next_page) next_page->Header()->prev_sibling = new_leaf;
    }
    
    int mid = old_h->num_keys / 2;
    out_split_key = GetKeyFromSlot(old_page, old_slots[mid]);
    
    auto* new_slots = reinterpret_cast<Slot*>(new_page->Body());
    for (int i = mid; i < old_h->num_keys; ++i) {
        auto& os = old_slots[i];
        uint16_t koff = new_page->Allocate(os.key_len);
        std::memcpy(new_page->data + koff, old_page->data + os.key_offset, os.key_len);
        uint16_t voff = new_page->Allocate(os.val_len);
        std::memcpy(new_page->data + voff, old_page->data + os.val_offset, os.val_len);
        
        int ni = new_page->Header()->num_keys;
        new_slots[ni].key_offset = koff;
        new_slots[ni].key_len = os.key_len;
        new_slots[ni].val_offset = voff;
        new_slots[ni].val_len = os.val_len;
        new_page->Header()->num_keys++;
    }
    new_page->Header()->free_start = PageHeader::SIZE + new_page->Header()->num_keys * static_cast<uint16_t>(SLOT_SIZE);
    
    old_h->num_keys = mid;
    old_h->free_start = PageHeader::SIZE + old_h->num_keys * static_cast<uint16_t>(SLOT_SIZE);
    
    out_right_page = new_leaf;
    return Status::OK;
}

Status BPlusTree::InsertIntoParent(const std::vector<PageId>& path,
                                   std::vector<uint8_t> split_key,
                                   PageId left_child, PageId right_child,
                                   PageId& current_root,
                                   std::vector<PageId>& dirty_pages) {
    if (path.size() < 2) return Status::ERROR;
    
    for (int level = static_cast<int>(path.size()) - 2; level >= 0; --level) {
        PageId parent_id = path[level];
        
        bool already_cow = false;
        for (auto d : dirty_pages) { if (d == parent_id) { already_cow = true; break; } }
        PageId cow_parent = already_cow ? parent_id : CopyPage(parent_id, dirty_pages);
        if (level == 0) current_root = cow_parent;
        
        auto* page = pm_->GetPage(cow_parent);
        auto* h = page->Header();
        auto* slots = reinterpret_cast<Slot*>(page->Body());
        
        size_t need = SLOT_SIZE + split_key.size() + sizeof(PageId) * 2;
        if (page->HasSpace(need)) {
            int pos = 0;
            for (int i = 0; i < h->num_keys; ++i) {
                auto k = GetKeyFromSlot(page, slots[i]);
                if (cmp_(k, split_key) > 0) break;
                pos = i + 1;
            }
            
            for (int i = h->num_keys; i > pos; --i) slots[i] = slots[i-1];
            
            uint16_t koff = page->Allocate(split_key.size());
            std::memcpy(page->data + koff, split_key.data(), split_key.size());
            
            uint16_t voff = page->Allocate(sizeof(PageId) * 2);
            auto* cp = reinterpret_cast<PageId*>(page->data + voff);
            cp[0] = left_child;
            cp[1] = right_child;
            
            slots[pos].key_offset = koff;
            slots[pos].key_len = static_cast<uint16_t>(split_key.size());
            slots[pos].val_offset = voff;
            slots[pos].val_len = sizeof(PageId) * 2;
            h->num_keys++;
            h->free_start = PageHeader::SIZE + h->num_keys * static_cast<uint16_t>(SLOT_SIZE);
            return Status::OK;
        }
        
        std::vector<uint8_t> mid_key;
        PageId new_branch = INVALID_PAGE;
        Status s = SplitBranch(cow_parent, dirty_pages, split_key, left_child, right_child,
                               mid_key, new_branch);
        if (!IsOk(s)) return s;
        
        if (level == 0) {
            PageId branch_root = pm_->AllocatePage(PageType::BTREE_BRANCH);
            dirty_pages.push_back(branch_root);
            auto* nr = pm_->GetPage(branch_root);
            nr->Header()->num_keys = 1;
            nr->Header()->free_start = PageHeader::SIZE + nr->Header()->num_keys * static_cast<uint16_t>(SLOT_SIZE);
            
            auto* ns = reinterpret_cast<Slot*>(nr->Body());
            uint16_t koff = nr->Allocate(mid_key.size());
            std::memcpy(nr->data + koff, mid_key.data(), mid_key.size());
            uint16_t voff = nr->Allocate(sizeof(PageId) * 2);
            auto* cp = reinterpret_cast<PageId*>(nr->data + voff);
            cp[0] = cow_parent;
            cp[1] = new_branch;
            ns[0].key_offset = koff;
            ns[0].key_len = static_cast<uint16_t>(mid_key.size());
            ns[0].val_offset = voff;
            ns[0].val_len = sizeof(PageId) * 2;
            current_root = branch_root;
            return Status::OK;
        }
        
        split_key = std::move(mid_key);
        left_child = cow_parent;
        right_child = new_branch;
    }
    return Status::OK;
}

Status BPlusTree::SplitBranch(PageId branch_id, std::vector<PageId>& dirty_pages,
                              const std::vector<uint8_t>& split_key,
                              PageId left_child, PageId right_child,
                              std::vector<uint8_t>& out_mid_key,
                              PageId& out_right_sibling) {
    auto* page = pm_->GetPage(branch_id);
    auto* h = page->Header();
    auto* slots = reinterpret_cast<Slot*>(page->Body());
    
    // 确定新键在合并序列 (原 slots + 新键) 中的排序位置
    int insert_pos = 0;
    for (int i = 0; i < h->num_keys; ++i) {
        auto k = GetKeyFromSlot(page, slots[i]);
        if (cmp_(k, split_key) > 0) break;
        insert_pos = i + 1;
    }
    
    int total_keys = h->num_keys + 1;
    int mid = total_keys / 2;
    
    // 确定中位键（将提升到父节点）
    if (insert_pos == mid) {
        out_mid_key = split_key;
    } else {
        int src_idx = (insert_pos < mid) ? (mid - 1) : mid;
        out_mid_key = GetKeyFromSlot(page, slots[src_idx]);
    }
    
    // 将原页数据暂存到堆上，以便在原页上原地重建左半
    size_t old_data_size = PAGE_SIZE;
    auto backup = std::make_unique<uint8_t[]>(old_data_size);
    std::memcpy(backup.get(), page->data, old_data_size);
    auto* old_slots = reinterpret_cast<const Slot*>(backup.get() + PageHeader::SIZE);
    
    // 创建右兄弟页
    PageId new_branch = pm_->AllocatePage(PageType::BTREE_BRANCH);
    dirty_pages.push_back(new_branch);
    auto* nb = pm_->GetPage(new_branch);
    nb->Header()->num_keys = 0;
    nb->Header()->free_start = PageHeader::SIZE;
    auto* ns = reinterpret_cast<Slot*>(nb->Body());
    
    // 重建原页为左半（< mid 的条目）
    h->num_keys = 0;
    h->free_start = PageHeader::SIZE;
    h->free_end = PAGE_SIZE;
    std::memset(page->data + PageHeader::SIZE, 0, PAGE_SIZE - PageHeader::SIZE);
    
    auto copy_slot_to = [this](Page* dst, const Page* src, const Slot& s,
                                Slot* slots_out, int idx) -> void {
        uint16_t koff = dst->Allocate(s.key_len);
        std::memcpy(dst->data + koff, src->data + s.key_offset, s.key_len);
        uint16_t voff = dst->Allocate(s.val_len);
        std::memcpy(dst->data + voff, src->data + s.val_offset, s.val_len);
        slots_out[idx].key_offset = koff;
        slots_out[idx].key_len = s.key_len;
        slots_out[idx].val_offset = voff;
        slots_out[idx].val_len = s.val_len;
    };
    
    // 遍历原 slots + 新键，分配到左页或右页
    int old_idx = 0;
    
    for (int merged_pos = 0; merged_pos < total_keys; ++merged_pos) {
        if (merged_pos == mid) continue; // 跳过中位键（提升到父节点）
        
        if (merged_pos == insert_pos) {
            // 当前应放新键
            if (merged_pos < mid) {
                // 放入左页
                int ni = h->num_keys;
                auto* ls = reinterpret_cast<Slot*>(page->Body());
                uint16_t koff = page->Allocate(split_key.size());
                std::memcpy(page->data + koff, split_key.data(), split_key.size());
                uint16_t voff = page->Allocate(sizeof(PageId) * 2);
                auto* cp = reinterpret_cast<PageId*>(page->data + voff);
                cp[0] = left_child;
                cp[1] = right_child;
                ls[ni].key_offset = koff;
                ls[ni].key_len = static_cast<uint16_t>(split_key.size());
                ls[ni].val_offset = voff;
                ls[ni].val_len = sizeof(PageId) * 2;
                h->num_keys++;
            } else {
                // 放入右页
                int ni = nb->Header()->num_keys;
                uint16_t koff = nb->Allocate(split_key.size());
                std::memcpy(nb->data + koff, split_key.data(), split_key.size());
                uint16_t voff = nb->Allocate(sizeof(PageId) * 2);
                auto* cp = reinterpret_cast<PageId*>(nb->data + voff);
                cp[0] = left_child;
                cp[1] = right_child;
                ns[ni].key_offset = koff;
                ns[ni].key_len = static_cast<uint16_t>(split_key.size());
                ns[ni].val_offset = voff;
                ns[ni].val_len = sizeof(PageId) * 2;
                nb->Header()->num_keys++;
            }
        } else {
            // 从原 slots 取条目
            const auto& os = old_slots[old_idx];
            int this_pos = (insert_pos <= old_idx) ? (old_idx + 1) : old_idx;
            
            if (this_pos < mid) {
                // 放入左页
                int ni = h->num_keys;
                auto* ls = reinterpret_cast<Slot*>(page->Body());
                copy_slot_to(page, reinterpret_cast<const Page*>(backup.get()), os, ls, ni);
                h->num_keys++;
            } else if (this_pos > mid) {
                // 放入右页
                int ni = nb->Header()->num_keys;
                copy_slot_to(nb, reinterpret_cast<const Page*>(backup.get()), os, ns, ni);
                nb->Header()->num_keys++;
            }
            old_idx++;
        }
    }
    
    h->free_start = PageHeader::SIZE + h->num_keys * static_cast<uint16_t>(SLOT_SIZE);
    nb->Header()->free_start = PageHeader::SIZE + nb->Header()->num_keys * static_cast<uint16_t>(SLOT_SIZE);
    
    // 不在此创建新根 — 由调用方 InsertIntoParent 在 level==0 时处理
    out_right_sibling = new_branch;
    return Status::OK;
}

BPlusTree::Slot* BPlusTree::GetSlotArray(Page* page) {
    return reinterpret_cast<Slot*>(page->Body());
}

const BPlusTree::Slot* BPlusTree::GetSlotArray(const Page* page) const {
    return reinterpret_cast<const Slot*>(page->Body());
}

std::vector<uint8_t> BPlusTree::GetKeyFromSlot(const Page* page, const Slot& slot) const {
    return std::vector<uint8_t>(page->data + slot.key_offset, 
                                 page->data + slot.key_offset + slot.key_len);
}

PageId BPlusTree::CopyPage(PageId old_id, std::vector<PageId>& dirty_pages) {
    auto* old_page = pm_->GetPage(old_id);
    if (!old_page) return INVALID_PAGE;
    
    PageId new_id = pm_->AllocatePage(old_page->Header()->type);
    dirty_pages.push_back(new_id);
    
    auto* new_page = pm_->GetPage(new_id);
    std::memcpy(new_page->data, old_page->data, PAGE_SIZE);
    new_page->Header()->id = new_id;
    // 保留 sibling 指针
    
    return new_id;
}

void BPlusTree::PatchVersion(PageId pid, TxId old_ver, TxId new_ver) {
    auto* page = pm_->GetPage(pid);
    if (!page) return;
    auto* h = page->Header();
    if (h->type != PageType::BTREE_LEAF) return;
    auto* slots = reinterpret_cast<Slot*>(page->data + PageHeader::SIZE);
    for (uint16_t i = 0; i < h->num_keys; ++i) {
        uint16_t cur_off = slots[i].val_offset;
        while (cur_off >= PageHeader::SIZE) {
            auto* vn = reinterpret_cast<VersionNodeEx*>(page->data + cur_off);
            if (vn->version == old_ver)
                vn->version = new_ver;
            cur_off = vn->older_offset;
        }
    }
}

} // namespace graphstore

#include "page_manager.h"
#include "bplus_tree.h"
#include "graph_store.h"
#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#else
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#endif
#include <algorithm>

namespace graphstore {

// ==================== Page ====================
void Page::Init(PageId id, PageType type) {
    std::memset(data, 0, PAGE_SIZE);
    auto* h = Header();
    h->id = id;
    h->type = type;
    h->num_keys = 0;
    h->free_start = PageHeader::SIZE;
    h->free_end = PAGE_SIZE;
    h->checksum = 0;
    h->next_sibling = INVALID_PAGE;
    h->prev_sibling = INVALID_PAGE;
    h->next_overflow = INVALID_PAGE;
    UpdateChecksum();
}

static uint32_t Crc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int j = 0; j < 8; ++j) crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320u : 0u);
    }
    return ~crc;
}

uint32_t Page::ComputeChecksum() const {
    return Crc32(data + PageHeader::SIZE, PAGE_SIZE - PageHeader::SIZE);
}

void Page::UpdateChecksum() {
    Header()->checksum = ComputeChecksum();
}

bool Page::Validate() const {
    auto* h = Header();
    if (h->free_start < PageHeader::SIZE || h->free_start > PAGE_SIZE) return false;
    if (h->free_end < PageHeader::SIZE || h->free_end > PAGE_SIZE) return false;
    if (h->free_start > h->free_end) return false;
    // 校验 checksum（跳过 checksum 字段自身）
    uint32_t saved = h->checksum;
    const_cast<Page*>(this)->Header()->checksum = 0;
    uint32_t computed = ComputeChecksum();
    const_cast<Page*>(this)->Header()->checksum = saved;
    return saved == 0 || saved == computed;
}

uint16_t Page::Allocate(size_t size) {
    auto* h = Header();
    if (h->free_end < h->free_start) return 0;
    if (static_cast<size_t>(h->free_end - h->free_start) < size) return 0;
    h->free_end -= static_cast<uint16_t>(size);
    return h->free_end;
}

bool Page::HasSpace(size_t size) const {
    auto* h = Header();
    if (h->free_end < h->free_start) return false;
    return static_cast<size_t>(h->free_end - h->free_start) >= size;
}

size_t Page::GetFragmentation() const {
    auto* h = Header();
    return (h->free_end - h->free_start);
}

PageManager::PageManager() = default;
PageManager::~PageManager() { Close(); }

Status PageManager::Init(const std::filesystem::path& path, size_t map_size) {
    db_path_ = path;
    map_size_ = map_size;
    std::filesystem::path parent = path.parent_path();
    if(!parent.empty() && !std::filesystem::exists(parent)){
        std::filesystem::create_directories(parent);
    }
    
#ifdef _WIN32
    file_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file_ == INVALID_HANDLE_VALUE) return Status::IO_ERROR;

    if (!IsOk(EnsureFileSize(PAGE_SIZE * 2))) {
        CloseHandle(file_); file_ = INVALID_HANDLE_VALUE;
        return Status::IO_ERROR;
    }

    // 标记文件为稀疏文件，使 pre-allocation 不占用物理磁盘空间
    {
        DWORD dwTemp;
        DeviceIoControl(file_, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &dwTemp, nullptr);
    }

    mapped_size_ = map_size;
    capacity_ = map_size;
    {
        LARGE_INTEGER li;
        li.QuadPart = mapped_size_;
        SetFilePointerEx(file_, li, nullptr, FILE_BEGIN);
        SetEndOfFile(file_);
        li.QuadPart = 0;
        SetFilePointerEx(file_, li, nullptr, FILE_BEGIN);
    }

    file_mapping_ = CreateFileMappingW(file_, nullptr, PAGE_READWRITE,
        (mapped_size_ >> 32) & 0xFFFFFFFF, mapped_size_ & 0xFFFFFFFF, nullptr);
    if (!file_mapping_) {
        CloseHandle(file_); file_ = INVALID_HANDLE_VALUE;
        return Status::IO_ERROR;
    }

    mapped_ = static_cast<uint8_t*>(MapViewOfFile(file_mapping_, FILE_MAP_ALL_ACCESS,
        0, 0, mapped_size_));
    if (!mapped_) {
        CloseHandle(file_mapping_); file_mapping_ = nullptr;
        CloseHandle(file_); file_ = INVALID_HANDLE_VALUE;
        return Status::IO_ERROR;
    }
#else
    fd_ = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd_ < 0) return Status::IO_ERROR;
    
    if (!IsOk(EnsureFileSize(PAGE_SIZE * 2))) {
        ::close(fd_); fd_ = -1;
        return Status::IO_ERROR;
    }
    
    mapped_size_ = map_size;
    capacity_ = map_size;
    mapped_ = static_cast<uint8_t*>(::mmap(nullptr, mapped_size_, PROT_READ | PROT_WRITE, 
                                            MAP_SHARED, fd_, 0));
    if (mapped_ == MAP_FAILED) {
        ::close(fd_); fd_ = -1;
        return Status::IO_ERROR;
    }
#endif
    
    meta_ = reinterpret_cast<MetaPage*>(mapped_);
    if (meta_->magic != MetaPage::MAGIC) {
        // v3: 初始化6个独立根页
        meta_->magic = MetaPage::MAGIC;
        meta_->version = MetaPage::VERSION;
        meta_->vertex_root = INVALID_PAGE;
        meta_->edge_root = INVALID_PAGE;
        meta_->out_edge_root = INVALID_PAGE;
        meta_->in_edge_root = INVALID_PAGE;
        meta_->prop_root = INVALID_PAGE;
        meta_->label_root = INVALID_PAGE;
        meta_->prop_index_root = INVALID_PAGE;
        meta_->global_commit_ts = 0;
        meta_->num_pages = 1;
        meta_->checksum = 0;
        num_pages_.store(1, std::memory_order_relaxed);
    } else {
        num_pages_.store(meta_->num_pages, std::memory_order_relaxed);
        // 验证 meta page checksum（清零自身 checksum 字段后重算）
        uint32_t saved_cs = meta_->checksum;
        meta_->checksum = 0;
        uint32_t expected = Crc32(reinterpret_cast<const uint8_t*>(meta_), sizeof(MetaPage));
        meta_->checksum = saved_cs;
        if (saved_cs != 0 && saved_cs != expected) return Status::CORRUPTION;
        // 验证所有数据页 checksum
        for (PageId i = 1; i < num_pages_.load(std::memory_order_acquire); ++i) {
            if (!reinterpret_cast<const Page*>(mapped_ + i * PAGE_SIZE)->Validate())
                return Status::CORRUPTION;
        }
    }
    return Status::OK;
}

Status PageManager::Close() {
    if (mapped_) {
        CheckpointWal();
        FlushAll();
    }
    if (mapped_) {
#ifdef _WIN32
        FlushViewOfFile(mapped_, mapped_size_);
        UnmapViewOfFile(mapped_);
#else
        ::msync(mapped_, mapped_size_, MS_SYNC);
        ::munmap(mapped_, mapped_size_);
#endif
        mapped_ = nullptr;
    }
#ifdef _WIN32
    if (wal_fd_ != INVALID_HANDLE_VALUE) { CloseHandle(wal_fd_); wal_fd_ = INVALID_HANDLE_VALUE; }
    // 关闭后删除 WAL 文件（正常关闭不需要 WAL）
    wal_path_ = db_path_;
    wal_path_ += L".wal";
    std::error_code ec;
    std::filesystem::remove(wal_path_, ec);
    
    if (file_mapping_) { CloseHandle(file_mapping_); file_mapping_ = nullptr; }
    if (file_ != INVALID_HANDLE_VALUE) { CloseHandle(file_); file_ = INVALID_HANDLE_VALUE; }
#else
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
#endif
    return Status::OK;
}

PageId PageManager::AllocatePage(PageType type) {
    std::lock_guard<std::mutex> lock(alloc_mutex_);
    {
        std::lock_guard<std::mutex> fl_lock(freelist_mutex_);
        TxId current_commit = meta_->global_commit_ts;
        auto it = freelist_.begin();
        while (it != freelist_.end() && it->first <= current_commit) {
            PageId pid = it->second;
            freelist_.erase(it++);
            auto* page = GetPage(pid);
            page->Init(pid, type);
            return pid;
        }
    }
    return DoAllocate(type);
}

Status PageManager::FreePage(PageId id, TxId obsolete_ver) {
    std::lock_guard<std::mutex> lock(freelist_mutex_);
    freelist_.insert({obsolete_ver, id});
    return Status::OK;
}

Page* PageManager::GetPage(PageId id) {
    if (id >= num_pages_.load(std::memory_order_acquire)) return nullptr;
    auto* page = reinterpret_cast<Page*>(mapped_ + id * PAGE_SIZE);
    if (id == 0) {
        meta_->checksum = 0;
    } else {
        if (page->Header()->checksum != 0) {
            uint32_t saved = page->Header()->checksum;
            page->Header()->checksum = 0;
            uint32_t computed = page->ComputeChecksum();
            page->Header()->checksum = saved;
            if (saved != computed) return nullptr;
        }
        page->Header()->checksum = 0;
    }
    return page;
}

const Page* PageManager::GetPage(PageId id) const {
    if (id >= num_pages_.load(std::memory_order_acquire)) return nullptr;
    auto* page = reinterpret_cast<const Page*>(mapped_ + id * PAGE_SIZE);
    if (id == 0) {
        // page 0 用 MetaPage::checksum 校验，避免破坏 PageHeader 重叠区
        uint32_t saved = meta_->checksum;
        if (saved == 0) return page; // 脏页未刷新
        meta_->checksum = 0;
        uint32_t expected = Crc32(reinterpret_cast<const uint8_t*>(meta_), sizeof(MetaPage));
        meta_->checksum = saved;
        if (saved != expected) return nullptr;
        return page;
    }
    if (!page->Validate()) return nullptr;
    return page;
}

Status PageManager::FlushPage(PageId id) {
    if (id >= num_pages_.load(std::memory_order_acquire)) return Status::INVALID_ARGUMENT;
    if (id == 0) {
        // page 0 用 MetaPage 级 checksum
        meta_->checksum = 0;
        meta_->checksum = Crc32(reinterpret_cast<const uint8_t*>(meta_), sizeof(MetaPage));
    } else {
        auto* page = reinterpret_cast<Page*>(mapped_ + id * PAGE_SIZE);
        page->UpdateChecksum();
    }
    void* addr = mapped_ + id * PAGE_SIZE;
#ifdef _WIN32
    if (!FlushViewOfFile(addr, PAGE_SIZE)) return Status::IO_ERROR;
#else
    if (::msync(addr, PAGE_SIZE, MS_SYNC) != 0) return Status::IO_ERROR;
#endif
    return Status::OK;
}

Status PageManager::FlushAll() {
    // 先同步 meta page 的页面计数
    meta_->num_pages = num_pages_.load(std::memory_order_acquire);
    // page 0: 用 MetaPage 级 checksum（避免覆盖 Header 重叠区的 root 指针）
    {
        meta_->checksum = 0;
        meta_->checksum = Crc32(reinterpret_cast<const uint8_t*>(meta_), sizeof(MetaPage));
    }
    // 其余页面用 Page::UpdateChecksum
    for (PageId i = 1; i < meta_->num_pages; ++i) {
        auto* page = reinterpret_cast<Page*>(mapped_ + i * PAGE_SIZE);
        page->UpdateChecksum();
    }
#ifdef _WIN32
    if (!FlushViewOfFile(mapped_, num_pages_.load(std::memory_order_acquire) * PAGE_SIZE)) return Status::IO_ERROR;
#else
    if (::msync(mapped_, num_pages_.load(std::memory_order_acquire) * PAGE_SIZE, MS_SYNC) != 0) return Status::IO_ERROR;
#endif
    return Status::OK;
}

MetaPage* PageManager::GetMetaPage() { return meta_; }

Status PageManager::UpdateMeta(const MetaPage& meta) {
    *meta_ = meta;
    return FlushPage(0);
}

TxId PageManager::GetGlobalCommitTs() const { return meta_->global_commit_ts; }
void PageManager::SetGlobalCommitTs(TxId ts) { meta_->global_commit_ts = ts; }

Status PageManager::EnsureFileSize(size_t size) {
#ifdef _WIN32
    LARGE_INTEGER cur;
    if (!GetFileSizeEx(file_, &cur)) return Status::IO_ERROR;
    if (static_cast<size_t>(cur.QuadPart) < size) {
        LARGE_INTEGER dist;
        dist.QuadPart = size;
        if (!SetFilePointerEx(file_, dist, nullptr, FILE_BEGIN)) return Status::IO_ERROR;
        if (!SetEndOfFile(file_)) return Status::IO_ERROR;
    }
    return Status::OK;
#else
    struct stat st;
    if (::fstat(fd_, &st) != 0) return Status::IO_ERROR;
    if (static_cast<size_t>(st.st_size) < size) {
        if (::ftruncate(fd_, size) != 0) return Status::IO_ERROR;
    }
    return Status::OK;
#endif
}

#ifdef _WIN32
Status PageManager::Remap() {
    if (mapped_) {
        UnmapViewOfFile(mapped_);
        mapped_ = nullptr;
    }
    if (file_mapping_) {
        CloseHandle(file_mapping_);
        file_mapping_ = nullptr;
    }

    LARGE_INTEGER li;
    GetFileSizeEx(file_, &li);
    size_t file_sz = (size_t)li.QuadPart;

    file_mapping_ = CreateFileMappingW(file_, nullptr, PAGE_READWRITE, 0, 0, nullptr);
    if (!file_mapping_) return Status::IO_ERROR;

    mapped_ = static_cast<uint8_t*>(MapViewOfFile(file_mapping_, FILE_MAP_ALL_ACCESS, 0, 0, 0));
    if (!mapped_) {
        CloseHandle(file_mapping_);
        file_mapping_ = nullptr;
        return Status::IO_ERROR;
    }

    mapped_size_ = file_sz;
    meta_ = reinterpret_cast<MetaPage*>(mapped_);
    return Status::OK;
}
#endif

PageId PageManager::DoAllocate(PageType type) {
    // 先确保文件/mmap空间够，再递增 num_pages_
    size_t next_count = num_pages_.load(std::memory_order_acquire) + 1;
    size_t required = next_count * PAGE_SIZE;
    if (required > mapped_size_) {
        mapped_size_ = required * 2;
        if (!IsOk(EnsureFileSize(mapped_size_))) return INVALID_PAGE;
#ifdef _WIN32
        if (!IsOk(Remap())) return INVALID_PAGE;
#endif
    } else {
        if (!IsOk(EnsureFileSize(required))) return INVALID_PAGE;
    }
    PageId id = num_pages_.fetch_add(1, std::memory_order_acq_rel);
    auto* page = GetPage(id);
    page->Init(id, type);
    return id;
}

// v3: 分配溢出页链
PageId PageManager::AllocateOverflowChain(const std::vector<uint8_t>& data, 
                                           std::vector<PageId>& dirty) {
    if (data.empty()) return INVALID_PAGE;
    
    size_t offset = 0;
    PageId first_page = INVALID_PAGE;
    PageId prev_page = INVALID_PAGE;
    
    while (offset < data.size()) {
        PageId pid = AllocatePage(PageType::OVERFLOW_PAGE);
        dirty.push_back(pid);
        auto* page = GetPage(pid);
        auto* h = page->Header();
        
        if (first_page == INVALID_PAGE) first_page = pid;
        if (prev_page != INVALID_PAGE) {
            auto* prev = GetPage(prev_page);
            auto* prev_oh = reinterpret_cast<OverflowHeader*>(prev->data + PageHeader::SIZE);
            prev_oh->next_overflow = pid;
        }
        
        // 用动态 Allocate 分配 OverflowHeader 位置（从 free_start 前进）
        uint16_t hdr_off = h->free_start;
        h->free_start += OverflowHeader::SIZE;
        auto* oh = reinterpret_cast<OverflowHeader*>(page->data + hdr_off);
        oh->next_overflow = INVALID_PAGE;
        oh->data_len = 0;
        oh->total_len = static_cast<uint32_t>(data.size());
        
        // 数据分配在页末尾，剩余空间 = free_end - free_start
        size_t avail = h->free_end > h->free_start ? h->free_end - h->free_start : 0;
        size_t chunk_size = std::min(data.size() - offset, static_cast<size_t>(avail));
        if (chunk_size == 0) break;
        
        uint16_t doff = page->Allocate(chunk_size);
        oh->data_len = static_cast<uint32_t>(chunk_size);
        oh->data_offset = doff;
        std::memcpy(page->data + doff, data.data() + offset, chunk_size);
        
        offset += chunk_size;
        prev_page = pid;
    }
    
    return first_page;
}

std::vector<uint8_t> PageManager::ReadOverflowChain(PageId first_page) const {
    std::vector<uint8_t> result;
    PageId current = first_page;
    
    while (current != INVALID_PAGE && current != 0) {
        auto* page = GetPage(current);
        if (!page || page->Header()->type != PageType::OVERFLOW_PAGE) {
            result.clear();
            break;
        }
        
        auto* oh = reinterpret_cast<const OverflowHeader*>(page->data + PageHeader::SIZE);
        result.insert(result.end(), page->data + oh->data_offset,
                       page->data + oh->data_offset + oh->data_len);
        
        current = oh->next_overflow;
    }
    return result;
}

void PageManager::FreeOverflowChain(PageId first_page, TxId obsolete_ver) {
    PageId current = first_page;
    while (current != INVALID_PAGE && current != 0) {
        auto* page = GetPage(current);
        if (!page) break;
        auto* oh = reinterpret_cast<const OverflowHeader*>(page->data + PageHeader::SIZE);
        PageId next = oh->next_overflow;
        FreePage(current, obsolete_ver);
        current = next;
    }
}

// ==================== WalHeader ====================
void WalHeader::SetChecksum(const void* body) {
    body_crc = (body && data_len > 0) ? Crc32(static_cast<const uint8_t*>(body), data_len) : 0;
    hdr_crc = 0;
    uint8_t buf[SIZE];
    std::memcpy(buf, this, SIZE);
    hdr_crc = Crc32(buf, SIZE);
}

bool WalHeader::Validate(const void* body) const {
    uint32_t saved_hdr = hdr_crc;
    const_cast<WalHeader*>(this)->hdr_crc = 0;
    uint8_t buf[SIZE];
    std::memcpy(buf, this, SIZE);
    uint32_t hdr_expected = Crc32(buf, SIZE);
    const_cast<WalHeader*>(this)->hdr_crc = saved_hdr;
    if (saved_hdr != hdr_expected) return false;
    if (body && data_len > 0) {
        uint32_t body_expected = Crc32(static_cast<const uint8_t*>(body), data_len);
        if (body_crc != body_expected) return false;
    }
    return true;
}

// ==================== PageManager WAL ====================
Status PageManager::EnsureWalOpen() {
    if (wal_fd_ != INVALID_HANDLE_VALUE) return Status::OK;
    wal_path_ = db_path_;
    wal_path_ += L".wal";
#ifdef _WIN32
    wal_fd_ = CreateFileW(wal_path_.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (wal_fd_ == INVALID_HANDLE_VALUE) return Status::IO_ERROR;
#endif
    return Status::OK;
}

Status PageManager::WriteWalRecordRaw(const void* buf, size_t len) {
    Status s = EnsureWalOpen();
    if (!IsOk(s)) return s;
#ifdef _WIN32
    DWORD written;
    if (!WriteFile(wal_fd_, buf, static_cast<DWORD>(len), &written, nullptr))
        return Status::IO_ERROR;
    if (written != len) return Status::IO_ERROR;
    wal_file_size_ += len;
#endif
    return Status::OK;
}

Status PageManager::InternalWriteWalRecord(WalRecordType type, TxId tx_id, PageId page_id,
                                            const void* body, size_t body_len) {
    WalHeader hdr;
    hdr.type = type;
    hdr.tx_id = tx_id;
    hdr.page_id = page_id;
    hdr.data_len = static_cast<uint32_t>(body_len);
    hdr.body_crc = 0;
    hdr.hdr_crc = 0;
    hdr.SetChecksum(body);
    
    Status s = WriteWalRecordRaw(&hdr, WalHeader::SIZE);
    if (!IsOk(s)) return s;
    if (body && body_len > 0) {
        s = WriteWalRecordRaw(body, body_len);
        if (!IsOk(s)) return s;
    }
    return Status::OK;
}

Status PageManager::BeginWalTx(TxId tx_id) {
    return InternalWriteWalRecord(WalRecordType::BEGIN_TX, tx_id, INVALID_PAGE, nullptr, 0);
}

Status PageManager::WriteWalPageImage(TxId tx_id, PageId page_id, const void* data, size_t len) {
    return InternalWriteWalRecord(WalRecordType::PAGE_IMAGE, tx_id, page_id, data, len);
}

Status PageManager::CommitWalTx(TxId tx_id, const WalCommitPayload& meta_snap) {
    return InternalWriteWalRecord(WalRecordType::COMMIT_TX, tx_id, 0, &meta_snap, sizeof(meta_snap));
}

Status PageManager::FlushWal() {
    Status s = EnsureWalOpen();
    if (!IsOk(s)) return s;
#ifdef _WIN32
    if (!FlushFileBuffers(wal_fd_)) return Status::IO_ERROR;
#endif
    return Status::OK;
}

Status PageManager::RecoverFromWal() {
    wal_path_ = db_path_;
    wal_path_ += L".wal";
#ifdef _WIN32
    HANDLE wal = CreateFileW(wal_path_.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (wal == INVALID_HANDLE_VALUE) {
        return Status::OK;
    }
    
    LARGE_INTEGER li;
    if (!GetFileSizeEx(wal, &li) || li.QuadPart == 0) {
        CloseHandle(wal);
        return Status::OK;
    }
    
    size_t wal_size = static_cast<size_t>(li.QuadPart);
    auto buf = std::make_unique<uint8_t[]>(wal_size);
    DWORD read;
    if (!ReadFile(wal, buf.get(), static_cast<DWORD>(wal_size), &read, nullptr) || read != wal_size) {
        CloseHandle(wal);
        return Status::IO_ERROR;
    }
    CloseHandle(wal);
    
    // 扫描 WAL，收集所有提交事务
    size_t offset = 0;
    WalCommitPayload latest_meta{};
    bool has_latest_meta = false;
    TxId latest_commit_ts = 0;
    std::unordered_map<TxId, bool> tx_committed;
    
    // 第一遍：确定哪些事务已提交
    while (offset + WalHeader::SIZE <= wal_size) {
        auto* hdr = reinterpret_cast<const WalHeader*>(buf.get() + offset);
        if (offset + WalHeader::SIZE + hdr->data_len > wal_size) break;
        const uint8_t* body = (hdr->data_len > 0) ? (buf.get() + offset + WalHeader::SIZE) : nullptr;
        if (!hdr->Validate(body)) break;
        
        if (hdr->type == WalRecordType::BEGIN_TX)
            tx_committed[hdr->tx_id] = false;
        else if (hdr->type == WalRecordType::COMMIT_TX)
            tx_committed[hdr->tx_id] = true;
        
        offset += WalHeader::SIZE + hdr->data_len;
    }
    
    // 第二遍：应用已提交事务的 PAGE_IMAGE
    offset = 0;
    while (offset + WalHeader::SIZE <= wal_size) {
        auto* hdr = reinterpret_cast<const WalHeader*>(buf.get() + offset);
        if (offset + WalHeader::SIZE + hdr->data_len > wal_size) break;
        const uint8_t* body = (hdr->data_len > 0) ? (buf.get() + offset + WalHeader::SIZE) : nullptr;
        if (!hdr->Validate(body)) break;
        
        if (hdr->type == WalRecordType::PAGE_IMAGE && tx_committed[hdr->tx_id]) {
            if (hdr->data_len >= PAGE_SIZE) {
                // 先确保文件映射足够大，再递增 num_pages_
                PageId max_pid = hdr->page_id + 1;
                size_t current = num_pages_.load(std::memory_order_acquire);
                while (current < max_pid) {
                    size_t required = (current + 1) * PAGE_SIZE;
                    if (required > mapped_size_) {
                        mapped_size_ = required * 2;
                        if (!IsOk(EnsureFileSize(mapped_size_))) break;
#ifdef _WIN32
                        if (!IsOk(Remap())) break;
#endif
                    } else {
                        if (!IsOk(EnsureFileSize(required))) break;
                    }
                    num_pages_.fetch_add(1, std::memory_order_acq_rel);
                    ++current;
                }
                if (current < max_pid) break;  // 空间不足，中止恢复
                std::memcpy(mapped_ + hdr->page_id * PAGE_SIZE, body, PAGE_SIZE);
                // 恢复 checksum
                auto* page = reinterpret_cast<Page*>(mapped_ + hdr->page_id * PAGE_SIZE);
                page->UpdateChecksum();
            }
        } else if (hdr->type == WalRecordType::COMMIT_TX) {
            if (body && hdr->data_len >= sizeof(WalCommitPayload)) {
                auto* cp = reinterpret_cast<const WalCommitPayload*>(body);
                if (cp->global_commit_ts > latest_commit_ts) {
                    latest_meta = *cp;
                    has_latest_meta = true;
                    latest_commit_ts = cp->global_commit_ts;
                }
            }
        }
        
        offset += WalHeader::SIZE + hdr->data_len;
    }
    
    // 更新 meta page 到最新已提交状态
    if (has_latest_meta) {
        meta_->vertex_root = latest_meta.vertex_root;
        meta_->edge_root = latest_meta.edge_root;
        meta_->out_edge_root = latest_meta.out_edge_root;
        meta_->in_edge_root = latest_meta.in_edge_root;
        meta_->prop_root = latest_meta.prop_root;
        meta_->label_root = latest_meta.label_root;
        meta_->prop_index_root = latest_meta.prop_index_root;
        meta_->global_commit_ts = latest_meta.global_commit_ts;
        meta_->num_pages = latest_meta.num_pages;
        num_pages_.store(latest_meta.num_pages, std::memory_order_release);
        SetGlobalCommitTs(latest_meta.global_commit_ts);
        
        // 刷新恢复后的页面和 meta
        FlushAll();
    }
#endif
    
    // 删除 WAL 文件
    std::error_code ec;
    std::filesystem::remove(wal_path_, ec);
    
    return Status::OK;
}

Status PageManager::CheckpointWal() {
    // 1. 刷新所有数据页到磁盘
    FlushAll();
    
    // 2. 写入 CHECKPOINT 记录
    WalCommitPayload meta_snap;
    meta_snap.vertex_root = meta_->vertex_root;
    meta_snap.edge_root = meta_->edge_root;
    meta_snap.out_edge_root = meta_->out_edge_root;
    meta_snap.in_edge_root = meta_->in_edge_root;
    meta_snap.prop_root = meta_->prop_root;
    meta_snap.label_root = meta_->label_root;
    meta_snap.prop_index_root = meta_->prop_index_root;
    meta_snap.global_commit_ts = meta_->global_commit_ts;
    meta_snap.num_pages = num_pages_.load(std::memory_order_acquire);
    
    Status s = InternalWriteWalRecord(WalRecordType::CHECKPOINT, 0, 0, &meta_snap, sizeof(meta_snap));
    if (!IsOk(s)) return s;
    s = FlushWal();
    if (!IsOk(s)) return s;
    
    // 3. 关闭并重新创建 WAL（截断）
#ifdef _WIN32
    if (wal_fd_ != INVALID_HANDLE_VALUE) {
        CloseHandle(wal_fd_);
        wal_fd_ = INVALID_HANDLE_VALUE;
    }
    wal_path_ = db_path_;
    wal_path_ += L".wal";
    wal_fd_ = CreateFileW(wal_path_.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (wal_fd_ == INVALID_HANDLE_VALUE) return Status::IO_ERROR;
    wal_file_size_ = 0;
#endif
    
    return Status::OK;
}

} // namespace graphstore

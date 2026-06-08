#ifndef PAGE_MANAGER_H
#define PAGE_MANAGER_H

#include "common.h"
#include <filesystem>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#undef ERROR
#endif

namespace graphstore {

// ==================== WAL (Write-Ahead Log) ====================
#pragma pack(push, 1)
enum class WalRecordType : uint8_t {
    BEGIN_TX = 1,
    PAGE_IMAGE = 2,
    COMMIT_TX = 3,
    CHECKPOINT = 4,
};

struct WalHeader {
    WalRecordType type;      // 1B
    TxId          tx_id;     // 8B
    PageId        page_id;   // 4B
    uint32_t      data_len;  // 4B
    uint32_t      body_crc;  // 4B — CRC of body data
    uint32_t      hdr_crc;   // 4B — CRC of [0..20]

    static constexpr size_t SIZE = 25;

    void SetChecksum(const void* body);
    bool Validate(const void* body) const;
};

struct WalCommitPayload {
    PageId vertex_root;
    PageId edge_root;
    PageId out_edge_root;
    PageId in_edge_root;
    PageId prop_root;
    PageId label_root;
    PageId prop_index_root;
    TxId   global_commit_ts;
    PageId num_pages;
};

static_assert(sizeof(WalHeader) == WalHeader::SIZE, "WalHeader size");
#pragma pack(pop)

enum class PageType : uint8_t {
    META = 0, FREELIST = 1, BTREE_BRANCH = 2, BTREE_LEAF = 3,
    OVERFLOW_PAGE = 4, DATA = 5
};

// v3: 24字节页头，含溢出页链指针
struct PageHeader {
    PageId    id;
    PageType  type;
    uint16_t  num_keys;
    uint16_t  free_start;
    uint16_t  free_end;
    uint32_t  checksum;
    PageId    next_sibling;   // 叶子页链表
    PageId    prev_sibling;
    PageId    next_overflow;  // v3: 溢出页链
    
    static constexpr size_t SIZE = 28;
};

static_assert(sizeof(PageHeader) == PageHeader::SIZE, "PageHeader size");

class Page {
public:
    alignas(alignof(std::max_align_t)) uint8_t data[PAGE_SIZE];
    
    PageHeader* Header() { return reinterpret_cast<PageHeader*>(data); }
    const PageHeader* Header() const { return reinterpret_cast<const PageHeader*>(data); }
    void* Body() { return data + PageHeader::SIZE; }
    const void* Body() const { return data + PageHeader::SIZE; }
    size_t BodySize() const { return PAGE_SIZE - PageHeader::SIZE; }
    
    void Init(PageId id, PageType type);
    uint32_t ComputeChecksum() const;
    void UpdateChecksum();
    bool Validate() const;
    uint16_t Allocate(size_t size);
    bool HasSpace(size_t size) const;
    // v3: 页内碎片统计
    size_t GetFragmentation() const;
};

static_assert(sizeof(Page) == PAGE_SIZE, "Page size");

// v3: MetaPage存储6个独立根页
struct MetaPage {
    static constexpr uint64_t MAGIC = 0x475241504853544F;
    static constexpr uint32_t VERSION = 3;
    
    uint64_t magic;
    uint32_t version;
    PageId   vertex_root;
    PageId   edge_root;
    PageId   out_edge_root;
    PageId   in_edge_root;
    PageId   prop_root;
    PageId   label_root;
    PageId   prop_index_root;  // v4: 属性索引树根
    TxId     global_commit_ts;
    PageId   num_pages;
    uint32_t reserved[2];
    uint32_t checksum;
};

class PageManager {
public:
    PageManager();
    ~PageManager();
    
    Status Init(const std::filesystem::path& path, size_t map_size = 1ULL << 30);
    Status Close();
    
    PageId AllocatePage(PageType type);
    Status FreePage(PageId id, TxId obsolete_ver);
    Page*  GetPage(PageId id);
    const Page* GetPage(PageId id) const;
    
    Status FlushPage(PageId id);
    Status FlushAll();
    
    MetaPage* GetMetaPage();
    Status UpdateMeta(const MetaPage& meta);
    
    TxId GetGlobalCommitTs() const;
    void SetGlobalCommitTs(TxId ts);
    PageId GetNumPages() const { return num_pages_.load(std::memory_order_acquire); }
    const std::filesystem::path& GetDbPath() const { return db_path_; }
    
    // WAL 操作
    Status BeginWalTx(TxId tx_id);
    Status WriteWalPageImage(TxId tx_id, PageId page_id, const void* data, size_t len);
    Status CommitWalTx(TxId tx_id, const WalCommitPayload& meta_snap);
    Status FlushWal();
    Status RecoverFromWal();
    Status CheckpointWal();

    // v3: 分配溢出页链
    PageId AllocateOverflowChain(const std::vector<uint8_t>& data, std::vector<PageId>& dirty);
    std::vector<uint8_t> ReadOverflowChain(PageId first_page) const;
    void FreeOverflowChain(PageId first_page, TxId obsolete_ver);
    
private:
    std::filesystem::path db_path_;
    size_t map_size_ = 0;
    
    // 内存映射
#ifdef _WIN32
    HANDLE file_ = INVALID_HANDLE_VALUE;
    HANDLE file_mapping_ = nullptr;
    Status Remap();
#else
    int fd_ = -1;
#endif
    uint8_t* mapped_ = nullptr;
    size_t mapped_size_ = 0;
    size_t capacity_ = 0;             // 最大容量（仅在 Windows 用于稀疏文件映射）
    // 页管理
    std::atomic<PageId> num_pages_{0};
    std::mutex alloc_mutex_;
    
    // 空闲页管理 (PageId -> 最早可释放的版本)
    std::mutex freelist_mutex_;
    std::set<std::pair<TxId, PageId>> freelist_;  // 按版本排序
    
    // 元数据页缓存
    MetaPage* meta_ = nullptr;
    
    // WAL
    std::filesystem::path wal_path_;
#ifdef _WIN32
    HANDLE wal_fd_ = INVALID_HANDLE_VALUE;
#endif
    std::vector<uint8_t> wal_buf_;  // 写缓冲
    size_t wal_file_size_ = 0;
    
    // WAL 恢复时暂存的事务状态
    struct WalTxState {
        bool began = false;
        bool committed = false;
        WalCommitPayload meta_snap{};
        // 本事务的 PAGE_IMAGE 记录直接应用到 mapped_，后续不需要单独存储
    };
    
    Status WriteWalRecordRaw(const void* buf, size_t len);
    Status EnsureWalOpen();
    Status InternalWriteWalRecord(WalRecordType type, TxId tx_id, PageId page_id,
                                   const void* body, size_t body_len);
    
    Status EnsureFileSize(size_t size);
    PageId DoAllocate(PageType type);
};

} // namespace graphstore

#endif // PAGE_MANAGER_H

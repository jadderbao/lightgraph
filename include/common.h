#ifndef COMMON_H
#define COMMON_H

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <optional>
#include <memory>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <set>
#include <algorithm>
#include <cstring>
#include <cassert>
#include <fstream>
#include <iostream>
#include <span>
#include <bit>

#ifdef _MSC_VER
#include <intrin.h>
#pragma intrinsic(_byteswap_uint64)
#endif

namespace graphstore {

// ==================== 基础类型 ====================
using TxId      = uint64_t;
using PageId    = uint32_t;
using VertexId  = uint64_t;
using EdgeId    = uint64_t;
using LabelId   = uint32_t;
using Timestamp = uint64_t;

constexpr size_t PAGE_SIZE = 4096;
constexpr PageId INVALID_PAGE = 0xFFFFFFFF;
constexpr TxId   INVALID_TX   = 0xFFFFFFFFFFFFFFFF;

// ==================== FNV-1a 哈希（跨平台确定性的字符串→LabelId 映射） ====================
inline uint32_t Fnv1a(const std::string& s) {
    uint32_t h = 2166136261u;
    for (unsigned char c : s) {
        h ^= c;
        h *= 16777619u;
    }
    return h;
}

// ==================== 状态码 ====================
enum class Status {
    OK = 0,
    ERROR,
    NOT_FOUND,
    ALREADY_EXISTS,
    TX_CONFLICT,
    INVALID_ARGUMENT,
    IO_ERROR,
    CORRUPTION
};

inline bool IsOk(Status s) { return s == Status::OK; }

// ==================== 序列化辅助 ====================
template<typename T>
concept TriviallyCopyable = std::is_trivially_copyable_v<T>;

template<TriviallyCopyable T>
std::vector<uint8_t> Serialize(const T& value) {
    std::vector<uint8_t> buf(sizeof(T));
    std::memcpy(buf.data(), &value, sizeof(T));
    return buf;
}

template<TriviallyCopyable T>
T Deserialize(const std::vector<uint8_t>& buf) {
    T value;
    std::memcpy(&value, buf.data(), sizeof(T));
    return value;
}

// ==================== 字节序转换 ====================
inline uint64_t HostToBig(uint64_t x) {
    if constexpr (std::endian::native == std::endian::little) {
#ifdef _MSC_VER
        return _byteswap_uint64(x);
#else
        return __builtin_bswap64(x);
#endif
    }
    return x;
}

inline uint64_t BigToHost(uint64_t x) {
    return HostToBig(x);
}

// ==================== 键编码辅助 ====================
inline std::vector<uint8_t> EncodeUint64(uint64_t value) {
    auto be = HostToBig(value);
    return Serialize(be);
}

inline uint64_t DecodeUint64(const std::vector<uint8_t>& buf) {
    auto be = Deserialize<uint64_t>(buf);
    return BigToHost(be);
}

// ==================== 属性结构 ====================
struct Properties {
    std::unordered_map<std::string, std::string> data;
    
    std::vector<uint8_t> Serialize() const;
    static Properties Deserialize(std::span<const uint8_t> buf);
    
    bool Has(const std::string& key) const { return data.contains(key); }
    std::optional<std::string> Get(const std::string& key) const;
    void Set(const std::string& key, const std::string& val);
    void Remove(const std::string& key);
};

} // namespace graphstore

#endif // COMMON_H

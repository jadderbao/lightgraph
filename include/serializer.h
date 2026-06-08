#ifndef SERIALIZER_H
#define SERIALIZER_H

#include "common.h"

namespace graphstore {

#pragma pack(push, 1)

// ==================== 顶点负载 ====================
struct VertexPayload {
    VertexId id;
    LabelId  label;
    TxId     create_ver;
    TxId     delete_ver;
    PageId   first_prop_page;  // 属性链起始页
    uint32_t out_degree;
    uint32_t in_degree;
    uint32_t flags;
    
    static constexpr size_t SIZE = 44;
    
    std::vector<uint8_t> Serialize() const;
    static VertexPayload Deserialize(std::span<const uint8_t> buf);
};

static_assert(sizeof(VertexPayload) == VertexPayload::SIZE, "VertexPayload size");

// ==================== 边负载 ====================
struct EdgePayload {
    EdgeId   id;
    VertexId src;
    VertexId dst;
    LabelId  label;
    TxId     create_ver;
    TxId     delete_ver;
    PageId   first_prop_page;
    uint32_t flags;
    
    static constexpr size_t SIZE = 52;
    
    std::vector<uint8_t> Serialize() const;
    static EdgePayload Deserialize(std::span<const uint8_t> buf);
};

static_assert(sizeof(EdgePayload) == EdgePayload::SIZE, "EdgePayload size");

#pragma pack(pop)

// ==================== 键编码器 ====================
class KeyEncoder {
public:
    // 顶点主键: VertexId
    static std::vector<uint8_t> VertexKey(VertexId id);
    
    // 边主键: EdgeId
    static std::vector<uint8_t> EdgeKey(EdgeId id);
    
    // 出边索引: (SrcId, EdgeId)
    static std::vector<uint8_t> OutEdgeKey(VertexId src, EdgeId eid);
    
    // 入边索引: (DstId, EdgeId)
    static std::vector<uint8_t> InEdgeKey(VertexId dst, EdgeId eid);
    
    // 属性键: (OwnerId, PropKey)
    static std::vector<uint8_t> PropertyKey(uint64_t owner_id, const std::string& key);
    
    // v5: 实体属性块键: (OwnerId) — 存储所有属性的单个序列化 blob
    static std::vector<uint8_t> EntityPropertyKey(uint64_t owner_id);
    
    // 标签索引: (LabelId, EntityId)
    static std::vector<uint8_t> LabelKey(LabelId label, uint64_t entity_id);
    
    // v4: 属性索引: (LabelId, PropKey, PropValue, EntityId)
    // 用于 WHERE n.name = "Alice" 的高效查找
    static std::vector<uint8_t> PropIndexKey(LabelId label, const std::string& key,
                                              const std::string& value, uint64_t entity_id);
    // 属性索引前缀: 匹配 label + key 的所有条目
    static std::vector<uint8_t> PropIndexPrefix(LabelId label, const std::string& key);
    // 属性索引精确前缀: 匹配 label + key + value 的所有条目
    static std::vector<uint8_t> PropIndexExactPrefix(LabelId label, const std::string& key,
                                                      const std::string& value);
    
    // 解码辅助
    static VertexId DecodeVertexId(std::span<const uint8_t> key);
    static EdgeId   DecodeEdgeId(std::span<const uint8_t> key);
    static std::pair<VertexId, EdgeId> DecodeOutEdgeKey(std::span<const uint8_t> key);
    static std::pair<VertexId, EdgeId> DecodeInEdgeKey(std::span<const uint8_t> key);
};

} // namespace graphstore
#endif // SERIALIZER_H

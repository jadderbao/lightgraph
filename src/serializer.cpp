#include "serializer.h"
#include <cstring>
#include <cstring>

namespace graphstore {

// ==================== Properties ====================
std::vector<uint8_t> Properties::Serialize() const {
    // 格式: num_pairs(uint32) + [key_len(uint16) + key + val_len(uint16) + val]...
    size_t total = sizeof(uint32_t);
    for (const auto& [k, v] : data) {
        total += sizeof(uint16_t) + k.size() + sizeof(uint16_t) + v.size();
    }
    
    std::vector<uint8_t> buf;
    buf.reserve(total);
    
    uint32_t num = static_cast<uint32_t>(data.size());
    buf.insert(buf.end(), reinterpret_cast<const uint8_t*>(&num), 
               reinterpret_cast<const uint8_t*>(&num) + sizeof(num));
    
    for (const auto& [k, v] : data) {
        uint16_t klen = static_cast<uint16_t>(k.size());
        uint16_t vlen = static_cast<uint16_t>(v.size());
        buf.insert(buf.end(), reinterpret_cast<const uint8_t*>(&klen), 
                   reinterpret_cast<const uint8_t*>(&klen) + sizeof(klen));
        buf.insert(buf.end(), k.begin(), k.end());
        buf.insert(buf.end(), reinterpret_cast<const uint8_t*>(&vlen), 
                   reinterpret_cast<const uint8_t*>(&vlen) + sizeof(vlen));
        buf.insert(buf.end(), v.begin(), v.end());
    }
    return buf;
}

Properties Properties::Deserialize(std::span<const uint8_t> buf) {
    Properties props;
    if (buf.size() < sizeof(uint32_t)) return props;
    
    const uint8_t* p = buf.data();
    const uint8_t* end = buf.data() + buf.size();
    
    uint32_t num;
    std::memcpy(&num, p, sizeof(num));
    p += sizeof(num);
    
    for (uint32_t i = 0; i < num && p < end; ++i) {
        if (p + sizeof(uint16_t) > end) break;
        uint16_t klen;
        std::memcpy(&klen, p, sizeof(klen));
        p += sizeof(klen);
        
        if (p + klen > end) break;
        std::string key(reinterpret_cast<const char*>(p), klen);
        p += klen;
        
        if (p + sizeof(uint16_t) > end) break;
        uint16_t vlen;
        std::memcpy(&vlen, p, sizeof(vlen));
        p += sizeof(vlen);
        
        if (p + vlen > end) break;
        std::string val(reinterpret_cast<const char*>(p), vlen);
        p += vlen;
        
        props.data[key] = val;
    }
    return props;
}

std::optional<std::string> Properties::Get(const std::string& key) const {
    auto it = data.find(key);
    if (it != data.end()) return it->second;
    return std::nullopt;
}

void Properties::Set(const std::string& key, const std::string& val) {
    data[key] = val;
}

void Properties::Remove(const std::string& key) {
    data.erase(key);
}

// ==================== VertexPayload ====================
std::vector<uint8_t> VertexPayload::Serialize() const {
    std::vector<uint8_t> buf(SIZE);
    std::memcpy(buf.data(), this, SIZE);
    return buf;
}

VertexPayload VertexPayload::Deserialize(std::span<const uint8_t> buf) {
    VertexPayload payload{};
    if (buf.size() >= SIZE) {
        std::memcpy(&payload, buf.data(), SIZE);
    }
    return payload;
}

// ==================== EdgePayload ====================
std::vector<uint8_t> EdgePayload::Serialize() const {
    std::vector<uint8_t> buf(SIZE);
    std::memcpy(buf.data(), this, SIZE);
    return buf;
}

EdgePayload EdgePayload::Deserialize(std::span<const uint8_t> buf) {
    EdgePayload payload{};
    if (buf.size() >= SIZE) {
        std::memcpy(&payload, buf.data(), SIZE);
    }
    return payload;
}

// ==================== KeyEncoder ====================
std::vector<uint8_t> KeyEncoder::VertexKey(VertexId id) {
    return EncodeUint64(id);
}

std::vector<uint8_t> KeyEncoder::EdgeKey(EdgeId id) {
    return EncodeUint64(id);
}

std::vector<uint8_t> KeyEncoder::OutEdgeKey(VertexId src, EdgeId eid) {
    auto key = EncodeUint64(src);
    auto eid_enc = EncodeUint64(eid);
    key.insert(key.end(), eid_enc.begin(), eid_enc.end());
    return key;
}

std::vector<uint8_t> KeyEncoder::InEdgeKey(VertexId dst, EdgeId eid) {
    auto key = EncodeUint64(dst);
    auto eid_enc = EncodeUint64(eid);
    key.insert(key.end(), eid_enc.begin(), eid_enc.end());
    return key;
}

std::vector<uint8_t> KeyEncoder::PropertyKey(uint64_t owner_id, const std::string& prop_key) {
    auto key = EncodeUint64(owner_id);
    key.insert(key.end(), prop_key.begin(), prop_key.end());
    return key;
}

std::vector<uint8_t> KeyEncoder::EntityPropertyKey(uint64_t owner_id) {
    return EncodeUint64(owner_id);
}

std::vector<uint8_t> KeyEncoder::LabelKey(LabelId label, uint64_t entity_id) {
    auto key = EncodeUint64(static_cast<uint64_t>(label));
    auto eid_enc = EncodeUint64(entity_id);
    key.insert(key.end(), eid_enc.begin(), eid_enc.end());
    return key;
}

std::vector<uint8_t> KeyEncoder::PropIndexKey(LabelId label, const std::string& key,
                                               const std::string& value, uint64_t entity_id) {
    auto buf = EncodeUint64(static_cast<uint64_t>(label));
    buf.insert(buf.end(), key.begin(), key.end());
    buf.push_back('\0');
    buf.insert(buf.end(), value.begin(), value.end());
    buf.push_back('\0');
    auto eid_enc = EncodeUint64(entity_id);
    buf.insert(buf.end(), eid_enc.begin(), eid_enc.end());
    return buf;
}

std::vector<uint8_t> KeyEncoder::PropIndexPrefix(LabelId label, const std::string& key) {
    auto buf = EncodeUint64(static_cast<uint64_t>(label));
    buf.insert(buf.end(), key.begin(), key.end());
    buf.push_back('\0');
    return buf;
}

std::vector<uint8_t> KeyEncoder::PropIndexExactPrefix(LabelId label, const std::string& key,
                                                       const std::string& value) {
    auto buf = EncodeUint64(static_cast<uint64_t>(label));
    buf.insert(buf.end(), key.begin(), key.end());
    buf.push_back('\0');
    buf.insert(buf.end(), value.begin(), value.end());
    buf.push_back('\0');
    return buf;
}

VertexId KeyEncoder::DecodeVertexId(std::span<const uint8_t> key) {
    if (key.size() >= sizeof(uint64_t)) {
        return DecodeUint64(std::vector<uint8_t>(key.begin(), key.begin() + sizeof(uint64_t)));
    }
    return 0;
}

EdgeId KeyEncoder::DecodeEdgeId(std::span<const uint8_t> key) {
    if (key.size() >= sizeof(uint64_t)) {
        return DecodeUint64(std::vector<uint8_t>(key.begin(), key.begin() + sizeof(uint64_t)));
    }
    return 0;
}

std::pair<VertexId, EdgeId> KeyEncoder::DecodeOutEdgeKey(std::span<const uint8_t> key) {
    if (key.size() >= 2 * sizeof(uint64_t)) {
        VertexId src = DecodeUint64(std::vector<uint8_t>(key.begin(), key.begin() + sizeof(uint64_t)));
        EdgeId eid = DecodeUint64(std::vector<uint8_t>(key.begin() + sizeof(uint64_t), key.begin() + 2 * sizeof(uint64_t)));
        return {src, eid};
    }
    return {0, 0};
}

std::pair<VertexId, EdgeId> KeyEncoder::DecodeInEdgeKey(std::span<const uint8_t> key) {
    // 与 OutEdgeKey 编码相同
    return DecodeOutEdgeKey(key);
}

} // namespace graphstore

#pragma once
#include <cstdint>
#include <string>

namespace codec
{
// 网络协议统一使用大端序。调用前由协议层确认 data 至少有 4 字节；
// 这里仅负责字节与 uint32_t 之间的转换，不消费 Buffer 的读位置。
inline uint32_t ReadUint32(const char *data)
{
    return (static_cast<uint8_t>(data[0]) << 24) |
           (static_cast<uint8_t>(data[1]) << 16) |
           (static_cast<uint8_t>(data[2]) << 8) |
           static_cast<uint8_t>(data[3]);
}

inline void AppendUint32(std::string &out, uint32_t value)
{
    out.push_back(static_cast<char>(value >> 24));
    out.push_back(static_cast<char>((value >> 16) & 0xff));
    out.push_back(static_cast<char>((value >> 8) & 0xff));
    out.push_back(static_cast<char>(value & 0xff));
}

// TLV 字段格式：[Type(1)][Length(4)][Value]。Type 的具体含义
// 仍由业务层定义，这里只负责共同的字段编解码。
inline void AppendField(std::string &out, uint8_t type,
                        const std::string &value)
{
    out.push_back(static_cast<char>(type));
    AppendUint32(out, static_cast<uint32_t>(value.size()));
    out.append(value);
}

// pos 指向当前字段；成功后移到下一个字段，字段不完整
// 或 Length 超出剩余数据时返回 false。
inline bool ReadField(const std::string &payload, size_t &pos,
                      uint8_t &type, std::string &value)
{
    constexpr size_t kHeaderLength = 5;
    if (pos > payload.size() || payload.size() - pos < kHeaderLength)
    {
        return false;
    }

    type = static_cast<uint8_t>(payload[pos]);
    uint32_t length = ReadUint32(payload.data() + pos + 1);
    pos += kHeaderLength;
    if (length > payload.size() - pos)
    {
        return false;
    }

    value.assign(payload.data() + pos, length);
    pos += length;
    return true;
}
} // namespace codec


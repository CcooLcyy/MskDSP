#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include <grpcpp/support/status.h>

namespace IEC61850 {

// 一份BER TLV的只读视图；value不拥有底层内存。
struct BerTlvView {
  // 低标签时tag保存完整Identifier Octet；高标签时tag仍保存首个
  // Identifier Octet，tagNumber保存实际标签号。
  std::uint32_t tag = 0;
  std::uint32_t tagNumber = 0;
  std::uint8_t identifierOctet = 0;
  std::span<const std::uint8_t> value;
  std::size_t encodedSize = 0;
};

// 从input[offset]读取一份确定长度TLV，并推进offset。
grpc::Status ReadBerTlv(std::span<const std::uint8_t> input,
                        std::size_t* offset, BerTlvView* tlv);

grpc::Status ReadBerBoolean(std::span<const std::uint8_t> value,
                            bool* result);
grpc::Status ReadBerUnsigned(std::span<const std::uint8_t> value,
                             std::uint64_t* result);
grpc::Status ReadBerSigned(std::span<const std::uint8_t> value,
                           std::int64_t* result);
grpc::Status ReadBerOid(std::span<const std::uint8_t> value,
                        std::span<std::uint32_t> arcs,
                        std::size_t* arcCount);

// 有界BER写入器；所有输出空间由调用方预分配。
class BerWriter {
public:
  explicit BerWriter(std::span<std::uint8_t> output) : output_(output) {}

  bool Tlv(std::uint32_t tag, std::span<const std::uint8_t> value) noexcept;
  bool Boolean(std::uint8_t tag, bool value) noexcept;
  bool Unsigned(std::uint8_t tag, std::uint64_t value) noexcept;
  bool Signed(std::uint8_t tag, std::int64_t value) noexcept;
  bool Oid(std::uint8_t tag, std::span<const std::uint32_t> arcs) noexcept;

  std::size_t size() const noexcept { return offset_; }
  std::span<const std::uint8_t> written() const noexcept {
    return output_.first(offset_);
  }

private:
  bool Byte(std::uint8_t value) noexcept;
  bool Tag(std::uint32_t tag) noexcept;
  bool Length(std::size_t value) noexcept;

  std::span<std::uint8_t> output_;
  std::size_t offset_ = 0;
};

}  // namespace IEC61850

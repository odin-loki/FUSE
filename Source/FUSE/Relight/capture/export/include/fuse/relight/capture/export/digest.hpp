// FUSE Relight RL-1.8: content digests of the capture store.
//
//   SHA-256 (FIPS 180-4)  blob addresses (<store>/blobs/sha256/<ab>/<sha256>) and the
//                         "fuse.capture.sha256" canonical keys (Remaster plan §1.3, §2.1);
//   SHA-1 (FIPS 180-4)    only for RFC 4122 name-based UUIDv5, the Remaster `oaid`
//                         (UUIDv5 of game id + first strong hash).
// Plain implementations of the published algorithms; no third-party code.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace fuse::relight::capture::exporter {

class Sha256 {
public:
    Sha256();
    void update(const void* data, std::size_t size);
    std::array<std::uint8_t, 32> digest();
    std::string hexDigest();

private:
    void block(const std::uint8_t* p);
    std::array<std::uint32_t, 8> m_h;
    std::array<std::uint8_t, 64> m_buf{};
    std::size_t m_used = 0;
    std::uint64_t m_bytes = 0;
};

class Sha1 {
public:
    Sha1();
    void update(const void* data, std::size_t size);
    std::array<std::uint8_t, 20> digest();

private:
    void block(const std::uint8_t* p);
    std::array<std::uint32_t, 5> m_h;
    std::array<std::uint8_t, 64> m_buf{};
    std::size_t m_used = 0;
    std::uint64_t m_bytes = 0;
};

/// Lower-case hex SHA-256 of `data`.
std::string sha256Hex(const void* data, std::size_t size);
inline std::string sha256Hex(std::span<const std::uint8_t> data) { return sha256Hex(data.data(), data.size()); }

/// Lower-case hex of arbitrary bytes.
std::string toHex(const std::uint8_t* data, std::size_t size);

using Uuid = std::array<std::uint8_t, 16>;
/// RFC 4122 §4.3 name-based UUID, version 5 (SHA-1), of `name` in `ns`.
Uuid uuidV5(const Uuid& ns, std::string_view name);
/// "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" (lower case).
std::string uuidString(const Uuid& u);
/// RFC 4122 Appendix C NameSpace_URL (6ba7b811-9dad-11d1-80b4-00c04fd430c8).
Uuid uuidNamespaceUrl();

} // namespace fuse::relight::capture::exporter

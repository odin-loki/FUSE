// FUSE Relight RL-1.1: SHA-256 (FIPS 180-4) for the recording tap's content keys. Same keys as
// the RL-0.4 app sidecars (lower-case hex of the bytes' SHA-256).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace fuse::relight::tap::detail {

class Sha256 {
public:
    Sha256();
    void update(const void* data, std::size_t size);
    /// Finishes the digest; the object must not be updated afterwards.
    std::string hexDigest();

private:
    void block(const std::uint8_t* p);
    std::uint32_t m_h[8];
    std::uint8_t m_buf[64];
    std::size_t m_bufLen = 0;
    std::uint64_t m_totalBytes = 0;
};

std::string sha256Hex(const void* data, std::size_t size);

} // namespace fuse::relight::tap::detail

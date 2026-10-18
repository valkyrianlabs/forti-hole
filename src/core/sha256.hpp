//
// SHA-256 (FIPS 180-4), incremental. Feed content hashes and cache integrity; no crypto library
// dependency for a hash this small.
//

#ifndef FORTI_HOLE_CORE_SHA256_HPP
#define FORTI_HOLE_CORE_SHA256_HPP

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace fh {

    class Sha256 {
        std::array<std::uint32_t, 8> state_;
        std::array<unsigned char, 64> block_{};
        std::size_t used_ = 0;
        std::uint64_t length_ = 0;

        void compress(const unsigned char* block);

    public:
        Sha256();
        Sha256& update(std::string_view data);
        // Lowercase hex of the digest. The object is spent afterwards.
        std::string hex();
    };

    [[nodiscard]] std::string sha256_hex(std::string_view data);

}  // namespace fh

#endif  // FORTI_HOLE_CORE_SHA256_HPP

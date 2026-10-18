#include "core/sha256.hpp"

#include <algorithm>
#include <bit>

namespace fh {

    namespace {
        constexpr std::array<std::uint32_t, 64> k = {
                0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
                0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
                0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
                0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
                0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
                0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
                0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    }

    Sha256::Sha256() : state_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19} {}

    void Sha256::compress(const unsigned char* block) {
        std::array<std::uint32_t, 64> w{};
        for (std::size_t i = 0; i < 16; ++i)
            w[i] = (std::uint32_t(block[4 * i]) << 24) | (std::uint32_t(block[4 * i + 1]) << 16) | (std::uint32_t(block[4 * i + 2]) << 8) |
                   std::uint32_t(block[4 * i + 3]);
        for (std::size_t i = 16; i < 64; ++i) {
            std::uint32_t s0 = std::rotr(w[i - 15], 7) ^ std::rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            std::uint32_t s1 = std::rotr(w[i - 2], 17) ^ std::rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        auto [a, b, c, d, e, f, g, h] = state_;
        for (std::size_t i = 0; i < 64; ++i) {
            std::uint32_t t1 = h + (std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            std::uint32_t t2 = (std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    Sha256& Sha256::update(std::string_view data) {
        length_ += data.size();
        auto* bytes = reinterpret_cast<const unsigned char*>(data.data());
        std::size_t n = data.size();
        if (used_ > 0) {
            std::size_t take = std::min(n, 64 - used_);
            std::copy_n(bytes, take, block_.begin() + static_cast<std::ptrdiff_t>(used_));
            used_ += take;
            bytes += take;
            n -= take;
            if (used_ < 64) return *this;
            compress(block_.data());
            used_ = 0;
        }
        for (; n >= 64; bytes += 64, n -= 64) compress(bytes);
        std::copy_n(bytes, n, block_.begin());
        used_ = n;
        return *this;
    }

    std::string Sha256::hex() {
        std::uint64_t bits = length_ * 8;
        block_[used_++] = 0x80;
        if (used_ > 56) {
            std::fill(block_.begin() + static_cast<std::ptrdiff_t>(used_), block_.end(), 0);
            compress(block_.data());
            used_ = 0;
        }
        std::fill(block_.begin() + static_cast<std::ptrdiff_t>(used_), block_.begin() + 56, 0);
        for (std::size_t i = 0; i < 8; ++i) block_[56 + i] = static_cast<unsigned char>(bits >> (56 - 8 * i));
        compress(block_.data());
        static constexpr char digits[] = "0123456789abcdef";
        std::string out;
        out.reserve(64);
        for (std::uint32_t word: state_)
            for (int shift = 28; shift >= 0; shift -= 4) out.push_back(digits[(word >> shift) & 0xF]);
        return out;
    }

    std::string sha256_hex(std::string_view data) { return Sha256().update(data).hex(); }

}  // namespace fh

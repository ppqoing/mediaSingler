#include "CoreModels.h"

namespace videosc::dedup {
namespace {

/** @brief 把单个十六进制字符转换为 0 到 15，非法字符返回 -1。 */
int HexValue(const char value) {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

}  // namespace

std::string Sha512ToHex(const Sha512Digest& digest) {
    constexpr char kHexDigits[] = "0123456789abcdef";
    std::string result(digest.size() * 2, '0');
    for (std::size_t index = 0; index < digest.size(); ++index) {
        result[index * 2] = kHexDigits[digest[index] >> 4];
        result[index * 2 + 1] = kHexDigits[digest[index] & 0x0F];
    }
    return result;
}

std::optional<Sha512Digest> Sha512FromHex(const std::string& hex) {
    Sha512Digest result{};
    if (hex.size() != result.size() * 2) {
        return std::nullopt;
    }
    for (std::size_t index = 0; index < result.size(); ++index) {
        const int high = HexValue(hex[index * 2]);
        const int low = HexValue(hex[index * 2 + 1]);
        if (high < 0 || low < 0) {
            return std::nullopt;
        }
        result[index] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return result;
}

}  // namespace videosc::dedup

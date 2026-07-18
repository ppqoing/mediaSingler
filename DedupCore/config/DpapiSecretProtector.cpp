#include "DpapiSecretProtector.h"

#include <Windows.h>
#include <dpapi.h>
#include <wincrypt.h>

#include <stdexcept>
#include <vector>

#pragma comment(lib, "Crypt32.lib")

namespace videosc::dedup {
namespace {

constexpr char kDpapiDescription[] = "VideoSc MySQL password";
constexpr unsigned char kEntropyBytes[] = {
    'V', 'i', 'd', 'e', 'o', 'S', 'c', '.', 'C', 'o', 'n', 'f', 'i', 'g', '.',
    'M', 'y', 'S', 'q', 'l', 'P', 'a', 's', 's', 'w', 'o', 'r', 'd', '.', 'v', '1'};

/** @brief 将 UTF-16 密码转换成 UTF-8，避免 DPAPI 明文格式受 wchar_t 宽度影响。 */
std::vector<unsigned char> WideToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int byte_count = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (byte_count <= 0) {
        throw std::runtime_error("Failed to encode secret as UTF-8");
    }
    std::vector<unsigned char> bytes(static_cast<std::size_t>(byte_count));
    if (WideCharToMultiByte(CP_UTF8,
                            WC_ERR_INVALID_CHARS,
                            value.data(),
                            static_cast<int>(value.size()),
                            reinterpret_cast<char*>(bytes.data()),
                            byte_count,
                            nullptr,
                            nullptr) != byte_count) {
        SecureZeroMemory(bytes.data(), bytes.size());
        throw std::runtime_error("Failed to encode secret as UTF-8");
    }
    return bytes;
}

/** @brief 将 DPAPI 解密出的 UTF-8 字节恢复为 UTF-16。 */
std::wstring Utf8ToWide(const unsigned char* bytes, const std::size_t byte_count) {
    if (byte_count == 0) {
        return {};
    }
    const int wchar_count = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, reinterpret_cast<const char*>(bytes), static_cast<int>(byte_count), nullptr, 0);
    if (wchar_count <= 0) {
        throw std::runtime_error("Failed to decode protected secret as UTF-8");
    }
    std::wstring value(static_cast<std::size_t>(wchar_count), L'\0');
    if (MultiByteToWideChar(CP_UTF8,
                            MB_ERR_INVALID_CHARS,
                            reinterpret_cast<const char*>(bytes),
                            static_cast<int>(byte_count),
                            value.data(),
                            wchar_count) != wchar_count) {
        throw std::runtime_error("Failed to decode protected secret as UTF-8");
    }
    return value;
}

/** @brief 对二进制密文进行无换行 Base64 编码。 */
std::string EncodeBase64(const unsigned char* bytes, const DWORD byte_count) {
    DWORD char_count = 0;
    constexpr DWORD flags = CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF;
    if (!CryptBinaryToStringA(bytes, byte_count, flags, nullptr, &char_count)) {
        throw std::runtime_error("Failed to calculate Base64 length");
    }
    std::string encoded(char_count, '\0');
    if (!CryptBinaryToStringA(bytes, byte_count, flags, encoded.data(), &char_count)) {
        throw std::runtime_error("Failed to encode Base64");
    }
    if (!encoded.empty() && encoded.back() == '\0') {
        encoded.pop_back();
    }
    return encoded;
}

/** @brief 解码无换行 Base64，并拒绝包含无法解析字符的输入。 */
std::vector<unsigned char> DecodeBase64(const std::string& encoded) {
    DWORD byte_count = 0;
    if (!CryptStringToBinaryA(encoded.c_str(),
                              static_cast<DWORD>(encoded.size()),
                              CRYPT_STRING_BASE64,
                              nullptr,
                              &byte_count,
                              nullptr,
                              nullptr)) {
        throw std::runtime_error("Invalid Base64 protected secret");
    }
    std::vector<unsigned char> bytes(byte_count);
    if (!CryptStringToBinaryA(encoded.c_str(),
                              static_cast<DWORD>(encoded.size()),
                              CRYPT_STRING_BASE64,
                              bytes.data(),
                              &byte_count,
                              nullptr,
                              nullptr)) {
        throw std::runtime_error("Invalid Base64 protected secret");
    }
    bytes.resize(byte_count);
    return bytes;
}

}  // namespace

std::string DpapiSecretProtector::Protect(const std::wstring& plaintext) {
    std::vector<unsigned char> plaintext_bytes = WideToUtf8(plaintext);
    DATA_BLOB input{
        static_cast<DWORD>(plaintext_bytes.size()), plaintext_bytes.empty() ? nullptr : plaintext_bytes.data()};
    DATA_BLOB entropy{static_cast<DWORD>(sizeof(kEntropyBytes)), const_cast<BYTE*>(kEntropyBytes)};
    DATA_BLOB output{};

    if (!CryptProtectData(&input,
                          L"VideoSc MySQL password",
                          &entropy,
                          nullptr,
                          nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN,
                          &output)) {
        if (!plaintext_bytes.empty()) {
            SecureZeroMemory(plaintext_bytes.data(), plaintext_bytes.size());
        }
        throw std::runtime_error("CryptProtectData failed");
    }

    std::string encoded;
    try {
        encoded = EncodeBase64(output.pbData, output.cbData);
    } catch (...) {
        SecureZeroMemory(output.pbData, output.cbData);
        LocalFree(output.pbData);
        if (!plaintext_bytes.empty()) {
            SecureZeroMemory(plaintext_bytes.data(), plaintext_bytes.size());
        }
        throw;
    }

    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    if (!plaintext_bytes.empty()) {
        SecureZeroMemory(plaintext_bytes.data(), plaintext_bytes.size());
    }
    return encoded;
}

std::wstring DpapiSecretProtector::Unprotect(const std::string& protected_base64) {
    if (protected_base64.empty()) {
        return {};
    }
    std::vector<unsigned char> encrypted = DecodeBase64(protected_base64);
    DATA_BLOB input{static_cast<DWORD>(encrypted.size()), encrypted.data()};
    DATA_BLOB entropy{static_cast<DWORD>(sizeof(kEntropyBytes)), const_cast<BYTE*>(kEntropyBytes)};
    DATA_BLOB output{};

    if (!CryptUnprotectData(&input,
                            nullptr,
                            &entropy,
                            nullptr,
                            nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN,
                            &output)) {
        SecureZeroMemory(encrypted.data(), encrypted.size());
        throw std::runtime_error("CryptUnprotectData failed");
    }

    std::wstring plaintext;
    try {
        plaintext = Utf8ToWide(output.pbData, output.cbData);
    } catch (...) {
        SecureZeroMemory(output.pbData, output.cbData);
        LocalFree(output.pbData);
        SecureZeroMemory(encrypted.data(), encrypted.size());
        throw;
    }
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    SecureZeroMemory(encrypted.data(), encrypted.size());
    return plaintext;
}

}  // namespace videosc::dedup

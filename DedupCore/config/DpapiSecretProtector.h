#pragma once

#include <string>

namespace videosc::dedup {

/**
 * @brief 使用 Windows DPAPI CurrentUser 范围保护 MySQL 密码。
 *
 * 密文以无换行 Base64 返回，只有同一 Windows 用户上下文可以正常解密。
 */
class DpapiSecretProtector final {
public:
    /**
     * @brief 加密内存中的密码。
     * @param plaintext 明文密码；空字符串会被保护为空数据，不会直接写入 JSON。
     * @return Base64 编码的 DPAPI 密文。
     * @throws std::runtime_error DPAPI 或 Base64 编码失败时抛出。
     */
    static std::string Protect(const std::wstring& plaintext);

    /**
     * @brief 解密 Base64 DPAPI 密文。
     * @param protected_base64 JSON 中保存的密文。
     * @return 解密后的密码。
     * @throws std::runtime_error Base64 非法、用户不匹配或密文损坏时抛出。
     */
    static std::wstring Unprotect(const std::string& protected_base64);
};

}  // namespace videosc::dedup

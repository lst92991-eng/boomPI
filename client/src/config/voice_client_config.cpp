/** @file voice_client_config.cpp
 * @brief 应用启动配置：必要时创建设备身份 → 读文件 → 检查字段 → 交付设备身份与已配对端点。
 *
 * 程序从固定板端路径读取client.conf，在首次运行时生成并保存设备身份。
 * 本文件只校验格式；建立网络连接及验证服务端身份由网络模块完成。
 */
#include "boompi/config/voice_client_config.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <string_view>
#include <utility>

namespace config
{
static const char kConfigPath[] = "/userdata/boompi/config/client.conf";

/** @brief 配置缺失时由内核生成设备UUID，连同后续配对信息使用同一文件保存。 */
static bool create_config()
{
    for (const char *directory : {"/userdata/boompi", "/userdata/boompi/config"})
    {
        if (mkdir(directory, 0700) < 0 && errno != EEXIST)
        {
            return false;
        }
    }
    std::ifstream random_id("/proc/sys/kernel/random/uuid");
    std::string id;
    if (!(random_id >> id) || !IsValidDeviceId(id))
    {
        return false;
    }
    VoiceClientConfig settings;
    settings.device_id = std::move(id);
    return SaveClientConfig(settings);
}

/** @brief 完整解析无符号十进制字段并检查上限，不接受尾随字符；成功时 number 可用。 */
static bool decimal(std::string_view text, unsigned limit, unsigned &number)
{
    const auto end = text.data() + text.size();
    const auto parsed = std::from_chars(text.data(), end, number);
    return !text.empty() && parsed.ec == std::errc{} && parsed.ptr == end && number <= limit;
}

/** @brief 按四段 0..255 检查 IPv4，不接受前导零或缺段，以免不同解析器理解不一致。 */
static bool ipv4(std::string_view text)
{
    for (unsigned part = 0; part < 4; ++part)
    {
        const auto dot = text.find('.');
        const auto word = text.substr(0, dot);
        unsigned octet = 0;
        if (!decimal(word, 255, octet) || (word.size() > 1 && word.front() == '0') ||
            ((part == 3) != (dot == std::string_view::npos)))
        {
            return false;
        }
        if (part != 3)
        {
            text.remove_prefix(dot + 1);
        }
    }
    return true;
}

/** @brief 返回标准 Base64 字符值，非法字符返回 -1；用于检查 pin 的规范形式。 */
static int base64(char c)
{
    const std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const auto at = alphabet.find(c);
    return at == std::string_view::npos ? -1 : static_cast<int>(at);
}
bool IsValidDeviceId(std::string_view text)
{
    if (text.size() != 36)
    {
        return false;
    }
    bool nonzero = false;
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        const char c = text[i];
        if (dash)
        {
            if (c != '-')
            {
                return false;
            }
            continue;
        }
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!hex)
        {
            return false;
        }
        nonzero = nonzero || c != '0';
    }
    return nonzero;
}

bool IsValidSpkiSha256(std::string_view text)
{
    // 32字节的标准Base64，末数据字符低两位必须为0；TLS仍另验服务器持有此公钥。
    return text.size() == 44 && text.back() == '=' &&
           std::all_of(text.begin(), text.end() - 1,
                       [](char c)
                       {
                           return base64(c) >= 0;
                       }) &&
           (base64(text[42]) & 3) == 0;
}

bool LoadClientConfig(VoiceClientConfig *output, std::string *error)
{
    const auto fail = [error](const char *field)
    {
        if (error)
        {
            *error = std::string(field) + " is invalid";
        }
        return false;
    };
    if (!output)
    {
        return fail("configuration output");
    }
    *output = {};
    // 1. 文件缺失时创建配置；其他读取错误直接报告，保持既有设备身份。
    std::ifstream file(kConfigPath);
    if (!file)
    {
        if (errno != ENOENT || !create_config())
        {
            return fail("client.conf access");
        }
        file.clear();
        file.open(kConfigPath);
    }
    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    if (size <= 0 || size > 4096)
    {
        return fail("client.conf size");
    }
    file.seekg(0);
    std::string id, ip, pin, port, line;
    // 2. 四个字段先读入局部变量，完成全部校验后再交付网络使用。
    while (std::getline(file, line))
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        if (line.empty() || line.front() == '#')
        {
            continue;
        }
        const auto equal = line.find('=');
        if (equal == std::string::npos)
        {
            return fail("client.conf format");
        }
        const auto name = std::string_view(line).substr(0, equal);
        const auto value = std::string_view(line).substr(equal + 1);
        if (name == "device_id")
        {
            id = value;
        }
        else if (name == "server_ip")
        {
            ip = value;
        }
        else if (name == "server_port")
        {
            port = value;
        }
        else if (name == "server_spki_sha256")
        {
            pin = value;
        }
        else
        {
            return fail("client.conf field");
        }
    }
    if (file.bad())
    {
        return fail("client.conf read");
    }
    // 升级时把原server.conf的配对身份迁入统一配置，保留已信任的公钥。
    const char *legacy_path = "/userdata/boompi/config/server.conf";
    bool migrated = false;
    if (ip.empty() && pin.empty())
    {
        std::ifstream legacy(legacy_path);
        if (legacy)
        {
            std::string extra;
            if (!(legacy >> ip >> port >> pin) || (legacy >> extra))
            {
                return fail("saved server identity");
            }
            migrated = true;
        }
        else if (errno != ENOENT)
        {
            return fail("saved server identity access");
        }
    }
    unsigned number = output->server_port;
    // 3. 全部字段通过后一次交付；失败提示只包含字段名，不打印配置内容。
    if (!IsValidDeviceId(id))
    {
        return fail("device_id");
    }
    if (!ip.empty() && (ip.size() > 15 || !ipv4(ip)))
    {
        return fail("server_ip");
    }
    if (ip.empty() != pin.empty() || (!pin.empty() && !IsValidSpkiSha256(pin)))
    {
        return fail("server_spki_sha256");
    }
    if (!port.empty() && (port.size() > 5 || !decimal(port, 65535, number) || number == 0))
    {
        return fail("server_port");
    }
    *output = {std::move(id), std::move(ip), static_cast<std::uint16_t>(number),
               std::move(pin)};
    if (migrated)
    {
        if (!SaveClientConfig(*output))
        {
            return fail("server identity migration");
        }
        unlink(legacy_path);
    }
    if (error)
    {
        error->clear();
    }
    return true;
}
bool SaveClientConfig(const VoiceClientConfig &settings)
{
    // 写完整临时文件后再替换；保存失败时原设备身份和配对记录仍可使用。
    const char *temporary = "/userdata/boompi/config/client.conf.tmp";
    const int fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    std::FILE *file = fd < 0 ? nullptr : fdopen(fd, "w");
    bool ok = file && fchmod(fd, 0600) == 0 &&
              std::fprintf(file, "device_id=%s\n", settings.device_id.c_str()) > 0;
    if (ok && !settings.server_ip.empty())
    {
        ok = std::fprintf(file, "server_ip=%s\nserver_port=%u\nserver_spki_sha256=%s\n",
                          settings.server_ip.c_str(), settings.server_port,
                          settings.server_spki_sha256.c_str()) > 0;
    }
    if (file)
    {
        ok = std::fflush(file) == 0 && fsync(fd) == 0 && ok;
        ok = std::fclose(file) == 0 && ok;
    }
    else if (fd >= 0)
    {
        close(fd);
    }
    if (ok && std::rename(temporary, kConfigPath) == 0)
    {
        return true;
    }
    unlink(temporary);
    return false;
}
}  // namespace config

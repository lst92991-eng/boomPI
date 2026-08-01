#ifndef BOOMPI_EVENT_STATUS_H_
#define BOOMPI_EVENT_STATUS_H_

#include <cstdint>
#include <string>

namespace boompi {

enum class StatusCode : std::uint8_t {
  kOk = 0,             ///< 操作完成。
  kInvalidArgument,    ///< 调用参数或启动配置不满足契约。
  kResourceExhausted,  ///< 内存、线程或其他进程资源不足。
  kInternal,           ///< 模块内部或第三方后端发生不可恢复错误。
};

/// @brief 跨模块同步启动/停止错误；实时音频帧和网络事件不使用本类型传递。
class Status final {
 public:
  Status() = default;

  /// @brief 创建不携带文本的成功状态。
  static Status Ok();

  /// @brief 创建错误状态。
  ///
  /// `code == kOk` 会被转换为 `kInternal`；`message` 按原样接收，因此调用方负责提供非空且
  /// 不含令牌、密钥或原始音频的说明。
  static Status Error(StatusCode code, std::string message);

  /// @brief 返回状态是否成功。
  bool ok() const noexcept;

  /// @brief 返回诊断文本；成功状态通常为空，引用只在本 Status 生命周期内有效。
  const std::string& message() const noexcept;

 private:
  Status(StatusCode code, std::string message);

  StatusCode code_{StatusCode::kOk};
  std::string message_;
};

}  // namespace boompi

#endif  // BOOMPI_EVENT_STATUS_H_

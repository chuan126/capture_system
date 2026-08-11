#ifndef RTK_DRIVER__SERIAL_DISCOVERY_HPP_
#define RTK_DRIVER__SERIAL_DISCOVERY_HPP_

#include <chrono>
#include <string>
#include <vector>

namespace rtk_driver
{

struct SerialDiscoveryResult
{
  std::string device;
  std::vector<std::string> candidates;
  std::string detail;
};

// 列举稳定串口路径。优先返回 /dev/serial/by-id 下的入口；该目录没有候选时，
// 再退化到 /dev/ttyUSB* 和 /dev/ttyACM*。返回值按路径排序并去重。
std::vector<std::string> list_serial_candidates(
  const std::string & by_id_directory = "/dev/serial/by-id",
  const std::string & device_directory = "/dev");

// 从候选路径中选择唯一匹配项。preferred_tokens 只与路径文件名做大小写不敏感的
// 子串匹配；返回空字符串表示当前无法唯一确定设备。
std::string select_unique_serial_candidate(
  const std::vector<std::string> & candidates,
  const std::vector<std::string> & preferred_tokens);

// 只用于多串口歧义时的设备识别，不替代 NMEA0183 解析器。检测典型 GNSS 文本帧
// 起始标记，例如 $GN/$GP/$BD 和 NovAtel #BESTPOSA。
bool contains_gnss_stream_signature(const std::string & data);

// 自动发现 RTK 串口。自动模式始终要求候选在探测窗口内出现 GNSS 文本特征。
// 仅有唯一串口候选，或 preferred_tokens 在多候选中唯一命中时才会主动打开探测；
// 多个未知串口不会逐口尝试，避免改变其他串口的通信参数。无法唯一确定时不猜测。
SerialDiscoveryResult discover_rtk_serial_device(
  int baud_rate,
  const std::vector<std::string> & preferred_tokens,
  std::chrono::milliseconds probe_duration,
  const std::string & by_id_directory = "/dev/serial/by-id",
  const std::string & device_directory = "/dev");

}  // namespace rtk_driver

#endif  // RTK_DRIVER__SERIAL_DISCOVERY_HPP_

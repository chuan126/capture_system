#include "rtk_driver/serial_discovery.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cctype>
#include <filesystem>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "rtk_driver/serial_port.hpp"

namespace rtk_driver
{
namespace
{

std::string lowercase(std::string value)
{
  std::transform(
    value.begin(), value.end(), value.begin(),
    [](const unsigned char value_char) {return static_cast<char>(std::tolower(value_char));});
  return value;
}

bool starts_with(const std::string & value, const std::string & prefix)
{
  return value.size() >= prefix.size() &&
         value.compare(0, prefix.size(), prefix) == 0;
}

std::string join_candidates(const std::vector<std::string> & candidates)
{
  if (candidates.empty()) {
    return "无候选串口";
  }
  std::ostringstream stream;
  for (std::size_t index = 0; index < candidates.size(); ++index) {
    if (index > 0U) {
      stream << ", ";
    }
    stream << candidates[index];
  }
  return stream.str();
}

bool probe_candidate_for_gnss(
  const std::string & device,
  const int baud_rate,
  const std::chrono::milliseconds probe_duration,
  std::string & error_message)
{
  SerialPort serial_port;
  if (!serial_port.open_device(device, baud_rate, error_message)) {
    return false;
  }

  const auto deadline = std::chrono::steady_clock::now() + probe_duration;
  std::array<std::uint8_t, 512> buffer{};
  std::string sample;
  sample.reserve(4096);

  while (std::chrono::steady_clock::now() < deadline) {
    const std::ptrdiff_t length = serial_port.read_bytes(buffer.data(), buffer.size(), error_message);
    if (length < 0) {
      return false;
    }
    if (length > 0) {
      sample.append(
        reinterpret_cast<const char *>(buffer.data()),
        static_cast<std::size_t>(length));
      if (sample.size() > 8192U) {
        sample.erase(0U, sample.size() - 8192U);
      }
      if (contains_gnss_stream_signature(sample)) {
        error_message.clear();
        return true;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  error_message = "探测期间未发现GNSS文本帧";
  return false;
}

}  // namespace

std::vector<std::string> list_serial_candidates(
  const std::string & by_id_directory,
  const std::string & device_directory)
{
  namespace fs = std::filesystem;
  std::set<std::string> stable_candidates;
  std::error_code error;

  const fs::path by_id_path(by_id_directory);
  if (fs::exists(by_id_path, error) && fs::is_directory(by_id_path, error)) {
    for (fs::directory_iterator iterator(by_id_path, error), end; iterator != end && !error;
      iterator.increment(error))
    {
      const fs::path entry_path = iterator->path();
      std::error_code exists_error;
      if (fs::exists(entry_path, exists_error)) {
        stable_candidates.insert(entry_path.string());
      }
    }
  }

  if (!stable_candidates.empty()) {
    return {stable_candidates.begin(), stable_candidates.end()};
  }

  std::set<std::string> fallback_candidates;
  const fs::path device_path(device_directory);
  error.clear();
  if (!fs::exists(device_path, error) || !fs::is_directory(device_path, error)) {
    return {};
  }

  for (fs::directory_iterator iterator(device_path, error), end; iterator != end && !error;
    iterator.increment(error))
  {
    const std::string name = iterator->path().filename().string();
    if (!starts_with(name, "ttyUSB") && !starts_with(name, "ttyACM")) {
      continue;
    }
    std::error_code exists_error;
    if (fs::exists(iterator->path(), exists_error)) {
      fallback_candidates.insert(iterator->path().string());
    }
  }
  return {fallback_candidates.begin(), fallback_candidates.end()};
}

std::string select_unique_serial_candidate(
  const std::vector<std::string> & candidates,
  const std::vector<std::string> & preferred_tokens)
{
  if (candidates.size() == 1U) {
    return candidates.front();
  }
  if (candidates.empty() || preferred_tokens.empty()) {
    return {};
  }

  std::vector<std::string> matched;
  for (const auto & candidate : candidates) {
    const std::string candidate_lower = lowercase(candidate);
    for (const auto & token : preferred_tokens) {
      if (token.empty()) {
        continue;
      }
      if (candidate_lower.find(lowercase(token)) != std::string::npos) {
        matched.push_back(candidate);
        break;
      }
    }
  }
  if (matched.size() == 1U) {
    return matched.front();
  }
  return {};
}

bool contains_gnss_stream_signature(const std::string & data)
{
  static constexpr std::array<const char *, 11> signatures{{
    "$GN", "$GP", "$GB", "$BD", "$GL", "$GA", "$GQ", "$QZ", "$GI", "#BESTPOSA", "#BESTPOSB"
  }};
  return std::any_of(
    signatures.begin(), signatures.end(),
    [&data](const char * signature) {return data.find(signature) != std::string::npos;});
}

SerialDiscoveryResult discover_rtk_serial_device(
  const int baud_rate,
  const std::vector<std::string> & preferred_tokens,
  const std::chrono::milliseconds probe_duration,
  const std::string & by_id_directory,
  const std::string & device_directory)
{
  SerialDiscoveryResult result;
  result.candidates = list_serial_candidates(by_id_directory, device_directory);
  if (result.candidates.empty()) {
    result.detail = "未发现 /dev/serial/by-id、ttyUSB 或 ttyACM 串口";
    return result;
  }

  const std::string preferred_candidate =
    select_unique_serial_candidate(result.candidates, preferred_tokens);

  // 多串口环境下不主动逐个打开未知外设。串口探测会临时设置波特率，若误碰调试串口、
  // RS-485 等设备可能干扰其他程序。只有唯一候选，或用户提供的稳定特征唯一命中时才探测。
  std::string probe_candidate;
  if (result.candidates.size() == 1U) {
    probe_candidate = result.candidates.front();
  } else if (!preferred_candidate.empty()) {
    probe_candidate = preferred_candidate;
  } else {
    result.detail = "发现多个串口，为避免扰动未知外设不进行逐口探测；请配置auto_preferred_tokens或显式device：" +
      join_candidates(result.candidates);
    return result;
  }

  std::string probe_error;
  if (probe_candidate_for_gnss(probe_candidate, baud_rate, probe_duration, probe_error)) {
    result.device = probe_candidate;
    result.detail = result.candidates.size() == 1U ?
      "唯一串口候选已确认输出GNSS数据" :
      "首选设备特征匹配项已确认输出GNSS数据";
    return result;
  }

  if (result.candidates.size() == 1U) {
    result.detail = "发现唯一串口候选但探测期间未发现GNSS数据；请检查波特率，非文本协议请显式配置device：" +
      result.candidates.front();
  } else {
    result.detail = "首选设备特征匹配项未检测到GNSS数据，请检查波特率或显式配置device：" +
      preferred_candidate;
  }
  return result;
}

}  // namespace rtk_driver

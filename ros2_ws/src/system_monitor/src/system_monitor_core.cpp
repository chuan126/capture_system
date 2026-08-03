#include "system_monitor/system_monitor_core.hpp"

#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <vector>

namespace system_monitor
{

StreamMonitor::StreamMonitor(Clock::time_point started_at) : started_at_(started_at) {}

void StreamMonitor::observe(Clock::time_point received_at)
{
  samples_.push_back(received_at);
  while (samples_.size() > 100U) {
    samples_.pop_front();
  }
}

TimedHealth StreamMonitor::evaluate(
  Clock::time_point now, std::chrono::milliseconds startup_grace,
  std::chrono::milliseconds timeout, const std::string & waiting_message,
  const std::string & ok_message, const std::string & timeout_message) const
{
  if (samples_.empty()) {
    const auto startup_age = std::chrono::duration_cast<std::chrono::milliseconds>(now - started_at_);
    return {
      startup_age <= startup_grace ? Level::stale : Level::error,
      startup_age <= startup_grace ? waiting_message : timeout_message, -1.0, 0.0};
  }

  const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(now - samples_.back());
  double frequency = 0.0;
  if (samples_.size() >= 2U) {
    const auto span = std::chrono::duration<double>(samples_.back() - samples_.front()).count();
    if (span > 0.0) {
      frequency = static_cast<double>(samples_.size() - 1U) / span;
    }
  }
  return {
    age <= timeout ? Level::ok : Level::error,
    age <= timeout ? ok_message : timeout_message,
    static_cast<double>(age.count()), frequency};
}

namespace
{
std::string decode_mount_field(std::string value)
{
  const std::pair<const char *, const char *> escapes[] = {
    {"\\040", " "}, {"\\011", "\t"}, {"\\012", "\n"}, {"\\134", "\\"}};
  for (const auto & item : escapes) {
    std::string::size_type position = 0;
    while ((position = value.find(item.first, position)) != std::string::npos) {
      value.replace(position, 4U, item.second);
      position += 1U;
    }
  }
  return value;
}

void find_mount(const std::filesystem::path & path, std::string & source, std::string & target)
{
  std::ifstream input("/proc/self/mountinfo");
  std::string line;
  const std::string resolved = std::filesystem::weakly_canonical(path).string();
  std::size_t best_length = 0;
  while (std::getline(input, line)) {
    std::istringstream stream(line);
    std::vector<std::string> fields;
    std::string field;
    while (stream >> field) {
      fields.push_back(field);
    }
    const auto separator = std::find(fields.begin(), fields.end(), "-");
    if (fields.size() < 10U || separator == fields.end() || separator + 2 >= fields.end()) {
      continue;
    }
    const std::string mount_target = decode_mount_field(fields[4]);
    const bool matches = resolved == mount_target || mount_target == "/" ||
      (resolved.rfind(mount_target + "/", 0) == 0);
    if (matches && mount_target.size() >= best_length) {
      best_length = mount_target.size();
      target = mount_target;
      source = decode_mount_field(*(separator + 2));
    }
  }
}
}  // namespace

StorageHealth inspect_storage(
  const std::string & data_path, std::uint64_t warn_available_bytes,
  std::uint64_t error_available_bytes)
{
  StorageHealth result;
  std::error_code error;
  const std::filesystem::path path(data_path);
  if (!std::filesystem::is_directory(path, error)) {
    result.message = "数据目录不存在";
    return result;
  }

  struct statvfs status {};
  if (statvfs(path.c_str(), &status) != 0) {
    result.message = "无法读取存储容量";
    return result;
  }
  result.total_bytes = static_cast<std::uint64_t>(status.f_blocks) * status.f_frsize;
  result.available_bytes = static_cast<std::uint64_t>(status.f_bavail) * status.f_frsize;
  result.writable = (status.f_flag & ST_RDONLY) == 0 && access(path.c_str(), W_OK) == 0;
  find_mount(path, result.source, result.mount_point);
  if (!result.writable) {
    result.message = "数据目录不可写";
    return result;
  }
  if (result.available_bytes <= error_available_bytes) {
    result.message = "可用空间严重不足";
    return result;
  }
  if (result.available_bytes <= warn_available_bytes) {
    result.level = Level::warn;
    result.message = "可用空间较低";
    return result;
  }
  result.level = Level::ok;
  result.message = "存储正常";
  return result;
}

}  // namespace system_monitor

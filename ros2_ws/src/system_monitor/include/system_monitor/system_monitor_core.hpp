#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <string>

namespace system_monitor
{

enum class Level : std::uint8_t { ok = 0, warn = 1, error = 2, stale = 3 };

struct TimedHealth
{
  Level level{Level::stale};
  std::string message;
  double age_ms{-1.0};
  double frequency_hz{0.0};
};

class StreamMonitor
{
public:
  using Clock = std::chrono::steady_clock;
  explicit StreamMonitor(Clock::time_point started_at = Clock::now());
  void observe(Clock::time_point received_at);
  TimedHealth evaluate(
    Clock::time_point now, std::chrono::milliseconds startup_grace,
    std::chrono::milliseconds timeout, const std::string & waiting_message,
    const std::string & ok_message, const std::string & timeout_message) const;

private:
  Clock::time_point started_at_;
  std::deque<Clock::time_point> samples_;
};

struct StorageHealth
{
  Level level{Level::error};
  std::string message;
  std::string source;
  std::string mount_point;
  std::uint64_t total_bytes{0};
  std::uint64_t available_bytes{0};
  bool writable{false};
};

StorageHealth inspect_storage(
  const std::string & data_path, std::uint64_t warn_available_bytes,
  std::uint64_t error_available_bytes);

}  // namespace system_monitor

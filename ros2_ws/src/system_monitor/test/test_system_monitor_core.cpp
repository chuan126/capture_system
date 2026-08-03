#include "system_monitor/system_monitor_core.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>

using namespace std::chrono_literals;

TEST(StreamMonitor, DistinguishesStartupHealthyAndTimeout)
{
  using Clock = system_monitor::StreamMonitor::Clock;
  const auto start = Clock::time_point(10s);
  system_monitor::StreamMonitor monitor(start);
  EXPECT_EQ(monitor.evaluate(start + 1s, 5s, 1s, "等待", "正常", "超时").level,
    system_monitor::Level::stale);
  monitor.observe(start + 2s);
  monitor.observe(start + 2100ms);
  const auto healthy = monitor.evaluate(start + 2200ms, 5s, 1s, "等待", "正常", "超时");
  EXPECT_EQ(healthy.level, system_monitor::Level::ok);
  EXPECT_NEAR(healthy.frequency_hz, 10.0, 0.01);
  EXPECT_EQ(monitor.evaluate(start + 4s, 5s, 1s, "等待", "正常", "超时").level,
    system_monitor::Level::error);
}

TEST(StorageMonitor, ReportsActualFilesystemAndThreshold)
{
  const auto path = std::filesystem::temp_directory_path();
  const auto healthy = system_monitor::inspect_storage(path.string(), 0, 0);
  EXPECT_EQ(healthy.level, system_monitor::Level::ok);
  EXPECT_GT(healthy.total_bytes, 0U);
  EXPECT_GT(healthy.available_bytes, 0U);
  EXPECT_TRUE(healthy.writable);

  const auto warning = system_monitor::inspect_storage(
    path.string(), healthy.available_bytes + 1U, 0);
  EXPECT_EQ(warning.level, system_monitor::Level::warn);
}

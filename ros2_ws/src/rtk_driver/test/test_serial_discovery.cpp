#include <chrono>
#include <filesystem>
#include <array>
#include <fstream>
#include <thread>

#include <pty.h>
#include <unistd.h>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "rtk_driver/serial_discovery.hpp"

namespace
{

TEST(SerialDiscovery, SelectsSingleCandidateWithoutIdentityHint)
{
  const std::vector<std::string> candidates{
    "/dev/serial/by-id/usb-New_RTK_Receiver-if00-port0"};
  EXPECT_EQ(
    rtk_driver::select_unique_serial_candidate(candidates, {}),
    candidates.front());
}

TEST(SerialDiscovery, UsesPreferredTokenOnlyWhenItIsUnique)
{
  const std::vector<std::string> candidates{
    "/dev/serial/by-id/usb-Debug_UART-if00-port0",
    "/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0"};
  EXPECT_EQ(
    rtk_driver::select_unique_serial_candidate(candidates, {"1a86_USB_Serial"}),
    candidates[1]);
  EXPECT_TRUE(
    rtk_driver::select_unique_serial_candidate(candidates, {"usb"}).empty());
}

TEST(SerialDiscovery, RecognizesOnlySupportedGnssTextSignatures)
{
  EXPECT_TRUE(rtk_driver::contains_gnss_stream_signature("noise\r\n$GNRMC,1234"));
  EXPECT_TRUE(rtk_driver::contains_gnss_stream_signature("noise\r\n$GNGGA,1234"));
  EXPECT_TRUE(rtk_driver::contains_gnss_stream_signature("noise\r\n$GPGSA,A,3"));
  EXPECT_TRUE(rtk_driver::contains_gnss_stream_signature("#BESTPOSA,COM1,0,0"));
  EXPECT_FALSE(rtk_driver::contains_gnss_stream_signature("$GNVTG,1234"));
  EXPECT_FALSE(rtk_driver::contains_gnss_stream_signature("#BESTPOSB,COM1,0,0"));
  EXPECT_FALSE(rtk_driver::contains_gnss_stream_signature("debug uart output only"));
}

TEST(SerialDiscovery, PrefersByIdAndFallsBackToTtyNames)
{
  namespace fs = std::filesystem;
  const auto root = fs::temp_directory_path() /
    ("rtk_discovery_test_" + std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count()));
  const auto by_id = root / "serial" / "by-id";
  const auto dev = root / "dev";
  fs::create_directories(by_id);
  fs::create_directories(dev);
  std::ofstream(dev / "ttyUSB0").put('\n');
  std::ofstream(dev / "ttyACM0").put('\n');
  std::ofstream(by_id / "usb-RTK-if00-port0").put('\n');

  auto candidates = rtk_driver::list_serial_candidates(by_id.string(), dev.string());
  ASSERT_EQ(candidates.size(), 1U);
  EXPECT_EQ(candidates.front(), (by_id / "usb-RTK-if00-port0").string());

  fs::remove_all(by_id);
  candidates = rtk_driver::list_serial_candidates(by_id.string(), dev.string());
  ASSERT_EQ(candidates.size(), 2U);
  EXPECT_EQ(candidates[0], (dev / "ttyACM0").string());
  EXPECT_EQ(candidates[1], (dev / "ttyUSB0").string());
  fs::remove_all(root);
}


TEST(SerialDiscovery, RefusesToProbeMultipleUnknownSerialPorts)
{
  namespace fs = std::filesystem;
  const auto root = fs::temp_directory_path() /
    ("rtk_discovery_ambiguous_" + std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count()));
  const auto by_id = root / "serial" / "by-id";
  const auto dev = root / "dev";
  fs::create_directories(by_id);
  fs::create_directories(dev);
  std::ofstream(by_id / "usb-debug-if00-port0").put('\n');
  std::ofstream(by_id / "usb-other-if00-port0").put('\n');

  const auto result = rtk_driver::discover_rtk_serial_device(
    115200, {}, std::chrono::milliseconds(100), by_id.string(), dev.string());

  EXPECT_EQ(result.candidates.size(), 2U);
  EXPECT_TRUE(result.device.empty());
  EXPECT_NE(result.detail.find("不进行逐口探测"), std::string::npos);
  fs::remove_all(root);
}

TEST(SerialDiscovery, ProbesOnlyUniquelyPreferredCandidate)
{
  namespace fs = std::filesystem;
  int rtk_master = -1;
  int rtk_slave = -1;
  std::array<char, 128> rtk_name{};
  ASSERT_EQ(openpty(&rtk_master, &rtk_slave, rtk_name.data(), nullptr, nullptr), 0);
  close(rtk_slave);

  const auto root = fs::temp_directory_path() /
    ("rtk_discovery_probe_" + std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count()));
  const auto by_id = root / "serial" / "by-id";
  const auto dev = root / "dev";
  fs::create_directories(by_id);
  fs::create_directories(dev);
  std::ofstream(by_id / "usb-debug-if00-port0").put('\n');
  fs::create_symlink(rtk_name.data(), by_id / "usb-field-rtk-if00-port0");

  std::thread writer([rtk_master]() {
    constexpr char sample[] = "$GNGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n";
    for (int index = 0; index < 20; ++index) {
      const auto sample_size = sizeof(sample) - 1U;
      const auto written = write(rtk_master, sample, sample_size);
      // 探测结束后从设备会关闭，写端此时停止即可，不能忽略 write 的失败结果。
      if (written != static_cast<ssize_t>(sample_size)) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
  });

  const auto result = rtk_driver::discover_rtk_serial_device(
    115200, {"field-rtk"}, std::chrono::milliseconds(250), by_id.string(), dev.string());
  writer.join();

  EXPECT_EQ(result.candidates.size(), 2U);
  EXPECT_EQ(result.device, (by_id / "usb-field-rtk-if00-port0").string());
  EXPECT_NE(result.detail.find("首选设备特征"), std::string::npos);

  close(rtk_master);
  fs::remove_all(root);
}

}  // namespace

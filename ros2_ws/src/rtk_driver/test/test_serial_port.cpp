#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

#include <pty.h>
#include <unistd.h>

#include "gtest/gtest.h"
#include "rtk_driver/serial_port.hpp"

namespace
{

TEST(SerialPort, ReadsBytesFromPseudoTerminalAndDetectsDisconnect)
{
  int master_descriptor = -1;
  int slave_descriptor = -1;
  std::array<char, 128> slave_name{};
  ASSERT_EQ(
    openpty(&master_descriptor, &slave_descriptor, slave_name.data(), nullptr, nullptr), 0);
  ASSERT_GE(master_descriptor, 0);
  ASSERT_GE(slave_descriptor, 0);
  close(slave_descriptor);

  rtk_driver::SerialPort serial_port;
  std::string error_message;
  ASSERT_TRUE(serial_port.open_device(slave_name.data(), 115200, error_message)) << error_message;

  constexpr std::array<std::uint8_t, 5> expected{{'R', 'T', 'K', '\r', '\n'}};
  ASSERT_EQ(
    write(master_descriptor, expected.data(), expected.size()),
    static_cast<ssize_t>(expected.size()));

  std::array<std::uint8_t, 16> received{};
  std::ptrdiff_t received_length = 0;
  for (int attempt = 0; attempt < 20 && received_length == 0; ++attempt) {
    received_length = serial_port.read_bytes(received.data(), received.size(), error_message);
    if (received_length == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
  ASSERT_EQ(received_length, static_cast<std::ptrdiff_t>(expected.size()));
  EXPECT_TRUE(std::equal(expected.begin(), expected.end(), received.begin()));

  close(master_descriptor);
  std::ptrdiff_t disconnect_result = 0;
  for (int attempt = 0; attempt < 20 && disconnect_result == 0; ++attempt) {
    disconnect_result = serial_port.read_bytes(received.data(), received.size(), error_message);
    if (disconnect_result == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
  EXPECT_LT(disconnect_result, 0);
  EXPECT_FALSE(error_message.empty());
}

}  // namespace

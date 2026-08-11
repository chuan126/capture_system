#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "diagnostic_msgs/msg/diagnostic_array.hpp"
#include "diagnostic_msgs/msg/diagnostic_status.hpp"
#include "diagnostic_msgs/msg/key_value.hpp"
#include "interfaces/msg/rtk_status.hpp"
#include "rclcpp/rclcpp.hpp"
#include "rtk_driver/serial_discovery.hpp"
#include "rtk_driver/serial_port.hpp"
#include "sensor_msgs/msg/nav_sat_fix.hpp"
#include "sensor_msgs/msg/nav_sat_status.hpp"

extern "C"
{
#include "gnss_nmea.h"
}

namespace rtk_driver
{
namespace
{

diagnostic_msgs::msg::KeyValue make_key_value(
  const std::string & key,
  const std::string & value)
{
  diagnostic_msgs::msg::KeyValue item;
  item.key = key;
  item.value = value;
  return item;
}

}  // namespace

class RtkDriverNode final : public rclcpp::Node
{
public:
  RtkDriverNode()
  : Node("rtk_driver_node")
  {
    device_ = declare_parameter<std::string>("device", "auto");
    baud_rate_ = declare_parameter<int>("baud_rate", 115200);
    auto_preferred_tokens_ = declare_parameter<std::vector<std::string>>(
      "auto_preferred_tokens", std::vector<std::string>{});
    auto_probe_duration_ms_ = declare_parameter<int>("auto_probe_duration_ms", 1200);
    frame_id_ = declare_parameter<std::string>("frame_id", "rtk_link");
    reconnect_interval_ms_ = declare_parameter<int>("reconnect_interval_ms", 1000);
    read_period_ms_ = declare_parameter<int>("read_period_ms", 10);
    read_buffer_size_ = declare_parameter<int>("read_buffer_size", 512);
    max_reads_per_cycle_ = declare_parameter<int>("max_reads_per_cycle", 8);
    fix_topic_ = declare_parameter<std::string>("fix_topic", "/capture/rtk/fix");
    status_topic_ = declare_parameter<std::string>("status_topic", "/capture/rtk/status");
    diagnostics_topic_ = declare_parameter<std::string>("diagnostics_topic", "/diagnostics");

    validate_parameters();
    read_buffer_.resize(static_cast<std::size_t>(read_buffer_size_));

    const auto state_qos =
      rclcpp::QoS(rclcpp::KeepLast(5)).reliable().durability_volatile();
    fix_publisher_ = create_publisher<sensor_msgs::msg::NavSatFix>(fix_topic_, state_qos);
    status_publisher_ = create_publisher<interfaces::msg::RtkStatus>(status_topic_, state_qos);
    diagnostics_publisher_ =
      create_publisher<diagnostic_msgs::msg::DiagnosticArray>(diagnostics_topic_, state_qos);

    gnss_nmea_init();
    const auto read_period = std::chrono::milliseconds(read_period_ms_);
    read_timer_ = create_wall_timer(read_period, [this]() {read_serial();});
    diagnostics_timer_ = create_wall_timer(
      std::chrono::seconds(1), [this]() {publish_diagnostics();});

    RCLCPP_INFO(
      get_logger(),
      "RTK驱动已启动：设备配置=%s，波特率=%d，fix=%s，status=%s",
      device_.c_str(), baud_rate_, fix_topic_.c_str(), status_topic_.c_str());
  }

private:
  void validate_parameters() const
  {
    if (device_.empty()) {
      throw std::invalid_argument("参数device不能为空");
    }
    if (frame_id_.empty()) {
      throw std::invalid_argument("参数frame_id不能为空");
    }
    if (auto_probe_duration_ms_ < 100 || auto_probe_duration_ms_ > 10000) {
      throw std::invalid_argument("参数auto_probe_duration_ms必须位于[100, 10000] ms");
    }
    if (fix_topic_.empty() || status_topic_.empty() || diagnostics_topic_.empty()) {
      throw std::invalid_argument("Topic参数不能为空");
    }
    if (reconnect_interval_ms_ < 100 || reconnect_interval_ms_ > 60000) {
      throw std::invalid_argument("参数reconnect_interval_ms必须位于[100, 60000] ms");
    }
    if (read_period_ms_ < 1 || read_period_ms_ > 1000) {
      throw std::invalid_argument("参数read_period_ms必须位于[1, 1000] ms");
    }
    if (read_buffer_size_ < 64 || read_buffer_size_ > 65536) {
      throw std::invalid_argument("参数read_buffer_size必须位于[64, 65536]字节");
    }
    if (max_reads_per_cycle_ < 1 || max_reads_per_cycle_ > 64) {
      throw std::invalid_argument("参数max_reads_per_cycle必须位于[1, 64]");
    }
  }

  bool connect_if_due()
  {
    if (serial_port_.is_open()) {
      return true;
    }

    const auto current_time = std::chrono::steady_clock::now();
    if (has_attempted_connection_ &&
      current_time - last_connection_attempt_ <
      std::chrono::milliseconds(reconnect_interval_ms_))
    {
      return false;
    }

    has_attempted_connection_ = true;
    last_connection_attempt_ = current_time;
    ++connection_attempt_count_;

    if (device_ == "auto") {
      const auto discovery = discover_rtk_serial_device(
        baud_rate_, auto_preferred_tokens_, std::chrono::milliseconds(auto_probe_duration_ms_));
      discovery_candidate_count_ = discovery.candidates.size();
      last_discovery_detail_ = discovery.detail;
      if (discovery.device.empty()) {
        last_serial_error_ = discovery.detail;
        RCLCPP_WARN_THROTTLE(
          get_logger(), *get_clock(), 5000,
          "正在自动发现RTK串口：%s", discovery.detail.c_str());
        return false;
      }
      if (active_device_ != discovery.device) {
        active_device_ = discovery.device;
        RCLCPP_INFO(
          get_logger(), "自动选择RTK串口%s：%s",
          active_device_.c_str(), discovery.detail.c_str());
      }
    } else {
      active_device_ = device_;
      discovery_candidate_count_ = 1U;
      last_discovery_detail_ = "使用显式device参数";
    }

    std::string error_message;
    if (!serial_port_.open_device(active_device_, baud_rate_, error_message)) {
      last_serial_error_ = error_message;
      RCLCPP_WARN_THROTTLE(
        get_logger(), *get_clock(), 5000,
        "正在等待RTK串口%s：%s", active_device_.c_str(), error_message.c_str());
      if (device_ == "auto") {
        active_device_.clear();
      }
      return false;
    }

    ++connection_success_count_;
    last_serial_error_.clear();
    gnss_nmea_init();
    RCLCPP_INFO(get_logger(), "已连接RTK串口%s", active_device_.c_str());
    return true;
  }

  void read_serial()
  {
    if (!connect_if_due()) {
      return;
    }

    for (int read_index = 0; read_index < max_reads_per_cycle_; ++read_index) {
      std::string error_message;
      const std::ptrdiff_t length = serial_port_.read_bytes(
        read_buffer_.data(), read_buffer_.size(), error_message);
      if (length < 0) {
        ++read_error_count_;
        last_serial_error_ = error_message;
        serial_port_.close_device();
        if (device_ == "auto") {
          active_device_.clear();
        }
        RCLCPP_WARN(get_logger(), "RTK串口读取失败，等待重连：%s", error_message.c_str());
        return;
      }
      if (length == 0) {
        return;
      }

      received_byte_count_ += static_cast<std::uint64_t>(length);
      for (std::ptrdiff_t index = 0; index < length; ++index) {
        handle_parser_event(gnss_nmea_input_byte(read_buffer_[static_cast<std::size_t>(index)]));
      }
    }
  }

  void handle_parser_event(const gnss_nmea_event_t event)
  {
    const auto event_mask = static_cast<std::uint32_t>(event);
    if (event_mask == GNSS_NMEA_EVENT_NONE) {
      return;
    }

    if ((event_mask & GNSS_NMEA_EVENT_RMC_OK) != 0U) {
      ++rmc_count_;
    }
    if ((event_mask & GNSS_NMEA_EVENT_GGA_OK) != 0U) {
      ++gga_count_;
    }
    if ((event_mask & GNSS_NMEA_EVENT_GSA_OK) != 0U) {
      ++gsa_count_;
    }
    if ((event_mask & GNSS_NMEA_EVENT_BESTPOSA_OK) != 0U) {
      ++bestposa_count_;
    }
    if ((event_mask & GNSS_NMEA_EVENT_CHECKSUM_ERROR) != 0U) {
      ++checksum_error_count_;
    }
    if ((event_mask & GNSS_NMEA_EVENT_FRAME_OVERFLOW) != 0U) {
      ++frame_overflow_count_;
    }

    const auto stamp = now();
    if ((event_mask & GNSS_NMEA_EVENT_GGA_OK) != 0U) {
      publish_fix(stamp);
    }
    if ((event_mask & (GNSS_NMEA_EVENT_RMC_OK | GNSS_NMEA_EVENT_GGA_OK |
      GNSS_NMEA_EVENT_GSA_OK | GNSS_NMEA_EVENT_BESTPOSA_OK)) != 0U)
    {
      publish_status(stamp, event_mask);
    }
  }

  void publish_fix(const rclcpp::Time & stamp)
  {
    const gnss_nmea_data_t * data = gnss_nmea_get_data();
    sensor_msgs::msg::NavSatFix message;
    message.header.stamp = stamp;
    message.header.frame_id = frame_id_;
    // 这里只把解析器的0/非0状态机械映射到标准消息，不评价解类型或稳定性。
    message.status.status = data->GPS_state == 0U ?
      sensor_msgs::msg::NavSatStatus::STATUS_NO_FIX :
      sensor_msgs::msg::NavSatStatus::STATUS_FIX;
    message.status.service = sensor_msgs::msg::NavSatStatus::SERVICE_GPS;
    message.latitude = gnss_nmea_get_latitude();
    message.longitude = gnss_nmea_get_longitude();
    message.altitude = gnss_nmea_get_height();
    message.position_covariance_type = sensor_msgs::msg::NavSatFix::COVARIANCE_TYPE_UNKNOWN;
    fix_publisher_->publish(std::move(message));
  }

  void publish_status(const rclcpp::Time & stamp, const std::uint32_t event_mask)
  {
    const gnss_nmea_data_t * data = gnss_nmea_get_data();
    interfaces::msg::RtkStatus message;
    message.header.stamp = stamp;
    message.header.frame_id = frame_id_;
    message.event_mask = event_mask;
    message.rmc_validity = static_cast<std::uint8_t>(data->isGnssVaild);
    message.gps_state = data->GPS_state;
    message.satellite_count = static_cast<std::uint8_t>(data->StarNum);
    message.hdop = data->HDOP;
    message.pdop = data->PDOP;
    message.latitude_sigma = data->lat_sigma;
    message.longitude_sigma = data->lon_sigma;
    message.height_sigma = data->height_sigma;
    message.speed_knots = data->Gnss_Velocity;
    message.track_degrees = data->Gnss_track;
    message.utc_year = data->Gnss_UTC_Year;
    message.utc_month = data->Gnss_UTC_Month;
    message.utc_day = data->Gnss_UTC_Day;
    message.utc_hour = data->Gnss_UTC_Hour;
    message.utc_minute = data->Gnss_UTC_Min;
    message.utc_second = data->Gnss_UTC_Second;
    status_publisher_->publish(std::move(message));
  }

  void publish_diagnostics()
  {
    diagnostic_msgs::msg::DiagnosticArray array;
    array.header.stamp = now();

    diagnostic_msgs::msg::DiagnosticStatus status;
    status.name = "rtk_driver/serial";
    status.hardware_id = active_device_.empty() ? device_ : active_device_;
    if (serial_port_.is_open()) {
      status.level = diagnostic_msgs::msg::DiagnosticStatus::OK;
      status.message = "串口已连接";
    } else {
      status.level = diagnostic_msgs::msg::DiagnosticStatus::WARN;
      status.message = "串口未连接，正在重试";
    }

    status.values.push_back(
      make_key_value("device", active_device_.empty() ? device_ : active_device_));
    status.values.push_back(make_key_value("configured_device", device_));
    status.values.push_back(make_key_value("active_device", active_device_));
    status.values.push_back(make_key_value("auto_discovery", device_ == "auto" ? "true" : "false"));
    status.values.push_back(
      make_key_value("serial_connected", serial_port_.is_open() ? "true" : "false"));
    status.values.push_back(
      make_key_value("discovery_candidate_count", std::to_string(discovery_candidate_count_)));
    status.values.push_back(make_key_value("discovery_detail", last_discovery_detail_));
    status.values.push_back(make_key_value("baud_rate", std::to_string(baud_rate_)));
    status.values.push_back(
      make_key_value("received_bytes", std::to_string(received_byte_count_)));
    status.values.push_back(
      make_key_value("connection_attempts", std::to_string(connection_attempt_count_)));
    status.values.push_back(
      make_key_value("connection_successes", std::to_string(connection_success_count_)));
    status.values.push_back(make_key_value("read_errors", std::to_string(read_error_count_)));
    status.values.push_back(make_key_value("rmc_events", std::to_string(rmc_count_)));
    status.values.push_back(make_key_value("gga_events", std::to_string(gga_count_)));
    status.values.push_back(make_key_value("gsa_events", std::to_string(gsa_count_)));
    status.values.push_back(make_key_value("bestposa_events", std::to_string(bestposa_count_)));
    status.values.push_back(
      make_key_value("checksum_errors", std::to_string(checksum_error_count_)));
    status.values.push_back(
      make_key_value("frame_overflows", std::to_string(frame_overflow_count_)));
    status.values.push_back(make_key_value("last_serial_error", last_serial_error_));
    array.status.push_back(std::move(status));
    diagnostics_publisher_->publish(std::move(array));
  }

  std::string device_;
  int baud_rate_{115200};
  std::vector<std::string> auto_preferred_tokens_;
  int auto_probe_duration_ms_{1200};
  std::string frame_id_;
  int reconnect_interval_ms_{1000};
  int read_period_ms_{10};
  int read_buffer_size_{512};
  int max_reads_per_cycle_{8};
  std::string fix_topic_;
  std::string status_topic_;
  std::string diagnostics_topic_;

  SerialPort serial_port_;
  std::vector<std::uint8_t> read_buffer_;
  bool has_attempted_connection_{false};
  std::chrono::steady_clock::time_point last_connection_attempt_{};
  std::string active_device_;
  std::size_t discovery_candidate_count_{0U};
  std::string last_discovery_detail_;
  std::string last_serial_error_;

  std::uint64_t received_byte_count_{0U};
  std::uint64_t connection_attempt_count_{0U};
  std::uint64_t connection_success_count_{0U};
  std::uint64_t read_error_count_{0U};
  std::uint64_t rmc_count_{0U};
  std::uint64_t gga_count_{0U};
  std::uint64_t gsa_count_{0U};
  std::uint64_t bestposa_count_{0U};
  std::uint64_t checksum_error_count_{0U};
  std::uint64_t frame_overflow_count_{0U};

  rclcpp::Publisher<sensor_msgs::msg::NavSatFix>::SharedPtr fix_publisher_;
  rclcpp::Publisher<interfaces::msg::RtkStatus>::SharedPtr status_publisher_;
  rclcpp::Publisher<diagnostic_msgs::msg::DiagnosticArray>::SharedPtr diagnostics_publisher_;
  rclcpp::TimerBase::SharedPtr read_timer_;
  rclcpp::TimerBase::SharedPtr diagnostics_timer_;
};

}  // namespace rtk_driver

int main(int argc, char * argv[])
{
  rclcpp::init(argc, argv);
  try {
    // 单线程执行器保证既有全局解析器不会被并发调用。
    rclcpp::spin(std::make_shared<rtk_driver::RtkDriverNode>());
  } catch (const std::exception & exception) {
    RCLCPP_FATAL(
      rclcpp::get_logger("rtk_driver_node"), "RTK驱动启动或运行失败：%s", exception.what());
    rclcpp::shutdown();
    return 1;
  }
  rclcpp::shutdown();
  return 0;
}

#include "system_monitor/system_monitor_core.hpp"

#include <diagnostic_msgs/msg/diagnostic_array.hpp>
#include <diagnostic_msgs/msg/diagnostic_status.hpp>
#include <diagnostic_msgs/msg/key_value.hpp>
#include <rclcpp/rclcpp.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>

namespace
{
using diagnostic_msgs::msg::DiagnosticStatus;
using system_monitor::Level;

diagnostic_msgs::msg::KeyValue value(const std::string & key, const std::string & text)
{
  diagnostic_msgs::msg::KeyValue item;
  item.key = key;
  item.value = text;
  return item;
}

std::string number(double input, int precision = 2)
{
  std::ostringstream stream;
  stream << std::fixed << std::setprecision(precision) << input;
  return stream.str();
}

std::uint8_t diagnostic_level(Level level)
{
  switch (level) {
    case Level::ok: return DiagnosticStatus::OK;
    case Level::warn: return DiagnosticStatus::WARN;
    case Level::error: return DiagnosticStatus::ERROR;
    case Level::stale: return DiagnosticStatus::STALE;
  }
  return DiagnosticStatus::STALE;
}

struct CpuCounters
{
  std::uint64_t total{0};
  std::uint64_t idle{0};
  bool valid{false};
};

CpuCounters read_cpu()
{
  std::ifstream input("/proc/stat");
  std::string label;
  input >> label;
  if (label != "cpu") {
    return {};
  }
  CpuCounters counters;
  std::uint64_t item = 0;
  for (int index = 0; index < 10 && input >> item; ++index) {
    counters.total += item;
    if (index == 3 || index == 4) {
      counters.idle += item;
    }
  }
  counters.valid = counters.total > 0;
  return counters;
}

double read_memory_percent()
{
  std::ifstream input("/proc/meminfo");
  std::string key;
  std::uint64_t amount = 0;
  std::string unit;
  std::uint64_t total = 0;
  std::uint64_t available = 0;
  while (input >> key >> amount >> unit) {
    if (key == "MemTotal:") total = amount;
    if (key == "MemAvailable:") available = amount;
  }
  return total == 0 ? -1.0 : 100.0 * static_cast<double>(total - available) / total;
}

double read_max_temperature()
{
  double maximum = -1.0;
  std::error_code error;
  const std::filesystem::path thermal_root("/sys/class/thermal");
  for (const auto & entry : std::filesystem::directory_iterator(thermal_root, error)) {
    if (error || entry.path().filename().string().rfind("thermal_zone", 0) != 0) continue;
    std::ifstream input(entry.path() / "temp");
    double millidegrees = 0.0;
    if (input >> millidegrees) maximum = std::max(maximum, millidegrees / 1000.0);
  }
  return maximum;
}
}  // namespace

class SystemMonitorNode : public rclcpp::Node
{
public:
  SystemMonitorNode() : Node("system_monitor_node"), rtk_monitor_()
  {
    lidar_device_online_topic_ = declare_parameter<std::string>(
      "lidar_device_online_topic", "/capture/lidar/device_online");
    raw_diagnostics_topic_ = declare_parameter<std::string>("raw_diagnostics_topic", "/diagnostics");
    output_topic_ = declare_parameter<std::string>(
      "output_diagnostics_topic", "/capture/system/diagnostics");
    rtk_diagnostic_name_ = declare_parameter<std::string>(
      "rtk_diagnostic_name", "rtk_driver/serial");
    const char * configured_data_root = std::getenv("CAPTURE_DATA_ROOT");
    storage_path_ = declare_parameter<std::string>(
      "storage_data_path", configured_data_root && *configured_data_root ? configured_data_root :
      (std::filesystem::current_path() / "runtime").string());
    publish_period_ms_ = positive("publish_period_ms", 1000);
    rtk_startup_grace_ms_ = positive("rtk_startup_grace_ms", 5000);
    rtk_timeout_ms_ = positive("rtk_timeout_ms", 3000);
    memory_warn_percent_ = bounded_percent("memory_warn_percent", 85.0);
    cpu_warn_percent_ = bounded_percent("cpu_warn_percent", 90.0);
    temperature_warn_celsius_ = declare_parameter<double>("temperature_warn_celsius", 80.0);
    storage_warn_bytes_ = gibibytes("storage_warn_available_gib", 20.0);
    storage_error_bytes_ = gibibytes("storage_error_available_gib", 5.0);
    if (lidar_device_online_topic_.empty() || raw_diagnostics_topic_.empty() || output_topic_.empty() ||
      storage_path_.empty() || storage_error_bytes_ > storage_warn_bytes_)
    {
      throw std::invalid_argument("system_monitor参数为空或存储阈值顺序无效");
    }

    publisher_ = create_publisher<diagnostic_msgs::msg::DiagnosticArray>(
      output_topic_, rclcpp::QoS(5).reliable().transient_local());
    diagnostic_subscription_ = create_subscription<diagnostic_msgs::msg::DiagnosticArray>(
      raw_diagnostics_topic_, 10,
      [this](diagnostic_msgs::msg::DiagnosticArray::ConstSharedPtr message) {
        for (const auto & status : message->status) {
          if (status.name == rtk_diagnostic_name_) {
            rtk_level_ = status.level;
            rtk_message_ = status.message;
            rtk_monitor_.observe(system_monitor::StreamMonitor::Clock::now());
            break;
          }
        }
      });
    timer_ = create_wall_timer(
      std::chrono::milliseconds(publish_period_ms_), [this]() {publish_diagnostics();});
  }

private:
  int positive(const std::string & name, int default_value)
  {
    const int result = declare_parameter<int>(name, default_value);
    if (result <= 0) throw std::invalid_argument(name + "必须大于0");
    return result;
  }

  double bounded_percent(const std::string & name, double default_value)
  {
    const double result = declare_parameter<double>(name, default_value);
    if (result <= 0.0 || result > 100.0) throw std::invalid_argument(name + "必须在(0,100]内");
    return result;
  }

  std::uint64_t gibibytes(const std::string & name, double default_value)
  {
    const double result = declare_parameter<double>(name, default_value);
    if (result < 0.0) throw std::invalid_argument(name + "不得为负数");
    return static_cast<std::uint64_t>(result * 1024.0 * 1024.0 * 1024.0);
  }

  DiagnosticStatus lidar_status()
  {
    const std::size_t online_publishers = count_publishers(lidar_device_online_topic_);
    DiagnosticStatus status;
    status.name = "system_monitor/lidar";
    status.hardware_id = "ODIN1 Lite";
    status.level = online_publishers > 0U ? DiagnosticStatus::OK : DiagnosticStatus::WARN;
    status.message = online_publishers > 0U ? "雷达已接入" : "雷达未接入";
    status.values = {value("device_online_topic", lidar_device_online_topic_),
      value("online_publishers", std::to_string(online_publishers))};
    return status;
  }

  DiagnosticStatus rtk_status(const system_monitor::StreamMonitor::Clock::time_point now)
  {
    const auto health = rtk_monitor_.evaluate(
      now, std::chrono::milliseconds(rtk_startup_grace_ms_),
      std::chrono::milliseconds(rtk_timeout_ms_), "等待RTK诊断", "RTK诊断正常",
      "RTK诊断超时");
    DiagnosticStatus status;
    status.name = "system_monitor/rtk";
    status.hardware_id = "rtk";
    if (health.level == Level::ok) {
      status.level = rtk_level_;
      status.message = rtk_message_.empty() ? "RTK诊断正常" : rtk_message_;
    } else {
      status.level = diagnostic_level(health.level);
      status.message = health.message;
    }
    status.values = {value("source", rtk_diagnostic_name_), value("age_ms", number(health.age_ms))};
    return status;
  }

  DiagnosticStatus controller_status()
  {
    const CpuCounters current = read_cpu();
    double cpu_percent = -1.0;
    if (current.valid && previous_cpu_.valid && current.total > previous_cpu_.total) {
      const auto total_delta = current.total - previous_cpu_.total;
      const auto idle_delta = current.idle - previous_cpu_.idle;
      cpu_percent = 100.0 * static_cast<double>(total_delta - idle_delta) / total_delta;
    }
    previous_cpu_ = current;
    const double memory_percent = read_memory_percent();
    const double temperature = read_max_temperature();

    DiagnosticStatus status;
    status.name = "system_monitor/controller";
    status.hardware_id = "RK3588";
    status.level = DiagnosticStatus::OK;
    status.message = "运行正常";
    if (temperature >= temperature_warn_celsius_) {
      status.level = DiagnosticStatus::WARN;
      status.message = "控制器温度较高";
    } else if (memory_percent >= memory_warn_percent_) {
      status.level = DiagnosticStatus::WARN;
      status.message = "控制器内存占用较高";
    } else if (cpu_percent >= cpu_warn_percent_) {
      status.level = DiagnosticStatus::WARN;
      status.message = "控制器CPU占用较高";
    }
    status.values = {value("cpu_percent", number(cpu_percent)),
      value("memory_percent", number(memory_percent)),
      value("temperature_celsius", number(temperature))};
    return status;
  }

  DiagnosticStatus storage_status()
  {
    const auto health = system_monitor::inspect_storage(
      storage_path_, storage_warn_bytes_, storage_error_bytes_);
    DiagnosticStatus status;
    status.name = "system_monitor/storage";
    status.hardware_id = health.source;
    status.level = diagnostic_level(health.level);
    status.message = health.message;
    status.values = {value("data_path", storage_path_), value("source", health.source),
      value("mount_point", health.mount_point), value("total_bytes", std::to_string(health.total_bytes)),
      value("available_bytes", std::to_string(health.available_bytes)),
      value("writable", health.writable ? "true" : "false")};
    return status;
  }

  void publish_diagnostics()
  {
    diagnostic_msgs::msg::DiagnosticArray array;
    array.header.stamp = now();
    const auto steady_now = system_monitor::StreamMonitor::Clock::now();
    array.status.push_back(lidar_status());
    array.status.push_back(rtk_status(steady_now));
    array.status.push_back(controller_status());
    array.status.push_back(storage_status());
    publisher_->publish(std::move(array));
  }

  std::string lidar_device_online_topic_, raw_diagnostics_topic_, output_topic_, rtk_diagnostic_name_, storage_path_;
  int publish_period_ms_{1000}, rtk_startup_grace_ms_{5000}, rtk_timeout_ms_{3000};
  double memory_warn_percent_{85.0}, cpu_warn_percent_{90.0}, temperature_warn_celsius_{80.0};
  std::uint64_t storage_warn_bytes_{0}, storage_error_bytes_{0};
  system_monitor::StreamMonitor rtk_monitor_;
  std::uint8_t rtk_level_{DiagnosticStatus::STALE};
  std::string rtk_message_;
  CpuCounters previous_cpu_;
  rclcpp::Publisher<diagnostic_msgs::msg::DiagnosticArray>::SharedPtr publisher_;
  rclcpp::Subscription<diagnostic_msgs::msg::DiagnosticArray>::SharedPtr diagnostic_subscription_;
  rclcpp::TimerBase::SharedPtr timer_;
};

int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);
  try {
    rclcpp::spin(std::make_shared<SystemMonitorNode>());
  } catch (const std::exception & error) {
    RCLCPP_FATAL(rclcpp::get_logger("system_monitor"), "%s", error.what());
    rclcpp::shutdown();
    return 1;
  }
  rclcpp::shutdown();
  return 0;
}

#include <builtin_interfaces/msg/time.hpp>
#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/imu.hpp>

#include "motion_compensation/odometry_timestamp_expander.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

namespace motion_compensation
{
namespace
{

std::int64_t toNanoseconds(const builtin_interfaces::msg::Time & stamp) noexcept
{
  return static_cast<std::int64_t>(stamp.sec) * 1000000000LL +
         static_cast<std::int64_t>(stamp.nanosec);
}

builtin_interfaces::msg::Time fromNanoseconds(const std::int64_t stamp_ns) noexcept
{
  builtin_interfaces::msg::Time stamp;
  stamp.sec = static_cast<std::int32_t>(stamp_ns / 1000000000LL);
  stamp.nanosec = static_cast<std::uint32_t>(stamp_ns % 1000000000LL);
  return stamp;
}

}  // namespace

class ImuTimestampAdapterNode final : public rclcpp::Node
{
public:
  ImuTimestampAdapterNode()
  : Node("imu_timestamp_adapter_node")
  {
    input_topic_ = declare_parameter<std::string>("input_topic", "/capture/imu/data_raw");
    output_topic_ = declare_parameter<std::string>("output_topic", "/capture/imu/data");
    const double sample_rate_hz = declare_parameter<double>("sample_rate_hz", 400.0);
    timestamp_is_first_sample_ = declare_parameter<bool>(
      "packet_timestamp_is_first_sample", true);
    const int flush_timeout_ms = declare_parameter<int>("flush_timeout_ms", 5);
    const int maximum_bundle_samples = declare_parameter<int>("maximum_bundle_samples", 64);
    const double reset_threshold_s = declare_parameter<double>(
      "timestamp_reset_threshold_s", 1.0);
    if (!(sample_rate_hz > 0.0) || flush_timeout_ms <= 0 || maximum_bundle_samples <= 0 ||
      !(reset_threshold_s > 0.0))
    {
      throw std::invalid_argument("IMU时间展开参数必须为有限正数");
    }
    sample_period_ns_ = static_cast<std::int64_t>(std::llround(1.0e9 / sample_rate_hz));
    expander_ = std::make_unique<OdometryTimestampExpander>(
      sample_period_ns_, timestamp_is_first_sample_,
      static_cast<std::int64_t>(std::llround(reset_threshold_s * 1.0e9)));
    flush_timeout_ = std::chrono::milliseconds(flush_timeout_ms);
    maximum_bundle_samples_ = static_cast<std::size_t>(maximum_bundle_samples);

    publisher_ = create_publisher<sensor_msgs::msg::Imu>(
      output_topic_, rclcpp::QoS(rclcpp::KeepLast(1000)).best_effort().durability_volatile());
    subscription_ = create_subscription<sensor_msgs::msg::Imu>(
      input_topic_, rclcpp::QoS(rclcpp::KeepLast(1000)).best_effort().durability_volatile(),
      std::bind(&ImuTimestampAdapterNode::callback, this, std::placeholders::_1));
    flush_timer_ = create_wall_timer(
      flush_timeout_, std::bind(&ImuTimestampAdapterNode::flushExpiredBundle, this));

    RCLCPP_INFO(
      get_logger(), "IMU时间适配已启动：input=%s output=%s rate=%.3f Hz timestamp=%s。",
      input_topic_.c_str(), output_topic_.c_str(), sample_rate_hz,
      timestamp_is_first_sample_ ? "first_sample" : "last_sample");
  }

  ~ImuTimestampAdapterNode() override
  {
    flushBundle();
  }

private:
  void callback(const sensor_msgs::msg::Imu::ConstSharedPtr message)
  {
    const std::int64_t raw_stamp_ns = toNanoseconds(message->header.stamp);
    if (raw_stamp_ns <= 0) {
      RCLCPP_WARN_THROTTLE(get_logger(), *get_clock(), 5000, "收到无效IMU时间戳");
      return;
    }
    bool should_flush = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      should_flush = !bundle_.empty() && raw_stamp_ns != bundle_raw_stamp_ns_;
    }
    if (should_flush) {
      flushBundle();
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (bundle_.empty()) {
        bundle_raw_stamp_ns_ = raw_stamp_ns;
      }
      bundle_.push_back(*message);
      last_arrival_time_ = std::chrono::steady_clock::now();
    }
    bool bundle_full = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      bundle_full = bundle_.size() >= maximum_bundle_samples_;
    }
    if (bundle_full) {
      RCLCPP_WARN_THROTTLE(
        get_logger(), *get_clock(), 5000,
        "同时间戳IMU样本达到上限%zu，提前刷新。", maximum_bundle_samples_);
      flushBundle();
    }
  }

  void flushExpiredBundle()
  {
    bool expired = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      expired = !bundle_.empty() &&
        std::chrono::steady_clock::now() - last_arrival_time_ >= flush_timeout_;
    }
    if (expired) {
      flushBundle();
    }
  }

  void flushBundle()
  {
    std::deque<sensor_msgs::msg::Imu> bundle;
    std::int64_t raw_stamp_ns = 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (bundle_.empty()) {
        return;
      }
      bundle.swap(bundle_);
      raw_stamp_ns = bundle_raw_stamp_ns_;
      bundle_raw_stamp_ns_ = 0;
    }
    const auto expanded = expander_->expand(raw_stamp_ns, bundle.size());
    if (expanded.epoch_reset) {
      RCLCPP_WARN(get_logger(), "检测到IMU设备时间戳大幅回退，已开启新时间纪元");
    }
    if (expanded.stamps_ns.size() != bundle.size()) {
      RCLCPP_WARN_THROTTLE(
        get_logger(), *get_clock(), 5000, "丢弃时间戳无效、重叠或小幅乱序的IMU数据包");
      return;
    }
    for (const std::int64_t corrected_stamp_ns : expanded.stamps_ns) {
      auto message = std::move(bundle.front());
      bundle.pop_front();
      message.header.stamp = fromNanoseconds(corrected_stamp_ns);
      publisher_->publish(message);
    }
  }

  std::string input_topic_;
  std::string output_topic_;
  std::int64_t sample_period_ns_{2500000LL};
  std::int64_t bundle_raw_stamp_ns_{0};
  std::size_t maximum_bundle_samples_{64U};
  std::chrono::milliseconds flush_timeout_{5};
  bool timestamp_is_first_sample_{true};
  std::unique_ptr<OdometryTimestampExpander> expander_;
  std::mutex mutex_;
  std::deque<sensor_msgs::msg::Imu> bundle_;
  std::chrono::steady_clock::time_point last_arrival_time_{};
  rclcpp::Publisher<sensor_msgs::msg::Imu>::SharedPtr publisher_;
  rclcpp::Subscription<sensor_msgs::msg::Imu>::SharedPtr subscription_;
  rclcpp::TimerBase::SharedPtr flush_timer_;
};

}  // namespace motion_compensation

int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);
  try {
    rclcpp::spin(std::make_shared<motion_compensation::ImuTimestampAdapterNode>());
  } catch (const std::exception & error) {
    RCLCPP_FATAL(rclcpp::get_logger("imu_timestamp_adapter_node"), "%s", error.what());
    rclcpp::shutdown();
    return 1;
  }
  rclcpp::shutdown();
  return 0;
}

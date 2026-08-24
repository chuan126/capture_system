#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "cloud_visualization/cloud_preview_converter.hpp"
#include "interfaces/msg/cloud_preview_diagnostics.hpp"
#include "interfaces/msg/raw_clearance_diagnostics.hpp"
#include "rclcpp/rclcpp.hpp"
#include "sensor_msgs/msg/point_cloud2.hpp"

namespace cloud_visualization
{
namespace
{
std::int64_t stampNs(const std_msgs::msg::Header & header)
{
  return static_cast<std::int64_t>(header.stamp.sec) * 1'000'000'000LL + header.stamp.nanosec;
}
}  // namespace

class CloudVisualizationNode final : public rclcpp::Node
{
public:
  CloudVisualizationNode() : Node("cloud_visualization_node")
  {
    enabled_ = declare_parameter<bool>("enabled", true);
    input_topic_ = declare_parameter<std::string>("input_topic", "/capture/lidar/points_compensated_enu");
    expected_frame_id_ = declare_parameter<std::string>("expected_frame_id", "lidar_local_enu");
    output_topic_ = declare_parameter<std::string>("output_topic", "/capture/visualization/cloud_preview");
    diagnostics_topic_ = declare_parameter<std::string>("diagnostics_topic", "/capture/clearance/raw_diagnostics");
    preview_diagnostics_topic_ = declare_parameter<std::string>(
      "preview_diagnostics_topic", "/capture/visualization/diagnostics");
    publish_rate_hz_ = declare_parameter<double>("publish_rate_hz", 5.0);
    const auto configured_max_points = declare_parameter<std::int64_t>("max_points", 10000);
    voxel_size_m_ = declare_parameter<double>("voxel_size_m", 0.05);
    validateParameters(configured_max_points);
    max_points_ = static_cast<std::size_t>(configured_max_points);

    publisher_ = create_publisher<sensor_msgs::msg::PointCloud2>(
      output_topic_, rclcpp::QoS(rclcpp::KeepLast(1)).best_effort().durability_volatile());
    preview_diagnostics_publisher_ =
      create_publisher<interfaces::msg::CloudPreviewDiagnostics>(
      preview_diagnostics_topic_, rclcpp::QoS(rclcpp::KeepLast(4)).best_effort().durability_volatile());
    cloud_subscription_ = create_subscription<sensor_msgs::msg::PointCloud2>(
      input_topic_, rclcpp::QoS(rclcpp::KeepLast(1)).reliable().durability_volatile(),
      std::bind(&CloudVisualizationNode::onCloud, this, std::placeholders::_1));
    diagnostics_subscription_ = create_subscription<interfaces::msg::RawClearanceDiagnostics>(
      // 大索引数组只服务旁路着色，丢帧退蓝，禁止可靠传输反压正式净空计算。
      diagnostics_topic_, rclcpp::QoS(rclcpp::KeepLast(16)).best_effort().durability_volatile(),
      std::bind(&CloudVisualizationNode::onDiagnostics, this, std::placeholders::_1));
    timer_ = create_wall_timer(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::duration<double>(1.0 / publish_rate_hz_)),
      std::bind(&CloudVisualizationNode::publishLatest, this));
    RCLCPP_INFO(get_logger(), "三色旁路预览已启动：input=%s diagnostics=%s output=%s",
      input_topic_.c_str(), diagnostics_topic_.c_str(), output_topic_.c_str());
  }

private:
  void validateParameters(const std::int64_t configured_max_points) const
  {
    if (input_topic_.empty() || output_topic_.empty() || expected_frame_id_.empty() ||
      diagnostics_topic_.empty() || preview_diagnostics_topic_.empty()) {
      throw std::invalid_argument("点云预览Topic和frame参数不能为空");
    }
    if (publish_rate_hz_ < 1.0 || publish_rate_hz_ > 10.0) {
      throw std::invalid_argument("参数publish_rate_hz必须位于[1.0, 10.0] Hz");
    }
    if (configured_max_points < 500 || configured_max_points > 20000) {
      throw std::invalid_argument("参数max_points必须位于[500, 20000]");
    }
    if (!std::isfinite(voxel_size_m_) || voxel_size_m_ < 0.005 || voxel_size_m_ > 1.0) {
      throw std::invalid_argument("参数voxel_size_m必须位于[0.005, 1.0] m");
    }
  }

  void onCloud(const sensor_msgs::msg::PointCloud2::ConstSharedPtr message)
  {
    if (message->header.frame_id != expected_frame_id_) {
      RCLCPP_ERROR_THROTTLE(get_logger(), *get_clock(), 5000,
        "点云预览拒绝错误坐标帧：actual=%s expected=%s", message->header.frame_id.c_str(), expected_frame_id_.c_str());
      return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    latest_cloud_ = message;
    ++received_sequence_;
  }

  void onDiagnostics(const interfaces::msg::RawClearanceDiagnostics::ConstSharedPtr message)
  {
    if (stampNs(message->header) <= 0) {return;}
    std::lock_guard<std::mutex> lock(mutex_);
    const auto stamp = stampNs(message->header);
    if (diagnostics_by_stamp_.find(stamp) == diagnostics_by_stamp_.end()) {
      diagnostic_stamp_order_.push_back(stamp);
    }
    diagnostics_by_stamp_[stamp] = message;
    // 诊断可能晚于补偿预览帧到达；若该帧曾全蓝发布，触发一次同帧重新着色。
    if (latest_cloud_ && stampNs(latest_cloud_->header) == stamp) {
      ++received_sequence_;
    }
    while (diagnostic_stamp_order_.size() > 16U) {
      diagnostics_by_stamp_.erase(diagnostic_stamp_order_.front());
      diagnostic_stamp_order_.pop_front();
    }
  }

  void publishLatest()
  {
    if (!enabled_) {return;}
    sensor_msgs::msg::PointCloud2::ConstSharedPtr cloud;
    interfaces::msg::RawClearanceDiagnostics::ConstSharedPtr diagnostics;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!latest_cloud_ || published_sequence_ == received_sequence_) {return;}
      cloud = latest_cloud_;
      const auto diagnostic_it = diagnostics_by_stamp_.find(stampNs(cloud->header));
      if (diagnostic_it != diagnostics_by_stamp_.end()) {
        diagnostics = diagnostic_it->second;
        diagnostics_by_stamp_.erase(diagnostic_it);
        const auto order_it = std::find(
          diagnostic_stamp_order_.begin(), diagnostic_stamp_order_.end(), stampNs(cloud->header));
        if (order_it != diagnostic_stamp_order_.end()) {diagnostic_stamp_order_.erase(order_it);}
      }
      published_sequence_ = received_sequence_;
    }
    if (publisher_->get_subscription_count() == 0U && publisher_->get_intra_process_subscription_count() == 0U) {return;}

    const auto processing_start = std::chrono::steady_clock::now();
    const auto classification_start = processing_start;
    const std::size_t point_count = static_cast<std::size_t>(cloud->width) * cloud->height;
    std::vector<std::uint8_t> classifications(point_count, 0U);
    const bool classification_matched = diagnostics && diagnostics->input_point_count == point_count;
    if (classification_matched) {
      for (const auto index : diagnostics->roi_point_indices) {
        if (index < point_count) {classifications[index] = 1U;}
      }
      if (diagnostics->measurement_valid && diagnostics->lowest_cluster_valid) {
        for (const auto index : diagnostics->lowest_cluster_point_indices) {
          if (index < point_count) {classifications[index] = 2U;}
        }
      }
    }
    const auto classification_end = std::chrono::steady_clock::now();
    try {
      const auto conversion_start = std::chrono::steady_clock::now();
      auto output = converter_.convert(*cloud, max_points_, voxel_size_m_, classifications);
      const auto conversion_end = std::chrono::steady_clock::now();
      publisher_->publish(output);
      interfaces::msg::CloudPreviewDiagnostics preview_diagnostics;
      preview_diagnostics.header = cloud->header;
      preview_diagnostics.input_point_count = static_cast<std::uint32_t>(point_count);
      preview_diagnostics.output_point_count = output.width * output.height;
      preview_diagnostics.classification_matched = classification_matched;
      preview_diagnostics.classification_time_ms = std::chrono::duration<double, std::milli>(
        classification_end - classification_start).count();
      preview_diagnostics.conversion_time_ms = std::chrono::duration<double, std::milli>(
        conversion_end - conversion_start).count();
      preview_diagnostics.total_processing_time_ms = std::chrono::duration<double, std::milli>(
        conversion_end - processing_start).count();
      preview_diagnostics_publisher_->publish(preview_diagnostics);
    } catch (const std::invalid_argument & error) {
      RCLCPP_ERROR_THROTTLE(get_logger(), *get_clock(), 5000, "点云预览跳过不兼容输入：%s", error.what());
    }
  }

  bool enabled_{true};
  std::string input_topic_, expected_frame_id_, output_topic_, diagnostics_topic_;
  std::string preview_diagnostics_topic_;
  double publish_rate_hz_{5.0};
  std::size_t max_points_{10000U};
  double voxel_size_m_{0.05};
  CloudPreviewConverter converter_;
  std::mutex mutex_;
  sensor_msgs::msg::PointCloud2::ConstSharedPtr latest_cloud_;
  std::unordered_map<std::int64_t, interfaces::msg::RawClearanceDiagnostics::ConstSharedPtr>
    diagnostics_by_stamp_;
  std::deque<std::int64_t> diagnostic_stamp_order_;
  std::uint64_t received_sequence_{0U}, published_sequence_{0U};
  rclcpp::Publisher<sensor_msgs::msg::PointCloud2>::SharedPtr publisher_;
  rclcpp::Publisher<interfaces::msg::CloudPreviewDiagnostics>::SharedPtr
    preview_diagnostics_publisher_;
  rclcpp::Subscription<sensor_msgs::msg::PointCloud2>::SharedPtr cloud_subscription_;
  rclcpp::Subscription<interfaces::msg::RawClearanceDiagnostics>::SharedPtr diagnostics_subscription_;
  rclcpp::TimerBase::SharedPtr timer_;
};
}  // namespace cloud_visualization

int main(int argc, char * argv[])
{
  rclcpp::init(argc, argv);
  try {rclcpp::spin(std::make_shared<cloud_visualization::CloudVisualizationNode>());}
  catch (const std::exception & error) {
    RCLCPP_FATAL(rclcpp::get_logger("cloud_visualization_node"), "%s", error.what());
    rclcpp::shutdown();
    return 1;
  }
  rclcpp::shutdown();
  return 0;
}

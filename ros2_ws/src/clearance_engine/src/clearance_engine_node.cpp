#include "clearance_engine/clearance_estimator.hpp"

#include <interfaces/msg/clearance_result.hpp>
#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/point_cloud2.hpp>
#include <sensor_msgs/point_cloud2_iterator.hpp>
#include <std_msgs/msg/header.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace clearance_engine
{
namespace
{

bool hasFloat32Field(const sensor_msgs::msg::PointCloud2 & message, const std::string & name)
{
  for (const auto & field : message.fields) {
    if (field.name == name) {
      return field.datatype == sensor_msgs::msg::PointField::FLOAT32 && field.count == 1U;
    }
  }
  return false;
}

}  // namespace

class ClearanceEngineNode : public rclcpp::Node
{
public:
  ClearanceEngineNode()
  : Node("clearance_engine_node"), estimator_(loadConfig())
  {
    const auto input_topic = declare_parameter<std::string>(
      "input_topic", "/capture/lidar/points_compensated_enu");
    const auto output_topic = declare_parameter<std::string>(
      "output_topic", "/capture/clearance/result");
    expected_frame_id_ = declare_parameter<std::string>("expected_frame_id", "lidar_local_enu");
    if (expected_frame_id_.empty()) {
      throw std::invalid_argument("expected_frame_id不能为空");
    }

    result_publisher_ = create_publisher<interfaces::msg::ClearanceResult>(
      output_topic, rclcpp::QoS(rclcpp::KeepLast(10)).reliable());
    // 10 Hz实时测量只保留最新一帧，避免RANSAC耗时波动时累计旧点云。
    point_cloud_subscription_ = create_subscription<sensor_msgs::msg::PointCloud2>(
      input_topic, rclcpp::QoS(rclcpp::KeepLast(1)).reliable().durability_volatile(),
      std::bind(&ClearanceEngineNode::pointCloudCallback, this, std::placeholders::_1));

    RCLCPP_INFO(
      get_logger(),
      "单帧风机底面检测已启动：input=%s output=%s；10 Hz最新帧、多候选近水平小平面。",
      input_topic.c_str(), output_topic.c_str());
  }

private:
  ClearanceConfig loadConfig()
  {
    ClearanceConfig config;
    const auto positiveIntegerParameter = [this](
      const std::string & name, const int default_value) -> std::size_t
      {
        const int value = declare_parameter<int>(name, default_value);
        if (value <= 0) {
          throw std::invalid_argument(name + "必须是正整数");
        }
        return static_cast<std::size_t>(value);
      };
    config.min_range_m = declare_parameter<double>("filter.min_range_m", config.min_range_m);
    config.min_up_height_m = declare_parameter<double>(
      "roi.min_up_height_m", config.min_up_height_m);
    config.max_up_height_m = declare_parameter<double>(
      "roi.max_up_height_m", config.max_up_height_m);
    config.east_half_angle_deg = declare_parameter<double>(
      "roi.east_half_angle_deg", config.east_half_angle_deg);
    config.north_half_angle_deg = declare_parameter<double>(
      "roi.north_half_angle_deg", config.north_half_angle_deg);
    config.max_normal_angle_deg = declare_parameter<double>(
      "ransac.max_normal_angle_deg", config.max_normal_angle_deg);
    config.distance_threshold_m = declare_parameter<double>(
      "ransac.distance_threshold_m", config.distance_threshold_m);
    config.voxel_size_m = declare_parameter<double>(
      "ransac.voxel_size_m", config.voxel_size_m);
    config.max_iterations = declare_parameter<int>(
      "ransac.max_iterations", config.max_iterations);
    config.probability = declare_parameter<double>("ransac.probability", config.probability);
    config.max_candidate_planes = declare_parameter<int>(
      "ransac.max_candidate_planes", config.max_candidate_planes);
    config.min_remaining_points = positiveIntegerParameter(
      "ransac.min_remaining_points", static_cast<int>(config.min_remaining_points));
    config.min_inliers_absolute = positiveIntegerParameter(
      "ransac.min_inliers_absolute", static_cast<int>(config.min_inliers_absolute));
    config.min_inlier_ratio = declare_parameter<double>(
      "ransac.min_inlier_ratio", config.min_inlier_ratio);
    config.region_grid_size_m = declare_parameter<double>(
      "region.grid_size_m", config.region_grid_size_m);
    config.min_region_span_cells = positiveIntegerParameter(
      "region.min_span_cells", static_cast<int>(config.min_region_span_cells));
    config.min_region_occupied_cells = positiveIntegerParameter(
      "region.min_occupied_cells", static_cast<int>(config.min_region_occupied_cells));
    config.max_residual_p95_m = declare_parameter<double>(
      "region.max_residual_p95_m", config.max_residual_p95_m);
    return config;
  }

  void publishInvalid(
    const std_msgs::msg::Header & header, const std::string & reason,
    const double processing_time_ms)
  {
    interfaces::msg::ClearanceResult output;
    output.header = header;
    output.valid = false;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    output.lidar_to_top_m = nan;
    output.selected_area_m2 = nan;
    output.selected_tilt_deg = nan;
    output.residual_median_m = nan;
    output.residual_p95_m = nan;
    output.minimum_position_east_m = nan;
    output.minimum_position_north_m = nan;
    output.minimum_position_up_m = nan;
    output.valid_point_ratio = 0.0;
    output.invalid_reason = reason;
    output.processing_time_ms = processing_time_ms;
    result_publisher_->publish(output);
  }

  void pointCloudCallback(const sensor_msgs::msg::PointCloud2::ConstSharedPtr message)
  {
    const auto start = std::chrono::steady_clock::now();
    if (message->header.frame_id != expected_frame_id_) {
      publishInvalid(message->header, "INVALID_POINT_CLOUD_FRAME", 0.0);
      return;
    }
    if (message->is_bigendian || !hasFloat32Field(*message, "x") ||
      !hasFloat32Field(*message, "y") || !hasFloat32Field(*message, "z") ||
      message->point_step == 0U || message->data.size() <
      static_cast<std::size_t>(message->row_step) * message->height)
    {
      publishInvalid(message->header, "INVALID_POINT_CLOUD_LAYOUT", 0.0);
      return;
    }

    std::vector<Point3f> points;
    points.reserve(static_cast<std::size_t>(message->width) * message->height);
    try {
      sensor_msgs::PointCloud2ConstIterator<float> x(*message, "x");
      sensor_msgs::PointCloud2ConstIterator<float> y(*message, "y");
      sensor_msgs::PointCloud2ConstIterator<float> z(*message, "z");
      for (; x != x.end(); ++x, ++y, ++z) {
        // 补偿后点云字段固定为x=East、y=North、z=Up。
        points.push_back(Point3f{*x, *y, *z});
      }
    } catch (const std::runtime_error & error) {
      RCLCPP_WARN_THROTTLE(
        get_logger(), *get_clock(), 5000, "点云字段读取失败：%s", error.what());
      publishInvalid(message->header, "INVALID_POINT_CLOUD_LAYOUT", 0.0);
      return;
    }

    const ClearanceEstimate estimate = estimator_.estimate(points);
    const double elapsed_ms = std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now() - start).count();

    interfaces::msg::ClearanceResult output;
    output.header = message->header;
    output.valid = estimate.valid;
    output.candidate_count = static_cast<std::uint32_t>(estimate.candidates.size());
    output.valid_point_ratio = estimate.valid_point_ratio;
    output.invalid_reason = estimate.invalid_reason;
    output.processing_time_ms = elapsed_ms;
    if (estimate.valid) {
      output.lidar_to_top_m = estimate.selected.min_height_m;
      output.selected_inlier_count = static_cast<std::uint32_t>(estimate.selected.inlier_count);
      output.selected_area_m2 = estimate.selected.area_m2;
      output.selected_tilt_deg = estimate.selected.tilt_deg;
      output.residual_median_m = estimate.selected.residual_median_m;
      output.residual_p95_m = estimate.selected.residual_p95_m;
      output.minimum_position_east_m = estimate.selected.min_position_east_m;
      output.minimum_position_north_m = estimate.selected.min_position_north_m;
      output.minimum_position_up_m = estimate.selected.min_position_up_m;
    } else {
      const double nan = std::numeric_limits<double>::quiet_NaN();
      output.lidar_to_top_m = nan;
      output.selected_area_m2 = nan;
      output.selected_tilt_deg = nan;
      output.residual_median_m = nan;
      output.residual_p95_m = nan;
      output.minimum_position_east_m = nan;
      output.minimum_position_north_m = nan;
      output.minimum_position_up_m = nan;
    }
    result_publisher_->publish(output);
  }

  ClearanceEstimator estimator_;
  std::string expected_frame_id_;
  rclcpp::Publisher<interfaces::msg::ClearanceResult>::SharedPtr result_publisher_;
  rclcpp::Subscription<sensor_msgs::msg::PointCloud2>::SharedPtr point_cloud_subscription_;
};

}  // namespace clearance_engine

int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);
  try {
    rclcpp::spin(std::make_shared<clearance_engine::ClearanceEngineNode>());
  } catch (const std::exception & error) {
    RCLCPP_FATAL(rclcpp::get_logger("clearance_engine_node"), "%s", error.what());
    rclcpp::shutdown();
    return 1;
  }
  rclcpp::shutdown();
  return 0;
}

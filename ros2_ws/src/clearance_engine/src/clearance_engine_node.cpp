#include "clearance_engine/clearance_estimator.hpp"

#include <interfaces/msg/clearance_result.hpp>
#include <interfaces/msg/raw_clearance_diagnostics.hpp>
#include <rcl_interfaces/msg/set_parameters_result.hpp>
#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/point_cloud2.hpp>
#include <sensor_msgs/point_cloud2_iterator.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace clearance_engine
{
namespace
{

bool hasFloat32Field(const sensor_msgs::msg::PointCloud2 & message, const std::string & name)
{
  return std::any_of(message.fields.begin(), message.fields.end(), [&name, &message](const auto & field) {
    return field.name == name && field.datatype == sensor_msgs::msg::PointField::FLOAT32 &&
           field.count == 1U && field.offset + sizeof(float) <= message.point_step;
  });
}

double nanValue() {return std::numeric_limits<double>::quiet_NaN();}

}  // namespace

class ClearanceEngineNode final : public rclcpp::Node
{
public:
  ClearanceEngineNode()
  : Node("clearance_engine_node"), config_(loadConfig()), estimator_(config_)
  {
    input_topic_ = declare_parameter<std::string>("input_topic", "/capture/lidar/points_raw");
    output_topic_ = declare_parameter<std::string>("output_topic", "/capture/clearance/result");
    diagnostics_topic_ = declare_parameter<std::string>(
      "diagnostics_topic", "/capture/clearance/raw_diagnostics");
    expected_frame_id_ = declare_parameter<std::string>("expected_frame_id", "");
    if (input_topic_.empty() || output_topic_.empty() || diagnostics_topic_.empty()) {
      throw std::invalid_argument("净空Topic参数不能为空");
    }
    parameter_callback_ = add_on_set_parameters_callback(
      std::bind(&ClearanceEngineNode::onParameters, this, std::placeholders::_1));
    result_publisher_ = create_publisher<interfaces::msg::ClearanceResult>(
      output_topic_, rclcpp::QoS(rclcpp::KeepLast(10)).reliable());
    diagnostics_publisher_ = create_publisher<interfaces::msg::RawClearanceDiagnostics>(
      diagnostics_topic_, rclcpp::QoS(rclcpp::KeepLast(16)).best_effort().durability_volatile());
    subscription_ = create_subscription<sensor_msgs::msg::PointCloud2>(
      input_topic_, rclcpp::QoS(rclcpp::KeepLast(1)).reliable().durability_volatile(),
      std::bind(&ClearanceEngineNode::onCloud, this, std::placeholders::_1));
    RCLCPP_INFO(
      get_logger(),
      "原始本体系最低可信点簇净空已启动：input=%s output=%s diagnostics=%s frame=%s",
      input_topic_.c_str(), output_topic_.c_str(), diagnostics_topic_.c_str(),
      expected_frame_id_.empty() ? "由部署实测帧透传" : expected_frame_id_.c_str());
  }

private:
  ClearanceConfig loadConfig()
  {
    ClearanceConfig config;
    config.min_detection_x_m = declare_parameter<double>(
      "raw_cluster.min_detection_x_m", config.min_detection_x_m);
    config.max_detection_x_m = declare_parameter<double>(
      "raw_cluster.max_detection_x_m", config.max_detection_x_m);
    config.detection_radius_m = declare_parameter<double>(
      "raw_cluster.detection_radius_m", config.detection_radius_m);
    config.support_height_band_m = declare_parameter<double>(
      "raw_cluster.support_height_band_m", config.support_height_band_m);
    config.min_support_points = positiveSizeParameter(
      "raw_cluster.min_support_points", config.min_support_points);
    config.spatial_grid_size_m = declare_parameter<double>(
      "raw_cluster.spatial_grid_size_m", config.spatial_grid_size_m);
    config.min_occupied_cells = positiveSizeParameter(
      "raw_cluster.min_occupied_cells", config.min_occupied_cells);
    config.min_spatial_span_m = declare_parameter<double>(
      "raw_cluster.min_spatial_span_m", config.min_spatial_span_m);
    return config;
  }

  std::size_t positiveSizeParameter(const std::string & name, const std::size_t default_value)
  {
    const auto value = declare_parameter<std::int64_t>(name, static_cast<std::int64_t>(default_value));
    if (value <= 0) {
      throw std::invalid_argument(name + "必须为正整数");
    }
    return static_cast<std::size_t>(value);
  }

  rcl_interfaces::msg::SetParametersResult onParameters(
    const std::vector<rclcpp::Parameter> & parameters)
  {
    rcl_interfaces::msg::SetParametersResult result;
    ClearanceConfig next;
    {
      std::lock_guard<std::mutex> lock(estimator_mutex_);
      next = estimator_.config();
    }
    try {
      for (const auto & parameter : parameters) {
        const auto & name = parameter.get_name();
        if (name == "raw_cluster.min_detection_x_m") {
          next.min_detection_x_m = parameter.as_double();
        } else if (name == "raw_cluster.max_detection_x_m") {
          next.max_detection_x_m = parameter.as_double();
        } else if (name == "raw_cluster.detection_radius_m") {
          next.detection_radius_m = parameter.as_double();
        } else if (name == "raw_cluster.support_height_band_m") {
          next.support_height_band_m = parameter.as_double();
        } else if (name == "raw_cluster.min_support_points") {
          if (parameter.as_int() <= 0) {throw std::invalid_argument(name + "必须为正整数");}
          next.min_support_points = static_cast<std::size_t>(parameter.as_int());
        } else if (name == "raw_cluster.spatial_grid_size_m") {
          next.spatial_grid_size_m = parameter.as_double();
        } else if (name == "raw_cluster.min_occupied_cells") {
          if (parameter.as_int() <= 0) {throw std::invalid_argument(name + "必须为正整数");}
          next.min_occupied_cells = static_cast<std::size_t>(parameter.as_int());
        } else if (name == "raw_cluster.min_spatial_span_m") {
          next.min_spatial_span_m = parameter.as_double();
        } else {
          result.successful = false;
          result.reason = "参数不支持运行时修改：" + name;
          return result;
        }
      }
      ClearanceEstimator validated(next);
      {
        std::lock_guard<std::mutex> lock(estimator_mutex_);
        config_ = next;
        estimator_ = std::move(validated);
      }
      result.successful = true;
      result.reason = "当前运行值已应用，节点重启后恢复YAML值";
    } catch (const std::exception & error) {
      result.successful = false;
      result.reason = error.what();
    }
    return result;
  }

  void publish(
    const sensor_msgs::msg::PointCloud2 & message, const ClearanceEstimate & estimate,
    const double elapsed_ms)
  {
    interfaces::msg::ClearanceResult output;
    output.header = message.header;
    output.valid = estimate.valid;
    output.lidar_to_top_m = estimate.valid ? estimate.cluster_median_x : nanValue();
    output.ransac_plane_count = 0U;
    output.surface_count = 0U;
    output.candidate_count = estimate.valid ? 1U : 0U;
    output.selected_inlier_count = static_cast<std::uint32_t>(estimate.cluster_point_indices.size());
    output.selected_area_m2 = nanValue();
    output.selected_tilt_deg = nanValue();
    output.residual_median_m = nanValue();
    output.residual_p95_m = nanValue();
    // 原始本体系坐标不得冒充ENU；真实代表点通过RawClearanceDiagnostics发布。
    output.minimum_position_east_m = nanValue();
    output.minimum_position_north_m = nanValue();
    output.minimum_position_up_m = nanValue();
    output.minimum_point_x_m = estimate.valid ? estimate.representative.x : nanValue();
    output.minimum_point_y_m = estimate.valid ? estimate.representative.y : nanValue();
    output.minimum_point_z_m = estimate.valid ? estimate.representative.z : nanValue();
    output.valid_point_ratio = estimate.valid_point_ratio;
    output.invalid_reason = estimate.invalid_reason;
    output.processing_time_ms = elapsed_ms;
    result_publisher_->publish(output);

    interfaces::msg::RawClearanceDiagnostics diagnostics;
    diagnostics.header = message.header;
    diagnostics.input_point_count = static_cast<std::uint32_t>(estimate.input_point_count);
    diagnostics.valid_raw_point_count = static_cast<std::uint32_t>(estimate.valid_point_count);
    diagnostics.radius_roi_point_count = static_cast<std::uint32_t>(estimate.roi_point_count);
    diagnostics.detection_radius_m = estimate.detection_radius_m;
    diagnostics.lowest_raw_x = estimate.roi_point_count > 0U ? estimate.lowest_raw_x : nanValue();
    diagnostics.lowest_cluster_valid = estimate.valid;
    diagnostics.lowest_cluster_point_count =
      static_cast<std::uint32_t>(estimate.cluster_point_indices.size());
    diagnostics.lowest_cluster_min_x = estimate.valid ? estimate.cluster_min_x : nanValue();
    diagnostics.lowest_cluster_median_x = estimate.valid ? estimate.cluster_median_x : nanValue();
    diagnostics.lowest_cluster_max_x = estimate.valid ? estimate.cluster_max_x : nanValue();
    diagnostics.representative_raw_x = estimate.valid ? estimate.representative.x : nanValue();
    diagnostics.representative_raw_y = estimate.valid ? estimate.representative.y : nanValue();
    diagnostics.representative_raw_z = estimate.valid ? estimate.representative.z : nanValue();
    diagnostics.roi_point_indices = estimate.roi_point_indices;
    diagnostics.lowest_cluster_point_indices = estimate.cluster_point_indices;
    diagnostics.measurement_valid = estimate.valid;
    diagnostics.filtering_time_ms = estimate.filtering_time_ms;
    diagnostics.roi_time_ms = estimate.roi_time_ms;
    diagnostics.sorting_time_ms = estimate.sorting_time_ms;
    diagnostics.support_band_time_ms = estimate.support_band_time_ms;
    diagnostics.connectivity_time_ms = estimate.connectivity_time_ms;
    diagnostics.processing_time_ms = elapsed_ms;
    diagnostics_publisher_->publish(diagnostics);
  }

  void publishInvalid(const sensor_msgs::msg::PointCloud2 & message, const std::string & reason)
  {
    ClearanceEstimate estimate;
    estimate.input_point_count = static_cast<std::size_t>(message.width) * message.height;
    estimate.invalid_reason = reason;
    {
      std::lock_guard<std::mutex> lock(estimator_mutex_);
      estimate.detection_radius_m = estimator_.config().detection_radius_m;
    }
    publish(message, estimate, 0.0);
  }

  void onCloud(const sensor_msgs::msg::PointCloud2::ConstSharedPtr message)
  {
    const auto start = std::chrono::steady_clock::now();
    if (message->header.stamp.sec == 0 && message->header.stamp.nanosec == 0U) {
      publishInvalid(*message, "INVALID_POINT_CLOUD_TIME");
      return;
    }
    if (message->header.frame_id.empty()) {
      publishInvalid(*message, "INVALID_POINT_CLOUD_FRAME");
      return;
    }
    if (!expected_frame_id_.empty() && message->header.frame_id != expected_frame_id_) {
      publishInvalid(*message, "INVALID_POINT_CLOUD_FRAME");
      return;
    }
    const std::size_t expected_size = message->height == 0U ? 0U :
      (static_cast<std::size_t>(message->height) - 1U) * message->row_step +
      static_cast<std::size_t>(message->width) * message->point_step;
    if (message->is_bigendian || !hasFloat32Field(*message, "x") ||
      !hasFloat32Field(*message, "y") || !hasFloat32Field(*message, "z") ||
      message->point_step == 0U || message->data.size() < expected_size)
    {
      publishInvalid(*message, "INVALID_POINT_CLOUD_LAYOUT");
      return;
    }
    std::vector<Point3f> points;
    const std::size_t count = static_cast<std::size_t>(message->width) * message->height;
    points.reserve(count);
    try {
      sensor_msgs::PointCloud2ConstIterator<float> x(*message, "x");
      sensor_msgs::PointCloud2ConstIterator<float> y(*message, "y");
      sensor_msgs::PointCloud2ConstIterator<float> z(*message, "z");
      std::uint32_t index = 0U;
      for (; x != x.end(); ++x, ++y, ++z, ++index) {
        points.push_back(Point3f{*x, *y, *z, index});
      }
    } catch (const std::runtime_error & error) {
      RCLCPP_WARN_THROTTLE(get_logger(), *get_clock(), 5000, "原始点云字段读取失败：%s", error.what());
      publishInvalid(*message, "INVALID_POINT_CLOUD_LAYOUT");
      return;
    }
    // 空配置只允许首个布局完整且可解析的原始帧建立坐标语义，损坏消息不能污染锁存。
    if (expected_frame_id_.empty()) {
      if (observed_frame_id_.empty()) {
        observed_frame_id_ = message->header.frame_id;
        RCLCPP_INFO(get_logger(), "锁存原始点云坐标帧：%s", observed_frame_id_.c_str());
      } else if (message->header.frame_id != observed_frame_id_) {
        publishInvalid(*message, "INVALID_POINT_CLOUD_FRAME");
        return;
      }
    }
    ClearanceEstimate estimate;
    {
      std::lock_guard<std::mutex> lock(estimator_mutex_);
      estimate = estimator_.estimate(points);
    }
    const double elapsed_ms = std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now() - start).count();
    publish(*message, estimate, elapsed_ms);
    RCLCPP_DEBUG_THROTTLE(
      get_logger(), *get_clock(), 1000,
      "raw=%zu valid=%zu roi=%zu cluster=%zu median_x=%.3f valid=%s reason=%s time=%.3fms",
      estimate.input_point_count, estimate.valid_point_count, estimate.roi_point_count,
      estimate.cluster_point_indices.size(), estimate.valid ? estimate.cluster_median_x : nanValue(),
      estimate.valid ? "true" : "false", estimate.invalid_reason.c_str(), elapsed_ms);
  }

  ClearanceConfig config_;
  ClearanceEstimator estimator_;
  std::mutex estimator_mutex_;
  std::string input_topic_;
  std::string output_topic_;
  std::string diagnostics_topic_;
  std::string expected_frame_id_;
  std::string observed_frame_id_;
  rclcpp::node_interfaces::OnSetParametersCallbackHandle::SharedPtr parameter_callback_;
  rclcpp::Publisher<interfaces::msg::ClearanceResult>::SharedPtr result_publisher_;
  rclcpp::Publisher<interfaces::msg::RawClearanceDiagnostics>::SharedPtr diagnostics_publisher_;
  rclcpp::Subscription<sensor_msgs::msg::PointCloud2>::SharedPtr subscription_;
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

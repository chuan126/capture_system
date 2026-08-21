#include "clearance_engine/clearance_estimator.hpp"
#include "clearance_engine/surface_candidate.hpp"
#include "clearance_engine/surface_detector.hpp"

#include <interfaces/msg/clearance_result.hpp>
#include <rclcpp/rclcpp.hpp>
#include <rcl_interfaces/msg/set_parameters_result.hpp>
#include <sensor_msgs/msg/point_cloud2.hpp>
#include <sensor_msgs/point_cloud2_iterator.hpp>
#include <std_msgs/msg/header.hpp>

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
  : Node("clearance_engine_node"), plane_config_(loadConfig()), estimator_(plane_config_),
    surface_detector_(loadSurfaceConfig(plane_config_))
  {
    const auto input_topic = declare_parameter<std::string>(
      "input_topic", "/capture/lidar/points_compensated_enu");
    const auto output_topic = declare_parameter<std::string>(
      "output_topic", "/capture/clearance/result");
    expected_frame_id_ = declare_parameter<std::string>("expected_frame_id", "lidar_local_enu");
    if (expected_frame_id_.empty()) {
      throw std::invalid_argument("expected_frame_id不能为空");
    }

    parameter_callback_ = add_on_set_parameters_callback(
      std::bind(&ClearanceEngineNode::onParameters, this, std::placeholders::_1));

    result_publisher_ = create_publisher<interfaces::msg::ClearanceResult>(
      output_topic, rclcpp::QoS(rclcpp::KeepLast(10)).reliable());
    // 10 Hz实时测量只保留最新一帧，避免RANSAC耗时波动时累计旧点云。
    point_cloud_subscription_ = create_subscription<sensor_msgs::msg::PointCloud2>(
      input_topic, rclcpp::QoS(rclcpp::KeepLast(1)).reliable().durability_volatile(),
      std::bind(&ClearanceEngineNode::pointCloudCallback, this, std::placeholders::_1));

    RCLCPP_INFO(
      get_logger(),
      "净空平面/曲面检测已启动：input=%s output=%s；surface=%s update_rate=%.2f Hz。",
      input_topic.c_str(), output_topic.c_str(),
      surface_detector_.config().enabled ? "enabled" : "disabled",
      surface_detector_.config().update_rate_hz);
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

  SurfaceDetectorConfig loadSurfaceConfig(const ClearanceConfig & plane_config)
  {
    SurfaceDetectorConfig config;
    config.min_range_m = plane_config.min_range_m;
    config.min_up_height_m = plane_config.min_up_height_m;
    config.max_up_height_m = plane_config.max_up_height_m;
    config.east_half_angle_deg = plane_config.east_half_angle_deg;
    config.north_half_angle_deg = plane_config.north_half_angle_deg;

    const auto positiveIntegerParameter = [this](
      const std::string & name, const int default_value) -> std::size_t
      {
        const int value = declare_parameter<int>(name, default_value);
        if (value <= 0) {
          throw std::invalid_argument(name + "必须是正整数");
        }
        return static_cast<std::size_t>(value);
      };
    config.enabled = declare_parameter<bool>("surface.enabled", config.enabled);
    config.voxel_size_m = declare_parameter<double>(
      "surface.voxel_size_m", config.voxel_size_m);
    config.normal_k_neighbors = declare_parameter<int>(
      "surface.normal_k_neighbors", config.normal_k_neighbors);
    config.region_neighbor_number = declare_parameter<int>(
      "surface.region_neighbor_number", config.region_neighbor_number);
    config.smoothness_threshold_deg = declare_parameter<double>(
      "surface.smoothness_threshold_deg", config.smoothness_threshold_deg);
    config.curvature_threshold = declare_parameter<double>(
      "surface.curvature_threshold", config.curvature_threshold);
    config.min_cluster_points = positiveIntegerParameter(
      "surface.min_cluster_points", static_cast<int>(config.min_cluster_points));
    config.min_span_m = declare_parameter<double>("surface.min_span_m", config.min_span_m);
    config.grid_size_m = declare_parameter<double>("surface.grid_size_m", config.grid_size_m);
    config.min_occupied_cells = positiveIntegerParameter(
      "surface.min_occupied_cells", static_cast<int>(config.min_occupied_cells));
    config.max_residual_p95_m = declare_parameter<double>(
      "surface.max_residual_p95_m", config.max_residual_p95_m);
    config.max_curvature = declare_parameter<double>(
      "surface.max_curvature", config.max_curvature);
    config.min_downward_normal_z = declare_parameter<double>(
      "surface.min_downward_normal_z", config.min_downward_normal_z);
    config.max_input_points = positiveIntegerParameter(
      "surface.max_input_points", static_cast<int>(config.max_input_points));
    config.min_confidence = declare_parameter<double>(
      "surface.min_confidence", config.min_confidence);
    config.plane_preference_tolerance_m = declare_parameter<double>(
      "surface.plane_preference_tolerance_m", config.plane_preference_tolerance_m);
    config.update_rate_hz = declare_parameter<double>(
      "surface.update_rate_hz", config.update_rate_hz);
    return config;
  }

  rcl_interfaces::msg::SetParametersResult onParameters(
    const std::vector<rclcpp::Parameter> & parameters)
  {
    rcl_interfaces::msg::SetParametersResult result;
    result.successful = false;
    ClearanceConfig next;
    {
      std::lock_guard<std::mutex> lock(estimator_mutex_);
      next = estimator_.config();
    }

    try {
      for (const auto & parameter : parameters) {
        const auto & name = parameter.get_name();
        if (name == "ransac.distance_threshold_m") {
          next.distance_threshold_m = parameter.as_double();
        } else if (name == "ransac.voxel_size_m") {
          next.voxel_size_m = parameter.as_double();
        } else if (name == "ransac.max_candidate_planes") {
          next.max_candidate_planes = static_cast<int>(parameter.as_int());
        } else if (name == "ransac.min_inliers_absolute") {
          const auto value = parameter.as_int();
          if (value <= 0) {
            throw std::invalid_argument(name + "必须为正整数");
          }
          next.min_inliers_absolute = static_cast<std::size_t>(value);
        } else if (name == "region.grid_size_m") {
          next.region_grid_size_m = parameter.as_double();
        } else if (name == "region.min_occupied_cells") {
          const auto value = parameter.as_int();
          if (value <= 0) {
            throw std::invalid_argument(name + "必须为正整数");
          }
          next.min_region_occupied_cells = static_cast<std::size_t>(value);
        } else if (name == "region.max_residual_p95_m") {
          next.max_residual_p95_m = parameter.as_double();
        } else {
          result.reason = "参数不支持运行时修改：" + name;
          return result;
        }
      }
      if (!(next.distance_threshold_m > 0.0) || !(next.voxel_size_m > 0.0) ||
        next.max_candidate_planes <= 0 || !(next.region_grid_size_m > 0.0) ||
        !(next.max_residual_p95_m > 0.0))
      {
        throw std::invalid_argument("运行时净空参数必须保持正值");
      }
      {
        std::lock_guard<std::mutex> lock(estimator_mutex_);
        plane_config_ = next;
        estimator_ = ClearanceEstimator(next);
      }
      result.successful = true;
      result.reason = "当前运行值已应用，节点重启后恢复YAML值";
    } catch (const std::exception & error) {
      result.reason = error.what();
    }
    return result;
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
    RCLCPP_DEBUG(
      get_logger(),
      "ROI points: 0; Plane candidate count: 0; Plane accepted: false; "
      "Surface cluster count: 0; Surface accepted: 0; Final clearance height: nan; "
      "Selected type: NONE; Invalid reason: %s",
      reason.c_str());
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

    ClearanceEstimate estimate;
    const auto plane_start = std::chrono::steady_clock::now();
    {
      std::lock_guard<std::mutex> lock(estimator_mutex_);
      estimate = estimator_.estimate(points);
    }
    const double plane_time_ms = std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now() - plane_start).count();

    SurfaceDetectionResult surface_result;
    const auto surface_config = surface_detector_.config();
    const auto current_time = std::chrono::steady_clock::now();
    const double surface_period_s = 1.0 / surface_config.update_rate_hz;
    const bool surface_due = !surface_has_run_ ||
      std::chrono::duration<double>(current_time - last_surface_run_).count() >= surface_period_s;
    const bool run_surface = surface_config.enabled && (!estimate.valid || surface_due);
    if (run_surface) {
      surface_result = surface_detector_.detect(points);
      last_surface_run_ = std::chrono::steady_clock::now();
      surface_has_run_ = true;
    } else {
      surface_result.invalid_reason = surface_config.enabled ?
        "SURFACE_RATE_LIMITED" : "SURFACE_DISABLED";
    }

    std::vector<SurfaceCandidate> candidates;
    candidates.reserve(estimate.candidates.size() + surface_result.candidates.size());
    for (const PlaneCandidate & plane : estimate.candidates) {
      candidates.push_back(makeSurfaceCandidate(plane));
    }
    candidates.insert(
      candidates.end(), surface_result.candidates.begin(), surface_result.candidates.end());
    const CandidateSelection selection = selectLowestConfidentCandidate(
      candidates, surface_config.min_confidence, surface_config.plane_preference_tolerance_m);
    const std::size_t accepted_surface_count = static_cast<std::size_t>(std::count_if(
        surface_result.candidates.begin(), surface_result.candidates.end(),
        [&surface_config](const SurfaceCandidate & candidate) {
          return std::isfinite(candidate.confidence) &&
                 candidate.confidence > surface_config.min_confidence;
        }));
    const double elapsed_ms = std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now() - start).count();

    interfaces::msg::ClearanceResult output;
    output.header = message->header;
    output.valid = selection.valid;
    output.ransac_plane_count = static_cast<std::uint32_t>(estimate.ransac_plane_count);
    output.surface_count = static_cast<std::uint32_t>(accepted_surface_count);
    output.candidate_count = static_cast<std::uint32_t>(selection.accepted_count);
    output.valid_point_ratio = estimate.valid_point_ratio;
    if (selection.valid) {
      output.invalid_reason = "NONE";
    } else if (!run_surface) {
      output.invalid_reason = estimate.invalid_reason;
    } else if (surface_result.valid) {
      output.invalid_reason = "NO_CANDIDATE_ABOVE_CONFIDENCE";
    } else {
      output.invalid_reason =
        "PLANE:" + estimate.invalid_reason + "|SURFACE:" + surface_result.invalid_reason;
    }
    output.processing_time_ms = elapsed_ms;
    if (selection.valid) {
      output.lidar_to_top_m = selection.selected.min_height_m;
      output.selected_inlier_count = static_cast<std::uint32_t>(selection.selected.point_count);
      // 兼容字段selected_area_m2表示选中连通区域的水平投影占用网格面积，不是几何平面表面积。
      output.selected_area_m2 = selection.selected.area_m2;
      output.selected_tilt_deg = selection.selected.tilt_deg;
      output.residual_median_m = selection.selected.residual_median_m;
      output.residual_p95_m = selection.selected.residual_p95_m;
      output.minimum_position_east_m = selection.selected.min_position_east_m;
      output.minimum_position_north_m = selection.selected.min_position_north_m;
      output.minimum_position_up_m = selection.selected.min_position_up_m;
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
    RCLCPP_DEBUG(
      get_logger(),
      "ROI points: %zu; Plane candidate count: %zu; Plane accepted: %s; "
      "Surface cluster count: %zu; Surface accepted: %zu; Final clearance height: %.3f; "
      "Selected type: %s; Plane reason: %s; Surface reason: %s; "
      "Plane time: %.3f ms; Surface time: %.3f ms; Total time: %.3f ms",
      estimate.valid_point_count, estimate.candidates.size(), estimate.valid ? "true" : "false",
      surface_result.cluster_count, accepted_surface_count,
      selection.valid ? selection.selected.min_height_m :
      std::numeric_limits<double>::quiet_NaN(),
      selection.valid ? surfaceCandidateTypeName(selection.selected.type) : "NONE",
      estimate.invalid_reason.c_str(), surface_result.invalid_reason.c_str(),
      plane_time_ms, surface_result.processing_time_ms, elapsed_ms);
    result_publisher_->publish(output);
  }

  ClearanceConfig plane_config_;
  ClearanceEstimator estimator_;
  SurfaceDetector surface_detector_;
  std::mutex estimator_mutex_;
  bool surface_has_run_{false};
  std::chrono::steady_clock::time_point last_surface_run_{};
  rclcpp::node_interfaces::OnSetParametersCallbackHandle::SharedPtr parameter_callback_;
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

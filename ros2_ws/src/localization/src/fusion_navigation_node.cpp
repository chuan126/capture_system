#include "localization/attitude_transform.hpp"
#include "localization/dead_reckoning.hpp"
#include "localization/fusion_navigator.hpp"
#include "localization/geodesy.hpp"
#include "localization/heading_alignment.hpp"
#include "localization/heading_rigid_alignment.hpp"
#include "localization/lidar_localizer.hpp"
#include "localization/odometry_buffer.hpp"
#include "localization/sensor_synchronizer.hpp"

#include "builtin_interfaces/msg/time.hpp"
#include "diagnostic_msgs/msg/diagnostic_array.hpp"
#include "diagnostic_msgs/msg/diagnostic_status.hpp"
#include "diagnostic_msgs/msg/key_value.hpp"
#include "geometry_msgs/msg/quaternion.hpp"
#include "interfaces/msg/localization_status.hpp"
#include "interfaces/msg/rtk_status.hpp"
#include "nav_msgs/msg/odometry.hpp"
#include "rclcpp/rclcpp.hpp"
#include "rclcpp/executors/multi_threaded_executor.hpp"
#include "sensor_msgs/msg/imu.hpp"
#include "sensor_msgs/msg/nav_sat_fix.hpp"
#include "sensor_msgs/msg/nav_sat_status.hpp"
#include "sensor_msgs/msg/point_cloud2.hpp"
#include "sensor_msgs/msg/point_field.hpp"
#include "sensor_msgs/point_cloud2_iterator.hpp"

#include <Eigen/Core>
#include <Eigen/Geometry>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace localization
{
namespace
{

constexpr double kNanosecondsToSeconds = 1.0e-9;
constexpr double kSecondsToNanoseconds = 1.0e9;

std::int64_t toNanoseconds(const builtin_interfaces::msg::Time & stamp) noexcept
{
  return static_cast<std::int64_t>(stamp.sec) * 1000000000LL +
         static_cast<std::int64_t>(stamp.nanosec);
}

builtin_interfaces::msg::Time fromNanoseconds(const std::int64_t stamp_ns) noexcept
{
  builtin_interfaces::msg::Time stamp;
  if (stamp_ns <= 0) {
    return stamp;
  }
  stamp.sec = static_cast<std::int32_t>(stamp_ns / 1000000000LL);
  stamp.nanosec = static_cast<std::uint32_t>(stamp_ns % 1000000000LL);
  return stamp;
}

std::int64_t steadyNowNanoseconds() noexcept
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count();
}

double ageSeconds(const std::int64_t now_ns, const std::int64_t stamp_ns) noexcept
{
  if (now_ns <= 0 || stamp_ns <= 0) {
    return std::numeric_limits<double>::infinity();
  }
  return static_cast<double>(std::max<std::int64_t>(0, now_ns - stamp_ns)) *
         kNanosecondsToSeconds;
}

double finiteOrZero(const double value) noexcept
{
  return std::isfinite(value) ? value : 0.0;
}

diagnostic_msgs::msg::KeyValue diagnosticValue(
  const std::string & key, const std::string & value)
{
  diagnostic_msgs::msg::KeyValue result;
  result.key = key;
  result.value = value;
  return result;
}

Eigen::Quaterniond rosQuaternion(const geometry_msgs::msg::Quaternion & quaternion) noexcept
{
  return Eigen::Quaterniond(quaternion.w, quaternion.x, quaternion.y, quaternion.z);
}

bool finiteQuaternion(const Eigen::Quaterniond & quaternion) noexcept
{
  return quaternion.coeffs().array().isFinite().all() && quaternion.norm() > 1.0e-12;
}

bool hasFloat32Field(const sensor_msgs::msg::PointCloud2 & message, const std::string & name)
{
  return std::any_of(
    message.fields.begin(), message.fields.end(), [&name](const auto & field) {
      return field.name == name && field.datatype == sensor_msgs::msg::PointField::FLOAT32;
    });
}

struct HistoryState
{
  std::int64_t stamp_ns{0};
  Eigen::Vector3d position_m{Eigen::Vector3d::Zero()};
  Eigen::Vector3d velocity_mps{Eigen::Vector3d::Zero()};
  Eigen::Quaterniond orientation_local_from_body{Eigen::Quaterniond::Identity()};
};

}  // namespace

class FusionNavigationNode final : public rclcpp::Node
{
public:
  FusionNavigationNode()
  : Node("fusion_navigation_node"),
    navigator_(declareFusionConfig()),
    synchronizer_(
      secondsToNs(declare_parameter<double>("sensor_sync_max_gap_s", 0.015)),
      secondsToNs(declare_parameter<double>("timestamp_reset_threshold_s", 1.0)),
      static_cast<std::size_t>(declare_parameter<int>("sensor_sync_max_samples", 2000))),
    heading_options_(declareHeadingOptions()),
    heading_estimator_(heading_options_),
    lidar_localizer_(declareLidarConfig())
  {
    declareRemainingParameters();
    validateParameters();

    high_rate_group_ = create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
    rtk_group_ = create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
    lidar_group_ = create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
    rclcpp::SubscriptionOptions high_rate_options;
    high_rate_options.callback_group = high_rate_group_;
    rclcpp::SubscriptionOptions rtk_options;
    rtk_options.callback_group = rtk_group_;
    rclcpp::SubscriptionOptions lidar_options;
    lidar_options.callback_group = lidar_group_;

    const auto reliable_state_qos =
      rclcpp::QoS(rclcpp::KeepLast(20)).reliable().durability_volatile();
    orientation_subscription_ = create_subscription<nav_msgs::msg::Odometry>(
      odin_orientation_topic_,
      rclcpp::QoS(rclcpp::KeepLast(1000)).reliable().durability_volatile(),
      std::bind(&FusionNavigationNode::onOdinOrientation, this, std::placeholders::_1),
      high_rate_options);
    imu_subscription_ = create_subscription<sensor_msgs::msg::Imu>(
      imu_topic_, rclcpp::QoS(rclcpp::KeepLast(1000)).best_effort().durability_volatile(),
      std::bind(&FusionNavigationNode::onImu, this, std::placeholders::_1), high_rate_options);
    rtk_fix_subscription_ = create_subscription<sensor_msgs::msg::NavSatFix>(
      rtk_fix_topic_, reliable_state_qos,
      std::bind(&FusionNavigationNode::onRtkFix, this, std::placeholders::_1), rtk_options);
    rtk_status_subscription_ = create_subscription<interfaces::msg::RtkStatus>(
      rtk_status_topic_, reliable_state_qos,
      std::bind(&FusionNavigationNode::onRtkStatus, this, std::placeholders::_1), rtk_options);
    auto cloud_qos = rclcpp::SensorDataQoS();
    cloud_qos.keep_last(1);
    lidar_subscription_ = create_subscription<sensor_msgs::msg::PointCloud2>(
      compensated_cloud_topic_, cloud_qos,
      std::bind(&FusionNavigationNode::onCompensatedCloud, this, std::placeholders::_1),
      lidar_options);

    fusion_odometry_publisher_ = create_publisher<nav_msgs::msg::Odometry>(
      fusion_odometry_topic_,
      rclcpp::QoS(rclcpp::KeepLast(1000)).reliable().durability_volatile());
    odometry_publisher_ = create_publisher<nav_msgs::msg::Odometry>(
      localization_odometry_topic_, reliable_state_qos);
    fix_publisher_ = create_publisher<sensor_msgs::msg::NavSatFix>(
      localization_fix_topic_, reliable_state_qos);
    status_publisher_ = create_publisher<interfaces::msg::LocalizationStatus>(
      localization_status_topic_, reliable_state_qos);
    diagnostics_publisher_ = create_publisher<diagnostic_msgs::msg::DiagnosticArray>(
      diagnostics_topic_, reliable_state_qos);

    const auto output_period = std::chrono::duration<double>(1.0 / output_rate_hz_);
    output_timer_ = create_wall_timer(
      std::chrono::duration_cast<std::chrono::nanoseconds>(output_period),
      std::bind(&FusionNavigationNode::publishOutputs, this));
    diagnostics_timer_ = create_wall_timer(
      std::chrono::seconds(1), std::bind(&FusionNavigationNode::publishDiagnostics, this));

    RCLCPP_INFO(
      get_logger(),
      "连续组合导航已启动：ODIN quaternion=%s IMU=%s RTK=%s LiDAR=%s fusion=%s",
      odin_orientation_topic_.c_str(), imu_topic_.c_str(), rtk_fix_topic_.c_str(),
      compensated_cloud_topic_.c_str(), fusion_odometry_topic_.c_str());
    RCLCPP_INFO(
      get_logger(), "ODIN pose.position和twist.linear仅保留在原始Topic作诊断，不进入融合计算");
  }

private:
  struct LatestFix
  {
    bool available{false};
    std::int64_t receipt_ns{0};
    std::int8_t status{sensor_msgs::msg::NavSatStatus::STATUS_NO_FIX};
    Llh llh;
  };

  struct LatestRtkStatus
  {
    bool available{false};
    std::int64_t receipt_ns{0};
    std::uint8_t rmc_validity{0U};
    std::uint8_t gps_state{0U};
    std::uint8_t satellite_count{0U};
    double hdop{0.0};
    double latitude_sigma_m{0.0};
    double longitude_sigma_m{0.0};
    double height_sigma_m{0.0};
  };

  static std::int64_t secondsToNs(const double seconds) noexcept
  {
    return static_cast<std::int64_t>(std::llround(seconds * kSecondsToNanoseconds));
  }

  FusionNavigatorConfig declareFusionConfig()
  {
    FusionNavigatorConfig config;
    config.gravity_mps2 = declare_parameter<double>("gravity_mps2", 9.80665);
    config.maximum_propagation_interval_s = declare_parameter<double>(
      "maximum_imu_propagation_interval_s", 0.02);
    config.maximum_acceleration_mps2 = declare_parameter<double>(
      "maximum_acceleration_mps2", 50.0);
    config.accelerometer_noise_mps2_sqrt_hz = declare_parameter<double>(
      "accelerometer_noise_mps2_sqrt_hz", 0.20);
    config.odin_attitude_increment_noise_rad_sqrt_hz = declare_parameter<double>(
      "odin_attitude_increment_noise_rad_sqrt_hz", 0.01);
    config.accelerometer_bias_random_walk_mps3_sqrt_hz = declare_parameter<double>(
      "accelerometer_bias_random_walk_mps3_sqrt_hz", 0.01);
    config.initial_position_std_m = declare_parameter<double>("initial_position_std_m", 0.10);
    config.initial_velocity_std_mps = declare_parameter<double>(
      "initial_velocity_std_mps", 1.0);
    config.initial_attitude_std_rad = degreesToRadians(declare_parameter<double>(
      "initial_attitude_std_deg", 20.0));
    config.initial_accelerometer_bias_std_mps2 = declare_parameter<double>(
      "initial_accelerometer_bias_std_mps2", 0.30);
    config.maximum_translation_position_std_m = declare_parameter<double>(
      "maximum_translation_position_std_m", 1.0);
    config.maximum_inertial_only_duration_s = declare_parameter<double>(
      "maximum_inertial_only_duration_s", 2.0);
    config.iterated_update_max_iterations = declare_parameter<int>(
      "iterated_update_max_iterations", 8);
    config.iterated_update_position_tolerance_m = declare_parameter<double>(
      "iterated_update_position_tolerance_m", 1.0e-4);
    config.iterated_update_attitude_tolerance_rad = declare_parameter<double>(
      "iterated_update_attitude_tolerance_rad", 1.0e-5);
    config.large_attitude_correction_rad = degreesToRadians(declare_parameter<double>(
      "large_attitude_correction_deg", 5.0));
    return config;
  }

  HeadingRigidAlignmentOptions declareHeadingOptions()
  {
    HeadingRigidAlignmentOptions options;
    options.sample_spacing_m = declare_parameter<double>("heading_fit_sample_spacing_m", 2.0);
    options.max_samples = static_cast<std::size_t>(
      declare_parameter<int>("heading_fit_max_samples", 200));
    options.min_samples = static_cast<std::size_t>(
      declare_parameter<int>("heading_fit_min_samples", 5));
    options.min_baseline_m = declare_parameter<double>("heading_fit_min_baseline_m", 10.0);
    options.valid_baseline_m = declare_parameter<double>("heading_fit_valid_baseline_m", 30.0);
    options.target_baseline_m = declare_parameter<double>("heading_fit_target_baseline_m", 100.0);
    options.baseline_ratio_min = declare_parameter<double>("heading_baseline_ratio_min", 0.5);
    options.baseline_ratio_max = declare_parameter<double>("heading_baseline_ratio_max", 1.5);
    options.max_rmse_m = declare_parameter<double>("heading_fit_max_rmse_m", 3.0);
    options.max_p95_residual_m = declare_parameter<double>(
      "heading_fit_max_p95_residual_m", 5.0);
    options.outlier_rejection_enabled = declare_parameter<bool>(
      "heading_fit_outlier_rejection_enabled", true);
    options.outlier_min_threshold_m = declare_parameter<double>(
      "heading_fit_outlier_min_threshold_m", 3.0);
    options.outlier_mad_multiplier = declare_parameter<double>(
      "heading_fit_outlier_mad_multiplier", 3.0);
    options.min_inlier_ratio = declare_parameter<double>("heading_fit_min_inlier_ratio", 0.7);
    options.filter_alpha = 1.0;
    options.max_update_jump_rad = 3.14159265358979323846;
    return options;
  }

  LidarLocalizerConfig declareLidarConfig()
  {
    LidarLocalizerConfig config;
    config.enabled = declare_parameter<bool>("lidar.enabled", true);
    config.voxel_size_m = declare_parameter<double>("lidar.voxel_size_m", 0.15);
    config.minimum_scan_points = static_cast<std::size_t>(
      declare_parameter<int>("lidar.minimum_scan_points", 300));
    config.maximum_scan_points = static_cast<std::size_t>(
      declare_parameter<int>("lidar.maximum_scan_points", 8000));
    config.minimum_map_points = static_cast<std::size_t>(
      declare_parameter<int>("lidar.minimum_map_points", 500));
    config.maximum_map_points = static_cast<std::size_t>(
      declare_parameter<int>("lidar.maximum_map_points", 80000));
    config.map_radius_m = declare_parameter<double>("lidar.map_radius_m", 60.0);
    config.maximum_correspondence_distance_m = declare_parameter<double>(
      "lidar.maximum_correspondence_distance_m", 1.5);
    config.maximum_iterations = declare_parameter<int>("lidar.maximum_iterations", 30);
    config.transformation_epsilon = declare_parameter<double>(
      "lidar.transformation_epsilon", 1.0e-4);
    config.fitness_epsilon = declare_parameter<double>("lidar.fitness_epsilon", 1.0e-4);
    config.maximum_fitness_score_m2 = declare_parameter<double>(
      "lidar.maximum_fitness_score_m2", 0.20);
    config.maximum_position_correction_m = declare_parameter<double>(
      "lidar.maximum_position_correction_m", 2.0);
    config.minimum_inlier_ratio = declare_parameter<double>(
      "lidar.minimum_inlier_ratio", 0.50);
    config.maximum_quality_points = static_cast<std::size_t>(
      declare_parameter<int>("lidar.maximum_quality_points", 2500));
    config.normal_neighbor_count = declare_parameter<int>(
      "lidar.normal_neighbor_count", 10);
    config.maximum_surface_variation = declare_parameter<double>(
      "lidar.maximum_surface_variation", 0.20);
    config.minimum_second_eigenvalue_m2 = declare_parameter<double>(
      "lidar.minimum_second_eigenvalue_m2", 0.01);
    config.degeneracy_relative_eigenvalue = declare_parameter<double>(
      "lidar.degeneracy_relative_eigenvalue", 1.0e-4);
    config.degeneracy_absolute_eigenvalue = declare_parameter<double>(
      "lidar.degeneracy_absolute_eigenvalue", 1.0e-5);
    config.minimum_observable_dof = declare_parameter<int>(
      "lidar.minimum_observable_dof", 4);
    config.unobservable_variance = declare_parameter<double>(
      "lidar.unobservable_variance", 1.0e6);
    config.position_noise_floor_m = declare_parameter<double>(
      "lidar.position_noise_floor_m", 0.05);
    config.attitude_noise_floor_deg = declare_parameter<double>(
      "lidar.attitude_noise_floor_deg", 0.20);
    config.large_rotation_threshold_deg = declare_parameter<double>(
      "lidar.large_rotation_threshold_deg", 5.0);
    config.large_rotation_confirmation_frames = declare_parameter<int>(
      "lidar.large_rotation_confirmation_frames", 3);
    config.large_rotation_consistency_deg = declare_parameter<double>(
      "lidar.large_rotation_consistency_deg", 8.0);
    config.large_rotation_minimum_fitness_improvement_ratio = declare_parameter<double>(
      "lidar.large_rotation_minimum_fitness_improvement_ratio", 0.10);
    config.large_rotation_minimum_observable_rotation_dof = declare_parameter<int>(
      "lidar.large_rotation_minimum_observable_rotation_dof", 2);
    config.map_update_interval = declare_parameter<int>("lidar.map_update_interval", 2);
    return config;
  }

  void declareRemainingParameters()
  {
    output_rate_hz_ = declare_parameter<double>("output_rate_hz", 10.0);
    rtk_timeout_s_ = declare_parameter<double>("rtk_timeout_s", 1.0);
    rtk_time_offset_s_ = declare_parameter<double>("rtk_time_offset_s", 0.0);
    minimum_rtk_satellites_ = declare_parameter<int>("minimum_rtk_satellites", 4);
    maximum_rtk_hdop_ = declare_parameter<double>("maximum_rtk_hdop", 5.0);
    minimum_rtk_horizontal_std_m_ = declare_parameter<double>(
      "minimum_rtk_horizontal_std_m", 0.10);
    minimum_rtk_vertical_std_m_ = declare_parameter<double>(
      "minimum_rtk_vertical_std_m", 0.20);
    maximum_rtk_innovation_m_ = declare_parameter<double>("maximum_rtk_innovation_m", 10.0);
    active_update_timeout_s_ = declare_parameter<double>("active_update_timeout_s", 1.0);
    history_duration_s_ = declare_parameter<double>("state_history_duration_s", 10.0);
    history_max_samples_ = static_cast<std::size_t>(
      declare_parameter<int>("state_history_max_samples", 10000));
    heading_projection_min_norm_ = declare_parameter<double>("heading_projection_min_norm", 0.2);
    const auto forward = declare_parameter<std::vector<double>>(
      "vehicle_forward_axis_body", {0.0, 0.0, -1.0});
    if (forward.size() != 3U) {
      throw std::invalid_argument("vehicle_forward_axis_body必须包含3个元素");
    }
    vehicle_forward_axis_body_ = Eigen::Vector3d(forward[0], forward[1], forward[2]);
    if (!vehicle_forward_axis_body_.array().isFinite().all() ||
      vehicle_forward_axis_body_.norm() <= 1.0e-9)
    {
      throw std::invalid_argument("vehicle_forward_axis_body必须是有限非零向量");
    }
    vehicle_forward_axis_body_.normalize();

    odin_orientation_topic_ = declare_parameter<std::string>(
      "odin_orientation_topic", "/capture/odometry/high_rate");
    imu_topic_ = declare_parameter<std::string>("imu_topic", "/capture/imu/data");
    rtk_fix_topic_ = declare_parameter<std::string>("rtk_fix_topic", "/capture/rtk/fix");
    rtk_status_topic_ = declare_parameter<std::string>("rtk_status_topic", "/capture/rtk/status");
    compensated_cloud_topic_ = declare_parameter<std::string>(
      "compensated_cloud_topic", "/capture/lidar/points_compensated_enu");
    fusion_odometry_topic_ = declare_parameter<std::string>(
      "fusion_odometry_topic", "/capture/localization/fusion_odometry");
    localization_odometry_topic_ = declare_parameter<std::string>(
      "localization_odometry_topic", "/capture/localization/odometry");
    localization_fix_topic_ = declare_parameter<std::string>(
      "localization_fix_topic", "/capture/localization/fix");
    localization_status_topic_ = declare_parameter<std::string>(
      "localization_status_topic", "/capture/localization/status");
    diagnostics_topic_ = declare_parameter<std::string>("diagnostics_topic", "/diagnostics");
    local_frame_id_ = declare_parameter<std::string>("local_frame_id", "local_enu");
    global_frame_id_ = declare_parameter<std::string>("global_frame_id", "map");
    child_frame_id_ = declare_parameter<std::string>("child_frame_id", "base_link");
  }

  void validateParameters() const
  {
    if (!(output_rate_hz_ > 0.0) || !(rtk_timeout_s_ > 0.0) ||
      !(history_duration_s_ > 0.0) || history_max_samples_ < 10U ||
      minimum_rtk_satellites_ < 0 || !(maximum_rtk_hdop_ > 0.0) ||
      !(maximum_rtk_innovation_m_ > 0.0))
    {
      throw std::invalid_argument("组合导航频率、RTK质量或历史缓存参数无效");
    }
  }

  void onOdinOrientation(const nav_msgs::msg::Odometry::ConstSharedPtr message)
  {
    const std::int64_t stamp_ns = toNanoseconds(message->header.stamp);
    Eigen::Quaterniond orientation = rosQuaternion(message->pose.pose.orientation);
    if (stamp_ns <= 0 || !finiteQuaternion(orientation)) {
      RCLCPP_WARN_THROTTLE(
        get_logger(), *get_clock(), 5000, "丢弃无效ODIN四元数或时间戳");
      return;
    }
    orientation.normalize();
    const auto result = synchronizer_.addOrientation(TimedOrientation{stamp_ns, orientation});
    latest_orientation_receipt_ns_.store(steadyNowNanoseconds(), std::memory_order_relaxed);
    latest_orientation_source_stamp_ns_.store(stamp_ns, std::memory_order_relaxed);
    if (result == SensorSynchronizer::AddResult::kEpochReset) {
      std::lock_guard<std::mutex> lock(state_mutex_);
      navigator_.rebaseTimeAndOrientation(stamp_ns, orientation);
      state_history_.clear();
      RCLCPP_WARN(
        get_logger(), "ODIN时间纪元切换：保留local p/v/bias和LiDAR地图，仅重建设备时间缓存");
    }
    drainSynchronizedSamples();
  }

  void onImu(const sensor_msgs::msg::Imu::ConstSharedPtr message)
  {
    const std::int64_t stamp_ns = toNanoseconds(message->header.stamp);
    const Eigen::Vector3d acceleration(
      message->linear_acceleration.x, message->linear_acceleration.y,
      message->linear_acceleration.z);
    const auto result = synchronizer_.addImu(TimedImuAcceleration{stamp_ns, acceleration});
    latest_imu_receipt_ns_.store(steadyNowNanoseconds(), std::memory_order_relaxed);
    if (result == SensorSynchronizer::AddResult::kEpochReset) {
      RCLCPP_WARN(get_logger(), "IMU时间纪元切换：等待新纪元ODIN四元数后继续传播");
    }
    drainSynchronizedSamples();
  }

  void drainSynchronizedSamples()
  {
    SynchronizedImuSample sample;
    while (synchronizer_.popSynchronized(sample)) {
      nav_msgs::msg::Odometry odometry;
      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        navigator_.propagate(
          sample.stamp_ns, sample.specific_force_body_mps2,
          sample.orientation_odin_from_body);
        if (!navigator_.state().local_navigation_valid ||
          navigator_.state().stamp_ns != sample.stamp_ns)
        {
          continue;
        }
        recordCurrentStateLocked();
        odometry = makeOdometryMessageLocked(navigator_.state(), sample.stamp_ns, true);
      }
      fusion_odometry_publisher_->publish(odometry);
    }
    pending_imu_count_.store(synchronizer_.pendingImuCount(), std::memory_order_relaxed);
    dropped_imu_count_.store(synchronizer_.droppedImuCount(), std::memory_order_relaxed);
  }

  void recordCurrentStateLocked()
  {
    const auto & state = navigator_.state();
    HistoryState sample{
      state.stamp_ns, state.position_m, state.velocity_mps,
      state.orientation_local_from_body};
    if (!state_history_.empty() && sample.stamp_ns == state_history_.back().stamp_ns) {
      state_history_.back() = sample;
    } else if (state_history_.empty() || sample.stamp_ns > state_history_.back().stamp_ns) {
      state_history_.push_back(sample);
    }
    const std::int64_t duration_ns = secondsToNs(history_duration_s_);
    while (state_history_.size() > 2U &&
      (state_history_.size() > history_max_samples_ ||
      state_history_.back().stamp_ns - state_history_.front().stamp_ns > duration_ns))
    {
      state_history_.pop_front();
    }
  }

  bool interpolateHistoryLocked(const std::int64_t stamp_ns, HistoryState & output) const
  {
    if (state_history_.empty() || stamp_ns < state_history_.front().stamp_ns ||
      stamp_ns > state_history_.back().stamp_ns)
    {
      return false;
    }
    const auto upper = std::lower_bound(
      state_history_.begin(), state_history_.end(), stamp_ns,
      [](const HistoryState & sample, const std::int64_t target) {
        return sample.stamp_ns < target;
      });
    if (upper != state_history_.end() && upper->stamp_ns == stamp_ns) {
      output = *upper;
      return true;
    }
    if (upper == state_history_.begin() || upper == state_history_.end()) {
      return false;
    }
    const auto lower = std::prev(upper);
    const std::int64_t gap_ns = upper->stamp_ns - lower->stamp_ns;
    if (gap_ns <= 0 || gap_ns > secondsToNs(0.02)) {
      return false;
    }
    const double fraction = static_cast<double>(stamp_ns - lower->stamp_ns) /
      static_cast<double>(gap_ns);
    output.stamp_ns = stamp_ns;
    output.position_m = lower->position_m + fraction * (upper->position_m - lower->position_m);
    output.velocity_mps = lower->velocity_mps + fraction * (upper->velocity_mps - lower->velocity_mps);
    output.orientation_local_from_body = lower->orientation_local_from_body.slerp(
      fraction, upper->orientation_local_from_body).normalized();
    return true;
  }

  void onRtkFix(const sensor_msgs::msg::NavSatFix::ConstSharedPtr message)
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    latest_fix_.available = true;
    latest_fix_.receipt_ns = steadyNowNanoseconds();
    latest_fix_.status = message->status.status;
    latest_fix_.llh = Llh{message->latitude, message->longitude, message->altitude};
    tryProcessRtkLocked();
  }

  void onRtkStatus(const interfaces::msg::RtkStatus::ConstSharedPtr message)
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    latest_rtk_status_.available = true;
    latest_rtk_status_.receipt_ns = steadyNowNanoseconds();
    latest_rtk_status_.rmc_validity = message->rmc_validity;
    latest_rtk_status_.gps_state = message->gps_state;
    latest_rtk_status_.satellite_count = message->satellite_count;
    latest_rtk_status_.hdop = message->hdop;
    latest_rtk_status_.latitude_sigma_m = message->latitude_sigma;
    latest_rtk_status_.longitude_sigma_m = message->longitude_sigma;
    latest_rtk_status_.height_sigma_m = message->height_sigma;
    tryProcessRtkLocked();
  }

  bool rtkReliableLocked(const std::int64_t now_ns) const noexcept
  {
    return latest_fix_.available && latest_rtk_status_.available &&
      latest_fix_.status >= sensor_msgs::msg::NavSatStatus::STATUS_FIX &&
      isFinite(latest_fix_.llh) &&
      ageSeconds(now_ns, latest_fix_.receipt_ns) <= rtk_timeout_s_ &&
      ageSeconds(now_ns, latest_rtk_status_.receipt_ns) <= rtk_timeout_s_ &&
      latest_rtk_status_.gps_state > 0U && isRmcValid(latest_rtk_status_.rmc_validity) &&
      static_cast<int>(latest_rtk_status_.satellite_count) >= minimum_rtk_satellites_ &&
      (!std::isfinite(latest_rtk_status_.hdop) || latest_rtk_status_.hdop <= maximum_rtk_hdop_);
  }

  void tryProcessRtkLocked()
  {
    const std::int64_t now_ns = steadyNowNanoseconds();
    if (!rtkReliableLocked(now_ns) ||
      latest_fix_.receipt_ns <= last_processed_rtk_receipt_ns_ || !navigator_.initialized())
    {
      return;
    }
    if (!rtk_origin_llh_.has_value()) {
      rtk_origin_llh_ = latest_fix_.llh;
    }
    const Enu rtk_enu_value = llhToEnu(*rtk_origin_llh_, latest_fix_.llh);
    if (!isFinite(rtk_enu_value)) {
      return;
    }
    const auto sync_stamp = mapReceiptTimeToSensorTimeNs(
      latest_fix_.receipt_ns, latest_orientation_receipt_ns_.load(std::memory_order_relaxed),
      latest_orientation_source_stamp_ns_.load(std::memory_order_relaxed), rtk_time_offset_s_);
    if (!sync_stamp.has_value()) {
      heading_reason_ = "INVALID_RTK_SYNC_TIME";
      return;
    }
    HistoryState local_at_rtk;
    if (!interpolateHistoryLocked(*sync_stamp, local_at_rtk)) {
      heading_reason_ = "FUSION_HISTORY_UNAVAILABLE";
      return;
    }
    const Eigen::Vector3d rtk_enu(
      rtk_enu_value.east_m, rtk_enu_value.north_m, rtk_enu_value.up_m);

    if (!heading_alignment_valid_) {
      heading_estimator_.addSample(
        HeadingFitSample{
          latest_fix_.receipt_ns, local_at_rtk.position_m.x(), local_at_rtk.position_m.y(),
          rtk_enu.x(), rtk_enu.y()});
      const auto & fit = heading_estimator_.state();
      heading_reason_ = fit.invalid_reason;
      if (fit.update_accepted &&
        fit.baseline_odin_m >= heading_options_.min_baseline_m)
      {
        // 临时对齐只用于把RTK位置残差送入12维误差状态滤波，不对外宣称absolute heading有效。
        alignment_estimate_available_ = true;
        global_yaw_from_local_rad_ = fit.delta_yaw_rad;
        global_translation_xy_m_ = Eigen::Vector2d(
          fit.translation_east_m, fit.translation_north_m);
        global_translation_up_m_ = rtk_enu.z() - local_at_rtk.position_m.z();
      }
      if (fit.valid && fit.update_accepted) {
        heading_alignment_valid_ = true;
        global_yaw_from_local_rad_ = fit.delta_yaw_rad;
        global_translation_xy_m_ = Eigen::Vector2d(
          fit.translation_east_m, fit.translation_north_m);
        global_translation_up_m_ = rtk_enu.z() - local_at_rtk.position_m.z();
        heading_reason_ = "NONE";
        RCLCPP_INFO(
          get_logger(),
          "local->ENU全局对齐有效：baseline=%.2fm yaw=%.3fdeg rmse=%.3fm samples=%zu",
          fit.baseline_odin_m, radiansToDegrees(fit.delta_yaw_rad), fit.rmse_m,
          fit.input_count);
      }
    }

    latest_rtk_innovation_m_ = 0.0;
    if (alignment_estimate_available_) {
      const Eigen::Vector3d observed_local = globalToLocalPosition(rtk_enu);
      const Eigen::Vector3d residual_at_measurement = observed_local - local_at_rtk.position_m;
      latest_rtk_innovation_m_ = residual_at_measurement.norm();
      if (std::isfinite(latest_rtk_innovation_m_) &&
        latest_rtk_innovation_m_ <= maximum_rtk_innovation_m_)
      {
        const Eigen::Vector3d observed_current =
          navigator_.state().position_m + residual_at_measurement;
        Eigen::Matrix3d covariance = Eigen::Matrix3d::Zero();
        const double east_std = validSigma(latest_rtk_status_.longitude_sigma_m) ?
          std::max(minimum_rtk_horizontal_std_m_, latest_rtk_status_.longitude_sigma_m) :
          minimum_rtk_horizontal_std_m_;
        const double north_std = validSigma(latest_rtk_status_.latitude_sigma_m) ?
          std::max(minimum_rtk_horizontal_std_m_, latest_rtk_status_.latitude_sigma_m) :
          minimum_rtk_horizontal_std_m_;
        const double up_std = validSigma(latest_rtk_status_.height_sigma_m) ?
          std::max(minimum_rtk_vertical_std_m_, latest_rtk_status_.height_sigma_m) :
          minimum_rtk_vertical_std_m_;
        covariance.diagonal() << east_std * east_std, north_std * north_std, up_std * up_std;
        const Eigen::Vector3d position_before = navigator_.state().position_m;
        const Eigen::Quaterniond orientation_before =
          navigator_.state().orientation_local_from_body;
        const bool correction_accepted = navigator_.correctPosition(
          navigator_.state().stamp_ns, observed_current, covariance, "RTK_POSITION");
        if (correction_accepted) {
          std::lock_guard<std::mutex> map_lock(lidar_map_mutex_);
          lidar_localizer_.applyReferenceFrameCorrection(
            position_before, orientation_before, navigator_.state().position_m,
            navigator_.state().orientation_local_from_body);
          last_rtk_update_receipt_ns_ = now_ns;
          recordCurrentStateLocked();
        }
      } else {
        RCLCPP_WARN_THROTTLE(
          get_logger(), *get_clock(), 5000,
          "RTK位置创新%.3fm超过门限%.3fm，本次松组合更新跳过",
          latest_rtk_innovation_m_, maximum_rtk_innovation_m_);
      }
    }
    last_processed_rtk_receipt_ns_ = latest_fix_.receipt_ns;
  }

  static bool validSigma(const double sigma) noexcept
  {
    return std::isfinite(sigma) && sigma > 0.0;
  }

  Eigen::Vector3d globalToLocalPosition(const Eigen::Vector3d & global) const noexcept
  {
    const double cosine = std::cos(global_yaw_from_local_rad_);
    const double sine = std::sin(global_yaw_from_local_rad_);
    const Eigen::Vector2d centered = global.head<2>() - global_translation_xy_m_;
    return Eigen::Vector3d(
      cosine * centered.x() + sine * centered.y(),
      -sine * centered.x() + cosine * centered.y(),
      global.z() - global_translation_up_m_);
  }

  Eigen::Vector3d localToGlobalPosition(const Eigen::Vector3d & local) const noexcept
  {
    const double cosine = std::cos(global_yaw_from_local_rad_);
    const double sine = std::sin(global_yaw_from_local_rad_);
    return Eigen::Vector3d(
      global_translation_xy_m_.x() + cosine * local.x() - sine * local.y(),
      global_translation_xy_m_.y() + sine * local.x() + cosine * local.y(),
      global_translation_up_m_ + local.z());
  }

  Eigen::Vector3d localToGlobalVector(const Eigen::Vector3d & local) const noexcept
  {
    const double cosine = std::cos(global_yaw_from_local_rad_);
    const double sine = std::sin(global_yaw_from_local_rad_);
    return Eigen::Vector3d(
      cosine * local.x() - sine * local.y(),
      sine * local.x() + cosine * local.y(), local.z());
  }

  Eigen::Quaterniond localToGlobalOrientation(const Eigen::Quaterniond & local) const noexcept
  {
    return (Eigen::AngleAxisd(global_yaw_from_local_rad_, Eigen::Vector3d::UnitZ()) * local)
      .normalized();
  }

  void onCompensatedCloud(const sensor_msgs::msg::PointCloud2::ConstSharedPtr message)
  {
    if (message->is_bigendian || !hasFloat32Field(*message, "x") ||
      !hasFloat32Field(*message, "y") || !hasFloat32Field(*message, "z"))
    {
      return;
    }
    std::vector<Eigen::Vector3f> points;
    points.reserve(static_cast<std::size_t>(message->width) * message->height);
    try {
      sensor_msgs::PointCloud2ConstIterator<float> x(*message, "x");
      sensor_msgs::PointCloud2ConstIterator<float> y(*message, "y");
      sensor_msgs::PointCloud2ConstIterator<float> z(*message, "z");
      for (; x != x.end(); ++x, ++y, ++z) {
        points.emplace_back(*x, *y, *z);
      }
    } catch (const std::runtime_error &) {
      return;
    }

    const std::int64_t cloud_stamp_ns = toNanoseconds(message->header.stamp);
    HistoryState predicted;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (!interpolateHistoryLocked(cloud_stamp_ns, predicted)) {
        latest_lidar_reason_ = "FUSION_HISTORY_UNAVAILABLE";
        return;
      }
    }

    LidarLocalizationResult result;
    {
      std::lock_guard<std::mutex> map_lock(lidar_map_mutex_);
      result = lidar_localizer_.process(
        points, predicted.position_m, predicted.orientation_local_from_body);
    }
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      latest_lidar_reason_ = result.reason;
      latest_lidar_initial_fitness_m2_ = result.initial_fitness_score_m2;
      latest_lidar_fitness_m2_ = result.fitness_score_m2;
      latest_lidar_inlier_ratio_ = result.inlier_ratio;
      latest_lidar_observable_dof_ = result.observable_dof;
      latest_lidar_observable_rotation_dof_ = result.observable_rotation_dof;
      latest_lidar_large_rotation_pending_ = result.large_rotation_pending;
      latest_lidar_large_rotation_confirmation_count_ =
        result.large_rotation_confirmation_count;
      latest_lidar_map_points_ = result.map_point_count;
      if (result.update_valid && navigator_.initialized()) {
        const Eigen::Vector3d residual_at_cloud =
          result.observed_position_local_m - predicted.position_m;
        const Eigen::Vector3d observed_current =
          navigator_.state().position_m + residual_at_cloud;
        const Eigen::Quaterniond attitude_correction =
          (result.observed_orientation_local_from_body *
          predicted.orientation_local_from_body.conjugate()).normalized();
        const Eigen::Quaterniond observed_current_orientation =
          (attitude_correction * navigator_.state().orientation_local_from_body).normalized();
        const bool correction_accepted = navigator_.correctPose(
          navigator_.state().stamp_ns, observed_current,
          observed_current_orientation, result.observation_covariance,
          "LIDAR_LOCAL_MAP_POSE");
        if (correction_accepted) {
          last_lidar_update_receipt_ns_ = steadyNowNanoseconds();
          recordCurrentStateLocked();
        }
      }
    }
  }

  nav_msgs::msg::Odometry makeOdometryMessageLocked(
    const FusionState & state, const std::int64_t stamp_ns,
    const bool high_rate) const
  {
    nav_msgs::msg::Odometry message;
    message.header.stamp = fromNanoseconds(stamp_ns);
    message.header.frame_id = local_frame_id_;
    message.child_frame_id = child_frame_id_;
    message.pose.pose.position.x = state.position_m.x();
    message.pose.pose.position.y = state.position_m.y();
    message.pose.pose.position.z = state.position_m.z();
    message.pose.pose.orientation.x = state.orientation_local_from_body.x();
    message.pose.pose.orientation.y = state.orientation_local_from_body.y();
    message.pose.pose.orientation.z = state.orientation_local_from_body.z();
    message.pose.pose.orientation.w = state.orientation_local_from_body.w();
    message.twist.twist.linear.x = state.velocity_mps.x();
    message.twist.twist.linear.y = state.velocity_mps.y();
    message.twist.twist.linear.z = state.velocity_mps.z();
    message.pose.covariance[0] = state.covariance(0, 0);
    message.pose.covariance[7] = state.covariance(1, 1);
    message.pose.covariance[14] = state.covariance(2, 2);
    message.twist.covariance[0] = state.covariance(3, 3);
    message.twist.covariance[7] = state.covariance(4, 4);
    message.twist.covariance[14] = state.covariance(5, 5);
    message.pose.covariance[21] = state.covariance(6, 6);
    message.pose.covariance[28] = state.covariance(7, 7);
    message.pose.covariance[35] = state.covariance(8, 8);
    // 负协方差是本项目内部明确的“仅姿态可用”标记；正式位置仍保留作诊断。
    if (high_rate && !state.translation_quality_valid) {
      message.pose.covariance[0] = -1.0;
      message.pose.covariance[7] = -1.0;
      message.pose.covariance[14] = -1.0;
    }
    return message;
  }

  void publishOutputs()
  {
    sensor_msgs::msg::NavSatFix fix;
    nav_msgs::msg::Odometry odometry;
    interfaces::msg::LocalizationStatus status;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      const std::int64_t now_steady_ns = steadyNowNanoseconds();
      const std::int64_t ros_now_ns = now().nanoseconds();
      if (!navigator_.initialized()) {
        status.header.stamp = fromNanoseconds(ros_now_ns);
        status.header.frame_id = local_frame_id_;
        status.valid = false;
        status.fusion_mode = interfaces::msg::LocalizationStatus::FUSION_MODE_INVALID;
        status.invalid_reason = "LOCAL_NAVIGATION_NOT_INITIALIZED";
        status.fusion_quality_reason = "LOCAL_NAVIGATION_NOT_INITIALIZED";
        status_publisher_->publish(status);
        return;
      }
      const FusionState & state = navigator_.state();
      odometry = makeOdometryMessageLocked(state, ros_now_ns, false);
      odometry.header.stamp = fromNanoseconds(ros_now_ns);
      fillStatusLocked(status, state, now_steady_ns, ros_now_ns);
      fillFixLocked(fix, state, now_steady_ns, ros_now_ns, status.global_position_valid);
    }
    odometry_publisher_->publish(odometry);
    fix_publisher_->publish(fix);
    status_publisher_->publish(status);
  }

  void fillFixLocked(
    sensor_msgs::msg::NavSatFix & fix, const FusionState & state,
    const std::int64_t now_steady_ns, const std::int64_t ros_now_ns,
    const bool global_position_valid) const
  {
    fix.header.stamp = fromNanoseconds(ros_now_ns);
    fix.header.frame_id = "wgs84";
    fix.status.service = sensor_msgs::msg::NavSatStatus::SERVICE_GPS;
    fix.status.status = global_position_valid ? sensor_msgs::msg::NavSatStatus::STATUS_FIX :
      sensor_msgs::msg::NavSatStatus::STATUS_NO_FIX;
    if (global_position_valid && heading_alignment_valid_ && rtk_origin_llh_.has_value()) {
      const Eigen::Vector3d global = localToGlobalPosition(state.position_m);
      const Llh llh = enuToLlh(*rtk_origin_llh_, Enu{global.x(), global.y(), global.z()});
      if (isFinite(llh)) {
        fix.latitude = llh.latitude_deg;
        fix.longitude = llh.longitude_deg;
        fix.altitude = llh.altitude_m;
      }
    } else if (rtkReliableLocked(now_steady_ns)) {
      fix.latitude = latest_fix_.llh.latitude_deg;
      fix.longitude = latest_fix_.llh.longitude_deg;
      fix.altitude = latest_fix_.llh.altitude_m;
    }
    fix.position_covariance_type = sensor_msgs::msg::NavSatFix::COVARIANCE_TYPE_UNKNOWN;
  }

  void fillStatusLocked(
    interfaces::msg::LocalizationStatus & message, const FusionState & state,
    const std::int64_t now_steady_ns, const std::int64_t ros_now_ns) const
  {
    const bool rtk_active = ageSeconds(now_steady_ns, last_rtk_update_receipt_ns_) <=
      active_update_timeout_s_;
    const bool lidar_active = ageSeconds(now_steady_ns, last_lidar_update_receipt_ns_) <=
      active_update_timeout_s_;
    const bool raw_rtk_valid = rtkReliableLocked(now_steady_ns);
    const bool global_position_valid = heading_alignment_valid_ ?
      (state.local_navigation_valid && (state.translation_quality_valid || raw_rtk_valid)) :
      raw_rtk_valid;

    message.header.stamp = fromNanoseconds(ros_now_ns);
    message.header.frame_id = local_frame_id_;
    message.local_navigation_valid = state.local_navigation_valid;
    message.global_position_valid = global_position_valid;
    message.heading_alignment_valid = heading_alignment_valid_;
    message.valid = global_position_valid;
    message.rtk_update_valid = rtk_active;
    message.lidar_update_valid = lidar_active;
    message.translation_compensation_valid = state.translation_quality_valid;
    if (!state.local_navigation_valid) {
      message.fusion_mode = interfaces::msg::LocalizationStatus::FUSION_MODE_INVALID;
    } else if (raw_rtk_valid && !heading_alignment_valid_) {
      message.fusion_mode = interfaces::msg::LocalizationStatus::FUSION_MODE_GLOBAL_ALIGNING;
    } else if (rtk_active && lidar_active) {
      message.fusion_mode = interfaces::msg::LocalizationStatus::FUSION_MODE_GLOBAL_RTK_LIO;
    } else if (rtk_active) {
      message.fusion_mode = interfaces::msg::LocalizationStatus::FUSION_MODE_GLOBAL_RTK_INS;
    } else if (lidar_active) {
      message.fusion_mode = interfaces::msg::LocalizationStatus::FUSION_MODE_LOCAL_LIO;
    } else {
      message.fusion_mode = interfaces::msg::LocalizationStatus::FUSION_MODE_LOCAL_INS;
    }
    message.mode = global_position_valid ?
      ((raw_rtk_valid || rtk_active) ? interfaces::msg::LocalizationStatus::MODE_RTK :
      interfaces::msg::LocalizationStatus::MODE_DEAD_RECKONING) :
      interfaces::msg::LocalizationStatus::MODE_INVALID;
    message.heading_source = heading_alignment_valid_ ?
      interfaces::msg::LocalizationStatus::HEADING_FUSION :
      interfaces::msg::LocalizationStatus::HEADING_INVALID;

    message.local_position_east_m = state.position_m.x();
    message.local_position_north_m = state.position_m.y();
    message.local_position_up_m = state.position_m.z();
    message.local_velocity_east_mps = state.velocity_mps.x();
    message.local_velocity_north_mps = state.velocity_mps.y();
    message.local_velocity_up_mps = state.velocity_mps.z();
    message.accelerometer_bias_x_mps2 = state.accelerometer_bias_mps2.x();
    message.accelerometer_bias_y_mps2 = state.accelerometer_bias_mps2.y();
    message.accelerometer_bias_z_mps2 = state.accelerometer_bias_mps2.z();
    message.position_std_m = std::sqrt(std::max(
      0.0, state.covariance.block<3, 3>(0, 0).diagonal().maxCoeff()));
    message.velocity_std_mps = std::sqrt(std::max(
      0.0, state.covariance.block<3, 3>(3, 3).diagonal().maxCoeff()));
    message.attitude_std_deg = radiansToDegrees(std::sqrt(std::max(
      0.0, state.covariance.block<3, 3>(6, 6).diagonal().maxCoeff())));
    message.inertial_only_duration_s = ageSeconds(
      state.stamp_ns, navigator_.lastExternalCorrectionStampNs());
    message.lidar_fitness_score_m2 = latest_lidar_fitness_m2_;
    message.lidar_initial_fitness_score_m2 = finiteOrZero(latest_lidar_initial_fitness_m2_);
    message.lidar_inlier_ratio = latest_lidar_inlier_ratio_;
    message.lidar_observable_dof = static_cast<std::uint8_t>(std::clamp(
      latest_lidar_observable_dof_, 0, 6));
    message.lidar_observable_rotation_dof = static_cast<std::uint8_t>(std::clamp(
      latest_lidar_observable_rotation_dof_, 0, 3));
    message.lidar_large_rotation_pending = latest_lidar_large_rotation_pending_;
    message.lidar_large_rotation_confirmation_count = static_cast<std::uint8_t>(std::clamp(
      latest_lidar_large_rotation_confirmation_count_, 0, 255));
    message.lidar_map_point_count = static_cast<std::uint32_t>(latest_lidar_map_points_);
    message.last_attitude_correction_deg = radiansToDegrees(
      navigator_.lastAttitudeCorrectionRad().norm());
    message.last_fusion_update_iteration_count = static_cast<std::uint8_t>(std::clamp(
      navigator_.lastUpdateIterationCount(), 0, 255));
    message.fusion_quality_reason = state.quality_reason;
    message.invalid_reason = global_position_valid ? "NONE" : "GLOBAL_ALIGNMENT_UNAVAILABLE";

    message.delta_yaw_deg = heading_alignment_valid_ ?
      radiansToDegrees(global_yaw_from_local_rad_) : 0.0;
    message.heading_alignment_reason = heading_reason_;
    const auto & fit = heading_estimator_.state();
    message.heading_fit_sample_count = static_cast<std::uint32_t>(fit.input_count);
    message.heading_fit_baseline_m = fit.baseline_odin_m;
    message.heading_fit_rmse_m = finiteOrZero(fit.rmse_m);
    message.heading_fit_p95_residual_m = finiteOrZero(fit.residual_p95_m);
    message.heading_fit_inlier_ratio = finiteOrZero(fit.inlier_ratio);
    message.heading_fit_delta_yaw_deg = finiteOrZero(radiansToDegrees(fit.delta_yaw_rad));
    message.heading_fit_valid = fit.valid;
    message.heading_fit_window_span_m = fit.window_span_m;
    message.heading_error_before_deg = finiteOrZero(radiansToDegrees(fit.heading_error_before_rad));
    message.heading_error_after_deg = finiteOrZero(radiansToDegrees(fit.heading_error_after_rad));
    message.heading_baseline_m = fit.baseline_odin_m;
    message.scale_calibration_mode = 0U;
    message.scale_status = interfaces::msg::LocalizationStatus::SCALE_DISABLED;
    message.horizontal_scale = 1.0;
    message.vertical_scale = 1.0;
    message.rtk_age_s = latest_fix_.available ?
      ageSeconds(now_steady_ns, latest_fix_.receipt_ns) : -1.0;
    const std::int64_t orientation_receipt_ns =
      latest_orientation_receipt_ns_.load(std::memory_order_relaxed);
    const std::int64_t imu_receipt_ns = latest_imu_receipt_ns_.load(std::memory_order_relaxed);
    message.odometry_age_s = orientation_receipt_ns > 0 ?
      ageSeconds(now_steady_ns, orientation_receipt_ns) : -1.0;
    message.imu_age_s = imu_receipt_ns > 0 ?
      ageSeconds(now_steady_ns, imu_receipt_ns) : -1.0;
    message.position_difference_to_rtk_m = latest_rtk_innovation_m_;

    if (global_position_valid && rtk_origin_llh_.has_value()) {
      Llh llh;
      if (heading_alignment_valid_) {
        const Eigen::Vector3d global = localToGlobalPosition(state.position_m);
        llh = enuToLlh(*rtk_origin_llh_, Enu{global.x(), global.y(), global.z()});
      } else {
        llh = latest_fix_.llh;
      }
      if (isFinite(llh)) {
        message.latitude = llh.latitude_deg;
        message.longitude = llh.longitude_deg;
        message.altitude = llh.altitude_m;
      }
    }

    Eigen::Quaterniond attitude = state.orientation_local_from_body;
    if (heading_alignment_valid_) {
      attitude = localToGlobalOrientation(attitude);
      const Eigen::Vector3d forward_global = attitude * vehicle_forward_axis_body_;
      const double horizontal_norm = std::hypot(forward_global.x(), forward_global.y());
      if (horizontal_norm >= heading_projection_min_norm_) {
        const double yaw = std::atan2(forward_global.y(), forward_global.x());
        message.heading_deg = enuYawRadToClockwiseCourseDegrees(yaw);
        message.vehicle_heading_deg = message.heading_deg;
      }
    }
    const Quaterniond display_quaternion{
      attitude.x(), attitude.y(), attitude.z(), attitude.w()};
    VehicleAttitude vehicle_attitude{};
    if (vehicleAttitudeFromOdinQuaternion(
        display_quaternion.x, display_quaternion.y, display_quaternion.z,
        display_quaternion.w, kDefaultVehicleAttitudeMountRotationBm, vehicle_attitude))
    {
      message.vehicle_attitude_valid = true;
      message.vehicle_pitch_deg = radiansToDegrees(vehicle_attitude.pitch_rad);
      message.vehicle_roll_deg = radiansToDegrees(vehicle_attitude.roll_rad);
      if (!heading_alignment_valid_) {
        message.vehicle_heading_deg = 0.0;
      }
    }
  }

  void publishDiagnostics()
  {
    diagnostic_msgs::msg::DiagnosticArray array;
    array.header.stamp = now();
    diagnostic_msgs::msg::DiagnosticStatus status;
    status.name = "localization/fusion_navigation";
    status.hardware_id = "RK3588";
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (!navigator_.initialized()) {
        status.level = diagnostic_msgs::msg::DiagnosticStatus::WARN;
        status.message = "等待同步ODIN四元数和IMU加速度";
      } else if (!navigator_.state().translation_quality_valid) {
        status.level = diagnostic_msgs::msg::DiagnosticStatus::WARN;
        status.message = navigator_.state().quality_reason;
      } else {
        status.level = diagnostic_msgs::msg::DiagnosticStatus::OK;
        status.message = "连续组合导航正常";
      }
      status.values = {
        diagnosticValue("odin_position_used", "false"),
        diagnosticValue("odin_twist_linear_used", "false"),
        diagnosticValue("local_frame_id", local_frame_id_),
        diagnosticValue("global_frame_id", global_frame_id_),
        diagnosticValue("local_navigation_valid", navigator_.initialized() &&
          navigator_.state().local_navigation_valid ? "true" : "false"),
        diagnosticValue("heading_alignment_valid", heading_alignment_valid_ ? "true" : "false"),
        diagnosticValue("heading_alignment_reason", heading_reason_),
        diagnosticValue("last_correction_source", navigator_.lastCorrectionSource()),
        diagnosticValue(
          "pending_imu_samples",
          std::to_string(pending_imu_count_.load(std::memory_order_relaxed))),
        diagnosticValue(
          "dropped_imu_samples",
          std::to_string(dropped_imu_count_.load(std::memory_order_relaxed))),
        diagnosticValue("rtk_innovation_m", std::to_string(latest_rtk_innovation_m_)),
        diagnosticValue("lidar_reason", latest_lidar_reason_),
        diagnosticValue("lidar_initial_fitness_m2", std::to_string(
          latest_lidar_initial_fitness_m2_)),
        diagnosticValue("lidar_fitness_m2", std::to_string(latest_lidar_fitness_m2_)),
        diagnosticValue("lidar_inlier_ratio", std::to_string(latest_lidar_inlier_ratio_)),
        diagnosticValue("lidar_observable_dof", std::to_string(latest_lidar_observable_dof_)),
        diagnosticValue(
          "lidar_observable_rotation_dof",
          std::to_string(latest_lidar_observable_rotation_dof_)),
        diagnosticValue(
          "lidar_large_rotation_confirmation_count",
          std::to_string(latest_lidar_large_rotation_confirmation_count_)),
        diagnosticValue(
          "last_attitude_correction_deg",
          std::to_string(radiansToDegrees(navigator_.lastAttitudeCorrectionRad().norm()))),
        diagnosticValue(
          "last_update_iterations", std::to_string(navigator_.lastUpdateIterationCount())),
        diagnosticValue("lidar_map_points", std::to_string(latest_lidar_map_points_))};
    }
    array.status.push_back(std::move(status));
    diagnostics_publisher_->publish(array);
  }

  FusionNavigator navigator_;
  SensorSynchronizer synchronizer_;
  HeadingRigidAlignmentOptions heading_options_;
  HeadingRigidAlignmentEstimator heading_estimator_;
  LidarLocalizer lidar_localizer_;

  mutable std::mutex state_mutex_;
  mutable std::mutex lidar_map_mutex_;
  std::deque<HistoryState> state_history_;
  LatestFix latest_fix_;
  LatestRtkStatus latest_rtk_status_;
  std::optional<Llh> rtk_origin_llh_;
  bool heading_alignment_valid_{false};
  bool alignment_estimate_available_{false};
  double global_yaw_from_local_rad_{0.0};
  Eigen::Vector2d global_translation_xy_m_{Eigen::Vector2d::Zero()};
  double global_translation_up_m_{0.0};
  std::string heading_reason_{"INSUFFICIENT_TRAJECTORY"};
  double latest_rtk_innovation_m_{0.0};
  double latest_lidar_initial_fitness_m2_{0.0};
  double latest_lidar_fitness_m2_{0.0};
  double latest_lidar_inlier_ratio_{0.0};
  int latest_lidar_observable_dof_{0};
  int latest_lidar_observable_rotation_dof_{0};
  bool latest_lidar_large_rotation_pending_{false};
  int latest_lidar_large_rotation_confirmation_count_{0};
  std::size_t latest_lidar_map_points_{0U};
  std::string latest_lidar_reason_{"NOT_INITIALIZED"};

  double output_rate_hz_{10.0};
  double rtk_timeout_s_{1.0};
  double rtk_time_offset_s_{0.0};
  int minimum_rtk_satellites_{4};
  double maximum_rtk_hdop_{5.0};
  double minimum_rtk_horizontal_std_m_{0.10};
  double minimum_rtk_vertical_std_m_{0.20};
  double maximum_rtk_innovation_m_{10.0};
  double active_update_timeout_s_{1.0};
  double history_duration_s_{10.0};
  std::size_t history_max_samples_{10000U};
  double heading_projection_min_norm_{0.2};
  Eigen::Vector3d vehicle_forward_axis_body_{0.0, 0.0, -1.0};

  std::atomic<std::int64_t> latest_orientation_receipt_ns_{0};
  std::atomic<std::int64_t> latest_orientation_source_stamp_ns_{0};
  std::atomic<std::int64_t> latest_imu_receipt_ns_{0};
  std::atomic<std::size_t> pending_imu_count_{0U};
  std::atomic<std::uint64_t> dropped_imu_count_{0U};
  std::int64_t last_processed_rtk_receipt_ns_{0};
  std::int64_t last_rtk_update_receipt_ns_{0};
  std::int64_t last_lidar_update_receipt_ns_{0};

  std::string odin_orientation_topic_;
  std::string imu_topic_;
  std::string rtk_fix_topic_;
  std::string rtk_status_topic_;
  std::string compensated_cloud_topic_;
  std::string fusion_odometry_topic_;
  std::string localization_odometry_topic_;
  std::string localization_fix_topic_;
  std::string localization_status_topic_;
  std::string diagnostics_topic_;
  std::string local_frame_id_;
  std::string global_frame_id_;
  std::string child_frame_id_;

  rclcpp::CallbackGroup::SharedPtr high_rate_group_;
  rclcpp::CallbackGroup::SharedPtr rtk_group_;
  rclcpp::CallbackGroup::SharedPtr lidar_group_;
  rclcpp::Subscription<nav_msgs::msg::Odometry>::SharedPtr orientation_subscription_;
  rclcpp::Subscription<sensor_msgs::msg::Imu>::SharedPtr imu_subscription_;
  rclcpp::Subscription<sensor_msgs::msg::NavSatFix>::SharedPtr rtk_fix_subscription_;
  rclcpp::Subscription<interfaces::msg::RtkStatus>::SharedPtr rtk_status_subscription_;
  rclcpp::Subscription<sensor_msgs::msg::PointCloud2>::SharedPtr lidar_subscription_;
  rclcpp::Publisher<nav_msgs::msg::Odometry>::SharedPtr fusion_odometry_publisher_;
  rclcpp::Publisher<nav_msgs::msg::Odometry>::SharedPtr odometry_publisher_;
  rclcpp::Publisher<sensor_msgs::msg::NavSatFix>::SharedPtr fix_publisher_;
  rclcpp::Publisher<interfaces::msg::LocalizationStatus>::SharedPtr status_publisher_;
  rclcpp::Publisher<diagnostic_msgs::msg::DiagnosticArray>::SharedPtr diagnostics_publisher_;
  rclcpp::TimerBase::SharedPtr output_timer_;
  rclcpp::TimerBase::SharedPtr diagnostics_timer_;
};

}  // namespace localization

int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);
  try {
    auto node = std::make_shared<localization::FusionNavigationNode>();
    rclcpp::executors::MultiThreadedExecutor executor(rclcpp::ExecutorOptions(), 3U);
    executor.add_node(node);
    executor.spin();
  } catch (const std::exception & error) {
    RCLCPP_FATAL(rclcpp::get_logger("fusion_navigation_node"), "%s", error.what());
    rclcpp::shutdown();
    return 1;
  }
  rclcpp::shutdown();
  return 0;
}

#include "localization/dead_reckoning.hpp"
#include "localization/geodesy.hpp"
#include "localization/heading_alignment.hpp"
#include "localization/heading_rigid_alignment.hpp"
#include "localization/odometry_buffer.hpp"
#include "localization/rtk_path_simulation.hpp"
#include "localization/similarity_alignment.hpp"
#include "localization/attitude_transform.hpp"

#include "builtin_interfaces/msg/time.hpp"
#include "interfaces/msg/localization_status.hpp"
#include "interfaces/msg/rtk_status.hpp"
#include "nav_msgs/msg/odometry.hpp"
#include "rclcpp/rclcpp.hpp"
#include "sensor_msgs/msg/imu.hpp"
#include "sensor_msgs/msg/nav_sat_fix.hpp"
#include "sensor_msgs/msg/nav_sat_status.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace localization
{
namespace
{

constexpr double kKnotsToMps = 0.5144444444444445;
constexpr double kNsToSeconds = 1.0e-9;
constexpr double kSecondsToNs = 1.0e9;

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

double ageSeconds(std::int64_t now_ns, std::int64_t stamp_ns) noexcept
{
  if (now_ns <= 0 || stamp_ns <= 0) {
    return std::numeric_limits<double>::infinity();
  }
  return std::max(0.0, static_cast<double>(now_ns - stamp_ns) * kNsToSeconds);
}

double statusAgeSeconds(std::int64_t now_ns, std::int64_t stamp_ns) noexcept
{
  const double age = ageSeconds(now_ns, stamp_ns);
  return std::isfinite(age) ? age : -1.0;
}

double finiteOrZero(const double value) noexcept
{
  return std::isfinite(value) ? value : 0.0;
}

}  // namespace

class DeadReckoningNode final : public rclcpp::Node
{
public:
  DeadReckoningNode()
  : Node("dead_reckoning_node"),
    heading_fit_options_(declareHeadingFitOptions()),
    heading_fit_estimator_(heading_fit_options_),
    simulation_options_(declareSimulationOptions()),
    rtk_path_simulation_(simulation_options_),
    odom_buffer_(
      secondsToNs(declare_parameter<double>("odometry_cache_duration_s", 60.0)),
      secondsToNs(declare_parameter<double>("odometry_interpolation_max_gap_s", 0.02)),
      static_cast<std::size_t>(declare_parameter<int>("odometry_cache_max_samples", 5000)))
  {
    declareRemainingParameters();
    validateParameters();
    if (rtk_path_simulation_.active()) {
      rtk_origin_llh_ = rtk_path_simulation_.pointA();
    }
    scale_status_ = scale_calibration_mode_ == 0 ?
      interfaces::msg::LocalizationStatus::SCALE_DISABLED :
      interfaces::msg::LocalizationStatus::SCALE_COLLECTING;

    const auto reliable_qos = rclcpp::QoS(rclcpp::KeepLast(20)).reliable().durability_volatile();
    rtk_fix_subscription_ = create_subscription<sensor_msgs::msg::NavSatFix>(
      rtk_fix_topic_, reliable_qos,
      [this](sensor_msgs::msg::NavSatFix::SharedPtr message) {onRtkFix(std::move(message));});
    rtk_status_subscription_ = create_subscription<interfaces::msg::RtkStatus>(
      rtk_status_topic_, reliable_qos,
      [this](interfaces::msg::RtkStatus::SharedPtr message) {onRtkStatus(std::move(message));});
    odom_subscription_ = create_subscription<nav_msgs::msg::Odometry>(
      odometry_topic_, rclcpp::QoS(rclcpp::KeepLast(1000)).reliable().durability_volatile(),
      [this](nav_msgs::msg::Odometry::SharedPtr message) {onOdometry(std::move(message));});
    imu_subscription_ = create_subscription<sensor_msgs::msg::Imu>(
      imu_topic_, rclcpp::QoS(rclcpp::KeepLast(100)).best_effort().durability_volatile(),
      [this](sensor_msgs::msg::Imu::SharedPtr message) {onImu(std::move(message));});

    fix_publisher_ = create_publisher<sensor_msgs::msg::NavSatFix>(
      localization_fix_topic_, reliable_qos);
    odometry_publisher_ = create_publisher<nav_msgs::msg::Odometry>(
      localization_odometry_topic_, reliable_qos);
    status_publisher_ = create_publisher<interfaces::msg::LocalizationStatus>(
      localization_status_topic_, reliable_qos);

    const auto period = std::chrono::duration<double>(1.0 / output_rate_hz_);
    output_timer_ = create_wall_timer(
      std::chrono::duration_cast<std::chrono::nanoseconds>(period),
      [this]() {publishOutput();});
    if (rtk_path_simulation_.active()) {
      simulation_timer_ = create_wall_timer(100ms, [this]() {requestSimulatedRtk();});
    }

    RCLCPP_INFO(
      get_logger(),
      "ODIN航位推算节点已启动：fix=%s status=%s odom=%s output=%s",
      rtk_fix_topic_.c_str(), rtk_status_topic_.c_str(), odometry_topic_.c_str(),
      localization_status_topic_.c_str());
    RCLCPP_INFO(
      get_logger(), "vehicle_forward_axis_body = [%.6f, %.6f, %.6f]",
      vehicle_forward_axis_body_.x, vehicle_forward_axis_body_.y,
      vehicle_forward_axis_body_.z);
    RCLCPP_INFO(
      get_logger(),
      "vehicle_attitude_mount_rotation_bm = [%.0f, %.0f, %.0f; %.0f, %.0f, %.0f; %.0f, %.0f, %.0f]",
      vehicle_attitude_mount_rotation_bm_[0], vehicle_attitude_mount_rotation_bm_[1],
      vehicle_attitude_mount_rotation_bm_[2], vehicle_attitude_mount_rotation_bm_[3],
      vehicle_attitude_mount_rotation_bm_[4], vehicle_attitude_mount_rotation_bm_[5],
      vehicle_attitude_mount_rotation_bm_[6], vehicle_attitude_mount_rotation_bm_[7],
      vehicle_attitude_mount_rotation_bm_[8]);
    if (rtk_path_simulation_.active()) {
      RCLCPP_INFO(
        get_logger(),
        "Simulation RTK A=[%.10f, %.10f, %.3f], B=[%.10f, %.10f, %.3f], "
        "A-B horizontal distance=%.3f m, geographic direction=%.3f deg",
        rtk_path_simulation_.pointA().latitude_deg,
        rtk_path_simulation_.pointA().longitude_deg,
        rtk_path_simulation_.pointA().altitude_m,
        rtk_path_simulation_.pointB().latitude_deg,
        rtk_path_simulation_.pointB().longitude_deg,
        rtk_path_simulation_.pointB().altitude_m,
        rtk_path_simulation_.horizontalDistanceM(),
        rtk_path_simulation_.geographicDirectionDeg());
    }
  }

private:
  enum class InternalMode
  {
    kWaitingForRtk,
    kRtkValid,
    kDeadReckoning,
    kRtkRecovery,
  };

  struct LatestFix
  {
    bool available{false};
    std::int64_t stamp_ns{0};
    Llh llh;
    int status{sensor_msgs::msg::NavSatStatus::STATUS_NO_FIX};
  };

  struct LatestRtkStatus
  {
    bool available{false};
    std::int64_t stamp_ns{0};
    std::uint8_t rmc_validity{0U};
    std::uint8_t gps_state{0U};
    double speed_mps{0.0};
    double track_degrees{0.0};
  };

  struct LatestImu
  {
    bool available{false};
    std::int64_t stamp_ns{0};
    Vector3d angular_velocity_rad_s;
  };

  struct AnchorCandidate
  {
    bool available{false};
    std::int64_t stamp_ns{0};
    Llh llh;
    OdomSample odom;
  };

  struct CalibrationPair
  {
    std::int64_t stamp_ns{0};
    Point2d odin_xy;
    Point2d rtk_enu;
  };

  struct RecoveryState
  {
    bool active{false};
    std::int64_t start_ns{0};
    Enu from_dr_enu;
    Enu to_rtk_enu;
    double position_error_m{0.0};
  };

  struct Output
  {
    bool valid{false};
    std::uint8_t mode{interfaces::msg::LocalizationStatus::MODE_INVALID};
    std::uint8_t heading_source{interfaces::msg::LocalizationStatus::HEADING_INVALID};
    Llh llh;
    Enu enu;
    Quaterniond orientation;
    bool heading_valid{false};
    double heading_enu_rad{0.0};
    double distance_from_anchor_m{0.0};
    double dr_duration_s{0.0};
    double position_difference_to_rtk_m{0.0};
    std::string invalid_reason{"NO_VALID_RTK_ANCHOR"};
  };

  static std::int64_t secondsToNs(const double seconds)
  {
    if (!(seconds > 0.0) || !std::isfinite(seconds)) {
      throw std::invalid_argument("时间参数必须为有限正数");
    }
    return static_cast<std::int64_t>(std::llround(seconds * kSecondsToNs));
  }

  HeadingRigidAlignmentOptions declareHeadingFitOptions()
  {
    HeadingRigidAlignmentOptions options;
    options.sample_spacing_m = declare_parameter<double>("heading_fit_sample_spacing_m", 5.0);
    const int max_samples = declare_parameter<int>("heading_fit_max_samples", 100);
    const int min_samples = declare_parameter<int>("heading_fit_min_samples", 3);
    if (min_samples < 3 || max_samples < min_samples || max_samples > 10000) {
      throw std::invalid_argument("heading_fit样本数量参数无效");
    }
    options.max_samples = static_cast<std::size_t>(max_samples);
    options.min_samples = static_cast<std::size_t>(min_samples);
    options.min_baseline_m = declare_parameter<double>("heading_fit_min_baseline_m", 50.0);
    options.valid_baseline_m = declare_parameter<double>("heading_fit_valid_baseline_m", 100.0);
    options.target_baseline_m = declare_parameter<double>("heading_fit_target_baseline_m", 500.0);
    options.baseline_ratio_min = declare_parameter<double>("heading_baseline_ratio_min", 0.7);
    options.baseline_ratio_max = declare_parameter<double>("heading_baseline_ratio_max", 1.3);
    options.max_rmse_m = declare_parameter<double>("heading_fit_max_rmse_m", 15.0);
    options.max_p95_residual_m = declare_parameter<double>(
      "heading_fit_max_p95_residual_m", 25.0);
    options.outlier_rejection_enabled = declare_parameter<bool>(
      "heading_fit_outlier_rejection_enabled", true);
    options.outlier_min_threshold_m = declare_parameter<double>(
      "heading_fit_outlier_min_threshold_m", 20.0);
    options.outlier_mad_multiplier = declare_parameter<double>(
      "heading_fit_outlier_mad_multiplier", 3.0);
    options.min_inlier_ratio = declare_parameter<double>("heading_fit_min_inlier_ratio", 0.7);
    options.filter_alpha = declare_parameter<double>("heading_fit_filter_alpha", 0.1);
    options.max_update_jump_rad = degreesToRadians(
      declare_parameter<double>("heading_fit_max_update_jump_deg", 5.0));
    return options;
  }

  RtkPathSimulationOptions declareSimulationOptions()
  {
    RtkPathSimulationOptions options;
    options.test_mode = declare_parameter<int>("simulation_test_mode", 1);
    const auto point_a = declare_parameter<std::vector<double>>(
      "simulation_rtk_point_a", std::vector<double>{24.5738888889, 118.0894444444, 20.0});
    const auto point_b = declare_parameter<std::vector<double>>(
      "simulation_rtk_point_b", std::vector<double>{24.5741666667, 118.0897222222, 20.0});
    if (point_a.size() != 3U || point_b.size() != 3U) {
      throw std::invalid_argument("simulation_rtk_point_a/B必须各包含纬度、经度、高度3个数");
    }
    options.point_a = Llh{point_a[0], point_a[1], point_a[2]};
    options.point_b = Llh{point_b[0], point_b[1], point_b[2]};
    return options;
  }

  void declareRemainingParameters()
  {
    output_rate_hz_ = declare_parameter<double>("output_rate_hz", 10.0);
    rtk_timeout_s_ = declare_parameter<double>("rtk_timeout_s", 1.0);
    odometry_timeout_s_ = declare_parameter<double>("odometry_timeout_s", 0.1);
    imu_timeout_s_ = declare_parameter<double>("imu_timeout_s", 0.1);
    rtk_time_offset_s_ = declare_parameter<double>("rtk_time_offset_s", 0.0);

    const std::vector<double> forward_axis = declare_parameter<std::vector<double>>(
      "vehicle_forward_axis_body", std::vector<double>{0.0, 0.0, -1.0});
    if (forward_axis.size() != 3U) {
      throw std::invalid_argument("vehicle_forward_axis_body必须包含3个分量");
    }
    vehicle_forward_axis_body_ = Vector3d{forward_axis[0], forward_axis[1], forward_axis[2]};
    const std::vector<double> attitude_mount_rotation = declare_parameter<std::vector<double>>(
      "vehicle_attitude_mount_rotation_bm",
      std::vector<double>(
        kDefaultVehicleAttitudeMountRotationBm.begin(),
        kDefaultVehicleAttitudeMountRotationBm.end()));
    if (attitude_mount_rotation.size() != 9U) {
      throw std::invalid_argument("vehicle_attitude_mount_rotation_bm必须包含9个分量");
    }
    std::copy(
      attitude_mount_rotation.begin(), attitude_mount_rotation.end(),
      vehicle_attitude_mount_rotation_bm_.begin());
    heading_projection_min_norm_ = declare_parameter<double>("heading_projection_min_norm", 0.2);
    forward_axis_motion_validation_enabled_ = declare_parameter<bool>(
      "forward_axis_motion_validation_enabled", true);
    forward_axis_validation_min_speed_mps_ = declare_parameter<double>(
      "forward_axis_validation_min_speed_mps", 5.0);
    forward_axis_validation_min_distance_m_ = declare_parameter<double>(
      "forward_axis_validation_min_distance_m", 20.0);
    forward_axis_validation_min_dot_ = declare_parameter<double>(
      "forward_axis_validation_min_dot", 0.8);

    scale_calibration_mode_ = declare_parameter<int>("scale_calibration_mode", 0);
    scale_min_baseline_m_ = declare_parameter<double>("scale_min_baseline_m", 500.0);
    scale_target_baseline_m_ = declare_parameter<double>("scale_target_baseline_m", 1000.0);
    scale_min_samples_ = static_cast<std::size_t>(declare_parameter<int>("scale_min_samples", 50));
    scale_min_value_ = declare_parameter<double>("scale_min_value", 0.8);
    scale_max_value_ = declare_parameter<double>("scale_max_value", 1.2);
    scale_max_fit_residual_m_ = declare_parameter<double>("scale_max_fit_residual_m", 15.0);
    scale_filter_alpha_ = declare_parameter<double>("scale_filter_alpha", 0.1);
    calibration_max_samples_ = static_cast<std::size_t>(
      declare_parameter<int>("calibration_max_samples", 5000));
    calibration_max_window_s_ = declare_parameter<double>("calibration_max_window_s", 600.0);

    vertical_scale_ = declare_parameter<double>("vertical_scale", 1.0);

    gyro_fallback_enabled_ = declare_parameter<bool>("gyro_fallback_enabled", true);
    gyro_fallback_max_duration_s_ = declare_parameter<double>("gyro_fallback_max_duration_s", 2.0);
    gyro_bias_rad_s_.x = declare_parameter<double>("gyro_bias_x_rad_s", 0.0);
    gyro_bias_rad_s_.y = declare_parameter<double>("gyro_bias_y_rad_s", 0.0);
    gyro_bias_rad_s_.z = declare_parameter<double>("gyro_bias_z_rad_s", 0.0);

    rtk_recovery_mode_ = declare_parameter<std::string>("rtk_recovery_mode", "smooth");
    rtk_recovery_duration_s_ = declare_parameter<double>("rtk_recovery_duration_s", 3.0);
    max_dead_reckoning_duration_s_ =
      declare_parameter<double>("max_dead_reckoning_duration_s", 1800.0);
    max_dead_reckoning_distance_m_ =
      declare_parameter<double>("max_dead_reckoning_distance_m", 30000.0);

    rtk_fix_topic_ = declare_parameter<std::string>("rtk_fix_topic", "/capture/rtk/fix");
    rtk_status_topic_ = declare_parameter<std::string>("rtk_status_topic", "/capture/rtk/status");
    odometry_topic_ =
      declare_parameter<std::string>("odometry_topic", "/capture/odometry/high_rate");
    imu_topic_ = declare_parameter<std::string>("imu_topic", "/capture/imu/data");
    localization_fix_topic_ =
      declare_parameter<std::string>("localization_fix_topic", "/capture/localization/fix");
    localization_status_topic_ =
      declare_parameter<std::string>("localization_status_topic", "/capture/localization/status");
    localization_odometry_topic_ = declare_parameter<std::string>(
      "localization_odometry_topic", "/capture/localization/odometry");
    output_frame_id_ = declare_parameter<std::string>("output_frame_id", "local_enu");
    output_child_frame_id_ = declare_parameter<std::string>("output_child_frame_id", "base_link");
  }

  void validateParameters()
  {
    if (!(output_rate_hz_ > 0.0) || output_rate_hz_ > 100.0) {
      throw std::invalid_argument("output_rate_hz必须位于(0,100] Hz");
    }
    if (!(rtk_timeout_s_ > 0.0) || !(odometry_timeout_s_ > 0.0) ||
      !(imu_timeout_s_ > 0.0))
    {
      throw std::invalid_argument("timeout参数必须为正数");
    }
    if (rtk_recovery_mode_ != "immediate" && rtk_recovery_mode_ != "smooth") {
      throw std::invalid_argument("rtk_recovery_mode只能为immediate或smooth");
    }
    if (!(vertical_scale_ > 0.0) || !std::isfinite(vertical_scale_)) {
      throw std::invalid_argument("vertical_scale必须为有限正数");
    }
    if (scale_target_baseline_m_ < scale_min_baseline_m_) {
      throw std::invalid_argument("scale_target_baseline_m不能小于scale_min_baseline_m");
    }
    if (!rtk_path_simulation_.valid()) {
      throw std::invalid_argument(
              "simulation_test_mode只能为0或1，A/B必须为有效且水平距离非零的经纬高");
    }
    if (!std::isfinite(rtk_time_offset_s_) || std::abs(rtk_time_offset_s_) > 10.0) {
      throw std::invalid_argument("rtk_time_offset_s必须为[-10,10]内有限数");
    }
    if (heading_fit_options_.min_baseline_m < 0.0 ||
      !std::isfinite(heading_fit_options_.sample_spacing_m) ||
      heading_fit_options_.sample_spacing_m <= 0.0 ||
      !std::isfinite(heading_fit_options_.min_baseline_m) ||
      !std::isfinite(heading_fit_options_.valid_baseline_m) ||
      !std::isfinite(heading_fit_options_.target_baseline_m) ||
      heading_fit_options_.valid_baseline_m < heading_fit_options_.min_baseline_m ||
      heading_fit_options_.target_baseline_m < heading_fit_options_.valid_baseline_m ||
      !std::isfinite(heading_fit_options_.baseline_ratio_min) ||
      !std::isfinite(heading_fit_options_.baseline_ratio_max) ||
      heading_fit_options_.baseline_ratio_min <= 0.0 ||
      heading_fit_options_.baseline_ratio_max < heading_fit_options_.baseline_ratio_min ||
      !std::isfinite(heading_fit_options_.max_rmse_m) ||
      !std::isfinite(heading_fit_options_.max_p95_residual_m) ||
      heading_fit_options_.max_rmse_m <= 0.0 ||
      heading_fit_options_.max_p95_residual_m <= 0.0 ||
      !std::isfinite(heading_fit_options_.outlier_min_threshold_m) ||
      heading_fit_options_.outlier_min_threshold_m < 0.0 ||
      !std::isfinite(heading_fit_options_.outlier_mad_multiplier) ||
      heading_fit_options_.outlier_mad_multiplier < 0.0 ||
      !std::isfinite(heading_fit_options_.min_inlier_ratio) ||
      heading_fit_options_.min_inlier_ratio < 0.0 ||
      heading_fit_options_.min_inlier_ratio > 1.0 ||
      !std::isfinite(heading_fit_options_.filter_alpha) ||
      heading_fit_options_.filter_alpha < 0.0 || heading_fit_options_.filter_alpha > 1.0 ||
      !std::isfinite(heading_fit_options_.max_update_jump_rad) ||
      heading_fit_options_.max_update_jump_rad <= 0.0)
    {
      throw std::invalid_argument("长轨迹方位拟合参数无效");
    }
    if (!isFinite(vehicle_forward_axis_body_)) {
      throw std::invalid_argument("vehicle_forward_axis_body的3个分量必须为有限数");
    }
    const double forward_norm = std::sqrt(
      vehicle_forward_axis_body_.x * vehicle_forward_axis_body_.x +
      vehicle_forward_axis_body_.y * vehicle_forward_axis_body_.y +
      vehicle_forward_axis_body_.z * vehicle_forward_axis_body_.z);
    if (!std::isfinite(forward_norm) || forward_norm <= 1.0e-9) {
      throw std::invalid_argument("vehicle_forward_axis_body模长不能接近0");
    }
    vehicle_forward_axis_body_.x /= forward_norm;
    vehicle_forward_axis_body_.y /= forward_norm;
    vehicle_forward_axis_body_.z /= forward_norm;
    if (!isProperRotationMatrix(vehicle_attitude_mount_rotation_bm_)) {
      throw std::invalid_argument(
              "vehicle_attitude_mount_rotation_bm必须是有限、正交且行列式为+1的旋转矩阵");
    }
    if (!(heading_projection_min_norm_ > 0.0) || heading_projection_min_norm_ > 1.0 ||
      !std::isfinite(heading_projection_min_norm_))
    {
      throw std::invalid_argument("heading_projection_min_norm必须位于(0,1]");
    }
    if (scale_calibration_mode_ != 0 && scale_calibration_mode_ != 1) {
      throw std::invalid_argument("scale_calibration_mode只能为0或1");
    }
    if (!(forward_axis_validation_min_speed_mps_ >= 0.0) ||
      !(forward_axis_validation_min_distance_m_ > 0.0) ||
      forward_axis_validation_min_dot_ < -1.0 || forward_axis_validation_min_dot_ > 1.0)
    {
      throw std::invalid_argument("车辆前向轴运动诊断参数无效");
    }
  }

  void onRtkFix(sensor_msgs::msg::NavSatFix::SharedPtr message)
  {
    const std::int64_t stamp_ns = toNanoseconds(message->header.stamp);
    if (stamp_ns <= 0) {
      RCLCPP_WARN_THROTTLE(get_logger(), *get_clock(), 5000, "收到无效RTK fix时间戳");
      return;
    }
    latest_real_fix_.available = true;
    latest_real_fix_.stamp_ns = stamp_ns;
    latest_real_fix_.status = message->status.status;
    latest_real_fix_.llh = Llh{message->latitude, message->longitude, message->altitude};
    if (!rtk_path_simulation_.active()) {
      latest_fix_ = latest_real_fix_;
    }
  }

  void onRtkStatus(interfaces::msg::RtkStatus::SharedPtr message)
  {
    const std::int64_t stamp_ns = toNanoseconds(message->header.stamp);
    if (stamp_ns <= 0) {
      return;
    }
    latest_real_rtk_status_.available = true;
    latest_real_rtk_status_.stamp_ns = stamp_ns;
    latest_real_rtk_status_.rmc_validity = message->rmc_validity;
    latest_real_rtk_status_.gps_state = message->gps_state;
    latest_real_rtk_status_.speed_mps = static_cast<double>(message->speed_knots) * kKnotsToMps;
    latest_real_rtk_status_.track_degrees = message->track_degrees;
    if (!rtk_path_simulation_.active()) {
      latest_rtk_status_ = latest_real_rtk_status_;
    }
  }

  void requestSimulatedRtk()
  {
    pending_simulation_stamp_ns_ = now().nanoseconds();
  }

  void generatePendingSimulatedRtk()
  {
    if (!pending_simulation_stamp_ns_.has_value()) {
      return;
    }
    OdomSample synchronized_odin;
    if (!odom_buffer_.interpolate(*pending_simulation_stamp_ns_, synchronized_odin)) {
      return;
    }
    const auto simulated = rtk_path_simulation_.generate(synchronized_odin.position_m);
    if (!simulated.has_value()) {
      return;
    }
    const std::int64_t simulation_stamp_ns = *pending_simulation_stamp_ns_;
    pending_simulation_stamp_ns_.reset();
    latest_fix_.available = true;
    latest_fix_.stamp_ns = simulation_stamp_ns;
    latest_fix_.status = sensor_msgs::msg::NavSatStatus::STATUS_FIX;
    latest_fix_.llh = simulated->llh;

    latest_rtk_status_.available = true;
    latest_rtk_status_.stamp_ns = simulation_stamp_ns;
    latest_rtk_status_.rmc_validity = static_cast<std::uint8_t>('V');
    latest_rtk_status_.gps_state = 4U;
    latest_rtk_status_.speed_mps = 0.0;
    latest_rtk_status_.track_degrees = 0.0;
    simulation_progress_ratio_ = simulated->progress_ratio;
    if (simulated->reached_point_b && !simulation_reached_logged_) {
      RCLCPP_INFO(get_logger(), "SIMULATION REACHED POINT B");
      simulation_reached_logged_ = true;
    }
  }

  void onOdometry(nav_msgs::msg::Odometry::SharedPtr message)
  {
    OdomSample sample;
    sample.stamp_ns = toNanoseconds(message->header.stamp);
    if (sample.stamp_ns <= 0) {
      sample.stamp_ns = now().nanoseconds();
    }
    sample.position_m = Vector3d{
      message->pose.pose.position.x,
      message->pose.pose.position.y,
      message->pose.pose.position.z};
    sample.orientation_xyzw = Quaterniond{
      message->pose.pose.orientation.x,
      message->pose.pose.orientation.y,
      message->pose.pose.orientation.z,
      message->pose.pose.orientation.w};
    if (!odom_buffer_.add(sample)) {
      RCLCPP_WARN_THROTTLE(
        get_logger(), *get_clock(), 5000, "丢弃时间戳乱序或非有限的ODIN里程计样本");
      return;
    }
    if (rtk_path_simulation_.active()) {
      if (rtk_path_simulation_.captureOdinOrigin(sample.position_m)) {
        RCLCPP_INFO(
          get_logger(), "仿真已记录真实ODIN起点：[%.3f, %.3f, %.3f] m",
          sample.position_m.x, sample.position_m.y, sample.position_m.z);
      }
      generatePendingSimulatedRtk();
    }
    validateForwardAxisMotion(sample);
    last_absolute_orientation_ =
      heading_alignment_valid_ ?
      absoluteQuaternionFromOdin(delta_yaw_rad_, sample.orientation_xyzw) :
      sample.orientation_xyzw;
  }

  void validateForwardAxisMotion(const OdomSample & sample)
  {
    if (!forward_axis_motion_validation_enabled_ ||
      latest_rtk_status_.speed_mps < forward_axis_validation_min_speed_mps_)
    {
      forward_axis_validation_reference_.reset();
      return;
    }
    if (!forward_axis_validation_reference_.has_value()) {
      forward_axis_validation_reference_ = sample;
      return;
    }
    const double delta_x = sample.position_m.x - forward_axis_validation_reference_->position_m.x;
    const double delta_y = sample.position_m.y - forward_axis_validation_reference_->position_m.y;
    const double distance = std::hypot(delta_x, delta_y);
    if (distance < forward_axis_validation_min_distance_m_) {
      return;
    }
    const HorizontalDirection2d forward = projectBodyAxisToHorizontal(
      sample.orientation_xyzw, vehicle_forward_axis_body_, heading_projection_min_norm_);
    if (forward.valid) {
      const double dot = forward.x * delta_x / distance + forward.y * delta_y / distance;
      if (dot < forward_axis_validation_min_dot_) {
        RCLCPP_WARN(
          get_logger(),
          "车辆前向轴与ODIN运动方向不一致：dot=%.3f，检查vehicle_forward_axis_body符号",
          dot);
      }
    }
    forward_axis_validation_reference_ = sample;
  }

  void onImu(sensor_msgs::msg::Imu::SharedPtr message)
  {
    const std::int64_t stamp_ns = toNanoseconds(message->header.stamp);
    if (stamp_ns <= 0) {
      return;
    }
    latest_imu_.available = true;
    latest_imu_.stamp_ns = stamp_ns;
    latest_imu_.angular_velocity_rad_s = Vector3d{
      message->angular_velocity.x - gyro_bias_rad_s_.x,
      message->angular_velocity.y - gyro_bias_rad_s_.y,
      message->angular_velocity.z - gyro_bias_rad_s_.z};

    if (!gyro_fallback_enabled_ || !last_absolute_orientation_.has_value() ||
      !isFinite(latest_imu_.angular_velocity_rad_s))
    {
      return;
    }
    if (last_imu_stamp_ns_ > 0 && stamp_ns > last_imu_stamp_ns_) {
      const double dt_s = static_cast<double>(stamp_ns - last_imu_stamp_ns_) * kNsToSeconds;
      if (dt_s <= imu_timeout_s_ &&
        gyro_fallback_duration_s_ + dt_s <= gyro_fallback_max_duration_s_)
      {
        const Quaterniond integrated = integrateGyro(
          *last_absolute_orientation_, latest_imu_.angular_velocity_rad_s, dt_s);
        if (isValidQuaternion(integrated)) {
          last_absolute_orientation_ = integrated;
          gyro_fallback_duration_s_ += dt_s;
        }
      }
    }
    last_imu_stamp_ns_ = stamp_ns;
  }

  bool rawFixUsable() const noexcept
  {
    return latest_fix_.available && latest_fix_.status >= sensor_msgs::msg::NavSatStatus::STATUS_FIX &&
           isFinite(latest_fix_.llh);
  }

  bool rtkReliable(const std::int64_t now_ns) const noexcept
  {
    if (!rawFixUsable() || !latest_rtk_status_.available ||
      ageSeconds(now_ns, latest_fix_.stamp_ns) > rtk_timeout_s_ ||
      ageSeconds(now_ns, latest_rtk_status_.stamp_ns) > rtk_timeout_s_)
    {
      return false;
    }
    return latest_rtk_status_.gps_state > 0U &&
           (rtk_path_simulation_.active() || isRmcValid(latest_rtk_status_.rmc_validity));
  }

  void updateCalibrationFromLatestRtk(const std::int64_t now_ns)
  {
    if (!rtkReliable(now_ns) ||
      (last_processed_rtk_stamp_ns_ > 0 && latest_fix_.stamp_ns <= last_processed_rtk_stamp_ns_))
    {
      return;
    }
    if (!rtk_origin_llh_.has_value()) {
      rtk_origin_llh_ = latest_fix_.llh;
    }
    const Enu rtk_enu = llhToEnu(*rtk_origin_llh_, latest_fix_.llh);
    if (!isFinite(rtk_enu)) {
      return;
    }
    const auto sync_stamp = applyRtkTimeOffsetNs(latest_fix_.stamp_ns, rtk_time_offset_s_);
    if (!sync_stamp.has_value()) {
      heading_alignment_reason_ = "INVALID_RTK_SYNC_TIME";
      return;
    }
    const std::int64_t sync_stamp_ns = *sync_stamp;
    OdomSample odom_at_rtk;
    if (!odom_buffer_.interpolate(sync_stamp_ns, odom_at_rtk)) {
      heading_alignment_reason_ = "ODOMETRY_INTERPOLATION_UNAVAILABLE";
      return;
    }

    if (scale_calibration_mode_ == 1) {
      calibration_pairs_.push_back(
        CalibrationPair{
          sync_stamp_ns,
          Point2d{odom_at_rtk.position_m.x, odom_at_rtk.position_m.y},
          Point2d{rtk_enu.east_m, rtk_enu.north_m}});
      trimCalibrationPairs();
    }

    const bool fit_sample_added = heading_fit_estimator_.addSample(
      HeadingFitSample{
        sync_stamp_ns,
        odom_at_rtk.position_m.x,
        odom_at_rtk.position_m.y,
        rtk_enu.east_m,
        rtk_enu.north_m});

    if (scale_calibration_mode_ == 1) {
      updateSimilarityCalibration();
    } else {
      horizontal_scale_ = 1.0;
      scale_valid_ = false;
      scale_status_ = interfaces::msg::LocalizationStatus::SCALE_DISABLED;
      scale_baseline_m_ = 0.0;
      scale_fit_residual_m_ = 0.0;
    }
    const HeadingRigidAlignmentState & heading_state = heading_fit_estimator_.state();
    heading_baseline_m_ = heading_state.baseline_odin_m;
    heading_alignment_reason_ = heading_state.invalid_reason;
    if (heading_state.valid && heading_state.update_accepted) {
      heading_alignment_valid_ = true;
      delta_yaw_rad_ = heading_state.delta_yaw_rad;
      last_reliable_delta_yaw_rad_ = heading_state.delta_yaw_rad;
    }
    if (heading_state.valid || (!fit_sample_added && heading_alignment_valid_)) {
      last_reliable_anchor_candidate_ =
        AnchorCandidate{true, sync_stamp_ns, latest_fix_.llh, odom_at_rtk};
    }
    last_processed_rtk_stamp_ns_ = latest_fix_.stamp_ns;
  }

  void trimCalibrationPairs()
  {
    const std::int64_t max_window_ns = static_cast<std::int64_t>(
      std::max(1.0, calibration_max_window_s_) * kSecondsToNs);
    while (calibration_pairs_.size() > 2U &&
      (calibration_pairs_.size() > calibration_max_samples_ ||
      calibration_pairs_.back().stamp_ns - calibration_pairs_.front().stamp_ns > max_window_ns))
    {
      calibration_pairs_.pop_front();
    }
  }

  void updateSimilarityCalibration()
  {
    std::vector<Point2d> source;
    std::vector<Point2d> target;
    source.reserve(calibration_pairs_.size());
    target.reserve(calibration_pairs_.size());
    for (const CalibrationPair & pair : calibration_pairs_) {
      source.push_back(pair.odin_xy);
      target.push_back(pair.rtk_enu);
    }

    Similarity2dOptions options;
    options.min_samples = scale_min_samples_;
    options.min_baseline_m = scale_min_baseline_m_;
    options.min_scale = scale_min_value_;
    options.max_scale = scale_max_value_;
    options.max_rms_residual_m = scale_max_fit_residual_m_;
    const Similarity2dResult fit = estimateSimilarity2d(source, target, options);
    scale_baseline_m_ = fit.baseline_m;
    scale_fit_residual_m_ = fit.rms_residual_m;
    if (!fit.valid) {
      if (fit.invalid_reason == "INSUFFICIENT_SAMPLES" ||
        fit.invalid_reason == "INSUFFICIENT_VALID_SAMPLES" ||
        fit.invalid_reason == "BASELINE_TOO_SHORT")
      {
        scale_status_ = interfaces::msg::LocalizationStatus::SCALE_COLLECTING;
      } else {
        scale_status_ = interfaces::msg::LocalizationStatus::SCALE_REJECTED;
      }
      return;
    }
    horizontal_scale_ = scale_valid_ ?
      horizontal_scale_ + scale_filter_alpha_ * (fit.scale - horizontal_scale_) :
      fit.scale;
    scale_valid_ = true;
    scale_status_ = interfaces::msg::LocalizationStatus::SCALE_VALID;
  }

  void publishOutput()
  {
    const std::int64_t now_ns = now().nanoseconds();
    updateCalibrationFromLatestRtk(now_ns);
    const bool reliable_rtk = rtkReliable(now_ns);

    if (mode_ == InternalMode::kWaitingForRtk) {
      if (reliable_rtk) {
        mode_ = InternalMode::kRtkValid;
      } else if (canEnterDeadReckoning(now_ns)) {
        enterDeadReckoning(now_ns);
      }
    }
    if (mode_ == InternalMode::kRtkValid && !reliable_rtk) {
      if (canEnterDeadReckoning(now_ns)) {
        enterDeadReckoning(now_ns);
      } else {
        mode_ = InternalMode::kWaitingForRtk;
      }
    }
    if (mode_ == InternalMode::kRtkRecovery && !reliable_rtk) {
      recovery_state_ = RecoveryState{};
      mode_ = active_anchor_.has_value() ? InternalMode::kDeadReckoning :
        InternalMode::kWaitingForRtk;
    }
    if (mode_ == InternalMode::kDeadReckoning && reliable_rtk) {
      startRecovery(now_ns);
    }

    Output output;
    if (mode_ == InternalMode::kRtkValid && reliable_rtk) {
      output = makeRtkOutput(now_ns);
    } else if (mode_ == InternalMode::kDeadReckoning) {
      output = makeDeadReckoningOutput(now_ns);
    } else if (mode_ == InternalMode::kRtkRecovery) {
      output = makeRecoveryOutput(now_ns);
    } else {
      output = makeInvalidOutput("NO_VALID_RTK_ANCHOR");
    }

    publishFix(output, now_ns);
    publishOdometry(output, now_ns);
    publishStatus(output, now_ns);
  }

  bool canEnterDeadReckoning(const std::int64_t now_ns) const noexcept
  {
    if (!last_reliable_anchor_candidate_.available) {
      return false;
    }
    const auto latest_odom = odom_buffer_.latest();
    if (!latest_odom.has_value() || !odometryUsable(now_ns, *latest_odom)) {
      return false;
    }
    return last_reliable_anchor_candidate_.stamp_ns > 0 && latest_odom->stamp_ns > 0;
  }

  void enterDeadReckoning(const std::int64_t now_ns)
  {
    DeadReckoningAnchor anchor;
    anchor.llh = last_reliable_anchor_candidate_.llh;
    anchor.odin_position_m = last_reliable_anchor_candidate_.odom.position_m;
    anchor.delta_yaw_rad = last_reliable_delta_yaw_rad_;
    anchor.horizontal_scale = scale_calibration_mode_ == 1 && scale_valid_ ? horizontal_scale_ : 1.0;
    anchor.vertical_scale = vertical_scale_;
    anchor.vehicle_forward_axis_body = vehicle_forward_axis_body_;
    anchor.heading_projection_min_norm = heading_projection_min_norm_;
    active_anchor_ = anchor;
    dr_start_ns_ = now_ns;
    recovery_state_ = RecoveryState{};
    mode_ = InternalMode::kDeadReckoning;
  }

  void startRecovery(const std::int64_t now_ns)
  {
    if (!active_anchor_.has_value() || !last_dr_result_.has_value() || !rawFixUsable()) {
      mode_ = InternalMode::kRtkValid;
      return;
    }
    const Enu rtk_enu = llhToEnu(active_anchor_->llh, latest_fix_.llh);
    const double position_error = std::hypot(
      rtk_enu.east_m - last_dr_result_->enu_position_m.east_m,
      rtk_enu.north_m - last_dr_result_->enu_position_m.north_m);
    recovery_state_.active = true;
    recovery_state_.start_ns = now_ns;
    recovery_state_.from_dr_enu = last_dr_result_->enu_position_m;
    recovery_state_.to_rtk_enu = rtk_enu;
    recovery_state_.position_error_m = position_error;
    last_recovery_position_error_m_ = position_error;
    mode_ = rtk_recovery_mode_ == "immediate" ? InternalMode::kRtkValid : InternalMode::kRtkRecovery;
  }

  Output makeInvalidOutput(const std::string & reason) const
  {
    Output output;
    output.valid = false;
    output.mode = interfaces::msg::LocalizationStatus::MODE_INVALID;
    output.heading_source = interfaces::msg::LocalizationStatus::HEADING_INVALID;
    output.llh = Llh{0.0, 0.0, 0.0};
    output.enu = Enu{0.0, 0.0, 0.0};
    output.orientation = Quaterniond{};
    output.heading_enu_rad = 0.0;
    output.invalid_reason = reason;
    output.position_difference_to_rtk_m = last_recovery_position_error_m_;
    return output;
  }

  Output makeRtkOutput(const std::int64_t now_ns)
  {
    Output output;
    output.valid = true;
    output.mode = interfaces::msg::LocalizationStatus::MODE_RTK;
    output.llh = latest_fix_.llh;
    output.enu = active_anchor_.has_value() ? llhToEnu(active_anchor_->llh, latest_fix_.llh) :
      Enu{0.0, 0.0, 0.0};
    output.heading_source = currentHeadingSourceForRtk(now_ns, output.heading_enu_rad);
    output.heading_valid =
      output.heading_source != interfaces::msg::LocalizationStatus::HEADING_INVALID;
    if (output.heading_source == interfaces::msg::LocalizationStatus::HEADING_INVALID) {
      output.heading_enu_rad = 0.0;
    }
    const auto latest_odom = odom_buffer_.latest();
    if (heading_alignment_valid_ && latest_odom.has_value() &&
      ageSeconds(now_ns, latest_odom->stamp_ns) <= odometry_timeout_s_)
    {
      output.orientation = absoluteQuaternionFromOdin(delta_yaw_rad_, latest_odom->orientation_xyzw);
    } else {
      output.orientation = yawQuaternion(output.heading_enu_rad);
    }
    output.distance_from_anchor_m = horizontalNorm(output.enu);
    output.position_difference_to_rtk_m = last_recovery_position_error_m_;
    output.invalid_reason = "NONE";
    return output;
  }

  std::uint8_t currentHeadingSourceForRtk(
    const std::int64_t now_ns, double & heading_enu_rad)
  {
    const auto latest_odom = odom_buffer_.latest();
    if (heading_alignment_valid_ && latest_odom.has_value() &&
      ageSeconds(now_ns, latest_odom->stamp_ns) <= odometry_timeout_s_)
    {
      const Quaterniond absolute_orientation = absoluteQuaternionFromOdin(
        delta_yaw_rad_, latest_odom->orientation_xyzw);
      const HorizontalDirection2d forward_enu = projectBodyAxisToHorizontal(
        absolute_orientation, vehicle_forward_axis_body_, heading_projection_min_norm_);
      heading_enu_rad = yawFromHorizontalDirection(forward_enu);
      if (forward_enu.valid && std::isfinite(heading_enu_rad)) {
        return interfaces::msg::LocalizationStatus::HEADING_ODIN;
      }
    }
    if (latest_rtk_status_.available && isRmcValid(latest_rtk_status_.rmc_validity) &&
      std::isfinite(latest_rtk_status_.track_degrees))
    {
      heading_enu_rad = clockwiseCourseDegreesToEnuYawRad(latest_rtk_status_.track_degrees);
      return interfaces::msg::LocalizationStatus::HEADING_RTK_TRACK;
    }
    return interfaces::msg::LocalizationStatus::HEADING_INVALID;
  }

  std::optional<Output> makeGyroFallbackOutput(const std::int64_t now_ns) const
  {
    const auto latest_odom = odom_buffer_.latest();
    if (!gyro_fallback_enabled_ || !last_dr_result_.has_value() ||
      !last_dr_result_->valid || !last_absolute_orientation_.has_value() ||
      !latest_odom.has_value() ||
      ageSeconds(now_ns, latest_odom->stamp_ns) >
      odometry_timeout_s_ + gyro_fallback_max_duration_s_ ||
      !latest_imu_.available || ageSeconds(now_ns, latest_imu_.stamp_ns) > imu_timeout_s_)
    {
      return std::nullopt;
    }

    Quaterniond orientation = *last_absolute_orientation_;
    if (!normalizeQuaternion(orientation)) {
      return std::nullopt;
    }
    const HorizontalDirection2d forward_enu = projectBodyAxisToHorizontal(
      orientation, vehicle_forward_axis_body_, heading_projection_min_norm_);
    const double heading_enu_rad = yawFromHorizontalDirection(forward_enu);

    Output output;
    output.valid = true;
    output.mode = interfaces::msg::LocalizationStatus::MODE_DEAD_RECKONING;
    output.heading_valid = forward_enu.valid && std::isfinite(heading_enu_rad);
    output.heading_source = output.heading_valid ?
      interfaces::msg::LocalizationStatus::HEADING_IMU_GYRO :
      interfaces::msg::LocalizationStatus::HEADING_INVALID;
    output.llh = last_dr_result_->llh;
    output.enu = last_dr_result_->enu_position_m;
    output.orientation = orientation;
    output.heading_enu_rad = output.heading_valid ? heading_enu_rad : 0.0;
    output.distance_from_anchor_m = last_dr_result_->distance_from_anchor_m;
    output.dr_duration_s = ageSeconds(now_ns, dr_start_ns_);
    output.position_difference_to_rtk_m = last_recovery_position_error_m_;
    output.invalid_reason = "NONE";
    return output;
  }

  Output makeDeadReckoningOutput(const std::int64_t now_ns)
  {
    if (!active_anchor_.has_value()) {
      return makeInvalidOutput("NO_VALID_RTK_ANCHOR");
    }
    const auto latest_odom = odom_buffer_.latest();
    if (!latest_odom.has_value() || !odometryUsable(now_ns, *latest_odom)) {
      const std::optional<Output> gyro_fallback = makeGyroFallbackOutput(now_ns);
      if (gyro_fallback.has_value()) {
        return *gyro_fallback;
      }
      return makeInvalidOutput("ODOMETRY_TIMEOUT");
    }
    DeadReckoningResult result = propagateDeadReckoning(
      *active_anchor_, latest_odom->position_m, latest_odom->orientation_xyzw);
    if (!result.valid) {
      return makeInvalidOutput(result.invalid_reason);
    }
    const double dr_duration_s = ageSeconds(now_ns, dr_start_ns_);
    if (dr_duration_s > max_dead_reckoning_duration_s_) {
      return makeInvalidOutput("DEAD_RECKONING_DURATION_EXCEEDED");
    }
    if (result.distance_from_anchor_m > max_dead_reckoning_distance_m_) {
      return makeInvalidOutput("DEAD_RECKONING_DISTANCE_EXCEEDED");
    }
    last_dr_result_ = result;
    last_absolute_orientation_ = result.orientation_xyzw;
    gyro_fallback_duration_s_ = 0.0;

    Output output;
    output.valid = true;
    output.mode = interfaces::msg::LocalizationStatus::MODE_DEAD_RECKONING;
    output.heading_source = result.heading_valid ?
      interfaces::msg::LocalizationStatus::HEADING_ODIN :
      interfaces::msg::LocalizationStatus::HEADING_INVALID;
    output.llh = result.llh;
    output.enu = result.enu_position_m;
    output.orientation = result.orientation_xyzw;
    output.heading_valid = result.heading_valid;
    output.heading_enu_rad = result.heading_enu_rad;
    output.distance_from_anchor_m = result.distance_from_anchor_m;
    output.dr_duration_s = dr_duration_s;
    output.position_difference_to_rtk_m = last_recovery_position_error_m_;
    output.invalid_reason = "NONE";
    return output;
  }

  Output makeRecoveryOutput(const std::int64_t now_ns)
  {
    if (!active_anchor_.has_value() || !recovery_state_.active) {
      mode_ = InternalMode::kRtkValid;
      return makeRtkOutput(now_ns);
    }
    const double duration = std::max(0.001, rtk_recovery_duration_s_);
    const double ratio = std::clamp(ageSeconds(now_ns, recovery_state_.start_ns) / duration, 0.0, 1.0);
    Enu blended{
      recovery_state_.from_dr_enu.east_m +
        ratio * (recovery_state_.to_rtk_enu.east_m - recovery_state_.from_dr_enu.east_m),
      recovery_state_.from_dr_enu.north_m +
        ratio * (recovery_state_.to_rtk_enu.north_m - recovery_state_.from_dr_enu.north_m),
      recovery_state_.from_dr_enu.up_m +
        ratio * (recovery_state_.to_rtk_enu.up_m - recovery_state_.from_dr_enu.up_m)};

    Output output;
    output.valid = true;
    output.mode = interfaces::msg::LocalizationStatus::MODE_RTK_RECOVERY;
    output.heading_source = currentHeadingSourceForRtk(now_ns, output.heading_enu_rad);
    output.heading_valid =
      output.heading_source != interfaces::msg::LocalizationStatus::HEADING_INVALID;
    output.llh = enuToLlh(active_anchor_->llh, blended);
    output.enu = blended;
    const auto latest_odom = odom_buffer_.latest();
    if (heading_alignment_valid_ && latest_odom.has_value() &&
      ageSeconds(now_ns, latest_odom->stamp_ns) <= odometry_timeout_s_)
    {
      output.orientation = absoluteQuaternionFromOdin(delta_yaw_rad_, latest_odom->orientation_xyzw);
    } else {
      output.orientation = yawQuaternion(output.heading_enu_rad);
    }
    output.distance_from_anchor_m = horizontalNorm(blended);
    output.dr_duration_s = ageSeconds(now_ns, dr_start_ns_);
    output.position_difference_to_rtk_m = recovery_state_.position_error_m;
    output.invalid_reason = "NONE";
    if (ratio >= 1.0) {
      recovery_state_ = RecoveryState{};
      mode_ = InternalMode::kRtkValid;
    }
    return output;
  }

  void publishFix(const Output & output, const std::int64_t now_ns)
  {
    sensor_msgs::msg::NavSatFix message;
    message.header.stamp = fromNanoseconds(now_ns);
    message.header.frame_id = "wgs84";
    message.status.status = output.valid ?
      sensor_msgs::msg::NavSatStatus::STATUS_FIX :
      sensor_msgs::msg::NavSatStatus::STATUS_NO_FIX;
    message.status.service = sensor_msgs::msg::NavSatStatus::SERVICE_GPS;
    message.latitude = output.llh.latitude_deg;
    message.longitude = output.llh.longitude_deg;
    message.altitude = output.llh.altitude_m;
    message.position_covariance_type = sensor_msgs::msg::NavSatFix::COVARIANCE_TYPE_UNKNOWN;
    fix_publisher_->publish(message);
  }

  void publishOdometry(const Output & output, const std::int64_t now_ns)
  {
    nav_msgs::msg::Odometry message;
    message.header.stamp = fromNanoseconds(now_ns);
    message.header.frame_id = output_frame_id_;
    message.child_frame_id = output_child_frame_id_;
    message.pose.pose.position.x = output.enu.east_m;
    message.pose.pose.position.y = output.enu.north_m;
    message.pose.pose.position.z = output.enu.up_m;
    Quaterniond orientation = output.orientation;
    if (!isValidQuaternion(orientation)) {
      orientation = yawQuaternion(output.heading_enu_rad);
    }
    if (!isValidQuaternion(orientation)) {
      orientation = Quaterniond{};
    }
    message.pose.pose.orientation.x = orientation.x;
    message.pose.pose.orientation.y = orientation.y;
    message.pose.pose.orientation.z = orientation.z;
    message.pose.pose.orientation.w = orientation.w;
    odometry_publisher_->publish(message);
  }

  void publishStatus(const Output & output, const std::int64_t now_ns)
  {
    interfaces::msg::LocalizationStatus message;
    message.header.stamp = fromNanoseconds(now_ns);
    message.header.frame_id = output_frame_id_;
    message.valid = output.valid;
    message.mode = output.mode;
    message.heading_source = output.heading_source;
    message.latitude = output.llh.latitude_deg;
    message.longitude = output.llh.longitude_deg;
    message.altitude = output.llh.altitude_m;
    message.heading_deg = output.valid && output.heading_valid ?
      enuYawRadToClockwiseCourseDegrees(output.heading_enu_rad) : 0.0;
    const auto latest_odom = odom_buffer_.latest();
    if (latest_odom.has_value()) {
      const auto & orientation = latest_odom->orientation_xyzw;
      VehicleAttitude attitude{};
      if (vehicleAttitudeFromOdinQuaternion(
          orientation.x, orientation.y, orientation.z, orientation.w,
          vehicle_attitude_mount_rotation_bm_, attitude))
      {
        message.vehicle_attitude_valid = true;
        message.vehicle_pitch_deg = radiansToDegrees(attitude.pitch_rad);
        message.vehicle_roll_deg = radiansToDegrees(attitude.roll_rad);
        message.vehicle_heading_deg = radiansToDegrees(attitude.heading_rad);
      }
    }
    message.heading_alignment_valid = heading_alignment_valid_;
    message.delta_yaw_deg = radiansToDegrees(delta_yaw_rad_);
    message.scale_calibration_mode = static_cast<std::uint8_t>(scale_calibration_mode_);
    message.scale_status = scale_status_;
    message.scale_valid = scale_valid_;
    message.horizontal_scale = scale_calibration_mode_ == 1 && scale_valid_ ? horizontal_scale_ : 1.0;
    message.vertical_scale = vertical_scale_;
    message.scale_baseline_m = scale_baseline_m_;
    message.scale_fit_residual_m = scale_fit_residual_m_;
    message.heading_baseline_m = heading_baseline_m_;
    message.heading_alignment_reason = heading_alignment_reason_;
    const HeadingRigidAlignmentState & fit = heading_fit_estimator_.state();
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
    message.simulation_test_mode = static_cast<std::uint8_t>(simulation_options_.test_mode);
    message.simulation_progress_percent = 100.0 * simulation_progress_ratio_;
    message.distance_from_anchor_m = output.distance_from_anchor_m;
    message.dr_duration_s = output.dr_duration_s;
    message.rtk_age_s = statusAgeSeconds(now_ns, latest_fix_.stamp_ns);
    message.odometry_age_s =
      latest_odom.has_value() ? statusAgeSeconds(now_ns, latest_odom->stamp_ns) : -1.0;
    message.imu_age_s = statusAgeSeconds(now_ns, latest_imu_.stamp_ns);
    message.position_difference_to_rtk_m = output.position_difference_to_rtk_m;
    message.invalid_reason = output.valid ? "NONE" : output.invalid_reason;
    status_publisher_->publish(message);
  }

  bool odometryUsable(const std::int64_t now_ns, const OdomSample & odom) const noexcept
  {
    return ageSeconds(now_ns, odom.stamp_ns) <= odometry_timeout_s_;
  }

  double output_rate_hz_{10.0};
  double rtk_timeout_s_{1.0};
  double odometry_timeout_s_{0.1};
  double imu_timeout_s_{0.1};
  double rtk_time_offset_s_{0.0};

  Vector3d vehicle_forward_axis_body_{0.0, 0.0, -1.0};
  RotationMatrix3d vehicle_attitude_mount_rotation_bm_ =
    kDefaultVehicleAttitudeMountRotationBm;
  double heading_projection_min_norm_{0.2};
  bool forward_axis_motion_validation_enabled_{true};
  double forward_axis_validation_min_speed_mps_{5.0};
  double forward_axis_validation_min_distance_m_{20.0};
  double forward_axis_validation_min_dot_{0.8};

  int scale_calibration_mode_{0};
  double scale_min_baseline_m_{500.0};
  double scale_target_baseline_m_{1000.0};
  std::size_t scale_min_samples_{50U};
  double scale_min_value_{0.8};
  double scale_max_value_{1.2};
  double scale_max_fit_residual_m_{15.0};
  double scale_filter_alpha_{0.1};
  std::size_t calibration_max_samples_{5000U};
  double calibration_max_window_s_{600.0};
  double vertical_scale_{1.0};

  bool gyro_fallback_enabled_{true};
  double gyro_fallback_max_duration_s_{2.0};
  Vector3d gyro_bias_rad_s_;
  std::string rtk_recovery_mode_{"smooth"};
  double rtk_recovery_duration_s_{3.0};
  double max_dead_reckoning_duration_s_{1800.0};
  double max_dead_reckoning_distance_m_{30000.0};

  std::string rtk_fix_topic_;
  std::string rtk_status_topic_;
  std::string odometry_topic_;
  std::string imu_topic_;
  std::string localization_fix_topic_;
  std::string localization_status_topic_;
  std::string localization_odometry_topic_;
  std::string output_frame_id_;
  std::string output_child_frame_id_;

  LatestFix latest_fix_;
  LatestRtkStatus latest_rtk_status_;
  LatestFix latest_real_fix_;
  LatestRtkStatus latest_real_rtk_status_;
  LatestImu latest_imu_;
  HeadingRigidAlignmentOptions heading_fit_options_;
  HeadingRigidAlignmentEstimator heading_fit_estimator_;
  RtkPathSimulationOptions simulation_options_;
  RtkPathSimulation rtk_path_simulation_;
  OdometryBuffer odom_buffer_;
  std::deque<CalibrationPair> calibration_pairs_;
  std::optional<Llh> rtk_origin_llh_;
  std::int64_t last_processed_rtk_stamp_ns_{0};
  bool heading_alignment_valid_{false};
  double delta_yaw_rad_{0.0};
  double last_reliable_delta_yaw_rad_{0.0};
  double heading_baseline_m_{0.0};
  bool scale_valid_{false};
  double horizontal_scale_{1.0};
  double scale_baseline_m_{0.0};
  double scale_fit_residual_m_{0.0};
  std::uint8_t scale_status_{interfaces::msg::LocalizationStatus::SCALE_DISABLED};
  std::string heading_alignment_reason_{"NOT_INITIALIZED"};

  InternalMode mode_{InternalMode::kWaitingForRtk};
  AnchorCandidate last_reliable_anchor_candidate_;
  double simulation_progress_ratio_{0.0};
  bool simulation_reached_logged_{false};
  std::optional<std::int64_t> pending_simulation_stamp_ns_;
  std::optional<DeadReckoningAnchor> active_anchor_;
  std::int64_t dr_start_ns_{0};
  std::optional<DeadReckoningResult> last_dr_result_;
  RecoveryState recovery_state_;
  double last_recovery_position_error_m_{0.0};
  std::optional<Quaterniond> last_absolute_orientation_;
  std::int64_t last_imu_stamp_ns_{0};
  double gyro_fallback_duration_s_{0.0};
  std::optional<OdomSample> forward_axis_validation_reference_;

  rclcpp::Subscription<sensor_msgs::msg::NavSatFix>::SharedPtr rtk_fix_subscription_;
  rclcpp::Subscription<interfaces::msg::RtkStatus>::SharedPtr rtk_status_subscription_;
  rclcpp::Subscription<nav_msgs::msg::Odometry>::SharedPtr odom_subscription_;
  rclcpp::Subscription<sensor_msgs::msg::Imu>::SharedPtr imu_subscription_;
  rclcpp::Publisher<sensor_msgs::msg::NavSatFix>::SharedPtr fix_publisher_;
  rclcpp::Publisher<nav_msgs::msg::Odometry>::SharedPtr odometry_publisher_;
  rclcpp::Publisher<interfaces::msg::LocalizationStatus>::SharedPtr status_publisher_;
  rclcpp::TimerBase::SharedPtr output_timer_;
  rclcpp::TimerBase::SharedPtr simulation_timer_;
};

}  // namespace localization

int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);
  try {
    rclcpp::spin(std::make_shared<localization::DeadReckoningNode>());
  } catch (const std::exception & error) {
    RCLCPP_FATAL(
      rclcpp::get_logger("dead_reckoning_node"), "ODIN航位推算节点启动失败：%s",
      error.what());
    rclcpp::shutdown();
    return 1;
  }
  rclcpp::shutdown();
  return 0;
}

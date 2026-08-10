#include "localization/dead_reckoning.hpp"
#include "localization/geodesy.hpp"
#include "localization/heading_alignment.hpp"
#include "localization/similarity_alignment.hpp"

#include "builtin_interfaces/msg/time.hpp"
#include "interfaces/msg/localization_status.hpp"
#include "interfaces/msg/rtk_status.hpp"
#include "nav_msgs/msg/odometry.hpp"
#include "rcl_interfaces/msg/set_parameters_result.hpp"
#include "rclcpp/rclcpp.hpp"
#include "sensor_msgs/msg/imu.hpp"
#include "sensor_msgs/msg/nav_sat_fix.hpp"
#include "sensor_msgs/msg/nav_sat_status.hpp"

#include <algorithm>
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
constexpr double kSimulatedRtkLatitudeDeg = 24.0 + 34.0 / 60.0 + 26.0 / 3600.0;
constexpr double kSimulatedRtkLongitudeDeg = 118.0 + 5.0 / 60.0 + 22.0 / 3600.0;
constexpr double kSimulatedRtkAltitudeM = 20.0;
constexpr double kSimulatedRtkTrackDeg = 45.0;

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

double filteredAngle(double current, double target, double alpha) noexcept
{
  if (!std::isfinite(current)) {
    return target;
  }
  alpha = std::clamp(alpha, 0.0, 1.0);
  return wrapAngleRad(current + alpha * wrapAngleRad(target - current));
}

}  // namespace

struct OdomSample
{
  std::int64_t stamp_ns{0};
  Vector3d position_m;
  Quaterniond orientation_xyzw;
};

class OdomBuffer
{
public:
  OdomBuffer(
    const std::int64_t cache_duration_ns, const std::int64_t max_interpolation_gap_ns,
    const std::size_t max_samples)
  : cache_duration_ns_(cache_duration_ns),
    max_interpolation_gap_ns_(max_interpolation_gap_ns),
    max_samples_(std::max<std::size_t>(2U, max_samples))
  {
  }

  bool add(OdomSample sample) noexcept
  {
    if (sample.stamp_ns <= 0 || !isFinite(sample.position_m) ||
      !normalizeQuaternion(sample.orientation_xyzw))
    {
      return false;
    }
    if (!samples_.empty() && sample.stamp_ns < samples_.back().stamp_ns) {
      return false;
    }
    if (!samples_.empty() && sample.stamp_ns == samples_.back().stamp_ns) {
      samples_.back() = sample;
    } else {
      samples_.push_back(sample);
    }
    while (samples_.size() > 1U &&
      (samples_.size() > max_samples_ ||
      samples_.back().stamp_ns - samples_.front().stamp_ns > cache_duration_ns_))
    {
      samples_.pop_front();
    }
    return true;
  }

  bool interpolate(const std::int64_t stamp_ns, OdomSample & output) const noexcept
  {
    if (samples_.empty() || stamp_ns < samples_.front().stamp_ns ||
      stamp_ns > samples_.back().stamp_ns)
    {
      return false;
    }
    auto upper = std::lower_bound(
      samples_.begin(), samples_.end(), stamp_ns,
      [](const OdomSample & sample, const std::int64_t target) {
        return sample.stamp_ns < target;
      });
    if (upper != samples_.end() && upper->stamp_ns == stamp_ns) {
      output = *upper;
      return true;
    }
    if (upper == samples_.begin() || upper == samples_.end()) {
      return false;
    }
    const auto lower = std::prev(upper);
    const std::int64_t gap_ns = upper->stamp_ns - lower->stamp_ns;
    if (gap_ns <= 0 || gap_ns > max_interpolation_gap_ns_) {
      return false;
    }
    const double fraction = static_cast<double>(stamp_ns - lower->stamp_ns) /
      static_cast<double>(gap_ns);
    output.stamp_ns = stamp_ns;
    output.position_m = Vector3d{
      lower->position_m.x + fraction * (upper->position_m.x - lower->position_m.x),
      lower->position_m.y + fraction * (upper->position_m.y - lower->position_m.y),
      lower->position_m.z + fraction * (upper->position_m.z - lower->position_m.z)};
    output.orientation_xyzw = slerpQuaternion(
      lower->orientation_xyzw, upper->orientation_xyzw, fraction);
    return isValidQuaternion(output.orientation_xyzw);
  }

  std::optional<OdomSample> latest() const noexcept
  {
    if (samples_.empty()) {
      return std::nullopt;
    }
    return samples_.back();
  }

  std::size_t size() const noexcept
  {
    return samples_.size();
  }

private:
  std::int64_t cache_duration_ns_{0};
  std::int64_t max_interpolation_gap_ns_{0};
  std::size_t max_samples_{1000U};
  std::deque<OdomSample> samples_;
};

class DeadReckoningNode final : public rclcpp::Node
{
public:
  DeadReckoningNode()
  : Node("dead_reckoning_node"),
    heading_estimator_(declareHeadingOptions()),
    course_estimator_(declareCourseOptions()),
    odom_buffer_(
      secondsToNs(declare_parameter<double>("odometry_cache_duration_s", 60.0)),
      secondsToNs(declare_parameter<double>("odometry_interpolation_max_gap_s", 0.2)),
      static_cast<std::size_t>(declare_parameter<int>("odometry_cache_max_samples", 5000)))
  {
    declareRemainingParameters();
    validateParameters();

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

    parameter_callback_handle_ = add_on_set_parameters_callback(
      [this](const std::vector<rclcpp::Parameter> & parameters) {
        return onSetParameters(parameters);
      });

    const auto period = std::chrono::duration<double>(1.0 / output_rate_hz_);
    output_timer_ = create_wall_timer(
      std::chrono::duration_cast<std::chrono::nanoseconds>(period),
      [this]() {publishOutput();});

    RCLCPP_INFO(
      get_logger(),
      "ODIN航位推算节点已启动：fix=%s status=%s odom=%s output=%s",
      rtk_fix_topic_.c_str(), rtk_status_topic_.c_str(), odometry_topic_.c_str(),
      localization_status_topic_.c_str());
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

  HeadingAlignmentOptions declareHeadingOptions()
  {
    HeadingAlignmentOptions options;
    options.min_samples = static_cast<std::size_t>(
      declare_parameter<int>("heading_alignment_min_samples", 10));
    options.min_distance_m = declare_parameter<double>("heading_alignment_min_distance_m", 50.0);
    options.max_std_rad = degreesToRadians(
      declare_parameter<double>("heading_alignment_max_std_deg", 5.0));
    options.filter_alpha = declare_parameter<double>("heading_filter_alpha", 0.1);
    options.max_samples = static_cast<std::size_t>(
      declare_parameter<int>("heading_alignment_max_samples", 200));
    return options;
  }

  CourseEstimatorOptions declareCourseOptions()
  {
    CourseEstimatorOptions options;
    options.enabled = declare_parameter<bool>("course_from_position_enabled", true);
    options.min_speed_mps = declare_parameter<double>("course_min_speed_mps", 5.0);
    options.min_baseline_m = declare_parameter<double>("course_min_baseline_m", 30.0);
    options.max_baseline_m = declare_parameter<double>("course_max_baseline_m", 200.0);
    options.max_window_s = declare_parameter<double>("course_max_window_s", 30.0);
    options.max_jump_rad = degreesToRadians(declare_parameter<double>("course_max_jump_deg", 20.0));
    options.max_samples = static_cast<std::size_t>(
      declare_parameter<int>("course_max_samples", 300));
    course_min_speed_mps_ = options.min_speed_mps;
    course_max_jump_rad_ = options.max_jump_rad;
    return options;
  }

  void declareRemainingParameters()
  {
    output_rate_hz_ = declare_parameter<double>("output_rate_hz", 10.0);
    rtk_timeout_s_ = declare_parameter<double>("rtk_timeout_s", 1.0);
    odometry_timeout_s_ = declare_parameter<double>("odometry_timeout_s", 0.1);
    imu_timeout_s_ = declare_parameter<double>("imu_timeout_s", 0.1);
    rtk_simulation_enabled_ = declare_parameter<int>("rtk_simulation_enabled", 0);

    scale_calibration_enabled_ = declare_parameter<bool>("scale_calibration_enabled", false);
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

  void validateParameters() const
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
    if (rtk_simulation_enabled_ != 0 && rtk_simulation_enabled_ != 1) {
      throw std::invalid_argument("rtk_simulation_enabled只能为0或1");
    }
  }

  rcl_interfaces::msg::SetParametersResult onSetParameters(
    const std::vector<rclcpp::Parameter> & parameters)
  {
    rcl_interfaces::msg::SetParametersResult result;
    result.successful = true;

    for (const auto & parameter : parameters) {
      if (parameter.get_name() != "rtk_simulation_enabled") {
        continue;
      }
      if (parameter.get_type() != rclcpp::ParameterType::PARAMETER_INTEGER) {
        result.successful = false;
        result.reason = "rtk_simulation_enabled must be integer 0 or 1";
        return result;
      }
      const int value = parameter.as_int();
      if (value != 0 && value != 1) {
        result.successful = false;
        result.reason = "rtk_simulation_enabled must be 0 or 1";
        return result;
      }
      setRtkSimulationEnabled(value);
    }
    return result;
  }

  void setRtkSimulationEnabled(const int value)
  {
    if (rtk_simulation_enabled_ == value) {
      return;
    }
    rtk_simulation_enabled_ = value;
    if (rtk_simulation_enabled_ == 0) {
      latest_fix_ = latest_real_fix_;
      latest_rtk_status_ = latest_real_rtk_status_;
      last_processed_rtk_stamp_ns_ = 0;
      latest_heading_observation_source_ =
        interfaces::msg::LocalizationStatus::HEADING_INVALID;
      RCLCPP_INFO(get_logger(), "RTK simulation disabled; real RTK input restored");
      return;
    }

    recovery_state_ = RecoveryState{};
    last_recovery_position_error_m_ = 0.0;
    mode_ = InternalMode::kRtkValid;
    RCLCPP_INFO(
      get_logger(),
      "RTK simulation enabled: lat=%.10f lon=%.10f alt=%.2f track=%.1f deg",
      kSimulatedRtkLatitudeDeg, kSimulatedRtkLongitudeDeg, kSimulatedRtkAltitudeM,
      kSimulatedRtkTrackDeg);
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
    if (rtk_simulation_enabled_ == 0) {
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
    if (rtk_simulation_enabled_ == 0) {
      latest_rtk_status_ = latest_real_rtk_status_;
    }
  }

  void applyRtkSimulation(const std::int64_t now_ns)
  {
    std::int64_t simulation_stamp_ns = now_ns;
    const auto latest_odom = odom_buffer_.latest();
    const bool odom_fresh = latest_odom.has_value() &&
      ageSeconds(now_ns, latest_odom->stamp_ns) <= odometry_timeout_s_;
    if (odom_fresh) {
      simulation_stamp_ns = latest_odom->stamp_ns;
    }

    latest_fix_.available = true;
    latest_fix_.stamp_ns = simulation_stamp_ns;
    latest_fix_.status = sensor_msgs::msg::NavSatStatus::STATUS_FIX;
    latest_fix_.llh =
      Llh{kSimulatedRtkLatitudeDeg, kSimulatedRtkLongitudeDeg, kSimulatedRtkAltitudeM};

    latest_rtk_status_.available = true;
    latest_rtk_status_.stamp_ns = simulation_stamp_ns;
    latest_rtk_status_.rmc_validity = static_cast<std::uint8_t>('A');
    latest_rtk_status_.gps_state = 4U;
    latest_rtk_status_.speed_mps = std::max(course_min_speed_mps_, 5.0);
    latest_rtk_status_.track_degrees = kSimulatedRtkTrackDeg;
    latest_heading_observation_source_ =
      interfaces::msg::LocalizationStatus::HEADING_RTK_TRACK;

    if (!rtk_origin_llh_.has_value()) {
      rtk_origin_llh_ = latest_fix_.llh;
    }
    if (!odom_fresh) {
      return;
    }

    const double absolute_yaw_enu =
      clockwiseCourseDegreesToEnuYawRad(kSimulatedRtkTrackDeg);
    const double odin_yaw = yawFromRosQuaternion(latest_odom->orientation_xyzw);
    if (!std::isfinite(absolute_yaw_enu) || !std::isfinite(odin_yaw)) {
      return;
    }

    delta_yaw_rad_ = wrapAngleRad(absolute_yaw_enu - odin_yaw);
    heading_alignment_valid_ = true;
    heading_baseline_m_ = std::max(heading_baseline_m_, 1.0);
    last_reliable_anchor_candidate_ =
      AnchorCandidate{true, simulation_stamp_ns, latest_fix_.llh, *latest_odom};
    last_processed_rtk_stamp_ns_ = simulation_stamp_ns;
    last_absolute_orientation_ =
      absoluteQuaternionFromOdin(delta_yaw_rad_, latest_odom->orientation_xyzw);
  }

  void onOdometry(nav_msgs::msg::Odometry::SharedPtr message)
  {
    OdomSample sample;
    sample.stamp_ns = toNanoseconds(message->header.stamp);
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
    last_absolute_orientation_ =
      heading_alignment_valid_ ?
      absoluteQuaternionFromOdin(delta_yaw_rad_, sample.orientation_xyzw) :
      sample.orientation_xyzw;
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
    return latest_rtk_status_.gps_state > 0U && isRmcValid(latest_rtk_status_.rmc_validity);
  }

  bool trackHeadingUsable(double & yaw_enu_rad) noexcept
  {
    if (!latest_rtk_status_.available || !isRmcValid(latest_rtk_status_.rmc_validity) ||
      latest_rtk_status_.speed_mps < course_min_speed_mps() ||
      !std::isfinite(latest_rtk_status_.track_degrees))
    {
      return false;
    }
    yaw_enu_rad = clockwiseCourseDegreesToEnuYawRad(latest_rtk_status_.track_degrees);
    if (last_rtk_track_yaw_enu_rad_.has_value() &&
      std::abs(wrapAngleRad(yaw_enu_rad - *last_rtk_track_yaw_enu_rad_)) >
      course_max_jump_rad())
    {
      return false;
    }
    last_rtk_track_yaw_enu_rad_ = yaw_enu_rad;
    return true;
  }

  double course_min_speed_mps() const noexcept
  {
    return course_min_speed_mps_;
  }

  double course_max_jump_rad() const noexcept
  {
    return course_max_jump_rad_;
  }

  void updateCalibrationFromLatestRtk(const std::int64_t now_ns)
  {
    latest_heading_observation_source_ = interfaces::msg::LocalizationStatus::HEADING_INVALID;
    if (!rtkReliable(now_ns) || latest_fix_.stamp_ns == last_processed_rtk_stamp_ns_) {
      return;
    }
    if (!rtk_origin_llh_.has_value()) {
      rtk_origin_llh_ = latest_fix_.llh;
    }
    const Enu rtk_enu = llhToEnu(*rtk_origin_llh_, latest_fix_.llh);
    if (!isFinite(rtk_enu)) {
      return;
    }
    course_estimator_.addSample(
      CoursePositionSample{
        latest_fix_.stamp_ns, rtk_enu.east_m, rtk_enu.north_m,
        latest_rtk_status_.speed_mps});

    OdomSample odom_at_rtk;
    if (!odom_buffer_.interpolate(latest_fix_.stamp_ns, odom_at_rtk)) {
      return;
    }

    calibration_pairs_.push_back(
      CalibrationPair{
        latest_fix_.stamp_ns,
        Point2d{odom_at_rtk.position_m.x, odom_at_rtk.position_m.y},
        Point2d{rtk_enu.east_m, rtk_enu.north_m}});
    trimCalibrationPairs();

    double absolute_yaw_enu = 0.0;
    double heading_baseline_m = 0.0;
    std::uint8_t source = interfaces::msg::LocalizationStatus::HEADING_INVALID;
    if (trackHeadingUsable(absolute_yaw_enu)) {
      source = interfaces::msg::LocalizationStatus::HEADING_RTK_TRACK;
      heading_baseline_m = std::max(0.1, latest_rtk_status_.speed_mps / std::max(1.0, output_rate_hz_));
    } else {
      const CourseEstimate estimate = course_estimator_.estimate();
      if (estimate.valid) {
        absolute_yaw_enu = estimate.yaw_enu_rad;
        heading_baseline_m = estimate.baseline_m;
        source = interfaces::msg::LocalizationStatus::HEADING_RTK_POSITION;
      }
    }

    if (source != interfaces::msg::LocalizationStatus::HEADING_INVALID) {
      const double odin_yaw = yawFromRosQuaternion(odom_at_rtk.orientation_xyzw);
      const double delta_sample = wrapAngleRad(absolute_yaw_enu - odin_yaw);
      if (heading_estimator_.addObservation(delta_sample, heading_baseline_m)) {
        latest_heading_observation_source_ = source;
      }
    }

    updateSimilarityCalibration();
    const HeadingAlignmentState heading_state = heading_estimator_.state();
    heading_baseline_m_ = heading_state.baseline_m;
    if (heading_state.valid) {
      heading_alignment_valid_ = true;
      delta_yaw_rad_ = heading_state.delta_yaw_rad;
    }

    last_reliable_anchor_candidate_ =
      AnchorCandidate{true, latest_fix_.stamp_ns, latest_fix_.llh, odom_at_rtk};
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
    if (!scale_calibration_enabled_) {
      horizontal_scale_ = 1.0;
      scale_valid_ = false;
      scale_baseline_m_ = calibrationTrajectoryBaseline();
      return;
    }
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
    if (!fit.valid) {
      return;
    }
    horizontal_scale_ = scale_valid_ ?
      horizontal_scale_ + scale_filter_alpha_ * (fit.scale - horizontal_scale_) :
      fit.scale;
    scale_valid_ = true;
    delta_yaw_rad_ = heading_alignment_valid_ ?
      filteredAngle(delta_yaw_rad_, fit.yaw_rad, scale_filter_alpha_) :
      fit.yaw_rad;
    heading_alignment_valid_ = true;
  }

  double calibrationTrajectoryBaseline() const noexcept
  {
    if (calibration_pairs_.size() < 2U) {
      return 0.0;
    }
    double min_x = calibration_pairs_.front().rtk_enu.x;
    double max_x = calibration_pairs_.front().rtk_enu.x;
    double min_y = calibration_pairs_.front().rtk_enu.y;
    double max_y = calibration_pairs_.front().rtk_enu.y;
    for (const CalibrationPair & pair : calibration_pairs_) {
      min_x = std::min(min_x, pair.rtk_enu.x);
      max_x = std::max(max_x, pair.rtk_enu.x);
      min_y = std::min(min_y, pair.rtk_enu.y);
      max_y = std::max(max_y, pair.rtk_enu.y);
    }
    return std::hypot(max_x - min_x, max_y - min_y);
  }

  void publishOutput()
  {
    const std::int64_t now_ns = now().nanoseconds();
    if (rtk_simulation_enabled_ == 1) {
      applyRtkSimulation(now_ns);
    } else {
      updateCalibrationFromLatestRtk(now_ns);
    }
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
    if (!last_reliable_anchor_candidate_.available || !heading_alignment_valid_) {
      return false;
    }
    const auto latest_odom = odom_buffer_.latest();
    if (!latest_odom.has_value() ||
      ageSeconds(now_ns, latest_odom->stamp_ns) > odometry_timeout_s_)
    {
      return false;
    }
    return last_reliable_anchor_candidate_.stamp_ns > 0 && latest_odom->stamp_ns > 0;
  }

  void enterDeadReckoning(const std::int64_t now_ns)
  {
    DeadReckoningAnchor anchor;
    anchor.llh = last_reliable_anchor_candidate_.llh;
    anchor.odin_position_m = last_reliable_anchor_candidate_.odom.position_m;
    anchor.delta_yaw_rad = delta_yaw_rad_;
    anchor.horizontal_scale = scale_calibration_enabled_ && scale_valid_ ? horizontal_scale_ : 1.0;
    anchor.vertical_scale = vertical_scale_;
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
    if (latest_heading_observation_source_ != interfaces::msg::LocalizationStatus::HEADING_INVALID) {
      if (latest_heading_observation_source_ == interfaces::msg::LocalizationStatus::HEADING_RTK_TRACK) {
        heading_enu_rad = clockwiseCourseDegreesToEnuYawRad(latest_rtk_status_.track_degrees);
      } else {
        const CourseEstimate estimate = course_estimator_.estimate();
        heading_enu_rad = estimate.valid ? estimate.yaw_enu_rad : 0.0;
      }
      return latest_heading_observation_source_;
    }
    const auto latest_odom = odom_buffer_.latest();
    if (heading_alignment_valid_ && latest_odom.has_value() &&
      ageSeconds(now_ns, latest_odom->stamp_ns) <= odometry_timeout_s_)
    {
      heading_enu_rad = wrapAngleRad(yawFromRosQuaternion(latest_odom->orientation_xyzw) + delta_yaw_rad_);
      return interfaces::msg::LocalizationStatus::HEADING_ODIN;
    }
    return interfaces::msg::LocalizationStatus::HEADING_INVALID;
  }

  Output makeDeadReckoningOutput(const std::int64_t now_ns)
  {
    if (!active_anchor_.has_value()) {
      return makeInvalidOutput("NO_VALID_RTK_ANCHOR");
    }
    const auto latest_odom = odom_buffer_.latest();
    if (!latest_odom.has_value() ||
      ageSeconds(now_ns, latest_odom->stamp_ns) > odometry_timeout_s_)
    {
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
    output.heading_source = interfaces::msg::LocalizationStatus::HEADING_ODIN;
    output.llh = result.llh;
    output.enu = result.enu_position_m;
    output.orientation = result.orientation_xyzw;
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
    message.heading_deg = output.valid ? enuYawRadToClockwiseCourseDegrees(output.heading_enu_rad) : 0.0;
    message.heading_alignment_valid = heading_alignment_valid_;
    message.delta_yaw_deg = radiansToDegrees(delta_yaw_rad_);
    message.scale_calibration_enabled = scale_calibration_enabled_;
    message.scale_valid = scale_valid_;
    message.horizontal_scale = scale_calibration_enabled_ && scale_valid_ ? horizontal_scale_ : 1.0;
    message.vertical_scale = vertical_scale_;
    message.scale_baseline_m = scale_baseline_m_;
    message.heading_baseline_m = heading_baseline_m_;
    message.distance_from_anchor_m = output.distance_from_anchor_m;
    message.dr_duration_s = output.dr_duration_s;
    message.rtk_age_s = statusAgeSeconds(now_ns, latest_fix_.stamp_ns);
    const auto latest_odom = odom_buffer_.latest();
    message.odometry_age_s =
      latest_odom.has_value() ? statusAgeSeconds(now_ns, latest_odom->stamp_ns) : -1.0;
    message.imu_age_s = statusAgeSeconds(now_ns, latest_imu_.stamp_ns);
    message.position_difference_to_rtk_m = output.position_difference_to_rtk_m;
    message.invalid_reason = output.valid ? "NONE" : output.invalid_reason;
    status_publisher_->publish(message);
  }

  double output_rate_hz_{10.0};
  double rtk_timeout_s_{1.0};
  double odometry_timeout_s_{0.1};
  double imu_timeout_s_{0.1};
  int rtk_simulation_enabled_{0};
  double course_min_speed_mps_{5.0};
  double course_max_jump_rad_{degreesToRadians(20.0)};

  bool scale_calibration_enabled_{false};
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
  HeadingAlignmentEstimator heading_estimator_;
  CourseFromPositionEstimator course_estimator_;
  OdomBuffer odom_buffer_;
  std::deque<CalibrationPair> calibration_pairs_;
  std::optional<Llh> rtk_origin_llh_;
  std::int64_t last_processed_rtk_stamp_ns_{0};
  std::optional<double> last_rtk_track_yaw_enu_rad_;
  std::uint8_t latest_heading_observation_source_{
    interfaces::msg::LocalizationStatus::HEADING_INVALID};

  bool heading_alignment_valid_{false};
  double delta_yaw_rad_{0.0};
  double heading_baseline_m_{0.0};
  bool scale_valid_{false};
  double horizontal_scale_{1.0};
  double scale_baseline_m_{0.0};

  InternalMode mode_{InternalMode::kWaitingForRtk};
  AnchorCandidate last_reliable_anchor_candidate_;
  std::optional<DeadReckoningAnchor> active_anchor_;
  std::int64_t dr_start_ns_{0};
  std::optional<DeadReckoningResult> last_dr_result_;
  RecoveryState recovery_state_;
  double last_recovery_position_error_m_{0.0};
  std::optional<Quaterniond> last_absolute_orientation_;
  std::int64_t last_imu_stamp_ns_{0};
  double gyro_fallback_duration_s_{0.0};

  rclcpp::Subscription<sensor_msgs::msg::NavSatFix>::SharedPtr rtk_fix_subscription_;
  rclcpp::Subscription<interfaces::msg::RtkStatus>::SharedPtr rtk_status_subscription_;
  rclcpp::Subscription<nav_msgs::msg::Odometry>::SharedPtr odom_subscription_;
  rclcpp::Subscription<sensor_msgs::msg::Imu>::SharedPtr imu_subscription_;
  rclcpp::Publisher<sensor_msgs::msg::NavSatFix>::SharedPtr fix_publisher_;
  rclcpp::Publisher<nav_msgs::msg::Odometry>::SharedPtr odometry_publisher_;
  rclcpp::Publisher<interfaces::msg::LocalizationStatus>::SharedPtr status_publisher_;
  rclcpp::TimerBase::SharedPtr output_timer_;
  rclcpp::node_interfaces::OnSetParametersCallbackHandle::SharedPtr parameter_callback_handle_;
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

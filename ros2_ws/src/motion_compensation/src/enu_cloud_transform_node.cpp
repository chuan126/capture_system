#include "motion_compensation/enu_cloud_transformer.hpp"
#include "motion_compensation/enu_processing_diagnostics.hpp"

#include <builtin_interfaces/msg/time.hpp>
#include <diagnostic_msgs/msg/diagnostic_array.hpp>
#include <diagnostic_msgs/msg/diagnostic_status.hpp>
#include <diagnostic_msgs/msg/key_value.hpp>
#include <rcl_interfaces/msg/integer_range.hpp>
#include <rcl_interfaces/msg/parameter_descriptor.hpp>
#include <nav_msgs/msg/odometry.hpp>
#include <rclcpp/rclcpp.hpp>
#include <rclcpp/create_timer.hpp>
#include <rclcpp/executors/multi_threaded_executor.hpp>
#include <sensor_msgs/msg/point_cloud2.hpp>
#include <sensor_msgs/point_cloud2_iterator.hpp>
#include <std_msgs/msg/string.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <iomanip>
#include <memory>
#include <mutex>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace motion_compensation
{
namespace
{

std::int64_t stampToNanoseconds(const builtin_interfaces::msg::Time & stamp) noexcept
{
  return static_cast<std::int64_t>(stamp.sec) * 1000000000LL + stamp.nanosec;
}

bool hasFloat32Field(const sensor_msgs::msg::PointCloud2 & message, const std::string & name)
{
  return std::any_of(
    message.fields.begin(), message.fields.end(), [&name](const auto & field) {
      return field.name == name && field.datatype == sensor_msgs::msg::PointField::FLOAT32 &&
      field.count == 1U;
    });
}

diagnostic_msgs::msg::KeyValue diagnosticValue(
  const std::string & key, const std::string & value)
{
  diagnostic_msgs::msg::KeyValue item;
  item.key = key;
  item.value = value;
  return item;
}

std::string milliseconds(const double value)
{
  std::ostringstream stream;
  stream << std::fixed << std::setprecision(3) << value;
  return stream.str();
}

std::int64_t steadyNowNanoseconds() noexcept
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
    EnuProcessingDiagnostics::Clock::now().time_since_epoch()).count();
}

bool isInterpolationFailure(const std::string & reason) noexcept
{
  return reason == "REFERENCE_POSE_NOT_COVERED" || reason == "NO_POINT_POSE_COVERED" ||
         reason == "INSUFFICIENT_POSE_COVERAGE";
}

}  // namespace

class EnuCloudTransformNode : public rclcpp::Node
{
public:
  EnuCloudTransformNode()
  : Node("enu_cloud_transform_node")
  {
    transformer_ = std::make_unique<EnuCloudTransformer>(
      secondsToNanoseconds(declare_parameter<double>("pose_cache_duration_s", 2.0)),
      secondsToNanoseconds(declare_parameter<double>("max_interpolation_gap_s", 0.015)),
      declare_parameter<bool>("use_odometry_translation", true),
      declare_parameter<double>("minimum_valid_pose_ratio", 0.75),
      declare_parameter<double>("max_translation_per_scan_m", 2.5),
      declare_parameter<bool>("fallback_to_rotation_only", true),
      secondsToNanoseconds(declare_parameter<double>("timestamp_reset_threshold_s", 1.0)));

    input_topic_ = declare_parameter<std::string>(
      "input_cloud_topic", "/capture/lidar/points_raw");
    odometry_topic_ = declare_parameter<std::string>(
      "odometry_topic", "/capture/odometry/high_rate");
    output_topic_ = declare_parameter<std::string>(
      "output_cloud_topic", "/capture/lidar/points_compensated_enu");
    output_frame_id_ = declare_parameter<std::string>("output_frame_id", "lidar_local_enu");

    const int pending_cloud_limit = declare_parameter<int>("pending_cloud_limit", 2);
    rcl_interfaces::msg::ParameterDescriptor poll_interval_descriptor;
    poll_interval_descriptor.description = "ENU pending点云处理轮询周期，单位ms；修改后需重启节点";
    poll_interval_descriptor.read_only = true;
    rcl_interfaces::msg::IntegerRange poll_interval_range;
    poll_interval_range.from_value = kMinimumProcessingPollIntervalMs;
    poll_interval_range.to_value = kMaximumProcessingPollIntervalMs;
    poll_interval_range.step = 1;
    poll_interval_descriptor.integer_range = {poll_interval_range};
    processing_poll_interval_ms_ = declare_parameter<std::int64_t>(
      "processing_poll_interval_ms", kDefaultProcessingPollIntervalMs,
      poll_interval_descriptor);
    validateProcessingPollIntervalMs(processing_poll_interval_ms_);
    diagnostics_topic_ = declare_parameter<std::string>("diagnostics_topic", "/diagnostics");
    if (pending_cloud_limit <= 0 || output_frame_id_.empty() || diagnostics_topic_.empty()) {
      throw std::invalid_argument(
              "pending_cloud_limit必须为正数，output_frame_id和diagnostics_topic不能为空");
    }
    pending_cloud_limit_ = static_cast<std::size_t>(pending_cloud_limit);
    allowed_partial_tail_ns_ = nonnegativeSecondsToNanoseconds(
      declare_parameter<double>("allowed_partial_tail_s", 0.0));
    max_cloud_wait_ns_ = secondsToNanoseconds(
      declare_parameter<double>("max_cloud_wait_s", 0.05));
    pose_stream_timeout_ns_ = secondsToNanoseconds(
      declare_parameter<double>("pose_stream_timeout_s", 0.05));
    recovery_continuous_pose_ns_ = secondsToNanoseconds(
      declare_parameter<double>("recovery_continuous_pose_s", 0.12));
    odometry_time_offset_ns_ = signedSecondsToNanoseconds(
      declare_parameter<double>("odometry_time_offset_s", 0.0));
    cloud_time_offset_ns_ = signedSecondsToNanoseconds(
      declare_parameter<double>("cloud_time_offset_s", 0.0));
    publish_partial_cloud_ = declare_parameter<bool>("publish_partial_cloud", false);
    publish_empty_on_failure_ = declare_parameter<bool>("publish_empty_on_failure", false);
    device_online_topic_ = declare_parameter<std::string>(
      "device_online_topic", "/capture/lidar/device_online");
    device_offline_topic_ = declare_parameter<std::string>(
      "device_offline_topic", "/capture/lidar/device_offline");
    if (device_online_topic_.empty() || device_offline_topic_.empty()) {
      throw std::invalid_argument("device_online_topic和device_offline_topic不能为空");
    }

    output_publisher_ = create_publisher<sensor_msgs::msg::PointCloud2>(
      output_topic_, rclcpp::QoS(rclcpp::KeepLast(5)).reliable().durability_volatile());
    diagnostics_publisher_ = create_publisher<diagnostic_msgs::msg::DiagnosticArray>(
      diagnostics_topic_, rclcpp::QoS(rclcpp::KeepLast(5)).reliable().durability_volatile());

    odometry_callback_group_ = create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
    cloud_callback_group_ = create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
    processing_callback_group_ = create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
    device_callback_group_ = create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);

    rclcpp::SubscriptionOptions odometry_options;
    odometry_options.callback_group = odometry_callback_group_;
    odometry_subscription_ = create_subscription<nav_msgs::msg::Odometry>(
      odometry_topic_, rclcpp::QoS(rclcpp::KeepLast(1000)).reliable().durability_volatile(),
      std::bind(&EnuCloudTransformNode::odometryCallback, this, std::placeholders::_1),
      odometry_options);

    rclcpp::SubscriptionOptions cloud_options;
    cloud_options.callback_group = cloud_callback_group_;
    cloud_subscription_ = create_subscription<sensor_msgs::msg::PointCloud2>(
      input_topic_, rclcpp::QoS(rclcpp::KeepLast(5)).reliable().durability_volatile(),
      std::bind(&EnuCloudTransformNode::cloudCallback, this, std::placeholders::_1),
      cloud_options);

    rclcpp::SubscriptionOptions device_options;
    device_options.callback_group = device_callback_group_;
    device_online_subscription_ = create_subscription<std_msgs::msg::String>(
      device_online_topic_, rclcpp::QoS(rclcpp::KeepLast(10)).reliable().durability_volatile(),
      [this](const std_msgs::msg::String::ConstSharedPtr) {
        handleDeviceStateChange("DEVICE_ONLINE");
      },
      device_options);
    device_offline_subscription_ = create_subscription<std_msgs::msg::String>(
      device_offline_topic_, rclcpp::QoS(rclcpp::KeepLast(10)).reliable().durability_volatile(),
      [this](const std_msgs::msg::String::ConstSharedPtr) {
        handleDeviceStateChange("DEVICE_OFFLINE");
      },
      device_options);

    processing_timer_ = rclcpp::create_wall_timer(
      std::chrono::milliseconds(processing_poll_interval_ms_),
      std::bind(&EnuCloudTransformNode::processPendingClouds, this),
      processing_callback_group_, get_node_base_interface().get(),
      get_node_timers_interface().get());
    diagnostics_timer_ = create_wall_timer(
      std::chrono::seconds(1), [this]() {publishDiagnostics();});

    RCLCPP_INFO(
      get_logger(),
      "ENU点云转换已启动：cloud=%s odom=%s output=%s frame=%s poll=%ldms wait=%.0fms "
      "recovery=%.0fms；逐点旋转和平移补偿已启用",
      input_topic_.c_str(), odometry_topic_.c_str(), output_topic_.c_str(),
      output_frame_id_.c_str(), static_cast<long>(processing_poll_interval_ms_),
      static_cast<double>(max_cloud_wait_ns_) / 1.0e6,
      static_cast<double>(recovery_continuous_pose_ns_) / 1.0e6);
  }

private:
  enum class MotionState {kNormal, kPoseGap, kRecovering};

  struct PendingCloud
  {
    sensor_msgs::msg::PointCloud2::ConstSharedPtr message;
    EnuProcessingDiagnostics::Clock::time_point enqueued_at{};
  };

  static std::int64_t secondsToNanoseconds(const double seconds)
  {
    if (!(seconds > 0.0) || !std::isfinite(seconds)) {
      throw std::invalid_argument("时间参数必须为有限正数");
    }
    return static_cast<std::int64_t>(std::llround(seconds * 1.0e9));
  }

  static std::int64_t nonnegativeSecondsToNanoseconds(const double seconds)
  {
    if (seconds < 0.0 || !std::isfinite(seconds)) {
      throw std::invalid_argument("允许的点云尾部缺失时间必须为有限非负数");
    }
    return static_cast<std::int64_t>(std::llround(seconds * 1.0e9));
  }

  static std::int64_t signedSecondsToNanoseconds(const double seconds)
  {
    if (!std::isfinite(seconds)) {
      throw std::invalid_argument("时间偏移必须为有限数值");
    }
    return static_cast<std::int64_t>(std::llround(seconds * 1.0e9));
  }

  static bool addOffset(
    const std::int64_t stamp_ns, const std::int64_t offset_ns,
    std::int64_t & adjusted_stamp_ns) noexcept
  {
    if ((offset_ns > 0 && stamp_ns > std::numeric_limits<std::int64_t>::max() - offset_ns) ||
      (offset_ns < 0 && stamp_ns < std::numeric_limits<std::int64_t>::min() - offset_ns))
    {
      return false;
    }
    adjusted_stamp_ns = stamp_ns + offset_ns;
    return adjusted_stamp_ns > 0;
  }

  static const char * motionStateName(const MotionState state) noexcept
  {
    switch (state) {
      case MotionState::kNormal:
        return "NORMAL";
      case MotionState::kPoseGap:
        return "POSE_GAP";
      case MotionState::kRecovering:
        return "RECOVERING";
    }
    return "UNKNOWN";
  }

  static void updateMaximum(
    std::atomic<std::int64_t> & target, const std::int64_t value) noexcept
  {
    auto maximum = target.load(std::memory_order_relaxed);
    while (maximum < value && !target.compare_exchange_weak(
        maximum, value, std::memory_order_relaxed, std::memory_order_relaxed))
    {
    }
  }

  void setStateReason(const std::string & reason)
  {
    std::lock_guard<std::mutex> lock(state_reason_mutex_);
    state_reason_ = reason;
  }

  std::string stateReason() const
  {
    std::lock_guard<std::mutex> lock(state_reason_mutex_);
    return state_reason_;
  }

  std::size_t clearPendingCloudsForPoseGap()
  {
    std::deque<PendingCloud> dropped;
    {
      std::lock_guard<std::mutex> lock(pending_clouds_mutex_);
      dropped.swap(pending_clouds_);
      diagnostics_.observePendingCloudCount(0U);
    }
    for (const auto & pending : dropped) {
      diagnostics_.recordCloudDropped();
      clouds_dropped_pose_gap_total_.fetch_add(1U, std::memory_order_relaxed);
      if (publish_empty_on_failure_ && pending.message) {
        publishEmptyCloud(*pending.message);
      }
    }
    return dropped.size();
  }

  void enterPoseGap(const std::string & reason, const bool clear_poses)
  {
    std::lock_guard<std::mutex> pose_lock(pose_stream_mutex_);
    const MotionState previous = motion_state_.exchange(
      MotionState::kPoseGap, std::memory_order_relaxed);
    setStateReason(reason);
    last_pose_arrival_ns_.store(0, std::memory_order_relaxed);
    if (previous == MotionState::kPoseGap) {
      return;
    }
    pose_generation_.fetch_add(1U, std::memory_order_relaxed);
    if (clear_poses) {
      transformer_->clearPoses();
    }
    continuous_pose_duration_ns_.store(0, std::memory_order_relaxed);
    pose_gap_count_.fetch_add(1U, std::memory_order_relaxed);
    const std::size_t dropped_count = clearPendingCloudsForPoseGap();
    RCLCPP_WARN(
      get_logger(), "运动补偿进入POSE_GAP：%s，已清空%zu帧待处理点云",
      reason.c_str(), dropped_count);
  }

  void enterRecovering(const std::string & reason)
  {
    pose_generation_.fetch_add(1U, std::memory_order_relaxed);
    motion_state_.store(MotionState::kRecovering, std::memory_order_relaxed);
    setStateReason(reason);
    recovery_count_.fetch_add(1U, std::memory_order_relaxed);
    const std::size_t dropped_count = clearPendingCloudsForPoseGap();
    RCLCPP_WARN_THROTTLE(
      get_logger(), *get_clock(), 2000,
      "运动补偿进入RECOVERING：%s，已清空%zu帧待处理点云",
      reason.c_str(), dropped_count);
  }

  void tryFinishRecovery()
  {
    const std::int64_t continuous_ns = transformer_->continuousPoseDurationNs();
    continuous_pose_duration_ns_.store(continuous_ns, std::memory_order_relaxed);
    if (motion_state_.load(std::memory_order_relaxed) != MotionState::kRecovering ||
      continuous_ns < recovery_continuous_pose_ns_)
    {
      return;
    }
    motion_state_.store(MotionState::kNormal, std::memory_order_relaxed);
    setStateReason("POSE_STREAM_STABLE");
    RCLCPP_INFO(
      get_logger(), "高频里程计已连续覆盖%.1fms，运动补偿恢复NORMAL",
      static_cast<double>(continuous_ns) / 1.0e6);
  }

  void handleDeviceStateChange(const std::string & reason)
  {
    enterPoseGap(reason, true);
  }

  bool takePendingCloud(PendingCloud & pending)
  {
    std::lock_guard<std::mutex> lock(pending_clouds_mutex_);
    if (pending_clouds_.empty()) {
      return false;
    }
    pending = std::move(pending_clouds_.front());
    pending_clouds_.pop_front();
    diagnostics_.observePendingCloudCount(pending_clouds_.size());
    return true;
  }

  bool requeueWaitingCloud(PendingCloud pending)
  {
    std::lock_guard<std::mutex> lock(pending_clouds_mutex_);
    if (pending_clouds_.size() >= pending_cloud_limit_) {
      return false;
    }
    pending_clouds_.push_front(std::move(pending));
    diagnostics_.observePendingCloudCount(pending_clouds_.size());
    return true;
  }

  void odometryCallback(const nav_msgs::msg::Odometry::ConstSharedPtr message)
  {
    PoseSample sample;
    const std::int64_t raw_stamp_ns = stampToNanoseconds(message->header.stamp);
    if (!addOffset(raw_stamp_ns, odometry_time_offset_ns_, sample.stamp_ns)) {
      RCLCPP_WARN_THROTTLE(
        get_logger(), *get_clock(), 5000, "里程计时间戳叠加时间偏移后无效");
      return;
    }
    sample.position_m = {
      message->pose.pose.position.x, message->pose.pose.position.y,
      message->pose.pose.position.z};
    sample.quaternion_xyzw = {
      message->pose.pose.orientation.x, message->pose.pose.orientation.y,
      message->pose.pose.orientation.z, message->pose.pose.orientation.w};
    std::lock_guard<std::mutex> pose_lock(pose_stream_mutex_);
    const std::int64_t previous_pose_stamp_ns = transformer_->newestPoseStampNs();
    const auto add_result = transformer_->addPose(sample);
    if (add_result == PoseBuffer::AddResult::kRejected) {
      RCLCPP_WARN_THROTTLE(
        get_logger(), *get_clock(), 5000, "拒绝无效或乱序的高频里程计姿态");
      return;
    }

    last_pose_arrival_ns_.store(steadyNowNanoseconds(), std::memory_order_relaxed);
    if (add_result == PoseBuffer::AddResult::kEpochReset) {
      enterRecovering("TIMESTAMP_EPOCH_RESET");
    } else if (add_result == PoseBuffer::AddResult::kGapReset) {
      const std::int64_t gap_ns = previous_pose_stamp_ns > 0 ?
        sample.stamp_ns - previous_pose_stamp_ns : 0;
      updateMaximum(max_pose_gap_ns_, gap_ns);
      pose_gap_count_.fetch_add(1U, std::memory_order_relaxed);
      enterRecovering("POSE_TIMESTAMP_GAP");
    } else if (motion_state_.load(std::memory_order_relaxed) == MotionState::kPoseGap) {
      enterRecovering("POSE_STREAM_RESUMED");
    }
    tryFinishRecovery();
  }

  void cloudCallback(const sensor_msgs::msg::PointCloud2::ConstSharedPtr message)
  {
    diagnostics_.recordCloudReceived();
    PendingCloud dropped;
    {
      std::lock_guard<std::mutex> lock(pending_clouds_mutex_);
      if (pending_clouds_.size() >= pending_cloud_limit_) {
        dropped = pending_clouds_.front();
        pending_clouds_.pop_front();
        diagnostics_.recordCloudDropped();
      }
      // queue_wait从点云真正进入pending队列开始计时，不包含等待队列互斥锁的时间。
      pending_clouds_.push_back(
        PendingCloud{message, EnuProcessingDiagnostics::Clock::now()});
      diagnostics_.observePendingCloudCount(pending_clouds_.size());
    }
    if (dropped.message) {
      RCLCPP_WARN_THROTTLE(
        get_logger(), *get_clock(), 5000,
        "ENU点云等待队列已满，丢弃最旧点云但不发布空点云");
      if (publish_empty_on_failure_) {
        publishEmptyCloud(*dropped.message);
      }
    }
  }

  bool readPoints(
    const sensor_msgs::msg::PointCloud2 & message, std::vector<TimedRadarPoint> & points,
    std::int64_t & cloud_stamp_ns, std::int64_t & last_point_stamp_ns) const
  {
    if (message.is_bigendian || !hasFloat32Field(message, "x") ||
      !hasFloat32Field(message, "y") || !hasFloat32Field(message, "z") ||
      !hasFloat32Field(message, "offset_time") || message.point_step == 0U ||
      message.data.size() < static_cast<std::size_t>(message.row_step) * message.height)
    {
      return false;
    }
    if (!addOffset(
        stampToNanoseconds(message.header.stamp), cloud_time_offset_ns_, cloud_stamp_ns))
    {
      return false;
    }

    float maximum_offset_s = 0.0F;
    points.clear();
    points.reserve(static_cast<std::size_t>(message.width) * message.height);
    try {
      sensor_msgs::PointCloud2ConstIterator<float> x(message, "x");
      sensor_msgs::PointCloud2ConstIterator<float> y(message, "y");
      sensor_msgs::PointCloud2ConstIterator<float> z(message, "z");
      sensor_msgs::PointCloud2ConstIterator<float> offset(message, "offset_time");
      for (; x != x.end(); ++x, ++y, ++z, ++offset) {
        points.push_back(TimedRadarPoint{*x, *y, *z, *offset});
        if (std::isfinite(*offset) && *offset >= 0.0F) {
          maximum_offset_s = std::max(maximum_offset_s, *offset);
        }
      }
    } catch (const std::runtime_error &) {
      return false;
    }
    const auto maximum_offset_ns = static_cast<std::int64_t>(
      std::llround(static_cast<double>(maximum_offset_s) * 1.0e9));
    return addOffset(cloud_stamp_ns, maximum_offset_ns, last_point_stamp_ns);
  }

  void processPendingClouds()
  {
    const std::int64_t now_ns = steadyNowNanoseconds();
    const std::int64_t last_pose_arrival_ns =
      last_pose_arrival_ns_.load(std::memory_order_relaxed);
    if (last_pose_arrival_ns > 0 && now_ns - last_pose_arrival_ns > pose_stream_timeout_ns_ &&
      motion_state_.load(std::memory_order_relaxed) != MotionState::kPoseGap)
    {
      enterPoseGap("POSE_STREAM_TIMEOUT", true);
      return;
    }

    PendingCloud pending;
    if (!takePendingCloud(pending) || !pending.message) {
      return;
    }
    const auto message = pending.message;
    const auto processing_started_at = EnuProcessingDiagnostics::Clock::now();
    const auto drop_for_pose_gap = [this, &message]() {
        diagnostics_.recordCloudDropped();
        clouds_dropped_pose_gap_total_.fetch_add(1U, std::memory_order_relaxed);
        if (publish_empty_on_failure_) {
          publishEmptyCloud(*message);
        }
      };
    const auto drop_for_timeout = [this, &message]() {
        diagnostics_.recordCloudDropped();
        clouds_dropped_timeout_total_.fetch_add(1U, std::memory_order_relaxed);
        if (publish_empty_on_failure_) {
          publishEmptyCloud(*message);
        }
      };

    if (motion_state_.load(std::memory_order_relaxed) != MotionState::kNormal ||
      !transformer_->initialized())
    {
      drop_for_pose_gap();
      return;
    }
    const std::uint64_t pose_generation = pose_generation_.load(std::memory_order_relaxed);

    std::vector<TimedRadarPoint> raw_points;
    std::int64_t cloud_stamp_ns = 0;
    std::int64_t last_point_stamp_ns = 0;
    if (!readPoints(*message, raw_points, cloud_stamp_ns, last_point_stamp_ns)) {
      RCLCPP_WARN_THROTTLE(
        get_logger(), *get_clock(), 5000,
        "点云布局、时间戳或FLOAT32 xyz/offset_time字段无效");
      if (publish_empty_on_failure_) {
        publishEmptyCloud(*message);
      }
      diagnostics_.recordCloudDropped();
      return;
    }

    cloud_start_stamp_ns_.store(cloud_stamp_ns, std::memory_order_relaxed);
    cloud_end_stamp_ns_.store(last_point_stamp_ns, std::memory_order_relaxed);
    const std::int64_t oldest_pose_stamp_ns = transformer_->oldestPoseStampNs();
    const std::int64_t newest_pose_stamp_ns = transformer_->newestPoseStampNs();
    newest_pose_stamp_ns_.store(newest_pose_stamp_ns, std::memory_order_relaxed);
    cloud_pose_lag_ns_.store(
      last_point_stamp_ns - newest_pose_stamp_ns, std::memory_order_relaxed);

    if (oldest_pose_stamp_ns <= 0 || oldest_pose_stamp_ns > cloud_stamp_ns) {
      RCLCPP_WARN_THROTTLE(
        get_logger(), *get_clock(), 5000,
        "丢弃无法恢复的旧点云：cloud_start=%ld oldest_pose=%ld",
        static_cast<long>(cloud_stamp_ns), static_cast<long>(oldest_pose_stamp_ns));
      diagnostics_.recordInterpolationFailure();
      drop_for_timeout();
      return;
    }

    if (newest_pose_stamp_ns < last_point_stamp_ns) {
      const auto wait_duration = processing_started_at - pending.enqueued_at;
      const std::int64_t wait_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        wait_duration).count();
      cloud_wait_ns_last_.store(std::max<std::int64_t>(0, wait_ns), std::memory_order_relaxed);
      if (wait_ns < max_cloud_wait_ns_) {
        diagnostics_.recordPoseWait();
        if (requeueWaitingCloud(std::move(pending))) {
          return;
        }
        RCLCPP_WARN_THROTTLE(
          get_logger(), *get_clock(), 5000,
          "点云等待位姿时队列已被新帧占满，丢弃旧点云保持实时性");
        drop_for_timeout();
        return;
      }
      if (last_point_stamp_ns - newest_pose_stamp_ns > allowed_partial_tail_ns_) {
        RCLCPP_WARN_THROTTLE(
          get_logger(), *get_clock(), 5000,
          "点云等待位姿超过%.1fms，尾部仍缺少%.1fms姿态，丢弃本帧",
          static_cast<double>(max_cloud_wait_ns_) / 1.0e6,
          static_cast<double>(last_point_stamp_ns - newest_pose_stamp_ns) / 1.0e6);
        drop_for_timeout();
        return;
      }
    }

    diagnostics_.observeQueueWait(processing_started_at - pending.enqueued_at);
    std::vector<EnuPoint> enu_points;
    std::string invalid_reason;
    TransformStatistics statistics;
    const bool fully_qualified = transformer_->transform(
      cloud_stamp_ns, raw_points, enu_points, invalid_reason, &statistics);
    if (!fully_qualified && isInterpolationFailure(invalid_reason)) {
      diagnostics_.recordInterpolationFailure();
    }
    const bool can_publish_partial =
      publish_partial_cloud_ && statistics.transformed_point_count > 0U &&
      enu_points.size() == raw_points.size();

    if (!fully_qualified && !can_publish_partial) {
      RCLCPP_WARN_THROTTLE(
        get_logger(), *get_clock(), 5000,
        "ENU点云转换失败：%s，姿态覆盖率=%.3f，有效点=%zu/%zu",
        invalid_reason.c_str(), statistics.valid_pose_ratio,
        statistics.transformed_point_count, statistics.finite_nonzero_point_count);
      if (publish_empty_on_failure_) {
        publishEmptyCloud(*message);
      }
      diagnostics_.recordCloudDropped();
      return;
    }

    if (pose_generation != pose_generation_.load(std::memory_order_relaxed) ||
      motion_state_.load(std::memory_order_relaxed) != MotionState::kNormal)
    {
      drop_for_pose_gap();
      return;
    }
    if (!fully_qualified) {
      RCLCPP_WARN_THROTTLE(
        get_logger(), *get_clock(), 5000,
        "发布部分补偿点云：%s，姿态覆盖率=%.3f，有效点=%zu/%zu",
        invalid_reason.c_str(), statistics.valid_pose_ratio,
        statistics.transformed_point_count, statistics.finite_nonzero_point_count);
    }
    if (statistics.translation_fallback) {
      RCLCPP_WARN_THROTTLE(
        get_logger(), *get_clock(), 5000,
        "帧起始位置缺少可插值姿态，本帧退化为仅旋转补偿");
    }

    publishCloud(*message, enu_points);
    diagnostics_.recordCloudProcessed();
    diagnostics_.observeProcessingTime(
      EnuProcessingDiagnostics::Clock::now() - processing_started_at);
  }

  void publishDiagnostics()
  {
    const auto snapshot = diagnostics_.snapshot();
    const MotionState motion_state = motion_state_.load(std::memory_order_relaxed);
    const std::int64_t now_ns = steadyNowNanoseconds();
    const std::int64_t last_pose_arrival_ns =
      last_pose_arrival_ns_.load(std::memory_order_relaxed);
    const double pose_stream_age_ms = last_pose_arrival_ns > 0 ?
      static_cast<double>(std::max<std::int64_t>(0, now_ns - last_pose_arrival_ns)) / 1.0e6 :
      -1.0;
    double pending_oldest_age_ms = 0.0;
    {
      std::lock_guard<std::mutex> lock(pending_clouds_mutex_);
      if (!pending_clouds_.empty()) {
        pending_oldest_age_ms = std::chrono::duration<double, std::milli>(
          EnuProcessingDiagnostics::Clock::now() - pending_clouds_.front().enqueued_at).count();
      }
    }
    diagnostic_msgs::msg::DiagnosticArray array;
    array.header.stamp = now();
    diagnostic_msgs::msg::DiagnosticStatus status;
    status.name = "motion_compensation/enu_cloud_transform";
    status.hardware_id = "RK3588";
    status.level = motion_state != MotionState::kNormal ||
      snapshot.pending_cloud_count >= pending_cloud_limit_ ?
      diagnostic_msgs::msg::DiagnosticStatus::WARN :
      diagnostic_msgs::msg::DiagnosticStatus::OK;
    status.message = motion_state != MotionState::kNormal ?
      std::string("ENU运动补偿状态：") + motionStateName(motion_state) :
      (status.level == diagnostic_msgs::msg::DiagnosticStatus::OK ?
      "ENU点云处理正常" : "ENU点云等待队列已满");
    status.values = {
      diagnosticValue("motion_state", motionStateName(motion_state)),
      diagnosticValue("state_reason", stateReason()),
      diagnosticValue(
        "processing_poll_interval_ms", std::to_string(processing_poll_interval_ms_)),
      diagnosticValue(
        "max_cloud_wait_ms", milliseconds(static_cast<double>(max_cloud_wait_ns_) / 1.0e6)),
      diagnosticValue("pose_stream_age_ms", milliseconds(pose_stream_age_ms)),
      diagnosticValue(
        "continuous_pose_duration_ms",
        milliseconds(
          static_cast<double>(continuous_pose_duration_ns_.load(std::memory_order_relaxed)) /
          1.0e6)),
      diagnosticValue(
        "max_pose_gap_ms",
        milliseconds(
          static_cast<double>(max_pose_gap_ns_.load(std::memory_order_relaxed)) / 1.0e6)),
      diagnosticValue(
        "cloud_start_stamp_ns",
        std::to_string(cloud_start_stamp_ns_.load(std::memory_order_relaxed))),
      diagnosticValue(
        "cloud_end_stamp_ns",
        std::to_string(cloud_end_stamp_ns_.load(std::memory_order_relaxed))),
      diagnosticValue(
        "newest_pose_stamp_ns",
        std::to_string(newest_pose_stamp_ns_.load(std::memory_order_relaxed))),
      diagnosticValue(
        "cloud_pose_lag_ms",
        milliseconds(
          static_cast<double>(cloud_pose_lag_ns_.load(std::memory_order_relaxed)) / 1.0e6)),
      diagnosticValue(
        "cloud_wait_ms_last",
        milliseconds(
          static_cast<double>(cloud_wait_ns_last_.load(std::memory_order_relaxed)) / 1.0e6)),
      diagnosticValue("pending_cloud_count", std::to_string(snapshot.pending_cloud_count)),
      diagnosticValue("pending_oldest_age_ms", milliseconds(pending_oldest_age_ms)),
      diagnosticValue(
        "pending_cloud_max_count", std::to_string(snapshot.pending_cloud_max_count)),
      diagnosticValue(
        "clouds_received_total", std::to_string(snapshot.clouds_received_total)),
      diagnosticValue(
        "clouds_processed_total", std::to_string(snapshot.clouds_processed_total)),
      diagnosticValue("clouds_dropped_total", std::to_string(snapshot.clouds_dropped_total)),
      diagnosticValue(
        "clouds_dropped_pose_gap_total",
        std::to_string(clouds_dropped_pose_gap_total_.load(std::memory_order_relaxed))),
      diagnosticValue(
        "clouds_dropped_timeout_total",
        std::to_string(clouds_dropped_timeout_total_.load(std::memory_order_relaxed))),
      diagnosticValue(
        "pose_gap_count", std::to_string(pose_gap_count_.load(std::memory_order_relaxed))),
      diagnosticValue(
        "recovery_count", std::to_string(recovery_count_.load(std::memory_order_relaxed))),
      diagnosticValue("pose_wait_count", std::to_string(snapshot.pose_wait_count)),
      diagnosticValue(
        "interpolation_failure_count", std::to_string(snapshot.interpolation_failure_count)),
      diagnosticValue("queue_wait_ms_last", milliseconds(snapshot.queue_wait_ms_last)),
      diagnosticValue("queue_wait_ms_mean", milliseconds(snapshot.queue_wait_ms_mean)),
      diagnosticValue("queue_wait_ms_max", milliseconds(snapshot.queue_wait_ms_max)),
      diagnosticValue("processing_time_ms_last", milliseconds(snapshot.processing_time_ms_last)),
      diagnosticValue("processing_time_ms_mean", milliseconds(snapshot.processing_time_ms_mean)),
      diagnosticValue("processing_time_ms_max", milliseconds(snapshot.processing_time_ms_max))};
    array.status.push_back(std::move(status));
    diagnostics_publisher_->publish(std::move(array));
  }

  void publishCloud(
    const sensor_msgs::msg::PointCloud2 & input, const std::vector<EnuPoint> & enu_points)
  {
    sensor_msgs::msg::PointCloud2 output;
    output.header = input.header;
    output.header.frame_id = output_frame_id_;
    output.height = 1U;
    output.width = static_cast<std::uint32_t>(enu_points.size());
    output.is_bigendian = false;
    output.is_dense = false;
    sensor_msgs::PointCloud2Modifier modifier(output);
    modifier.setPointCloud2FieldsByString(1, "xyz");
    modifier.resize(enu_points.size());
    sensor_msgs::PointCloud2Iterator<float> east(output, "x");
    sensor_msgs::PointCloud2Iterator<float> north(output, "y");
    sensor_msgs::PointCloud2Iterator<float> up(output, "z");
    for (const EnuPoint & point : enu_points) {
      *east = point.east;
      *north = point.north;
      *up = point.up;
      ++east;
      ++north;
      ++up;
    }
    output_publisher_->publish(output);
  }

  void publishEmptyCloud(const sensor_msgs::msg::PointCloud2 & input)
  {
    sensor_msgs::msg::PointCloud2 output;
    output.header = input.header;
    output.header.frame_id = output_frame_id_;
    output.height = 1U;
    output.width = 0U;
    output.is_bigendian = false;
    output.is_dense = false;
    sensor_msgs::PointCloud2Modifier modifier(output);
    modifier.setPointCloud2FieldsByString(1, "xyz");
    modifier.resize(0U);
    output_publisher_->publish(output);
  }

  std::unique_ptr<EnuCloudTransformer> transformer_;
  std::string input_topic_;
  std::string odometry_topic_;
  std::string output_topic_;
  std::string output_frame_id_;
  std::string diagnostics_topic_;
  std::string device_online_topic_;
  std::string device_offline_topic_;
  std::size_t pending_cloud_limit_{2U};
  std::int64_t processing_poll_interval_ms_{kDefaultProcessingPollIntervalMs};
  std::int64_t allowed_partial_tail_ns_{0};
  std::int64_t max_cloud_wait_ns_{50000000LL};
  std::int64_t pose_stream_timeout_ns_{50000000LL};
  std::int64_t recovery_continuous_pose_ns_{120000000LL};
  std::int64_t odometry_time_offset_ns_{0};
  std::int64_t cloud_time_offset_ns_{0};
  bool publish_partial_cloud_{false};
  bool publish_empty_on_failure_{false};

  EnuProcessingDiagnostics diagnostics_;
  std::mutex pose_stream_mutex_;
  std::atomic<MotionState> motion_state_{MotionState::kPoseGap};
  std::atomic<std::uint64_t> pose_generation_{0U};
  std::atomic<std::uint64_t> pose_gap_count_{0U};
  std::atomic<std::uint64_t> recovery_count_{0U};
  std::atomic<std::uint64_t> clouds_dropped_pose_gap_total_{0U};
  std::atomic<std::uint64_t> clouds_dropped_timeout_total_{0U};
  std::atomic<std::int64_t> last_pose_arrival_ns_{0};
  std::atomic<std::int64_t> continuous_pose_duration_ns_{0};
  std::atomic<std::int64_t> max_pose_gap_ns_{0};
  std::atomic<std::int64_t> cloud_start_stamp_ns_{0};
  std::atomic<std::int64_t> cloud_end_stamp_ns_{0};
  std::atomic<std::int64_t> newest_pose_stamp_ns_{0};
  std::atomic<std::int64_t> cloud_pose_lag_ns_{0};
  std::atomic<std::int64_t> cloud_wait_ns_last_{0};
  mutable std::mutex state_reason_mutex_;
  std::string state_reason_{"WAITING_FOR_POSE"};
  std::mutex pending_clouds_mutex_;
  std::deque<PendingCloud> pending_clouds_;
  rclcpp::CallbackGroup::SharedPtr odometry_callback_group_;
  rclcpp::CallbackGroup::SharedPtr cloud_callback_group_;
  rclcpp::CallbackGroup::SharedPtr processing_callback_group_;
  rclcpp::CallbackGroup::SharedPtr device_callback_group_;
  rclcpp::Subscription<nav_msgs::msg::Odometry>::SharedPtr odometry_subscription_;
  rclcpp::Subscription<sensor_msgs::msg::PointCloud2>::SharedPtr cloud_subscription_;
  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr device_online_subscription_;
  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr device_offline_subscription_;
  rclcpp::Publisher<sensor_msgs::msg::PointCloud2>::SharedPtr output_publisher_;
  rclcpp::Publisher<diagnostic_msgs::msg::DiagnosticArray>::SharedPtr diagnostics_publisher_;
  rclcpp::TimerBase::SharedPtr processing_timer_;
  rclcpp::TimerBase::SharedPtr diagnostics_timer_;
};

}  // namespace motion_compensation

int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);
  try {
    auto node = std::make_shared<motion_compensation::EnuCloudTransformNode>();
    rclcpp::executors::MultiThreadedExecutor executor(rclcpp::ExecutorOptions(), 3U);
    executor.add_node(node);
    executor.spin();
  } catch (const std::exception & error) {
    RCLCPP_FATAL(rclcpp::get_logger("enu_cloud_transform_node"), "%s", error.what());
    rclcpp::shutdown();
    return 1;
  }
  rclcpp::shutdown();
  return 0;
}

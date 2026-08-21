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

#include <algorithm>
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

    const int pending_cloud_limit = declare_parameter<int>("pending_cloud_limit", 5);
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
      declare_parameter<double>("allowed_partial_tail_s", 0.015));
    odometry_time_offset_ns_ = signedSecondsToNanoseconds(
      declare_parameter<double>("odometry_time_offset_s", 0.0));
    cloud_time_offset_ns_ = signedSecondsToNanoseconds(
      declare_parameter<double>("cloud_time_offset_s", 0.0));
    publish_partial_cloud_ = declare_parameter<bool>("publish_partial_cloud", false);
    publish_empty_on_failure_ = declare_parameter<bool>("publish_empty_on_failure", false);

    output_publisher_ = create_publisher<sensor_msgs::msg::PointCloud2>(
      output_topic_, rclcpp::QoS(rclcpp::KeepLast(5)).reliable().durability_volatile());
    diagnostics_publisher_ = create_publisher<diagnostic_msgs::msg::DiagnosticArray>(
      diagnostics_topic_, rclcpp::QoS(rclcpp::KeepLast(5)).reliable().durability_volatile());

    odometry_callback_group_ = create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
    cloud_callback_group_ = create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
    processing_callback_group_ = create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);

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

    processing_timer_ = rclcpp::create_wall_timer(
      std::chrono::milliseconds(processing_poll_interval_ms_),
      std::bind(&EnuCloudTransformNode::processPendingClouds, this),
      processing_callback_group_, get_node_base_interface().get(),
      get_node_timers_interface().get());
    diagnostics_timer_ = create_wall_timer(
      std::chrono::seconds(1), [this]() {publishDiagnostics();});

    RCLCPP_INFO(
      get_logger(),
      "ENU点云转换已启动：cloud=%s odom=%s output=%s frame=%s poll=%ldms；"
      "逐点旋转和平移补偿已启用",
      input_topic_.c_str(), odometry_topic_.c_str(), output_topic_.c_str(),
      output_frame_id_.c_str(), static_cast<long>(processing_poll_interval_ms_));
  }

private:
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
    const auto add_result = transformer_->addPose(sample);
    if (add_result == PoseBuffer::AddResult::kEpochReset) {
      std::size_t dropped_count = 0U;
      {
        std::lock_guard<std::mutex> lock(pending_clouds_mutex_);
        dropped_count = pending_clouds_.size();
        pending_clouds_.clear();
        diagnostics_.observePendingCloudCount(0U);
      }
      for (std::size_t index = 0; index < dropped_count; ++index) {
        diagnostics_.recordCloudDropped();
      }
      RCLCPP_WARN(
        get_logger(), "检测到里程计时间纪元切换，已清空%zu帧旧纪元待处理点云",
        dropped_count);
    } else if (add_result == PoseBuffer::AddResult::kRejected) {
      RCLCPP_WARN_THROTTLE(
        get_logger(), *get_clock(), 5000, "拒绝无效或乱序的高频里程计姿态");
    }
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
    std::lock_guard<std::mutex> queue_lock(pending_clouds_mutex_);
    while (!pending_clouds_.empty()) {
      if (!transformer_->initialized()) {
        diagnostics_.recordPoseWait();
        return;
      }
      const auto pending = pending_clouds_.front();
      const auto message = pending.message;
      const auto processing_started_at = EnuProcessingDiagnostics::Clock::now();
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
        pending_clouds_.pop_front();
        diagnostics_.observePendingCloudCount(pending_clouds_.size());
        continue;
      }

      const std::int64_t newest_pose_stamp_ns = transformer_->newestPoseStampNs();
      if (newest_pose_stamp_ns < last_point_stamp_ns &&
        last_point_stamp_ns - newest_pose_stamp_ns > allowed_partial_tail_ns_)
      {
        diagnostics_.recordPoseWait();
        return;
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
        pending_clouds_.pop_front();
        diagnostics_.observePendingCloudCount(pending_clouds_.size());
        continue;
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
      pending_clouds_.pop_front();
      diagnostics_.observePendingCloudCount(pending_clouds_.size());
    }
  }

  void publishDiagnostics()
  {
    const auto snapshot = diagnostics_.snapshot();
    diagnostic_msgs::msg::DiagnosticArray array;
    array.header.stamp = now();
    diagnostic_msgs::msg::DiagnosticStatus status;
    status.name = "motion_compensation/enu_cloud_transform";
    status.hardware_id = "RK3588";
    status.level = snapshot.pending_cloud_count >= pending_cloud_limit_ ?
      diagnostic_msgs::msg::DiagnosticStatus::WARN :
      diagnostic_msgs::msg::DiagnosticStatus::OK;
    status.message = status.level == diagnostic_msgs::msg::DiagnosticStatus::OK ?
      "ENU点云处理正常" : "ENU点云等待队列已满";
    status.values = {
      diagnosticValue(
        "processing_poll_interval_ms", std::to_string(processing_poll_interval_ms_)),
      diagnosticValue("pending_cloud_count", std::to_string(snapshot.pending_cloud_count)),
      diagnosticValue(
        "pending_cloud_max_count", std::to_string(snapshot.pending_cloud_max_count)),
      diagnosticValue(
        "clouds_received_total", std::to_string(snapshot.clouds_received_total)),
      diagnosticValue(
        "clouds_processed_total", std::to_string(snapshot.clouds_processed_total)),
      diagnosticValue("clouds_dropped_total", std::to_string(snapshot.clouds_dropped_total)),
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
  std::size_t pending_cloud_limit_{5U};
  std::int64_t processing_poll_interval_ms_{kDefaultProcessingPollIntervalMs};
  std::int64_t allowed_partial_tail_ns_{15000000LL};
  std::int64_t odometry_time_offset_ns_{0};
  std::int64_t cloud_time_offset_ns_{0};
  bool publish_partial_cloud_{false};
  bool publish_empty_on_failure_{false};

  struct PendingCloud
  {
    sensor_msgs::msg::PointCloud2::ConstSharedPtr message;
    EnuProcessingDiagnostics::Clock::time_point enqueued_at{};
  };

  EnuProcessingDiagnostics diagnostics_;
  std::mutex pending_clouds_mutex_;
  std::deque<PendingCloud> pending_clouds_;
  rclcpp::CallbackGroup::SharedPtr odometry_callback_group_;
  rclcpp::CallbackGroup::SharedPtr cloud_callback_group_;
  rclcpp::CallbackGroup::SharedPtr processing_callback_group_;
  rclcpp::Subscription<nav_msgs::msg::Odometry>::SharedPtr odometry_subscription_;
  rclcpp::Subscription<sensor_msgs::msg::PointCloud2>::SharedPtr cloud_subscription_;
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

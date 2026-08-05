#include "motion_compensation/enu_cloud_transformer.hpp"

#include <builtin_interfaces/msg/time.hpp>
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
#include <memory>
#include <mutex>
#include <limits>
#include <stdexcept>
#include <string>
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
      declare_parameter<bool>("fallback_to_rotation_only", true));

    input_topic_ = declare_parameter<std::string>(
      "input_cloud_topic", "/capture/lidar/points_raw");
    odometry_topic_ = declare_parameter<std::string>(
      "odometry_topic", "/capture/odometry/high_rate");
    output_topic_ = declare_parameter<std::string>(
      "output_cloud_topic", "/capture/lidar/points_compensated_enu");
    output_frame_id_ = declare_parameter<std::string>("output_frame_id", "lidar_local_enu");

    const int pending_cloud_limit = declare_parameter<int>("pending_cloud_limit", 5);
    const int processing_period_ms = declare_parameter<int>("processing_period_ms", 2);
    if (pending_cloud_limit <= 0 || processing_period_ms <= 0 || output_frame_id_.empty()) {
      throw std::invalid_argument(
              "pending_cloud_limit和processing_period_ms必须为正数，output_frame_id不能为空");
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
      std::chrono::milliseconds(processing_period_ms),
      std::bind(&EnuCloudTransformNode::processPendingClouds, this),
      processing_callback_group_, get_node_base_interface().get(),
      get_node_timers_interface().get());

    RCLCPP_INFO(
      get_logger(),
      "ENU点云转换已启动：cloud=%s odom=%s output=%s frame=%s；逐点旋转和平移补偿已启用",
      input_topic_.c_str(), odometry_topic_.c_str(), output_topic_.c_str(),
      output_frame_id_.c_str());
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
    if (!transformer_->addPose(sample)) {
      RCLCPP_WARN_THROTTLE(
        get_logger(), *get_clock(), 5000, "拒绝无效或乱序的高频里程计姿态");
    }
  }

  void cloudCallback(const sensor_msgs::msg::PointCloud2::ConstSharedPtr message)
  {
    sensor_msgs::msg::PointCloud2::ConstSharedPtr dropped;
    {
      std::lock_guard<std::mutex> lock(pending_clouds_mutex_);
      if (pending_clouds_.size() >= pending_cloud_limit_) {
        dropped = pending_clouds_.front();
        pending_clouds_.pop_front();
      }
      pending_clouds_.push_back(message);
    }
    if (dropped) {
      RCLCPP_WARN_THROTTLE(
        get_logger(), *get_clock(), 5000,
        "ENU点云等待队列已满，丢弃最旧点云但不发布空点云");
      if (publish_empty_on_failure_) {
        publishEmptyCloud(*dropped);
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
    while (!pending_clouds_.empty() && transformer_->initialized()) {
      const auto message = pending_clouds_.front();
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
        pending_clouds_.pop_front();
        continue;
      }

      const std::int64_t newest_pose_stamp_ns = transformer_->newestPoseStampNs();
      if (newest_pose_stamp_ns < last_point_stamp_ns &&
        last_point_stamp_ns - newest_pose_stamp_ns > allowed_partial_tail_ns_)
      {
        return;
      }

      std::vector<EnuPoint> enu_points;
      std::string invalid_reason;
      TransformStatistics statistics;
      const bool fully_qualified = transformer_->transform(
        cloud_stamp_ns, raw_points, enu_points, invalid_reason, &statistics);
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
        pending_clouds_.pop_front();
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
      pending_clouds_.pop_front();
    }
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
  std::size_t pending_cloud_limit_{5U};
  std::int64_t allowed_partial_tail_ns_{15000000LL};
  std::int64_t odometry_time_offset_ns_{0};
  std::int64_t cloud_time_offset_ns_{0};
  bool publish_partial_cloud_{false};
  bool publish_empty_on_failure_{false};

  std::mutex pending_clouds_mutex_;
  std::deque<sensor_msgs::msg::PointCloud2::ConstSharedPtr> pending_clouds_;
  rclcpp::CallbackGroup::SharedPtr odometry_callback_group_;
  rclcpp::CallbackGroup::SharedPtr cloud_callback_group_;
  rclcpp::CallbackGroup::SharedPtr processing_callback_group_;
  rclcpp::Subscription<nav_msgs::msg::Odometry>::SharedPtr odometry_subscription_;
  rclcpp::Subscription<sensor_msgs::msg::PointCloud2>::SharedPtr cloud_subscription_;
  rclcpp::Publisher<sensor_msgs::msg::PointCloud2>::SharedPtr output_publisher_;
  rclcpp::TimerBase::SharedPtr processing_timer_;
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

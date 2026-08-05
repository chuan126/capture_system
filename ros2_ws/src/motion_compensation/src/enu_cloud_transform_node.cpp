#include "motion_compensation/enu_cloud_transformer.hpp"

#include <builtin_interfaces/msg/time.hpp>
#include <nav_msgs/msg/odometry.hpp>
#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/point_cloud2.hpp>
#include <sensor_msgs/point_cloud2_iterator.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
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

RotationMatrix3d vectorToRotationMatrix(const std::vector<double> & values)
{
  if (values.size() != 9U) {
    throw std::invalid_argument("lidar_to_odometry_rotation必须包含9个按行排列的数值");
  }
  RotationMatrix3d rotation{};
  std::copy(values.begin(), values.end(), rotation.begin());
  return rotation;
}

}  // namespace

class EnuCloudTransformNode : public rclcpp::Node
{
public:
  EnuCloudTransformNode()
  : Node("enu_cloud_transform_node")
  {
    // const auto lidar_to_odometry_rotation = vectorToRotationMatrix(
    //   declare_parameter<std::vector<double>>(
    //     "lidar_to_odometry_rotation",
    //     std::vector<double>{-1.0, 0.0, 0.0, 0.0, -1.0, 0.0, 0.0, 0.0, 1.0}));

    
    const auto lidar_to_odometry_rotation = vectorToRotationMatrix(
      declare_parameter<std::vector<double>>(
        "lidar_to_odometry_rotation",
        std::vector<double>{
          1.0, 0.0, 0.0,
          0.0, 1.0, 0.0,
          0.0, 0.0, 1.0
        }));

    transformer_ = std::make_unique<EnuCloudTransformer>(
      secondsToNanoseconds(declare_parameter<double>("pose_cache_duration_s", 2.0)),
      secondsToNanoseconds(declare_parameter<double>("max_interpolation_gap_s", 0.02)),
      declare_parameter<bool>("use_odometry_translation", false),
      lidar_to_odometry_rotation);

    input_topic_ = declare_parameter<std::string>(
      "input_cloud_topic", "/capture/lidar/points_raw");
    odometry_topic_ = declare_parameter<std::string>(
      "odometry_topic", "/capture/odometry/high_rate");
    output_topic_ = declare_parameter<std::string>(
      "output_cloud_topic", "/capture/lidar/points_compensated_enu");
    output_frame_id_ = declare_parameter<std::string>("output_frame_id", "lidar_local_enu");
    const int pending_cloud_limit = declare_parameter<int>("pending_cloud_limit", 2);
    if (pending_cloud_limit <= 0 || output_frame_id_.empty()) {
      throw std::invalid_argument("pending_cloud_limit必须为正数且output_frame_id不能为空");
    }
    pending_cloud_limit_ = static_cast<std::size_t>(pending_cloud_limit);

    output_publisher_ = create_publisher<sensor_msgs::msg::PointCloud2>(
      output_topic_, rclcpp::QoS(rclcpp::KeepLast(2)).reliable().durability_volatile());
    odometry_subscription_ = create_subscription<nav_msgs::msg::Odometry>(
      odometry_topic_, rclcpp::QoS(rclcpp::KeepLast(1000)).best_effort().durability_volatile(),
      std::bind(&EnuCloudTransformNode::odometryCallback, this, std::placeholders::_1));
    cloud_subscription_ = create_subscription<sensor_msgs::msg::PointCloud2>(
      input_topic_, rclcpp::QoS(rclcpp::KeepLast(2)).reliable().durability_volatile(),
      std::bind(&EnuCloudTransformNode::cloudCallback, this, std::placeholders::_1));

    RCLCPP_INFO(
      get_logger(),
      "ENU点云转换已启动：cloud=%s odom=%s output=%s frame=%s；天向仅由里程计四元数确定",
      input_topic_.c_str(), odometry_topic_.c_str(), output_topic_.c_str(),
      output_frame_id_.c_str());
  }

private:
  static std::int64_t secondsToNanoseconds(const double seconds)
  {
    if (!(seconds > 0.0)) {
      throw std::invalid_argument("时间参数必须为正数");
    }
    return static_cast<std::int64_t>(seconds * 1.0e9);
  }

  void odometryCallback(const nav_msgs::msg::Odometry::ConstSharedPtr message)
  {
    PoseSample sample;
    sample.stamp_ns = stampToNanoseconds(message->header.stamp);
    sample.position_m = {
      message->pose.pose.position.x, message->pose.pose.position.y,
      message->pose.pose.position.z};
    sample.quaternion_xyzw = {
      message->pose.pose.orientation.x, message->pose.pose.orientation.y,
      message->pose.pose.orientation.z, message->pose.pose.orientation.w};
    if (!transformer_->addPose(sample)) {
      RCLCPP_WARN_THROTTLE(
        get_logger(), *get_clock(), 5000, "拒绝无效或乱序的高频里程计姿态");
      return;
    }
    processPendingClouds();
  }

  void cloudCallback(const sensor_msgs::msg::PointCloud2::ConstSharedPtr message)
  {
    if (pending_clouds_.size() >= pending_cloud_limit_) {
      RCLCPP_WARN_THROTTLE(
        get_logger(), *get_clock(), 5000, "ENU点云等待队列已满，丢弃最旧点云");
      publishEmptyCloud(*pending_clouds_.front());
      pending_clouds_.pop_front();
    }
    pending_clouds_.push_back(message);
    processPendingClouds();
  }

  bool readPoints(
    const sensor_msgs::msg::PointCloud2 & message, std::vector<TimedRadarPoint> & points,
    std::int64_t & last_point_stamp_ns) const
  {
    if (message.is_bigendian || !hasFloat32Field(message, "x") ||
      !hasFloat32Field(message, "y") || !hasFloat32Field(message, "z") ||
      !hasFloat32Field(message, "offset_time") || message.point_step == 0U ||
      message.data.size() < static_cast<std::size_t>(message.row_step) * message.height)
    {
      return false;
    }
    const std::int64_t cloud_stamp_ns = stampToNanoseconds(message.header.stamp);
    float maximum_offset_s = 0.0F;
    points.clear();
    points.reserve(static_cast<std::size_t>(message.width) * message.height);
    try {
      sensor_msgs::PointCloud2ConstIterator<float> x(message, "x");
      sensor_msgs::PointCloud2ConstIterator<float> y(message, "y");
      sensor_msgs::PointCloud2ConstIterator<float> z(message, "z");
      sensor_msgs::PointCloud2ConstIterator<float> offset(message, "offset_time");
      for (; x != x.end(); ++x, ++y, ++z, ++offset) {
        if (!std::isfinite(*offset) || *offset < 0.0F) {
          return false;
        }
        points.push_back(TimedRadarPoint{*x, *y, *z, *offset});
        maximum_offset_s = std::max(maximum_offset_s, *offset);
      }
    } catch (const std::runtime_error &) {
      return false;
    }
    last_point_stamp_ns = cloud_stamp_ns + static_cast<std::int64_t>(
      static_cast<double>(maximum_offset_s) * 1.0e9);
    return true;
  }

  void processPendingClouds()
  {
    while (!pending_clouds_.empty() && transformer_->initialized()) {
      const auto message = pending_clouds_.front();
      std::vector<TimedRadarPoint> raw_points;
      std::int64_t last_point_stamp_ns = 0;
      if (!readPoints(*message, raw_points, last_point_stamp_ns)) {
        RCLCPP_WARN_THROTTLE(
          get_logger(), *get_clock(), 5000, "点云缺少FLOAT32 xyz/offset_time字段");
        publishEmptyCloud(*message);
        pending_clouds_.pop_front();
        continue;
      }
      const std::int64_t cloud_stamp_ns = stampToNanoseconds(message->header.stamp);
      if (transformer_->newestPoseStampNs() < last_point_stamp_ns) {
        return;
      }
      if (transformer_->oldestPoseStampNs() > cloud_stamp_ns) {
        RCLCPP_WARN_THROTTLE(
          get_logger(), *get_clock(), 5000, "点云起始时间早于姿态缓存，无法补偿");
        publishEmptyCloud(*message);
        pending_clouds_.pop_front();
        continue;
      }

      std::vector<EnuPoint> enu_points;
      std::string invalid_reason;
      if (!transformer_->transform(cloud_stamp_ns, raw_points, enu_points, invalid_reason)) {
        RCLCPP_WARN_THROTTLE(
          get_logger(), *get_clock(), 5000, "ENU点云转换失败：%s", invalid_reason.c_str());
        publishEmptyCloud(*message);
        pending_clouds_.pop_front();
        continue;
      }

      sensor_msgs::msg::PointCloud2 output;
      output.header = message->header;
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
      pending_clouds_.pop_front();
    }
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
  std::size_t pending_cloud_limit_{2U};
  std::deque<sensor_msgs::msg::PointCloud2::ConstSharedPtr> pending_clouds_;
  rclcpp::Subscription<nav_msgs::msg::Odometry>::SharedPtr odometry_subscription_;
  rclcpp::Subscription<sensor_msgs::msg::PointCloud2>::SharedPtr cloud_subscription_;
  rclcpp::Publisher<sensor_msgs::msg::PointCloud2>::SharedPtr output_publisher_;
};

}  // namespace motion_compensation

int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);
  try {
    rclcpp::spin(std::make_shared<motion_compensation::EnuCloudTransformNode>());
  } catch (const std::exception & error) {
    RCLCPP_FATAL(rclcpp::get_logger("enu_cloud_transform_node"), "%s", error.what());
    rclcpp::shutdown();
    return 1;
  }
  rclcpp::shutdown();
  return 0;
}

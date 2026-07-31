#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>

#include "cloud_visualization/cloud_preview_converter.hpp"
#include "rclcpp/rclcpp.hpp"
#include "sensor_msgs/msg/point_cloud2.hpp"

namespace cloud_visualization
{

class CloudVisualizationNode final : public rclcpp::Node
{
public:
  CloudVisualizationNode()
  : Node("cloud_visualization_node")
  {
    enabled_ = declare_parameter<bool>("enabled", true);
    input_topic_ = declare_parameter<std::string>(
      "input_topic", "/capture/lidar/points_slam");
    output_topic_ = declare_parameter<std::string>(
      "output_topic", "/capture/visualization/cloud_preview");
    publish_rate_hz_ = declare_parameter<double>("publish_rate_hz", 5.0);
    const auto configured_max_points = declare_parameter<std::int64_t>("max_points", 10000);

    validate_parameters(configured_max_points);
    max_points_ = static_cast<std::size_t>(configured_max_points);

    const auto preview_qos =
      rclcpp::QoS(rclcpp::KeepLast(1)).best_effort().durability_volatile();

    publisher_ = create_publisher<sensor_msgs::msg::PointCloud2>(
      output_topic_, preview_qos);
    subscription_ = create_subscription<sensor_msgs::msg::PointCloud2>(
      input_topic_,
      preview_qos,
      std::bind(&CloudVisualizationNode::on_cloud, this, std::placeholders::_1));

    const auto timer_period = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::duration<double>(1.0 / publish_rate_hz_));
    timer_ = create_wall_timer(
      timer_period,
      std::bind(&CloudVisualizationNode::publish_latest_cloud, this));

    RCLCPP_INFO(
      get_logger(),
      "SLAM点云预览已启动：输入=%s，输出=%s，频率=%.2f Hz，最大点数=%zu，启用=%s",
      input_topic_.c_str(),
      output_topic_.c_str(),
      publish_rate_hz_,
      max_points_,
      enabled_ ? "是" : "否");
  }

private:
  void validate_parameters(const std::int64_t configured_max_points) const
  {
    if (input_topic_.empty()) {
      throw std::invalid_argument("参数input_topic不能为空");
    }
    if (output_topic_.empty()) {
      throw std::invalid_argument("参数output_topic不能为空");
    }
    if (publish_rate_hz_ < 1.0 || publish_rate_hz_ > 10.0) {
      throw std::invalid_argument("参数publish_rate_hz必须位于[1.0, 10.0] Hz");
    }
    if (configured_max_points < 500 || configured_max_points > 20000) {
      throw std::invalid_argument("参数max_points必须位于[500, 20000]");
    }
  }

  void on_cloud(const sensor_msgs::msg::PointCloud2::ConstSharedPtr message)
  {
    // 回调只替换共享指针，避免约10 Hz输入链路同步执行整帧复制。
    latest_cloud_ = message;
    ++received_sequence_;
  }

  void publish_latest_cloud()
  {
    if (!enabled_ || !latest_cloud_ || published_sequence_ == received_sequence_) {
      return;
    }

    const auto cloud = latest_cloud_;
    published_sequence_ = received_sequence_;

    // 没有消费者时仍把当前帧标记为已处理，避免订阅者稍后接入时发布陈旧点云。
    if (publisher_->get_subscription_count() == 0U &&
      publisher_->get_intra_process_subscription_count() == 0U)
    {
      return;
    }

    publisher_->publish(converter_.convert(*cloud, max_points_));
  }

  bool enabled_{true};
  std::string input_topic_;
  std::string output_topic_;
  double publish_rate_hz_{5.0};
  std::size_t max_points_{10000U};

  CloudPreviewConverter converter_;
  sensor_msgs::msg::PointCloud2::ConstSharedPtr latest_cloud_;
  std::uint64_t received_sequence_{0U};
  std::uint64_t published_sequence_{0U};

  rclcpp::Publisher<sensor_msgs::msg::PointCloud2>::SharedPtr publisher_;
  rclcpp::Subscription<sensor_msgs::msg::PointCloud2>::SharedPtr subscription_;
  rclcpp::TimerBase::SharedPtr timer_;
};

}  // 结束cloud_visualization命名空间

int main(int argc, char * argv[])
{
  rclcpp::init(argc, argv);

  try {
    // 首版明确使用单线程执行器，最新帧指针无需跨回调加锁。
    rclcpp::spin(std::make_shared<cloud_visualization::CloudVisualizationNode>());
  } catch (const std::exception & exception) {
    RCLCPP_FATAL(
      rclcpp::get_logger("cloud_visualization_node"),
      "SLAM点云预览节点启动或运行失败：%s",
      exception.what());
    rclcpp::shutdown();
    return 1;
  }

  rclcpp::shutdown();
  return 0;
}

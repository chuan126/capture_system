#ifndef CLOUD_VISUALIZATION__CLOUD_PREVIEW_CONVERTER_HPP_
#define CLOUD_VISUALIZATION__CLOUD_PREVIEW_CONVERTER_HPP_

#include <cstddef>

#include "sensor_msgs/msg/point_cloud2.hpp"

namespace cloud_visualization
{

/**
 * @brief 将带标准XYZ FLOAT32字段的PointCloud2转换为紧凑XYZ预览点云。
 *
 * 支持当前原始点云18字节和SLAM点云16字节点步长，并检查字段与负载边界。
 */
class CloudPreviewConverter
{
public:
  /**
   * @brief 裁减RGB，并在需要时对完整扫描序列做确定性等间隔限点。
   *
   * @param input Little Endian且包含x/y/z FLOAT32字段的点云。
   * @param max_points 输出允许的最大点数，必须大于0。
   * @return 固定为xyz FLOAT32、12字节点步长的预览点云。
   */
  sensor_msgs::msg::PointCloud2 convert(
    const sensor_msgs::msg::PointCloud2 & input,
    std::size_t max_points) const;
};

}  // 结束cloud_visualization命名空间

#endif

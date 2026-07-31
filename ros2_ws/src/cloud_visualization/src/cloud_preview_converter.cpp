#include "cloud_visualization/cloud_preview_converter.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "sensor_msgs/msg/point_field.hpp"

namespace cloud_visualization
{
namespace
{

// 首版直接依赖2026-07-31实测确认的ODIN SLAM点云二进制布局。
constexpr std::size_t kInputPointStep = 16U;
constexpr std::size_t kOutputPointStep = 12U;

sensor_msgs::msg::PointField make_xyz_field(const char * name, const std::uint32_t offset)
{
  sensor_msgs::msg::PointField field;
  field.name = name;
  field.offset = offset;
  field.datatype = sensor_msgs::msg::PointField::FLOAT32;
  field.count = 1U;
  return field;
}

}  // 结束匿名命名空间

sensor_msgs::msg::PointCloud2 CloudPreviewConverter::convert(
  const sensor_msgs::msg::PointCloud2 & input,
  const std::size_t max_points) const
{
  const std::size_t input_point_count = input.width;
  const std::size_t output_point_count = std::min(input_point_count, max_points);

  sensor_msgs::msg::PointCloud2 output;
  output.header = input.header;
  output.height = 1U;
  output.width = static_cast<std::uint32_t>(output_point_count);
  output.fields = {
    make_xyz_field("x", 0U),
    make_xyz_field("y", 4U),
    make_xyz_field("z", 8U),
  };
  output.is_bigendian = false;
  output.point_step = static_cast<std::uint32_t>(kOutputPointStep);
  output.row_step = output.width * output.point_step;
  output.is_dense = input.is_dense;
  output.data.resize(output.row_step);

  for (std::size_t output_index = 0U; output_index < output_point_count; ++output_index) {
    // 使用64位乘法避免点数相乘溢出；相同输入始终选择相同的扫描序列位置。
    const auto scaled_index =
      static_cast<std::uint64_t>(output_index) * static_cast<std::uint64_t>(input_point_count);
    const std::size_t input_index =
      static_cast<std::size_t>(scaled_index / output_point_count);

    const std::uint8_t * input_point = input.data.data() + input_index * kInputPointStep;
    std::uint8_t * output_point = output.data.data() + output_index * kOutputPointStep;

    // XYZ位于每个16字节输入点的前12字节，直接复制可避免不必要的浮点解析。
    std::memcpy(output_point, input_point, kOutputPointStep);
  }

  return output;
}

}  // 结束cloud_visualization命名空间

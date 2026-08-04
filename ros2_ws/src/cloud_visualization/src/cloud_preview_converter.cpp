#include "cloud_visualization/cloud_preview_converter.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

#include "sensor_msgs/msg/point_field.hpp"

namespace cloud_visualization
{
namespace
{

constexpr std::size_t kOutputPointStep = 12U;

std::uint32_t find_float32_field_offset(
  const sensor_msgs::msg::PointCloud2 & input,
  const std::string & name)
{
  const auto field = std::find_if(
    input.fields.begin(), input.fields.end(),
    [&name](const sensor_msgs::msg::PointField & candidate) {
      return candidate.name == name;
    });
  if (field == input.fields.end() ||
    field->datatype != sensor_msgs::msg::PointField::FLOAT32 || field->count != 1U)
  {
    throw std::invalid_argument("输入点云缺少FLOAT32类型的" + name + "字段");
  }
  if (static_cast<std::size_t>(field->offset) + sizeof(float) > input.point_step) {
    throw std::invalid_argument("输入点云" + name + "字段超出point_step");
  }
  return field->offset;
}

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
  if (max_points == 0U) {
    throw std::invalid_argument("max_points必须大于0");
  }
  if (input.is_bigendian) {
    throw std::invalid_argument("预览暂不支持大端PointCloud2");
  }

  const auto x_offset = find_float32_field_offset(input, "x");
  const auto y_offset = find_float32_field_offset(input, "y");
  const auto z_offset = find_float32_field_offset(input, "z");
  const std::size_t input_point_count =
    static_cast<std::size_t>(input.width) * static_cast<std::size_t>(input.height);
  const std::size_t minimum_data_size =
    input.height == 0U ? 0U :
    (static_cast<std::size_t>(input.height) - 1U) * input.row_step +
    static_cast<std::size_t>(input.width) * input.point_step;
  if (input.data.size() < minimum_data_size) {
    throw std::invalid_argument("输入点云data长度小于布局声明");
  }
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

    const std::size_t input_row = input_index / input.width;
    const std::size_t input_column = input_index % input.width;
    const std::uint8_t * input_point = input.data.data() +
      input_row * input.row_step + input_column * input.point_step;
    std::uint8_t * output_point = output.data.data() + output_index * kOutputPointStep;

    // 原始点为18字节、SLAM点为16字节；按字段偏移复制以免跨点读取错误数据。
    std::memcpy(output_point, input_point + x_offset, sizeof(float));
    std::memcpy(output_point + sizeof(float), input_point + y_offset, sizeof(float));
    std::memcpy(output_point + 2U * sizeof(float), input_point + z_offset, sizeof(float));
  }

  return output;
}

}  // 结束cloud_visualization命名空间

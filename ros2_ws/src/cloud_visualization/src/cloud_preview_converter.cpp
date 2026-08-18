#include "cloud_visualization/cloud_preview_converter.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

#include "sensor_msgs/msg/point_field.hpp"

namespace cloud_visualization
{
namespace
{

constexpr std::size_t kOutputPointStep = 12U;

struct PreviewPoint
{
  std::size_t input_index;
  float x;
  float y;
  float z;
};

struct VoxelKey
{
  std::int64_t x;
  std::int64_t y;
  std::int64_t z;

  bool operator==(const VoxelKey & other) const
  {
    return x == other.x && y == other.y && z == other.z;
  }
};

struct VoxelKeyHash
{
  std::size_t operator()(const VoxelKey & key) const
  {
    std::size_t seed = std::hash<std::int64_t>{}(key.x);
    seed ^= std::hash<std::int64_t>{}(key.y) + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
    seed ^= std::hash<std::int64_t>{}(key.z) + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
    return seed;
  }
};

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

float read_float(const std::uint8_t * point, const std::uint32_t offset)
{
  float value = 0.0F;
  std::memcpy(&value, point + offset, sizeof(float));
  return value;
}

bool is_preview_point(const PreviewPoint & point)
{
  if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z)) {
    return false;
  }
  return point.x != 0.0F || point.y != 0.0F || point.z != 0.0F;
}

VoxelKey voxel_key(const PreviewPoint & point, const double voxel_size_m)
{
  return VoxelKey{
    static_cast<std::int64_t>(std::floor(static_cast<double>(point.x) / voxel_size_m)),
    static_cast<std::int64_t>(std::floor(static_cast<double>(point.y) / voxel_size_m)),
    static_cast<std::int64_t>(std::floor(static_cast<double>(point.z) / voxel_size_m)),
  };
}

std::vector<PreviewPoint> evenly_limit(
  const std::vector<PreviewPoint> & points,
  const std::size_t max_points)
{
  if (points.size() <= max_points) {
    return points;
  }

  std::vector<PreviewPoint> output;
  output.reserve(max_points);
  for (std::size_t output_index = 0U; output_index < max_points; ++output_index) {
    const auto scaled_index =
      static_cast<std::uint64_t>(output_index) * static_cast<std::uint64_t>(points.size());
    const std::size_t point_index = static_cast<std::size_t>(scaled_index / max_points);
    output.push_back(points[point_index]);
  }
  return output;
}

std::vector<PreviewPoint> spatial_limit(
  const std::vector<PreviewPoint> & points,
  const std::size_t max_points,
  const double voxel_size_m)
{
  if (points.size() <= max_points) {
    return points;
  }

  std::unordered_set<VoxelKey, VoxelKeyHash> occupied;
  occupied.reserve(std::min(points.size(), max_points * 2U));
  std::vector<PreviewPoint> voxel_representatives;
  voxel_representatives.reserve(std::min(points.size(), max_points * 2U));
  std::unordered_set<std::size_t> selected_indices;
  selected_indices.reserve(max_points * 2U);

  for (const auto & point : points) {
    if (occupied.insert(voxel_key(point, voxel_size_m)).second) {
      voxel_representatives.push_back(point);
      selected_indices.insert(point.input_index);
    }
  }

  if (voxel_representatives.size() >= max_points) {
    return evenly_limit(voxel_representatives, max_points);
  }

  // 体素代表点不足上限时，用全体有效点按扫描位置均匀补足，兼顾空间覆盖和视觉密度。
  std::vector<PreviewPoint> selected = voxel_representatives;
  selected.reserve(max_points);
  const std::size_t remaining_capacity = max_points - selected.size();
  const std::size_t candidate_count = points.size();
  for (std::size_t slot = 0U; slot < remaining_capacity && selected.size() < max_points; ++slot) {
    const auto scaled_index =
      static_cast<std::uint64_t>(slot) * static_cast<std::uint64_t>(candidate_count);
    std::size_t candidate_index = static_cast<std::size_t>(scaled_index / remaining_capacity);
    while (candidate_index < candidate_count &&
      selected_indices.find(points[candidate_index].input_index) != selected_indices.end())
    {
      ++candidate_index;
    }
    if (candidate_index >= candidate_count) {
      candidate_index = 0U;
      while (candidate_index < candidate_count &&
        selected_indices.find(points[candidate_index].input_index) != selected_indices.end())
      {
        ++candidate_index;
      }
    }
    if (candidate_index >= candidate_count) {
      break;
    }
    selected.push_back(points[candidate_index]);
    selected_indices.insert(points[candidate_index].input_index);
  }

  if (selected.size() < max_points) {
    for (const auto & point : points) {
      if (selected.size() >= max_points) {
        break;
      }
      if (selected_indices.insert(point.input_index).second) {
        selected.push_back(point);
      }
    }
  }

  std::sort(
    selected.begin(), selected.end(),
    [](const PreviewPoint & left, const PreviewPoint & right) {
      return left.input_index < right.input_index;
    });
  return selected;
}

}  // 结束匿名命名空间

sensor_msgs::msg::PointCloud2 CloudPreviewConverter::convert(
  const sensor_msgs::msg::PointCloud2 & input,
  const std::size_t max_points,
  const double voxel_size_m) const
{
  if (max_points == 0U) {
    throw std::invalid_argument("max_points必须大于0");
  }
  if (!std::isfinite(voxel_size_m) || voxel_size_m <= 0.0) {
    throw std::invalid_argument("voxel_size_m必须为有限正数");
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

  std::vector<PreviewPoint> valid_points;
  valid_points.reserve(input_point_count);
  for (std::size_t input_index = 0U; input_index < input_point_count; ++input_index) {
    const std::size_t input_row = input.width == 0U ? 0U : input_index / input.width;
    const std::size_t input_column = input.width == 0U ? 0U : input_index % input.width;
    const std::uint8_t * input_point = input.data.data() +
      input_row * input.row_step + input_column * input.point_step;
    const PreviewPoint point{
      input_index,
      read_float(input_point, x_offset),
      read_float(input_point, y_offset),
      read_float(input_point, z_offset),
    };
    if (is_preview_point(point)) {
      valid_points.push_back(point);
    }
  }

  const auto selected_points = spatial_limit(valid_points, max_points, voxel_size_m);

  sensor_msgs::msg::PointCloud2 output;
  output.header = input.header;
  output.height = 1U;
  output.width = static_cast<std::uint32_t>(selected_points.size());
  output.fields = {
    make_xyz_field("x", 0U),
    make_xyz_field("y", 4U),
    make_xyz_field("z", 8U),
  };
  output.is_bigendian = false;
  output.point_step = static_cast<std::uint32_t>(kOutputPointStep);
  output.row_step = output.width * output.point_step;
  output.is_dense = true;
  output.data.resize(output.row_step);

  for (std::size_t output_index = 0U; output_index < selected_points.size(); ++output_index) {
    const auto & point = selected_points[output_index];
    std::uint8_t * output_point = output.data.data() + output_index * kOutputPointStep;
    std::memcpy(output_point, &point.x, sizeof(float));
    std::memcpy(output_point + sizeof(float), &point.y, sizeof(float));
    std::memcpy(output_point + 2U * sizeof(float), &point.z, sizeof(float));
  }

  return output;
}

}  // 结束cloud_visualization命名空间

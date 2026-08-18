#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

#include "cloud_visualization/cloud_preview_converter.hpp"
#include "gtest/gtest.h"
#include "sensor_msgs/msg/point_cloud2.hpp"
#include "sensor_msgs/msg/point_field.hpp"

namespace cloud_visualization
{
namespace
{

sensor_msgs::msg::PointCloud2 make_fixed_layout_cloud(
  const std::size_t point_count,
  const bool is_dense = true,
  const std::uint32_t point_step = 16U)
{
  sensor_msgs::msg::PointCloud2 cloud;
  cloud.header.stamp.sec = 123;
  cloud.header.stamp.nanosec = 456U;
  cloud.header.frame_id = "device0/odom";
  cloud.height = 1U;
  cloud.width = static_cast<std::uint32_t>(point_count);
  cloud.fields.resize(4U);

  const std::array<const char *, 4U> names{"x", "y", "z", "rgb"};
  for (std::size_t field_index = 0U; field_index < names.size(); ++field_index) {
    auto & field = cloud.fields[field_index];
    field.name = names[field_index];
    field.offset = static_cast<std::uint32_t>(field_index * sizeof(float));
    field.datatype = sensor_msgs::msg::PointField::FLOAT32;
    field.count = 1U;
  }

  cloud.is_bigendian = false;
  cloud.point_step = point_step;
  cloud.row_step = cloud.width * cloud.point_step;
  cloud.is_dense = is_dense;
  cloud.data.resize(cloud.row_step);

  for (std::size_t point_index = 0U; point_index < point_count; ++point_index) {
    const std::array<float, 4U> values{
      static_cast<float>(point_index),
      static_cast<float>(point_index) + 0.25F,
      -static_cast<float>(point_index),
      1000.0F + static_cast<float>(point_index),
    };
    std::memcpy(
      cloud.data.data() + point_index * cloud.point_step,
      values.data(),
      std::min<std::size_t>(values.size() * sizeof(float), cloud.point_step));
  }

  return cloud;
}

std::array<float, 3U> read_xyz(
  const sensor_msgs::msg::PointCloud2 & cloud,
  const std::size_t point_index)
{
  std::array<float, 3U> xyz{};
  std::memcpy(
    xyz.data(),
    cloud.data.data() + point_index * cloud.point_step,
    cloud.point_step);
  return xyz;
}

void write_xyz(
  sensor_msgs::msg::PointCloud2 & cloud,
  const std::size_t point_index,
  const std::array<float, 3U> & xyz)
{
  std::memcpy(
    cloud.data.data() + point_index * cloud.point_step,
    xyz.data(),
    xyz.size() * sizeof(float));
}

TEST(CloudPreviewConverterTest, PreservesAllPointsBelowLimitAndRemovesRgb)
{
  const auto input = make_fixed_layout_cloud(3U, false);
  const auto output = CloudPreviewConverter{}.convert(input, 10U);

  EXPECT_EQ(output.header.stamp, input.header.stamp);
  EXPECT_EQ(output.header.frame_id, input.header.frame_id);
  EXPECT_EQ(output.height, 1U);
  EXPECT_EQ(output.width, 3U);
  ASSERT_EQ(output.fields.size(), 3U);
  EXPECT_EQ(output.fields[0].name, "x");
  EXPECT_EQ(output.fields[0].offset, 0U);
  EXPECT_EQ(output.fields[1].name, "y");
  EXPECT_EQ(output.fields[1].offset, 4U);
  EXPECT_EQ(output.fields[2].name, "z");
  EXPECT_EQ(output.fields[2].offset, 8U);
  EXPECT_EQ(output.point_step, 12U);
  EXPECT_EQ(output.row_step, 36U);
  EXPECT_FALSE(output.is_bigendian);
  EXPECT_TRUE(output.is_dense);
  EXPECT_EQ(output.data.size(), 36U);

  EXPECT_EQ(read_xyz(output, 0U), (std::array<float, 3U>{0.0F, 0.25F, 0.0F}));
  EXPECT_EQ(read_xyz(output, 2U), (std::array<float, 3U>{2.0F, 2.25F, -2.0F}));
}

TEST(CloudPreviewConverterTest, FiltersZeroPlaceholdersAndNonFinitePointsBeforeLimiting)
{
  auto input = make_fixed_layout_cloud(6U, false);
  write_xyz(input, 0U, {0.0F, 0.0F, 0.0F});
  write_xyz(input, 1U, {std::numeric_limits<float>::quiet_NaN(), 1.0F, 1.0F});
  write_xyz(input, 2U, {2.0F, 2.0F, 2.0F});
  write_xyz(input, 3U, {0.0F, 0.0F, 0.0F});
  write_xyz(input, 4U, {4.0F, 4.0F, 4.0F});
  write_xyz(input, 5U, {5.0F, 5.0F, 5.0F});

  const auto output = CloudPreviewConverter{}.convert(input, 10U);

  ASSERT_EQ(output.width, 3U);
  EXPECT_TRUE(output.is_dense);
  EXPECT_EQ(read_xyz(output, 0U), (std::array<float, 3U>{2.0F, 2.0F, 2.0F}));
  EXPECT_EQ(read_xyz(output, 1U), (std::array<float, 3U>{4.0F, 4.0F, 4.0F}));
  EXPECT_EQ(read_xyz(output, 2U), (std::array<float, 3U>{5.0F, 5.0F, 5.0F}));
}

TEST(CloudPreviewConverterTest, KeepsConfiguredVisualDensityAfterVoxelSelection)
{
  auto input = make_fixed_layout_cloud(8U);
  for (std::size_t index = 0U; index < 8U; ++index) {
    const float coordinate = 0.001F * static_cast<float>(index + 1U);
    write_xyz(input, index, {coordinate, coordinate, coordinate});
  }

  const auto output = CloudPreviewConverter{}.convert(input, 4U, 0.05);

  ASSERT_EQ(output.width, 4U);
  for (std::size_t index = 0U; index < output.width; ++index) {
    const auto xyz = read_xyz(output, index);
    EXPECT_TRUE(std::isfinite(xyz[0]));
    EXPECT_TRUE(xyz[0] != 0.0F || xyz[1] != 0.0F || xyz[2] != 0.0F);
  }
}

TEST(CloudPreviewConverterTest, PreservesPointCountAtExactLimit)
{
  const auto input = make_fixed_layout_cloud(4U);
  const auto output = CloudPreviewConverter{}.convert(input, 4U);

  EXPECT_EQ(output.width, 4U);
  EXPECT_EQ(read_xyz(output, 3U), (std::array<float, 3U>{3.0F, 3.25F, -3.0F}));
}

TEST(CloudPreviewConverterTest, SelectsDeterministicEvenlySpacedPointsAboveLimit)
{
  const auto input = make_fixed_layout_cloud(5U);
  const CloudPreviewConverter converter;

  const auto first_output = converter.convert(input, 3U);
  const auto second_output = converter.convert(input, 3U);

  ASSERT_EQ(first_output.width, 3U);
  EXPECT_EQ(read_xyz(first_output, 0U), (std::array<float, 3U>{0.0F, 0.25F, 0.0F}));
  EXPECT_EQ(read_xyz(first_output, 1U), (std::array<float, 3U>{1.0F, 1.25F, -1.0F}));
  EXPECT_EQ(read_xyz(first_output, 2U), (std::array<float, 3U>{3.0F, 3.25F, -3.0F}));
  EXPECT_EQ(first_output.data, second_output.data);
}

TEST(CloudPreviewConverterTest, ProducesEmptyOutputForEmptyFixedLayoutCloud)
{
  const auto input = make_fixed_layout_cloud(0U);
  const auto output = CloudPreviewConverter{}.convert(input, 10000U);

  EXPECT_EQ(output.width, 0U);
  EXPECT_EQ(output.row_step, 0U);
  EXPECT_TRUE(output.data.empty());
}

TEST(CloudPreviewConverterTest, ReadsRawPointCloudWithEighteenBytePointStep)
{
  const auto input = make_fixed_layout_cloud(3U, true, 18U);
  const auto output = CloudPreviewConverter{}.convert(input, 10U);

  ASSERT_EQ(output.width, 3U);
  EXPECT_EQ(read_xyz(output, 1U), (std::array<float, 3U>{1.0F, 1.25F, -1.0F}));
  EXPECT_EQ(read_xyz(output, 2U), (std::array<float, 3U>{2.0F, 2.25F, -2.0F}));
}

TEST(CloudPreviewConverterTest, RejectsMissingCoordinateField)
{
  auto input = make_fixed_layout_cloud(1U);
  input.fields.erase(input.fields.begin() + 2);

  EXPECT_THROW(CloudPreviewConverter{}.convert(input, 10U), std::invalid_argument);
}

}  // 结束匿名命名空间
}  // 结束cloud_visualization命名空间

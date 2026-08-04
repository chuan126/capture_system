#include "motion_compensation/enu_cloud_transformer.hpp"

#include "localization/attitude_transform.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <stdexcept>

namespace motion_compensation
{
namespace
{

bool normalizeQuaternion(std::array<double, 4> & quaternion) noexcept
{
  double squared_norm = 0.0;
  for (const double value : quaternion) {
    if (!std::isfinite(value)) {
      return false;
    }
    squared_norm += value * value;
  }
  const double norm = std::sqrt(squared_norm);
  if (!std::isfinite(norm) || norm <= 1.0e-12) {
    return false;
  }
  for (double & value : quaternion) {
    value /= norm;
  }
  return true;
}

bool validPose(PoseSample sample) noexcept
{
  if (sample.stamp_ns <= 0 || !normalizeQuaternion(sample.quaternion_xyzw)) {
    return false;
  }
  return std::all_of(
    sample.position_m.begin(), sample.position_m.end(),
    [](const double value) {return std::isfinite(value);});
}

std::array<double, 4> slerp(
  const std::array<double, 4> & first, const std::array<double, 4> & second,
  const double fraction) noexcept
{
  std::array<double, 4> target = second;
  double dot = 0.0;
  for (std::size_t index = 0; index < 4U; ++index) {
    dot += first[index] * target[index];
  }
  if (dot < 0.0) {
    dot = -dot;
    for (double & value : target) {
      value = -value;
    }
  }
  dot = std::clamp(dot, -1.0, 1.0);

  std::array<double, 4> result{};
  if (dot > 0.9995) {
    for (std::size_t index = 0; index < 4U; ++index) {
      result[index] = first[index] + fraction * (target[index] - first[index]);
    }
    normalizeQuaternion(result);
    return result;
  }

  const double angle = std::acos(dot);
  const double sine = std::sin(angle);
  const double first_weight = std::sin((1.0 - fraction) * angle) / sine;
  const double second_weight = std::sin(fraction * angle) / sine;
  for (std::size_t index = 0; index < 4U; ++index) {
    result[index] = first_weight * first[index] + second_weight * target[index];
  }
  return result;
}

bool validRotationMatrix(const RotationMatrix3d & rotation) noexcept
{
  for (const double value : rotation) {
    if (!std::isfinite(value)) {
      return false;
    }
  }

  constexpr double tolerance = 1.0e-6;
  for (int row = 0; row < 3; ++row) {
    for (int other_row = 0; other_row < 3; ++other_row) {
      double dot = 0.0;
      for (int col = 0; col < 3; ++col) {
        dot += rotation[row * 3 + col] * rotation[other_row * 3 + col];
      }
      const double expected = row == other_row ? 1.0 : 0.0;
      if (std::abs(dot - expected) > tolerance) {
        return false;
      }
    }
  }

  const double determinant =
    rotation[0] * (rotation[4] * rotation[8] - rotation[5] * rotation[7]) -
    rotation[1] * (rotation[3] * rotation[8] - rotation[5] * rotation[6]) +
    rotation[2] * (rotation[3] * rotation[7] - rotation[4] * rotation[6]);
  return std::abs(determinant - 1.0) <= tolerance;
}

void multiplyMatrixVector(
  const double matrix[9], const double input[3], double output[3]) noexcept
{
  output[0] = matrix[0] * input[0] + matrix[1] * input[1] + matrix[2] * input[2];
  output[1] = matrix[3] * input[0] + matrix[4] * input[1] + matrix[5] * input[2];
  output[2] = matrix[6] * input[0] + matrix[7] * input[1] + matrix[8] * input[2];
}

}  // namespace

PoseBuffer::PoseBuffer(
  const std::int64_t cache_duration_ns, const std::int64_t max_interpolation_gap_ns)
: cache_duration_ns_(cache_duration_ns), max_interpolation_gap_ns_(max_interpolation_gap_ns)
{
}

bool PoseBuffer::add(const PoseSample & sample) noexcept
{
  PoseSample normalized = sample;
  if (!validPose(normalized) || cache_duration_ns_ <= 0 || max_interpolation_gap_ns_ <= 0) {
    return false;
  }
  normalizeQuaternion(normalized.quaternion_xyzw);

  if (!samples_.empty() && normalized.stamp_ns < samples_.back().stamp_ns) {
    return false;
  }
  if (!samples_.empty() && normalized.stamp_ns == samples_.back().stamp_ns) {
    samples_.back() = normalized;
  } else {
    samples_.push_back(normalized);
  }
  while (samples_.size() > 1U &&
    samples_.back().stamp_ns - samples_.front().stamp_ns > cache_duration_ns_)
  {
    samples_.pop_front();
  }
  return true;
}

bool PoseBuffer::interpolate(const std::int64_t stamp_ns, PoseSample & output) const noexcept
{
  if (samples_.empty() || stamp_ns < samples_.front().stamp_ns ||
    stamp_ns > samples_.back().stamp_ns)
  {
    return false;
  }
  const auto upper = std::lower_bound(
    samples_.begin(), samples_.end(), stamp_ns,
    [](const PoseSample & sample, const std::int64_t target) {
      return sample.stamp_ns < target;
    });
  if (upper != samples_.end() && upper->stamp_ns == stamp_ns) {
    output = *upper;
    return true;
  }
  if (upper == samples_.begin() || upper == samples_.end()) {
    return false;
  }
  const auto lower = std::prev(upper);
  const std::int64_t gap_ns = upper->stamp_ns - lower->stamp_ns;
  if (gap_ns <= 0 || gap_ns > max_interpolation_gap_ns_) {
    return false;
  }
  const double fraction = static_cast<double>(stamp_ns - lower->stamp_ns) /
    static_cast<double>(gap_ns);
  output.stamp_ns = stamp_ns;
  for (std::size_t index = 0; index < 3U; ++index) {
    output.position_m[index] = lower->position_m[index] +
      fraction * (upper->position_m[index] - lower->position_m[index]);
  }
  output.quaternion_xyzw = slerp(
    lower->quaternion_xyzw, upper->quaternion_xyzw, fraction);
  return true;
}

bool PoseBuffer::empty() const noexcept
{
  return samples_.empty();
}

std::int64_t PoseBuffer::oldestStampNs() const noexcept
{
  return samples_.empty() ? 0 : samples_.front().stamp_ns;
}

std::int64_t PoseBuffer::newestStampNs() const noexcept
{
  return samples_.empty() ? 0 : samples_.back().stamp_ns;
}

EnuCloudTransformer::EnuCloudTransformer(
  const std::int64_t cache_duration_ns, const std::int64_t max_interpolation_gap_ns,
  const bool use_odometry_translation, const RotationMatrix3d & lidar_to_odometry_rotation)
: pose_buffer_(cache_duration_ns, max_interpolation_gap_ns),
  use_odometry_translation_(use_odometry_translation),
  lidar_to_odometry_rotation_(lidar_to_odometry_rotation)
{
  if (cache_duration_ns <= 0 || max_interpolation_gap_ns <= 0) {
    throw std::invalid_argument("时间参数必须为正数");
  }
  if (!validRotationMatrix(lidar_to_odometry_rotation_)) {
    throw std::invalid_argument("lidar_to_odometry_rotation必须为正交且行列式为1的3×3旋转矩阵");
  }
}

bool EnuCloudTransformer::addPose(const PoseSample & sample) noexcept
{
  return pose_buffer_.add(sample);
}

bool EnuCloudTransformer::initialized() const noexcept
{
  return !pose_buffer_.empty();
}

std::int64_t EnuCloudTransformer::oldestPoseStampNs() const noexcept
{
  return pose_buffer_.oldestStampNs();
}

std::int64_t EnuCloudTransformer::newestPoseStampNs() const noexcept
{
  return pose_buffer_.newestStampNs();
}

bool EnuCloudTransformer::transform(
  const std::int64_t cloud_stamp_ns, const std::vector<TimedRadarPoint> & input,
  std::vector<EnuPoint> & output, std::string & invalid_reason) const noexcept
{
  output.clear();
  if (!initialized()) {
    invalid_reason = "POSE_NOT_INITIALIZED";
    return false;
  }
  if (cloud_stamp_ns <= 0 || input.empty()) {
    invalid_reason = "INVALID_POINT_CLOUD_TIME";
    return false;
  }

  PoseSample reference_pose;
  if (use_odometry_translation_ && !pose_buffer_.interpolate(cloud_stamp_ns, reference_pose)) {
    invalid_reason = "REFERENCE_POSE_NOT_COVERED";
    return false;
  }

  output.reserve(input.size());
  for (const TimedRadarPoint & point : input) {
    if (!std::isfinite(point.offset_time_s) || point.offset_time_s < 0.0F) {
      output.clear();
      invalid_reason = "INVALID_POINT_TIME";
      return false;
    }
    if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z)) {
      const float nan = std::numeric_limits<float>::quiet_NaN();
      output.push_back(EnuPoint{nan, nan, nan});
      continue;
    }
    // 厂商驱动用全零点表示低置信度无效点，不能经过旋转和平移后变成伪有效点。
    if (point.x == 0.0F && point.y == 0.0F && point.z == 0.0F) {
      output.push_back(EnuPoint{});
      continue;
    }

    const auto offset_ns = static_cast<std::int64_t>(
      std::llround(static_cast<double>(point.offset_time_s) * 1.0e9));
    if (offset_ns < 0 || cloud_stamp_ns > std::numeric_limits<std::int64_t>::max() - offset_ns) {
      output.clear();
      invalid_reason = "INVALID_POINT_TIME";
      return false;
    }

    PoseSample point_pose;
    if (!pose_buffer_.interpolate(cloud_stamp_ns + offset_ns, point_pose)) {
      output.clear();
      invalid_reason = "POINT_POSE_NOT_COVERED";
      return false;
    }

    // q2mat经rosQuaternionToMatrix输出R_n<-b，即里程计机体系到导航ENU系的旋转矩阵。
    double rotation_navigation_from_body[9];
    if (!localization::rosQuaternionToMatrix(
        point_pose.quaternion_xyzw[0], point_pose.quaternion_xyzw[1],
        point_pose.quaternion_xyzw[2], point_pose.quaternion_xyzw[3],
        rotation_navigation_from_body))
    {
      output.clear();
      invalid_reason = "INVALID_INTERPOLATED_QUATERNION";
      return false;
    }

    const double radar_point[3]{point.x, point.y, point.z};
    double body_point[3];
    multiplyMatrixVector(lidar_to_odometry_rotation_.data(), radar_point, body_point);

    // r_n = R_n<-b(t_i) * C0_b<-l * r_l。
    double navigation_point[3];
    multiplyMatrixVector(rotation_navigation_from_body, body_point, navigation_point);

    if (use_odometry_translation_) {
      for (std::size_t index = 0; index < 3U; ++index) {
        navigation_point[index] +=
          point_pose.position_m[index] - reference_pose.position_m[index];
      }
    }

    output.push_back(
      EnuPoint{
        static_cast<float>(navigation_point[0]),
        static_cast<float>(navigation_point[1]),
        static_cast<float>(navigation_point[2])});
  }

  invalid_reason = "NONE";
  return true;
}

}  // namespace motion_compensation

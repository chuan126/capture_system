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

bool validImu(const ImuSample & sample) noexcept
{
  if (sample.stamp_ns <= 0) {
    return false;
  }
  return std::all_of(
    sample.angular_velocity_rad_s.begin(), sample.angular_velocity_rad_s.end(),
    [](const double value) {return std::isfinite(value);}) &&
         std::all_of(
    sample.linear_acceleration_m_s2.begin(), sample.linear_acceleration_m_s2.end(),
    [](const double value) {return std::isfinite(value);});
}

bool gravityAlignmentMatrix(
  const double acceleration_odom[3], const double min_norm, const double max_norm,
  double alignment[9]) noexcept
{
  const double norm = std::sqrt(
    acceleration_odom[0] * acceleration_odom[0] +
    acceleration_odom[1] * acceleration_odom[1] +
    acceleration_odom[2] * acceleration_odom[2]);
  if (!std::isfinite(norm) || norm < min_norm || norm > max_norm) {
    return false;
  }
  const double x = acceleration_odom[0] / norm;
  const double y = acceleration_odom[1] / norm;
  const double z = std::clamp(acceleration_odom[2] / norm, -1.0, 1.0);
  const double sine_squared = x * x + y * y;
  if (sine_squared <= 1.0e-18) {
    if (z < 0.0) {
      alignment[0] = 1.0;
      alignment[1] = 0.0;
      alignment[2] = 0.0;
      alignment[3] = 0.0;
      alignment[4] = -1.0;
      alignment[5] = 0.0;
      alignment[6] = 0.0;
      alignment[7] = 0.0;
      alignment[8] = -1.0;
    } else {
      const double identity[9]{1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
      std::copy(identity, identity + 9, alignment);
    }
    return true;
  }

  // Rodrigues公式：把四元数估计下的加速度方向旋转到ENU的+Z。
  const double factor = (1.0 - z) / sine_squared;
  alignment[0] = 1.0 - x * x * factor;
  alignment[1] = -x * y * factor;
  alignment[2] = -x;
  alignment[3] = -x * y * factor;
  alignment[4] = 1.0 - y * y * factor;
  alignment[5] = -y;
  alignment[6] = x;
  alignment[7] = y;
  alignment[8] = z;
  return true;
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
  const bool use_odometry_translation, const double min_gravity_norm_m_s2,
  const double max_gravity_norm_m_s2)
: pose_buffer_(cache_duration_ns, max_interpolation_gap_ns),
  cache_duration_ns_(cache_duration_ns),
  max_interpolation_gap_ns_(max_interpolation_gap_ns),
  use_odometry_translation_(use_odometry_translation),
  min_gravity_norm_m_s2_(min_gravity_norm_m_s2),
  max_gravity_norm_m_s2_(max_gravity_norm_m_s2)
{
  if (!(min_gravity_norm_m_s2_ > 0.0) ||
    !(max_gravity_norm_m_s2_ > min_gravity_norm_m_s2_))
  {
    throw std::invalid_argument("重力模长范围参数不合法");
  }
}

bool EnuCloudTransformer::addPose(const PoseSample & sample) noexcept
{
  if (!pose_buffer_.add(sample)) {
    return false;
  }
  if (!initialized_) {
    initialized_ = localization::initializeGravityAlignedEnuReference(
      sample.quaternion_xyzw[0], sample.quaternion_xyzw[1], sample.quaternion_xyzw[2],
      sample.quaternion_xyzw[3], Cenu_odom_);
  }
  return initialized_;
}

bool EnuCloudTransformer::addImu(const ImuSample & sample) noexcept
{
  if (!validImu(sample) || cache_duration_ns_ <= 0 || max_interpolation_gap_ns_ <= 0) {
    return false;
  }
  if (!imu_samples_.empty() && sample.stamp_ns < imu_samples_.back().stamp_ns) {
    return false;
  }
  if (!imu_samples_.empty() && sample.stamp_ns == imu_samples_.back().stamp_ns) {
    imu_samples_.back() = sample;
  } else {
    imu_samples_.push_back(sample);
  }
  while (imu_samples_.size() > 1U &&
    imu_samples_.back().stamp_ns - imu_samples_.front().stamp_ns > cache_duration_ns_)
  {
    imu_samples_.pop_front();
  }
  return true;
}

bool EnuCloudTransformer::initialized() const noexcept
{
  return initialized_ && !imu_samples_.empty();
}

std::int64_t EnuCloudTransformer::oldestImuStampNs() const noexcept
{
  return imu_samples_.empty() ? 0 : imu_samples_.front().stamp_ns;
}

std::int64_t EnuCloudTransformer::newestImuStampNs() const noexcept
{
  return imu_samples_.empty() ? 0 : imu_samples_.back().stamp_ns;
}

bool EnuCloudTransformer::interpolateImu(
  const std::int64_t stamp_ns, ImuSample & output) const noexcept
{
  if (imu_samples_.empty() || stamp_ns < imu_samples_.front().stamp_ns ||
    stamp_ns > imu_samples_.back().stamp_ns)
  {
    return false;
  }
  const auto upper = std::lower_bound(
    imu_samples_.begin(), imu_samples_.end(), stamp_ns,
    [](const ImuSample & sample, const std::int64_t target) {
      return sample.stamp_ns < target;
    });
  if (upper != imu_samples_.end() && upper->stamp_ns == stamp_ns) {
    output = *upper;
    return true;
  }
  if (upper == imu_samples_.begin() || upper == imu_samples_.end()) {
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
    output.angular_velocity_rad_s[index] = lower->angular_velocity_rad_s[index] +
      fraction * (upper->angular_velocity_rad_s[index] - lower->angular_velocity_rad_s[index]);
    output.linear_acceleration_m_s2[index] = lower->linear_acceleration_m_s2[index] +
      fraction *
      (upper->linear_acceleration_m_s2[index] - lower->linear_acceleration_m_s2[index]);
  }
  return true;
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
    invalid_reason = "REFERENCE_NOT_INITIALIZED";
    return false;
  }
  if (cloud_stamp_ns <= 0 || input.empty()) {
    invalid_reason = "INVALID_POINT_CLOUD_TIME";
    return false;
  }

  PoseSample reference_pose;
  if (!pose_buffer_.interpolate(cloud_stamp_ns, reference_pose)) {
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
    // 厂商驱动用全零点表示低置信度无效点，不能叠加运动平移后变成伪有效点。
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

    ImuSample imu_sample;
    if (!interpolateImu(cloud_stamp_ns + offset_ns, imu_sample)) {
      output.clear();
      invalid_reason = "POINT_IMU_NOT_COVERED";
      return false;
    }

    double Codom_lidar[9];
    if (!localization::rosQuaternionToMatrix(
        point_pose.quaternion_xyzw[0], point_pose.quaternion_xyzw[1],
        point_pose.quaternion_xyzw[2], point_pose.quaternion_xyzw[3], Codom_lidar))
    {
      output.clear();
      invalid_reason = "INVALID_INTERPOLATED_QUATERNION";
      return false;
    }
    const double acceleration_odom[3]{
      Codom_lidar[0] * imu_sample.linear_acceleration_m_s2[0] +
      Codom_lidar[1] * imu_sample.linear_acceleration_m_s2[1] +
      Codom_lidar[2] * imu_sample.linear_acceleration_m_s2[2],
      Codom_lidar[3] * imu_sample.linear_acceleration_m_s2[0] +
      Codom_lidar[4] * imu_sample.linear_acceleration_m_s2[1] +
      Codom_lidar[5] * imu_sample.linear_acceleration_m_s2[2],
      Codom_lidar[6] * imu_sample.linear_acceleration_m_s2[0] +
      Codom_lidar[7] * imu_sample.linear_acceleration_m_s2[1] +
      Codom_lidar[8] * imu_sample.linear_acceleration_m_s2[2]};
    double Cgravity_odom[9];
    if (!gravityAlignmentMatrix(
        acceleration_odom, min_gravity_norm_m_s2_, max_gravity_norm_m_s2_, Cgravity_odom))
    {
      output.clear();
      invalid_reason = "INVALID_GRAVITY_NORM";
      return false;
    }
    double Ccorrected_lidar[9];
    for (int row = 0; row < 3; ++row) {
      for (int col = 0; col < 3; ++col) {
        Ccorrected_lidar[row * 3 + col] =
          Cgravity_odom[row * 3] * Codom_lidar[col] +
          Cgravity_odom[row * 3 + 1] * Codom_lidar[3 + col] +
          Cgravity_odom[row * 3 + 2] * Codom_lidar[6 + col];
      }
    }
    const double radar[3]{point.x, point.y, point.z};
    double odom[3]{
      Ccorrected_lidar[0] * radar[0] + Ccorrected_lidar[1] * radar[1] +
      Ccorrected_lidar[2] * radar[2],
      Ccorrected_lidar[3] * radar[0] + Ccorrected_lidar[4] * radar[1] +
      Ccorrected_lidar[5] * radar[2],
      Ccorrected_lidar[6] * radar[0] + Ccorrected_lidar[7] * radar[1] +
      Ccorrected_lidar[8] * radar[2]};
    if (use_odometry_translation_) {
      for (std::size_t index = 0; index < 3U; ++index) {
        odom[index] += point_pose.position_m[index] - reference_pose.position_m[index];
      }
    }
    const double enu[3]{
      Cenu_odom_[0] * odom[0] + Cenu_odom_[1] * odom[1] + Cenu_odom_[2] * odom[2],
      Cenu_odom_[3] * odom[0] + Cenu_odom_[4] * odom[1] + Cenu_odom_[5] * odom[2],
      Cenu_odom_[6] * odom[0] + Cenu_odom_[7] * odom[1] + Cenu_odom_[8] * odom[2]};
    output.push_back(
      EnuPoint{
        static_cast<float>(enu[0]), static_cast<float>(enu[1]), static_cast<float>(enu[2])});
  }
  invalid_reason = "NONE";
  return true;
}

}  // namespace motion_compensation

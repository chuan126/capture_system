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

bool interpolateSamples(
  const std::vector<PoseSample> & samples, const std::int64_t max_interpolation_gap_ns,
  const std::int64_t stamp_ns, PoseSample & output) noexcept
{
  if (samples.empty() || stamp_ns < samples.front().stamp_ns || stamp_ns > samples.back().stamp_ns) {
    return false;
  }
  const auto upper = std::lower_bound(
    samples.begin(), samples.end(), stamp_ns,
    [](const PoseSample & sample, const std::int64_t target) {
      return sample.stamp_ns < target;
    });
  if (upper != samples.end() && upper->stamp_ns == stamp_ns) {
    output = *upper;
    return true;
  }
  if (upper == samples.begin() || upper == samples.end()) {
    return false;
  }
  const auto lower = std::prev(upper);
  const std::int64_t gap_ns = upper->stamp_ns - lower->stamp_ns;
  if (gap_ns <= 0 || gap_ns > max_interpolation_gap_ns) {
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
  output.translation_valid = lower->translation_valid && upper->translation_valid;
  return true;
}

void multiplyMatrixVector(
  const double matrix[9], const double input[3], double output[3]) noexcept
{
  output[0] = matrix[0] * input[0] + matrix[1] * input[1] + matrix[2] * input[2];
  output[1] = matrix[3] * input[0] + matrix[4] * input[1] + matrix[5] * input[2];
  output[2] = matrix[6] * input[0] + matrix[7] * input[1] + matrix[8] * input[2];
}

EnuPoint nanPoint() noexcept
{
  const float nan = std::numeric_limits<float>::quiet_NaN();
  return EnuPoint{nan, nan, nan};
}

}  // namespace

const char * transformModeName(const TransformMode mode) noexcept
{
  switch (mode) {
    case TransformMode::kReject:
      return "REJECT";
    case TransformMode::kFullSe3:
      return "FULL_SE3";
    case TransformMode::kRotationOnly:
      return "ROTATION_ONLY";
  }
  return "UNKNOWN";
}

PoseBuffer::PoseBuffer(
  const std::int64_t cache_duration_ns, const std::int64_t max_interpolation_gap_ns,
  const std::int64_t timestamp_reset_threshold_ns)
: cache_duration_ns_(cache_duration_ns), max_interpolation_gap_ns_(max_interpolation_gap_ns),
  timestamp_reset_threshold_ns_(timestamp_reset_threshold_ns)
{
}

PoseBuffer::AddResult PoseBuffer::add(const PoseSample & sample) noexcept
{
  PoseSample normalized = sample;
  if (!validPose(normalized) || cache_duration_ns_ <= 0 || max_interpolation_gap_ns_ <= 0) {
    return AddResult::kRejected;
  }
  normalizeQuaternion(normalized.quaternion_xyzw);

  std::lock_guard<std::mutex> lock(mutex_);
  if (!samples_.empty() && normalized.stamp_ns < samples_.back().stamp_ns) {
    if (samples_.back().stamp_ns - normalized.stamp_ns > timestamp_reset_threshold_ns_) {
      samples_.clear();
      samples_.push_back(normalized);
      return AddResult::kEpochReset;
    }
    return AddResult::kRejected;
  }
  const bool gap_detected = !samples_.empty() &&
    normalized.stamp_ns - samples_.back().stamp_ns > max_interpolation_gap_ns_;
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
  return gap_detected ? AddResult::kGapDetected : AddResult::kAccepted;
}

bool PoseBuffer::interpolate(const std::int64_t stamp_ns, PoseSample & output) const noexcept
{
  const auto samples = snapshot();
  return interpolateSamples(samples, max_interpolation_gap_ns_, stamp_ns, output);
}

bool PoseBuffer::empty() const noexcept
{
  std::lock_guard<std::mutex> lock(mutex_);
  return samples_.empty();
}

void PoseBuffer::clear() noexcept
{
  std::lock_guard<std::mutex> lock(mutex_);
  samples_.clear();
}

std::int64_t PoseBuffer::oldestStampNs() const noexcept
{
  std::lock_guard<std::mutex> lock(mutex_);
  return samples_.empty() ? 0 : samples_.front().stamp_ns;
}

std::int64_t PoseBuffer::newestStampNs() const noexcept
{
  std::lock_guard<std::mutex> lock(mutex_);
  return samples_.empty() ? 0 : samples_.back().stamp_ns;
}

std::int64_t PoseBuffer::continuousDurationNs() const noexcept
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (samples_.size() < 2U) {
    return 0;
  }
  auto segment_start = std::prev(samples_.end());
  while (segment_start != samples_.begin()) {
    const auto previous = std::prev(segment_start);
    if (segment_start->stamp_ns - previous->stamp_ns > max_interpolation_gap_ns_) {
      break;
    }
    segment_start = previous;
  }
  return samples_.back().stamp_ns - segment_start->stamp_ns;
}

std::vector<PoseSample> PoseBuffer::snapshot() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return std::vector<PoseSample>(samples_.begin(), samples_.end());
}

std::int64_t PoseBuffer::maxInterpolationGapNs() const noexcept
{
  return max_interpolation_gap_ns_;
}

EnuCloudTransformer::EnuCloudTransformer(
  const std::int64_t cache_duration_ns, const std::int64_t max_interpolation_gap_ns,
  const bool use_odometry_translation,
  const double minimum_valid_pose_ratio, const double max_translation_per_scan_m,
  const bool fallback_to_rotation_only, const std::int64_t timestamp_reset_threshold_ns)
: pose_buffer_(cache_duration_ns, max_interpolation_gap_ns, timestamp_reset_threshold_ns),
  use_odometry_translation_(use_odometry_translation),
  minimum_valid_pose_ratio_(minimum_valid_pose_ratio),
  max_translation_per_scan_m_(max_translation_per_scan_m),
  fallback_to_rotation_only_(fallback_to_rotation_only)
{
  if (cache_duration_ns <= 0 || max_interpolation_gap_ns <= 0 ||
    timestamp_reset_threshold_ns <= 0) {
    throw std::invalid_argument("时间参数必须为正数");
  }
  if (!(minimum_valid_pose_ratio_ > 0.0 && minimum_valid_pose_ratio_ <= 1.0)) {
    throw std::invalid_argument("minimum_valid_pose_ratio必须位于(0,1]范围内");
  }
  if (!(max_translation_per_scan_m_ > 0.0) || !std::isfinite(max_translation_per_scan_m_)) {
    throw std::invalid_argument("max_translation_per_scan_m必须为有限正数");
  }
}

PoseBuffer::AddResult EnuCloudTransformer::addPose(const PoseSample & sample) noexcept
{
  return pose_buffer_.add(sample);
}

bool EnuCloudTransformer::initialized() const noexcept
{
  return !pose_buffer_.empty();
}

void EnuCloudTransformer::clearPoses() noexcept
{
  pose_buffer_.clear();
}

std::int64_t EnuCloudTransformer::oldestPoseStampNs() const noexcept
{
  return pose_buffer_.oldestStampNs();
}

std::int64_t EnuCloudTransformer::newestPoseStampNs() const noexcept
{
  return pose_buffer_.newestStampNs();
}

std::int64_t EnuCloudTransformer::continuousPoseDurationNs() const noexcept
{
  return pose_buffer_.continuousDurationNs();
}

bool EnuCloudTransformer::transform(
  const std::int64_t cloud_stamp_ns, const std::vector<TimedRadarPoint> & input,
  std::vector<EnuPoint> & output, std::string & invalid_reason,
  TransformStatistics * statistics) const noexcept
{
  TransformStatistics local_statistics;
  local_statistics.input_point_count = input.size();
  output.clear();

  const auto pose_samples = pose_buffer_.snapshot();
  if (pose_samples.empty()) {
    invalid_reason = "POSE_NOT_INITIALIZED";
    if (statistics != nullptr) {
      *statistics = local_statistics;
    }
    return false;
  }
  if (cloud_stamp_ns <= 0 || input.empty()) {
    invalid_reason = "INVALID_POINT_CLOUD_TIME";
    if (statistics != nullptr) {
      *statistics = local_statistics;
    }
    return false;
  }

  std::vector<std::int64_t> point_stamps_ns;
  point_stamps_ns.reserve(32U);
  std::int64_t previous_point_stamp_ns = std::numeric_limits<std::int64_t>::min();
  for (const TimedRadarPoint & point : input) {
    if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z)) {
      continue;
    }
    if (point.x == 0.0F && point.y == 0.0F && point.z == 0.0F) {
      continue;
    }
    ++local_statistics.finite_nonzero_point_count;
    if (!std::isfinite(point.offset_time_s) || point.offset_time_s < 0.0F) {
      ++local_statistics.invalid_time_point_count;
      continue;
    }
    const auto offset_ns = static_cast<std::int64_t>(
      std::llround(static_cast<double>(point.offset_time_s) * 1.0e9));
    if (offset_ns < 0 || cloud_stamp_ns > std::numeric_limits<std::int64_t>::max() - offset_ns) {
      ++local_statistics.invalid_time_point_count;
      continue;
    }
    const std::int64_t point_stamp_ns = cloud_stamp_ns + offset_ns;
    if (point_stamp_ns != previous_point_stamp_ns) {
      point_stamps_ns.push_back(point_stamp_ns);
      previous_point_stamp_ns = point_stamp_ns;
    }
  }
  if (local_statistics.finite_nonzero_point_count == 0U) {
    invalid_reason = "NO_VALID_RAW_POINTS";
    if (statistics != nullptr) {
      *statistics = local_statistics;
    }
    return false;
  }

  bool translation_active = use_odometry_translation_;
  PoseSample reference_pose;
  if (translation_active && (!interpolateSamples(
      pose_samples, pose_buffer_.maxInterpolationGapNs(), cloud_stamp_ns, reference_pose) ||
      !reference_pose.translation_valid))
  {
    if (!fallback_to_rotation_only_) {
      invalid_reason = "REFERENCE_POSE_NOT_COVERED";
      if (statistics != nullptr) {
        *statistics = local_statistics;
      }
      return false;
    }
    translation_active = false;
    local_statistics.translation_fallback = true;
  }
  if (translation_active) {
    bool translation_outlier = false;
    for (const std::int64_t point_stamp_ns : point_stamps_ns) {
      PoseSample point_pose;
      if (!interpolateSamples(
          pose_samples, pose_buffer_.maxInterpolationGapNs(), point_stamp_ns, point_pose))
      {
        continue;
      }
      if (!point_pose.translation_valid) {
        translation_outlier = true;
        break;
      }
      double squared_translation = 0.0;
      for (std::size_t index = 0; index < 3U; ++index) {
        const double delta = point_pose.position_m[index] - reference_pose.position_m[index];
        squared_translation += delta * delta;
      }
      const double translation_m = std::sqrt(squared_translation);
      if (!std::isfinite(translation_m)) {
        translation_outlier = true;
        break;
      }
      local_statistics.maximum_translation_m = std::max(
        local_statistics.maximum_translation_m, translation_m);
      if (translation_m > max_translation_per_scan_m_) {
        translation_outlier = true;
        break;
      }
    }
    if (translation_outlier) {
      if (!fallback_to_rotation_only_) {
        invalid_reason = "FUSION_TRANSLATION_OUTLIER";
        if (statistics != nullptr) {
          *statistics = local_statistics;
        }
        return false;
      }
      translation_active = false;
      local_statistics.translation_fallback = true;
    }
  }
  local_statistics.translation_applied = translation_active;
  local_statistics.mode = translation_active ?
    TransformMode::kFullSe3 : TransformMode::kRotationOnly;

  output.reserve(input.size());
  std::int64_t cached_point_stamp_ns = std::numeric_limits<std::int64_t>::min();
  bool cached_pose_valid = false;
  double cached_rotation_navigation_from_body[9]{};
  double cached_translation[3]{};

  for (const TimedRadarPoint & point : input) {
    if (!std::isfinite(point.offset_time_s) || point.offset_time_s < 0.0F) {
      output.push_back(nanPoint());
      continue;
    }
    if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z)) {
      output.push_back(nanPoint());
      continue;
    }
    // 厂商驱动用全零点表示低置信度无效点，保持全零，避免经平移后变成伪有效点。
    if (point.x == 0.0F && point.y == 0.0F && point.z == 0.0F) {
      output.push_back(EnuPoint{});
      continue;
    }
    const auto offset_ns = static_cast<std::int64_t>(
      std::llround(static_cast<double>(point.offset_time_s) * 1.0e9));
    if (offset_ns < 0 || cloud_stamp_ns > std::numeric_limits<std::int64_t>::max() - offset_ns) {
      output.push_back(nanPoint());
      continue;
    }
    const std::int64_t point_stamp_ns = cloud_stamp_ns + offset_ns;

    // ODIN原始点云约有32个滚动快门时间组。相同offset_time复用一次姿态插值和矩阵计算，
    // 避免对约49152个点重复执行SLERP和四元数转矩阵。
    if (point_stamp_ns != cached_point_stamp_ns) {
      cached_point_stamp_ns = point_stamp_ns;
      cached_pose_valid = false;
      PoseSample point_pose;
      if (interpolateSamples(
          pose_samples, pose_buffer_.maxInterpolationGapNs(), point_stamp_ns, point_pose) &&
        localization::rosQuaternionToMatrix(
          point_pose.quaternion_xyzw[0], point_pose.quaternion_xyzw[1],
          point_pose.quaternion_xyzw[2], point_pose.quaternion_xyzw[3],
          cached_rotation_navigation_from_body))
      {
        cached_translation[0] = 0.0;
        cached_translation[1] = 0.0;
        cached_translation[2] = 0.0;
        if (translation_active) {
          double squared_translation = 0.0;
          for (std::size_t index = 0; index < 3U; ++index) {
            cached_translation[index] =
              point_pose.position_m[index] - reference_pose.position_m[index];
            squared_translation += cached_translation[index] * cached_translation[index];
          }
          cached_pose_valid = std::isfinite(squared_translation);
        } else {
          cached_pose_valid = true;
        }
      }
    }

    if (!cached_pose_valid) {
      ++local_statistics.uncovered_point_count;
      output.push_back(nanPoint());
      continue;
    }
    ++local_statistics.pose_covered_point_count;

    const double radar_point[3]{point.x, point.y, point.z};

    // 雷达坐标系与ODIN机体系方向一致，杆臂按0处理；平移来自Fusion Local Navigation：
    // r_n = R_n<-b(t_i) * r_l + p_fusion(t_i) - p_fusion(t_0)。
    double navigation_point[3];
    multiplyMatrixVector(cached_rotation_navigation_from_body, radar_point, navigation_point);
    for (std::size_t index = 0; index < 3U; ++index) {
      navigation_point[index] += cached_translation[index];
    }

    output.push_back(
      EnuPoint{
        static_cast<float>(navigation_point[0]),
        static_cast<float>(navigation_point[1]),
        static_cast<float>(navigation_point[2])});
    ++local_statistics.transformed_point_count;
  }

  if (local_statistics.finite_nonzero_point_count > 0U) {
    local_statistics.valid_pose_ratio =
      static_cast<double>(local_statistics.pose_covered_point_count) /
      static_cast<double>(local_statistics.finite_nonzero_point_count);
  }

  if (statistics != nullptr) {
    *statistics = local_statistics;
  }
  if (local_statistics.transformed_point_count == 0U) {
    invalid_reason = "NO_POINT_POSE_COVERED";
    return false;
  }
  if (local_statistics.valid_pose_ratio < minimum_valid_pose_ratio_) {
    invalid_reason = "INSUFFICIENT_POSE_COVERAGE";
    return false;
  }

  invalid_reason = "NONE";
  return true;
}

}  // namespace motion_compensation

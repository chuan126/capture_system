#include "localization/odometry_buffer.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>

namespace localization
{

std::optional<std::int64_t> applyRtkTimeOffsetNs(
  const std::int64_t rtk_stamp_ns, const double rtk_time_offset_s) noexcept
{
  if (rtk_stamp_ns <= 0 || !std::isfinite(rtk_time_offset_s)) {
    return std::nullopt;
  }
  const long double offset_ns = static_cast<long double>(rtk_time_offset_s) * 1.0e9L;
  const long double synchronized_stamp = static_cast<long double>(rtk_stamp_ns) + offset_ns;
  if (synchronized_stamp <= 0.0L ||
    synchronized_stamp > static_cast<long double>(std::numeric_limits<std::int64_t>::max()))
  {
    return std::nullopt;
  }
  return static_cast<std::int64_t>(std::llround(synchronized_stamp));
}

std::optional<std::int64_t> mapReceiptTimeToSensorTimeNs(
  const std::int64_t target_received_ns, const std::int64_t reference_received_ns,
  const std::int64_t reference_sensor_stamp_ns, const double sensor_time_offset_s) noexcept
{
  if (target_received_ns <= 0 || reference_received_ns <= 0 ||
    reference_sensor_stamp_ns <= 0 || !std::isfinite(sensor_time_offset_s))
  {
    return std::nullopt;
  }
  const long double mapped_stamp = static_cast<long double>(reference_sensor_stamp_ns) +
    static_cast<long double>(target_received_ns) -
    static_cast<long double>(reference_received_ns) +
    static_cast<long double>(sensor_time_offset_s) * 1.0e9L;
  if (mapped_stamp <= 0.0L ||
    mapped_stamp > static_cast<long double>(std::numeric_limits<std::int64_t>::max()))
  {
    return std::nullopt;
  }
  return static_cast<std::int64_t>(std::llround(mapped_stamp));
}

OdometryBuffer::OdometryBuffer(
  const std::int64_t cache_duration_ns, const std::int64_t max_interpolation_gap_ns,
  const std::size_t max_samples, const std::int64_t timestamp_reset_threshold_ns)
: cache_duration_ns_(cache_duration_ns),
  max_interpolation_gap_ns_(max_interpolation_gap_ns),
  max_samples_(std::max<std::size_t>(2U, max_samples)),
  timestamp_reset_threshold_ns_(timestamp_reset_threshold_ns)
{
}

OdometryBuffer::AddResult OdometryBuffer::add(OdomSample sample) noexcept
{
  if (sample.stamp_ns <= 0 || !isFinite(sample.position_m) ||
    !normalizeQuaternion(sample.orientation_xyzw))
  {
    return AddResult::kRejected;
  }
  if (!samples_.empty() && sample.stamp_ns < samples_.back().stamp_ns &&
    samples_.back().stamp_ns - sample.stamp_ns > timestamp_reset_threshold_ns_)
  {
    samples_.clear();
    samples_.push_back(sample);
    return AddResult::kEpochReset;
  }
  if (!samples_.empty() && sample.stamp_ns <= samples_.back().stamp_ns) {
    return AddResult::kRejected;
  }
  samples_.push_back(sample);
  while (samples_.size() > 1U &&
    (samples_.size() > max_samples_ ||
    samples_.back().stamp_ns - samples_.front().stamp_ns > cache_duration_ns_))
  {
    samples_.pop_front();
  }
  return AddResult::kAccepted;
}

bool OdometryBuffer::interpolate(const std::int64_t stamp_ns, OdomSample & output) const noexcept
{
  if (samples_.empty() || stamp_ns < samples_.front().stamp_ns ||
    stamp_ns > samples_.back().stamp_ns)
  {
    return false;
  }
  const auto upper = std::lower_bound(
    samples_.begin(), samples_.end(), stamp_ns,
    [](const OdomSample & sample, const std::int64_t target) {
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
  if (!std::isfinite(fraction) || fraction < 0.0 || fraction > 1.0) {
    return false;
  }
  output.stamp_ns = stamp_ns;
  output.position_m = Vector3d{
    lower->position_m.x + fraction * (upper->position_m.x - lower->position_m.x),
    lower->position_m.y + fraction * (upper->position_m.y - lower->position_m.y),
    lower->position_m.z + fraction * (upper->position_m.z - lower->position_m.z)};
  output.orientation_xyzw = slerpQuaternion(
    lower->orientation_xyzw, upper->orientation_xyzw, fraction);
  return isValidQuaternion(output.orientation_xyzw);
}

std::optional<OdomSample> OdometryBuffer::latest() const noexcept
{
  if (samples_.empty()) {
    return std::nullopt;
  }
  return samples_.back();
}

std::size_t OdometryBuffer::size() const noexcept
{
  return samples_.size();
}

void OdometryBuffer::clear() noexcept
{
  samples_.clear();
}

}  // namespace localization

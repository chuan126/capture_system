#include "localization/sensor_synchronizer.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace localization
{
namespace
{

bool validOrientation(const TimedOrientation & sample) noexcept
{
  return sample.stamp_ns > 0 && sample.orientation_odin_from_body.coeffs().array().isFinite().all() &&
         sample.orientation_odin_from_body.norm() > 1.0e-12;
}

bool validImu(const TimedImuAcceleration & sample) noexcept
{
  return sample.stamp_ns > 0 && sample.specific_force_body_mps2.array().isFinite().all();
}

}  // namespace

SensorSynchronizer::SensorSynchronizer(
  const std::int64_t maximum_interpolation_gap_ns,
  const std::int64_t timestamp_reset_threshold_ns,
  const std::size_t maximum_samples)
: maximum_interpolation_gap_ns_(std::max<std::int64_t>(1, maximum_interpolation_gap_ns)),
  timestamp_reset_threshold_ns_(std::max<std::int64_t>(1, timestamp_reset_threshold_ns)),
  maximum_samples_(std::max<std::size_t>(4U, maximum_samples))
{
}

SensorSynchronizer::AddResult SensorSynchronizer::addOrientation(
  const TimedOrientation & sample) noexcept
{
  if (!validOrientation(sample)) {
    return AddResult::kRejected;
  }
  TimedOrientation normalized = sample;
  normalized.orientation_odin_from_body.normalize();
  if (!orientations_.empty() && normalized.stamp_ns < orientations_.back().stamp_ns) {
    if (orientations_.back().stamp_ns - normalized.stamp_ns > timestamp_reset_threshold_ns_) {
      clear();
      orientations_.push_back(normalized);
      return AddResult::kEpochReset;
    }
    return AddResult::kRejected;
  }
  if (!orientations_.empty() && normalized.stamp_ns == orientations_.back().stamp_ns) {
    orientations_.back() = normalized;
  } else {
    orientations_.push_back(normalized);
  }
  trim();
  return AddResult::kAccepted;
}

SensorSynchronizer::AddResult SensorSynchronizer::addImu(
  const TimedImuAcceleration & sample) noexcept
{
  if (!validImu(sample)) {
    return AddResult::kRejected;
  }
  if (!imu_samples_.empty() && sample.stamp_ns < imu_samples_.back().stamp_ns) {
    if (imu_samples_.back().stamp_ns - sample.stamp_ns > timestamp_reset_threshold_ns_) {
      clear();
      imu_samples_.push_back(sample);
      return AddResult::kEpochReset;
    }
    return AddResult::kRejected;
  }
  if (!imu_samples_.empty() && sample.stamp_ns == imu_samples_.back().stamp_ns) {
    imu_samples_.back() = sample;
  } else {
    imu_samples_.push_back(sample);
  }
  trim();
  return AddResult::kAccepted;
}

bool SensorSynchronizer::interpolateOrientation(
  const std::int64_t stamp_ns, Eigen::Quaterniond & orientation) const noexcept
{
  if (orientations_.empty() || stamp_ns < orientations_.front().stamp_ns ||
    stamp_ns > orientations_.back().stamp_ns)
  {
    return false;
  }
  const auto upper = std::lower_bound(
    orientations_.begin(), orientations_.end(), stamp_ns,
    [](const TimedOrientation & sample, const std::int64_t target) {
      return sample.stamp_ns < target;
    });
  if (upper != orientations_.end() && upper->stamp_ns == stamp_ns) {
    orientation = upper->orientation_odin_from_body;
    return true;
  }
  if (upper == orientations_.begin() || upper == orientations_.end()) {
    return false;
  }
  const auto lower = std::prev(upper);
  const std::int64_t gap_ns = upper->stamp_ns - lower->stamp_ns;
  if (gap_ns <= 0 || gap_ns > maximum_interpolation_gap_ns_) {
    return false;
  }
  const double fraction = static_cast<double>(stamp_ns - lower->stamp_ns) /
    static_cast<double>(gap_ns);
  orientation = lower->orientation_odin_from_body.slerp(
    std::clamp(fraction, 0.0, 1.0), upper->orientation_odin_from_body).normalized();
  return orientation.coeffs().array().isFinite().all();
}

bool SensorSynchronizer::popSynchronized(SynchronizedImuSample & output) noexcept
{
  while (!imu_samples_.empty() && !orientations_.empty()) {
    const TimedImuAcceleration imu = imu_samples_.front();
    if (imu.stamp_ns > orientations_.back().stamp_ns) {
      return false;
    }
    if (imu.stamp_ns < orientations_.front().stamp_ns) {
      imu_samples_.pop_front();
      ++dropped_imu_count_;
      continue;
    }
    Eigen::Quaterniond orientation;
    if (!interpolateOrientation(imu.stamp_ns, orientation)) {
      imu_samples_.pop_front();
      ++dropped_imu_count_;
      continue;
    }
    imu_samples_.pop_front();
    output.stamp_ns = imu.stamp_ns;
    output.specific_force_body_mps2 = imu.specific_force_body_mps2;
    output.orientation_odin_from_body = orientation;
    while (orientations_.size() > 2U && orientations_[1].stamp_ns <= imu.stamp_ns) {
      orientations_.pop_front();
    }
    return true;
  }
  return false;
}

void SensorSynchronizer::trim() noexcept
{
  while (orientations_.size() > maximum_samples_) {
    orientations_.pop_front();
  }
  while (imu_samples_.size() > maximum_samples_) {
    imu_samples_.pop_front();
    ++dropped_imu_count_;
  }
}

void SensorSynchronizer::clear() noexcept
{
  orientations_.clear();
  imu_samples_.clear();
}

std::size_t SensorSynchronizer::pendingImuCount() const noexcept
{
  return imu_samples_.size();
}

std::size_t SensorSynchronizer::orientationCount() const noexcept
{
  return orientations_.size();
}

std::uint64_t SensorSynchronizer::droppedImuCount() const noexcept
{
  return dropped_imu_count_;
}

}  // namespace localization

#include "localization/heading_alignment.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>

namespace localization
{
namespace
{

constexpr double kPi = 3.141592653589793238462643383279502884;

double angularDistance(double left, double right) noexcept
{
  return std::abs(wrapAngleRad(left - right));
}

double distance2d(
  const CoursePositionSample & left, const CoursePositionSample & right) noexcept
{
  return std::hypot(right.east_m - left.east_m, right.north_m - left.north_m);
}

CourseEstimate invalidCourse(const std::string & reason) noexcept
{
  CourseEstimate estimate;
  estimate.valid = false;
  estimate.invalid_reason = reason;
  return estimate;
}

}  // namespace

double degreesToRadians(const double degrees) noexcept
{
  return degrees * kPi / 180.0;
}

double radiansToDegrees(const double radians) noexcept
{
  return radians * 180.0 / kPi;
}

double wrapAngleRad(const double angle_rad) noexcept
{
  if (!std::isfinite(angle_rad)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  double wrapped = std::fmod(angle_rad + kPi, 2.0 * kPi);
  if (wrapped < 0.0) {
    wrapped += 2.0 * kPi;
  }
  wrapped -= kPi;
  if (wrapped <= -kPi) {
    wrapped += 2.0 * kPi;
  }
  return wrapped;
}

double wrapDegrees360(const double degrees) noexcept
{
  if (!std::isfinite(degrees)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  double wrapped = std::fmod(degrees, 360.0);
  if (wrapped < 0.0) {
    wrapped += 360.0;
  }
  return wrapped;
}

double clockwiseCourseDegreesToEnuYawRad(const double course_degrees) noexcept
{
  if (!std::isfinite(course_degrees)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return wrapAngleRad(kPi / 2.0 - degreesToRadians(course_degrees));
}

double enuYawRadToClockwiseCourseDegrees(const double yaw_rad) noexcept
{
  if (!std::isfinite(yaw_rad)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return wrapDegrees360(90.0 - radiansToDegrees(yaw_rad));
}

bool isRmcValid(const std::uint8_t rmc_validity) noexcept
{
  return rmc_validity == static_cast<std::uint8_t>('A') ||
         rmc_validity == static_cast<std::uint8_t>('D');
}

HeadingAlignmentEstimator::HeadingAlignmentEstimator(HeadingAlignmentOptions options)
: options_(options)
{
  options_.min_samples = std::max<std::size_t>(1U, options_.min_samples);
  options_.max_samples = std::max(options_.max_samples, options_.min_samples);
  options_.filter_alpha = std::clamp(options_.filter_alpha, 0.0, 1.0);
}

bool HeadingAlignmentEstimator::addObservation(
  const double delta_yaw_rad, const double baseline_m, const double weight) noexcept
{
  if (!std::isfinite(delta_yaw_rad) || !std::isfinite(baseline_m) || baseline_m < 0.0 ||
    !std::isfinite(weight) || weight <= 0.0)
  {
    return false;
  }
  observations_.push_back(Observation{wrapAngleRad(delta_yaw_rad), baseline_m, weight});
  while (observations_.size() > options_.max_samples) {
    observations_.pop_front();
  }

  const HeadingAlignmentState raw_state = state();
  if (raw_state.sample_count > 0U) {
    if (!filtered_delta_yaw_rad_.has_value() || options_.filter_alpha <= 0.0) {
      filtered_delta_yaw_rad_ = raw_state.delta_yaw_rad;
    } else {
      const double error = wrapAngleRad(raw_state.delta_yaw_rad - *filtered_delta_yaw_rad_);
      filtered_delta_yaw_rad_ = wrapAngleRad(*filtered_delta_yaw_rad_ + options_.filter_alpha * error);
    }
  }
  return true;
}

HeadingAlignmentState HeadingAlignmentEstimator::state() const noexcept
{
  HeadingAlignmentState result;
  result.sample_count = observations_.size();
  if (observations_.empty()) {
    return result;
  }

  double sum_sin = 0.0;
  double sum_cos = 0.0;
  double sum_weight = 0.0;
  double baseline = 0.0;
  for (const Observation & observation : observations_) {
    sum_sin += observation.weight * std::sin(observation.delta_yaw_rad);
    sum_cos += observation.weight * std::cos(observation.delta_yaw_rad);
    sum_weight += observation.weight;
    baseline += observation.baseline_m;
  }
  result.delta_yaw_rad = std::atan2(sum_sin, sum_cos);
  if (filtered_delta_yaw_rad_.has_value()) {
    result.delta_yaw_rad = *filtered_delta_yaw_rad_;
  }
  result.baseline_m = baseline;

  double variance = 0.0;
  for (const Observation & observation : observations_) {
    const double error = wrapAngleRad(observation.delta_yaw_rad - result.delta_yaw_rad);
    variance += observation.weight * error * error;
  }
  result.std_rad = std::sqrt(variance / std::max(1.0, sum_weight));
  result.valid = result.sample_count >= options_.min_samples &&
    result.baseline_m >= options_.min_distance_m &&
    result.std_rad <= options_.max_std_rad;
  return result;
}

void HeadingAlignmentEstimator::reset() noexcept
{
  observations_.clear();
  filtered_delta_yaw_rad_.reset();
}

CourseFromPositionEstimator::CourseFromPositionEstimator(CourseEstimatorOptions options)
: options_(options)
{
  options_.max_samples = std::max<std::size_t>(2U, options_.max_samples);
  options_.max_baseline_m = std::max(options_.max_baseline_m, options_.min_baseline_m);
}

bool CourseFromPositionEstimator::addSample(const CoursePositionSample & sample) noexcept
{
  if (!options_.enabled) {
    return false;
  }
  if (sample.stamp_ns <= 0 || !std::isfinite(sample.east_m) ||
    !std::isfinite(sample.north_m) || !std::isfinite(sample.speed_mps))
  {
    return false;
  }
  if (!samples_.empty() && sample.stamp_ns < samples_.back().stamp_ns) {
    return false;
  }
  if (!samples_.empty() && sample.stamp_ns == samples_.back().stamp_ns) {
    samples_.back() = sample;
  } else {
    samples_.push_back(sample);
  }

  const auto max_window_ns = static_cast<std::int64_t>(options_.max_window_s * 1.0e9);
  while (samples_.size() > 2U &&
    (samples_.size() > options_.max_samples ||
    samples_.back().stamp_ns - samples_.front().stamp_ns > max_window_ns))
  {
    samples_.pop_front();
  }
  return true;
}

CourseEstimate CourseFromPositionEstimator::estimate() noexcept
{
  if (!options_.enabled) {
    return invalidCourse("DISABLED");
  }
  if (samples_.size() < 2U) {
    return invalidCourse("INSUFFICIENT_SAMPLES");
  }
  const CoursePositionSample & end = samples_.back();
  if (end.speed_mps < options_.min_speed_mps) {
    return invalidCourse("SPEED_TOO_LOW");
  }

  std::optional<CoursePositionSample> start;
  double selected_baseline = 0.0;
  for (auto iterator = samples_.begin(); iterator != std::prev(samples_.end()); ++iterator) {
    const double baseline = distance2d(*iterator, end);
    if (baseline >= options_.min_baseline_m && baseline <= options_.max_baseline_m) {
      if (!start.has_value() || baseline > selected_baseline) {
        start = *iterator;
        selected_baseline = baseline;
      }
    }
  }
  if (!start.has_value()) {
    return invalidCourse("BASELINE_TOO_SHORT");
  }

  const double delta_east = end.east_m - start->east_m;
  const double delta_north = end.north_m - start->north_m;
  const double course_clockwise_from_north = std::atan2(delta_east, delta_north);
  const double yaw_enu = wrapAngleRad(kPi / 2.0 - course_clockwise_from_north);
  if (last_yaw_enu_rad_.has_value() &&
    angularDistance(yaw_enu, *last_yaw_enu_rad_) > options_.max_jump_rad)
  {
    return invalidCourse("COURSE_JUMP");
  }

  last_yaw_enu_rad_ = yaw_enu;
  CourseEstimate result;
  result.valid = true;
  result.yaw_enu_rad = yaw_enu;
  result.baseline_m = selected_baseline;
  result.invalid_reason = "NONE";
  return result;
}

void CourseFromPositionEstimator::reset() noexcept
{
  samples_.clear();
  last_yaw_enu_rad_.reset();
}

std::size_t CourseFromPositionEstimator::sampleCount() const noexcept
{
  return samples_.size();
}

}  // namespace localization

#include "motion_compensation/odometry_timestamp_expander.hpp"

#include <limits>
#include <stdexcept>

namespace motion_compensation
{

OdometryTimestampExpander::OdometryTimestampExpander(
  const std::int64_t sample_period_ns, const bool timestamp_is_first_sample,
  const std::int64_t reset_threshold_ns)
: sample_period_ns_(sample_period_ns),
  reset_threshold_ns_(reset_threshold_ns),
  timestamp_is_first_sample_(timestamp_is_first_sample)
{
  if (sample_period_ns_ <= 0 || reset_threshold_ns_ <= 0) {
    throw std::invalid_argument("采样周期和时间戳回退阈值必须为正数");
  }
}

ExpandedTimestampBundle OdometryTimestampExpander::expand(
  const std::int64_t raw_stamp_ns, const std::size_t sample_count) noexcept
{
  ExpandedTimestampBundle result;
  if (raw_stamp_ns <= 0 || sample_count == 0U ||
    sample_count > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()))
  {
    return result;
  }

  const auto count = static_cast<std::int64_t>(sample_count);
  std::int64_t first_stamp_ns = raw_stamp_ns;
  if (!timestamp_is_first_sample_) {
    const std::int64_t backward_samples = count - 1;
    if (backward_samples > raw_stamp_ns / sample_period_ns_) {
      return result;
    }
    first_stamp_ns -= backward_samples * sample_period_ns_;
  }
  if (first_stamp_ns <= 0 || count - 1 >
    (std::numeric_limits<std::int64_t>::max() - first_stamp_ns) / sample_period_ns_)
  {
    return result;
  }

  // 热重连会让ODIN设备时钟从较小值重新起步。此时必须开启新纪元，
  // 不能继续用+1 ns维持假单调，否则400 Hz位姿会被压缩成不可插值的时间序列。
  if (last_published_stamp_ns_ > 0 && first_stamp_ns < last_published_stamp_ns_ &&
    last_published_stamp_ns_ - first_stamp_ns > reset_threshold_ns_)
  {
    last_published_stamp_ns_ = 0;
    result.epoch_reset = true;
  }
  if (last_published_stamp_ns_ > 0 && first_stamp_ns <= last_published_stamp_ns_) {
    return result;
  }

  result.stamps_ns.reserve(sample_count);
  for (std::int64_t index = 0; index < count; ++index) {
    result.stamps_ns.push_back(first_stamp_ns + index * sample_period_ns_);
  }
  last_published_stamp_ns_ = result.stamps_ns.back();
  return result;
}

}  // namespace motion_compensation

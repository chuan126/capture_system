#ifndef MOTION_COMPENSATION__ODOMETRY_TIMESTAMP_EXPANDER_HPP_
#define MOTION_COMPENSATION__ODOMETRY_TIMESTAMP_EXPANDER_HPP_

#include <cstddef>
#include <cstdint>
#include <vector>

namespace motion_compensation
{

struct ExpandedTimestampBundle
{
  std::vector<std::int64_t> stamps_ns;
  bool epoch_reset{false};
};

class OdometryTimestampExpander
{
public:
  OdometryTimestampExpander(
    std::int64_t sample_period_ns, bool timestamp_is_first_sample,
    std::int64_t reset_threshold_ns);

  ExpandedTimestampBundle expand(std::int64_t raw_stamp_ns, std::size_t sample_count) noexcept;

private:
  std::int64_t sample_period_ns_;
  std::int64_t reset_threshold_ns_;
  std::int64_t last_published_stamp_ns_{0};
  bool timestamp_is_first_sample_;
};

}  // namespace motion_compensation

#endif  // MOTION_COMPENSATION__ODOMETRY_TIMESTAMP_EXPANDER_HPP_

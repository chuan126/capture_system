#ifndef LOCALIZATION__ODOMETRY_BUFFER_HPP_
#define LOCALIZATION__ODOMETRY_BUFFER_HPP_

#include "localization/dead_reckoning.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>

namespace localization
{

std::optional<std::int64_t> applyRtkTimeOffsetNs(
  std::int64_t rtk_stamp_ns, double rtk_time_offset_s) noexcept;

struct OdomSample
{
  std::int64_t stamp_ns{0};
  Vector3d position_m;
  Quaterniond orientation_xyzw;
};

class OdometryBuffer
{
public:
  OdometryBuffer(
    std::int64_t cache_duration_ns, std::int64_t max_interpolation_gap_ns,
    std::size_t max_samples);

  bool add(OdomSample sample) noexcept;
  bool interpolate(std::int64_t stamp_ns, OdomSample & output) const noexcept;
  std::optional<OdomSample> latest() const noexcept;
  std::size_t size() const noexcept;
  void clear() noexcept;

private:
  std::int64_t cache_duration_ns_{0};
  std::int64_t max_interpolation_gap_ns_{0};
  std::size_t max_samples_{1000U};
  std::deque<OdomSample> samples_;
};

}  // namespace localization

#endif  // LOCALIZATION__ODOMETRY_BUFFER_HPP_

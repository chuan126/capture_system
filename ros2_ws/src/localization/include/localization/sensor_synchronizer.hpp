#ifndef LOCALIZATION__SENSOR_SYNCHRONIZER_HPP_
#define LOCALIZATION__SENSOR_SYNCHRONIZER_HPP_

#include <Eigen/Core>
#include <Eigen/Geometry>

#include <cstddef>
#include <cstdint>
#include <deque>

namespace localization
{

struct TimedOrientation
{
  std::int64_t stamp_ns{0};
  Eigen::Quaterniond orientation_odin_from_body{Eigen::Quaterniond::Identity()};
};

struct TimedImuAcceleration
{
  std::int64_t stamp_ns{0};
  Eigen::Vector3d specific_force_body_mps2{Eigen::Vector3d::Zero()};
};

struct SynchronizedImuSample
{
  std::int64_t stamp_ns{0};
  Eigen::Vector3d specific_force_body_mps2{Eigen::Vector3d::Zero()};
  Eigen::Quaterniond orientation_odin_from_body{Eigen::Quaterniond::Identity()};
};

class SensorSynchronizer
{
public:
  enum class AddResult {kAccepted, kRejected, kEpochReset};

  SensorSynchronizer(
    std::int64_t maximum_interpolation_gap_ns,
    std::int64_t timestamp_reset_threshold_ns,
    std::size_t maximum_samples);

  AddResult addOrientation(const TimedOrientation & sample) noexcept;
  AddResult addImu(const TimedImuAcceleration & sample) noexcept;
  bool popSynchronized(SynchronizedImuSample & output) noexcept;
  void clear() noexcept;

  std::size_t pendingImuCount() const noexcept;
  std::size_t orientationCount() const noexcept;
  std::uint64_t droppedImuCount() const noexcept;

private:
  bool interpolateOrientation(
    std::int64_t stamp_ns, Eigen::Quaterniond & orientation) const noexcept;
  void trim() noexcept;

  std::int64_t maximum_interpolation_gap_ns_;
  std::int64_t timestamp_reset_threshold_ns_;
  std::size_t maximum_samples_;
  std::deque<TimedOrientation> orientations_;
  std::deque<TimedImuAcceleration> imu_samples_;
  std::uint64_t dropped_imu_count_{0U};
};

}  // namespace localization

#endif  // LOCALIZATION__SENSOR_SYNCHRONIZER_HPP_

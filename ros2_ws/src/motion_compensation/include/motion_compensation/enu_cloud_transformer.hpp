#ifndef MOTION_COMPENSATION__ENU_CLOUD_TRANSFORMER_HPP_
#define MOTION_COMPENSATION__ENU_CLOUD_TRANSFORMER_HPP_

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace motion_compensation
{

struct PoseSample
{
  std::int64_t stamp_ns{0};
  std::array<double, 3> position_m{};
  std::array<double, 4> quaternion_xyzw{};
};

struct TimedRadarPoint
{
  float x{0.0F};
  float y{0.0F};
  float z{0.0F};
  float offset_time_s{0.0F};
};

struct ImuSample
{
  std::int64_t stamp_ns{0};
  std::array<double, 3> angular_velocity_rad_s{};
  std::array<double, 3> linear_acceleration_m_s2{};
};

struct EnuPoint
{
  float east{0.0F};
  float north{0.0F};
  float up{0.0F};
};

class PoseBuffer
{
public:
  PoseBuffer(std::int64_t cache_duration_ns, std::int64_t max_interpolation_gap_ns);

  bool add(const PoseSample & sample) noexcept;
  bool interpolate(std::int64_t stamp_ns, PoseSample & output) const noexcept;
  bool empty() const noexcept;
  std::int64_t oldestStampNs() const noexcept;
  std::int64_t newestStampNs() const noexcept;

private:
  std::int64_t cache_duration_ns_;
  std::int64_t max_interpolation_gap_ns_;
  std::deque<PoseSample> samples_;
};

class EnuCloudTransformer
{
public:
  EnuCloudTransformer(
    std::int64_t cache_duration_ns, std::int64_t max_interpolation_gap_ns,
    bool use_odometry_translation, double min_gravity_norm_m_s2 = 7.0,
    double max_gravity_norm_m_s2 = 12.0);

  bool addPose(const PoseSample & sample) noexcept;
  bool addImu(const ImuSample & sample) noexcept;
  bool initialized() const noexcept;
  std::int64_t oldestPoseStampNs() const noexcept;
  std::int64_t newestPoseStampNs() const noexcept;
  std::int64_t oldestImuStampNs() const noexcept;
  std::int64_t newestImuStampNs() const noexcept;
  bool transform(
    std::int64_t cloud_stamp_ns, const std::vector<TimedRadarPoint> & input,
    std::vector<EnuPoint> & output, std::string & invalid_reason) const noexcept;

private:
  PoseBuffer pose_buffer_;
  std::int64_t cache_duration_ns_;
  std::int64_t max_interpolation_gap_ns_;
  std::deque<ImuSample> imu_samples_;
  bool use_odometry_translation_;
  double min_gravity_norm_m_s2_;
  double max_gravity_norm_m_s2_;
  bool initialized_{false};
  double Cenu_odom_[9]{};

  bool interpolateImu(std::int64_t stamp_ns, ImuSample & output) const noexcept;
};

}  // namespace motion_compensation

#endif  // MOTION_COMPENSATION__ENU_CLOUD_TRANSFORMER_HPP_

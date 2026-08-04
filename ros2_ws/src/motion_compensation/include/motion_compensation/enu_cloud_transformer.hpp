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

using RotationMatrix3d = std::array<double, 9>;

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
    bool use_odometry_translation, const RotationMatrix3d & lidar_to_odometry_rotation);

  bool addPose(const PoseSample & sample) noexcept;
  bool initialized() const noexcept;
  std::int64_t oldestPoseStampNs() const noexcept;
  std::int64_t newestPoseStampNs() const noexcept;
  bool transform(
    std::int64_t cloud_stamp_ns, const std::vector<TimedRadarPoint> & input,
    std::vector<EnuPoint> & output, std::string & invalid_reason) const noexcept;

private:
  PoseBuffer pose_buffer_;
  bool use_odometry_translation_;
  RotationMatrix3d lidar_to_odometry_rotation_;
};

}  // namespace motion_compensation

#endif  // MOTION_COMPENSATION__ENU_CLOUD_TRANSFORMER_HPP_

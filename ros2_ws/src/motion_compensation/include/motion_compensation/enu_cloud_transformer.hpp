#ifndef MOTION_COMPENSATION__ENU_CLOUD_TRANSFORMER_HPP_
#define MOTION_COMPENSATION__ENU_CLOUD_TRANSFORMER_HPP_

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
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

struct EnuPoint
{
  float east{0.0F};
  float north{0.0F};
  float up{0.0F};
};

enum class TransformMode
{
  kReject,
  kFullSe3,
  kRotationOnly
};

const char * transformModeName(TransformMode mode) noexcept;

struct TransformStatistics
{
  std::size_t input_point_count{0U};
  std::size_t finite_nonzero_point_count{0U};
  std::size_t pose_covered_point_count{0U};
  std::size_t transformed_point_count{0U};
  std::size_t uncovered_point_count{0U};
  std::size_t invalid_time_point_count{0U};
  bool translation_applied{false};
  bool translation_fallback{false};
  TransformMode mode{TransformMode::kReject};
  double maximum_translation_m{0.0};
  double valid_pose_ratio{0.0};
};

class PoseBuffer
{
public:
  enum class AddResult {kAccepted, kRejected, kGapDetected, kEpochReset};

  PoseBuffer(
    std::int64_t cache_duration_ns, std::int64_t max_interpolation_gap_ns,
    std::int64_t timestamp_reset_threshold_ns = 1000000000LL);

  AddResult add(const PoseSample & sample) noexcept;
  bool interpolate(std::int64_t stamp_ns, PoseSample & output) const noexcept;
  bool empty() const noexcept;
  void clear() noexcept;
  std::int64_t oldestStampNs() const noexcept;
  std::int64_t newestStampNs() const noexcept;
  std::int64_t continuousDurationNs() const noexcept;

  std::vector<PoseSample> snapshot() const;
  std::int64_t maxInterpolationGapNs() const noexcept;

private:
  std::int64_t cache_duration_ns_;
  std::int64_t max_interpolation_gap_ns_;
  std::int64_t timestamp_reset_threshold_ns_;
  mutable std::mutex mutex_;
  std::deque<PoseSample> samples_;
};

class EnuCloudTransformer
{
public:
  EnuCloudTransformer(
    std::int64_t cache_duration_ns, std::int64_t max_interpolation_gap_ns,
    bool use_odometry_translation,
    double minimum_valid_pose_ratio = 0.85,
    double max_translation_per_scan_m = 5.0,
    bool fallback_to_rotation_only = true,
    std::int64_t timestamp_reset_threshold_ns = 1000000000LL);

  PoseBuffer::AddResult addPose(const PoseSample & sample) noexcept;
  bool initialized() const noexcept;
  void clearPoses() noexcept;
  std::int64_t oldestPoseStampNs() const noexcept;
  std::int64_t newestPoseStampNs() const noexcept;
  std::int64_t continuousPoseDurationNs() const noexcept;
  bool transform(
    std::int64_t cloud_stamp_ns, const std::vector<TimedRadarPoint> & input,
    std::vector<EnuPoint> & output, std::string & invalid_reason,
    TransformStatistics * statistics = nullptr) const noexcept;

private:
  PoseBuffer pose_buffer_;
  bool use_odometry_translation_;
  double minimum_valid_pose_ratio_;
  double max_translation_per_scan_m_;
  bool fallback_to_rotation_only_;
};

}  // namespace motion_compensation

#endif  // MOTION_COMPENSATION__ENU_CLOUD_TRANSFORMER_HPP_

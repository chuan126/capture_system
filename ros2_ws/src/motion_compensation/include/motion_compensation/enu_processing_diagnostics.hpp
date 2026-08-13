#ifndef MOTION_COMPENSATION__ENU_PROCESSING_DIAGNOSTICS_HPP_
#define MOTION_COMPENSATION__ENU_PROCESSING_DIAGNOSTICS_HPP_

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace motion_compensation
{

inline constexpr std::int64_t kDefaultProcessingPollIntervalMs = 10;
inline constexpr std::int64_t kMinimumProcessingPollIntervalMs = 1;
inline constexpr std::int64_t kMaximumProcessingPollIntervalMs = 100;

void validateProcessingPollIntervalMs(std::int64_t interval_ms);

struct EnuProcessingDiagnosticsSnapshot
{
  std::size_t pending_cloud_count{0U};
  std::size_t pending_cloud_max_count{0U};
  std::uint64_t clouds_received_total{0U};
  std::uint64_t clouds_processed_total{0U};
  std::uint64_t clouds_dropped_total{0U};
  std::uint64_t pose_wait_count{0U};
  std::uint64_t interpolation_failure_count{0U};
  double queue_wait_ms_last{0.0};
  double queue_wait_ms_mean{0.0};
  double queue_wait_ms_max{0.0};
  double processing_time_ms_last{0.0};
  double processing_time_ms_mean{0.0};
  double processing_time_ms_max{0.0};
};

class EnuProcessingDiagnostics
{
public:
  using Clock = std::chrono::steady_clock;

  void observePendingCloudCount(std::size_t count) noexcept;
  void recordCloudReceived() noexcept;
  void recordCloudProcessed() noexcept;
  void recordCloudDropped() noexcept;
  void recordPoseWait() noexcept;
  void recordInterpolationFailure() noexcept;
  void observeQueueWait(Clock::duration duration) noexcept;
  void observeProcessingTime(Clock::duration duration) noexcept;

  EnuProcessingDiagnosticsSnapshot snapshot() const noexcept;

private:
  static void updateMaximum(std::atomic<std::uint64_t> & target, std::uint64_t value) noexcept;
  static std::uint64_t nonnegativeNanoseconds(Clock::duration duration) noexcept;

  std::atomic<std::size_t> pending_cloud_count_{0U};
  std::atomic<std::size_t> pending_cloud_max_count_{0U};
  std::atomic<std::uint64_t> clouds_received_total_{0U};
  std::atomic<std::uint64_t> clouds_processed_total_{0U};
  std::atomic<std::uint64_t> clouds_dropped_total_{0U};
  std::atomic<std::uint64_t> pose_wait_count_{0U};
  std::atomic<std::uint64_t> interpolation_failure_count_{0U};
  std::atomic<std::uint64_t> queue_wait_ns_last_{0U};
  std::atomic<std::uint64_t> queue_wait_ns_total_{0U};
  std::atomic<std::uint64_t> queue_wait_ns_max_{0U};
  std::atomic<std::uint64_t> queue_wait_sample_count_{0U};
  std::atomic<std::uint64_t> processing_time_ns_last_{0U};
  std::atomic<std::uint64_t> processing_time_ns_total_{0U};
  std::atomic<std::uint64_t> processing_time_ns_max_{0U};
  std::atomic<std::uint64_t> processing_time_sample_count_{0U};
};

}  // namespace motion_compensation

#endif  // MOTION_COMPENSATION__ENU_PROCESSING_DIAGNOSTICS_HPP_

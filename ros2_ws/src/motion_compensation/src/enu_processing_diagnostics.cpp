#include "motion_compensation/enu_processing_diagnostics.hpp"

#include <algorithm>
#include <stdexcept>

namespace motion_compensation
{
namespace
{

double nanosecondsToMilliseconds(const std::uint64_t nanoseconds) noexcept
{
  return static_cast<double>(nanoseconds) / 1.0e6;
}

double meanMilliseconds(
  const std::uint64_t total_nanoseconds, const std::uint64_t sample_count) noexcept
{
  return sample_count == 0U ? 0.0 :
         nanosecondsToMilliseconds(total_nanoseconds) / static_cast<double>(sample_count);
}

}  // namespace

void validateProcessingPollIntervalMs(const std::int64_t interval_ms)
{
  if (interval_ms < kMinimumProcessingPollIntervalMs ||
    interval_ms > kMaximumProcessingPollIntervalMs)
  {
    throw std::invalid_argument("processing_poll_interval_ms必须位于[1, 100] ms范围内");
  }
}

void EnuProcessingDiagnostics::observePendingCloudCount(const std::size_t count) noexcept
{
  pending_cloud_count_.store(count, std::memory_order_relaxed);
  auto maximum = pending_cloud_max_count_.load(std::memory_order_relaxed);
  while (maximum < count && !pending_cloud_max_count_.compare_exchange_weak(
      maximum, count, std::memory_order_relaxed, std::memory_order_relaxed))
  {
  }
}

void EnuProcessingDiagnostics::recordCloudReceived() noexcept
{
  clouds_received_total_.fetch_add(1U, std::memory_order_relaxed);
}

void EnuProcessingDiagnostics::recordCloudProcessed() noexcept
{
  clouds_processed_total_.fetch_add(1U, std::memory_order_relaxed);
}

void EnuProcessingDiagnostics::recordCloudDropped() noexcept
{
  clouds_dropped_total_.fetch_add(1U, std::memory_order_relaxed);
}

void EnuProcessingDiagnostics::recordPoseWait() noexcept
{
  pose_wait_count_.fetch_add(1U, std::memory_order_relaxed);
}

void EnuProcessingDiagnostics::recordInterpolationFailure() noexcept
{
  interpolation_failure_count_.fetch_add(1U, std::memory_order_relaxed);
}

void EnuProcessingDiagnostics::observeQueueWait(const Clock::duration duration) noexcept
{
  const auto nanoseconds = nonnegativeNanoseconds(duration);
  queue_wait_ns_last_.store(nanoseconds, std::memory_order_relaxed);
  queue_wait_ns_total_.fetch_add(nanoseconds, std::memory_order_relaxed);
  queue_wait_sample_count_.fetch_add(1U, std::memory_order_relaxed);
  updateMaximum(queue_wait_ns_max_, nanoseconds);
}

void EnuProcessingDiagnostics::observeProcessingTime(const Clock::duration duration) noexcept
{
  const auto nanoseconds = nonnegativeNanoseconds(duration);
  processing_time_ns_last_.store(nanoseconds, std::memory_order_relaxed);
  processing_time_ns_total_.fetch_add(nanoseconds, std::memory_order_relaxed);
  processing_time_sample_count_.fetch_add(1U, std::memory_order_relaxed);
  updateMaximum(processing_time_ns_max_, nanoseconds);
}

EnuProcessingDiagnosticsSnapshot EnuProcessingDiagnostics::snapshot() const noexcept
{
  EnuProcessingDiagnosticsSnapshot result;
  result.pending_cloud_count = pending_cloud_count_.load(std::memory_order_relaxed);
  result.pending_cloud_max_count = pending_cloud_max_count_.load(std::memory_order_relaxed);
  result.clouds_received_total = clouds_received_total_.load(std::memory_order_relaxed);
  result.clouds_processed_total = clouds_processed_total_.load(std::memory_order_relaxed);
  result.clouds_dropped_total = clouds_dropped_total_.load(std::memory_order_relaxed);
  result.pose_wait_count = pose_wait_count_.load(std::memory_order_relaxed);
  result.interpolation_failure_count =
    interpolation_failure_count_.load(std::memory_order_relaxed);

  const auto queue_wait_total = queue_wait_ns_total_.load(std::memory_order_relaxed);
  const auto queue_wait_count = queue_wait_sample_count_.load(std::memory_order_relaxed);
  result.queue_wait_ms_last = nanosecondsToMilliseconds(
    queue_wait_ns_last_.load(std::memory_order_relaxed));
  result.queue_wait_ms_mean = meanMilliseconds(queue_wait_total, queue_wait_count);
  result.queue_wait_ms_max = nanosecondsToMilliseconds(
    queue_wait_ns_max_.load(std::memory_order_relaxed));

  const auto processing_total = processing_time_ns_total_.load(std::memory_order_relaxed);
  const auto processing_count = processing_time_sample_count_.load(std::memory_order_relaxed);
  result.processing_time_ms_last = nanosecondsToMilliseconds(
    processing_time_ns_last_.load(std::memory_order_relaxed));
  result.processing_time_ms_mean = meanMilliseconds(processing_total, processing_count);
  result.processing_time_ms_max = nanosecondsToMilliseconds(
    processing_time_ns_max_.load(std::memory_order_relaxed));
  return result;
}

void EnuProcessingDiagnostics::updateMaximum(
  std::atomic<std::uint64_t> & target, const std::uint64_t value) noexcept
{
  auto maximum = target.load(std::memory_order_relaxed);
  while (maximum < value && !target.compare_exchange_weak(
      maximum, value, std::memory_order_relaxed, std::memory_order_relaxed))
  {
  }
}

std::uint64_t EnuProcessingDiagnostics::nonnegativeNanoseconds(
  const Clock::duration duration) noexcept
{
  const auto value = std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
  return static_cast<std::uint64_t>(std::max<std::int64_t>(0, value));
}

}  // namespace motion_compensation

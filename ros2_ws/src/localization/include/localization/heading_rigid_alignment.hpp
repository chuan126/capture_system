#ifndef LOCALIZATION__HEADING_RIGID_ALIGNMENT_HPP_
#define LOCALIZATION__HEADING_RIGID_ALIGNMENT_HPP_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace localization
{

struct HeadingFitSample
{
  std::int64_t stamp_ns{0};
  double odin_x_m{0.0};
  double odin_y_m{0.0};
  double rtk_east_m{0.0};
  double rtk_north_m{0.0};
};

struct HeadingRigidFitOptions
{
  bool outlier_rejection_enabled{true};
  double outlier_min_threshold_m{20.0};
  double outlier_mad_multiplier{3.0};
  double min_inlier_ratio{0.7};
};

struct HeadingRigidFitResult
{
  bool valid{false};
  double delta_yaw_rad{0.0};
  double translation_east_m{0.0};
  double translation_north_m{0.0};
  double rmse_m{0.0};
  double residual_median_m{0.0};
  double residual_p95_m{0.0};
  double residual_max_m{0.0};
  std::size_t input_count{0U};
  std::size_t inlier_count{0U};
  double inlier_ratio{0.0};
  double baseline_odin_m{0.0};
  double baseline_rtk_m{0.0};
  double baseline_ratio{0.0};
  double window_span_m{0.0};
  double heading_error_before_rad{0.0};
  double heading_error_after_rad{0.0};
  std::string invalid_reason{"NOT_INITIALIZED"};
};

HeadingRigidFitResult fitHeadingRigid2d(
  const std::vector<HeadingFitSample> & samples,
  const HeadingRigidFitOptions & options = {}) noexcept;

struct HeadingRigidAlignmentOptions
{
  double sample_spacing_m{5.0};
  std::size_t max_samples{100U};
  std::size_t min_samples{3U};
  double min_baseline_m{50.0};
  double valid_baseline_m{100.0};
  double target_baseline_m{500.0};
  double baseline_ratio_min{0.7};
  double baseline_ratio_max{1.3};
  double max_rmse_m{15.0};
  double max_p95_residual_m{25.0};
  bool outlier_rejection_enabled{true};
  double outlier_min_threshold_m{20.0};
  double outlier_mad_multiplier{3.0};
  double min_inlier_ratio{0.7};
  double filter_alpha{0.1};
  double max_update_jump_rad{0.08726646259971647};
};

struct HeadingRigidAlignmentState : public HeadingRigidFitResult
{
  bool estimate_available{false};
  bool update_accepted{false};
  bool target_baseline_reached{false};
};

class HeadingRigidAlignmentEstimator
{
public:
  explicit HeadingRigidAlignmentEstimator(HeadingRigidAlignmentOptions options = {});

  bool addSample(const HeadingFitSample & sample) noexcept;
  const HeadingRigidAlignmentState & state() const noexcept;
  std::size_t sampleCount() const noexcept;
  void reset() noexcept;

private:
  void updateState() noexcept;

  HeadingRigidAlignmentOptions options_;
  std::deque<HeadingFitSample> samples_;
  std::optional<double> filtered_delta_yaw_rad_;
  HeadingRigidAlignmentState state_;
};

}  // namespace localization

#endif  // LOCALIZATION__HEADING_RIGID_ALIGNMENT_HPP_

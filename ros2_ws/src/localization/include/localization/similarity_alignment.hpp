#ifndef LOCALIZATION__SIMILARITY_ALIGNMENT_HPP_
#define LOCALIZATION__SIMILARITY_ALIGNMENT_HPP_

#include <cstddef>
#include <string>
#include <vector>

namespace localization
{

struct Point2d
{
  double x{0.0};
  double y{0.0};
};

struct Similarity2dOptions
{
  std::size_t min_samples{50U};
  double min_baseline_m{500.0};
  double min_scale{0.8};
  double max_scale{1.2};
  double max_rms_residual_m{15.0};
};

struct Similarity2dResult
{
  bool valid{false};
  double scale{1.0};
  double yaw_rad{0.0};
  double translation_x_m{0.0};
  double translation_y_m{0.0};
  double rms_residual_m{0.0};
  double baseline_m{0.0};
  std::string invalid_reason{"NOT_INITIALIZED"};
};

Similarity2dResult estimateSimilarity2d(
  const std::vector<Point2d> & source_odin_xy,
  const std::vector<Point2d> & target_enu_xy,
  const Similarity2dOptions & options = {}) noexcept;

}  // namespace localization

#endif  // LOCALIZATION__SIMILARITY_ALIGNMENT_HPP_

#include "localization/similarity_alignment.hpp"

#include "localization/heading_alignment.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace localization
{
namespace
{

bool finitePoint(const Point2d & point) noexcept
{
  return std::isfinite(point.x) && std::isfinite(point.y);
}

double trajectoryBaseline(const std::vector<Point2d> & points) noexcept
{
  if (points.size() < 2U) {
    return 0.0;
  }
  double min_x = points.front().x;
  double max_x = points.front().x;
  double min_y = points.front().y;
  double max_y = points.front().y;
  for (const Point2d & point : points) {
    min_x = std::min(min_x, point.x);
    max_x = std::max(max_x, point.x);
    min_y = std::min(min_y, point.y);
    max_y = std::max(max_y, point.y);
  }
  return std::hypot(max_x - min_x, max_y - min_y);
}

Similarity2dResult invalidResult(const std::string & reason) noexcept
{
  Similarity2dResult result;
  result.valid = false;
  result.invalid_reason = reason;
  return result;
}

}  // namespace

Similarity2dResult estimateSimilarity2d(
  const std::vector<Point2d> & source_odin_xy,
  const std::vector<Point2d> & target_enu_xy,
  const Similarity2dOptions & options) noexcept
{
  if (source_odin_xy.size() != target_enu_xy.size()) {
    return invalidResult("SIZE_MISMATCH");
  }
  if (source_odin_xy.size() < std::max<std::size_t>(2U, options.min_samples)) {
    return invalidResult("INSUFFICIENT_SAMPLES");
  }

  Point2d source_mean{};
  Point2d target_mean{};
  std::size_t valid_count = 0U;
  for (std::size_t index = 0; index < source_odin_xy.size(); ++index) {
    if (!finitePoint(source_odin_xy[index]) || !finitePoint(target_enu_xy[index])) {
      continue;
    }
    source_mean.x += source_odin_xy[index].x;
    source_mean.y += source_odin_xy[index].y;
    target_mean.x += target_enu_xy[index].x;
    target_mean.y += target_enu_xy[index].y;
    ++valid_count;
  }
  if (valid_count < std::max<std::size_t>(2U, options.min_samples)) {
    return invalidResult("INSUFFICIENT_VALID_SAMPLES");
  }
  const double inverse_count = 1.0 / static_cast<double>(valid_count);
  source_mean.x *= inverse_count;
  source_mean.y *= inverse_count;
  target_mean.x *= inverse_count;
  target_mean.y *= inverse_count;

  const double baseline = trajectoryBaseline(target_enu_xy);
  if (baseline < options.min_baseline_m) {
    Similarity2dResult result = invalidResult("BASELINE_TOO_SHORT");
    result.baseline_m = baseline;
    return result;
  }

  double denominator = 0.0;
  double real_part = 0.0;
  double imaginary_part = 0.0;
  for (std::size_t index = 0; index < source_odin_xy.size(); ++index) {
    if (!finitePoint(source_odin_xy[index]) || !finitePoint(target_enu_xy[index])) {
      continue;
    }
    const double sx = source_odin_xy[index].x - source_mean.x;
    const double sy = source_odin_xy[index].y - source_mean.y;
    const double tx = target_enu_xy[index].x - target_mean.x;
    const double ty = target_enu_xy[index].y - target_mean.y;
    denominator += sx * sx + sy * sy;
    real_part += tx * sx + ty * sy;
    imaginary_part += ty * sx - tx * sy;
  }
  if (denominator <= 1.0e-12 || !std::isfinite(denominator)) {
    return invalidResult("DEGENERATE_SOURCE_TRAJECTORY");
  }

  const double a = real_part / denominator;
  const double b = imaginary_part / denominator;
  const double scale = std::hypot(a, b);
  const double yaw = wrapAngleRad(std::atan2(b, a));
  if (!std::isfinite(scale) || scale < options.min_scale || scale > options.max_scale) {
    Similarity2dResult result = invalidResult("SCALE_OUT_OF_RANGE");
    result.scale = scale;
    result.yaw_rad = yaw;
    result.baseline_m = baseline;
    return result;
  }

  const double cos_yaw = std::cos(yaw);
  const double sin_yaw = std::sin(yaw);
  const double tx = target_mean.x - scale * (cos_yaw * source_mean.x - sin_yaw * source_mean.y);
  const double ty = target_mean.y - scale * (sin_yaw * source_mean.x + cos_yaw * source_mean.y);

  double squared_residual_sum = 0.0;
  for (std::size_t index = 0; index < source_odin_xy.size(); ++index) {
    if (!finitePoint(source_odin_xy[index]) || !finitePoint(target_enu_xy[index])) {
      continue;
    }
    const double predicted_x =
      tx + scale * (cos_yaw * source_odin_xy[index].x - sin_yaw * source_odin_xy[index].y);
    const double predicted_y =
      ty + scale * (sin_yaw * source_odin_xy[index].x + cos_yaw * source_odin_xy[index].y);
    squared_residual_sum +=
      (predicted_x - target_enu_xy[index].x) * (predicted_x - target_enu_xy[index].x) +
      (predicted_y - target_enu_xy[index].y) * (predicted_y - target_enu_xy[index].y);
  }
  const double rms_residual = std::sqrt(squared_residual_sum / static_cast<double>(valid_count));
  if (!std::isfinite(rms_residual) || rms_residual > options.max_rms_residual_m) {
    Similarity2dResult result = invalidResult("RESIDUAL_TOO_LARGE");
    result.scale = scale;
    result.yaw_rad = yaw;
    result.translation_x_m = tx;
    result.translation_y_m = ty;
    result.rms_residual_m = rms_residual;
    result.baseline_m = baseline;
    return result;
  }

  Similarity2dResult result;
  result.valid = true;
  result.scale = scale;
  result.yaw_rad = yaw;
  result.translation_x_m = tx;
  result.translation_y_m = ty;
  result.rms_residual_m = rms_residual;
  result.baseline_m = baseline;
  result.invalid_reason = "NONE";
  return result;
}

}  // namespace localization

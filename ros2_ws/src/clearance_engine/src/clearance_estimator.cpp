#include "clearance_engine/clearance_estimator.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace clearance_engine
{
namespace
{

struct GridKey
{
  std::int64_t y;
  std::int64_t z;
  bool operator==(const GridKey & other) const noexcept {return y == other.y && z == other.z;}
};

struct GridKeyHash
{
  std::size_t operator()(const GridKey & key) const noexcept
  {
    const auto first = std::hash<std::int64_t>{}(key.y);
    const auto second = std::hash<std::int64_t>{}(key.z);
    return first ^ (second + 0x9e3779b97f4a7c15ULL + (first << 6U) + (first >> 2U));
  }
};

using Cells = std::unordered_map<GridKey, std::vector<const Point3f *>, GridKeyHash>;

void validateConfig(const ClearanceConfig & config)
{
  if (!std::isfinite(config.min_detection_x_m) || config.min_detection_x_m < 0.0 ||
    !std::isfinite(config.max_detection_x_m) ||
    config.max_detection_x_m <= config.min_detection_x_m ||
    !std::isfinite(config.detection_radius_m) || config.detection_radius_m <= 0.0 ||
    !std::isfinite(config.support_height_band_m) || config.support_height_band_m <= 0.0 ||
    config.min_support_points == 0U ||
    !std::isfinite(config.spatial_grid_size_m) || config.spatial_grid_size_m <= 0.0 ||
    config.min_occupied_cells == 0U ||
    !std::isfinite(config.min_spatial_span_m) || config.min_spatial_span_m < 0.0)
  {
    throw std::invalid_argument("原始点簇净空参数不合法");
  }
}

std::vector<std::vector<const Point3f *>> connectedComponents(const Cells & cells)
{
  std::unordered_map<GridKey, bool, GridKeyHash> visited;
  visited.reserve(cells.size());
  std::vector<std::vector<const Point3f *>> components;
  for (const auto & entry : cells) {
    if (visited[entry.first]) {
      continue;
    }
    std::deque<GridKey> pending{entry.first};
    visited[entry.first] = true;
    std::vector<const Point3f *> component;
    while (!pending.empty()) {
      const GridKey current = pending.front();
      pending.pop_front();
      const auto current_it = cells.find(current);
      component.insert(component.end(), current_it->second.begin(), current_it->second.end());
      for (std::int64_t dy = -1; dy <= 1; ++dy) {
        for (std::int64_t dz = -1; dz <= 1; ++dz) {
          if (dy == 0 && dz == 0) {
            continue;
          }
          const GridKey neighbor{current.y + dy, current.z + dz};
          if (cells.find(neighbor) != cells.end() && !visited[neighbor]) {
            visited[neighbor] = true;
            pending.push_back(neighbor);
          }
        }
      }
    }
    components.push_back(std::move(component));
  }
  return components;
}

double medianX(std::vector<const Point3f *> points)
{
  std::sort(points.begin(), points.end(), [](const Point3f * a, const Point3f * b) {
    return a->x < b->x;
  });
  const std::size_t middle = points.size() / 2U;
  if (points.size() % 2U != 0U) {
    return points[middle]->x;
  }
  return (static_cast<double>(points[middle - 1U]->x) + points[middle]->x) * 0.5;
}

}  // namespace

ClearanceEstimator::ClearanceEstimator(ClearanceConfig config)
: config_(std::move(config))
{
  validateConfig(config_);
}

const ClearanceConfig & ClearanceEstimator::config() const noexcept {return config_;}

ClearanceEstimate ClearanceEstimator::estimate(const std::vector<Point3f> & points) const
{
  ClearanceEstimate result;
  result.input_point_count = points.size();
  result.detection_radius_m = config_.detection_radius_m;
  std::vector<const Point3f *> valid_points;
  valid_points.reserve(points.size());
  std::vector<const Point3f *> roi;
  roi.reserve(points.size());
  const auto filtering_start = std::chrono::steady_clock::now();
  for (const auto & point : points) {
    if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z) ||
      (point.x == 0.0F && point.y == 0.0F && point.z == 0.0F))
    {
      continue;
    }
    ++result.valid_point_count;
    valid_points.push_back(&point);
  }
  result.filtering_time_ms = std::chrono::duration<double, std::milli>(
    std::chrono::steady_clock::now() - filtering_start).count();
  result.valid_point_ratio = points.empty() ? 0.0 :
    static_cast<double>(result.valid_point_count) / static_cast<double>(points.size());
  if (result.valid_point_count == 0U) {
    result.invalid_reason = "NO_VALID_RAW_POINTS";
    return result;
  }

  const auto roi_start = std::chrono::steady_clock::now();
  const double radius_squared = config_.detection_radius_m * config_.detection_radius_m;
  for (const auto * point : valid_points) {
    const double radial_squared = static_cast<double>(point->y) * point->y +
      static_cast<double>(point->z) * point->z;
    if (point->x >= config_.min_detection_x_m && point->x <= config_.max_detection_x_m &&
      radial_squared <= radius_squared)
    {
      roi.push_back(point);
      result.roi_point_indices.push_back(point->original_index);
    }
  }
  result.roi_time_ms = std::chrono::duration<double, std::milli>(
    std::chrono::steady_clock::now() - roi_start).count();
  result.roi_point_count = roi.size();
  if (roi.empty()) {
    result.invalid_reason = "NO_POINTS_IN_CYLINDRICAL_ROI";
    return result;
  }
  const auto sorting_start = std::chrono::steady_clock::now();
  std::sort(roi.begin(), roi.end(), [](const Point3f * a, const Point3f * b) {
    return a->x < b->x;
  });
  result.sorting_time_ms = std::chrono::duration<double, std::milli>(
    std::chrono::steady_clock::now() - sorting_start).count();
  result.lowest_raw_x = roi.front()->x;
  if (roi.size() < config_.min_support_points) {
    result.invalid_reason = "INSUFFICIENT_ROI_SUPPORT";
    return result;
  }

  std::size_t upper = 0U;
  std::chrono::steady_clock::duration support_band_duration{};
  std::chrono::steady_clock::duration connectivity_duration{};
  for (std::size_t lower = 0U; lower < roi.size(); ++lower) {
    const auto support_start = std::chrono::steady_clock::now();
    upper = std::max(upper, lower);
    const double band_end = static_cast<double>(roi[lower]->x) + config_.support_height_band_m;
    while (upper < roi.size() && roi[upper]->x <= band_end) {
      ++upper;
    }
    support_band_duration += std::chrono::steady_clock::now() - support_start;
    if (upper - lower < config_.min_support_points) {
      continue;
    }
    const auto connectivity_start = std::chrono::steady_clock::now();
    Cells cells;
    cells.reserve(upper - lower);
    for (std::size_t index = lower; index < upper; ++index) {
      const auto * point = roi[index];
      const GridKey key{
        static_cast<std::int64_t>(std::floor(point->y / config_.spatial_grid_size_m)),
        static_cast<std::int64_t>(std::floor(point->z / config_.spatial_grid_size_m))};
      cells[key].push_back(point);
    }
    std::vector<const Point3f *> best_component;
    double best_median = std::numeric_limits<double>::infinity();
    double best_minimum = std::numeric_limits<double>::infinity();
    std::uint32_t best_original_index = std::numeric_limits<std::uint32_t>::max();
    for (auto & component : connectedComponents(cells)) {
      if (component.size() < config_.min_support_points) {
        continue;
      }
      double min_y = component.front()->y;
      double max_y = component.front()->y;
      double min_z = component.front()->z;
      double max_z = component.front()->z;
      std::unordered_map<GridKey, bool, GridKeyHash> component_cells;
      for (const auto * point : component) {
        min_y = std::min(min_y, static_cast<double>(point->y));
        max_y = std::max(max_y, static_cast<double>(point->y));
        min_z = std::min(min_z, static_cast<double>(point->z));
        max_z = std::max(max_z, static_cast<double>(point->z));
        component_cells[GridKey{
          static_cast<std::int64_t>(std::floor(point->y / config_.spatial_grid_size_m)),
          static_cast<std::int64_t>(std::floor(point->z / config_.spatial_grid_size_m))}] = true;
      }
      const double span = std::max(max_y - min_y, max_z - min_z);
      if (component_cells.size() < config_.min_occupied_cells ||
        span < config_.min_spatial_span_m)
      {
        continue;
      }
      const double median = medianX(component);
      double component_minimum = std::numeric_limits<double>::infinity();
      std::uint32_t component_first_index = std::numeric_limits<std::uint32_t>::max();
      for (const auto * point : component) {
        component_minimum = std::min(component_minimum, static_cast<double>(point->x));
        component_first_index = std::min(component_first_index, point->original_index);
      }
      // 连通分量来自unordered_map，业务选择不能依赖遍历顺序。
      if (median < best_median ||
        (median == best_median && component_minimum < best_minimum) ||
        (median == best_median && component_minimum == best_minimum &&
        component_first_index < best_original_index))
      {
        best_component = std::move(component);
        best_median = median;
        best_minimum = component_minimum;
        best_original_index = component_first_index;
      }
    }
    if (!best_component.empty()) {
      result.cluster_min_x = std::numeric_limits<double>::infinity();
      result.cluster_max_x = -std::numeric_limits<double>::infinity();
      const Point3f * representative = best_component.front();
      for (const auto * point : best_component) {
        result.cluster_min_x = std::min(result.cluster_min_x, static_cast<double>(point->x));
        result.cluster_max_x = std::max(result.cluster_max_x, static_cast<double>(point->x));
        const double distance = std::abs(static_cast<double>(point->x) - best_median);
        const double current_distance =
          std::abs(static_cast<double>(representative->x) - best_median);
        if (distance < current_distance ||
          (distance == current_distance && point->original_index < representative->original_index))
        {
          representative = point;
        }
        result.cluster_point_indices.push_back(point->original_index);
      }
      std::sort(result.cluster_point_indices.begin(), result.cluster_point_indices.end());
      result.cluster_median_x = best_median;
      result.representative = *representative;
      result.valid = true;
      result.invalid_reason = "NONE";
      connectivity_duration += std::chrono::steady_clock::now() - connectivity_start;
      result.support_band_time_ms =
        std::chrono::duration<double, std::milli>(support_band_duration).count();
      result.connectivity_time_ms =
        std::chrono::duration<double, std::milli>(connectivity_duration).count();
      return result;
    }
    connectivity_duration += std::chrono::steady_clock::now() - connectivity_start;
    // upper不变时，删除窗口低端点只会让各连通分量的点数、占用格和跨度减小，
    // 不可能从不合格变为合格；直接跳到下一个新点将进入窗口的起点，避免密集失败帧退化为O(N²)。
    if (upper >= roi.size()) {
      break;
    }
    const double next_start_x = static_cast<double>(roi[upper]->x) -
      config_.support_height_band_m;
    const auto next_lower_it = std::lower_bound(
      roi.begin() + static_cast<std::ptrdiff_t>(lower + 1U), roi.end(), next_start_x,
      [](const Point3f * point, const double value) {return point->x < value;});
    if (next_lower_it != roi.end()) {
      lower = static_cast<std::size_t>(std::distance(roi.begin(), next_lower_it)) - 1U;
    }
  }
  result.support_band_time_ms =
    std::chrono::duration<double, std::milli>(support_band_duration).count();
  result.connectivity_time_ms =
    std::chrono::duration<double, std::milli>(connectivity_duration).count();
  result.invalid_reason = "NO_SPATIALLY_SUPPORTED_CLUSTER";
  return result;
}

}  // namespace clearance_engine

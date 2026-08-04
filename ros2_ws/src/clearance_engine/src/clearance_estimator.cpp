#include "clearance_engine/clearance_estimator.hpp"

#include <pcl/ModelCoefficients.h>
#include <pcl/PointIndices.h>
#include <pcl/filters/extract_indices.h>
#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl/segmentation/sac_segmentation.h>

#include <Eigen/Eigenvalues>

#include <algorithm>
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

constexpr double kPi = 3.14159265358979323846;

struct GridKey
{
  std::int64_t y;
  std::int64_t z;

  bool operator==(const GridKey & other) const noexcept
  {
    return y == other.y && z == other.z;
  }
};

struct GridKeyHash
{
  std::size_t operator()(const GridKey & key) const noexcept
  {
    const auto y_hash = std::hash<std::int64_t>{}(key.y);
    const auto z_hash = std::hash<std::int64_t>{}(key.z);
    return y_hash ^ (z_hash + 0x9e3779b97f4a7c15ULL + (y_hash << 6U) + (y_hash >> 2U));
  }
};

using CellPoints = std::unordered_map<GridKey, std::vector<std::size_t>, GridKeyHash>;

struct RegionAnalysis
{
  std::vector<PlaneCandidate> candidates;
  bool has_size_qualified_region{false};
};

double degreesToRadians(const double degrees)
{
  return degrees * kPi / 180.0;
}

double radiansToDegrees(const double radians)
{
  return radians * 180.0 / kPi;
}

double percentile(std::vector<double> values, const double fraction)
{
  if (values.empty()) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  std::sort(values.begin(), values.end());
  const double position = fraction * static_cast<double>(values.size() - 1U);
  const auto lower = static_cast<std::size_t>(std::floor(position));
  const auto upper = static_cast<std::size_t>(std::ceil(position));
  const double weight = position - static_cast<double>(lower);
  return values[lower] * (1.0 - weight) + values[upper] * weight;
}

void validateConfig(const ClearanceConfig & config)
{
  if (!(config.min_range_m > 0.0) || !(config.min_up_height_m > 0.0) ||
    !(config.max_up_height_m > config.min_up_height_m))
  {
    throw std::invalid_argument("净空高度和量程参数不合法");
  }
  if (!(config.lateral_half_angle_deg > 0.0 && config.lateral_half_angle_deg < 90.0) ||
    !(config.longitudinal_half_angle_deg > 0.0 &&
    config.longitudinal_half_angle_deg < 90.0))
  {
    throw std::invalid_argument("顶部角度ROI必须位于0至90度之间");
  }
  if (!(config.max_normal_angle_deg > 0.0 && config.max_normal_angle_deg < 90.0) ||
    !(config.distance_threshold_m > 0.0) || config.max_iterations <= 0 ||
    !(config.probability > 0.0 && config.probability < 1.0))
  {
    throw std::invalid_argument("RANSAC参数不合法");
  }
  if (config.max_candidate_planes <= 0 || config.min_remaining_points < 3U ||
    config.min_inliers_absolute < 3U || !(config.min_inlier_ratio >= 0.0) ||
    !(config.min_inlier_ratio <= 1.0))
  {
    throw std::invalid_argument("多平面提取停止参数不合法");
  }
  if (!(config.region_grid_size_m > 0.0) || config.min_region_span_cells == 0U ||
    config.min_region_occupied_cells == 0U || !(config.max_residual_p95_m > 0.0))
  {
    throw std::invalid_argument("平面连通区域参数不合法");
  }
}

std::vector<std::vector<GridKey>> connectedComponents(const CellPoints & cells)
{
  std::unordered_map<GridKey, bool, GridKeyHash> visited;
  visited.reserve(cells.size());
  std::vector<std::vector<GridKey>> components;

  for (const auto & entry : cells) {
    if (visited[entry.first]) {
      continue;
    }

    std::vector<GridKey> component;
    std::deque<GridKey> pending;
    pending.push_back(entry.first);
    visited[entry.first] = true;

    while (!pending.empty()) {
      const GridKey current = pending.front();
      pending.pop_front();
      component.push_back(current);

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

RegionAnalysis analyzeRegions(
  const pcl::PointCloud<pcl::PointXYZ> & cloud,
  const pcl::PointIndices & inliers,
  std::array<double, 4> coefficients,
  const ClearanceConfig & config)
{
  const double normal_norm = std::sqrt(
    coefficients[0] * coefficients[0] + coefficients[1] * coefficients[1] +
    coefficients[2] * coefficients[2]);
  if (!(normal_norm > std::numeric_limits<double>::epsilon())) {
    return {};
  }
  for (double & value : coefficients) {
    value /= normal_norm;
  }
  if (coefficients[0] < 0.0) {
    for (double & value : coefficients) {
      value = -value;
    }
  }

  const double tilt_deg = radiansToDegrees(
    std::acos(std::clamp(coefficients[0], -1.0, 1.0)));
  if (tilt_deg > config.max_normal_angle_deg ||
    std::abs(coefficients[0]) <= std::numeric_limits<double>::epsilon())
  {
    return {};
  }

  CellPoints cells;
  cells.reserve(inliers.indices.size());
  for (const int raw_index : inliers.indices) {
    if (raw_index < 0 || static_cast<std::size_t>(raw_index) >= cloud.size()) {
      continue;
    }
    const auto & point = cloud[static_cast<std::size_t>(raw_index)];
    const GridKey key{
      static_cast<std::int64_t>(std::floor(point.y / config.region_grid_size_m)),
      static_cast<std::int64_t>(std::floor(point.z / config.region_grid_size_m))};
    cells[key].push_back(static_cast<std::size_t>(raw_index));
  }

  RegionAnalysis analysis;
  for (const auto & component : connectedComponents(cells)) {
    if (component.empty()) {
      continue;
    }
    std::int64_t min_y_cell = component.front().y;
    std::int64_t max_y_cell = component.front().y;
    std::int64_t min_z_cell = component.front().z;
    std::int64_t max_z_cell = component.front().z;
    std::size_t component_point_count = 0U;
    Eigen::Vector3d centroid = Eigen::Vector3d::Zero();
    std::vector<Eigen::Vector3d> component_points;

    for (const GridKey & key : component) {
      min_y_cell = std::min(min_y_cell, key.y);
      max_y_cell = std::max(max_y_cell, key.y);
      min_z_cell = std::min(min_z_cell, key.z);
      max_z_cell = std::max(max_z_cell, key.z);
      const auto cell_it = cells.find(key);
      if (cell_it == cells.end()) {
        continue;
      }
      component_point_count += cell_it->second.size();
      for (const std::size_t index : cell_it->second) {
        const auto & point = cloud[index];
        const Eigen::Vector3d value(point.x, point.y, point.z);
        component_points.push_back(value);
        centroid += value;
      }
    }

    const auto span_y = static_cast<std::size_t>(max_y_cell - min_y_cell + 1);
    const auto span_z = static_cast<std::size_t>(max_z_cell - min_z_cell + 1);
    if (span_y < config.min_region_span_cells ||
      span_z < config.min_region_span_cells ||
      component.size() < config.min_region_occupied_cells)
    {
      continue;
    }
    analysis.has_size_qualified_region = true;

    // RANSAC可能把相距较远但近似共面的区域一起拟合；拆分后必须用本区域重拟合，
    // 否则远处区域会把局部顶面拉斜并放大最低高度抖动。
    if (component_points.size() < 3U) {
      continue;
    }
    centroid /= static_cast<double>(component_points.size());
    Eigen::Matrix3d covariance = Eigen::Matrix3d::Zero();
    for (const auto & point : component_points) {
      const Eigen::Vector3d centered = point - centroid;
      covariance.noalias() += centered * centered.transpose();
    }
    Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> solver(covariance);
    if (solver.info() != Eigen::Success) {
      continue;
    }
    Eigen::Vector3d region_normal = solver.eigenvectors().col(0).normalized();
    if (region_normal.x() < 0.0) {
      region_normal = -region_normal;
    }
    const std::array<double, 4> region_coefficients{
      region_normal.x(), region_normal.y(), region_normal.z(), -region_normal.dot(centroid)};
    const double region_tilt_deg = radiansToDegrees(
      std::acos(std::clamp(region_coefficients[0], -1.0, 1.0)));
    if (region_tilt_deg > config.max_normal_angle_deg ||
      std::abs(region_coefficients[0]) <= std::numeric_limits<double>::epsilon())
    {
      continue;
    }

    std::vector<double> residuals;
    residuals.reserve(component_points.size());
    for (const auto & point : component_points) {
      residuals.push_back(std::abs(
        region_coefficients[0] * point.x() + region_coefficients[1] * point.y() +
        region_coefficients[2] * point.z() + region_coefficients[3]));
    }

    const double residual_median = percentile(residuals, 0.50);
    const double residual_p95 = percentile(residuals, 0.95);
    if (!std::isfinite(residual_p95) || residual_p95 > config.max_residual_p95_m) {
      continue;
    }

    double min_height = std::numeric_limits<double>::infinity();
    double min_y = 0.0;
    double min_z = 0.0;
    for (const GridKey & key : component) {
      const double y = (static_cast<double>(key.y) + 0.5) * config.region_grid_size_m;
      const double z = (static_cast<double>(key.z) + 0.5) * config.region_grid_size_m;
      const double height = -(
        region_coefficients[1] * y + region_coefficients[2] * z +
        region_coefficients[3]) / region_coefficients[0];
      if (height < min_height) {
        min_height = height;
        min_y = y;
        min_z = z;
      }
    }
    if (!std::isfinite(min_height) || min_height < config.min_up_height_m ||
      min_height > config.max_up_height_m)
    {
      continue;
    }

    PlaneCandidate candidate;
    candidate.coefficients = region_coefficients;
    candidate.inlier_count = component_point_count;
    candidate.occupied_cell_count = component.size();
    candidate.area_m2 = static_cast<double>(component.size()) *
      config.region_grid_size_m * config.region_grid_size_m;
    candidate.tilt_deg = region_tilt_deg;
    candidate.residual_median_m = residual_median;
    candidate.residual_p95_m = residual_p95;
    candidate.min_height_m = min_height;
    candidate.min_position_y_m = min_y;
    candidate.min_position_z_m = min_z;
    analysis.candidates.push_back(candidate);
  }
  return analysis;
}

}  // namespace

ClearanceEstimator::ClearanceEstimator(ClearanceConfig config)
: config_(std::move(config))
{
  validateConfig(config_);
}

const ClearanceConfig & ClearanceEstimator::config() const noexcept
{
  return config_;
}

ClearanceEstimate ClearanceEstimator::estimate(const std::vector<Point3f> & points) const
{
  ClearanceEstimate result;
  result.input_point_count = points.size();
  if (points.empty()) {
    result.invalid_reason = "TOO_FEW_VALID_POINTS";
    return result;
  }

  const double lateral_limit = degreesToRadians(config_.lateral_half_angle_deg);
  const double longitudinal_limit = degreesToRadians(config_.longitudinal_half_angle_deg);
  auto filtered = pcl::make_shared<pcl::PointCloud<pcl::PointXYZ>>();
  filtered->reserve(points.size());
  for (const Point3f & point : points) {
    if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z)) {
      continue;
    }
    const double range = std::sqrt(
      static_cast<double>(point.x) * point.x + static_cast<double>(point.y) * point.y +
      static_cast<double>(point.z) * point.z);
    if (range < config_.min_range_m || point.x < config_.min_up_height_m ||
      point.x > config_.max_up_height_m)
    {
      continue;
    }
    if (std::abs(std::atan2(point.y, point.x)) > lateral_limit ||
      std::abs(std::atan2(point.z, point.x)) > longitudinal_limit)
    {
      continue;
    }
    filtered->push_back(pcl::PointXYZ{point.x, point.y, point.z});
  }

  result.valid_point_count = filtered->size();
  result.valid_point_ratio = static_cast<double>(result.valid_point_count) /
    static_cast<double>(result.input_point_count);
  if (filtered->size() < config_.min_remaining_points) {
    result.invalid_reason = "TOO_FEW_VALID_POINTS";
    return result;
  }

  auto remaining = filtered;
  const std::size_t min_inliers = std::max(
    config_.min_inliers_absolute,
    static_cast<std::size_t>(
      std::ceil(config_.min_inlier_ratio * static_cast<double>(filtered->size()))));
  bool plane_model_found = false;
  bool region_too_small = false;

  for (int plane_index = 0;
    plane_index < config_.max_candidate_planes &&
    remaining->size() >= config_.min_remaining_points;
    ++plane_index)
  {
    pcl::SACSegmentation<pcl::PointXYZ> segmentation;
    segmentation.setOptimizeCoefficients(true);
    segmentation.setModelType(pcl::SACMODEL_PERPENDICULAR_PLANE);
    segmentation.setMethodType(pcl::SAC_RANSAC);
    segmentation.setAxis(Eigen::Vector3f::UnitX());
    segmentation.setEpsAngle(degreesToRadians(config_.max_normal_angle_deg));
    segmentation.setDistanceThreshold(config_.distance_threshold_m);
    segmentation.setMaxIterations(config_.max_iterations);
    segmentation.setProbability(config_.probability);
    segmentation.setInputCloud(remaining);

    auto inliers = pcl::make_shared<pcl::PointIndices>();
    auto coefficients = pcl::make_shared<pcl::ModelCoefficients>();
    segmentation.segment(*inliers, *coefficients);
    if (inliers->indices.size() < min_inliers || coefficients->values.size() != 4U) {
      break;
    }
    plane_model_found = true;

    const std::array<double, 4> plane_coefficients{
      coefficients->values[0], coefficients->values[1], coefficients->values[2],
      coefficients->values[3]};
    auto region_analysis = analyzeRegions(*remaining, *inliers, plane_coefficients, config_);

    // 最大剩余平面都达不到配置的网格尺寸时，后续更小平面不再参与首版计算。
    if (!region_analysis.has_size_qualified_region) {
      region_too_small = true;
      break;
    }
    result.candidates.insert(
      result.candidates.end(), region_analysis.candidates.begin(),
      region_analysis.candidates.end());

    pcl::ExtractIndices<pcl::PointXYZ> extractor;
    extractor.setInputCloud(remaining);
    extractor.setIndices(inliers);
    extractor.setNegative(true);
    auto next_remaining = pcl::make_shared<pcl::PointCloud<pcl::PointXYZ>>();
    extractor.filter(*next_remaining);
    remaining = std::move(next_remaining);
  }

  if (result.candidates.empty()) {
    if (region_too_small) {
      result.invalid_reason = "NO_PLANE_PASSED_REGION_SIZE";
    } else if (plane_model_found) {
      result.invalid_reason = "NO_PLANE_PASSED_QUALITY_CHECK";
    } else {
      result.invalid_reason = "NO_PLANE_FOUND";
    }
    return result;
  }

  const auto selected = std::min_element(
    result.candidates.begin(), result.candidates.end(),
    [](const PlaneCandidate & lhs, const PlaneCandidate & rhs) {
      return lhs.min_height_m < rhs.min_height_m;
    });
  result.selected = *selected;
  result.valid = true;
  result.invalid_reason = "NONE";
  return result;
}

}  // namespace clearance_engine

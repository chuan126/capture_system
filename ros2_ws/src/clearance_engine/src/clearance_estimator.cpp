#include "clearance_engine/clearance_estimator.hpp"

#include <pcl/ModelCoefficients.h>
#include <pcl/PointIndices.h>
#include <pcl/filters/extract_indices.h>
#include <pcl/filters/voxel_grid.h>
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
  std::int64_t east;
  std::int64_t north;

  bool operator==(const GridKey & other) const noexcept
  {
    return east == other.east && north == other.north;
  }
};

struct GridKeyHash
{
  std::size_t operator()(const GridKey & key) const noexcept
  {
    const auto east_hash = std::hash<std::int64_t>{}(key.east);
    const auto north_hash = std::hash<std::int64_t>{}(key.north);
    return east_hash ^
           (north_hash + 0x9e3779b97f4a7c15ULL + (east_hash << 6U) + (east_hash >> 2U));
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

double percentileFromSorted(const std::vector<double> & values, const double fraction)
{
  if (values.empty()) {
    return std::numeric_limits<double>::quiet_NaN();
  }
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
  if (!(config.east_half_angle_deg > 0.0 && config.east_half_angle_deg < 90.0) ||
    !(config.north_half_angle_deg > 0.0 && config.north_half_angle_deg < 90.0))
  {
    throw std::invalid_argument("顶部角度ROI必须位于0至90度之间");
  }
  if (!(config.max_normal_angle_deg > 0.0 && config.max_normal_angle_deg < 90.0) ||
    !(config.distance_threshold_m > 0.0) || !std::isfinite(config.voxel_size_m) ||
    !(config.voxel_size_m >= 0.0) ||
    config.max_iterations <= 0 ||
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

      for (std::int64_t de = -1; de <= 1; ++de) {
        for (std::int64_t dn = -1; dn <= 1; ++dn) {
          if (de == 0 && dn == 0) {
            continue;
          }
          const GridKey neighbor{current.east + de, current.north + dn};
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

pcl::PointCloud<pcl::PointXYZ>::Ptr makeRansacSearchCloud(
  const pcl::PointCloud<pcl::PointXYZ>::ConstPtr & input, const double voxel_size_m)
{
  if (!(voxel_size_m > 0.0)) {
    return pcl::make_shared<pcl::PointCloud<pcl::PointXYZ>>(*input);
  }

  auto output = pcl::make_shared<pcl::PointCloud<pcl::PointXYZ>>();
  pcl::VoxelGrid<pcl::PointXYZ> voxel;
  voxel.setInputCloud(input);
  const float leaf = static_cast<float>(voxel_size_m);
  voxel.setLeafSize(leaf, leaf, leaf);
  voxel.filter(*output);
  return output;
}

pcl::PointIndices::Ptr collectOriginalResolutionInliers(
  const pcl::PointCloud<pcl::PointXYZ> & cloud,
  const std::array<double, 4> & coefficients,
  const double distance_threshold_m)
{
  auto inliers = pcl::make_shared<pcl::PointIndices>();
  const double normal_norm = std::sqrt(
    coefficients[0] * coefficients[0] + coefficients[1] * coefficients[1] +
    coefficients[2] * coefficients[2]);
  if (!(normal_norm > std::numeric_limits<double>::epsilon())) {
    return inliers;
  }

  inliers->indices.reserve(cloud.size());
  for (std::size_t index = 0; index < cloud.size(); ++index) {
    const auto & point = cloud[index];
    const double distance = std::abs(
      coefficients[0] * point.x + coefficients[1] * point.y +
      coefficients[2] * point.z + coefficients[3]) / normal_norm;
    if (distance <= distance_threshold_m) {
      inliers->indices.push_back(static_cast<int>(index));
    }
  }
  return inliers;
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
  if (coefficients[2] < 0.0) {
    for (double & value : coefficients) {
      value = -value;
    }
  }

  const double tilt_deg = radiansToDegrees(
    std::acos(std::clamp(coefficients[2], -1.0, 1.0)));
  if (tilt_deg > config.max_normal_angle_deg ||
    std::abs(coefficients[2]) <= std::numeric_limits<double>::epsilon())
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
      static_cast<std::int64_t>(std::floor(point.x / config.region_grid_size_m)),
      static_cast<std::int64_t>(std::floor(point.y / config.region_grid_size_m))};
    cells[key].push_back(static_cast<std::size_t>(raw_index));
  }

  RegionAnalysis analysis;
  for (const auto & component : connectedComponents(cells)) {
    if (component.empty()) {
      continue;
    }
    std::int64_t min_east_cell = component.front().east;
    std::int64_t max_east_cell = component.front().east;
    std::int64_t min_north_cell = component.front().north;
    std::int64_t max_north_cell = component.front().north;
    std::size_t component_point_count = 0U;
    Eigen::Vector3d centroid = Eigen::Vector3d::Zero();
    std::vector<Eigen::Vector3d> component_points;

    for (const GridKey & key : component) {
      min_east_cell = std::min(min_east_cell, key.east);
      max_east_cell = std::max(max_east_cell, key.east);
      min_north_cell = std::min(min_north_cell, key.north);
      max_north_cell = std::max(max_north_cell, key.north);
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

    const auto span_east = static_cast<std::size_t>(max_east_cell - min_east_cell + 1);
    const auto span_north = static_cast<std::size_t>(max_north_cell - min_north_cell + 1);
    if (span_east < config.min_region_span_cells ||
      span_north < config.min_region_span_cells ||
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
    if (region_normal.z() < 0.0) {
      region_normal = -region_normal;
    }
    const std::array<double, 4> region_coefficients{
      region_normal.x(), region_normal.y(), region_normal.z(), -region_normal.dot(centroid)};
    const double region_tilt_deg = radiansToDegrees(
      std::acos(std::clamp(region_coefficients[2], -1.0, 1.0)));
    if (region_tilt_deg > config.max_normal_angle_deg ||
      std::abs(region_coefficients[2]) <= std::numeric_limits<double>::epsilon())
    {
      continue;
    }

    std::vector<double> residuals;
    residuals.reserve(component_points.size());
    for (const auto & point : component_points) {
      residuals.push_back(
        std::abs(
          region_coefficients[0] * point.x() + region_coefficients[1] * point.y() +
          region_coefficients[2] * point.z() + region_coefficients[3]));
    }

    std::sort(residuals.begin(), residuals.end());
    const double residual_median = percentileFromSorted(residuals, 0.50);
    const double residual_p95 = percentileFromSorted(residuals, 0.95);
    if (!std::isfinite(residual_p95) || residual_p95 > config.max_residual_p95_m) {
      continue;
    }

    double min_height = std::numeric_limits<double>::infinity();
    double min_east = 0.0;
    double min_north = 0.0;
    for (const GridKey & key : component) {
      const double east = (static_cast<double>(key.east) + 0.5) * config.region_grid_size_m;
      const double north = (static_cast<double>(key.north) + 0.5) * config.region_grid_size_m;
      const double height = -(
        region_coefficients[0] * east + region_coefficients[1] * north +
        region_coefficients[3]) / region_coefficients[2];
      if (height < min_height) {
        min_height = height;
        min_east = east;
        min_north = north;
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
    candidate.occupied_area_m2 = static_cast<double>(component.size()) *
      config.region_grid_size_m * config.region_grid_size_m;
    candidate.tilt_deg = region_tilt_deg;
    candidate.residual_median_m = residual_median;
    candidate.residual_p95_m = residual_p95;
    candidate.min_height_m = min_height;
    candidate.min_position_east_m = min_east;
    candidate.min_position_north_m = min_north;
    candidate.min_position_up_m = min_height;
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

  const double east_limit = degreesToRadians(config_.east_half_angle_deg);
  const double north_limit = degreesToRadians(config_.north_half_angle_deg);
  auto filtered = pcl::make_shared<pcl::PointCloud<pcl::PointXYZ>>();
  filtered->reserve(points.size());
  for (const Point3f & point : points) {
    if (!std::isfinite(point.east) || !std::isfinite(point.north) ||
      !std::isfinite(point.up))
    {
      continue;
    }
    const double range = std::sqrt(
      static_cast<double>(point.east) * point.east +
      static_cast<double>(point.north) * point.north +
      static_cast<double>(point.up) * point.up);
    if (range < config_.min_range_m || point.up < config_.min_up_height_m ||
      point.up > config_.max_up_height_m)
    {
      continue;
    }
    if (std::abs(std::atan2(point.east, point.up)) > east_limit ||
      std::abs(std::atan2(point.north, point.up)) > north_limit)
    {
      continue;
    }
    filtered->push_back(pcl::PointXYZ{point.east, point.north, point.up});
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
    // RANSAC只在体素降采样点云上搜索模型，减少60 km/h工况下的单帧计算时间。
    // 候选模型随后回到原始ROI点云收集内点，区域拟合和最低高度仍使用原始分辨率。
    const auto search_cloud = makeRansacSearchCloud(remaining, config_.voxel_size_m);
    if (search_cloud->size() < 3U) {
      break;
    }

    pcl::SACSegmentation<pcl::PointXYZ> segmentation;
    segmentation.setOptimizeCoefficients(true);
    segmentation.setModelType(pcl::SACMODEL_PERPENDICULAR_PLANE);
    segmentation.setMethodType(pcl::SAC_RANSAC);
    segmentation.setAxis(Eigen::Vector3f::UnitZ());
    segmentation.setEpsAngle(degreesToRadians(config_.max_normal_angle_deg));
    segmentation.setDistanceThreshold(config_.distance_threshold_m);
    segmentation.setMaxIterations(config_.max_iterations);
    segmentation.setProbability(config_.probability);
    segmentation.setInputCloud(search_cloud);

    auto search_inliers = pcl::make_shared<pcl::PointIndices>();
    auto coefficients = pcl::make_shared<pcl::ModelCoefficients>();
    segmentation.segment(*search_inliers, *coefficients);
    if (search_inliers->indices.size() < 3U || coefficients->values.size() != 4U) {
      break;
    }

    const std::array<double, 4> plane_coefficients{
      coefficients->values[0], coefficients->values[1], coefficients->values[2],
      coefficients->values[3]};
    auto inliers = collectOriginalResolutionInliers(
      *remaining, plane_coefficients, config_.distance_threshold_m);
    if (inliers->indices.size() < min_inliers) {
      break;
    }
    plane_model_found = true;

    auto region_analysis = analyzeRegions(*remaining, *inliers, plane_coefficients, config_);
    if (!region_analysis.has_size_qualified_region) {
      region_too_small = true;
    }
    result.candidates.insert(
      result.candidates.end(), region_analysis.candidates.begin(),
      region_analysis.candidates.end());

    // 即使当前最大平面没有通过面积或残差检查，也删除其内点并继续寻找后续平面。
    // 这样主体顶面、标志牌或碎片不会阻止后面的风机底层平面进入候选集。
    pcl::ExtractIndices<pcl::PointXYZ> extractor;
    extractor.setInputCloud(remaining);
    extractor.setIndices(inliers);
    extractor.setNegative(true);
    auto next_remaining = pcl::make_shared<pcl::PointCloud<pcl::PointXYZ>>();
    extractor.filter(*next_remaining);
    if (next_remaining->size() >= remaining->size()) {
      break;
    }
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

#include "clearance_engine/surface_detector.hpp"

#include <pcl/PointIndices.h>
#include <pcl/features/normal_3d.h>
#include <pcl/filters/voxel_grid.h>
#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl/search/kdtree.h>
#include <pcl/segmentation/region_growing.h>

#include <Eigen/Eigenvalues>
#include <Eigen/QR>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <queue>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace clearance_engine
{
namespace
{

constexpr double kPi = 3.14159265358979323846;
constexpr double kMinimumTieToleranceM = 1.0e-4;
constexpr double kGridBoundaryTolerance = 1.0e-6;

std::int64_t gridCoordinate(const double coordinate, const double grid_size_m)
{
  // 点云坐标是float32时，负方向的理论网格边界可能略小于整数；直接floor会制造空洞，
  // 使内部最低点被误判为边界点。该容差只校正浮点边界误差。
  return static_cast<std::int64_t>(
    std::floor(coordinate / grid_size_m + kGridBoundaryTolerance));
}

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

struct CellStats
{
  double minimum_up_m{std::numeric_limits<double>::infinity()};
  double maximum_up_m{-std::numeric_limits<double>::infinity()};
  std::size_t point_count{0U};
};

struct ClusterEvaluation
{
  std::optional<SurfaceCandidate> candidate;
  SurfaceClusterDiagnostic diagnostic;
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

double clampedRatio(const double value, const double full_score_value)
{
  if (!(full_score_value > 0.0)) {
    return 0.0;
  }
  return std::clamp(value / full_score_value, 0.0, 1.0);
}

double evaluateQuadratic(
  const Eigen::VectorXd & coefficients, const double local_east,
  const double local_north)
{
  return coefficients[0] * local_east * local_east +
         coefficients[1] * local_east * local_north +
         coefficients[2] * local_north * local_north +
         coefficients[3] * local_east + coefficients[4] * local_north +
         coefficients[5];
}

SurfaceGeometryType classifySurface(
  const double lambda_min, const double lambda_max,
  const SurfaceDetectorConfig & config, std::string & reject_reason)
{
  const bool near_plane = std::abs(lambda_min) <=
    config.near_plane_curvature_threshold_per_m &&
    std::abs(lambda_max) <= config.near_plane_curvature_threshold_per_m;
  if (near_plane) {
    reject_reason = "NEAR_PLANE_USE_PLANE_BRANCH";
    return SurfaceGeometryType::kNearPlane;
  }

  const bool positive = lambda_max > config.min_positive_curvature_per_m;
  const bool negative = lambda_min < -config.max_negative_curvature_per_m;
  if (positive && negative) {
    reject_reason = "SADDLE_REJECTED";
    return SurfaceGeometryType::kSaddle;
  }
  if (negative) {
    reject_reason = "UPPER_ARCH_REJECTED";
    return SurfaceGeometryType::kUpperArch;
  }
  if (positive && lambda_min >= -config.max_negative_curvature_per_m) {
    reject_reason = "NONE";
    return SurfaceGeometryType::kLowerArch;
  }

  reject_reason = lambda_max <= config.min_positive_curvature_per_m ?
    "POSITIVE_CURVATURE_TOO_SMALL" : "NEGATIVE_CURVATURE_TOO_LARGE";
  return SurfaceGeometryType::kInvalid;
}

void validateConfig(const SurfaceDetectorConfig & config)
{
  if (!(config.min_range_m > 0.0) || !(config.min_up_height_m > 0.0) ||
    !(config.max_up_height_m > config.min_up_height_m) ||
    !(config.east_half_angle_deg > 0.0 && config.east_half_angle_deg < 90.0) ||
    !(config.north_half_angle_deg > 0.0 && config.north_half_angle_deg < 90.0))
  {
    throw std::invalid_argument("曲面检测ROI参数不合法");
  }
  if (!(config.voxel_size_m > 0.0) || config.normal_k_neighbors < 3 ||
    config.region_neighbor_number < 1 ||
    !(config.smoothness_threshold_deg > 0.0 && config.smoothness_threshold_deg < 90.0) ||
    !(config.curvature_threshold > 0.0))
  {
    throw std::invalid_argument("曲面法向量或区域生长参数不合法");
  }
  if (config.min_cluster_points < 6U || !(config.min_span_m > 0.0) ||
    !(config.grid_size_m > 0.0) || config.min_occupied_cells == 0U ||
    !(config.max_residual_p95_m > 0.0) || !(config.max_curvature > 0.0) ||
    !(config.max_tilt_deg > 0.0 && config.max_tilt_deg < 90.0) ||
    !(config.min_downward_normal_ratio > 0.0 && config.min_downward_normal_ratio <= 1.0) ||
    !(config.min_positive_curvature_per_m > 0.0) ||
    !(config.max_negative_curvature_per_m >= 0.0) ||
    !(config.near_plane_curvature_threshold_per_m >= 0.0 &&
    config.near_plane_curvature_threshold_per_m < config.min_positive_curvature_per_m) ||
    !(config.max_cell_vertical_span_m > 0.0) ||
    !(config.max_bad_vertical_cell_ratio >= 0.0 &&
    config.max_bad_vertical_cell_ratio < 1.0) ||
    !(config.min_minimum_boundary_distance_m >= 0.0) ||
    !(config.min_roi_boundary_distance_m >= 0.0) ||
    config.max_input_points < config.min_cluster_points || config.max_input_points >= 10000U)
  {
    throw std::invalid_argument("曲面候选质量参数不合法");
  }
  if (!(config.min_confidence >= 0.0 && config.min_confidence < 1.0) ||
    !(config.plane_surface_conflict_threshold_m >= 0.0))
  {
    throw std::invalid_argument("曲面融合参数不合法");
  }
}

pcl::PointCloud<pcl::PointXYZ>::Ptr voxelDownsample(
  const pcl::PointCloud<pcl::PointXYZ>::ConstPtr & input,
  const SurfaceDetectorConfig & config)
{
  auto output = pcl::make_shared<pcl::PointCloud<pcl::PointXYZ>>();
  double leaf_size = config.voxel_size_m;
  for (int attempt = 0; attempt < 4; ++attempt) {
    pcl::VoxelGrid<pcl::PointXYZ> voxel;
    voxel.setInputCloud(input);
    const float leaf = static_cast<float>(leaf_size);
    voxel.setLeafSize(leaf, leaf, leaf);
    voxel.filter(*output);
    if (output->size() <= config.max_input_points) {
      return output;
    }
    const double ratio = static_cast<double>(output->size()) /
      static_cast<double>(config.max_input_points);
    leaf_size *= std::max(1.10, std::cbrt(ratio) * 1.05);
  }

  auto capped = pcl::make_shared<pcl::PointCloud<pcl::PointXYZ>>();
  capped->reserve(config.max_input_points);
  const std::size_t stride = static_cast<std::size_t>(std::ceil(
      static_cast<double>(output->size()) / static_cast<double>(config.max_input_points)));
  for (std::size_t index = 0U; index < output->size() &&
    capped->size() < config.max_input_points; index += stride)
  {
    capped->push_back((*output)[index]);
  }
  capped->width = static_cast<std::uint32_t>(capped->size());
  capped->height = 1U;
  capped->is_dense = true;
  return capped;
}

std::unordered_map<GridKey, std::size_t, GridKeyHash> calculateBoundaryDistances(
  const std::unordered_set<GridKey, GridKeyHash> & occupied_cells)
{
  static constexpr std::array<GridKey, 4> kNeighbours = {
    GridKey{1, 0}, GridKey{-1, 0}, GridKey{0, 1}, GridKey{0, -1}};
  const std::size_t unreachable = std::numeric_limits<std::size_t>::max();
  std::unordered_map<GridKey, std::size_t, GridKeyHash> distances;
  distances.reserve(occupied_cells.size());
  std::queue<GridKey> pending;

  for (const GridKey & cell : occupied_cells) {
    bool boundary = false;
    for (const GridKey & offset : kNeighbours) {
      if (occupied_cells.count(GridKey{cell.east + offset.east, cell.north + offset.north}) == 0U) {
        boundary = true;
        break;
      }
    }
    distances.emplace(cell, boundary ? 0U : unreachable);
    if (boundary) {
      pending.push(cell);
    }
  }

  while (!pending.empty()) {
    const GridKey current = pending.front();
    pending.pop();
    const std::size_t next_distance = distances.at(current) + 1U;
    for (const GridKey & offset : kNeighbours) {
      const GridKey neighbour{current.east + offset.east, current.north + offset.north};
      auto found = distances.find(neighbour);
      if (found != distances.end() && found->second > next_distance) {
        found->second = next_distance;
        pending.push(neighbour);
      }
    }
  }
  return distances;
}

ClusterEvaluation fitCluster(
  const pcl::PointCloud<pcl::PointXYZ> & cloud,
  const pcl::PointCloud<pcl::Normal> & normals,
  const pcl::PointIndices & cluster,
  const SurfaceDetectorConfig & config)
{
  ClusterEvaluation evaluation;
  SurfaceClusterDiagnostic & diagnostic = evaluation.diagnostic;
  diagnostic.point_count = cluster.indices.size();
  const auto reject = [&evaluation](const char * reason) {
      evaluation.diagnostic.accepted = false;
      evaluation.diagnostic.reject_reason = reason;
      return evaluation;
    };

  if (cluster.indices.size() < config.min_cluster_points) {
    return reject("REGION_TOO_SMALL");
  }

  double min_east = std::numeric_limits<double>::infinity();
  double max_east = -std::numeric_limits<double>::infinity();
  double min_north = std::numeric_limits<double>::infinity();
  double max_north = -std::numeric_limits<double>::infinity();
  double origin_east = 0.0;
  double origin_north = 0.0;
  Eigen::Vector3d average_normal = Eigen::Vector3d::Zero();
  std::size_t downward_normal_count = 0U;
  std::vector<std::size_t> indices;
  indices.reserve(cluster.indices.size());
  const double minimum_downward_z = std::cos(degreesToRadians(config.max_tilt_deg));

  for (const int raw_index : cluster.indices) {
    if (raw_index < 0 || static_cast<std::size_t>(raw_index) >= cloud.size() ||
      static_cast<std::size_t>(raw_index) >= normals.size())
    {
      continue;
    }
    const std::size_t index = static_cast<std::size_t>(raw_index);
    const auto & point = cloud[index];
    const auto & normal = normals[index];
    if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z) ||
      !std::isfinite(normal.normal_x) || !std::isfinite(normal.normal_y) ||
      !std::isfinite(normal.normal_z) || !std::isfinite(normal.curvature))
    {
      continue;
    }
    indices.push_back(index);
    min_east = std::min(min_east, static_cast<double>(point.x));
    max_east = std::max(max_east, static_cast<double>(point.x));
    min_north = std::min(min_north, static_cast<double>(point.y));
    max_north = std::max(max_north, static_cast<double>(point.y));
    origin_east += point.x;
    origin_north += point.y;
    average_normal += Eigen::Vector3d(normal.normal_x, normal.normal_y, normal.normal_z);
    if (normal.normal_z <= -minimum_downward_z) {
      ++downward_normal_count;
    }
  }
  diagnostic.point_count = indices.size();
  if (indices.size() < config.min_cluster_points) {
    return reject("REGION_TOO_SMALL");
  }

  const double east_span = max_east - min_east;
  const double north_span = max_north - min_north;
  if (east_span < config.min_span_m || north_span < config.min_span_m) {
    return reject("REGION_TOO_SMALL");
  }
  origin_east /= static_cast<double>(indices.size());
  origin_north /= static_cast<double>(indices.size());

  Eigen::MatrixXd design(static_cast<Eigen::Index>(indices.size()), 6);
  Eigen::VectorXd heights(static_cast<Eigen::Index>(indices.size()));
  for (std::size_t row = 0U; row < indices.size(); ++row) {
    const auto & point = cloud[indices[row]];
    const double east = static_cast<double>(point.x) - origin_east;
    const double north = static_cast<double>(point.y) - origin_north;
    design.row(static_cast<Eigen::Index>(row)) <<
      east * east, east * north, north * north, east, north, 1.0;
    heights(static_cast<Eigen::Index>(row)) = point.z;
  }
  Eigen::ColPivHouseholderQR<Eigen::MatrixXd> decomposition(design);
  if (decomposition.rank() < 6) {
    return reject("FIT_RANK_DEFICIENT");
  }
  const Eigen::VectorXd coefficients = decomposition.solve(heights);
  if (!coefficients.allFinite()) {
    return reject("FIT_COEFFICIENTS_INVALID");
  }

  Eigen::Matrix2d hessian;
  hessian << 2.0 * coefficients[0], coefficients[1],
    coefficients[1], 2.0 * coefficients[2];
  const Eigen::SelfAdjointEigenSolver<Eigen::Matrix2d> eigen_solver(hessian);
  if (eigen_solver.info() != Eigen::Success || !eigen_solver.eigenvalues().allFinite()) {
    return reject("HESSIAN_INVALID");
  }
  diagnostic.lambda_min_per_m = eigen_solver.eigenvalues()[0];
  diagnostic.lambda_max_per_m = eigen_solver.eigenvalues()[1];
  std::string classification_reason;
  diagnostic.surface_type = classifySurface(
    diagnostic.lambda_min_per_m, diagnostic.lambda_max_per_m, config,
    classification_reason);
  if (diagnostic.surface_type != SurfaceGeometryType::kLowerArch) {
    evaluation.diagnostic.reject_reason = classification_reason;
    return evaluation;
  }

  const double normal_norm = average_normal.norm();
  if (!(normal_norm > std::numeric_limits<double>::epsilon())) {
    return reject("NORMAL_DIRECTION_INVALID");
  }
  average_normal /= normal_norm;
  diagnostic.average_tilt_deg = radiansToDegrees(
    std::acos(std::clamp(-average_normal.z(), -1.0, 1.0)));
  diagnostic.downward_normal_ratio = static_cast<double>(downward_normal_count) /
    static_cast<double>(indices.size());
  if (!std::isfinite(diagnostic.average_tilt_deg) ||
    diagnostic.average_tilt_deg > config.max_tilt_deg)
  {
    return reject("NORMAL_DIRECTION_INVALID");
  }
  if (diagnostic.downward_normal_ratio < config.min_downward_normal_ratio) {
    return reject("DOWNWARD_NORMAL_RATIO_LOW");
  }

  std::vector<double> residuals;
  std::vector<double> curvatures;
  residuals.reserve(indices.size());
  curvatures.reserve(indices.size());
  std::unordered_map<GridKey, CellStats, GridKeyHash> cell_statistics;
  cell_statistics.reserve(indices.size());
  for (const std::size_t index : indices) {
    const auto & point = cloud[index];
    const double local_east = static_cast<double>(point.x) - origin_east;
    const double local_north = static_cast<double>(point.y) - origin_north;
    const double predicted = evaluateQuadratic(coefficients, local_east, local_north);
    residuals.push_back(std::abs(static_cast<double>(point.z) - predicted));
    curvatures.push_back(normals[index].curvature);
    const GridKey key{
      gridCoordinate(point.x, config.grid_size_m),
      gridCoordinate(point.y, config.grid_size_m)};
    CellStats & statistics = cell_statistics[key];
    statistics.minimum_up_m = std::min(statistics.minimum_up_m, static_cast<double>(point.z));
    statistics.maximum_up_m = std::max(statistics.maximum_up_m, static_cast<double>(point.z));
    ++statistics.point_count;
  }
  diagnostic.occupied_cell_count = cell_statistics.size();
  if (cell_statistics.size() < config.min_occupied_cells) {
    return reject("OCCUPIED_CELLS_TOO_FEW");
  }

  std::size_t bad_vertical_cells = 0U;
  std::unordered_set<GridKey, GridKeyHash> occupied_cells;
  occupied_cells.reserve(cell_statistics.size());
  for (const auto & entry : cell_statistics) {
    occupied_cells.insert(entry.first);
    if (entry.second.maximum_up_m - entry.second.minimum_up_m >
      config.max_cell_vertical_span_m)
    {
      ++bad_vertical_cells;
    }
  }
  diagnostic.vertical_bad_cell_ratio = static_cast<double>(bad_vertical_cells) /
    static_cast<double>(cell_statistics.size());
  if (diagnostic.vertical_bad_cell_ratio > config.max_bad_vertical_cell_ratio) {
    diagnostic.surface_type = SurfaceGeometryType::kVerticalSurface;
    return reject("VERTICAL_SURFACE_LIKE");
  }

  std::sort(residuals.begin(), residuals.end());
  std::sort(curvatures.begin(), curvatures.end());
  const double residual_median = percentileFromSorted(residuals, 0.50);
  const double residual_p95 = percentileFromSorted(residuals, 0.95);
  const double curvature_p95 = percentileFromSorted(curvatures, 0.95);
  diagnostic.residual_p95_m = residual_p95;
  if (!std::isfinite(residual_p95) || residual_p95 > config.max_residual_p95_m) {
    return reject("FIT_RESIDUAL_TOO_HIGH");
  }
  if (!std::isfinite(curvature_p95) || curvature_p95 > config.max_curvature) {
    return reject("PCL_CURVATURE_TOO_HIGH");
  }

  const auto boundary_distances = calculateBoundaryDistances(occupied_cells);
  if (boundary_distances.size() != occupied_cells.size()) {
    return reject("OUTSIDE_SUPPORTED_DOMAIN");
  }
  double minimum_height = std::numeric_limits<double>::infinity();
  double minimum_east = 0.0;
  double minimum_north = 0.0;
  std::size_t best_boundary_distance_cells = 0U;
  for (const GridKey & cell : occupied_cells) {
    const double east = (static_cast<double>(cell.east) + 0.5) * config.grid_size_m;
    const double north = (static_cast<double>(cell.north) + 0.5) * config.grid_size_m;
    const double height = evaluateQuadratic(
      coefficients, east - origin_east, north - origin_north);
    const std::size_t boundary_distance_cells = boundary_distances.at(cell);
    if (height < minimum_height - kMinimumTieToleranceM ||
      (std::abs(height - minimum_height) <= kMinimumTieToleranceM &&
      boundary_distance_cells > best_boundary_distance_cells))
    {
      minimum_height = height;
      minimum_east = east;
      minimum_north = north;
      best_boundary_distance_cells = boundary_distance_cells;
    }
  }
  diagnostic.minimum_height_m = minimum_height;
  if (!std::isfinite(minimum_height) || minimum_height < config.min_up_height_m ||
    minimum_height > config.max_up_height_m)
  {
    return reject("OUTSIDE_CLEARANCE_HEIGHT_RANGE");
  }

  diagnostic.distance_to_boundary_m =
    static_cast<double>(best_boundary_distance_cells) * config.grid_size_m;
  diagnostic.minimum_is_interior =
    diagnostic.distance_to_boundary_m + 1.0e-12 >=
    config.min_minimum_boundary_distance_m;
  if (!diagnostic.minimum_is_interior) {
    return reject("MINIMUM_ON_BOUNDARY");
  }

  const double east_roi_margin = minimum_height *
    std::tan(degreesToRadians(config.east_half_angle_deg)) - std::abs(minimum_east);
  const double north_roi_margin = minimum_height *
    std::tan(degreesToRadians(config.north_half_angle_deg)) - std::abs(minimum_north);
  diagnostic.distance_to_roi_boundary_m = std::min(east_roi_margin, north_roi_margin);
  if (diagnostic.distance_to_roi_boundary_m < config.min_roi_boundary_distance_m) {
    return reject("ROI_BOUNDARY_MINIMUM");
  }

  const double support_score = clampedRatio(
    static_cast<double>(indices.size()), 2.0 * static_cast<double>(config.min_cluster_points));
  const double cell_score = clampedRatio(
    static_cast<double>(occupied_cells.size()),
    2.0 * static_cast<double>(config.min_occupied_cells));
  const double span_score = clampedRatio(
    std::min(east_span, north_span), 2.0 * config.min_span_m);
  const double residual_score = std::clamp(
    1.0 - residual_p95 / config.max_residual_p95_m, 0.0, 1.0);
  const double curvature_score = std::clamp(
    1.0 - curvature_p95 / config.max_curvature, 0.0, 1.0);
  const double normal_score = std::clamp(
    1.0 - diagnostic.average_tilt_deg / config.max_tilt_deg, 0.0, 1.0);
  const double normal_ratio_score = std::clamp(
    (diagnostic.downward_normal_ratio - config.min_downward_normal_ratio) /
    std::max(1.0e-9, 1.0 - config.min_downward_normal_ratio), 0.0, 1.0);
  const double boundary_score = config.min_minimum_boundary_distance_m > 0.0 ?
    clampedRatio(
    diagnostic.distance_to_boundary_m, 2.0 * config.min_minimum_boundary_distance_m) : 1.0;

  SurfaceCandidate candidate;
  candidate.type = SurfaceCandidateType::kLowerArch;
  candidate.valid = true;
  for (Eigen::Index index = 0; index < 6; ++index) {
    candidate.coefficients[static_cast<std::size_t>(index)] = coefficients[index];
  }
  candidate.model_origin_east_m = origin_east;
  candidate.model_origin_north_m = origin_north;
  candidate.min_height_m = minimum_height;
  candidate.min_position_east_m = minimum_east;
  candidate.min_position_north_m = minimum_north;
  candidate.min_position_up_m = minimum_height;
  candidate.point_count = indices.size();
  candidate.occupied_cell_count = occupied_cells.size();
  candidate.area_m2 = static_cast<double>(occupied_cells.size()) *
    config.grid_size_m * config.grid_size_m;
  candidate.tilt_deg = diagnostic.average_tilt_deg;
  candidate.residual_median_m = residual_median;
  candidate.residual_p95_m = residual_p95;
  candidate.curvature = curvature_p95;
  candidate.east_span_m = east_span;
  candidate.north_span_m = north_span;
  candidate.lambda_min_per_m = diagnostic.lambda_min_per_m;
  candidate.lambda_max_per_m = diagnostic.lambda_max_per_m;
  candidate.downward_normal_ratio = diagnostic.downward_normal_ratio;
  candidate.minimum_is_interior = diagnostic.minimum_is_interior;
  candidate.distance_to_boundary_m = diagnostic.distance_to_boundary_m;
  candidate.distance_to_roi_boundary_m = diagnostic.distance_to_roi_boundary_m;
  candidate.vertical_bad_cell_ratio = diagnostic.vertical_bad_cell_ratio;
  candidate.confidence = 0.10 * support_score + 0.10 * cell_score +
    0.10 * span_score + 0.20 * residual_score + 0.10 * curvature_score +
    0.15 * normal_score + 0.15 * normal_ratio_score + 0.10 * boundary_score;
  candidate.reject_reason = "NONE";

  diagnostic.accepted = true;
  diagnostic.reject_reason = "NONE";
  evaluation.candidate = candidate;
  return evaluation;
}

void recordDiagnostic(
  SurfaceDetectionResult & result, const SurfaceClusterDiagnostic & diagnostic)
{
  result.cluster_diagnostics.push_back(diagnostic);
  auto & statistics = result.rejection_statistics;
  switch (diagnostic.surface_type) {
    case SurfaceGeometryType::kLowerArch:
      ++statistics.lower_arch_count;
      if (diagnostic.accepted) {
        ++statistics.accepted_lower_arch_count;
      }
      break;
    case SurfaceGeometryType::kUpperArch:
      ++statistics.upper_arch_count;
      break;
    case SurfaceGeometryType::kSaddle:
      ++statistics.saddle_count;
      break;
    case SurfaceGeometryType::kNearPlane:
      ++statistics.near_plane_count;
      break;
    case SurfaceGeometryType::kVerticalSurface:
      ++statistics.vertical_surface_count;
      break;
    default:
      break;
  }
  if (diagnostic.reject_reason == "MINIMUM_ON_BOUNDARY") {
    ++statistics.minimum_boundary_count;
  } else if (diagnostic.reject_reason == "ROI_BOUNDARY_MINIMUM") {
    ++statistics.roi_boundary_count;
  } else if (diagnostic.reject_reason == "NORMAL_DIRECTION_INVALID" ||
    diagnostic.reject_reason == "DOWNWARD_NORMAL_RATIO_LOW")
  {
    ++statistics.normal_rejected_count;
  } else if (!diagnostic.accepted &&
    diagnostic.surface_type != SurfaceGeometryType::kUpperArch &&
    diagnostic.surface_type != SurfaceGeometryType::kSaddle &&
    diagnostic.surface_type != SurfaceGeometryType::kNearPlane &&
    diagnostic.surface_type != SurfaceGeometryType::kVerticalSurface)
  {
    ++statistics.quality_rejected_count;
  }
}

}  // namespace

SurfaceDetector::SurfaceDetector(SurfaceDetectorConfig config)
: config_(std::move(config))
{
  validateConfig(config_);
}

const SurfaceDetectorConfig & SurfaceDetector::config() const noexcept
{
  return config_;
}

SurfaceDetectionResult SurfaceDetector::detect(const std::vector<Point3f> & points) const
{
  const auto start = std::chrono::steady_clock::now();
  SurfaceDetectionResult result;
  result.input_point_count = points.size();
  const auto finish = [&start](SurfaceDetectionResult output, const std::string & reason) {
      output.invalid_reason = reason;
      output.processing_time_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
      return output;
    };

  if (!config_.enabled) {
    return finish(std::move(result), "SURFACE_DISABLED");
  }
  if (!config_.detect_lower_arch_only) {
    return finish(std::move(result), "LOWER_ARCH_ONLY_DISABLED");
  }

  const double east_limit = degreesToRadians(config_.east_half_angle_deg);
  const double north_limit = degreesToRadians(config_.north_half_angle_deg);
  auto roi_cloud = pcl::make_shared<pcl::PointCloud<pcl::PointXYZ>>();
  roi_cloud->reserve(points.size());
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
      point.up > config_.max_up_height_m ||
      std::abs(std::atan2(point.east, point.up)) > east_limit ||
      std::abs(std::atan2(point.north, point.up)) > north_limit)
    {
      continue;
    }
    roi_cloud->push_back(pcl::PointXYZ{point.east, point.north, point.up});
  }
  result.roi_point_count = roi_cloud->size();
  if (roi_cloud->size() < config_.min_cluster_points) {
    return finish(std::move(result), "TOO_FEW_SURFACE_ROI_POINTS");
  }

  auto downsampled = voxelDownsample(roi_cloud, config_);
  result.downsampled_point_count = downsampled->size();
  if (downsampled->size() < config_.min_cluster_points) {
    return finish(std::move(result), "TOO_FEW_SURFACE_DOWNSAMPLED_POINTS");
  }

  auto search_tree = pcl::make_shared<pcl::search::KdTree<pcl::PointXYZ>>();
  pcl::NormalEstimation<pcl::PointXYZ, pcl::Normal> normal_estimation;
  normal_estimation.setInputCloud(downsampled);
  normal_estimation.setSearchMethod(search_tree);
  normal_estimation.setKSearch(std::min(
      config_.normal_k_neighbors, static_cast<int>(downsampled->size() - 1U)));
  normal_estimation.setViewPoint(0.0F, 0.0F, 0.0F);
  auto normals = pcl::make_shared<pcl::PointCloud<pcl::Normal>>();
  normal_estimation.compute(*normals);

  auto valid_cloud = pcl::make_shared<pcl::PointCloud<pcl::PointXYZ>>();
  auto valid_normals = pcl::make_shared<pcl::PointCloud<pcl::Normal>>();
  valid_cloud->reserve(downsampled->size());
  valid_normals->reserve(normals->size());
  for (std::size_t index = 0U; index < downsampled->size() && index < normals->size(); ++index) {
    const auto & point = (*downsampled)[index];
    const auto & normal = (*normals)[index];
    if (std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z) &&
      std::isfinite(normal.normal_x) && std::isfinite(normal.normal_y) &&
      std::isfinite(normal.normal_z) && std::isfinite(normal.curvature))
    {
      valid_cloud->push_back(point);
      valid_normals->push_back(normal);
    }
  }
  if (valid_cloud->size() < config_.min_cluster_points) {
    return finish(std::move(result), "TOO_FEW_VALID_SURFACE_NORMALS");
  }

  auto region_tree = pcl::make_shared<pcl::search::KdTree<pcl::PointXYZ>>();
  pcl::RegionGrowing<pcl::PointXYZ, pcl::Normal> region_growing;
  region_growing.setMinClusterSize(static_cast<int>(config_.min_cluster_points));
  region_growing.setMaxClusterSize(static_cast<int>(valid_cloud->size()));
  region_growing.setSearchMethod(region_tree);
  region_growing.setNumberOfNeighbours(std::min(
      config_.region_neighbor_number, static_cast<int>(valid_cloud->size() - 1U)));
  region_growing.setInputCloud(valid_cloud);
  region_growing.setInputNormals(valid_normals);
  region_growing.setSmoothModeFlag(true);
  region_growing.setCurvatureTestFlag(true);
  region_growing.setSmoothnessThreshold(
    static_cast<float>(degreesToRadians(config_.smoothness_threshold_deg)));
  region_growing.setCurvatureThreshold(static_cast<float>(config_.curvature_threshold));

  std::vector<pcl::PointIndices> clusters;
  region_growing.extract(clusters);
  result.cluster_count = clusters.size();
  for (const pcl::PointIndices & cluster : clusters) {
    ClusterEvaluation evaluation = fitCluster(*valid_cloud, *valid_normals, cluster, config_);
    recordDiagnostic(result, evaluation.diagnostic);
    if (evaluation.candidate.has_value()) {
      result.candidates.push_back(*evaluation.candidate);
    }
  }
  if (result.candidates.empty()) {
    std::string reason = clusters.empty() ? "NO_SURFACE_CLUSTER" :
      "NO_VALID_LOWER_ARCH";
    if (!result.cluster_diagnostics.empty()) {
      reason += ":" + result.cluster_diagnostics.back().reject_reason;
    }
    return finish(std::move(result), reason);
  }
  result.valid = true;
  return finish(std::move(result), "NONE");
}

}  // namespace clearance_engine

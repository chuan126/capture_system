#include "localization/lidar_localizer.hpp"

#include "localization/finite_attitude_correction.hpp"

#include <Eigen/Eigenvalues>

#include <pcl/common/transforms.h>
#include <pcl/filters/voxel_grid.h>
#include <pcl/registration/icp.h>
#include <pcl/search/kdtree.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace localization
{
namespace
{

constexpr double kPi = 3.14159265358979323846;

}  // namespace

LidarLocalizer::LidarLocalizer(LidarLocalizerConfig config)
: config_(std::move(config)), local_map_(new pcl::PointCloud<pcl::PointXYZ>())
{
  config_.voxel_size_m = std::max(0.02, config_.voxel_size_m);
  config_.minimum_scan_points = std::max<std::size_t>(20U, config_.minimum_scan_points);
  config_.maximum_scan_points = std::max(config_.minimum_scan_points, config_.maximum_scan_points);
  config_.minimum_map_points = std::max<std::size_t>(20U, config_.minimum_map_points);
  config_.maximum_map_points = std::max(config_.minimum_map_points, config_.maximum_map_points);
  config_.map_radius_m = std::max(5.0, config_.map_radius_m);
  config_.maximum_correspondence_distance_m = std::max(
    config_.voxel_size_m, config_.maximum_correspondence_distance_m);
  config_.maximum_iterations = std::max(1, config_.maximum_iterations);
  config_.maximum_fitness_score_m2 = std::max(1.0e-6, config_.maximum_fitness_score_m2);
  config_.maximum_position_correction_m = std::max(
    0.01, config_.maximum_position_correction_m);
  config_.minimum_inlier_ratio = std::clamp(config_.minimum_inlier_ratio, 0.01, 1.0);
  config_.maximum_quality_points = std::max<std::size_t>(
    100U, config_.maximum_quality_points);
  config_.normal_neighbor_count = std::max(5, config_.normal_neighbor_count);
  config_.maximum_surface_variation = std::clamp(
    config_.maximum_surface_variation, 1.0e-4, 1.0);
  config_.minimum_second_eigenvalue_m2 = std::max(
    0.0, config_.minimum_second_eigenvalue_m2);
  config_.degeneracy_relative_eigenvalue = std::clamp(
    config_.degeneracy_relative_eigenvalue, 1.0e-12, 1.0);
  config_.degeneracy_absolute_eigenvalue = std::max(
    1.0e-12, config_.degeneracy_absolute_eigenvalue);
  config_.minimum_observable_dof = std::clamp(config_.minimum_observable_dof, 1, 6);
  config_.unobservable_variance = std::max(1.0, config_.unobservable_variance);
  config_.position_noise_floor_m = std::max(1.0e-4, config_.position_noise_floor_m);
  config_.attitude_noise_floor_deg = std::max(1.0e-4, config_.attitude_noise_floor_deg);
  config_.large_rotation_threshold_deg = std::max(0.0, config_.large_rotation_threshold_deg);
  config_.large_rotation_confirmation_frames = std::max(
    1, config_.large_rotation_confirmation_frames);
  config_.large_rotation_consistency_deg = std::max(
    0.1, config_.large_rotation_consistency_deg);
  config_.large_rotation_minimum_fitness_improvement_ratio = std::clamp(
    config_.large_rotation_minimum_fitness_improvement_ratio, 0.0, 1.0);
  config_.large_rotation_minimum_observable_rotation_dof = std::clamp(
    config_.large_rotation_minimum_observable_rotation_dof, 1, 3);
  config_.map_update_interval = std::max(1, config_.map_update_interval);
}

pcl::PointCloud<pcl::PointXYZ>::Ptr LidarLocalizer::prepareScan(
  const std::vector<Eigen::Vector3f> & points) const
{
  auto input = pcl::PointCloud<pcl::PointXYZ>::Ptr(new pcl::PointCloud<pcl::PointXYZ>());
  input->reserve(std::min(points.size(), config_.maximum_scan_points));
  const std::size_t stride = points.size() > config_.maximum_scan_points ?
    std::max<std::size_t>(
      1U, (points.size() + config_.maximum_scan_points - 1U) /
      config_.maximum_scan_points) : 1U;
  for (std::size_t index = 0; index < points.size(); index += stride) {
    const auto & point = points[index];
    if (!point.array().isFinite().all() || point.isZero(1.0e-8F)) {
      continue;
    }
    input->push_back(pcl::PointXYZ(point.x(), point.y(), point.z()));
  }
  input->width = static_cast<std::uint32_t>(input->size());
  input->height = 1U;
  input->is_dense = true;

  auto filtered = pcl::PointCloud<pcl::PointXYZ>::Ptr(new pcl::PointCloud<pcl::PointXYZ>());
  pcl::VoxelGrid<pcl::PointXYZ> voxel;
  voxel.setLeafSize(
    static_cast<float>(config_.voxel_size_m), static_cast<float>(config_.voxel_size_m),
    static_cast<float>(config_.voxel_size_m));
  voxel.setInputCloud(input);
  voxel.filter(*filtered);
  return filtered;
}

bool LidarLocalizer::geometryIsObservable(
  const pcl::PointCloud<pcl::PointXYZ> & cloud) const
{
  if (cloud.size() < config_.minimum_scan_points) {
    return false;
  }
  Eigen::Vector3d mean = Eigen::Vector3d::Zero();
  for (const auto & point : cloud) {
    mean += Eigen::Vector3d(point.x, point.y, point.z);
  }
  mean /= static_cast<double>(cloud.size());
  Eigen::Matrix3d covariance = Eigen::Matrix3d::Zero();
  for (const auto & point : cloud) {
    const Eigen::Vector3d centered = Eigen::Vector3d(point.x, point.y, point.z) - mean;
    covariance.noalias() += centered * centered.transpose();
  }
  covariance /= static_cast<double>(cloud.size());
  const Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> solver(covariance);
  return solver.info() == Eigen::Success &&
         solver.eigenvalues()[1] >= config_.minimum_second_eigenvalue_m2;
}

LidarLocalizer::AlignmentQuality LidarLocalizer::evaluateAlignment(
  const pcl::PointCloud<pcl::PointXYZ> & scan,
  const Eigen::Matrix4f & map_from_scan) const
{
  AlignmentQuality quality;
  if (scan.empty() || local_map_->empty() || !map_from_scan.array().isFinite().all()) {
    return quality;
  }

  pcl::search::KdTree<pcl::PointXYZ> tree;
  tree.setInputCloud(local_map_);
  const Eigen::Matrix3d rotation = map_from_scan.block<3, 3>(0, 0).cast<double>();
  const Eigen::Vector3d translation = map_from_scan.block<3, 1>(0, 3).cast<double>();
  const double maximum_squared_distance =
    config_.maximum_correspondence_distance_m * config_.maximum_correspondence_distance_m;
  Eigen::Matrix<double, 6, 6> information = Eigen::Matrix<double, 6, 6>::Zero();
  double squared_error_sum = 0.0;
  std::size_t inlier_count = 0U;
  std::size_t evaluated_count = 0U;
  const std::size_t stride = scan.size() > config_.maximum_quality_points ?
    std::max<std::size_t>(
      1U, (scan.size() + config_.maximum_quality_points - 1U) /
      config_.maximum_quality_points) : 1U;
  std::vector<int> neighbor_index(
    static_cast<std::size_t>(config_.normal_neighbor_count));
  std::vector<float> neighbor_squared_distance(
    static_cast<std::size_t>(config_.normal_neighbor_count));

  for (std::size_t point_index = 0; point_index < scan.size(); point_index += stride) {
    const auto & point = scan[point_index];
    ++evaluated_count;
    const Eigen::Vector3d source(point.x, point.y, point.z);
    const Eigen::Vector3d rotated = rotation * source;
    const Eigen::Vector3d aligned = rotated + translation;
    const pcl::PointXYZ query(
      static_cast<float>(aligned.x()), static_cast<float>(aligned.y()),
      static_cast<float>(aligned.z()));
    const int found = tree.nearestKSearch(
      query, config_.normal_neighbor_count, neighbor_index, neighbor_squared_distance);
    if (found < 5 ||
      neighbor_squared_distance[0] > maximum_squared_distance)
    {
      continue;
    }

    Eigen::Vector3d neighborhood_mean = Eigen::Vector3d::Zero();
    for (int neighbor = 0; neighbor < found; ++neighbor) {
      const auto & target = (*local_map_)[static_cast<std::size_t>(neighbor_index[neighbor])];
      neighborhood_mean += Eigen::Vector3d(target.x, target.y, target.z);
    }
    neighborhood_mean /= static_cast<double>(found);
    Eigen::Matrix3d neighborhood_covariance = Eigen::Matrix3d::Zero();
    for (int neighbor = 0; neighbor < found; ++neighbor) {
      const auto & target = (*local_map_)[static_cast<std::size_t>(neighbor_index[neighbor])];
      const Eigen::Vector3d centered =
        Eigen::Vector3d(target.x, target.y, target.z) - neighborhood_mean;
      neighborhood_covariance.noalias() += centered * centered.transpose();
    }
    const Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> normal_solver(
      neighborhood_covariance);
    if (normal_solver.info() != Eigen::Success) {
      continue;
    }
    const double eigenvalue_sum = normal_solver.eigenvalues().sum();
    if (!(eigenvalue_sum > 1.0e-12) ||
      normal_solver.eigenvalues()[0] / eigenvalue_sum > config_.maximum_surface_variation)
    {
      continue;
    }
    const Eigen::Vector3d normal = normal_solver.eigenvectors().col(0).normalized();
    const double point_to_plane_residual = normal.dot(aligned - neighborhood_mean);
    ++inlier_count;
    squared_error_sum += point_to_plane_residual * point_to_plane_residual;
    Eigen::Matrix<double, 1, 6> jacobian;
    jacobian.head<3>() = normal.transpose();
    jacobian.tail<3>() = (rotated.cross(normal)).transpose();
    information.noalias() += jacobian.transpose() * jacobian;
  }

  if (inlier_count < 6U) {
    return quality;
  }
  quality.inlier_ratio = evaluated_count > 0U ?
    static_cast<double>(inlier_count) / static_cast<double>(evaluated_count) : 0.0;
  quality.fitness_score_m2 = squared_error_sum / static_cast<double>(inlier_count);
  quality.normalized_information = information / static_cast<double>(inlier_count);
  if (!std::isfinite(quality.fitness_score_m2) ||
    !quality.normalized_information.array().isFinite().all())
  {
    return quality;
  }

  const Eigen::SelfAdjointEigenSolver<Eigen::Matrix<double, 6, 6>> solver(
    quality.normalized_information);
  if (solver.info() != Eigen::Success) {
    return quality;
  }
  const auto eigenvalues = solver.eigenvalues();
  const double maximum_eigenvalue = std::max(0.0, eigenvalues.maxCoeff());
  const double observable_threshold = std::max(
    config_.degeneracy_absolute_eigenvalue,
    config_.degeneracy_relative_eigenvalue * maximum_eigenvalue);
  Eigen::Matrix<double, 6, 1> variances;
  for (int index = 0; index < 6; ++index) {
    quality.eigenvalues[static_cast<std::size_t>(index)] = eigenvalues[index];
    if (eigenvalues[index] >= observable_threshold) {
      ++quality.observable_dof;
      variances[index] = std::max(
        1.0e-10, quality.fitness_score_m2 / std::max(eigenvalues[index], 1.0e-12));
    } else {
      variances[index] = config_.unobservable_variance;
    }
  }
  quality.covariance = solver.eigenvectors() * variances.asDiagonal() *
    solver.eigenvectors().transpose();
  quality.covariance.block<3, 3>(0, 0).diagonal().array() +=
    config_.position_noise_floor_m * config_.position_noise_floor_m;
  const double attitude_floor_rad = config_.attitude_noise_floor_deg * kPi / 180.0;
  quality.covariance.block<3, 3>(3, 3).diagonal().array() +=
    attitude_floor_rad * attitude_floor_rad;

  const Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> rotation_solver(
    quality.normalized_information.block<3, 3>(3, 3));
  if (rotation_solver.info() == Eigen::Success) {
    const double rotation_max = std::max(0.0, rotation_solver.eigenvalues().maxCoeff());
    const double rotation_threshold = std::max(
      config_.degeneracy_absolute_eigenvalue,
      config_.degeneracy_relative_eigenvalue * rotation_max);
    for (int index = 0; index < 3; ++index) {
      if (rotation_solver.eigenvalues()[index] >= rotation_threshold) {
        ++quality.observable_rotation_dof;
      }
    }
  }
  quality.valid = quality.covariance.array().isFinite().all();
  return quality;
}

LidarLocalizationResult LidarLocalizer::process(
  const std::vector<Eigen::Vector3f> & compensated_scan_local,
  const Eigen::Vector3d & predicted_position_local_m,
  const Eigen::Quaterniond & predicted_orientation_local_from_body)
{
  LidarLocalizationResult result;
  result.map_point_count = local_map_->size();
  if (!config_.enabled) {
    result.reason = "LIDAR_DISABLED";
    return result;
  }
  if (!predicted_position_local_m.array().isFinite().all() ||
    !predicted_orientation_local_from_body.coeffs().array().isFinite().all() ||
    predicted_orientation_local_from_body.norm() <= 1.0e-12)
  {
    result.reason = "INVALID_PREDICTED_POSE";
    return result;
  }
  auto scan = prepareScan(compensated_scan_local);
  result.scan_point_count = scan->size();
  if (scan->size() < config_.minimum_scan_points) {
    result.reason = "INSUFFICIENT_SCAN_POINTS";
    return result;
  }
  if (!geometryIsObservable(*scan)) {
    result.reason = "DEGENERATE_SCAN_GEOMETRY";
    return result;
  }
  result.scan_valid = true;

  Eigen::Matrix4f initial_guess = Eigen::Matrix4f::Identity();
  initial_guess.block<3, 1>(0, 3) = predicted_position_local_m.cast<float>();
  if (local_map_->size() < config_.minimum_map_points) {
    pcl::PointCloud<pcl::PointXYZ> positioned;
    pcl::transformPointCloud(*scan, positioned, initial_guess);
    updateMap(positioned, predicted_position_local_m);
    result.map_initialized = true;
    result.map_point_count = local_map_->size();
    result.reason = "MAP_INITIALIZED";
    return result;
  }

  const AlignmentQuality initial_quality = evaluateAlignment(*scan, initial_guess);
  result.initial_fitness_score_m2 = initial_quality.valid ?
    initial_quality.fitness_score_m2 : std::numeric_limits<double>::infinity();

  pcl::IterativeClosestPoint<pcl::PointXYZ, pcl::PointXYZ> icp;
  icp.setInputSource(scan);
  icp.setInputTarget(local_map_);
  icp.setMaxCorrespondenceDistance(config_.maximum_correspondence_distance_m);
  icp.setMaximumIterations(config_.maximum_iterations);
  icp.setTransformationEpsilon(config_.transformation_epsilon);
  icp.setEuclideanFitnessEpsilon(config_.fitness_epsilon);
  pcl::PointCloud<pcl::PointXYZ> aligned;
  icp.align(aligned, initial_guess);
  if (!icp.hasConverged()) {
    result.reason = "ICP_NOT_CONVERGED";
    return result;
  }

  const Eigen::Matrix4f transform = icp.getFinalTransformation();
  const AlignmentQuality quality = evaluateAlignment(*scan, transform);
  if (!quality.valid) {
    result.reason = "ICP_QUALITY_UNAVAILABLE";
    return result;
  }
  result.fitness_score_m2 = quality.fitness_score_m2;
  result.inlier_ratio = quality.inlier_ratio;
  result.information_eigenvalues = quality.eigenvalues;
  result.observable_dof = quality.observable_dof;
  result.observable_rotation_dof = quality.observable_rotation_dof;
  result.observation_covariance = quality.covariance;

  const Eigen::Matrix3d rotation_correction = transform.block<3, 3>(0, 0).cast<double>();
  const Eigen::Quaterniond correction_quaternion(rotation_correction);
  result.rotation_correction_deg = quaternionAngularDistanceRad(
    Eigen::Quaterniond::Identity(), correction_quaternion) * 180.0 / kPi;
  result.observed_position_local_m = transform.block<3, 1>(0, 3).cast<double>();
  result.correction_norm_m =
    (result.observed_position_local_m - predicted_position_local_m).norm();
  result.observed_orientation_local_from_body =
    (correction_quaternion.normalized() *
    predicted_orientation_local_from_body.normalized()).normalized();
  if (std::isfinite(result.initial_fitness_score_m2) &&
    result.initial_fitness_score_m2 > 1.0e-12)
  {
    result.fitness_improvement_ratio =
      (result.initial_fitness_score_m2 - result.fitness_score_m2) /
      result.initial_fitness_score_m2;
  } else {
    result.fitness_improvement_ratio = 1.0;
  }

  if (!std::isfinite(result.fitness_score_m2) ||
    result.fitness_score_m2 > config_.maximum_fitness_score_m2)
  {
    result.reason = "ICP_FITNESS_REJECTED";
    return result;
  }
  if (!std::isfinite(result.inlier_ratio) ||
    result.inlier_ratio < config_.minimum_inlier_ratio)
  {
    result.reason = "ICP_INLIER_RATIO_REJECTED";
    return result;
  }
  if (result.correction_norm_m > config_.maximum_position_correction_m) {
    result.reason = "ICP_TRANSLATION_REJECTED";
    return result;
  }
  if (result.observable_dof < config_.minimum_observable_dof) {
    result.reason = "ICP_DEGENERATE_INFORMATION";
    return result;
  }

  result.large_rotation =
    result.rotation_correction_deg >= config_.large_rotation_threshold_deg;
  if (result.large_rotation) {
    if (result.observable_rotation_dof <
      config_.large_rotation_minimum_observable_rotation_dof)
    {
      result.reason = "LARGE_ROTATION_DEGENERATE";
      return result;
    }
    if (result.fitness_improvement_ratio <
      config_.large_rotation_minimum_fitness_improvement_ratio)
    {
      result.reason = "LARGE_ROTATION_NO_FITNESS_IMPROVEMENT";
      return result;
    }

    const double consistency_rad = config_.large_rotation_consistency_deg * kPi / 180.0;
    if (pending_large_rotation_count_ == 0 ||
      quaternionAngularDistanceRad(
        pending_large_rotation_, correction_quaternion) > consistency_rad)
    {
      pending_large_rotation_ = correction_quaternion.normalized();
      pending_large_rotation_count_ = 1;
    } else {
      ++pending_large_rotation_count_;
      const double blend = 1.0 / static_cast<double>(pending_large_rotation_count_);
      pending_large_rotation_ = pending_large_rotation_.slerp(
        blend, correction_quaternion.normalized()).normalized();
    }
    result.large_rotation_confirmation_count = pending_large_rotation_count_;
    if (pending_large_rotation_count_ < config_.large_rotation_confirmation_frames) {
      result.large_rotation_pending = true;
      result.reason = "LARGE_ROTATION_PENDING_CONFIRMATION";
      return result;
    }
  } else {
    pending_large_rotation_count_ = 0;
    pending_large_rotation_ = Eigen::Quaterniond::Identity();
  }

  result.update_valid = true;
  result.reason = "NONE";
  ++accepted_update_count_;
  if (accepted_update_count_ % static_cast<std::size_t>(config_.map_update_interval) == 0U) {
    updateMap(aligned, result.observed_position_local_m);
  }
  result.map_point_count = local_map_->size();
  return result;
}

void LidarLocalizer::updateMap(
  const pcl::PointCloud<pcl::PointXYZ> & aligned_scan,
  const Eigen::Vector3d & position_local_m)
{
  *local_map_ += aligned_scan;

  auto cropped = pcl::PointCloud<pcl::PointXYZ>::Ptr(new pcl::PointCloud<pcl::PointXYZ>());
  cropped->reserve(local_map_->size());
  const double radius_squared = config_.map_radius_m * config_.map_radius_m;
  for (const auto & point : *local_map_) {
    const double dx = static_cast<double>(point.x) - position_local_m.x();
    const double dy = static_cast<double>(point.y) - position_local_m.y();
    const double dz = static_cast<double>(point.z) - position_local_m.z();
    if (dx * dx + dy * dy + dz * dz <= radius_squared) {
      cropped->push_back(point);
    }
  }

  pcl::VoxelGrid<pcl::PointXYZ> voxel;
  voxel.setLeafSize(
    static_cast<float>(config_.voxel_size_m), static_cast<float>(config_.voxel_size_m),
    static_cast<float>(config_.voxel_size_m));
  voxel.setInputCloud(cropped);
  auto filtered = pcl::PointCloud<pcl::PointXYZ>::Ptr(new pcl::PointCloud<pcl::PointXYZ>());
  voxel.filter(*filtered);
  if (filtered->size() > config_.maximum_map_points) {
    const std::size_t stride = std::max<std::size_t>(
      1U, (filtered->size() + config_.maximum_map_points - 1U) /
      config_.maximum_map_points);
    auto limited = pcl::PointCloud<pcl::PointXYZ>::Ptr(new pcl::PointCloud<pcl::PointXYZ>());
    limited->reserve(config_.maximum_map_points);
    for (std::size_t index = 0; index < filtered->size(); index += stride) {
      limited->push_back((*filtered)[index]);
      if (limited->size() >= config_.maximum_map_points) {
        break;
      }
    }
    local_map_ = limited;
  } else {
    local_map_ = filtered;
  }
  local_map_->width = static_cast<std::uint32_t>(local_map_->size());
  local_map_->height = 1U;
  local_map_->is_dense = true;
}

void LidarLocalizer::resetMap()
{
  local_map_->clear();
  accepted_update_count_ = 0U;
  pending_large_rotation_count_ = 0;
  pending_large_rotation_ = Eigen::Quaterniond::Identity();
}

void LidarLocalizer::applyReferenceFrameCorrection(
  const Eigen::Vector3d & position_before_local_m,
  const Eigen::Quaterniond & orientation_before_local_from_body,
  const Eigen::Vector3d & position_after_local_m,
  const Eigen::Quaterniond & orientation_after_local_from_body)
{
  if (local_map_->empty() || !position_before_local_m.array().isFinite().all() ||
    !position_after_local_m.array().isFinite().all() ||
    !orientation_before_local_from_body.coeffs().array().isFinite().all() ||
    !orientation_after_local_from_body.coeffs().array().isFinite().all() ||
    orientation_before_local_from_body.norm() <= 1.0e-12 ||
    orientation_after_local_from_body.norm() <= 1.0e-12)
  {
    return;
  }
  const Eigen::Matrix3d rotation =
    orientation_after_local_from_body.normalized().toRotationMatrix() *
    orientation_before_local_from_body.normalized().toRotationMatrix().transpose();
  Eigen::Matrix4f correction = Eigen::Matrix4f::Identity();
  correction.block<3, 3>(0, 0) = rotation.cast<float>();
  correction.block<3, 1>(0, 3) =
    (position_after_local_m - rotation * position_before_local_m).cast<float>();
  pcl::PointCloud<pcl::PointXYZ> corrected;
  pcl::transformPointCloud(*local_map_, corrected, correction);
  *local_map_ = std::move(corrected);
  pending_large_rotation_count_ = 0;
  pending_large_rotation_ = Eigen::Quaterniond::Identity();
}

std::size_t LidarLocalizer::mapPointCount() const noexcept
{
  return local_map_->size();
}

}  // namespace localization

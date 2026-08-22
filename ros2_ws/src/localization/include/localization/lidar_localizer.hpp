#ifndef LOCALIZATION__LIDAR_LOCALIZER_HPP_
#define LOCALIZATION__LIDAR_LOCALIZER_HPP_

#include <Eigen/Core>
#include <Eigen/Geometry>

#include <pcl/point_cloud.h>
#include <pcl/point_types.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace localization
{

using LidarPoseCovariance = Eigen::Matrix<double, 6, 6>;

struct LidarLocalizerConfig
{
  bool enabled{true};
  double voxel_size_m{0.15};
  std::size_t minimum_scan_points{300U};
  std::size_t maximum_scan_points{8000U};
  std::size_t minimum_map_points{500U};
  std::size_t maximum_map_points{80000U};
  double map_radius_m{60.0};
  double maximum_correspondence_distance_m{1.5};
  int maximum_iterations{40};
  double transformation_epsilon{1.0e-5};
  double fitness_epsilon{1.0e-5};
  double maximum_fitness_score_m2{0.20};
  double maximum_position_correction_m{2.0};
  double minimum_inlier_ratio{0.50};
  std::size_t maximum_quality_points{2500U};
  int normal_neighbor_count{10};
  double maximum_surface_variation{0.20};
  double minimum_second_eigenvalue_m2{0.01};
  double degeneracy_relative_eigenvalue{1.0e-4};
  double degeneracy_absolute_eigenvalue{1.0e-5};
  int minimum_observable_dof{4};
  double unobservable_variance{1.0e6};
  double position_noise_floor_m{0.05};
  double attitude_noise_floor_deg{0.20};
  double large_rotation_threshold_deg{5.0};
  int large_rotation_confirmation_frames{3};
  double large_rotation_consistency_deg{8.0};
  double large_rotation_minimum_fitness_improvement_ratio{0.10};
  int large_rotation_minimum_observable_rotation_dof{2};
  int map_update_interval{2};
};

struct LidarLocalizationResult
{
  bool scan_valid{false};
  bool map_initialized{false};
  bool update_valid{false};
  bool large_rotation{false};
  bool large_rotation_pending{false};
  Eigen::Vector3d observed_position_local_m{Eigen::Vector3d::Zero()};
  Eigen::Quaterniond observed_orientation_local_from_body{Eigen::Quaterniond::Identity()};
  LidarPoseCovariance observation_covariance{LidarPoseCovariance::Identity()};
  double initial_fitness_score_m2{0.0};
  double fitness_score_m2{0.0};
  double fitness_improvement_ratio{0.0};
  double inlier_ratio{0.0};
  double correction_norm_m{0.0};
  double rotation_correction_deg{0.0};
  std::array<double, 6> information_eigenvalues{};
  int observable_dof{0};
  int observable_rotation_dof{0};
  int large_rotation_confirmation_count{0};
  std::size_t scan_point_count{0U};
  std::size_t map_point_count{0U};
  std::string reason{"NOT_INITIALIZED"};
};

class LidarLocalizer
{
public:
  explicit LidarLocalizer(LidarLocalizerConfig config = {});

  LidarLocalizationResult process(
    const std::vector<Eigen::Vector3f> & compensated_scan_local,
    const Eigen::Vector3d & predicted_position_local_m,
    const Eigen::Quaterniond & predicted_orientation_local_from_body =
    Eigen::Quaterniond::Identity());

  void resetMap();
  void applyReferenceFrameCorrection(
    const Eigen::Vector3d & position_before_local_m,
    const Eigen::Quaterniond & orientation_before_local_from_body,
    const Eigen::Vector3d & position_after_local_m,
    const Eigen::Quaterniond & orientation_after_local_from_body);
  std::size_t mapPointCount() const noexcept;

private:
  struct AlignmentQuality
  {
    bool valid{false};
    double fitness_score_m2{0.0};
    double inlier_ratio{0.0};
    Eigen::Matrix<double, 6, 6> normalized_information{
      Eigen::Matrix<double, 6, 6>::Zero()};
    std::array<double, 6> eigenvalues{};
    int observable_dof{0};
    int observable_rotation_dof{0};
    LidarPoseCovariance covariance{LidarPoseCovariance::Identity()};
  };

  pcl::PointCloud<pcl::PointXYZ>::Ptr prepareScan(
    const std::vector<Eigen::Vector3f> & points) const;
  bool geometryIsObservable(const pcl::PointCloud<pcl::PointXYZ> & cloud) const;
  AlignmentQuality evaluateAlignment(
    const pcl::PointCloud<pcl::PointXYZ> & scan,
    const Eigen::Matrix4f & map_from_scan) const;
  void updateMap(
    const pcl::PointCloud<pcl::PointXYZ> & aligned_scan,
    const Eigen::Vector3d & position_local_m);

  LidarLocalizerConfig config_;
  pcl::PointCloud<pcl::PointXYZ>::Ptr local_map_;
  std::size_t accepted_update_count_{0U};
  Eigen::Quaterniond pending_large_rotation_{Eigen::Quaterniond::Identity()};
  int pending_large_rotation_count_{0};
};

}  // namespace localization

#endif  // LOCALIZATION__LIDAR_LOCALIZER_HPP_

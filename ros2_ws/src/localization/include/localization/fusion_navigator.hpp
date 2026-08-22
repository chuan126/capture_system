#ifndef LOCALIZATION__FUSION_NAVIGATOR_HPP_
#define LOCALIZATION__FUSION_NAVIGATOR_HPP_

#include <Eigen/Core>
#include <Eigen/Geometry>

#include <cstdint>
#include <string>

namespace localization
{

using FusionCovariance = Eigen::Matrix<double, 12, 12>;
using PoseObservationCovariance = Eigen::Matrix<double, 6, 6>;

struct FusionNavigatorConfig
{
  double gravity_mps2{9.80665};
  double maximum_propagation_interval_s{0.02};
  double maximum_acceleration_mps2{50.0};
  double accelerometer_noise_mps2_sqrt_hz{0.20};
  double odin_attitude_increment_noise_rad_sqrt_hz{0.01};
  double accelerometer_bias_random_walk_mps3_sqrt_hz{0.01};
  double initial_position_std_m{0.10};
  double initial_velocity_std_mps{1.0};
  double initial_attitude_std_rad{0.35};
  double initial_accelerometer_bias_std_mps2{0.30};
  double maximum_translation_position_std_m{1.0};
  double maximum_inertial_only_duration_s{2.0};
  int iterated_update_max_iterations{8};
  double iterated_update_position_tolerance_m{1.0e-4};
  double iterated_update_attitude_tolerance_rad{1.0e-5};
  double large_attitude_correction_rad{5.0 * 3.14159265358979323846 / 180.0};
};

struct FusionState
{
  std::int64_t stamp_ns{0};
  Eigen::Vector3d position_m{Eigen::Vector3d::Zero()};
  Eigen::Vector3d velocity_mps{Eigen::Vector3d::Zero()};
  Eigen::Vector3d accelerometer_bias_mps2{Eigen::Vector3d::Zero()};
  Eigen::Quaterniond orientation_local_from_body{Eigen::Quaterniond::Identity()};
  FusionCovariance covariance{FusionCovariance::Identity()};
  bool local_navigation_valid{false};
  bool translation_quality_valid{false};
  std::string quality_reason{"NOT_INITIALIZED"};
};

class FusionNavigator
{
public:
  explicit FusionNavigator(FusionNavigatorConfig config = {});

  bool initialize(
    std::int64_t stamp_ns, const Eigen::Quaterniond & orientation_odin_from_body) noexcept;

  // ODIN contributes only the adjacent finite rotation increment. IMU acceleration is
  // projected by the corrected fusion attitude, never directly by raw ODIN attitude.
  bool propagate(
    std::int64_t stamp_ns, const Eigen::Vector3d & specific_force_body_mps2,
    const Eigen::Quaterniond & orientation_odin_from_body) noexcept;

  bool correctPosition(
    std::int64_t stamp_ns, const Eigen::Vector3d & observed_position_local_m,
    const Eigen::Matrix3d & observation_covariance,
    const std::string & source) noexcept;

  bool correctPose(
    std::int64_t stamp_ns, const Eigen::Vector3d & observed_position_local_m,
    const Eigen::Quaterniond & observed_orientation_local_from_body,
    const PoseObservationCovariance & observation_covariance,
    const std::string & source) noexcept;

  void rebaseTime(std::int64_t stamp_ns) noexcept;
  void rebaseTimeAndOrientation(
    std::int64_t stamp_ns,
    const Eigen::Quaterniond & orientation_odin_from_body) noexcept;

  bool localOrientation(
    const Eigen::Quaterniond & orientation_odin_from_body,
    Eigen::Quaterniond & orientation_local_from_body) const noexcept;

  const FusionState & state() const noexcept;
  bool initialized() const noexcept;
  std::int64_t lastExternalCorrectionStampNs() const noexcept;
  const std::string & lastCorrectionSource() const noexcept;
  const Eigen::Vector3d & lastAttitudeCorrectionRad() const noexcept;
  int lastUpdateIterationCount() const noexcept;
  bool lastCorrectionWasLargeAngle() const noexcept;

private:
  bool normalizedQuaternion(
    const Eigen::Quaterniond & input, Eigen::Quaterniond & output) const noexcept;
  bool predictFusionOrientation(
    const Eigen::Quaterniond & orientation_odin_from_body,
    Eigen::Quaterniond & predicted_local_from_body) const noexcept;
  bool applyMeasurementUpdate(
    const Eigen::VectorXd & measurement,
    const Eigen::MatrixXd & observation_matrix,
    const Eigen::MatrixXd & observation_covariance,
    bool includes_attitude,
    const Eigen::Quaterniond & observed_orientation_local_from_body,
    const std::string & source) noexcept;
  void updateQuality(std::int64_t stamp_ns) noexcept;

  FusionNavigatorConfig config_;
  FusionState state_;
  Eigen::Quaterniond previous_odin_orientation_{Eigen::Quaterniond::Identity()};
  Eigen::Vector3d previous_navigation_acceleration_mps2_{Eigen::Vector3d::Zero()};
  Eigen::Matrix3d previous_rotation_local_from_body_{Eigen::Matrix3d::Identity()};
  bool initialized_{false};
  bool have_previous_odin_orientation_{false};
  bool have_previous_acceleration_{false};
  std::int64_t last_external_correction_stamp_ns_{0};
  std::string last_correction_source_{"NONE"};
  Eigen::Vector3d last_attitude_correction_rad_{Eigen::Vector3d::Zero()};
  int last_update_iteration_count_{0};
  bool last_correction_was_large_angle_{false};
};

}  // namespace localization

#endif  // LOCALIZATION__FUSION_NAVIGATOR_HPP_

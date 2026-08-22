#include "localization/fusion_navigator.hpp"

#include "localization/attitude_transform.hpp"
#include "localization/finite_attitude_correction.hpp"

#include <Eigen/Cholesky>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace localization
{
namespace
{

constexpr double kNanosecondsToSeconds = 1.0e-9;
constexpr double kQuaternionMinimumNorm = 1.0e-12;

bool finiteVector(const Eigen::Vector3d & vector) noexcept
{
  return vector.array().isFinite().all();
}

template<typename Derived>
bool finiteMatrix(const Eigen::MatrixBase<Derived> & matrix) noexcept
{
  return matrix.array().isFinite().all();
}

Eigen::Matrix3d skewSymmetric(const Eigen::Vector3d & vector) noexcept
{
  Eigen::Matrix3d result;
  result <<
    0.0, -vector.z(), vector.y(),
    vector.z(), 0.0, -vector.x(),
    -vector.y(), vector.x(), 0.0;
  return result;
}

bool finiteState(const FusionState & state) noexcept
{
  return finiteVector(state.position_m) && finiteVector(state.velocity_mps) &&
         finiteVector(state.accelerometer_bias_mps2) &&
         state.orientation_local_from_body.coeffs().array().isFinite().all() &&
         state.orientation_local_from_body.norm() > kQuaternionMinimumNorm &&
         state.covariance.array().isFinite().all();
}

}  // namespace

FusionNavigator::FusionNavigator(FusionNavigatorConfig config)
: config_(std::move(config))
{
  config_.gravity_mps2 = std::max(1.0, config_.gravity_mps2);
  config_.maximum_propagation_interval_s = std::max(
    1.0e-4, config_.maximum_propagation_interval_s);
  config_.maximum_acceleration_mps2 = std::max(1.0, config_.maximum_acceleration_mps2);
  config_.accelerometer_noise_mps2_sqrt_hz = std::max(
    1.0e-6, config_.accelerometer_noise_mps2_sqrt_hz);
  config_.odin_attitude_increment_noise_rad_sqrt_hz = std::max(
    1.0e-7, config_.odin_attitude_increment_noise_rad_sqrt_hz);
  config_.accelerometer_bias_random_walk_mps3_sqrt_hz = std::max(
    1.0e-8, config_.accelerometer_bias_random_walk_mps3_sqrt_hz);
  config_.initial_position_std_m = std::max(1.0e-6, config_.initial_position_std_m);
  config_.initial_velocity_std_mps = std::max(1.0e-6, config_.initial_velocity_std_mps);
  config_.initial_attitude_std_rad = std::max(1.0e-6, config_.initial_attitude_std_rad);
  config_.initial_accelerometer_bias_std_mps2 = std::max(
    1.0e-6, config_.initial_accelerometer_bias_std_mps2);
  config_.maximum_translation_position_std_m = std::max(
    1.0e-3, config_.maximum_translation_position_std_m);
  config_.maximum_inertial_only_duration_s = std::max(
    0.0, config_.maximum_inertial_only_duration_s);
  config_.iterated_update_max_iterations = std::max(1, config_.iterated_update_max_iterations);
  config_.iterated_update_position_tolerance_m = std::max(
    1.0e-8, config_.iterated_update_position_tolerance_m);
  config_.iterated_update_attitude_tolerance_rad = std::max(
    1.0e-9, config_.iterated_update_attitude_tolerance_rad);
  config_.large_attitude_correction_rad = std::max(
    0.0, config_.large_attitude_correction_rad);
}

bool FusionNavigator::normalizedQuaternion(
  const Eigen::Quaterniond & input, Eigen::Quaterniond & output) const noexcept
{
  if (!input.coeffs().array().isFinite().all()) {
    return false;
  }
  const double norm = input.norm();
  if (!std::isfinite(norm) || norm <= kQuaternionMinimumNorm) {
    return false;
  }
  output = input.normalized();
  return output.coeffs().array().isFinite().all();
}

bool FusionNavigator::initialize(
  const std::int64_t stamp_ns,
  const Eigen::Quaterniond & orientation_odin_from_body) noexcept
{
  Eigen::Quaterniond normalized;
  if (stamp_ns <= 0 || !normalizedQuaternion(orientation_odin_from_body, normalized)) {
    return false;
  }

  double reference_row_major[9];
  if (!initializeGravityAlignedEnuReference(
      normalized.x(), normalized.y(), normalized.z(), normalized.w(), reference_row_major))
  {
    return false;
  }
  const Eigen::Matrix3d rotation_local_from_odin = Eigen::Map<
    const Eigen::Matrix<double, 3, 3, Eigen::RowMajor>>(reference_row_major);
  if (!rotation_local_from_odin.array().isFinite().all()) {
    return false;
  }

  state_ = FusionState{};
  state_.stamp_ns = stamp_ns;
  state_.orientation_local_from_body = Eigen::Quaterniond(
    rotation_local_from_odin * normalized.toRotationMatrix()).normalized();
  state_.covariance.setZero();
  state_.covariance.block<3, 3>(0, 0).diagonal().setConstant(
    config_.initial_position_std_m * config_.initial_position_std_m);
  state_.covariance.block<3, 3>(3, 3).diagonal().setConstant(
    config_.initial_velocity_std_mps * config_.initial_velocity_std_mps);
  state_.covariance.block<3, 3>(6, 6).diagonal().setConstant(
    config_.initial_attitude_std_rad * config_.initial_attitude_std_rad);
  state_.covariance.block<3, 3>(9, 9).diagonal().setConstant(
    config_.initial_accelerometer_bias_std_mps2 *
    config_.initial_accelerometer_bias_std_mps2);
  previous_odin_orientation_ = normalized;
  have_previous_odin_orientation_ = true;
  initialized_ = true;
  state_.local_navigation_valid = finiteState(state_);
  have_previous_acceleration_ = false;
  last_external_correction_stamp_ns_ = stamp_ns;
  last_correction_source_ = "INITIAL_STATE";
  last_attitude_correction_rad_.setZero();
  last_update_iteration_count_ = 0;
  last_correction_was_large_angle_ = false;
  updateQuality(stamp_ns);
  return state_.local_navigation_valid;
}

bool FusionNavigator::predictFusionOrientation(
  const Eigen::Quaterniond & orientation_odin_from_body,
  Eigen::Quaterniond & predicted_local_from_body) const noexcept
{
  if (!initialized_ || !have_previous_odin_orientation_) {
    return false;
  }
  Eigen::Quaterniond normalized;
  if (!normalizedQuaternion(orientation_odin_from_body, normalized)) {
    return false;
  }

  // C_fusion(k) = C_fusion(k-1) * (C_odin(k-1)^T * C_odin(k)).
  const Eigen::Quaterniond odin_increment =
    (previous_odin_orientation_.conjugate() * normalized).normalized();
  predicted_local_from_body =
    (state_.orientation_local_from_body * odin_increment).normalized();
  return predicted_local_from_body.coeffs().array().isFinite().all();
}

bool FusionNavigator::localOrientation(
  const Eigen::Quaterniond & orientation_odin_from_body,
  Eigen::Quaterniond & orientation_local_from_body) const noexcept
{
  return predictFusionOrientation(orientation_odin_from_body, orientation_local_from_body);
}

bool FusionNavigator::propagate(
  const std::int64_t stamp_ns, const Eigen::Vector3d & specific_force_body_mps2,
  const Eigen::Quaterniond & orientation_odin_from_body) noexcept
{
  if (!initialized_) {
    return initialize(stamp_ns, orientation_odin_from_body);
  }
  if (stamp_ns <= 0 || !finiteVector(specific_force_body_mps2)) {
    return false;
  }

  Eigen::Quaterniond normalized_odin;
  Eigen::Quaterniond predicted_orientation;
  if (!normalizedQuaternion(orientation_odin_from_body, normalized_odin) ||
    !predictFusionOrientation(normalized_odin, predicted_orientation))
  {
    return false;
  }
  if (stamp_ns <= state_.stamp_ns) {
    return false;
  }

  const double dt_s = static_cast<double>(stamp_ns - state_.stamp_ns) *
    kNanosecondsToSeconds;
  if (!std::isfinite(dt_s) || dt_s > config_.maximum_propagation_interval_s) {
    state_.stamp_ns = stamp_ns;
    previous_odin_orientation_ = normalized_odin;
    have_previous_acceleration_ = false;
    last_external_correction_stamp_ns_ = 0;
    state_.translation_quality_valid = false;
    state_.quality_reason = "IMU_TIME_GAP";
    return false;
  }

  const Eigen::Matrix3d rotation_local_from_body = predicted_orientation.toRotationMatrix();
  const Eigen::Vector3d corrected_specific_force =
    specific_force_body_mps2 - state_.accelerometer_bias_mps2;
  const Eigen::Vector3d specific_force_navigation =
    rotation_local_from_body * corrected_specific_force;
  const Eigen::Vector3d acceleration_navigation = specific_force_navigation -
    Eigen::Vector3d(0.0, 0.0, config_.gravity_mps2);
  if (!finiteVector(acceleration_navigation) ||
    acceleration_navigation.norm() > config_.maximum_acceleration_mps2)
  {
    state_.stamp_ns = stamp_ns;
    state_.orientation_local_from_body = predicted_orientation;
    previous_odin_orientation_ = normalized_odin;
    have_previous_acceleration_ = false;
    state_.translation_quality_valid = false;
    state_.quality_reason = "IMU_ACCELERATION_OUTLIER";
    return false;
  }

  Eigen::Vector3d acceleration_average = acceleration_navigation;
  if (have_previous_acceleration_) {
    acceleration_average =
      0.5 * (previous_navigation_acceleration_mps2_ + acceleration_navigation);
  }
  state_.position_m += state_.velocity_mps * dt_s +
    0.5 * acceleration_average * dt_s * dt_s;
  state_.velocity_mps += acceleration_average * dt_s;

  Eigen::Matrix3d rotation_average = rotation_local_from_body;
  if (have_previous_acceleration_) {
    rotation_average =
      0.5 * (previous_rotation_local_from_body_ + rotation_local_from_body);
  }
  const Eigen::Vector3d average_specific_force_navigation =
    rotation_average * corrected_specific_force;
  const Eigen::Matrix3d attitude_to_acceleration =
    -skewSymmetric(average_specific_force_navigation);

  FusionCovariance transition = FusionCovariance::Identity();
  transition.block<3, 3>(0, 3) = Eigen::Matrix3d::Identity() * dt_s;
  transition.block<3, 3>(0, 6) =
    0.5 * attitude_to_acceleration * dt_s * dt_s;
  transition.block<3, 3>(0, 9) = -0.5 * rotation_average * dt_s * dt_s;
  transition.block<3, 3>(3, 6) = attitude_to_acceleration * dt_s;
  transition.block<3, 3>(3, 9) = -rotation_average * dt_s;

  FusionCovariance process_noise = FusionCovariance::Zero();
  const double acceleration_variance =
    config_.accelerometer_noise_mps2_sqrt_hz *
    config_.accelerometer_noise_mps2_sqrt_hz;
  const double attitude_variance =
    config_.odin_attitude_increment_noise_rad_sqrt_hz *
    config_.odin_attitude_increment_noise_rad_sqrt_hz;
  const double bias_walk_variance =
    config_.accelerometer_bias_random_walk_mps3_sqrt_hz *
    config_.accelerometer_bias_random_walk_mps3_sqrt_hz;
  process_noise.block<3, 3>(0, 0).diagonal().setConstant(
    0.25 * acceleration_variance * dt_s * dt_s * dt_s * dt_s);
  process_noise.block<3, 3>(3, 3).diagonal().setConstant(
    acceleration_variance * dt_s * dt_s);
  process_noise.block<3, 3>(6, 6).diagonal().setConstant(attitude_variance * dt_s);
  process_noise.block<3, 3>(9, 9).diagonal().setConstant(bias_walk_variance * dt_s);
  state_.covariance = transition * state_.covariance * transition.transpose() + process_noise;
  state_.covariance = 0.5 * (state_.covariance + state_.covariance.transpose());

  state_.stamp_ns = stamp_ns;
  state_.orientation_local_from_body = predicted_orientation;
  previous_odin_orientation_ = normalized_odin;
  previous_navigation_acceleration_mps2_ = acceleration_navigation;
  previous_rotation_local_from_body_ = rotation_local_from_body;
  have_previous_acceleration_ = true;
  state_.local_navigation_valid = finiteState(state_);
  updateQuality(stamp_ns);
  return state_.local_navigation_valid;
}

bool FusionNavigator::correctPosition(
  const std::int64_t stamp_ns, const Eigen::Vector3d & observed_position_local_m,
  const Eigen::Matrix3d & observation_covariance,
  const std::string & source) noexcept
{
  if (!initialized_ || stamp_ns <= 0 || !finiteVector(observed_position_local_m) ||
    !finiteMatrix(observation_covariance))
  {
    return false;
  }
  Eigen::Matrix<double, 3, 12> observation = Eigen::Matrix<double, 3, 12>::Zero();
  observation.block<3, 3>(0, 0) = Eigen::Matrix3d::Identity();
  return applyMeasurementUpdate(
    observed_position_local_m, observation, observation_covariance, false,
    Eigen::Quaterniond::Identity(), source);
}

bool FusionNavigator::correctPose(
  const std::int64_t stamp_ns, const Eigen::Vector3d & observed_position_local_m,
  const Eigen::Quaterniond & observed_orientation_local_from_body,
  const PoseObservationCovariance & observation_covariance,
  const std::string & source) noexcept
{
  Eigen::Quaterniond normalized_observation;
  if (!initialized_ || stamp_ns <= 0 || !finiteVector(observed_position_local_m) ||
    !finiteMatrix(observation_covariance) ||
    !normalizedQuaternion(observed_orientation_local_from_body, normalized_observation))
  {
    return false;
  }
  Eigen::Matrix<double, 6, 12> observation = Eigen::Matrix<double, 6, 12>::Zero();
  observation.block<3, 3>(0, 0) = Eigen::Matrix3d::Identity();
  observation.block<3, 3>(3, 6) = Eigen::Matrix3d::Identity();
  Eigen::Matrix<double, 6, 1> measurement = Eigen::Matrix<double, 6, 1>::Zero();
  measurement.head<3>() = observed_position_local_m;
  return applyMeasurementUpdate(
    measurement, observation, observation_covariance, true,
    normalized_observation, source);
}

bool FusionNavigator::applyMeasurementUpdate(
  const Eigen::VectorXd & measurement,
  const Eigen::MatrixXd & observation_matrix,
  const Eigen::MatrixXd & observation_covariance,
  const bool includes_attitude,
  const Eigen::Quaterniond & observed_orientation_local_from_body,
  const std::string & source) noexcept
{
  const Eigen::Index measurement_dimension = observation_matrix.rows();
  if (measurement.size() != measurement_dimension || observation_matrix.cols() != 12 ||
    observation_covariance.rows() != measurement_dimension ||
    observation_covariance.cols() != measurement_dimension ||
    !finiteMatrix(measurement) || !finiteMatrix(observation_matrix) ||
    !finiteMatrix(observation_covariance))
  {
    return false;
  }

  const FusionState prior = state_;
  const Eigen::MatrixXd innovation_covariance =
    observation_matrix * prior.covariance * observation_matrix.transpose() +
    observation_covariance;
  Eigen::LDLT<Eigen::MatrixXd> decomposition(innovation_covariance);
  if (decomposition.info() != Eigen::Success || !decomposition.isPositive()) {
    return false;
  }
  const Eigen::MatrixXd gain = prior.covariance * observation_matrix.transpose() *
    decomposition.solve(Eigen::MatrixXd::Identity(
      measurement_dimension, measurement_dimension));
  if (!finiteMatrix(gain)) {
    return false;
  }

  FusionState candidate = prior;
  Eigen::Matrix<double, 12, 1> total_correction =
    Eigen::Matrix<double, 12, 1>::Zero();
  Eigen::Matrix<double, 12, 1> previous_total = total_correction;
  const int maximum_iterations = includes_attitude ?
    config_.iterated_update_max_iterations : 1;
  int completed_iterations = 0;
  for (int iteration = 0; iteration < maximum_iterations; ++iteration) {
    Eigen::VectorXd residual = Eigen::VectorXd::Zero(measurement_dimension);
    residual.head<3>() = measurement.head<3>() - candidate.position_m;
    if (includes_attitude) {
      Eigen::Vector3d attitude_residual;
      if (!finiteAttitudeErrorAngles(
          observed_orientation_local_from_body,
          candidate.orientation_local_from_body, attitude_residual))
      {
        return false;
      }
      residual.segment<3>(3) = attitude_residual;
    }

    Eigen::Matrix<double, 12, 1> candidate_from_prior =
      Eigen::Matrix<double, 12, 1>::Zero();
    candidate_from_prior.segment<3>(0) = candidate.position_m - prior.position_m;
    candidate_from_prior.segment<3>(3) = candidate.velocity_mps - prior.velocity_mps;
    Eigen::Vector3d attitude_from_prior;
    if (!finiteAttitudeErrorAngles(
        candidate.orientation_local_from_body,
        prior.orientation_local_from_body, attitude_from_prior))
    {
      return false;
    }
    candidate_from_prior.segment<3>(6) = attitude_from_prior;
    candidate_from_prior.segment<3>(9) =
      candidate.accelerometer_bias_mps2 - prior.accelerometer_bias_mps2;

    const Eigen::VectorXd iterated_innovation =
      residual + observation_matrix * candidate_from_prior;
    total_correction = gain * iterated_innovation;
    if (!total_correction.array().isFinite().all()) {
      return false;
    }

    candidate = prior;
    candidate.position_m += total_correction.segment<3>(0);
    candidate.velocity_mps += total_correction.segment<3>(3);
    candidate.accelerometer_bias_mps2 += total_correction.segment<3>(9);
    if (!applyFiniteAttitudeCorrection(
        total_correction.segment<3>(6), prior.orientation_local_from_body,
        candidate.orientation_local_from_body))
    {
      return false;
    }
    completed_iterations = iteration + 1;

    const Eigen::Vector3d position_change =
      total_correction.segment<3>(0) - previous_total.segment<3>(0);
    const Eigen::Vector3d attitude_change =
      total_correction.segment<3>(6) - previous_total.segment<3>(6);
    previous_total = total_correction;
    if (iteration > 0 &&
      position_change.norm() <= config_.iterated_update_position_tolerance_m &&
      attitude_change.norm() <= config_.iterated_update_attitude_tolerance_rad)
    {
      break;
    }
  }

  const FusionCovariance identity = FusionCovariance::Identity();
  const Eigen::MatrixXd residual_matrix = identity - gain * observation_matrix;
  FusionCovariance corrected_covariance =
    residual_matrix * prior.covariance * residual_matrix.transpose() +
    gain * observation_covariance * gain.transpose();
  FusionCovariance reset = FusionCovariance::Identity();
  reset.block<3, 3>(6, 6) = finiteAttitudeResetJacobian(
    total_correction.segment<3>(6));
  corrected_covariance = reset * corrected_covariance * reset.transpose();
  corrected_covariance = 0.5 * (corrected_covariance + corrected_covariance.transpose());
  if (!corrected_covariance.array().isFinite().all()) {
    return false;
  }

  candidate.covariance = corrected_covariance;
  candidate.local_navigation_valid = finiteState(candidate);
  if (!candidate.local_navigation_valid) {
    return false;
  }
  state_ = candidate;
  last_attitude_correction_rad_ = total_correction.segment<3>(6);
  last_update_iteration_count_ = completed_iterations;
  last_correction_was_large_angle_ =
    last_attitude_correction_rad_.norm() >= config_.large_attitude_correction_rad;
  last_external_correction_stamp_ns_ = state_.stamp_ns;
  last_correction_source_ = source.empty() ? "UNKNOWN" : source;
  updateQuality(state_.stamp_ns);
  return true;
}

void FusionNavigator::rebaseTime(const std::int64_t stamp_ns) noexcept
{
  if (stamp_ns > 0) {
    state_.stamp_ns = stamp_ns;
  }
  have_previous_acceleration_ = false;
  updateQuality(state_.stamp_ns);
}

void FusionNavigator::rebaseTimeAndOrientation(
  const std::int64_t stamp_ns,
  const Eigen::Quaterniond & orientation_odin_from_body) noexcept
{
  Eigen::Quaterniond normalized;
  if (initialized_ && normalizedQuaternion(orientation_odin_from_body, normalized)) {
    // A new ODIN epoch may reset its absolute quaternion. Preserve the finite fusion
    // attitude and only rebase the source used for the next adjacent increment.
    previous_odin_orientation_ = normalized;
    have_previous_odin_orientation_ = true;
  }
  rebaseTime(stamp_ns);
  last_external_correction_stamp_ns_ = 0;
  updateQuality(stamp_ns);
}

void FusionNavigator::updateQuality(const std::int64_t stamp_ns) noexcept
{
  if (!initialized_ || !state_.local_navigation_valid) {
    state_.translation_quality_valid = false;
    state_.quality_reason = "LOCAL_NAVIGATION_INVALID";
    return;
  }
  const double maximum_position_variance = state_.covariance.block<3, 3>(0, 0)
    .diagonal().maxCoeff();
  const double position_std_m = maximum_position_variance >= 0.0 ?
    std::sqrt(maximum_position_variance) : std::numeric_limits<double>::infinity();
  if (!std::isfinite(position_std_m) ||
    position_std_m > config_.maximum_translation_position_std_m)
  {
    state_.translation_quality_valid = false;
    state_.quality_reason = "POSITION_UNCERTAINTY_TOO_LARGE";
    return;
  }
  const double inertial_only_s = last_external_correction_stamp_ns_ > 0 && stamp_ns > 0 ?
    static_cast<double>(std::max<std::int64_t>(
      0, stamp_ns - last_external_correction_stamp_ns_)) * kNanosecondsToSeconds :
    std::numeric_limits<double>::infinity();
  if (inertial_only_s > config_.maximum_inertial_only_duration_s) {
    state_.translation_quality_valid = false;
    state_.quality_reason = "INERTIAL_ONLY_TIMEOUT";
    return;
  }
  state_.translation_quality_valid = true;
  state_.quality_reason = "NONE";
}

const FusionState & FusionNavigator::state() const noexcept
{
  return state_;
}

bool FusionNavigator::initialized() const noexcept
{
  return initialized_;
}

std::int64_t FusionNavigator::lastExternalCorrectionStampNs() const noexcept
{
  return last_external_correction_stamp_ns_;
}

const std::string & FusionNavigator::lastCorrectionSource() const noexcept
{
  return last_correction_source_;
}

const Eigen::Vector3d & FusionNavigator::lastAttitudeCorrectionRad() const noexcept
{
  return last_attitude_correction_rad_;
}

int FusionNavigator::lastUpdateIterationCount() const noexcept
{
  return last_update_iteration_count_;
}

bool FusionNavigator::lastCorrectionWasLargeAngle() const noexcept
{
  return last_correction_was_large_angle_;
}

}  // namespace localization

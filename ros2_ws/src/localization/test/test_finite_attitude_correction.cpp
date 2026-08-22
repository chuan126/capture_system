#include "localization/finite_attitude_correction.hpp"

#include <gtest/gtest.h>

#include <Eigen/Core>
#include <Eigen/Geometry>

#include <array>
#include <cmath>

namespace localization
{
namespace
{

constexpr double kPi = 3.14159265358979323846;

Eigen::Matrix3d directProvidedA2mat(const Eigen::Vector3d & attitude)
{
  const double si = std::sin(attitude[0]);
  const double sj = std::sin(attitude[1]);
  const double sk = std::sin(attitude[2]);
  const double ci = std::cos(attitude[0]);
  const double cj = std::cos(attitude[1]);
  const double ck = std::cos(attitude[2]);
  Eigen::Matrix3d matrix;
  matrix <<
    cj * ck - si * sj * sk, -ci * sk, sj * ck + si * cj * sk,
    cj * sk + si * sj * ck, ci * ck, sj * sk - si * cj * ck,
    -ci * sj, si, ci * cj;
  return matrix;
}

}  // namespace

TEST(FiniteAttitudeCorrectionTest, ZeroCorrectionIsIdentity)
{
  const Eigen::Matrix3d correction = finiteAttitudeCorrectionMatrix(Eigen::Vector3d::Zero());
  EXPECT_TRUE(correction.isApprox(Eigen::Matrix3d::Identity(), 1.0e-15));
  Eigen::Quaterniond output;
  ASSERT_TRUE(applyFiniteAttitudeCorrection(
    Eigen::Vector3d::Zero(), Eigen::Quaterniond::Identity(), output));
  EXPECT_NEAR(output.angularDistance(Eigen::Quaterniond::Identity()), 0.0, 1.0e-15);
}

TEST(FiniteAttitudeCorrectionTest, MatchesProvidedA2matFromOneToNinetyDegrees)
{
  const std::array<double, 5> angles_deg{1.0, 5.0, 20.0, 45.0, 90.0};
  for (const double angle_deg : angles_deg) {
    for (int axis = 0; axis < 3; ++axis) {
      SCOPED_TRACE(::testing::Message() << "angle=" << angle_deg << " axis=" << axis);
      Eigen::Vector3d attitude = Eigen::Vector3d::Zero();
      attitude[axis] = angle_deg * kPi / 180.0;
      const Eigen::Matrix3d actual = finiteAttitudeCorrectionMatrix(attitude);
      const Eigen::Matrix3d expected = directProvidedA2mat(attitude);
      EXPECT_TRUE(actual.isApprox(expected, 1.0e-14));
    }
  }
}

TEST(FiniteAttitudeCorrectionTest, LargeThreeAxisCorrectionUsesFiniteComposition)
{
  const Eigen::Vector3d error_rad(
    30.0 * kPi / 180.0, -40.0 * kPi / 180.0, 20.0 * kPi / 180.0);
  const Eigen::Matrix3d before =
    Eigen::AngleAxisd(0.3, Eigen::Vector3d(1.0, 2.0, -1.0).normalized()).toRotationMatrix();
  const Eigen::Matrix3d expected = directProvidedA2mat(error_rad) * before;
  Eigen::Quaterniond corrected;
  ASSERT_TRUE(applyFiniteAttitudeCorrection(error_rad, Eigen::Quaterniond(before), corrected));
  EXPECT_TRUE(corrected.toRotationMatrix().isApprox(expected, 1.0e-12));

  Eigen::Matrix3d first_order = Eigen::Matrix3d::Identity();
  first_order <<
    1.0, -error_rad.z(), error_rad.y(),
    error_rad.z(), 1.0, -error_rad.x(),
    -error_rad.y(), error_rad.x(), 1.0;
  EXPECT_GT((first_order * before - expected).norm(), 0.20);
  EXPECT_NEAR(corrected.toRotationMatrix().determinant(), 1.0, 1.0e-12);
  EXPECT_TRUE((corrected.toRotationMatrix() * corrected.toRotationMatrix().transpose())
    .isApprox(Eigen::Matrix3d::Identity(), 1.0e-12));
}

TEST(FiniteAttitudeCorrectionTest, ResetJacobianRemainsFiniteAfterLargeInjection)
{
  const Eigen::Vector3d error_rad(
    35.0 * kPi / 180.0, -25.0 * kPi / 180.0, 40.0 * kPi / 180.0);
  const Eigen::Matrix3d jacobian = finiteAttitudeResetJacobian(error_rad);
  EXPECT_TRUE(jacobian.array().isFinite().all());
  EXPECT_GT(std::abs(jacobian.determinant()), 1.0e-3);
}

}  // namespace localization

#include "localization/attitude_transform.hpp"

#include "attitude_matrix.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

namespace localization
{
namespace
{

TEST(AttitudeMatrixTest, ProvidedQuaternionConversionKeepsIdentity)
{
  double quaternion[4]{1.0, 0.0, 0.0, 0.0};
  double matrix[9];
  q2mat(quaternion, matrix);

  const double expected[9]{1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
  for (int index = 0; index < 9; ++index) {
    EXPECT_DOUBLE_EQ(matrix[index], expected[index]);
  }
}

TEST(AttitudeMatrixTest, ProvidedEulerQuaternionAndMatrixConversionsAgree)
{
  double attitude[3]{0.2, -0.3, 0.4};
  double matrix_from_attitude[9];
  double quaternion[4];
  double matrix_from_quaternion[9];
  double recovered_attitude[3];

  a2mat(attitude, matrix_from_attitude);
  a2qua(attitude, quaternion);
  q2mat(quaternion, matrix_from_quaternion);
  q2att(quaternion, recovered_attitude);

  for (int index = 0; index < 9; ++index) {
    EXPECT_NEAR(matrix_from_quaternion[index], matrix_from_attitude[index], 1.0e-12);
  }
  for (int index = 0; index < 3; ++index) {
    EXPECT_NEAR(recovered_attitude[index], attitude[index], 1.0e-12);
  }
}

TEST(AttitudeMatrixTest, ProvidedMatrixToQuaternionRoundTripKeepsRotation)
{
  double attitude[3]{-0.15, 0.25, -0.35};
  double original_matrix[9];
  double quaternion[4];
  double recovered_matrix[9];

  a2mat(attitude, original_matrix);
  m2qua(original_matrix, quaternion);
  q2mat(quaternion, recovered_matrix);

  for (int index = 0; index < 9; ++index) {
    EXPECT_NEAR(recovered_matrix[index], original_matrix[index], 1.0e-12);
  }
}

TEST(AttitudeMatrixTest, RosQuaternionIsReorderedAndNormalized)
{
  double matrix[9];
  ASSERT_TRUE(rosQuaternionToMatrix(2.0, 0.0, 0.0, 0.0, matrix));

  const double expected[9]{1.0, 0.0, 0.0, 0.0, -1.0, 0.0, 0.0, 0.0, -1.0};
  for (int index = 0; index < 9; ++index) {
    EXPECT_NEAR(matrix[index], expected[index], 1.0e-12);
  }
}

TEST(AttitudeMatrixTest, RejectsInvalidQuaternionWithoutReusingOutput)
{
  double matrix[9]{};
  EXPECT_FALSE(rosQuaternionToMatrix(0.0, 0.0, 0.0, 0.0, matrix));
  for (const double value : matrix) {
    EXPECT_TRUE(std::isnan(value));
  }
}

TEST(AttitudeMatrixTest, LiveQuaternionRotatesMeasuredGravityToUp)
{
  // 取自2026-08-04静止实机样本，ROS四元数顺序为[x, y, z, w]。
  constexpr double qx = 0.99990038;
  constexpr double qy = -0.00011300004;
  constexpr double qz = -0.013932005;
  constexpr double qw = -0.0022600009;
  double matrix[9];
  ASSERT_TRUE(rosQuaternionToMatrix(qx, qy, qz, qw, matrix));

  double acceleration[3]{-0.27345, -0.03353, -9.79533};
  double rotated[3];
  MatMul(matrix, acceleration, rotated, 3, 3, 1);

  EXPECT_NEAR(rotated[0], 0.0, 0.02);
  EXPECT_NEAR(rotated[1], 0.0, 0.02);
  EXPECT_NEAR(rotated[2], 9.7992, 0.02);
}

TEST(AttitudeMatrixTest, CurrentPlacementInitializesLocalEnu)
{
  // 取自2026-08-04本次放置姿态的5秒静止均值，ROS顺序为[x, y, z, w]。
  constexpr double qx = -0.0030623169;
  constexpr double qy = -0.0009426496;
  constexpr double qz = -0.7722161424;
  constexpr double qw = 0.6353518419;
  double Cenu_odom[9];
  ASSERT_TRUE(initializeLocalEnuReference(qx, qy, qz, qw, Cenu_odom));

  // 当前放置被直接规定为X=East、Y=North、Z=Up，因此初始化时坐标数值不变。
  const RadarPoint3d lidar_point{1.25, -0.75, 2.5};
  EnuPoint3d enu{};
  ASSERT_TRUE(transformRadarPointToLocalEnu(
      lidar_point, qx, qy, qz, qw, Cenu_odom, enu));
  EXPECT_NEAR(enu.east, lidar_point.x, 1.0e-9);
  EXPECT_NEAR(enu.north, lidar_point.y, 1.0e-9);
  EXPECT_NEAR(enu.up, lidar_point.z, 1.0e-9);
}

TEST(AttitudeMatrixTest, CurrentRadarAxesAreDefinedAsEnu)
{
  constexpr double qx = -0.0030623169;
  constexpr double qy = -0.0009426496;
  constexpr double qz = -0.7722161424;
  constexpr double qw = 0.6353518419;
  double Cenu_odom[9];
  double Cnb[9];
  ASSERT_TRUE(initializeLocalEnuReference(qx, qy, qz, qw, Cenu_odom));
  ASSERT_TRUE(rosQuaternionToMatrix(qx, qy, qz, qw, Cnb));

  double Cenu_body[9];
  MatMul(Cenu_odom, Cnb, Cenu_body, 3, 3, 3);
  const double identity[9]{1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
  for (int index = 0; index < 9; ++index) {
    EXPECT_NEAR(Cenu_body[index], identity[index], 1.0e-9);
  }
}

TEST(AttitudeMatrixTest, RejectsNonFinitePoint)
{
  const RadarPoint3d lidar_point{std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0};
  EnuPoint3d output{};
  const double identity[9]{1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
  EXPECT_FALSE(transformRadarPointToLocalEnu(
      lidar_point, 0.0, 0.0, 0.0, 1.0, identity, output));
  EXPECT_TRUE(std::isnan(output.east));
  EXPECT_TRUE(std::isnan(output.north));
  EXPECT_TRUE(std::isnan(output.up));
}

}  // namespace
}  // namespace localization

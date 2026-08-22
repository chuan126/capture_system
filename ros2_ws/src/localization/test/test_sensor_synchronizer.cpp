#include "localization/sensor_synchronizer.hpp"

#include <gtest/gtest.h>

#include <Eigen/Core>
#include <Eigen/Geometry>

namespace localization
{

TEST(SensorSynchronizerTest, InterpolatesQuaternionAtImuTimestamp)
{
  SensorSynchronizer synchronizer(20000000LL, 1000000000LL, 100U);
  const Eigen::Quaterniond start = Eigen::Quaterniond::Identity();
  const Eigen::Quaterniond end(Eigen::AngleAxisd(0.2, Eigen::Vector3d::UnitZ()));
  ASSERT_EQ(
    synchronizer.addOrientation(TimedOrientation{1000000000LL, start}),
    SensorSynchronizer::AddResult::kAccepted);
  ASSERT_EQ(
    synchronizer.addOrientation(TimedOrientation{1010000000LL, end}),
    SensorSynchronizer::AddResult::kAccepted);
  ASSERT_EQ(
    synchronizer.addImu(TimedImuAcceleration{1005000000LL, Eigen::Vector3d::Ones()}),
    SensorSynchronizer::AddResult::kAccepted);
  SynchronizedImuSample sample;
  ASSERT_TRUE(synchronizer.popSynchronized(sample));
  EXPECT_NEAR(Eigen::AngleAxisd(sample.orientation_odin_from_body).angle(), 0.1, 1.0e-9);
}

TEST(SensorSynchronizerTest, DropsOnlyImuSampleAcrossPoseGap)
{
  SensorSynchronizer synchronizer(5000000LL, 1000000000LL, 100U);
  synchronizer.addOrientation(TimedOrientation{1000000000LL, Eigen::Quaterniond::Identity()});
  synchronizer.addOrientation(TimedOrientation{1020000000LL, Eigen::Quaterniond::Identity()});
  synchronizer.addImu(TimedImuAcceleration{1010000000LL, Eigen::Vector3d::Zero()});
  SynchronizedImuSample sample;
  EXPECT_FALSE(synchronizer.popSynchronized(sample));
  EXPECT_EQ(synchronizer.droppedImuCount(), 1U);
}

TEST(SensorSynchronizerTest, EpochResetClearsOldTimeWithoutResettingCallerState)
{
  SensorSynchronizer synchronizer(20000000LL, 500000000LL, 100U);
  synchronizer.addOrientation(TimedOrientation{2000000000LL, Eigen::Quaterniond::Identity()});
  EXPECT_EQ(
    synchronizer.addOrientation(TimedOrientation{100000000LL, Eigen::Quaterniond::Identity()}),
    SensorSynchronizer::AddResult::kEpochReset);
  EXPECT_EQ(synchronizer.orientationCount(), 1U);
}

}  // namespace localization

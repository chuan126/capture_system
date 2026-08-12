#include "localization/heading_alignment.hpp"

#include <cmath>
#include <limits>

namespace localization
{
namespace
{

constexpr double kPi = 3.141592653589793238462643383279502884;

}  // namespace

double degreesToRadians(const double degrees) noexcept
{
  return degrees * kPi / 180.0;
}

double radiansToDegrees(const double radians) noexcept
{
  return radians * 180.0 / kPi;
}

double wrapAngleRad(const double angle_rad) noexcept
{
  if (!std::isfinite(angle_rad)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  double wrapped = std::fmod(angle_rad + kPi, 2.0 * kPi);
  if (wrapped < 0.0) {
    wrapped += 2.0 * kPi;
  }
  wrapped -= kPi;
  if (wrapped <= -kPi) {
    wrapped += 2.0 * kPi;
  }
  return wrapped;
}

double wrapDegrees360(const double degrees) noexcept
{
  if (!std::isfinite(degrees)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  double wrapped = std::fmod(degrees, 360.0);
  if (wrapped < 0.0) {
    wrapped += 360.0;
  }
  return wrapped;
}

double clockwiseCourseDegreesToEnuYawRad(const double course_degrees) noexcept
{
  if (!std::isfinite(course_degrees)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return wrapAngleRad(kPi / 2.0 - degreesToRadians(course_degrees));
}

double enuYawRadToClockwiseCourseDegrees(const double yaw_rad) noexcept
{
  if (!std::isfinite(yaw_rad)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return wrapDegrees360(90.0 - radiansToDegrees(yaw_rad));
}

bool isRmcValid(const std::uint8_t rmc_validity) noexcept
{
  return rmc_validity == static_cast<std::uint8_t>('A') ||
         rmc_validity == static_cast<std::uint8_t>('D');
}

}  // namespace localization

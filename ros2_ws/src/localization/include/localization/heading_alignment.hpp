#ifndef LOCALIZATION__HEADING_ALIGNMENT_HPP_
#define LOCALIZATION__HEADING_ALIGNMENT_HPP_

#include <cstdint>

namespace localization
{

double degreesToRadians(double degrees) noexcept;
double radiansToDegrees(double radians) noexcept;
double wrapAngleRad(double angle_rad) noexcept;
double wrapDegrees360(double degrees) noexcept;
double clockwiseCourseDegreesToEnuYawRad(double course_degrees) noexcept;
double enuYawRadToClockwiseCourseDegrees(double yaw_rad) noexcept;
bool isRmcValid(std::uint8_t rmc_validity) noexcept;

}  // namespace localization

#endif  // LOCALIZATION__HEADING_ALIGNMENT_HPP_

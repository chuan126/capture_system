#ifndef LOCALIZATION__HEADING_ALIGNMENT_HPP_
#define LOCALIZATION__HEADING_ALIGNMENT_HPP_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>

namespace localization
{

double degreesToRadians(double degrees) noexcept;
double radiansToDegrees(double radians) noexcept;
double wrapAngleRad(double angle_rad) noexcept;
double wrapDegrees360(double degrees) noexcept;
double clockwiseCourseDegreesToEnuYawRad(double course_degrees) noexcept;
double enuYawRadToClockwiseCourseDegrees(double yaw_rad) noexcept;
bool isRmcValid(std::uint8_t rmc_validity) noexcept;

struct HeadingAlignmentOptions
{
  std::size_t min_samples{10U};
  double min_distance_m{50.0};
  double max_std_rad{degreesToRadians(5.0)};
  double filter_alpha{0.1};
  std::size_t max_samples{200U};
};

struct HeadingAlignmentState
{
  bool valid{false};
  std::size_t sample_count{0U};
  double delta_yaw_rad{0.0};
  double std_rad{0.0};
  double baseline_m{0.0};
};

class HeadingAlignmentEstimator
{
public:
  explicit HeadingAlignmentEstimator(HeadingAlignmentOptions options = {});

  bool addObservation(double delta_yaw_rad, double baseline_m, double weight = 1.0) noexcept;
  HeadingAlignmentState state() const noexcept;
  void reset() noexcept;

private:
  struct Observation
  {
    double delta_yaw_rad{0.0};
    double baseline_m{0.0};
    double weight{1.0};
  };

  HeadingAlignmentOptions options_;
  std::deque<Observation> observations_;
  std::optional<double> filtered_delta_yaw_rad_;
};

struct CourseEstimatorOptions
{
  bool enabled{true};
  double min_speed_mps{5.0};
  double min_baseline_m{30.0};
  double max_baseline_m{200.0};
  double max_window_s{30.0};
  double max_jump_rad{degreesToRadians(20.0)};
  std::size_t max_samples{300U};
};

struct CoursePositionSample
{
  std::int64_t stamp_ns{0};
  double east_m{0.0};
  double north_m{0.0};
  double speed_mps{0.0};
};

struct CourseEstimate
{
  bool valid{false};
  double yaw_enu_rad{0.0};
  double baseline_m{0.0};
  std::string invalid_reason{"NOT_INITIALIZED"};
};

class CourseFromPositionEstimator
{
public:
  explicit CourseFromPositionEstimator(CourseEstimatorOptions options = {});

  bool addSample(const CoursePositionSample & sample) noexcept;
  CourseEstimate estimate() noexcept;
  void reset() noexcept;
  std::size_t sampleCount() const noexcept;

private:
  CourseEstimatorOptions options_;
  std::deque<CoursePositionSample> samples_;
  std::optional<double> last_yaw_enu_rad_;
};

}  // namespace localization

#endif  // LOCALIZATION__HEADING_ALIGNMENT_HPP_

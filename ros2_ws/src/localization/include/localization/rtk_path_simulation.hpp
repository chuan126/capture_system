#ifndef LOCALIZATION__RTK_PATH_SIMULATION_HPP_
#define LOCALIZATION__RTK_PATH_SIMULATION_HPP_

#include "localization/dead_reckoning.hpp"
#include "localization/geodesy.hpp"
#include "localization/heading_rigid_alignment.hpp"

#include <optional>

namespace localization
{

struct RtkPathSimulationOptions
{
  int test_mode{0};
  Llh point_a{24.5738888889, 118.0894444444, 20.0};
  Llh point_b{24.5738912, 118.0894349, 20.0};
};

HeadingRigidAlignmentOptions simulationHeadingFitOptions(
  const HeadingRigidAlignmentOptions & actual_options,
  double simulation_path_distance_m) noexcept;

struct SimulatedRtkFix
{
  Llh llh;
  Enu enu_from_a;
  double progress_ratio{0.0};
  bool reached_point_b{false};
};

class RtkPathSimulation
{
public:
  explicit RtkPathSimulation(RtkPathSimulationOptions options = {});

  bool active() const noexcept;
  bool valid() const noexcept;
  bool captureOdinOrigin(const Vector3d & odin_position_m) noexcept;
  bool started() const noexcept;
  std::optional<SimulatedRtkFix> generate(const Vector3d & odin_position_m) const noexcept;
  const Llh & pointA() const noexcept;
  const Llh & pointB() const noexcept;
  const Enu & pointBEnu() const noexcept;
  double horizontalDistanceM() const noexcept;
  double geographicDirectionDeg() const noexcept;
  void reset() noexcept;

private:
  RtkPathSimulationOptions options_;
  Enu point_b_enu_;
  double horizontal_distance_m_{0.0};
  double geographic_direction_deg_{0.0};
  std::optional<Vector3d> odin_origin_m_;
};

}  // namespace localization

#endif  // LOCALIZATION__RTK_PATH_SIMULATION_HPP_

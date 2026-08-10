#include "localization/geodesy.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace localization
{
namespace
{

constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr double kDegreesToRadians = kPi / 180.0;
constexpr double kRadiansToDegrees = 180.0 / kPi;
constexpr double kWgs84A = 6378137.0;
constexpr double kWgs84F = 1.0 / 298.257223563;
constexpr double kWgs84B = kWgs84A * (1.0 - kWgs84F);
constexpr double kWgs84E2 = kWgs84F * (2.0 - kWgs84F);
constexpr double kWgs84Ep2 = (kWgs84A * kWgs84A - kWgs84B * kWgs84B) /
  (kWgs84B * kWgs84B);

double square(const double value) noexcept
{
  return value * value;
}

double normalizeLongitudeDeg(double longitude_deg) noexcept
{
  if (!std::isfinite(longitude_deg)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  double normalized = std::fmod(longitude_deg + 180.0, 360.0);
  if (normalized < 0.0) {
    normalized += 360.0;
  }
  return normalized - 180.0;
}

Llh invalidLlh() noexcept
{
  const double nan = std::numeric_limits<double>::quiet_NaN();
  return Llh{nan, nan, nan};
}

Ecef invalidEcef() noexcept
{
  const double nan = std::numeric_limits<double>::quiet_NaN();
  return Ecef{nan, nan, nan};
}

Enu invalidEnu() noexcept
{
  const double nan = std::numeric_limits<double>::quiet_NaN();
  return Enu{nan, nan, nan};
}

}  // namespace

bool isFinite(const Llh & llh) noexcept
{
  return std::isfinite(llh.latitude_deg) && std::isfinite(llh.longitude_deg) &&
         std::isfinite(llh.altitude_m) && llh.latitude_deg >= -90.0 &&
         llh.latitude_deg <= 90.0;
}

bool isFinite(const Ecef & ecef) noexcept
{
  return std::isfinite(ecef.x_m) && std::isfinite(ecef.y_m) && std::isfinite(ecef.z_m);
}

bool isFinite(const Enu & enu) noexcept
{
  return std::isfinite(enu.east_m) && std::isfinite(enu.north_m) && std::isfinite(enu.up_m);
}

Ecef llhToEcef(const Llh & llh) noexcept
{
  if (!isFinite(llh)) {
    return invalidEcef();
  }
  const double lat = llh.latitude_deg * kDegreesToRadians;
  const double lon = llh.longitude_deg * kDegreesToRadians;
  const double sin_lat = std::sin(lat);
  const double cos_lat = std::cos(lat);
  const double sin_lon = std::sin(lon);
  const double cos_lon = std::cos(lon);
  const double prime_vertical_radius = kWgs84A / std::sqrt(1.0 - kWgs84E2 * sin_lat * sin_lat);

  return Ecef{
    (prime_vertical_radius + llh.altitude_m) * cos_lat * cos_lon,
    (prime_vertical_radius + llh.altitude_m) * cos_lat * sin_lon,
    (prime_vertical_radius * (1.0 - kWgs84E2) + llh.altitude_m) * sin_lat};
}

Llh ecefToLlh(const Ecef & ecef) noexcept
{
  if (!isFinite(ecef)) {
    return invalidLlh();
  }

  const double p = std::hypot(ecef.x_m, ecef.y_m);
  if (p <= 1.0e-9 && std::abs(ecef.z_m) <= 1.0e-9) {
    return invalidLlh();
  }

  const double lon = std::atan2(ecef.y_m, ecef.x_m);
  const double theta = std::atan2(ecef.z_m * kWgs84A, p * kWgs84B);
  const double sin_theta = std::sin(theta);
  const double cos_theta = std::cos(theta);
  const double lat = std::atan2(
    ecef.z_m + kWgs84Ep2 * kWgs84B * sin_theta * sin_theta * sin_theta,
    p - kWgs84E2 * kWgs84A * cos_theta * cos_theta * cos_theta);
  const double sin_lat = std::sin(lat);
  const double prime_vertical_radius = kWgs84A / std::sqrt(1.0 - kWgs84E2 * sin_lat * sin_lat);
  const double altitude = p / std::max(1.0e-15, std::cos(lat)) - prime_vertical_radius;

  return Llh{
    lat * kRadiansToDegrees,
    normalizeLongitudeDeg(lon * kRadiansToDegrees),
    altitude};
}

Enu ecefToEnu(const Llh & origin_llh, const Ecef & target_ecef) noexcept
{
  if (!isFinite(origin_llh) || !isFinite(target_ecef)) {
    return invalidEnu();
  }
  const Ecef origin_ecef = llhToEcef(origin_llh);
  if (!isFinite(origin_ecef)) {
    return invalidEnu();
  }

  const double lat = origin_llh.latitude_deg * kDegreesToRadians;
  const double lon = origin_llh.longitude_deg * kDegreesToRadians;
  const double sin_lat = std::sin(lat);
  const double cos_lat = std::cos(lat);
  const double sin_lon = std::sin(lon);
  const double cos_lon = std::cos(lon);

  const double dx = target_ecef.x_m - origin_ecef.x_m;
  const double dy = target_ecef.y_m - origin_ecef.y_m;
  const double dz = target_ecef.z_m - origin_ecef.z_m;

  return Enu{
    -sin_lon * dx + cos_lon * dy,
    -sin_lat * cos_lon * dx - sin_lat * sin_lon * dy + cos_lat * dz,
    cos_lat * cos_lon * dx + cos_lat * sin_lon * dy + sin_lat * dz};
}

Ecef enuToEcef(const Llh & origin_llh, const Enu & enu) noexcept
{
  if (!isFinite(origin_llh) || !isFinite(enu)) {
    return invalidEcef();
  }
  const Ecef origin_ecef = llhToEcef(origin_llh);
  if (!isFinite(origin_ecef)) {
    return invalidEcef();
  }

  const double lat = origin_llh.latitude_deg * kDegreesToRadians;
  const double lon = origin_llh.longitude_deg * kDegreesToRadians;
  const double sin_lat = std::sin(lat);
  const double cos_lat = std::cos(lat);
  const double sin_lon = std::sin(lon);
  const double cos_lon = std::cos(lon);

  const double dx =
    -sin_lon * enu.east_m - sin_lat * cos_lon * enu.north_m +
    cos_lat * cos_lon * enu.up_m;
  const double dy =
    cos_lon * enu.east_m - sin_lat * sin_lon * enu.north_m +
    cos_lat * sin_lon * enu.up_m;
  const double dz = cos_lat * enu.north_m + sin_lat * enu.up_m;
  return Ecef{origin_ecef.x_m + dx, origin_ecef.y_m + dy, origin_ecef.z_m + dz};
}

Enu llhToEnu(const Llh & origin_llh, const Llh & target_llh) noexcept
{
  return ecefToEnu(origin_llh, llhToEcef(target_llh));
}

Llh enuToLlh(const Llh & origin_llh, const Enu & enu) noexcept
{
  return ecefToLlh(enuToEcef(origin_llh, enu));
}

double horizontalNorm(const Enu & enu) noexcept
{
  if (!isFinite(enu)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return std::hypot(enu.east_m, enu.north_m);
}

}  // namespace localization

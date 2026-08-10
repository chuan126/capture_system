#ifndef LOCALIZATION__GEODESY_HPP_
#define LOCALIZATION__GEODESY_HPP_

namespace localization
{

struct Llh
{
  double latitude_deg{0.0};
  double longitude_deg{0.0};
  double altitude_m{0.0};
};

struct Ecef
{
  double x_m{0.0};
  double y_m{0.0};
  double z_m{0.0};
};

struct Enu
{
  double east_m{0.0};
  double north_m{0.0};
  double up_m{0.0};
};

bool isFinite(const Llh & llh) noexcept;
bool isFinite(const Ecef & ecef) noexcept;
bool isFinite(const Enu & enu) noexcept;

Ecef llhToEcef(const Llh & llh) noexcept;
Llh ecefToLlh(const Ecef & ecef) noexcept;
Enu ecefToEnu(const Llh & origin_llh, const Ecef & target_ecef) noexcept;
Ecef enuToEcef(const Llh & origin_llh, const Enu & enu) noexcept;
Enu llhToEnu(const Llh & origin_llh, const Llh & target_llh) noexcept;
Llh enuToLlh(const Llh & origin_llh, const Enu & enu) noexcept;

double horizontalNorm(const Enu & enu) noexcept;

}  // namespace localization

#endif  // LOCALIZATION__GEODESY_HPP_

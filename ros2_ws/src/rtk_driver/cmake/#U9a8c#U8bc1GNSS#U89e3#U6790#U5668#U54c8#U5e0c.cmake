file(SHA256 "${SOURCE_DIR}/NMEA0183/gnss_nmea.c" GNSS_NMEA_C_HASH)
file(SHA256 "${SOURCE_DIR}/NMEA0183/gnss_nmea.h" GNSS_NMEA_H_HASH)

if(NOT GNSS_NMEA_C_HASH STREQUAL
  "7d3afe546b520e3d46f56f60498b28e3c63d26a7de00a49718b92610f4b2c7e9")
  message(FATAL_ERROR "gnss_nmea.c与已验证版本不一致，禁止修改")
endif()

if(NOT GNSS_NMEA_H_HASH STREQUAL
  "7405468a395b8591fd3c5261cbb9173c128e30dfc152c84a40c5a6d9bcd66277")
  message(FATAL_ERROR "gnss_nmea.h与已验证版本不一致，禁止修改")
endif()

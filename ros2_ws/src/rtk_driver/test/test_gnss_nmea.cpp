#include <cstdint>
#include <string>

#include "gtest/gtest.h"

extern "C"
{
#include "gnss_nmea.h"
}

namespace
{

std::uint32_t feed_sentence(const std::string & sentence)
{
  std::uint32_t events = GNSS_NMEA_EVENT_NONE;
  for (const unsigned char byte : sentence) {
    events |= static_cast<std::uint32_t>(gnss_nmea_input_byte(byte));
  }
  return events;
}

TEST(GnssNmea, ReplaysVerifiedRmcGgaAndGsa)
{
  gnss_nmea_init();

  const auto rmc_event = feed_sentence(
    "$GNRMC,073127.20,A,2434.43130447,N,11805.40100991,E,0.005,72.6,270726,4.6,W,A,C*6C\r\n");
  EXPECT_NE(rmc_event & GNSS_NMEA_EVENT_RMC_OK, 0U);

  const auto gga_event = feed_sentence(
    "$GNGGA,073127.30,2434.43130230,N,11805.40100582,E,1,14,1.1,14.2086,M,9.7459,M,,*79\r\n");
  EXPECT_NE(gga_event & GNSS_NMEA_EVENT_GGA_OK, 0U);

  const auto gsa_event = feed_sentence(
    "$GNGSA,M,3,23,,,,,,,,,,,,2.6,1.1,2.3,1*39\r\n");
  EXPECT_NE(gsa_event & GNSS_NMEA_EVENT_GSA_OK, 0U);

  const gnss_nmea_data_t * data = gnss_nmea_get_data();
  ASSERT_NE(data, nullptr);
  EXPECT_EQ(data->GPS_state, 1U);
  EXPECT_EQ(static_cast<unsigned char>(data->StarNum), 14U);
  EXPECT_FLOAT_EQ(data->HDOP, 1.1F);
  EXPECT_FLOAT_EQ(data->PDOP, 2.6F);
  EXPECT_NEAR(gnss_nmea_get_latitude(), 24.5738550383, 1.0e-8);
  EXPECT_NEAR(gnss_nmea_get_longitude(), 118.0900167637, 1.0e-8);
  EXPECT_NEAR(gnss_nmea_get_height(), 14.2086, 1.0e-4);
}

TEST(GnssNmea, ReportsChecksumErrorWithoutSuccessfulGga)
{
  gnss_nmea_init();
  const auto event = feed_sentence(
    "$GNGGA,073127.30,2434.43130230,N,11805.40100582,E,1,14,1.1,14.2086,M,9.7459,M,,*00\r\n");
  EXPECT_NE(event & GNSS_NMEA_EVENT_CHECKSUM_ERROR, 0U);
  EXPECT_EQ(event & GNSS_NMEA_EVENT_GGA_OK, 0U);
}

}  // namespace

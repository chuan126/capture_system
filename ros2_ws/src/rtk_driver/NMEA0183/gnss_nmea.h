#ifndef GNSS_NMEA_H
#define GNSS_NMEA_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

/**
 * @brief GNSS NMEA解析事件
 *
 * 事件值采用位标志形式，调用 gnss_nmea_input() 输入一段数据时，
 * 返回值中可能同时包含多个事件。
 */
typedef enum
{
    GNSS_NMEA_EVENT_NONE           = 0U,
    GNSS_NMEA_EVENT_GGA_HEADER     = 1U << 0,
    GNSS_NMEA_EVENT_RMC_OK         = 1U << 1,
    GNSS_NMEA_EVENT_GGA_OK         = 1U << 2,
    GNSS_NMEA_EVENT_GSA_OK         = 1U << 3,
    GNSS_NMEA_EVENT_BESTPOSA_OK    = 1U << 4,
    GNSS_NMEA_EVENT_CHECKSUM_ERROR = 1U << 5,
    GNSS_NMEA_EVENT_FRAME_OVERFLOW = 1U << 6
} gnss_nmea_event_t;

/**
 * @brief GNSS解析输出数据
 *
 */
typedef struct GnssData
{
    char isGnssVaild;
    char isGnssVeVaild;
    char isGnssPosVaild;
    unsigned int Gnss_UTC_Year;
    unsigned int Gnss_UTC_Month;
    unsigned int Gnss_UTC_Day;
    unsigned int Gnss_UTC_Hour;
    unsigned int Gnss_UTC_Min;
    unsigned int Gnss_UTC_Second;
    unsigned int Gnss_Latitude_degree;
    double Gnss_Latitude_min;
    unsigned int Gnss_Longitude_degree;
    double Gnss_Longitude_min;
    char Gnss_LatDir;
    char Gnss_LngDir;
    float Gnss_Velocity;
    float Gnss_track;
    char EW;
    char NS;
    float GNSS_High;
    float HDOP;
    char StarNum;

    char Latitude_DegreeDecade;
    char Latitude_DegreeUnit;
    char Latitude_MinDecade;
    char Latitude_MinUnit;
    char Latitude_MinDecimal_1;
    char Latitude_MinDecimal_2;
    char Latitude_MinDecimal_3;
    char Latitude_MinDecimal_4;
    char Latitude_MinDecimal_5;
    char Latitude_MinDecimal_6;
    char Latitude_MinDecimal_7;

    char Longitude_DegreeHundred;
    char Longitude_DegreeDecade;
    char Longitude_DegreeUnit;
    char Longitude_MinDecade;
    char Longitude_MinUnit;
    char Longitude_MinDecimal_1;
    char Longitude_MinDecimal_2;
    char Longitude_MinDecimal_3;
    char Longitude_MinDecimal_4;
    char Longitude_MinDecimal_5;
    char Longitude_MinDecimal_6;
    char Longitude_MinDecimal_7;

    char GNSS_Velo_Hundred;
    char GNSS_Velo_Decade;
    char GNSS_Velo_Unit;
    char GNSS_Velo_Decimal_1;
    char GNSS_Velo_Decimal_2;
    char GNSS_Velo_Decimal_3;

    char Rce_ID_GNSS_1;
    char Rce_ID_GNSS_2;
    char Rce_ID_GNSS_3;
    char Rce_ID_GNSS_4;
    char Rce_ID_GNSS_5;

    float GNSS_POS[2];
    float GNSS_Ven[2];
    char GPSFlag;
    unsigned char GPS_state;
    float PDOP;
    float lat_sigma;
    float lon_sigma;
    float height_sigma;
} gnss_nmea_data_t;


typedef gnss_nmea_data_t _gnssout_data;

/**
 * @brief 初始化GNSS解析模块
 *
 * 清零接收状态、报文缓冲区和解析输出数据。
 */
void gnss_nmea_init(void);

/**
 * @brief 向解析模块输入一个原始字节
 *
 * @param byte GNSS串口接收到的一个字节
 * @return 本字节触发的解析事件，见 gnss_nmea_event_t
 */
gnss_nmea_event_t gnss_nmea_input_byte(uint8_t byte);

/**
 * @brief 向解析模块输入一段连续数据
 *
 * @param data 输入数据地址
 * @param length 输入数据长度
 * @return 本次输入过程中触发的事件位集合；参数无效时返回
 *         GNSS_NMEA_EVENT_NONE
 */
uint32_t gnss_nmea_input(const uint8_t *data, size_t length);

/**
 * @brief 获取最近一次GNSS解析输出
 *
 * @return 模块内部只读数据地址，始终非空
 */
const gnss_nmea_data_t *gnss_nmea_get_data(void);

/**
 * @brief 获取原程序计算的十进制度纬度
 *
 * @return 纬度，单位为度
 */
double gnss_nmea_get_latitude(void);

/**
 * @brief 获取原程序计算的十进制度经度
 *
 * @return 经度，单位为度
 */
double gnss_nmea_get_longitude(void);

/**
 * @brief 获取GGA报文解析得到的高度
 *
 * @return 高度，单位为米
 */
double gnss_nmea_get_height(void);

/*
 * 兼容原程序直接访问全局量的方式。新代码建议使用上面的只读接口。
 */
#ifndef GNSS_NMEA_ENABLE_LEGACY_API
#define GNSS_NMEA_ENABLE_LEGACY_API 1
#endif

#if GNSS_NMEA_ENABLE_LEGACY_API
extern _gnssout_data m_GnssData;
extern double GPSLatitude_Cal;
extern double GPSLongitude_Cal;
extern double GPSHeight_Cal;
extern float GPSHDOP_Cal;
extern char GPSStarNum_Cal;
#endif

#ifdef __cplusplus
}
#endif

#endif

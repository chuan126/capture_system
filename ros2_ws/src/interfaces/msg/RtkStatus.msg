# 本消息只保存既有解析器的直接输出，不表达稳定性或质量判断。
std_msgs/Header header

# gnss_nmea_event_t的原始事件位集合。
uint32 event_mask

# RMC原始有效性字符的ASCII值，例如'A'或'V'。
uint8 rmc_validity
uint8 gps_state
uint8 satellite_count

float32 hdop
float32 pdop
float32 latitude_sigma
float32 longitude_sigma
float32 height_sigma

# 速度沿用解析器单位：节；航向单位：度。
float32 speed_knots
float32 track_degrees

# 日期和时间沿用解析器输出；年份为两位年份。
uint32 utc_year
uint32 utc_month
uint32 utc_day
uint32 utc_hour
uint32 utc_minute
uint32 utc_second

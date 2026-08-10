# 融合定位和ODIN航位推算状态。经纬高的有效性只看valid字段，不用0值判断。
uint8 MODE_INVALID=0
uint8 MODE_RTK=1
uint8 MODE_DEAD_RECKONING=2
uint8 MODE_RTK_RECOVERY=3

uint8 HEADING_INVALID=0
uint8 HEADING_RTK_TRACK=1
uint8 HEADING_RTK_POSITION=2
uint8 HEADING_ODIN=3
uint8 HEADING_IMU_GYRO=4

uint8 SCALE_DISABLED=0
uint8 SCALE_COLLECTING=1
uint8 SCALE_VALID=2
uint8 SCALE_REJECTED=3

std_msgs/Header header

bool valid
uint8 mode
uint8 heading_source

float64 latitude
float64 longitude
float64 altitude

# 用户显示用地理航向，单位deg，North=0，East=90，顺时针为正。
float64 heading_deg

# ODIN四元数按安装矩阵换算得到的车辆三轴姿态，单位deg，仅用于显示和记录。
bool vehicle_attitude_valid
float64 vehicle_pitch_deg
float64 vehicle_roll_deg
float64 vehicle_heading_deg

bool heading_alignment_valid

# ODIN水平坐标系到ENU坐标系的固定航向偏差，单位deg，采用内部ENU数学角。
float64 delta_yaw_deg

uint8 scale_calibration_mode
uint8 scale_status
bool scale_valid
float64 horizontal_scale
float64 vertical_scale
float64 scale_baseline_m
float64 scale_fit_residual_m
float64 heading_baseline_m

string heading_alignment_reason

float64 distance_from_anchor_m
float64 dr_duration_s

float64 rtk_age_s
float64 odometry_age_s
float64 imu_age_s

float64 position_difference_to_rtk_m
string invalid_reason

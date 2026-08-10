#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>

#include "interfaces/msg/clearance_result.hpp"
#include "interfaces/msg/localization_status.hpp"
#include "interfaces/msg/recording_status.hpp"
#include "interfaces/msg/rtk_status.hpp"
#include "interfaces/srv/prepare_recording.hpp"
#include "interfaces/srv/recording_command.hpp"
#include "nav_msgs/msg/odometry.hpp"
#include "rclcpp/rclcpp.hpp"
#include "sensor_msgs/msg/imu.hpp"
#include "sensor_msgs/msg/nav_sat_fix.hpp"
#include "sensor_msgs/msg/temperature.hpp"

#include "localization/Attitude/attitude_matrix.h"

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace
{

class SqliteError : public std::runtime_error
{
public:
  explicit SqliteError(const std::string & message)
  : std::runtime_error(message) {}
};

void check_sqlite(int code, sqlite3 * database, const std::string & action)
{
  if (code == SQLITE_OK || code == SQLITE_DONE || code == SQLITE_ROW) {
    return;
  }
  const char * detail = database != nullptr ? sqlite3_errmsg(database) : "unknown sqlite error";
  throw SqliteError(action + ": " + detail);
}

void execute(sqlite3 * database, const std::string & sql)
{
  char * error_message = nullptr;
  const int result = sqlite3_exec(database, sql.c_str(), nullptr, nullptr, &error_message);
  if (result != SQLITE_OK) {
    const std::string detail = error_message != nullptr ? error_message : sqlite3_errmsg(database);
    sqlite3_free(error_message);
    throw SqliteError(detail);
  }
}

std::string iso_utc_from_ns(std::int64_t timestamp_ns)
{
  if (timestamp_ns <= 0) {
    return {};
  }
  const std::time_t seconds = static_cast<std::time_t>(timestamp_ns / 1'000'000'000LL);
  const auto nanoseconds = timestamp_ns % 1'000'000'000LL;
  std::tm utc{};
  gmtime_r(&seconds, &utc);
  std::ostringstream stream;
  stream << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.'
         << std::setw(9) << std::setfill('0') << nanoseconds << 'Z';
  return stream.str();
}

std::int64_t system_now_ns()
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
}

std::int64_t steady_now_ns()
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string rtk_fix_type(std::uint8_t gps_state)
{
  switch (gps_state) {
    case 4:
      return "RTK_FIXED";
    case 5:
      return "RTK_FLOAT";
    case 2:
      return "DGPS";
    case 1:
      return "SINGLE";
    default:
      return "UNKNOWN";
  }
}

void bind_text(sqlite3_stmt * statement, int index, const std::string & value)
{
  check_sqlite(
    sqlite3_bind_text(statement, index, value.c_str(), -1, SQLITE_TRANSIENT),
    sqlite3_db_handle(statement), "绑定文本参数失败");
}

void bind_nullable_text(
  sqlite3_stmt * statement, int index, const std::optional<std::string> & value)
{
  if (value.has_value()) {
    bind_text(statement, index, *value);
  } else {
    check_sqlite(
      sqlite3_bind_null(statement, index), sqlite3_db_handle(statement), "绑定空文本失败");
  }
}

void bind_nullable_double(
  sqlite3_stmt * statement, int index, const std::optional<double> & value)
{
  if (value.has_value()) {
    check_sqlite(
      sqlite3_bind_double(statement, index, *value), sqlite3_db_handle(statement),
      "绑定浮点参数失败");
  } else {
    check_sqlite(
      sqlite3_bind_null(statement, index), sqlite3_db_handle(statement), "绑定空浮点失败");
  }
}

void bind_nullable_int64(
  sqlite3_stmt * statement, int index, const std::optional<std::int64_t> & value)
{
  if (value.has_value()) {
    check_sqlite(
      sqlite3_bind_int64(statement, index, *value), sqlite3_db_handle(statement),
      "绑定可空整数失败");
  } else {
    check_sqlite(
      sqlite3_bind_null(statement, index), sqlite3_db_handle(statement), "绑定空整数失败");
  }
}

}  // namespace

class DataRecorderNode : public rclcpp::Node
{
public:
  DataRecorderNode()
  : Node("data_recorder_node")
  {
    const char * configured_data_root = std::getenv("CAPTURE_DATA_ROOT");
    data_root_ = declare_parameter<std::string>(
      "data_root", configured_data_root && *configured_data_root ? configured_data_root :
      (fs::current_path() / "runtime").string());
    clearance_topic_ = declare_parameter<std::string>(
      "clearance_topic", "/capture/clearance/result");
    rtk_fix_topic_ = declare_parameter<std::string>("rtk_fix_topic", "/capture/rtk/fix");
    rtk_status_topic_ = declare_parameter<std::string>(
      "rtk_status_topic", "/capture/rtk/status");
    localization_fix_topic_ = declare_parameter<std::string>(
      "localization_fix_topic", "/capture/localization/fix");
    localization_status_topic_ = declare_parameter<std::string>(
      "localization_status_topic", "/capture/localization/status");
    localization_odometry_topic_ = declare_parameter<std::string>(
      "localization_odometry_topic", "/capture/localization/odometry");
    imu_topic_ = declare_parameter<std::string>("imu_topic", "/capture/imu/data");
    odometry_topic_ = declare_parameter<std::string>(
      "odometry_topic", "/capture/odometry/high_rate");
    radar_temperature_topic_ = declare_parameter<std::string>(
      "radar_temperature_topic", "/capture/lidar/temperature");
    sample_rate_hz_ = declare_parameter<double>("sample_rate_hz", 50.0);
    source_timeout_ms_ = declare_parameter<double>("source_timeout_ms", 250.0);
    endpoint_rtk_max_age_ms_ = declare_parameter<double>("endpoint_rtk_max_age_ms", 2000.0);
    odometry_snapshot_max_age_ms_ = declare_parameter<double>(
      "odometry_snapshot_max_age_ms", 250.0);
    radar_temperature_max_age_ms_ = declare_parameter<double>(
      "radar_temperature_max_age_ms", 2000.0);
    transaction_batch_size_ = declare_parameter<int>("transaction_batch_size", 100);
    software_version_ = declare_parameter<std::string>("software_version", "0.2.0");
    algorithm_version_ = declare_parameter<std::string>(
      "algorithm_version", "clearance_engine-current");
    config_version_ = declare_parameter<std::string>(
      "config_version", "clearance_engine_small_board_1cm.yaml");

    if (!(sample_rate_hz_ > 0.0 && sample_rate_hz_ <= 200.0)) {
      throw std::runtime_error("sample_rate_hz必须位于(0, 200]范围");
    }
    if (!(source_timeout_ms_ > 0.0)) {
      throw std::runtime_error("source_timeout_ms必须大于0");
    }
    if (!(endpoint_rtk_max_age_ms_ > 0.0)) {
      throw std::runtime_error("endpoint_rtk_max_age_ms必须大于0");
    }
    if (!(odometry_snapshot_max_age_ms_ > 0.0) || !(radar_temperature_max_age_ms_ > 0.0)) {
      throw std::runtime_error("里程计和雷达温度快照超时参数必须大于0");
    }
    transaction_batch_size_ = std::max(transaction_batch_size_, 1);

    recover_orphan_recordings();

    const auto reliable_qos = rclcpp::QoS(rclcpp::KeepLast(20)).reliable();
    clearance_subscription_ = create_subscription<interfaces::msg::ClearanceResult>(
      clearance_topic_, reliable_qos,
      std::bind(&DataRecorderNode::on_clearance, this, std::placeholders::_1));
    rtk_fix_subscription_ = create_subscription<sensor_msgs::msg::NavSatFix>(
      rtk_fix_topic_, reliable_qos,
      std::bind(&DataRecorderNode::on_rtk_fix, this, std::placeholders::_1));
    rtk_status_subscription_ = create_subscription<interfaces::msg::RtkStatus>(
      rtk_status_topic_, reliable_qos,
      std::bind(&DataRecorderNode::on_rtk_status, this, std::placeholders::_1));
    localization_fix_subscription_ = create_subscription<sensor_msgs::msg::NavSatFix>(
      localization_fix_topic_, reliable_qos,
      std::bind(&DataRecorderNode::on_localization_fix, this, std::placeholders::_1));
    localization_status_subscription_ =
      create_subscription<interfaces::msg::LocalizationStatus>(
      localization_status_topic_, reliable_qos,
      std::bind(&DataRecorderNode::on_localization_status, this, std::placeholders::_1));
    localization_odometry_subscription_ = create_subscription<nav_msgs::msg::Odometry>(
      localization_odometry_topic_, reliable_qos,
      std::bind(&DataRecorderNode::on_localization_odometry, this, std::placeholders::_1));
    imu_subscription_ = create_subscription<sensor_msgs::msg::Imu>(
      imu_topic_, rclcpp::QoS(rclcpp::KeepLast(1000)).best_effort(),
      std::bind(&DataRecorderNode::on_imu, this, std::placeholders::_1));
    odometry_subscription_ = create_subscription<nav_msgs::msg::Odometry>(
      odometry_topic_, rclcpp::QoS(rclcpp::KeepLast(1000)).reliable(),
      std::bind(&DataRecorderNode::on_odometry, this, std::placeholders::_1));
    radar_temperature_subscription_ = create_subscription<sensor_msgs::msg::Temperature>(
      radar_temperature_topic_, rclcpp::QoS(rclcpp::KeepLast(20)).best_effort(),
      std::bind(&DataRecorderNode::on_radar_temperature, this, std::placeholders::_1));

    status_publisher_ = create_publisher<interfaces::msg::RecordingStatus>(
      "/capture/recording/status",
      rclcpp::QoS(rclcpp::KeepLast(10)).reliable().transient_local());

    prepare_service_ = create_service<interfaces::srv::PrepareRecording>(
      "/capture/recording/prepare",
      std::bind(
        &DataRecorderNode::prepare_recording, this,
        std::placeholders::_1, std::placeholders::_2));
    command_service_ = create_service<interfaces::srv::RecordingCommand>(
      "/capture/recording/control",
      std::bind(
        &DataRecorderNode::recording_command, this,
        std::placeholders::_1, std::placeholders::_2));

    const auto period_ns = static_cast<std::int64_t>(1'000'000'000.0 / sample_rate_hz_);
    sample_timer_ = create_wall_timer(
      std::chrono::nanoseconds(period_ns),
      std::bind(&DataRecorderNode::write_periodic_sample, this));

    publish_status("idle", "记录器空闲", "");
  }

  ~DataRecorderNode() override
  {
    std::lock_guard<std::mutex> lock(mutex_);
    close_database_noexcept();
  }

private:
  struct LatestClearance
  {
    std::uint64_t sequence{0};
    std::int64_t source_timestamp_ns{0};
    std::int64_t received_timestamp_ns{0};
    bool valid{false};
    std::optional<double> lidar_to_top_m;
    std::string invalid_reason;
    std::optional<double> quality_score;
    interfaces::msg::ClearanceResult message;
  };

  struct LatestFix
  {
    bool available{false};
    bool valid{false};
    std::int64_t timestamp_ns{0};
    std::int64_t received_monotonic_ns{0};
    double latitude_deg{0.0};
    double longitude_deg{0.0};
    std::optional<double> altitude_m;
    std::string fix_type{"UNKNOWN"};
  };

  struct ImuAccumulator
  {
    std::uint64_t count{0};
    double gyro_x_sum{0.0};
    double gyro_y_sum{0.0};
    double gyro_z_sum{0.0};
    double accel_x_sum{0.0};
    double accel_y_sum{0.0};
    double accel_z_sum{0.0};
  };

  struct LatestOdin
  {
    bool available{false};
    std::int64_t received_monotonic_ns{0};
    double position_x_m{0.0};
    double position_y_m{0.0};
    double position_z_m{0.0};
    double pitch_deg{0.0};
    double roll_deg{0.0};
    double yaw_deg{0.0};
  };

  struct LatestTemperature
  {
    bool available{false};
    std::int64_t received_monotonic_ns{0};
    double celsius{0.0};
  };

  void recover_orphan_recordings()
  {
    const fs::path tasks_root = fs::path(data_root_) / "tasks";
    std::error_code filesystem_error;
    if (!fs::exists(tasks_root, filesystem_error) || filesystem_error) {
      return;
    }
    for (const auto & entry : fs::directory_iterator(tasks_root, filesystem_error)) {
      if (filesystem_error) {
        RCLCPP_WARN(get_logger(), "扫描任务目录失败：%s", filesystem_error.message().c_str());
        return;
      }
      if (!entry.is_directory()) {
        continue;
      }
      const fs::path temporary_path = entry.path() / "measurements.db.tmp";
      const fs::path final_path = entry.path() / "measurements.db";
      if (!fs::is_regular_file(temporary_path, filesystem_error) || filesystem_error) {
        filesystem_error.clear();
        continue;
      }
      if (fs::exists(final_path, filesystem_error)) {
        RCLCPP_WARN(
          get_logger(), "发现未处理的临时记录文件，但正式文件已存在，保留临时文件：%s",
          temporary_path.c_str());
        filesystem_error.clear();
        continue;
      }
      recover_one_orphan_recording(temporary_path, final_path);
    }
  }

  void recover_one_orphan_recording(
    const fs::path & temporary_path, const fs::path & final_path)
  {
    sqlite3 * recovery_database = nullptr;
    try {
      const int open_result = sqlite3_open_v2(
        temporary_path.c_str(), &recovery_database,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, nullptr);
      check_sqlite(open_result, recovery_database, "打开异常中断任务文件失败");
      execute(recovery_database, "PRAGMA busy_timeout=5000");
      execute(recovery_database, "BEGIN IMMEDIATE");

      const auto now_ns = system_now_ns();
      const auto now_text = iso_utc_from_ns(now_ns);
      const auto total_samples = recovery_scalar(
        recovery_database, "SELECT COUNT(*) FROM clearance_samples");
      const auto valid_samples = recovery_scalar(
        recovery_database,
        "SELECT COUNT(*) FROM clearance_samples "
        "WHERE valid=1 AND clearance_height_m IS NOT NULL");
      const auto source_frames = recovery_scalar(
        recovery_database, "SELECT COUNT(*) FROM clearance_source_frames");

      sqlite3_stmt * metadata = nullptr;
      check_sqlite(
        sqlite3_prepare_v2(
          recovery_database,
          "UPDATE recording_metadata SET ended_at=?, complete=0, "
          "exit_rtk_status=CASE WHEN exit_rtk_status='confirmed' THEN exit_rtk_status "
          "ELSE 'unconfirmed' END WHERE id=1",
          -1, &metadata, nullptr),
        recovery_database, "准备异常记录元数据更新失败");
      bind_text(metadata, 1, now_text);
      check_sqlite(sqlite3_step(metadata), recovery_database, "更新异常记录元数据失败");
      sqlite3_finalize(metadata);

      sqlite3_stmt * event = nullptr;
      check_sqlite(
        sqlite3_prepare_v2(
          recovery_database,
          "INSERT INTO task_events(event_type, occurred_at_ns, message, error_code) "
          "VALUES ('interrupted', ?, '记录节点启动时完成异常中断文件收尾', "
          "'data_recorder_restarted')",
          -1, &event, nullptr),
        recovery_database, "准备异常恢复事件写入失败");
      check_sqlite(sqlite3_bind_int64(event, 1, now_ns), recovery_database, "绑定恢复时间失败");
      check_sqlite(sqlite3_step(event), recovery_database, "写入异常恢复事件失败");
      sqlite3_finalize(event);

      sqlite3_stmt * counters = nullptr;
      check_sqlite(
        sqlite3_prepare_v2(
          recovery_database,
          "INSERT OR REPLACE INTO recording_counters("
          "id,total_samples,valid_samples,invalid_samples,source_frames,write_errors) "
          "VALUES (1,?,?,?,?,1)",
          -1, &counters, nullptr),
        recovery_database, "准备异常恢复计数写入失败");
      sqlite3_bind_int64(counters, 1, total_samples);
      sqlite3_bind_int64(counters, 2, valid_samples);
      sqlite3_bind_int64(counters, 3, total_samples - valid_samples);
      sqlite3_bind_int64(counters, 4, source_frames);
      check_sqlite(sqlite3_step(counters), recovery_database, "写入异常恢复计数失败");
      sqlite3_finalize(counters);

      execute(recovery_database, "COMMIT");
      execute(recovery_database, "PRAGMA wal_checkpoint(TRUNCATE)");
      verify_recovery_integrity(recovery_database);
      sqlite3_close(recovery_database);
      recovery_database = nullptr;
      fs::rename(temporary_path, final_path);
      RCLCPP_WARN(
        get_logger(), "已将异常中断任务文件收尾为可读取记录：%s",
        final_path.c_str());
    } catch (const std::exception & error) {
      if (recovery_database != nullptr) {
        try {
          execute(recovery_database, "ROLLBACK");
        } catch (...) {
        }
        sqlite3_close(recovery_database);
      }
      RCLCPP_ERROR(
        get_logger(), "异常中断任务文件恢复失败（%s）：%s",
        temporary_path.c_str(), error.what());
    }
  }

  static std::int64_t recovery_scalar(sqlite3 * database, const std::string & sql)
  {
    sqlite3_stmt * statement = nullptr;
    check_sqlite(
      sqlite3_prepare_v2(database, sql.c_str(), -1, &statement, nullptr),
      database, "准备异常恢复统计查询失败");
    check_sqlite(sqlite3_step(statement), database, "执行异常恢复统计查询失败");
    const auto value = sqlite3_column_int64(statement, 0);
    sqlite3_finalize(statement);
    return value;
  }

  static void verify_recovery_integrity(sqlite3 * database)
  {
    sqlite3_stmt * statement = nullptr;
    check_sqlite(
      sqlite3_prepare_v2(database, "PRAGMA integrity_check", -1, &statement, nullptr),
      database, "准备异常恢复完整性检查失败");
    check_sqlite(sqlite3_step(statement), database, "执行异常恢复完整性检查失败");
    const auto * text = reinterpret_cast<const char *>(sqlite3_column_text(statement, 0));
    const std::string value = text != nullptr ? text : "";
    sqlite3_finalize(statement);
    if (value != "ok") {
      throw SqliteError("异常恢复后的数据库完整性检查失败：" + value);
    }
  }

  void prepare_recording(
    const std::shared_ptr<interfaces::srv::PrepareRecording::Request> request,
    std::shared_ptr<interfaces::srv::PrepareRecording::Response> response)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_) {
      reject_prepare(*response, "recorder_busy", "记录器当前已有活动任务");
      return;
    }
    const std::string lane_side = request->lane_side.empty() ? request->lane : request->lane_side;
    const bool direction_valid = request->travel_direction.empty() ||
      request->travel_direction == "up" || request->travel_direction == "down";
    if (request->task_id.empty() || (lane_side != "left" && lane_side != "right") ||
      !direction_valid || (!request->lane.empty() && request->lane != lane_side) ||
      !std::isfinite(request->lidar_mount_height_m) ||
      !std::isfinite(request->clearance_threshold_m) ||
      request->lidar_mount_height_m < 0.0 || request->lidar_mount_height_m > 20.0 ||
      request->clearance_threshold_m < 0.0 || request->clearance_threshold_m > 20.0)
    {
      reject_prepare(*response, "invalid_parameters", "任务记录参数无效");
      return;
    }

    try {
      reset_session_state();
      task_id_ = request->task_id;
      task_sequence_ = request->task_sequence;
      tunnel_code_ = request->tunnel_code;
      tunnel_name_ = request->tunnel_name;
      travel_direction_ = request->travel_direction.empty() ? "unknown" : request->travel_direction;
      lane_side_ = lane_side;
      lane_ = lane_side;
      lidar_mount_height_m_ = request->lidar_mount_height_m;
      clearance_threshold_m_ = request->clearance_threshold_m;
      start_requested_ns_ = request->requested_at_ns > 0 ? request->requested_at_ns : system_now_ns();

      task_directory_ = fs::path(data_root_) / "tasks" / task_id_;
      final_database_path_ = task_directory_ / "measurements.db";
      temporary_database_path_ = task_directory_ / "measurements.db.tmp";
      fs::create_directories(task_directory_);
      if (fs::exists(final_database_path_)) {
        throw std::runtime_error("任务正式测量文件已经存在，拒绝覆盖");
      }
      if (fs::exists(temporary_database_path_)) {
        fs::remove(temporary_database_path_);
      }

      open_database();
      create_schema();
      insert_metadata();
      begin_transaction();
      entry_rtk_status_ = capture_endpoint("entry", start_requested_ns_);
      update_endpoint_status("entry_rtk_status", entry_rtk_status_);
      insert_event("recording_started", start_requested_ns_, "正式记录已启动", "");

      active_ = true;
      paused_ = false;
      response->success = true;
      response->recording_path = relative_recording_path();
      response->entry_rtk_status = entry_rtk_status_;
      response->error_code.clear();
      response->message = entry_rtk_status_ == "confirmed" ?
        "记录文件已创建，入口RTK已记录" :
        "记录文件已创建，入口RTK坐标未确认，任务继续记录";
      publish_status("recording", response->message, "");
    } catch (const std::exception & error) {
      close_database_noexcept();
      reset_session_state();
      reject_prepare(*response, "storage_error", error.what());
      publish_status("error", error.what(), "storage_error");
    }
  }

  void recording_command(
    const std::shared_ptr<interfaces::srv::RecordingCommand::Request> request,
    std::shared_ptr<interfaces::srv::RecordingCommand::Response> response)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_ || request->task_id != task_id_) {
      reject_command(*response, "recorder_not_active", "指定任务当前未在记录");
      return;
    }
    const auto requested_ns = request->requested_at_ns > 0 ? request->requested_at_ns : system_now_ns();
    try {
      if (request->command == "pause") {
        pause_recording(requested_ns, *response);
      } else if (request->command == "resume") {
        resume_recording(requested_ns, *response);
      } else if (request->command == "finalize") {
        finalize_recording(requested_ns, true, *response);
      } else if (request->command == "abort") {
        finalize_recording(requested_ns, false, *response);
      } else {
        reject_command(*response, "invalid_command", "不支持的记录控制命令");
      }
    } catch (const std::exception & error) {
      handle_runtime_storage_error(error.what());
      reject_command(*response, "storage_error", error.what());
    }
  }

  void pause_recording(
    std::int64_t requested_ns,
    interfaces::srv::RecordingCommand::Response & response)
  {
    if (paused_) {
      reject_command(response, "state_conflict", "记录器已经暂停");
      return;
    }
    flush_transaction();
    paused_ = true;
    imu_accumulator_ = ImuAccumulator{};
    pause_started_ns_ = requested_ns;
    insert_event("paused", requested_ns, "正式测量记录已暂停", "");
    capture_event_rtk("pause", requested_ns, latest_fix_is_fresh());
    populate_command_success(response, "记录已暂停", "not_requested", false);
    publish_status("paused", response.message, "");
  }

  void resume_recording(
    std::int64_t requested_ns,
    interfaces::srv::RecordingCommand::Response & response)
  {
    if (!paused_) {
      reject_command(response, "state_conflict", "记录器当前未暂停");
      return;
    }
    close_pause_interval(requested_ns);
    paused_ = false;
    imu_accumulator_ = ImuAccumulator{};
    begin_transaction();
    insert_event("resumed", requested_ns, "正式测量记录已继续", "");
    capture_event_rtk("resume", requested_ns, latest_fix_is_fresh());
    populate_command_success(response, "记录已继续", "not_requested", false);
    publish_status("recording", response.message, "");
  }

  void finalize_recording(
    std::int64_t requested_ns,
    bool complete,
    interfaces::srv::RecordingCommand::Response & response)
  {
    active_ = false;
    publish_status("finalizing", "正在完成任务文件", "");
    if (paused_) {
      close_pause_interval(requested_ns);
      paused_ = false;
    }
    flush_transaction();
    trim_samples_after(requested_ns);
    exit_rtk_status_ = capture_endpoint("exit", requested_ns);
    update_endpoint_status("exit_rtk_status", exit_rtk_status_);
    insert_event(
      complete ? "completed" : "interrupted", requested_ns,
      complete ? "任务记录正常完成" : "任务记录异常收尾", "");
    update_metadata_completion(requested_ns, complete);
    update_counters();
    execute(database_, "PRAGMA wal_checkpoint(TRUNCATE)");
    verify_integrity();
    sqlite3_close(database_);
    database_ = nullptr;
    transaction_open_ = false;

    fs::rename(temporary_database_path_, final_database_path_);
    response.success = true;
    response.recording_path = relative_recording_path();
    response.rtk_status = exit_rtk_status_;
    response.total_samples = total_samples_;
    response.valid_samples = valid_samples_;
    response.invalid_samples = invalid_samples_;
    response.complete = complete;
    response.error_code.clear();
    response.message = exit_rtk_status_ == "confirmed" ?
      "任务文件已完成，出口RTK已记录" :
      "任务文件已完成，出口RTK坐标未确认";
    publish_status(complete ? "completed" : "interrupted", response.message, "");
    reset_session_state();
  }

  void on_clearance(const interfaces::msg::ClearanceResult::SharedPtr message)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    LatestClearance latest;
    latest.sequence = ++source_sequence_;
    latest.source_timestamp_ns = rclcpp::Time(message->header.stamp).nanoseconds();
    if (latest.source_timestamp_ns <= 0) {
      latest.source_timestamp_ns = system_now_ns();
    }
    latest.received_timestamp_ns = system_now_ns();
    latest.valid = message->valid && std::isfinite(message->lidar_to_top_m);
    if (std::isfinite(message->lidar_to_top_m)) {
      latest.lidar_to_top_m = message->lidar_to_top_m;
    }
    latest.invalid_reason = message->invalid_reason;
    if (std::isfinite(message->valid_point_ratio)) {
      latest.quality_score = std::clamp(message->valid_point_ratio, 0.0, 1.0);
    }
    latest.message = *message;
    latest_clearance_ = latest;

    if (active_ && !paused_ && database_ != nullptr) {
      try {
        insert_source_frame(latest);
      } catch (const std::exception & error) {
        handle_runtime_storage_error(error.what());
      }
    }
  }

  void on_rtk_fix(const sensor_msgs::msg::NavSatFix::SharedPtr message)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    latest_fix_.available = true;
    latest_fix_.received_monotonic_ns = steady_now_ns();
    latest_fix_.timestamp_ns = rclcpp::Time(message->header.stamp).nanoseconds();
    if (latest_fix_.timestamp_ns <= 0) {
      latest_fix_.timestamp_ns = system_now_ns();
    }
    latest_fix_.latitude_deg = message->latitude;
    latest_fix_.longitude_deg = message->longitude;
    latest_fix_.valid = message->status.status >= 0 &&
      std::isfinite(message->latitude) && std::isfinite(message->longitude);
    latest_fix_.altitude_m = std::isfinite(message->altitude) ?
      std::optional<double>(message->altitude) : std::nullopt;

    if (active_ && database_ != nullptr) {
      try {
        insert_rtk_sample(latest_fix_);
      } catch (const std::exception & error) {
        handle_runtime_storage_error(error.what());
      }
    }
  }

  void on_rtk_status(const interfaces::msg::RtkStatus::SharedPtr message)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    latest_fix_.fix_type = rtk_fix_type(message->gps_state);
  }

  void on_localization_fix(const sensor_msgs::msg::NavSatFix::SharedPtr message)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_ && database_ != nullptr) {
      try {
        insert_localization_fix(*message);
      } catch (const std::exception & error) {
        handle_runtime_storage_error(error.what());
      }
    }
  }

  void on_localization_status(const interfaces::msg::LocalizationStatus::SharedPtr message)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_ && database_ != nullptr) {
      try {
        insert_localization_status(*message);
      } catch (const std::exception & error) {
        handle_runtime_storage_error(error.what());
      }
    }
  }

  void on_localization_odometry(const nav_msgs::msg::Odometry::SharedPtr message)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_ && database_ != nullptr) {
      try {
        insert_localization_odometry(*message);
      } catch (const std::exception & error) {
        handle_runtime_storage_error(error.what());
      }
    }
  }

  void on_imu(const sensor_msgs::msg::Imu::SharedPtr message)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_ || paused_) {
      return;
    }
    const auto & gyro = message->angular_velocity;
    const auto & accel = message->linear_acceleration;
    if (!std::isfinite(gyro.x) || !std::isfinite(gyro.y) || !std::isfinite(gyro.z) ||
      !std::isfinite(accel.x) || !std::isfinite(accel.y) || !std::isfinite(accel.z))
    {
      return;
    }
    ++imu_accumulator_.count;
    imu_accumulator_.gyro_x_sum += gyro.x;
    imu_accumulator_.gyro_y_sum += gyro.y;
    imu_accumulator_.gyro_z_sum += gyro.z;
    imu_accumulator_.accel_x_sum += accel.x;
    imu_accumulator_.accel_y_sum += accel.y;
    imu_accumulator_.accel_z_sum += accel.z;
  }

  void on_odometry(const nav_msgs::msg::Odometry::SharedPtr message)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto & position = message->pose.pose.position;
    const auto & orientation = message->pose.pose.orientation;
    if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z) ||
      !std::isfinite(orientation.x) || !std::isfinite(orientation.y) ||
      !std::isfinite(orientation.z) || !std::isfinite(orientation.w))
    {
      return;
    }
    const double norm = std::sqrt(
      orientation.x * orientation.x + orientation.y * orientation.y +
      orientation.z * orientation.z + orientation.w * orientation.w);
    if (!std::isfinite(norm) || norm <= 1.0e-12) {
      return;
    }
    double quaternion_wxyz[4]{
      orientation.w / norm, orientation.x / norm, orientation.y / norm, orientation.z / norm};
    double attitude_rad[3]{};
    q2att(quaternion_wxyz, attitude_rad);
    if (!std::isfinite(attitude_rad[0]) || !std::isfinite(attitude_rad[1]) ||
      !std::isfinite(attitude_rad[2]))
    {
      return;
    }
    constexpr double radians_to_degrees = 180.0 / 3.14159265358979323846;
    latest_odin_.available = true;
    latest_odin_.received_monotonic_ns = steady_now_ns();
    latest_odin_.position_x_m = position.x;
    latest_odin_.position_y_m = position.y;
    latest_odin_.position_z_m = position.z;
    latest_odin_.pitch_deg = attitude_rad[0] * radians_to_degrees;
    latest_odin_.roll_deg = attitude_rad[1] * radians_to_degrees;
    latest_odin_.yaw_deg = attitude_rad[2] * radians_to_degrees;
  }

  void on_radar_temperature(const sensor_msgs::msg::Temperature::SharedPtr message)
  {
    if (!std::isfinite(message->temperature)) {
      return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    latest_temperature_.available = true;
    latest_temperature_.received_monotonic_ns = steady_now_ns();
    latest_temperature_.celsius = message->temperature;
  }

  void write_periodic_sample()
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_ || paused_ || database_ == nullptr) {
      return;
    }
    try {
      const auto recorded_ns = system_now_ns();
      const auto elapsed_ms = std::max(
        0.0, static_cast<double>(recorded_ns - start_requested_ns_) / 1'000'000.0);
      std::int64_t source_ns = recorded_ns;
      std::uint64_t source_sequence = 0;
      double source_age_ms = 0.0;
      bool valid = false;
      std::optional<double> value;
      std::optional<double> quality;
      std::optional<std::string> invalid_reason;
      bool repeated = false;
      std::uint32_t repeat_index = 0;

      if (latest_clearance_.has_value()) {
        const auto & source = *latest_clearance_;
        insert_source_frame(source);
        source_ns = source.source_timestamp_ns;
        source_sequence = source.sequence;
        // 数据新鲜度按本机接收时间计算，避免设备时间戳与系统时钟基准不同
        // 时将正常源帧误判为超时。源时间戳仍原样保存用于追溯。
        source_age_ms = std::max(
          0.0,
          static_cast<double>(recorded_ns - source.received_timestamp_ns) / 1'000'000.0);
        repeated = source_sequence == last_written_source_sequence_;
        if (repeated) {
          ++current_repeat_index_;
        } else {
          current_repeat_index_ = 0;
          last_written_source_sequence_ = source_sequence;
        }
        repeat_index = current_repeat_index_;
        quality = source.quality_score;
        if (source_age_ms > source_timeout_ms_) {
          invalid_reason = "source_timeout";
        } else if (!source.valid || !source.lidar_to_top_m.has_value()) {
          invalid_reason = source.invalid_reason.empty() ?
            std::optional<std::string>("source_invalid") :
            std::optional<std::string>(source.invalid_reason);
        } else {
          valid = true;
          value = source.lidar_to_top_m;
        }
      } else {
        invalid_reason = "source_unavailable";
      }

      const std::optional<std::int64_t> rtk_timestamp_ns = latest_fix_.available ?
        std::optional<std::int64_t>(latest_fix_.timestamp_ns) : std::nullopt;
      const std::uint64_t imu_sample_count = imu_accumulator_.count;
      std::optional<double> gyro_x;
      std::optional<double> gyro_y;
      std::optional<double> gyro_z;
      std::optional<double> accel_x;
      std::optional<double> accel_y;
      std::optional<double> accel_z;
      if (imu_sample_count > 0U) {
        const double inverse_count = 1.0 / static_cast<double>(imu_sample_count);
        gyro_x = imu_accumulator_.gyro_x_sum * inverse_count;
        gyro_y = imu_accumulator_.gyro_y_sum * inverse_count;
        gyro_z = imu_accumulator_.gyro_z_sum * inverse_count;
        accel_x = imu_accumulator_.accel_x_sum * inverse_count;
        accel_y = imu_accumulator_.accel_y_sum * inverse_count;
        accel_z = imu_accumulator_.accel_z_sum * inverse_count;
      }
      imu_accumulator_ = ImuAccumulator{};

      std::optional<double> minimum_point_x;
      std::optional<double> minimum_point_y;
      std::optional<double> minimum_point_z;
      if (latest_clearance_.has_value()) {
        const auto & minimum = latest_clearance_->message;
        if (std::isfinite(minimum.minimum_position_east_m)) {
          minimum_point_x = minimum.minimum_position_east_m;
        }
        if (std::isfinite(minimum.minimum_position_north_m)) {
          minimum_point_y = minimum.minimum_position_north_m;
        }
        if (std::isfinite(minimum.minimum_position_up_m)) {
          minimum_point_z = minimum.minimum_position_up_m;
        }
      }

      const auto monotonic_now_ns = steady_now_ns();
      const bool odin_fresh = latest_odin_.available &&
        static_cast<double>(std::max<std::int64_t>(
          0, monotonic_now_ns - latest_odin_.received_monotonic_ns)) / 1'000'000.0 <=
        odometry_snapshot_max_age_ms_;
      const bool temperature_fresh = latest_temperature_.available &&
        static_cast<double>(std::max<std::int64_t>(
          0, monotonic_now_ns - latest_temperature_.received_monotonic_ns)) / 1'000'000.0 <=
        radar_temperature_max_age_ms_;

      sqlite3_stmt * statement = nullptr;
      check_sqlite(
        sqlite3_prepare_v2(
          database_,
          "INSERT INTO clearance_samples ("
           "sample_index, source_timestamp_ns, recorded_timestamp_ns, elapsed_ms, "
           "lidar_to_top_m, clearance_height_m, valid, invalid_reason, quality_score, "
           "source_sequence, source_age_ms, is_repeated, repeat_index, rtk_timestamp_ns, "
           "gyro_x_rad_s, gyro_y_rad_s, gyro_z_rad_s, accel_x_m_s2, accel_y_m_s2, "
           "accel_z_m_s2, imu_sample_count, radar_temperature_c, minimum_point_x_m, "
           "minimum_point_y_m, minimum_point_z_m, odin_pitch_deg, odin_roll_deg, "
           "odin_yaw_deg, odin_position_x_m, odin_position_y_m, odin_position_z_m) "
           "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, "
           "?, ?, ?, ?, ?, ?, ?, ?, ?)",
          -1, &statement, nullptr),
        database_, "准备50Hz样本写入失败");
      check_sqlite(sqlite3_bind_int64(statement, 1, static_cast<sqlite3_int64>(total_samples_)), database_, "绑定样本序号失败");
      check_sqlite(sqlite3_bind_int64(statement, 2, source_ns), database_, "绑定源时间失败");
      check_sqlite(sqlite3_bind_int64(statement, 3, recorded_ns), database_, "绑定记录时间失败");
      check_sqlite(sqlite3_bind_double(statement, 4, elapsed_ms), database_, "绑定相对时间失败");
      bind_nullable_double(statement, 5, value);
      std::optional<double> clearance_height;
      if (value.has_value()) {
        clearance_height = *value + lidar_mount_height_m_;
      }
      // 保留 lidar_to_top_m 作为算法原始输出，正式净空高度记录安装高度修正后的值。
      bind_nullable_double(statement, 6, clearance_height);
      check_sqlite(sqlite3_bind_int(statement, 7, valid ? 1 : 0), database_, "绑定有效标志失败");
      bind_nullable_text(statement, 8, invalid_reason);
      bind_nullable_double(statement, 9, quality);
      check_sqlite(sqlite3_bind_int64(statement, 10, static_cast<sqlite3_int64>(source_sequence)), database_, "绑定源序号失败");
      check_sqlite(sqlite3_bind_double(statement, 11, source_age_ms), database_, "绑定源年龄失败");
      check_sqlite(sqlite3_bind_int(statement, 12, repeated ? 1 : 0), database_, "绑定重复标志失败");
      check_sqlite(sqlite3_bind_int(statement, 13, static_cast<int>(repeat_index)), database_, "绑定重复序号失败");
      bind_nullable_int64(statement, 14, rtk_timestamp_ns);
      bind_nullable_double(statement, 15, gyro_x);
      bind_nullable_double(statement, 16, gyro_y);
      bind_nullable_double(statement, 17, gyro_z);
      bind_nullable_double(statement, 18, accel_x);
      bind_nullable_double(statement, 19, accel_y);
      bind_nullable_double(statement, 20, accel_z);
      check_sqlite(
        sqlite3_bind_int64(statement, 21, static_cast<sqlite3_int64>(imu_sample_count)),
        database_, "绑定IMU平均样本数失败");
      bind_nullable_double(
        statement, 22, temperature_fresh ?
        std::optional<double>(latest_temperature_.celsius) : std::nullopt);
      bind_nullable_double(statement, 23, minimum_point_x);
      bind_nullable_double(statement, 24, minimum_point_y);
      bind_nullable_double(statement, 25, minimum_point_z);
      bind_nullable_double(
        statement, 26, odin_fresh ? std::optional<double>(latest_odin_.pitch_deg) : std::nullopt);
      bind_nullable_double(
        statement, 27, odin_fresh ? std::optional<double>(latest_odin_.roll_deg) : std::nullopt);
      bind_nullable_double(
        statement, 28, odin_fresh ? std::optional<double>(latest_odin_.yaw_deg) : std::nullopt);
      bind_nullable_double(
        statement, 29, odin_fresh ?
        std::optional<double>(latest_odin_.position_x_m) : std::nullopt);
      bind_nullable_double(
        statement, 30, odin_fresh ?
        std::optional<double>(latest_odin_.position_y_m) : std::nullopt);
      bind_nullable_double(
        statement, 31, odin_fresh ?
        std::optional<double>(latest_odin_.position_z_m) : std::nullopt);
      check_sqlite(sqlite3_step(statement), database_, "写入50Hz样本失败");
      sqlite3_finalize(statement);

      ++total_samples_;
      if (valid) {
        ++valid_samples_;
      } else {
        ++invalid_samples_;
      }
      ++pending_transaction_samples_;
      if (pending_transaction_samples_ >= static_cast<std::uint64_t>(transaction_batch_size_)) {
        flush_transaction();
        begin_transaction();
      }
    } catch (const std::exception & error) {
      handle_runtime_storage_error(error.what());
    }
  }

  void open_database()
  {
    const int result = sqlite3_open_v2(
      temporary_database_path_.c_str(), &database_,
      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    check_sqlite(result, database_, "创建任务测量数据库失败");
    execute(database_, "PRAGMA journal_mode=WAL");
    execute(database_, "PRAGMA synchronous=NORMAL");
    execute(database_, "PRAGMA foreign_keys=ON");
    execute(database_, "PRAGMA busy_timeout=5000");
  }

  void create_schema()
  {
    execute(database_, R"SQL(
      CREATE TABLE recording_metadata (
        id INTEGER PRIMARY KEY CHECK (id = 1),
        schema_version INTEGER NOT NULL CHECK (schema_version > 0),
        task_id TEXT NOT NULL,
        data_origin TEXT NOT NULL CHECK (data_origin IN ('recorded', 'test_fixture')),
        lane TEXT NOT NULL CHECK (lane IN ('left', 'right', 'unknown')),
        travel_direction TEXT NOT NULL CHECK (travel_direction IN ('up', 'down', 'unknown')),
        lane_side TEXT NOT NULL CHECK (lane_side IN ('left', 'right', 'unknown')),
        started_at TEXT NOT NULL,
        ended_at TEXT,
        complete INTEGER NOT NULL CHECK (complete IN (0, 1)),
        nominal_sample_rate_hz REAL NOT NULL CHECK (nominal_sample_rate_hz > 0),
        algorithm_version TEXT,
        config_version TEXT,
        software_version TEXT,
        lidar_mount_height_m REAL,
        clearance_threshold_m REAL,
        entry_rtk_status TEXT NOT NULL DEFAULT 'pending',
        exit_rtk_status TEXT NOT NULL DEFAULT 'not_requested'
      );
      CREATE TABLE clearance_samples (
        sample_index INTEGER PRIMARY KEY CHECK (sample_index >= 0),
        source_timestamp_ns INTEGER NOT NULL,
        recorded_timestamp_ns INTEGER NOT NULL,
        elapsed_ms REAL NOT NULL CHECK (elapsed_ms >= 0),
        lidar_to_top_m REAL,
        clearance_height_m REAL,
        valid INTEGER NOT NULL CHECK (valid IN (0, 1)),
        invalid_reason TEXT,
        quality_score REAL,
        source_sequence INTEGER NOT NULL DEFAULT 0,
        source_age_ms REAL NOT NULL DEFAULT 0,
        is_repeated INTEGER NOT NULL DEFAULT 0 CHECK (is_repeated IN (0, 1)),
        repeat_index INTEGER NOT NULL DEFAULT 0,
        rtk_timestamp_ns INTEGER,
        gyro_x_rad_s REAL,
        gyro_y_rad_s REAL,
        gyro_z_rad_s REAL,
        accel_x_m_s2 REAL,
        accel_y_m_s2 REAL,
        accel_z_m_s2 REAL,
        imu_sample_count INTEGER NOT NULL DEFAULT 0 CHECK (imu_sample_count >= 0),
        radar_temperature_c REAL,
        minimum_point_x_m REAL,
        minimum_point_y_m REAL,
        minimum_point_z_m REAL,
        odin_pitch_deg REAL,
        odin_roll_deg REAL,
        odin_yaw_deg REAL,
        odin_position_x_m REAL,
        odin_position_y_m REAL,
        odin_position_z_m REAL
      );
      CREATE INDEX clearance_samples_timestamp_idx ON clearance_samples(source_timestamp_ns);
      CREATE TABLE clearance_source_frames (
        source_sequence INTEGER PRIMARY KEY,
        source_timestamp_ns INTEGER NOT NULL,
        received_timestamp_ns INTEGER NOT NULL,
        valid INTEGER NOT NULL CHECK (valid IN (0, 1)),
        lidar_to_top_m REAL,
        invalid_reason TEXT,
        quality_score REAL,
        candidate_region_count INTEGER,
        selected_inlier_count INTEGER,
        selected_grid_area_m2 REAL,
        selected_tilt_deg REAL,
        selected_residual_median_m REAL,
        selected_residual_p95_m REAL,
        processing_time_ms REAL
      );
      CREATE TABLE rtk_samples (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        timestamp_ns INTEGER NOT NULL,
        latitude_deg REAL,
        longitude_deg REAL,
        altitude_m REAL,
        fix_type TEXT NOT NULL,
        valid INTEGER NOT NULL CHECK (valid IN (0, 1))
      );
      CREATE TABLE localization_fix_samples (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        timestamp_ns INTEGER NOT NULL,
        latitude_deg REAL,
        longitude_deg REAL,
        altitude_m REAL,
        fix_status INTEGER NOT NULL,
        valid INTEGER NOT NULL CHECK (valid IN (0, 1))
      );
      CREATE TABLE localization_status_samples (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        timestamp_ns INTEGER NOT NULL,
        valid INTEGER NOT NULL CHECK (valid IN (0, 1)),
        mode INTEGER NOT NULL,
        heading_source INTEGER NOT NULL,
        latitude_deg REAL NOT NULL,
        longitude_deg REAL NOT NULL,
        altitude_m REAL NOT NULL,
        heading_deg REAL NOT NULL,
        odin_attitude_valid INTEGER NOT NULL CHECK (odin_attitude_valid IN (0, 1)),
        odin_pitch_deg REAL NOT NULL,
        odin_roll_deg REAL NOT NULL,
        odin_yaw_deg REAL NOT NULL,
        heading_alignment_valid INTEGER NOT NULL CHECK (heading_alignment_valid IN (0, 1)),
        delta_yaw_deg REAL NOT NULL,
        scale_calibration_mode INTEGER NOT NULL CHECK (scale_calibration_mode IN (0, 1)),
        scale_status INTEGER NOT NULL,
        scale_valid INTEGER NOT NULL CHECK (scale_valid IN (0, 1)),
        horizontal_scale REAL NOT NULL,
        vertical_scale REAL NOT NULL,
        scale_baseline_m REAL NOT NULL,
        scale_fit_residual_m REAL NOT NULL,
        heading_baseline_m REAL NOT NULL,
        heading_alignment_reason TEXT NOT NULL,
        distance_from_anchor_m REAL NOT NULL,
        dr_duration_s REAL NOT NULL,
        rtk_age_s REAL NOT NULL,
        odometry_age_s REAL NOT NULL,
        imu_age_s REAL NOT NULL,
        position_difference_to_rtk_m REAL NOT NULL,
        invalid_reason TEXT NOT NULL
      );
      CREATE TABLE localization_odometry_samples (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        timestamp_ns INTEGER NOT NULL,
        frame_id TEXT NOT NULL,
        child_frame_id TEXT NOT NULL,
        east_m REAL NOT NULL,
        north_m REAL NOT NULL,
        up_m REAL NOT NULL,
        qx REAL NOT NULL,
        qy REAL NOT NULL,
        qz REAL NOT NULL,
        qw REAL NOT NULL
      );
      CREATE TABLE rtk_endpoints (
        role TEXT PRIMARY KEY CHECK (role IN ('entry', 'exit')),
        timestamp_ns INTEGER NOT NULL,
        latitude_deg REAL NOT NULL,
        longitude_deg REAL NOT NULL,
        altitude_m REAL,
        fix_type TEXT NOT NULL,
        valid INTEGER NOT NULL CHECK (valid IN (0, 1))
      );
      CREATE TABLE event_rtk_snapshots (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        event_type TEXT NOT NULL,
        requested_timestamp_ns INTEGER NOT NULL,
        coordinate_timestamp_ns INTEGER,
        latitude_deg REAL,
        longitude_deg REAL,
        altitude_m REAL,
        fix_type TEXT,
        valid INTEGER NOT NULL CHECK (valid IN (0, 1))
      );
      CREATE TABLE pause_intervals (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        started_elapsed_ms REAL NOT NULL,
        ended_elapsed_ms REAL NOT NULL,
        CHECK (ended_elapsed_ms >= started_elapsed_ms)
      );
      CREATE TABLE task_events (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        event_type TEXT NOT NULL,
        occurred_at_ns INTEGER NOT NULL,
        message TEXT,
        error_code TEXT
      );
      CREATE TABLE recording_counters (
        id INTEGER PRIMARY KEY CHECK (id = 1),
        total_samples INTEGER NOT NULL,
        valid_samples INTEGER NOT NULL,
        invalid_samples INTEGER NOT NULL,
        source_frames INTEGER NOT NULL,
        write_errors INTEGER NOT NULL
      );
    )SQL");
  }

  void insert_metadata()
  {
    sqlite3_stmt * statement = nullptr;
    check_sqlite(
      sqlite3_prepare_v2(
        database_,
        "INSERT INTO recording_metadata ("
        "id, schema_version, task_id, data_origin, lane, travel_direction, lane_side, "
        "started_at, ended_at, complete, nominal_sample_rate_hz, algorithm_version, "
        "config_version, software_version, lidar_mount_height_m, clearance_threshold_m, "
        "entry_rtk_status, exit_rtk_status) "
        "VALUES (1, 7, ?, 'recorded', ?, ?, ?, ?, NULL, 0, ?, ?, ?, ?, ?, ?, 'pending', 'not_requested')",
        -1, &statement, nullptr),
      database_, "准备任务元数据写入失败");
    bind_text(statement, 1, task_id_);
    bind_text(statement, 2, lane_);
    bind_text(statement, 3, travel_direction_);
    bind_text(statement, 4, lane_side_);
    bind_text(statement, 5, iso_utc_from_ns(start_requested_ns_));
    check_sqlite(sqlite3_bind_double(statement, 6, sample_rate_hz_), database_, "绑定采样频率失败");
    bind_text(statement, 7, algorithm_version_);
    bind_text(statement, 8, config_version_);
    bind_text(statement, 9, software_version_);
    check_sqlite(sqlite3_bind_double(statement, 10, lidar_mount_height_m_), database_, "绑定安装高度失败");
    check_sqlite(sqlite3_bind_double(statement, 11, clearance_threshold_m_), database_, "绑定高度阈值失败");
    check_sqlite(sqlite3_step(statement), database_, "写入任务元数据失败");
    sqlite3_finalize(statement);
  }

  void insert_source_frame(const LatestClearance & source)
  {
    if (source.sequence == last_persisted_source_sequence_) {
      return;
    }
    sqlite3_stmt * statement = nullptr;
    check_sqlite(
      sqlite3_prepare_v2(
        database_,
        "INSERT OR REPLACE INTO clearance_source_frames ("
        "source_sequence, source_timestamp_ns, received_timestamp_ns, valid, lidar_to_top_m, "
        "invalid_reason, quality_score, candidate_region_count, selected_inlier_count, "
        "selected_grid_area_m2, selected_tilt_deg, selected_residual_median_m, "
        "selected_residual_p95_m, processing_time_ms) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
        -1, &statement, nullptr),
      database_, "准备源帧写入失败");
    check_sqlite(sqlite3_bind_int64(statement, 1, source.sequence), database_, "绑定源帧序号失败");
    check_sqlite(sqlite3_bind_int64(statement, 2, source.source_timestamp_ns), database_, "绑定源帧时间失败");
    check_sqlite(
      sqlite3_bind_int64(statement, 3, source.received_timestamp_ns),
      database_, "绑定接收时间失败");
    check_sqlite(sqlite3_bind_int(statement, 4, source.valid ? 1 : 0), database_, "绑定源帧有效性失败");
    bind_nullable_double(statement, 5, source.lidar_to_top_m);
    bind_nullable_text(
      statement, 6,
      source.invalid_reason.empty() ? std::nullopt : std::optional<std::string>(source.invalid_reason));
    bind_nullable_double(statement, 7, source.quality_score);
    check_sqlite(sqlite3_bind_int(statement, 8, static_cast<int>(source.message.candidate_count)), database_, "绑定合格连通区域数量失败");
    check_sqlite(sqlite3_bind_int(statement, 9, static_cast<int>(source.message.selected_inlier_count)), database_, "绑定内点数量失败");
    bind_nullable_double(statement, 10, std::isfinite(source.message.selected_area_m2) ? std::optional<double>(source.message.selected_area_m2) : std::nullopt);
    bind_nullable_double(statement, 11, std::isfinite(source.message.selected_tilt_deg) ? std::optional<double>(source.message.selected_tilt_deg) : std::nullopt);
    bind_nullable_double(statement, 12, std::isfinite(source.message.residual_median_m) ? std::optional<double>(source.message.residual_median_m) : std::nullopt);
    bind_nullable_double(statement, 13, std::isfinite(source.message.residual_p95_m) ? std::optional<double>(source.message.residual_p95_m) : std::nullopt);
    bind_nullable_double(statement, 14, std::isfinite(source.message.processing_time_ms) ? std::optional<double>(source.message.processing_time_ms) : std::nullopt);
    check_sqlite(sqlite3_step(statement), database_, "写入源帧失败");
    sqlite3_finalize(statement);
    last_persisted_source_sequence_ = source.sequence;
    ++source_frames_;
  }

  void insert_rtk_sample(const LatestFix & fix)
  {
    sqlite3_stmt * statement = nullptr;
    check_sqlite(
      sqlite3_prepare_v2(
        database_,
        "INSERT INTO rtk_samples (timestamp_ns, latitude_deg, longitude_deg, altitude_m, fix_type, valid) "
        "VALUES (?, ?, ?, ?, ?, ?)",
        -1, &statement, nullptr),
      database_, "准备RTK样本写入失败");
    check_sqlite(sqlite3_bind_int64(statement, 1, fix.timestamp_ns), database_, "绑定RTK时间失败");
    if (fix.available) {
      check_sqlite(sqlite3_bind_double(statement, 2, fix.latitude_deg), database_, "绑定纬度失败");
      check_sqlite(sqlite3_bind_double(statement, 3, fix.longitude_deg), database_, "绑定经度失败");
    } else {
      sqlite3_bind_null(statement, 2);
      sqlite3_bind_null(statement, 3);
    }
    bind_nullable_double(statement, 4, fix.altitude_m);
    bind_text(statement, 5, fix.fix_type);
    check_sqlite(sqlite3_bind_int(statement, 6, fix.valid ? 1 : 0), database_, "绑定RTK有效性失败");
    check_sqlite(sqlite3_step(statement), database_, "写入RTK样本失败");
    sqlite3_finalize(statement);
  }

  void insert_localization_fix(const sensor_msgs::msg::NavSatFix & fix)
  {
    sqlite3_stmt * statement = nullptr;
    check_sqlite(
      sqlite3_prepare_v2(
        database_,
        "INSERT INTO localization_fix_samples ("
        "timestamp_ns, latitude_deg, longitude_deg, altitude_m, fix_status, valid) "
        "VALUES (?, ?, ?, ?, ?, ?)",
        -1, &statement, nullptr),
      database_, "准备融合定位fix写入失败");
    const auto stamp_ns = rclcpp::Time(fix.header.stamp).nanoseconds();
    check_sqlite(sqlite3_bind_int64(statement, 1, stamp_ns > 0 ? stamp_ns : system_now_ns()), database_, "绑定融合fix时间失败");
    if (std::isfinite(fix.latitude) && std::isfinite(fix.longitude)) {
      check_sqlite(sqlite3_bind_double(statement, 2, fix.latitude), database_, "绑定融合纬度失败");
      check_sqlite(sqlite3_bind_double(statement, 3, fix.longitude), database_, "绑定融合经度失败");
    } else {
      sqlite3_bind_null(statement, 2);
      sqlite3_bind_null(statement, 3);
    }
    bind_nullable_double(
      statement, 4, std::isfinite(fix.altitude) ? std::optional<double>(fix.altitude) :
      std::nullopt);
    check_sqlite(sqlite3_bind_int(statement, 5, fix.status.status), database_, "绑定融合fix状态失败");
    const bool valid = fix.status.status >= 0 && std::isfinite(fix.latitude) &&
      std::isfinite(fix.longitude);
    check_sqlite(sqlite3_bind_int(statement, 6, valid ? 1 : 0), database_, "绑定融合fix有效性失败");
    check_sqlite(sqlite3_step(statement), database_, "写入融合定位fix失败");
    sqlite3_finalize(statement);
  }

  void insert_localization_status(const interfaces::msg::LocalizationStatus & status)
  {
    sqlite3_stmt * statement = nullptr;
    check_sqlite(
      sqlite3_prepare_v2(
        database_,
        "INSERT INTO localization_status_samples ("
        "timestamp_ns, valid, mode, heading_source, latitude_deg, longitude_deg, altitude_m, "
        "heading_deg, odin_attitude_valid, odin_pitch_deg, odin_roll_deg, odin_yaw_deg, "
        "heading_alignment_valid, delta_yaw_deg, scale_calibration_mode, scale_status, "
        "scale_valid, horizontal_scale, vertical_scale, scale_baseline_m, scale_fit_residual_m, "
        "heading_baseline_m, heading_alignment_reason, "
        "distance_from_anchor_m, dr_duration_s, rtk_age_s, odometry_age_s, imu_age_s, "
        "position_difference_to_rtk_m, invalid_reason) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, "
        "?, ?, ?, ?, ?, ?)",
        -1, &statement, nullptr),
      database_, "准备融合定位status写入失败");
    const auto stamp_ns = rclcpp::Time(status.header.stamp).nanoseconds();
    check_sqlite(sqlite3_bind_int64(statement, 1, stamp_ns > 0 ? stamp_ns : system_now_ns()), database_, "绑定融合status时间失败");
    check_sqlite(sqlite3_bind_int(statement, 2, status.valid ? 1 : 0), database_, "绑定融合有效性失败");
    check_sqlite(sqlite3_bind_int(statement, 3, status.mode), database_, "绑定融合模式失败");
    check_sqlite(sqlite3_bind_int(statement, 4, status.heading_source), database_, "绑定航向来源失败");
    check_sqlite(sqlite3_bind_double(statement, 5, status.latitude), database_, "绑定融合status纬度失败");
    check_sqlite(sqlite3_bind_double(statement, 6, status.longitude), database_, "绑定融合status经度失败");
    check_sqlite(sqlite3_bind_double(statement, 7, status.altitude), database_, "绑定融合status高度失败");
    check_sqlite(sqlite3_bind_double(statement, 8, status.heading_deg), database_, "绑定融合航向失败");
    check_sqlite(sqlite3_bind_int(statement, 9, status.odin_attitude_valid ? 1 : 0), database_, "绑定ODIN姿态有效性失败");
    check_sqlite(sqlite3_bind_double(statement, 10, status.odin_pitch_deg), database_, "绑定ODIN俯仰失败");
    check_sqlite(sqlite3_bind_double(statement, 11, status.odin_roll_deg), database_, "绑定ODIN横滚失败");
    check_sqlite(sqlite3_bind_double(statement, 12, status.odin_yaw_deg), database_, "绑定ODIN欧拉方位失败");
    check_sqlite(sqlite3_bind_int(statement, 13, status.heading_alignment_valid ? 1 : 0), database_, "绑定航向对齐有效性失败");
    check_sqlite(sqlite3_bind_double(statement, 14, status.delta_yaw_deg), database_, "绑定航向偏差失败");
    check_sqlite(sqlite3_bind_int(statement, 15, status.scale_calibration_mode), database_, "绑定尺度模式失败");
    check_sqlite(sqlite3_bind_int(statement, 16, status.scale_status), database_, "绑定尺度状态失败");
    check_sqlite(sqlite3_bind_int(statement, 17, status.scale_valid ? 1 : 0), database_, "绑定尺度有效性失败");
    check_sqlite(sqlite3_bind_double(statement, 18, status.horizontal_scale), database_, "绑定水平尺度失败");
    check_sqlite(sqlite3_bind_double(statement, 19, status.vertical_scale), database_, "绑定垂直尺度失败");
    check_sqlite(sqlite3_bind_double(statement, 20, status.scale_baseline_m), database_, "绑定尺度基线失败");
    check_sqlite(sqlite3_bind_double(statement, 21, status.scale_fit_residual_m), database_, "绑定尺度拟合残差失败");
    check_sqlite(sqlite3_bind_double(statement, 22, status.heading_baseline_m), database_, "绑定航向基线失败");
    bind_text(statement, 23, status.heading_alignment_reason);
    check_sqlite(sqlite3_bind_double(statement, 24, status.distance_from_anchor_m), database_, "绑定锚点距离失败");
    check_sqlite(sqlite3_bind_double(statement, 25, status.dr_duration_s), database_, "绑定DR时间失败");
    check_sqlite(sqlite3_bind_double(statement, 26, status.rtk_age_s), database_, "绑定RTK年龄失败");
    check_sqlite(sqlite3_bind_double(statement, 27, status.odometry_age_s), database_, "绑定里程计年龄失败");
    check_sqlite(sqlite3_bind_double(statement, 28, status.imu_age_s), database_, "绑定IMU年龄失败");
    check_sqlite(sqlite3_bind_double(statement, 29, status.position_difference_to_rtk_m), database_, "绑定恢复误差失败");
    bind_text(statement, 30, status.invalid_reason);
    check_sqlite(sqlite3_step(statement), database_, "写入融合定位status失败");
    sqlite3_finalize(statement);
  }

  void insert_localization_odometry(const nav_msgs::msg::Odometry & odometry)
  {
    sqlite3_stmt * statement = nullptr;
    check_sqlite(
      sqlite3_prepare_v2(
        database_,
        "INSERT INTO localization_odometry_samples ("
        "timestamp_ns, frame_id, child_frame_id, east_m, north_m, up_m, qx, qy, qz, qw) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
        -1, &statement, nullptr),
      database_, "准备融合定位odometry写入失败");
    const auto stamp_ns = rclcpp::Time(odometry.header.stamp).nanoseconds();
    check_sqlite(sqlite3_bind_int64(statement, 1, stamp_ns > 0 ? stamp_ns : system_now_ns()), database_, "绑定融合odom时间失败");
    bind_text(statement, 2, odometry.header.frame_id);
    bind_text(statement, 3, odometry.child_frame_id);
    check_sqlite(sqlite3_bind_double(statement, 4, odometry.pose.pose.position.x), database_, "绑定融合east失败");
    check_sqlite(sqlite3_bind_double(statement, 5, odometry.pose.pose.position.y), database_, "绑定融合north失败");
    check_sqlite(sqlite3_bind_double(statement, 6, odometry.pose.pose.position.z), database_, "绑定融合up失败");
    check_sqlite(sqlite3_bind_double(statement, 7, odometry.pose.pose.orientation.x), database_, "绑定融合qx失败");
    check_sqlite(sqlite3_bind_double(statement, 8, odometry.pose.pose.orientation.y), database_, "绑定融合qy失败");
    check_sqlite(sqlite3_bind_double(statement, 9, odometry.pose.pose.orientation.z), database_, "绑定融合qz失败");
    check_sqlite(sqlite3_bind_double(statement, 10, odometry.pose.pose.orientation.w), database_, "绑定融合qw失败");
    check_sqlite(sqlite3_step(statement), database_, "写入融合定位odometry失败");
    sqlite3_finalize(statement);
  }

  bool latest_fix_is_fresh() const
  {
    if (!latest_fix_.available || latest_fix_.received_monotonic_ns <= 0) {
      return false;
    }
    const auto age_ns = std::max<std::int64_t>(
      0, steady_now_ns() - latest_fix_.received_monotonic_ns);
    return static_cast<double>(age_ns) / 1'000'000.0 <= endpoint_rtk_max_age_ms_;
  }

  std::string capture_endpoint(const std::string & role, std::int64_t requested_ns)
  {
    const bool fresh = latest_fix_is_fresh();
    capture_event_rtk(role, requested_ns, fresh);
    if (!latest_fix_.available || !latest_fix_.valid || !fresh) {
      const std::string reason = !latest_fix_.available ? "no_fix" :
        (!latest_fix_.valid ? "invalid_fix" : "stale_fix");
      insert_event(
        role + "_rtk_unconfirmed", requested_ns,
        role + " RTK坐标未确认：" + reason, reason);
      return "unconfirmed";
    }
    sqlite3_stmt * statement = nullptr;
    check_sqlite(
      sqlite3_prepare_v2(
        database_,
        "INSERT OR REPLACE INTO rtk_endpoints ("
        "role, timestamp_ns, latitude_deg, longitude_deg, altitude_m, fix_type, valid) "
        "VALUES (?, ?, ?, ?, ?, ?, 1)",
        -1, &statement, nullptr),
      database_, "准备RTK端点写入失败");
    bind_text(statement, 1, role);
    check_sqlite(sqlite3_bind_int64(statement, 2, latest_fix_.timestamp_ns), database_, "绑定端点时间失败");
    check_sqlite(sqlite3_bind_double(statement, 3, latest_fix_.latitude_deg), database_, "绑定端点纬度失败");
    check_sqlite(sqlite3_bind_double(statement, 4, latest_fix_.longitude_deg), database_, "绑定端点经度失败");
    bind_nullable_double(statement, 5, latest_fix_.altitude_m);
    bind_text(statement, 6, latest_fix_.fix_type);
    check_sqlite(sqlite3_step(statement), database_, "写入RTK端点失败");
    sqlite3_finalize(statement);
    insert_event(role + "_rtk_captured", requested_ns, role + " RTK坐标已记录", "");
    return "confirmed";
  }

  void capture_event_rtk(
    const std::string & event_type, std::int64_t requested_ns, bool fresh)
  {
    sqlite3_stmt * statement = nullptr;
    check_sqlite(
      sqlite3_prepare_v2(
        database_,
        "INSERT INTO event_rtk_snapshots ("
        "event_type, requested_timestamp_ns, coordinate_timestamp_ns, latitude_deg, "
        "longitude_deg, altitude_m, fix_type, valid) VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
        -1, &statement, nullptr),
      database_, "准备事件RTK写入失败");
    bind_text(statement, 1, event_type);
    check_sqlite(sqlite3_bind_int64(statement, 2, requested_ns), database_, "绑定事件时间失败");
    if (latest_fix_.available) {
      check_sqlite(sqlite3_bind_int64(statement, 3, latest_fix_.timestamp_ns), database_, "绑定坐标时间失败");
      check_sqlite(sqlite3_bind_double(statement, 4, latest_fix_.latitude_deg), database_, "绑定事件纬度失败");
      check_sqlite(sqlite3_bind_double(statement, 5, latest_fix_.longitude_deg), database_, "绑定事件经度失败");
      bind_nullable_double(statement, 6, latest_fix_.altitude_m);
      bind_text(statement, 7, latest_fix_.fix_type);
    } else {
      sqlite3_bind_null(statement, 3);
      sqlite3_bind_null(statement, 4);
      sqlite3_bind_null(statement, 5);
      sqlite3_bind_null(statement, 6);
      sqlite3_bind_null(statement, 7);
    }
    check_sqlite(
      sqlite3_bind_int(statement, 8, latest_fix_.valid && fresh ? 1 : 0),
      database_, "绑定事件RTK有效性失败");
    check_sqlite(sqlite3_step(statement), database_, "写入事件RTK失败");
    sqlite3_finalize(statement);
  }

  void insert_event(
    const std::string & event_type, std::int64_t occurred_at_ns,
    const std::string & message, const std::string & error_code)
  {
    sqlite3_stmt * statement = nullptr;
    check_sqlite(
      sqlite3_prepare_v2(
        database_,
        "INSERT INTO task_events (event_type, occurred_at_ns, message, error_code) "
        "VALUES (?, ?, ?, ?)",
        -1, &statement, nullptr),
      database_, "准备任务事件写入失败");
    bind_text(statement, 1, event_type);
    check_sqlite(sqlite3_bind_int64(statement, 2, occurred_at_ns), database_, "绑定事件时间失败");
    bind_text(statement, 3, message);
    if (error_code.empty()) {
      sqlite3_bind_null(statement, 4);
    } else {
      bind_text(statement, 4, error_code);
    }
    check_sqlite(sqlite3_step(statement), database_, "写入任务事件失败");
    sqlite3_finalize(statement);
  }

  void update_endpoint_status(const std::string & field, const std::string & value)
  {
    if (field != "entry_rtk_status" && field != "exit_rtk_status") {
      throw std::runtime_error("RTK状态字段无效");
    }
    sqlite3_stmt * statement = nullptr;
    const std::string sql = "UPDATE recording_metadata SET " + field + " = ? WHERE id = 1";
    check_sqlite(sqlite3_prepare_v2(database_, sql.c_str(), -1, &statement, nullptr), database_, "准备RTK状态更新失败");
    bind_text(statement, 1, value);
    check_sqlite(sqlite3_step(statement), database_, "更新RTK状态失败");
    sqlite3_finalize(statement);
  }

  void trim_samples_after(std::int64_t requested_ns)
  {
    sqlite3_stmt * statement = nullptr;
    check_sqlite(
      sqlite3_prepare_v2(
        database_,
        "DELETE FROM clearance_samples WHERE recorded_timestamp_ns > ?",
        -1, &statement, nullptr),
      database_, "准备停止边界样本清理失败");
    check_sqlite(
      sqlite3_bind_int64(statement, 1, requested_ns), database_, "绑定停止边界失败");
    check_sqlite(sqlite3_step(statement), database_, "清理停止边界后的样本失败");
    sqlite3_finalize(statement);

    statement = nullptr;
    check_sqlite(
      sqlite3_prepare_v2(
        database_,
        "DELETE FROM clearance_source_frames WHERE received_timestamp_ns > ?",
        -1, &statement, nullptr),
      database_, "准备停止边界源帧清理失败");
    check_sqlite(
      sqlite3_bind_int64(statement, 1, requested_ns), database_, "绑定源帧停止边界失败");
    check_sqlite(sqlite3_step(statement), database_, "清理停止边界后的源帧失败");
    sqlite3_finalize(statement);

    statement = nullptr;
    check_sqlite(
      sqlite3_prepare_v2(
        database_,
        "SELECT COUNT(*), COALESCE(SUM(valid),0) FROM clearance_samples",
        -1, &statement, nullptr),
      database_, "准备停止边界计数刷新失败");
    check_sqlite(sqlite3_step(statement), database_, "刷新停止边界计数失败");
    total_samples_ = static_cast<std::uint64_t>(sqlite3_column_int64(statement, 0));
    valid_samples_ = static_cast<std::uint64_t>(sqlite3_column_int64(statement, 1));
    invalid_samples_ = total_samples_ - valid_samples_;
    sqlite3_finalize(statement);

    statement = nullptr;
    check_sqlite(
      sqlite3_prepare_v2(
        database_,
        "SELECT COUNT(*) FROM clearance_source_frames",
        -1, &statement, nullptr),
      database_, "准备源帧计数刷新失败");
    check_sqlite(sqlite3_step(statement), database_, "刷新源帧计数失败");
    source_frames_ = static_cast<std::uint64_t>(sqlite3_column_int64(statement, 0));
    sqlite3_finalize(statement);
  }

  void update_metadata_completion(std::int64_t ended_ns, bool complete)
  {
    sqlite3_stmt * statement = nullptr;
    check_sqlite(
      sqlite3_prepare_v2(
        database_,
        "UPDATE recording_metadata SET ended_at = ?, complete = ?, exit_rtk_status = ? WHERE id = 1",
        -1, &statement, nullptr),
      database_, "准备记录元数据收尾失败");
    bind_text(statement, 1, iso_utc_from_ns(ended_ns));
    check_sqlite(sqlite3_bind_int(statement, 2, complete ? 1 : 0), database_, "绑定完整性失败");
    bind_text(statement, 3, exit_rtk_status_);
    check_sqlite(sqlite3_step(statement), database_, "更新记录元数据失败");
    sqlite3_finalize(statement);
  }

  void update_counters()
  {
    sqlite3_stmt * statement = nullptr;
    check_sqlite(
      sqlite3_prepare_v2(
        database_,
        "INSERT OR REPLACE INTO recording_counters ("
        "id, total_samples, valid_samples, invalid_samples, source_frames, write_errors) "
        "VALUES (1, ?, ?, ?, ?, ?)",
        -1, &statement, nullptr),
      database_, "准备记录计数写入失败");
    sqlite3_bind_int64(statement, 1, total_samples_);
    sqlite3_bind_int64(statement, 2, valid_samples_);
    sqlite3_bind_int64(statement, 3, invalid_samples_);
    sqlite3_bind_int64(statement, 4, source_frames_);
    sqlite3_bind_int64(statement, 5, write_errors_);
    check_sqlite(sqlite3_step(statement), database_, "写入记录计数失败");
    sqlite3_finalize(statement);
  }

  void close_pause_interval(std::int64_t ended_ns)
  {
    if (!pause_started_ns_.has_value()) {
      return;
    }
    const auto start_elapsed = std::max(
      0.0, static_cast<double>(*pause_started_ns_ - start_requested_ns_) / 1'000'000.0);
    const auto end_elapsed = std::max(
      start_elapsed, static_cast<double>(ended_ns - start_requested_ns_) / 1'000'000.0);
    sqlite3_stmt * statement = nullptr;
    check_sqlite(
      sqlite3_prepare_v2(
        database_,
        "INSERT INTO pause_intervals(started_elapsed_ms, ended_elapsed_ms) VALUES (?, ?)",
        -1, &statement, nullptr),
      database_, "准备暂停区间写入失败");
    sqlite3_bind_double(statement, 1, start_elapsed);
    sqlite3_bind_double(statement, 2, end_elapsed);
    check_sqlite(sqlite3_step(statement), database_, "写入暂停区间失败");
    sqlite3_finalize(statement);
    pause_started_ns_.reset();
  }

  void verify_integrity()
  {
    sqlite3_stmt * statement = nullptr;
    check_sqlite(
      sqlite3_prepare_v2(database_, "PRAGMA integrity_check", -1, &statement, nullptr),
      database_, "准备数据库完整性检查失败");
    const int result = sqlite3_step(statement);
    check_sqlite(result, database_, "执行数据库完整性检查失败");
    const auto * text = reinterpret_cast<const char *>(sqlite3_column_text(statement, 0));
    const std::string value = text != nullptr ? text : "";
    sqlite3_finalize(statement);
    if (value != "ok") {
      throw SqliteError("任务测量数据库完整性检查失败：" + value);
    }
  }

  void begin_transaction()
  {
    if (database_ != nullptr && !transaction_open_) {
      execute(database_, "BEGIN IMMEDIATE");
      transaction_open_ = true;
      pending_transaction_samples_ = 0;
    }
  }

  void flush_transaction()
  {
    if (database_ != nullptr && transaction_open_) {
      execute(database_, "COMMIT");
      transaction_open_ = false;
      pending_transaction_samples_ = 0;
    }
  }

  void handle_runtime_storage_error(const std::string & message)
  {
    ++write_errors_;
    active_ = false;
    paused_ = false;
    try {
      if (database_ != nullptr) {
        if (transaction_open_) {
          execute(database_, "ROLLBACK");
          transaction_open_ = false;
        }
        insert_event("recording_error", system_now_ns(), message, "storage_error");
        update_counters();
      }
    } catch (...) {
    }
    close_database_noexcept();
    publish_status("error", message, "storage_error");
  }

  void populate_command_success(
    interfaces::srv::RecordingCommand::Response & response,
    const std::string & message, const std::string & rtk_status, bool complete)
  {
    response.success = true;
    response.recording_path = relative_recording_path();
    response.rtk_status = rtk_status;
    response.total_samples = total_samples_;
    response.valid_samples = valid_samples_;
    response.invalid_samples = invalid_samples_;
    response.complete = complete;
    response.error_code.clear();
    response.message = message;
  }

  void reject_prepare(
    interfaces::srv::PrepareRecording::Response & response,
    const std::string & code, const std::string & message)
  {
    response.success = false;
    response.recording_path.clear();
    response.entry_rtk_status = "unconfirmed";
    response.error_code = code;
    response.message = message;
  }

  void reject_command(
    interfaces::srv::RecordingCommand::Response & response,
    const std::string & code, const std::string & message)
  {
    response.success = false;
    response.recording_path = relative_recording_path();
    response.rtk_status = "not_requested";
    response.total_samples = total_samples_;
    response.valid_samples = valid_samples_;
    response.invalid_samples = invalid_samples_;
    response.complete = false;
    response.error_code = code;
    response.message = message;
  }

  void publish_status(
    const std::string & state, const std::string & message, const std::string & error_code)
  {
    interfaces::msg::RecordingStatus status;
    status.header.stamp = now();
    status.task_id = task_id_;
    status.state = state;
    status.message = message;
    status.error_code = error_code;
    status.total_samples = total_samples_;
    status.valid_samples = valid_samples_;
    status.invalid_samples = invalid_samples_;
    status.recording_path = relative_recording_path();
    status.entry_rtk_status = entry_rtk_status_;
    status.exit_rtk_status = exit_rtk_status_;
    status_publisher_->publish(status);
  }

  std::string relative_recording_path() const
  {
    return task_id_.empty() ? "" : task_id_ + "/measurements.db";
  }

  void close_database_noexcept()
  {
    if (database_ == nullptr) {
      return;
    }
    try {
      if (transaction_open_) {
        execute(database_, "ROLLBACK");
      }
    } catch (...) {
    }
    sqlite3_close(database_);
    database_ = nullptr;
    transaction_open_ = false;
  }

  void reset_session_state()
  {
    close_database_noexcept();
    active_ = false;
    paused_ = false;
    task_id_.clear();
    task_sequence_ = 0;
    tunnel_code_.clear();
    tunnel_name_.clear();
    travel_direction_.clear();
    lane_side_.clear();
    lane_.clear();
    task_directory_.clear();
    final_database_path_.clear();
    temporary_database_path_.clear();
    start_requested_ns_ = 0;
    pause_started_ns_.reset();
    total_samples_ = 0;
    valid_samples_ = 0;
    invalid_samples_ = 0;
    source_frames_ = 0;
    write_errors_ = 0;
    pending_transaction_samples_ = 0;
    last_written_source_sequence_ = 0;
    last_persisted_source_sequence_ = 0;
    current_repeat_index_ = 0;
    imu_accumulator_ = ImuAccumulator{};
    entry_rtk_status_ = "not_requested";
    exit_rtk_status_ = "not_requested";
  }

  std::mutex mutex_;
  std::string data_root_;
  std::string clearance_topic_;
  std::string rtk_fix_topic_;
  std::string rtk_status_topic_;
  std::string localization_fix_topic_;
  std::string localization_status_topic_;
  std::string localization_odometry_topic_;
  std::string imu_topic_;
  std::string odometry_topic_;
  std::string radar_temperature_topic_;
  double sample_rate_hz_{50.0};
  double source_timeout_ms_{250.0};
  double endpoint_rtk_max_age_ms_{2000.0};
  double odometry_snapshot_max_age_ms_{250.0};
  double radar_temperature_max_age_ms_{2000.0};
  int transaction_batch_size_{100};
  std::string software_version_;
  std::string algorithm_version_;
  std::string config_version_;

  sqlite3 * database_{nullptr};
  bool transaction_open_{false};
  bool active_{false};
  bool paused_{false};
  std::string task_id_;
  std::uint64_t task_sequence_{0};
  std::string tunnel_code_;
  std::string tunnel_name_;
  std::string travel_direction_;
  std::string lane_side_;
  std::string lane_;
  double lidar_mount_height_m_{0.0};
  double clearance_threshold_m_{0.0};
  fs::path task_directory_;
  fs::path final_database_path_;
  fs::path temporary_database_path_;
  std::int64_t start_requested_ns_{0};
  std::optional<std::int64_t> pause_started_ns_;
  std::uint64_t total_samples_{0};
  std::uint64_t valid_samples_{0};
  std::uint64_t invalid_samples_{0};
  std::uint64_t source_frames_{0};
  std::uint64_t write_errors_{0};
  std::uint64_t pending_transaction_samples_{0};
  std::uint64_t source_sequence_{0};
  std::uint64_t last_written_source_sequence_{0};
  std::uint64_t last_persisted_source_sequence_{0};
  std::uint32_t current_repeat_index_{0};
  std::string entry_rtk_status_{"not_requested"};
  std::string exit_rtk_status_{"not_requested"};
  std::optional<LatestClearance> latest_clearance_;
  LatestFix latest_fix_;
  ImuAccumulator imu_accumulator_;
  LatestOdin latest_odin_;
  LatestTemperature latest_temperature_;

  rclcpp::Subscription<interfaces::msg::ClearanceResult>::SharedPtr clearance_subscription_;
  rclcpp::Subscription<sensor_msgs::msg::NavSatFix>::SharedPtr rtk_fix_subscription_;
  rclcpp::Subscription<interfaces::msg::RtkStatus>::SharedPtr rtk_status_subscription_;
  rclcpp::Subscription<sensor_msgs::msg::NavSatFix>::SharedPtr localization_fix_subscription_;
  rclcpp::Subscription<interfaces::msg::LocalizationStatus>::SharedPtr localization_status_subscription_;
  rclcpp::Subscription<nav_msgs::msg::Odometry>::SharedPtr localization_odometry_subscription_;
  rclcpp::Subscription<sensor_msgs::msg::Imu>::SharedPtr imu_subscription_;
  rclcpp::Subscription<nav_msgs::msg::Odometry>::SharedPtr odometry_subscription_;
  rclcpp::Subscription<sensor_msgs::msg::Temperature>::SharedPtr radar_temperature_subscription_;
  rclcpp::Publisher<interfaces::msg::RecordingStatus>::SharedPtr status_publisher_;
  rclcpp::Service<interfaces::srv::PrepareRecording>::SharedPtr prepare_service_;
  rclcpp::Service<interfaces::srv::RecordingCommand>::SharedPtr command_service_;
  rclcpp::TimerBase::SharedPtr sample_timer_;
};

int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);
  rclcpp::spin(std::make_shared<DataRecorderNode>());
  rclcpp::shutdown();
  return 0;
}

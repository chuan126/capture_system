#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cctype>
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
#include "interfaces/msg/recording_status.hpp"
#include "interfaces/msg/rtk_status.hpp"
#include "interfaces/srv/prepare_recording.hpp"
#include "interfaces/srv/recording_command.hpp"
#include "nav_msgs/msg/odometry.hpp"
#include "rclcpp/rclcpp.hpp"
#include "sensor_msgs/msg/imu.hpp"
#include "sensor_msgs/msg/nav_sat_fix.hpp"
#include "sensor_msgs/msg/temperature.hpp"

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
  sqlite3_stmt * statement, int index, bool has_value, std::int64_t value)
{
  if (has_value) {
    check_sqlite(
      sqlite3_bind_int64(statement, index, value), sqlite3_db_handle(statement),
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
    imu_topic_ = declare_parameter<std::string>("imu_topic", "/capture/imu/data");
    odometry_topic_ = declare_parameter<std::string>(
      "odometry_topic", "/capture/odometry/high_rate_raw");
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
      "config_version", "clearance_engine.yaml");

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

  struct LatestRtkStatus
  {
    bool available{false};
    std::int64_t received_monotonic_ns{0};
    std::uint8_t satellite_count{0};
    double hdop{0.0};
    double pdop{0.0};
    double speed_knots{0.0};
    double track_degrees{0.0};
  };

  struct StagedEntrySnapshot
  {
    std::int64_t requested_timestamp_ns{0};
    bool coordinate_available{false};
    std::int64_t coordinate_timestamp_ns{0};
    double latitude_deg{0.0};
    double longitude_deg{0.0};
    std::optional<double> altitude_m;
    std::string fix_type{"UNKNOWN"};
    bool valid{false};
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
    double qx{0.0};
    double qy{0.0};
    double qz{0.0};
    double qw{1.0};
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
      !std::isfinite(request->clearance_upper_limit_m) ||
      !std::isfinite(request->detection_radius_m) ||
      request->lidar_mount_height_m < 0.0 || request->lidar_mount_height_m > 20.0 ||
      request->clearance_threshold_m < 0.0 || request->clearance_threshold_m > 20.0 ||
      request->clearance_upper_limit_m < 0.0 || request->clearance_upper_limit_m > 20.0 ||
      request->clearance_threshold_m > request->clearance_upper_limit_m ||
      request->detection_radius_m < 0.1 || request->detection_radius_m > 5.0 ||
      request->min_support_points < 1U || request->min_support_points > 10000U)
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
      clearance_upper_limit_m_ = request->clearance_upper_limit_m;
      detection_radius_m_ = request->detection_radius_m;
      min_support_points_ = request->min_support_points;
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
      const auto staged_entry = load_staged_entry_snapshot(task_id_);
      if (staged_entry.has_value()) {
        entry_rtk_status_ = staged_entry->valid ? "confirmed" : "unconfirmed";
      }
      execute(database_, "BEGIN IMMEDIATE");
      insert_metadata();
      if (staged_entry.has_value()) {
        import_staged_entry_snapshot(*staged_entry);
      }
      execute(database_, "COMMIT");
      begin_transaction();
      insert_event("recording_started", start_requested_ns_, "正式记录已启动", "");

      active_ = true;
      paused_ = false;
      response->success = true;
      response->recording_path = relative_recording_path();
      response->entry_rtk_status = entry_rtk_status_;
      response->error_code.clear();
      response->message = staged_entry.has_value() ?
        "记录文件已创建，开始前入口RTK快照已载入；出口RTK由操作员手动记录" :
        "记录文件已创建，入口和出口RTK由操作员手动记录";
      if (staged_entry.has_value()) {
        const auto staging_path = staged_entry_path(task_id_);
        for (const auto * suffix : {"", "-wal", "-shm"}) {
          std::error_code remove_error;
          fs::remove(fs::path(staging_path.string() + suffix), remove_error);
          if (remove_error) {
            RCLCPP_WARN(
              get_logger(), "入口RTK暂存文件已导入但清理失败（%s%s）：%s",
              staging_path.c_str(), suffix, remove_error.message().c_str());
          }
        }
      }
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
    const auto requested_ns = request->requested_at_ns > 0 ? request->requested_at_ns : system_now_ns();
    if (!active_ && request->command == "capture_entry_rtk") {
      try {
        capture_prestart_entry(request->task_id, requested_ns, *response);
      } catch (const std::exception & error) {
        reject_command(*response, "storage_error", error.what());
      }
      return;
    }
    if (!active_ && request->command == "capture_exit_rtk") {
      try {
        capture_poststop_exit(request->task_id, requested_ns, *response);
      } catch (const std::exception & error) {
        close_database_noexcept();
        reset_session_state();
        response->success = false;
        response->recording_path = request->task_id + "/measurements.db";
        response->rtk_status = "not_requested";
        response->total_samples = 0;
        response->valid_samples = 0;
        response->invalid_samples = 0;
        response->complete = true;
        response->error_code = "storage_error";
        response->message = error.what();
      }
      return;
    }
    if (!active_ || request->task_id != task_id_) {
      reject_command(*response, "recorder_not_active", "指定任务当前未在记录");
      return;
    }
    try {
      if (request->command == "pause") {
        pause_recording(requested_ns, *response);
      } else if (request->command == "resume") {
        resume_recording(requested_ns, *response);
      } else if (request->command == "capture_entry_rtk") {
        capture_manual_endpoint("entry", requested_ns, *response);
      } else if (request->command == "capture_exit_rtk") {
        capture_manual_endpoint("exit", requested_ns, *response);
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

  static bool safe_task_id(const std::string & task_id)
  {
    return !task_id.empty() && task_id.size() <= 128U &&
      std::all_of(task_id.begin(), task_id.end(), [](unsigned char character) {
        return std::isalnum(character) != 0 || character == '-' || character == '_';
      });
  }

  fs::path staged_entry_path(const std::string & task_id) const
  {
    if (!safe_task_id(task_id)) {
      throw std::runtime_error("任务UUID格式无效");
    }
    return fs::path(data_root_) / "tasks" / task_id / "entry_rtk_snapshot.db";
  }

  void capture_prestart_entry(
    const std::string & task_id, std::int64_t requested_ns,
    interfaces::srv::RecordingCommand::Response & response)
  {
    const bool fresh = latest_fix_is_fresh();
    StagedEntrySnapshot snapshot;
    snapshot.requested_timestamp_ns = requested_ns;
    snapshot.coordinate_available = latest_fix_.available;
    snapshot.coordinate_timestamp_ns = latest_fix_.timestamp_ns;
    snapshot.latitude_deg = latest_fix_.latitude_deg;
    snapshot.longitude_deg = latest_fix_.longitude_deg;
    snapshot.altitude_m = latest_fix_.altitude_m;
    snapshot.fix_type = latest_fix_.fix_type;
    snapshot.valid = latest_fix_.available && latest_fix_.valid && fresh;

    const auto path = staged_entry_path(task_id);
    fs::create_directories(path.parent_path());
    sqlite3 * staging_database = nullptr;
    try {
      check_sqlite(
        sqlite3_open_v2(
          path.c_str(), &staging_database,
          SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr),
        staging_database, "创建入口RTK暂存数据库失败");
      execute(staging_database, "PRAGMA journal_mode=WAL");
      execute(staging_database, "PRAGMA synchronous=FULL");
      execute(staging_database, "PRAGMA busy_timeout=5000");
      execute(
        staging_database,
        "CREATE TABLE IF NOT EXISTS entry_rtk_snapshot ("
        "id INTEGER PRIMARY KEY CHECK(id=1), requested_timestamp_ns INTEGER NOT NULL, "
        "coordinate_timestamp_ns INTEGER, latitude_deg REAL, longitude_deg REAL, "
        "altitude_m REAL, fix_type TEXT, valid INTEGER NOT NULL CHECK(valid IN (0,1)))");
      execute(staging_database, "BEGIN IMMEDIATE");
      sqlite3_stmt * statement = nullptr;
      check_sqlite(
        sqlite3_prepare_v2(
          staging_database,
          "INSERT OR REPLACE INTO entry_rtk_snapshot ("
          "id,requested_timestamp_ns,coordinate_timestamp_ns,latitude_deg,longitude_deg,"
          "altitude_m,fix_type,valid) VALUES (1,?,?,?,?,?,?,?)",
          -1, &statement, nullptr),
        staging_database, "准备入口RTK暂存写入失败");
      check_sqlite(
        sqlite3_bind_int64(statement, 1, snapshot.requested_timestamp_ns),
        staging_database, "绑定入口按钮时间失败");
      if (snapshot.coordinate_available) {
        check_sqlite(
          sqlite3_bind_int64(statement, 2, snapshot.coordinate_timestamp_ns),
          staging_database, "绑定入口坐标时间失败");
        check_sqlite(
          sqlite3_bind_double(statement, 3, snapshot.latitude_deg),
          staging_database, "绑定入口纬度失败");
        check_sqlite(
          sqlite3_bind_double(statement, 4, snapshot.longitude_deg),
          staging_database, "绑定入口经度失败");
        bind_nullable_double(statement, 5, snapshot.altitude_m);
        bind_text(statement, 6, snapshot.fix_type);
      } else {
        for (int index = 2; index <= 6; ++index) {
          check_sqlite(
            sqlite3_bind_null(statement, index), staging_database, "绑定入口空快照失败");
        }
      }
      check_sqlite(
        sqlite3_bind_int(statement, 7, snapshot.valid ? 1 : 0),
        staging_database, "绑定入口快照有效性失败");
      check_sqlite(sqlite3_step(statement), staging_database, "写入入口RTK暂存失败");
      sqlite3_finalize(statement);
      execute(staging_database, "COMMIT");
      execute(staging_database, "PRAGMA wal_checkpoint(TRUNCATE)");
      verify_recovery_integrity(staging_database);
      sqlite3_close(staging_database);
      staging_database = nullptr;
    } catch (...) {
      if (staging_database != nullptr) {
        try {execute(staging_database, "ROLLBACK");} catch (...) {}
        sqlite3_close(staging_database);
      }
      throw;
    }

    const std::string message = snapshot.valid ?
      "入口RTK快照已在开始采集前记录（定位有效）" :
      "入口RTK快照已在开始采集前记录（当前无有效定位）";
    populate_command_success(
      response, message, snapshot.valid ? "confirmed" : "unconfirmed", false);
  }

  void capture_poststop_exit(
    const std::string & task_id, std::int64_t requested_ns,
    interfaces::srv::RecordingCommand::Response & response)
  {
    task_id_ = task_id;
    task_directory_ = fs::path(data_root_) / "tasks" / task_id_;
    final_database_path_ = task_directory_ / "measurements.db";
    if (!safe_task_id(task_id_) || !fs::is_regular_file(final_database_path_)) {
      throw std::runtime_error("任务正式测量文件不存在，无法补录出口RTK");
    }

    // 只以读写方式打开现有正式文件，不携带CREATE标志，避免删除竞态下误建空库。
    check_sqlite(
      sqlite3_open_v2(
        final_database_path_.c_str(), &database_,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, nullptr),
      database_, "打开已完成任务测量数据库失败");
    execute(database_, "PRAGMA journal_mode=WAL");
    execute(database_, "PRAGMA synchronous=FULL");
    execute(database_, "PRAGMA foreign_keys=ON");
    execute(database_, "PRAGMA busy_timeout=5000");
    sqlite3_stmt * metadata = nullptr;
    check_sqlite(
      sqlite3_prepare_v2(
        database_,
        "SELECT task_id,complete,entry_rtk_status,exit_rtk_status "
        "FROM recording_metadata WHERE id=1",
        -1, &metadata, nullptr),
      database_, "准备已完成任务元数据校验失败");
    if (sqlite3_step(metadata) != SQLITE_ROW) {
      sqlite3_finalize(metadata);
      throw std::runtime_error("任务正式测量文件缺少元数据");
    }
    const auto * stored_task_id = reinterpret_cast<const char *>(sqlite3_column_text(metadata, 0));
    const bool complete = sqlite3_column_int(metadata, 1) != 0;
    const auto * entry_status = reinterpret_cast<const char *>(sqlite3_column_text(metadata, 2));
    const auto * exit_status = reinterpret_cast<const char *>(sqlite3_column_text(metadata, 3));
    entry_rtk_status_ = entry_status != nullptr ? entry_status : "not_requested";
    exit_rtk_status_ = exit_status != nullptr ? exit_status : "not_requested";
    const bool task_matches = stored_task_id != nullptr && task_id_ == stored_task_id;
    sqlite3_finalize(metadata);
    if (!task_matches || !complete) {
      throw std::runtime_error("任务测量文件身份不匹配或尚未完成，拒绝补录出口RTK");
    }

    total_samples_ = static_cast<std::uint64_t>(recovery_scalar(
      database_, "SELECT COALESCE(total_samples,0) FROM recording_counters WHERE id=1"));
    valid_samples_ = static_cast<std::uint64_t>(recovery_scalar(
      database_, "SELECT COALESCE(valid_samples,0) FROM recording_counters WHERE id=1"));
    invalid_samples_ = static_cast<std::uint64_t>(recovery_scalar(
      database_, "SELECT COALESCE(invalid_samples,0) FROM recording_counters WHERE id=1"));

    execute(database_, "BEGIN IMMEDIATE");
    transaction_open_ = true;
    const auto previous_status = exit_rtk_status_;
    const auto captured_status = capture_endpoint("exit", requested_ns);
    exit_rtk_status_ = captured_status == "confirmed" || previous_status != "confirmed" ?
      captured_status : previous_status;
    update_endpoint_status("exit_rtk_status", exit_rtk_status_);
    const std::string message = captured_status == "confirmed" ?
      "出口RTK快照已在停止后记录（定位有效）" :
      (previous_status == "confirmed" ?
        "本次停止后出口RTK快照无效，已保留此前有效坐标" :
        "出口RTK快照已在停止后记录（当前无有效定位）");
    insert_event(
      "exit_rtk_poststop_capture", requested_ns, message,
      captured_status == "confirmed" ? "" : "invalid_fix");
    execute(database_, "COMMIT");
    transaction_open_ = false;
    execute(database_, "PRAGMA wal_checkpoint(TRUNCATE)");
    verify_integrity();

    response.success = true;
    response.recording_path = relative_recording_path();
    response.rtk_status = exit_rtk_status_;
    response.total_samples = total_samples_;
    response.valid_samples = valid_samples_;
    response.invalid_samples = invalid_samples_;
    response.complete = true;
    response.error_code.clear();
    response.message = message;
    publish_status("completed", message, "");
    close_database_noexcept();
    reset_session_state();
  }

  std::optional<StagedEntrySnapshot> load_staged_entry_snapshot(const std::string & task_id)
  {
    const auto path = staged_entry_path(task_id);
    if (!fs::exists(path)) {
      return std::nullopt;
    }
    sqlite3 * staging_database = nullptr;
    sqlite3_stmt * statement = nullptr;
    try {
      check_sqlite(
        sqlite3_open_v2(
          path.c_str(), &staging_database,
          SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr),
        staging_database, "打开入口RTK暂存数据库失败");
      execute(staging_database, "PRAGMA busy_timeout=5000");
      check_sqlite(
        sqlite3_prepare_v2(
          staging_database,
          "SELECT requested_timestamp_ns,coordinate_timestamp_ns,latitude_deg,longitude_deg,"
          "altitude_m,fix_type,valid FROM entry_rtk_snapshot WHERE id=1",
          -1, &statement, nullptr),
        staging_database, "准备读取入口RTK暂存失败");
      if (sqlite3_step(statement) != SQLITE_ROW) {
        throw std::runtime_error("入口RTK暂存数据库缺少快照记录");
      }
      StagedEntrySnapshot snapshot;
      snapshot.requested_timestamp_ns = sqlite3_column_int64(statement, 0);
      snapshot.coordinate_available = sqlite3_column_type(statement, 1) != SQLITE_NULL;
      if (snapshot.coordinate_available) {
        snapshot.coordinate_timestamp_ns = sqlite3_column_int64(statement, 1);
        snapshot.latitude_deg = sqlite3_column_double(statement, 2);
        snapshot.longitude_deg = sqlite3_column_double(statement, 3);
        if (sqlite3_column_type(statement, 4) != SQLITE_NULL) {
          snapshot.altitude_m = sqlite3_column_double(statement, 4);
        }
        const auto * fix_type = reinterpret_cast<const char *>(sqlite3_column_text(statement, 5));
        snapshot.fix_type = fix_type != nullptr ? fix_type : "UNKNOWN";
      }
      snapshot.valid = sqlite3_column_int(statement, 6) != 0;
      sqlite3_finalize(statement);
      statement = nullptr;
      sqlite3_close(staging_database);
      return snapshot;
    } catch (...) {
      if (statement != nullptr) {
        sqlite3_finalize(statement);
      }
      if (staging_database != nullptr) {
        sqlite3_close(staging_database);
      }
      throw;
    }
  }

  void import_staged_entry_snapshot(const StagedEntrySnapshot & snapshot)
  {
    sqlite3_stmt * event = nullptr;
    check_sqlite(
      sqlite3_prepare_v2(
        database_,
        "INSERT INTO event_rtk_snapshots (event_type,requested_timestamp_ns,"
        "coordinate_timestamp_ns,latitude_deg,longitude_deg,altitude_m,fix_type,valid) "
        "VALUES ('entry',?,?,?,?,?,?,?)",
        -1, &event, nullptr),
      database_, "准备导入入口RTK快照失败");
    check_sqlite(
      sqlite3_bind_int64(event, 1, snapshot.requested_timestamp_ns),
      database_, "绑定暂存入口按钮时间失败");
    if (snapshot.coordinate_available) {
      check_sqlite(sqlite3_bind_int64(event, 2, snapshot.coordinate_timestamp_ns), database_, "绑定暂存入口坐标时间失败");
      check_sqlite(sqlite3_bind_double(event, 3, snapshot.latitude_deg), database_, "绑定暂存入口纬度失败");
      check_sqlite(sqlite3_bind_double(event, 4, snapshot.longitude_deg), database_, "绑定暂存入口经度失败");
      bind_nullable_double(event, 5, snapshot.altitude_m);
      bind_text(event, 6, snapshot.fix_type);
    } else {
      for (int index = 2; index <= 6; ++index) {
        check_sqlite(sqlite3_bind_null(event, index), database_, "绑定暂存入口空快照失败");
      }
    }
    check_sqlite(sqlite3_bind_int(event, 7, snapshot.valid ? 1 : 0), database_, "绑定暂存入口有效性失败");
    check_sqlite(sqlite3_step(event), database_, "导入入口RTK快照失败");
    sqlite3_finalize(event);

    if (snapshot.valid && snapshot.coordinate_available) {
      sqlite3_stmt * endpoint = nullptr;
      check_sqlite(
        sqlite3_prepare_v2(
          database_,
          "INSERT OR REPLACE INTO rtk_endpoints (role,timestamp_ns,latitude_deg,"
          "longitude_deg,altitude_m,fix_type,valid) VALUES ('entry',?,?,?,?,?,1)",
          -1, &endpoint, nullptr),
        database_, "准备导入有效入口RTK失败");
      check_sqlite(sqlite3_bind_int64(endpoint, 1, snapshot.coordinate_timestamp_ns), database_, "绑定有效入口时间失败");
      check_sqlite(sqlite3_bind_double(endpoint, 2, snapshot.latitude_deg), database_, "绑定有效入口纬度失败");
      check_sqlite(sqlite3_bind_double(endpoint, 3, snapshot.longitude_deg), database_, "绑定有效入口经度失败");
      bind_nullable_double(endpoint, 4, snapshot.altitude_m);
      bind_text(endpoint, 5, snapshot.fix_type);
      check_sqlite(sqlite3_step(endpoint), database_, "导入有效入口RTK失败");
      sqlite3_finalize(endpoint);
    }

    insert_event(
      "entry_rtk_prestart_imported", snapshot.requested_timestamp_ns,
      snapshot.valid ? "开始采集前入口RTK有效快照已导入" : "开始采集前入口RTK无效快照已导入",
      snapshot.valid ? "" : "invalid_fix");
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

  void capture_manual_endpoint(
    const std::string & role,
    std::int64_t requested_ns,
    interfaces::srv::RecordingCommand::Response & response)
  {
    const auto previous_status = role == "entry" ? entry_rtk_status_ : exit_rtk_status_;
    const auto captured_status = capture_endpoint(role, requested_ns);
    // 失败的重录不能覆盖此前已确认坐标；每次尝试仍由event_rtk_snapshots留痕。
    const auto effective_status =
      captured_status == "confirmed" || previous_status != "confirmed" ?
      captured_status : previous_status;
    if (role == "entry") {
      entry_rtk_status_ = effective_status;
      update_endpoint_status("entry_rtk_status", entry_rtk_status_);
    } else {
      exit_rtk_status_ = effective_status;
      update_endpoint_status("exit_rtk_status", exit_rtk_status_);
    }
    const bool captured = captured_status == "confirmed";
    const std::string endpoint_name = role == "entry" ? "入口" : "出口";
    const std::string message = captured ?
      endpoint_name + "RTK快照已记录（定位有效）" :
      (previous_status == "confirmed" ?
        "本次RTK快照已留痕但定位无效，已保留此前有效的" + endpoint_name + "RTK" :
        endpoint_name + "RTK快照已记录（当前无有效定位）");
    populate_command_success(response, message, effective_status, false);
    publish_status(paused_ ? "paused" : "recording", message, "");
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
    response.message = "任务文件已完成";
    publish_status(complete ? "completed" : "interrupted", response.message, "");
    reset_session_state();
  }

  void on_clearance(const interfaces::msg::ClearanceResult::SharedPtr message)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    LatestClearance latest;
    latest.source_timestamp_ns = rclcpp::Time(message->header.stamp).nanoseconds();
    if (latest.source_timestamp_ns <= 0) {
      latest.source_timestamp_ns = system_now_ns();
    }
    if (latest.source_timestamp_ns == last_received_clearance_timestamp_ns_) {
      RCLCPP_WARN_THROTTLE(
        get_logger(), *get_clock(), 5000,
        "忽略源时间戳完全相同的重复净空帧：%ld",
        static_cast<long>(latest.source_timestamp_ns));
      return;
    }
    last_received_clearance_timestamp_ns_ = latest.source_timestamp_ns;
    latest.sequence = ++source_sequence_;
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

    if (active_ && !paused_ && latest.valid && latest.lidar_to_top_m.has_value()) {
      const double clearance_height = *latest.lidar_to_top_m + lidar_mount_height_m_;
      if (!minimum_clearance_height_m_.has_value() ||
        clearance_height < *minimum_clearance_height_m_)
      {
        minimum_clearance_height_m_ = clearance_height;
      }
    }

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
    latest_rtk_status_.available = std::isfinite(message->hdop) &&
      std::isfinite(message->pdop) && std::isfinite(message->speed_knots) &&
      std::isfinite(message->track_degrees);
    if (latest_rtk_status_.available) {
      latest_rtk_status_.received_monotonic_ns = steady_now_ns();
      latest_rtk_status_.satellite_count = message->satellite_count;
      latest_rtk_status_.hdop = message->hdop;
      latest_rtk_status_.pdop = message->pdop;
      latest_rtk_status_.speed_knots = message->speed_knots;
      latest_rtk_status_.track_degrees = message->track_degrees;
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
    const bool imu_values_finite =
      std::isfinite(gyro.x) && std::isfinite(gyro.y) && std::isfinite(gyro.z) &&
      std::isfinite(accel.x) && std::isfinite(accel.y) && std::isfinite(accel.z);
    if (imu_values_finite) {
      ++imu_accumulator_.count;
      imu_accumulator_.gyro_x_sum += gyro.x;
      imu_accumulator_.gyro_y_sum += gyro.y;
      imu_accumulator_.gyro_z_sum += gyro.z;
      imu_accumulator_.accel_x_sum += accel.x;
      imu_accumulator_.accel_y_sum += accel.y;
      imu_accumulator_.accel_z_sum += accel.z;
    }
    // 原始数据保存以收到的IMU消息为采样基准；非有限分量只在该行落0，不丢整行。
    try {
      insert_imu_sample(*message);
    } catch (const std::exception & error) {
      handle_runtime_storage_error(error.what());
    }
  }

  void insert_imu_sample(const sensor_msgs::msg::Imu & message)
  {
    if (database_ == nullptr) {
      return;
    }
    const auto recorded_ns = system_now_ns();
    auto imu_timestamp_ns = rclcpp::Time(message.header.stamp).nanoseconds();
    if (imu_timestamp_ns <= 0) {
      imu_timestamp_ns = recorded_ns;
    }
    const auto elapsed_ms = std::max(
      0.0, static_cast<double>(recorded_ns - start_requested_ns_) / 1'000'000.0);
    const auto monotonic_now_ns = steady_now_ns();
    const bool odin_fresh = latest_odin_.available &&
      static_cast<double>(std::max<std::int64_t>(
        0, monotonic_now_ns - latest_odin_.received_monotonic_ns)) / 1'000'000.0 <=
      odometry_snapshot_max_age_ms_;
    const bool rtk_fix_fresh = latest_fix_.available &&
      static_cast<double>(std::max<std::int64_t>(
        0, monotonic_now_ns - latest_fix_.received_monotonic_ns)) / 1'000'000.0 <=
      endpoint_rtk_max_age_ms_;
    const bool rtk_status_fresh = latest_rtk_status_.available &&
      static_cast<double>(std::max<std::int64_t>(
        0, monotonic_now_ns - latest_rtk_status_.received_monotonic_ns)) / 1'000'000.0 <=
      endpoint_rtk_max_age_ms_;
    const bool temperature_fresh = latest_temperature_.available &&
      static_cast<double>(std::max<std::int64_t>(
        0, monotonic_now_ns - latest_temperature_.received_monotonic_ns)) / 1'000'000.0 <=
      radar_temperature_max_age_ms_;
    const auto finite_or_zero = [](double value) {
        return std::isfinite(value) ? value : 0.0;
      };

    std::optional<double> clearance_height;
    std::optional<double> minimum_point_x;
    std::optional<double> minimum_point_y;
    std::optional<double> minimum_point_z;
    if (latest_clearance_.has_value()) {
      const auto & source = *latest_clearance_;
      const double source_age_ms = std::max(
        0.0, static_cast<double>(recorded_ns - source.received_timestamp_ns) / 1'000'000.0);
      if (source_age_ms <= source_timeout_ms_ && source.valid &&
        source.lidar_to_top_m.has_value())
      {
        clearance_height = *source.lidar_to_top_m + lidar_mount_height_m_;
        if (!minimum_clearance_height_m_.has_value() ||
          *clearance_height < *minimum_clearance_height_m_)
        {
          minimum_clearance_height_m_ = clearance_height;
        }
        const auto & minimum = source.message;
        if (std::isfinite(minimum.minimum_point_x_m)) {
          minimum_point_x = minimum.minimum_point_x_m;
        }
        if (std::isfinite(minimum.minimum_point_y_m)) {
          minimum_point_y = minimum.minimum_point_y_m;
        }
        if (std::isfinite(minimum.minimum_point_z_m)) {
          minimum_point_z = minimum.minimum_point_z_m;
        }
      }
    }

    sqlite3_stmt * statement = nullptr;
    check_sqlite(
      sqlite3_prepare_v2(
        database_,
        "INSERT INTO imu_samples ("
        "sample_index, imu_timestamp_ns, recorded_timestamp_ns, elapsed_ms, "
        "clearance_height_m, minimum_clearance_height_m, rtk_timestamp_ns, "
        "rtk_latitude_deg, rtk_longitude_deg, rtk_altitude_m, rtk_valid, "
        "rtk_satellite_count, rtk_hdop, rtk_pdop, rtk_speed_knots, rtk_track_degrees, "
        "gyro_x_rad_s, gyro_y_rad_s, gyro_z_rad_s, accel_x_m_s2, accel_y_m_s2, "
        "accel_z_m_s2, radar_temperature_c, minimum_point_x_m, minimum_point_y_m, "
        "minimum_point_z_m, vehicle_pitch_deg, vehicle_roll_deg, vehicle_heading_deg, "
        "odin_position_x_m, odin_position_y_m, odin_position_z_m, odin_qx, odin_qy, "
        "odin_qz, odin_qw) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, "
        "?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
        -1, &statement, nullptr),
      database_, "准备IMU原始样本写入失败");
    check_sqlite(
      sqlite3_bind_int64(statement, 1, static_cast<sqlite3_int64>(imu_sample_index_++)),
      database_, "绑定IMU样本序号失败");
    check_sqlite(sqlite3_bind_int64(statement, 2, imu_timestamp_ns), database_, "绑定IMU时间失败");
    check_sqlite(sqlite3_bind_int64(statement, 3, recorded_ns), database_, "绑定IMU接收时间失败");
    check_sqlite(sqlite3_bind_double(statement, 4, elapsed_ms), database_, "绑定IMU相对时间失败");
    bind_nullable_double(statement, 5, clearance_height);
    bind_nullable_double(statement, 6, minimum_clearance_height_m_);
    bind_nullable_int64(statement, 7, rtk_fix_fresh, latest_fix_.timestamp_ns);
    bind_nullable_double(statement, 8, rtk_fix_fresh ?
      std::optional<double>(latest_fix_.latitude_deg) : std::nullopt);
    bind_nullable_double(statement, 9, rtk_fix_fresh ?
      std::optional<double>(latest_fix_.longitude_deg) : std::nullopt);
    bind_nullable_double(statement, 10, rtk_fix_fresh ? latest_fix_.altitude_m : std::nullopt);
    if (rtk_fix_fresh) {
      check_sqlite(
        sqlite3_bind_int(statement, 11, latest_fix_.valid ? 1 : 0),
        database_, "绑定IMU样本RTK有效性失败");
    } else {
      sqlite3_bind_null(statement, 11);
    }
    if (rtk_status_fresh) {
      check_sqlite(
        sqlite3_bind_int(statement, 12, latest_rtk_status_.satellite_count),
        database_, "绑定IMU样本RTK卫星数失败");
    } else {
      sqlite3_bind_null(statement, 12);
    }
    bind_nullable_double(statement, 13, rtk_status_fresh ?
      std::optional<double>(latest_rtk_status_.hdop) : std::nullopt);
    bind_nullable_double(statement, 14, rtk_status_fresh ?
      std::optional<double>(latest_rtk_status_.pdop) : std::nullopt);
    bind_nullable_double(statement, 15, rtk_status_fresh ?
      std::optional<double>(latest_rtk_status_.speed_knots) : std::nullopt);
    bind_nullable_double(statement, 16, rtk_status_fresh ?
      std::optional<double>(latest_rtk_status_.track_degrees) : std::nullopt);
    check_sqlite(
      sqlite3_bind_double(statement, 17, finite_or_zero(message.angular_velocity.x)),
      database_, "绑定陀螺X失败");
    check_sqlite(
      sqlite3_bind_double(statement, 18, finite_or_zero(message.angular_velocity.y)),
      database_, "绑定陀螺Y失败");
    check_sqlite(
      sqlite3_bind_double(statement, 19, finite_or_zero(message.angular_velocity.z)),
      database_, "绑定陀螺Z失败");
    check_sqlite(
      sqlite3_bind_double(statement, 20, finite_or_zero(message.linear_acceleration.x)),
      database_, "绑定加速度X失败");
    check_sqlite(
      sqlite3_bind_double(statement, 21, finite_or_zero(message.linear_acceleration.y)),
      database_, "绑定加速度Y失败");
    check_sqlite(
      sqlite3_bind_double(statement, 22, finite_or_zero(message.linear_acceleration.z)),
      database_, "绑定加速度Z失败");
    bind_nullable_double(statement, 23, temperature_fresh ?
      std::optional<double>(latest_temperature_.celsius) : std::nullopt);
    bind_nullable_double(statement, 24, minimum_point_x);
    bind_nullable_double(statement, 25, minimum_point_y);
    bind_nullable_double(statement, 26, minimum_point_z);
    // 融合定位已经移除，姿态三列保留为兼容占位并在TXT中导出为0。
    bind_nullable_double(statement, 27, std::nullopt);
    bind_nullable_double(statement, 28, std::nullopt);
    bind_nullable_double(statement, 29, std::nullopt);
    bind_nullable_double(statement, 30, odin_fresh ?
      std::optional<double>(latest_odin_.position_x_m) : std::nullopt);
    bind_nullable_double(statement, 31, odin_fresh ?
      std::optional<double>(latest_odin_.position_y_m) : std::nullopt);
    bind_nullable_double(statement, 32, odin_fresh ?
      std::optional<double>(latest_odin_.position_z_m) : std::nullopt);
    bind_nullable_double(statement, 33, odin_fresh ? std::optional<double>(latest_odin_.qx) : std::nullopt);
    bind_nullable_double(statement, 34, odin_fresh ? std::optional<double>(latest_odin_.qy) : std::nullopt);
    bind_nullable_double(statement, 35, odin_fresh ? std::optional<double>(latest_odin_.qz) : std::nullopt);
    bind_nullable_double(statement, 36, odin_fresh ? std::optional<double>(latest_odin_.qw) : std::nullopt);
    check_sqlite(sqlite3_step(statement), database_, "写入IMU原始样本失败");
    sqlite3_finalize(statement);

    ++pending_transaction_samples_;
    if (pending_transaction_samples_ >= static_cast<std::uint64_t>(transaction_batch_size_)) {
      flush_transaction();
      begin_transaction();
    }
  }

  void on_odometry(const nav_msgs::msg::Odometry::SharedPtr message)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto & position = message->pose.pose.position;
    const auto & orientation = message->pose.pose.orientation;
    if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z))
    {
      return;
    }
    if (!std::isfinite(orientation.x) || !std::isfinite(orientation.y) ||
      !std::isfinite(orientation.z) || !std::isfinite(orientation.w))
    {
      return;
    }
    latest_odin_.available = true;
    latest_odin_.received_monotonic_ns = steady_now_ns();
    latest_odin_.position_x_m = position.x;
    latest_odin_.position_y_m = position.y;
    latest_odin_.position_z_m = position.z;
    latest_odin_.qx = orientation.x;
    latest_odin_.qy = orientation.y;
    latest_odin_.qz = orientation.z;
    latest_odin_.qw = orientation.w;
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
        if (std::isfinite(minimum.minimum_point_x_m)) {
          minimum_point_x = minimum.minimum_point_x_m;
        }
        if (std::isfinite(minimum.minimum_point_y_m)) {
          minimum_point_y = minimum.minimum_point_y_m;
        }
        if (std::isfinite(minimum.minimum_point_z_m)) {
          minimum_point_z = minimum.minimum_point_z_m;
        }
      }

      const auto monotonic_now_ns = steady_now_ns();
      const bool odin_fresh = latest_odin_.available &&
        static_cast<double>(std::max<std::int64_t>(
          0, monotonic_now_ns - latest_odin_.received_monotonic_ns)) / 1'000'000.0 <=
        odometry_snapshot_max_age_ms_;
      const bool rtk_fix_fresh = latest_fix_.available &&
        static_cast<double>(std::max<std::int64_t>(
          0, monotonic_now_ns - latest_fix_.received_monotonic_ns)) / 1'000'000.0 <=
        endpoint_rtk_max_age_ms_;
      const bool rtk_status_fresh = latest_rtk_status_.available &&
        static_cast<double>(std::max<std::int64_t>(
          0, monotonic_now_ns - latest_rtk_status_.received_monotonic_ns)) / 1'000'000.0 <=
        endpoint_rtk_max_age_ms_;
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
           "rtk_latitude_deg, rtk_longitude_deg, rtk_altitude_m, rtk_fix_type, rtk_valid, "
           "rtk_satellite_count, rtk_hdop, rtk_pdop, rtk_speed_knots, rtk_track_degrees, "
           "gyro_x_rad_s, gyro_y_rad_s, gyro_z_rad_s, accel_x_m_s2, accel_y_m_s2, "
           "accel_z_m_s2, imu_sample_count, radar_temperature_c, minimum_point_x_m, "
           "minimum_point_y_m, minimum_point_z_m, vehicle_pitch_deg, vehicle_roll_deg, "
           "vehicle_heading_deg, odin_position_x_m, odin_position_y_m, odin_position_z_m, "
           "odin_qx, odin_qy, odin_qz, odin_qw) "
           "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, "
           "?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
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
      bind_nullable_int64(
        statement, 14, rtk_fix_fresh, latest_fix_.timestamp_ns);
      bind_nullable_double(statement, 15, rtk_fix_fresh ?
        std::optional<double>(latest_fix_.latitude_deg) : std::nullopt);
      bind_nullable_double(statement, 16, rtk_fix_fresh ?
        std::optional<double>(latest_fix_.longitude_deg) : std::nullopt);
      bind_nullable_double(statement, 17, rtk_fix_fresh ? latest_fix_.altitude_m : std::nullopt);
      bind_nullable_text(statement, 18, rtk_fix_fresh ?
        std::optional<std::string>(latest_fix_.fix_type) : std::nullopt);
      if (rtk_fix_fresh) {
        check_sqlite(sqlite3_bind_int(statement, 19, latest_fix_.valid ? 1 : 0), database_, "绑定RTK有效性失败");
      } else {
        sqlite3_bind_null(statement, 19);
      }
      if (rtk_status_fresh) {
        check_sqlite(sqlite3_bind_int(statement, 20, latest_rtk_status_.satellite_count), database_, "绑定RTK卫星数失败");
      } else {
        sqlite3_bind_null(statement, 20);
      }
      bind_nullable_double(statement, 21, rtk_status_fresh ?
        std::optional<double>(latest_rtk_status_.hdop) : std::nullopt);
      bind_nullable_double(statement, 22, rtk_status_fresh ?
        std::optional<double>(latest_rtk_status_.pdop) : std::nullopt);
      bind_nullable_double(statement, 23, rtk_status_fresh ?
        std::optional<double>(latest_rtk_status_.speed_knots) : std::nullopt);
      bind_nullable_double(statement, 24, rtk_status_fresh ?
        std::optional<double>(latest_rtk_status_.track_degrees) : std::nullopt);
      bind_nullable_double(statement, 25, gyro_x);
      bind_nullable_double(statement, 26, gyro_y);
      bind_nullable_double(statement, 27, gyro_z);
      bind_nullable_double(statement, 28, accel_x);
      bind_nullable_double(statement, 29, accel_y);
      bind_nullable_double(statement, 30, accel_z);
      check_sqlite(
        sqlite3_bind_int64(statement, 31, static_cast<sqlite3_int64>(imu_sample_count)),
        database_, "绑定IMU平均样本数失败");
      bind_nullable_double(
        statement, 32, temperature_fresh ?
        std::optional<double>(latest_temperature_.celsius) : std::nullopt);
      bind_nullable_double(statement, 33, minimum_point_x);
      bind_nullable_double(statement, 34, minimum_point_y);
      bind_nullable_double(statement, 35, minimum_point_z);
      // 融合定位已退出正式系统；保留历史列并明确写NULL，避免伪造车辆姿态和航向。
      bind_nullable_double(statement, 36, std::nullopt);
      bind_nullable_double(statement, 37, std::nullopt);
      bind_nullable_double(statement, 38, std::nullopt);
      bind_nullable_double(
        statement, 39, odin_fresh ?
        std::optional<double>(latest_odin_.position_x_m) : std::nullopt);
      bind_nullable_double(
        statement, 40, odin_fresh ?
        std::optional<double>(latest_odin_.position_y_m) : std::nullopt);
      bind_nullable_double(
        statement, 41, odin_fresh ?
        std::optional<double>(latest_odin_.position_z_m) : std::nullopt);
      bind_nullable_double(statement, 42, odin_fresh ? std::optional<double>(latest_odin_.qx) : std::nullopt);
      bind_nullable_double(statement, 43, odin_fresh ? std::optional<double>(latest_odin_.qy) : std::nullopt);
      bind_nullable_double(statement, 44, odin_fresh ? std::optional<double>(latest_odin_.qz) : std::nullopt);
      bind_nullable_double(statement, 45, odin_fresh ? std::optional<double>(latest_odin_.qw) : std::nullopt);
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
        clearance_upper_limit_m REAL,
        detection_radius_m REAL,
        min_support_points INTEGER,
        entry_rtk_status TEXT NOT NULL DEFAULT 'not_requested',
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
        rtk_latitude_deg REAL,
        rtk_longitude_deg REAL,
        rtk_altitude_m REAL,
        rtk_fix_type TEXT,
        rtk_valid INTEGER CHECK (rtk_valid IN (0, 1)),
        rtk_satellite_count INTEGER,
        rtk_hdop REAL,
        rtk_pdop REAL,
        rtk_speed_knots REAL,
        rtk_track_degrees REAL,
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
        vehicle_pitch_deg REAL,
        vehicle_roll_deg REAL,
        vehicle_heading_deg REAL,
        odin_position_x_m REAL,
        odin_position_y_m REAL,
        odin_position_z_m REAL,
        odin_qx REAL,
        odin_qy REAL,
        odin_qz REAL,
        odin_qw REAL
      );
      CREATE INDEX clearance_samples_timestamp_idx ON clearance_samples(source_timestamp_ns);
      CREATE INDEX clearance_samples_recorded_timestamp_idx ON clearance_samples(recorded_timestamp_ns);
      CREATE TABLE imu_samples (
        sample_index INTEGER PRIMARY KEY CHECK (sample_index >= 0),
        imu_timestamp_ns INTEGER NOT NULL,
        recorded_timestamp_ns INTEGER NOT NULL,
        elapsed_ms REAL NOT NULL CHECK (elapsed_ms >= 0),
        clearance_height_m REAL,
        minimum_clearance_height_m REAL,
        rtk_timestamp_ns INTEGER,
        rtk_latitude_deg REAL,
        rtk_longitude_deg REAL,
        rtk_altitude_m REAL,
        rtk_valid INTEGER CHECK (rtk_valid IN (0, 1)),
        rtk_satellite_count INTEGER,
        rtk_hdop REAL,
        rtk_pdop REAL,
        rtk_speed_knots REAL,
        rtk_track_degrees REAL,
        gyro_x_rad_s REAL NOT NULL,
        gyro_y_rad_s REAL NOT NULL,
        gyro_z_rad_s REAL NOT NULL,
        accel_x_m_s2 REAL NOT NULL,
        accel_y_m_s2 REAL NOT NULL,
        accel_z_m_s2 REAL NOT NULL,
        radar_temperature_c REAL,
        minimum_point_x_m REAL,
        minimum_point_y_m REAL,
        minimum_point_z_m REAL,
        vehicle_pitch_deg REAL,
        vehicle_roll_deg REAL,
        vehicle_heading_deg REAL,
        odin_position_x_m REAL,
        odin_position_y_m REAL,
        odin_position_z_m REAL,
        odin_qx REAL,
        odin_qy REAL,
        odin_qz REAL,
        odin_qw REAL
      );
      CREATE INDEX imu_samples_recorded_timestamp_idx ON imu_samples(recorded_timestamp_ns);
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
        vehicle_attitude_valid INTEGER NOT NULL CHECK (vehicle_attitude_valid IN (0, 1)),
        vehicle_pitch_deg REAL NOT NULL,
        vehicle_roll_deg REAL NOT NULL,
        vehicle_heading_deg REAL NOT NULL,
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
        "clearance_upper_limit_m, detection_radius_m, min_support_points, "
        "entry_rtk_status, exit_rtk_status) "
        "VALUES (1, 14, ?, 'recorded', ?, ?, ?, ?, NULL, 0, ?, ?, ?, ?, ?, ?, ?, ?, ?, "
        "?, 'not_requested')",
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
    check_sqlite(sqlite3_bind_double(statement, 11, clearance_threshold_m_), database_, "绑定高度下限阈值失败");
    check_sqlite(sqlite3_bind_double(statement, 12, clearance_upper_limit_m_), database_, "绑定高度上限阈值失败");
    check_sqlite(sqlite3_bind_double(statement, 13, detection_radius_m_), database_, "绑定检测半径失败");
    check_sqlite(sqlite3_bind_int64(statement, 14, static_cast<sqlite3_int64>(min_support_points_)), database_, "绑定最低支持点数失败");
    bind_text(statement, 15, entry_rtk_status_);
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
        "DELETE FROM imu_samples WHERE recorded_timestamp_ns > ?",
        -1, &statement, nullptr),
      database_, "准备停止边界IMU样本清理失败");
    check_sqlite(
      sqlite3_bind_int64(statement, 1, requested_ns), database_, "绑定IMU停止边界失败");
    check_sqlite(sqlite3_step(statement), database_, "清理停止边界后的IMU样本失败");
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
    imu_sample_index_ = 0;
    minimum_clearance_height_m_.reset();
    imu_accumulator_ = ImuAccumulator{};
    entry_rtk_status_ = "not_requested";
    exit_rtk_status_ = "not_requested";
  }

  std::mutex mutex_;
  std::string data_root_;
  std::string clearance_topic_;
  std::string rtk_fix_topic_;
  std::string rtk_status_topic_;
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
  double clearance_upper_limit_m_{20.0};
  double detection_radius_m_{1.0};
  std::uint32_t min_support_points_{5U};
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
  std::uint64_t imu_sample_index_{0};
  std::int64_t last_received_clearance_timestamp_ns_{0};
  std::string entry_rtk_status_{"not_requested"};
  std::string exit_rtk_status_{"not_requested"};
  std::optional<LatestClearance> latest_clearance_;
  std::optional<double> minimum_clearance_height_m_;
  LatestFix latest_fix_;
  LatestRtkStatus latest_rtk_status_;
  ImuAccumulator imu_accumulator_;
  LatestOdin latest_odin_;
  LatestTemperature latest_temperature_;

  rclcpp::Subscription<interfaces::msg::ClearanceResult>::SharedPtr clearance_subscription_;
  rclcpp::Subscription<sensor_msgs::msg::NavSatFix>::SharedPtr rtk_fix_subscription_;
  rclcpp::Subscription<interfaces::msg::RtkStatus>::SharedPtr rtk_status_subscription_;
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

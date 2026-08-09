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
#include "interfaces/msg/recording_status.hpp"
#include "interfaces/msg/rtk_status.hpp"
#include "interfaces/srv/prepare_recording.hpp"
#include "interfaces/srv/recording_command.hpp"
#include "rclcpp/rclcpp.hpp"
#include "sensor_msgs/msg/nav_sat_fix.hpp"

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
    sample_rate_hz_ = declare_parameter<double>("sample_rate_hz", 50.0);
    source_timeout_ms_ = declare_parameter<double>("source_timeout_ms", 250.0);
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
    double latitude_deg{0.0};
    double longitude_deg{0.0};
    std::optional<double> altitude_m;
    std::string fix_type{"UNKNOWN"};
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
    if (request->task_id.empty() || (request->lane != "left" && request->lane != "right") ||
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
      lane_ = request->lane;
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
    pause_started_ns_ = requested_ns;
    insert_event("paused", requested_ns, "正式测量记录已暂停", "");
    capture_event_rtk("pause", requested_ns);
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
    begin_transaction();
    insert_event("resumed", requested_ns, "正式测量记录已继续", "");
    capture_event_rtk("resume", requested_ns);
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

      sqlite3_stmt * statement = nullptr;
      check_sqlite(
        sqlite3_prepare_v2(
          database_,
          "INSERT INTO clearance_samples ("
          "sample_index, source_timestamp_ns, recorded_timestamp_ns, elapsed_ms, "
          "lidar_to_top_m, clearance_height_m, valid, invalid_reason, quality_score, "
          "source_sequence, source_age_ms, is_repeated, repeat_index) "
          "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
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
        repeat_index INTEGER NOT NULL DEFAULT 0
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
        candidate_plane_count INTEGER,
        selected_inlier_count INTEGER,
        selected_area_m2 REAL,
        selected_tilt_deg REAL,
        selected_rms_m REAL,
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
        "id, schema_version, task_id, data_origin, lane, started_at, ended_at, complete, "
        "nominal_sample_rate_hz, algorithm_version, config_version, software_version, "
        "lidar_mount_height_m, clearance_threshold_m, entry_rtk_status, exit_rtk_status) "
        "VALUES (1, 3, ?, 'recorded', ?, ?, NULL, 0, ?, ?, ?, ?, ?, ?, 'pending', 'not_requested')",
        -1, &statement, nullptr),
      database_, "准备任务元数据写入失败");
    bind_text(statement, 1, task_id_);
    bind_text(statement, 2, lane_);
    bind_text(statement, 3, iso_utc_from_ns(start_requested_ns_));
    check_sqlite(sqlite3_bind_double(statement, 4, sample_rate_hz_), database_, "绑定采样频率失败");
    bind_text(statement, 5, algorithm_version_);
    bind_text(statement, 6, config_version_);
    bind_text(statement, 7, software_version_);
    check_sqlite(sqlite3_bind_double(statement, 8, lidar_mount_height_m_), database_, "绑定安装高度失败");
    check_sqlite(sqlite3_bind_double(statement, 9, clearance_threshold_m_), database_, "绑定高度阈值失败");
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
        "invalid_reason, quality_score, candidate_plane_count, selected_inlier_count, "
        "selected_area_m2, selected_tilt_deg, selected_rms_m, processing_time_ms) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
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
    check_sqlite(sqlite3_bind_int(statement, 8, static_cast<int>(source.message.candidate_count)), database_, "绑定候选面数量失败");
    check_sqlite(sqlite3_bind_int(statement, 9, static_cast<int>(source.message.selected_inlier_count)), database_, "绑定内点数量失败");
    bind_nullable_double(statement, 10, std::isfinite(source.message.selected_area_m2) ? std::optional<double>(source.message.selected_area_m2) : std::nullopt);
    bind_nullable_double(statement, 11, std::isfinite(source.message.selected_tilt_deg) ? std::optional<double>(source.message.selected_tilt_deg) : std::nullopt);
    bind_nullable_double(statement, 12, std::isfinite(source.message.residual_median_m) ? std::optional<double>(source.message.residual_median_m) : std::nullopt);
    bind_nullable_double(statement, 13, std::isfinite(source.message.processing_time_ms) ? std::optional<double>(source.message.processing_time_ms) : std::nullopt);
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

  std::string capture_endpoint(const std::string & role, std::int64_t requested_ns)
  {
    capture_event_rtk(role, requested_ns);
    if (!latest_fix_.available || !latest_fix_.valid) {
      insert_event(role + "_rtk_unconfirmed", requested_ns, role + " RTK坐标未确认", "");
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

  void capture_event_rtk(const std::string & event_type, std::int64_t requested_ns)
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
    check_sqlite(sqlite3_bind_int(statement, 8, latest_fix_.valid ? 1 : 0), database_, "绑定事件RTK有效性失败");
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
    entry_rtk_status_ = "not_requested";
    exit_rtk_status_ = "not_requested";
  }

  std::mutex mutex_;
  std::string data_root_;
  std::string clearance_topic_;
  std::string rtk_fix_topic_;
  std::string rtk_status_topic_;
  double sample_rate_hz_{50.0};
  double source_timeout_ms_{250.0};
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

  rclcpp::Subscription<interfaces::msg::ClearanceResult>::SharedPtr clearance_subscription_;
  rclcpp::Subscription<sensor_msgs::msg::NavSatFix>::SharedPtr rtk_fix_subscription_;
  rclcpp::Subscription<interfaces::msg::RtkStatus>::SharedPtr rtk_status_subscription_;
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

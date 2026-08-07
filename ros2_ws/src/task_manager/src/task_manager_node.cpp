#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <future>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "interfaces/msg/recording_status.hpp"
#include "interfaces/msg/task_status.hpp"
#include "interfaces/srv/prepare_recording.hpp"
#include "interfaces/srv/recording_command.hpp"
#include "interfaces/srv/start_task.hpp"
#include "interfaces/srv/task_command.hpp"
#include "rclcpp/rclcpp.hpp"

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

void bind_text(sqlite3_stmt * statement, int index, const std::string & value)
{
  check_sqlite(
    sqlite3_bind_text(statement, index, value.c_str(), -1, SQLITE_TRANSIENT),
    sqlite3_db_handle(statement), "绑定文本参数失败");
}

void bind_optional_text(
  sqlite3_stmt * statement, int index, const std::optional<std::string> & value)
{
  if (value.has_value()) {
    bind_text(statement, index, *value);
  } else {
    check_sqlite(sqlite3_bind_null(statement, index), sqlite3_db_handle(statement), "绑定空值失败");
  }
}

std::int64_t system_now_ns()
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string iso_utc_from_ns(std::int64_t timestamp_ns)
{
  const std::time_t seconds = static_cast<std::time_t>(timestamp_ns / 1'000'000'000LL);
  const auto nanoseconds = timestamp_ns % 1'000'000'000LL;
  std::tm utc{};
  gmtime_r(&seconds, &utc);
  std::ostringstream stream;
  stream << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.'
         << std::setw(9) << std::setfill('0') << nanoseconds << 'Z';
  return stream.str();
}

std::string random_identifier()
{
  std::random_device device;
  std::mt19937_64 generator(device());
  std::uniform_int_distribution<std::uint64_t> distribution;
  std::ostringstream stream;
  stream << std::hex << std::setfill('0')
         << std::setw(16) << distribution(generator)
         << std::setw(16) << distribution(generator);
  return stream.str();
}

std::string column_text(sqlite3_stmt * statement, int index)
{
  const auto * value = reinterpret_cast<const char *>(sqlite3_column_text(statement, index));
  return value != nullptr ? value : "";
}

}  // namespace

class TaskManagerNode : public rclcpp::Node
{
public:
  TaskManagerNode()
  : Node("task_manager_node")
  {
    data_root_ = declare_parameter<std::string>(
      "data_root", (fs::path(std::getenv("HOME") ? std::getenv("HOME") : "/tmp") /
      ".local/share/capture_system").string());
    database_path_ = (fs::path(data_root_) / "capture.db").string();
    recorder_prepare_service_ = declare_parameter<std::string>(
      "recorder_prepare_service", "/capture/recording/prepare");
    recorder_control_service_ = declare_parameter<std::string>(
      "recorder_control_service", "/capture/recording/control");
    recorder_service_timeout_ms_ = declare_parameter<int>(
      "recorder_service_timeout_ms", 5000);
    start_transition_timeout_ms_ = declare_parameter<int>(
      "start_transition_timeout_ms", 8000);
    pause_resume_timeout_ms_ = declare_parameter<int>(
      "pause_resume_timeout_ms", 5000);
    stop_transition_timeout_ms_ = declare_parameter<int>(
      "stop_transition_timeout_ms", 20000);

    callback_group_ = create_callback_group(rclcpp::CallbackGroupType::Reentrant);
    status_publisher_ = create_publisher<interfaces::msg::TaskStatus>(
      "/capture/task/status",
      rclcpp::QoS(rclcpp::KeepLast(10)).reliable().transient_local());
    prepare_client_ = create_client<interfaces::srv::PrepareRecording>(
      recorder_prepare_service_, rmw_qos_profile_services_default, callback_group_);
    recorder_control_client_ = create_client<interfaces::srv::RecordingCommand>(
      recorder_control_service_, rmw_qos_profile_services_default, callback_group_);

    start_service_ = create_service<interfaces::srv::StartTask>(
      "/capture/task/start",
      std::bind(&TaskManagerNode::start_task, this, std::placeholders::_1, std::placeholders::_2),
      rmw_qos_profile_services_default, callback_group_);
    pause_service_ = create_service<interfaces::srv::TaskCommand>(
      "/capture/task/pause",
      std::bind(&TaskManagerNode::pause_task, this, std::placeholders::_1, std::placeholders::_2),
      rmw_qos_profile_services_default, callback_group_);
    resume_service_ = create_service<interfaces::srv::TaskCommand>(
      "/capture/task/resume",
      std::bind(&TaskManagerNode::resume_task, this, std::placeholders::_1, std::placeholders::_2),
      rmw_qos_profile_services_default, callback_group_);
    stop_service_ = create_service<interfaces::srv::TaskCommand>(
      "/capture/task/stop",
      std::bind(&TaskManagerNode::stop_task, this, std::placeholders::_1, std::placeholders::_2),
      rmw_qos_profile_services_default, callback_group_);
    recover_service_ = create_service<interfaces::srv::TaskCommand>(
      "/capture/task/recover",
      std::bind(&TaskManagerNode::recover_task, this, std::placeholders::_1, std::placeholders::_2),
      rmw_qos_profile_services_default, callback_group_);

    recording_status_subscription_ = create_subscription<interfaces::msg::RecordingStatus>(
      "/capture/recording/status",
      rclcpp::QoS(rclcpp::KeepLast(10)).reliable().transient_local(),
      std::bind(&TaskManagerNode::on_recording_status, this, std::placeholders::_1));

    recover_interrupted_tasks();
    if (!recovered_task_ids_.empty()) {
      recovery_timer_ = create_wall_timer(
        500ms, std::bind(&TaskManagerNode::recover_recording_files, this), callback_group_);
    }
    transition_watchdog_timer_ = create_wall_timer(
      500ms, std::bind(&TaskManagerNode::check_transition_watchdog, this), callback_group_);
  }

private:
  struct TaskRow
  {
    std::string task_id;
    std::uint64_t sequence{0};
    std::string tunnel_code;
    std::string tunnel_name;
    std::string status;
    std::string phase;
    std::string batch_status;
    std::uint64_t revision{0};
    std::string entry_rtk_status{"not_requested"};
    std::string exit_rtk_status{"not_requested"};
    bool has_measurements{false};
    std::string recording_path;
    std::int64_t start_requested_ns{0};
    std::int64_t started_at_ns{0};
    std::int64_t stop_requested_ns{0};
    std::int64_t completed_at_ns{0};
    std::int64_t updated_at_ns{0};
    std::int64_t transition_deadline_ns{0};
    bool active{false};
  };

  struct TransitionResult
  {
    bool accepted{false};
    TaskRow task;
    std::string error_code;
    std::string message;
  };

  void start_task(
    const std::shared_ptr<interfaces::srv::StartTask::Request> request,
    std::shared_ptr<interfaces::srv::StartTask::Response> response)
  {
    std::lock_guard<std::mutex> command_lock(command_mutex_);
    TransitionResult result;
    try {
      result = begin_start(*request);
      if (!result.accepted) {
        fill_response(*response, result);
        return;
      }
      publish_task(result.task, request->command_id, "开始命令已接受，执行雷达初始化阶段", "");

      result.task = set_phase(
        request->task_id, "entry_rtk_capture", request->command_id,
        "开始时同步记录入口RTK快照，不等待坐标确认");
      publish_task(result.task, request->command_id, "正在记录入口RTK快照", "");

      result.task = set_phase(
        request->task_id, "recorder_preparing", request->command_id,
        "正在创建正式测量文件");
      publish_task(result.task, request->command_id, "正在创建正式测量文件", "");

      auto recorder_response = call_prepare_recorder(*request, result.task);
      if (!recorder_response.has_value()) {
        result = recover_active_task(
          request->task_id, request->command_id, "recorder_unavailable",
          "记录器Service不可用或响应超时，系统已执行启动失败收尾");
        result.accepted = false;
        publish_task(result.task, request->command_id, result.message, result.error_code);
        fill_response(*response, result);
        return;
      }
      if (!recorder_response->success) {
        result = recover_active_task(
          request->task_id, request->command_id,
          recorder_response->error_code.empty() ? "storage_error" : recorder_response->error_code,
          recorder_response->message);
        result.accepted = false;
        publish_task(result.task, request->command_id, result.message, result.error_code);
        fill_response(*response, result);
        return;
      }

      result.task = complete_start(
        request->task_id, request->command_id,
        recorder_response->recording_path, recorder_response->entry_rtk_status);
      result.accepted = true;
      result.message = recorder_response->message;
      publish_task(result.task, request->command_id, result.message, "");
    } catch (const std::exception & error) {
      try {
        const auto task = load_task_by_id(request->task_id);
        if (task.has_value() && task->active && task->status == "pending") {
          result = recover_active_task(
            request->task_id, request->command_id, "start_exception", error.what());
          result.accepted = false;
          publish_task(result.task, request->command_id, result.message, result.error_code);
        } else {
          result = reject_with_task(
            task.value_or(TaskRow{}), "task_database_unavailable", error.what());
        }
      } catch (...) {
        result = reject_without_task("task_database_unavailable", error.what());
      }
    }
    fill_response(*response, result);
  }

  void pause_task(
    const std::shared_ptr<interfaces::srv::TaskCommand::Request> request,
    std::shared_ptr<interfaces::srv::TaskCommand::Response> response)
  {
    handle_simple_command("pause", *request, *response);
  }

  void resume_task(
    const std::shared_ptr<interfaces::srv::TaskCommand::Request> request,
    std::shared_ptr<interfaces::srv::TaskCommand::Response> response)
  {
    handle_simple_command("resume", *request, *response);
  }

  void stop_task(
    const std::shared_ptr<interfaces::srv::TaskCommand::Request> request,
    std::shared_ptr<interfaces::srv::TaskCommand::Response> response)
  {
    handle_simple_command("stop", *request, *response);
  }

  void recover_task(
    const std::shared_ptr<interfaces::srv::TaskCommand::Request> request,
    std::shared_ptr<interfaces::srv::TaskCommand::Response> response)
  {
    std::lock_guard<std::mutex> command_lock(command_mutex_);
    TransitionResult result;
    try {
      auto task = load_task_by_id(request->task_id);
      if (!task.has_value()) {
        result = reject_without_task("task_not_found", "任务不存在");
      } else if (task->revision != request->expected_revision) {
        result = reject_with_task(*task, "revision_conflict", "任务状态已经变化，请刷新后重试");
      } else if (!task->active) {
        result = reject_with_task(*task, "state_conflict", "任务当前未占用活动控制槽");
      } else {
        result = recover_transition_task(
          *task, request->command_id, "manual_recovery",
          "操作员请求恢复卡住的任务控制状态");
        result.accepted = true;
        publish_task(result.task, request->command_id, result.message, result.error_code);
      }
    } catch (const std::exception & error) {
      result = reject_without_task("task_database_unavailable", error.what());
    }
    fill_response(*response, result);
  }

  void handle_simple_command(
    const std::string & command,
    const interfaces::srv::TaskCommand::Request & request,
    interfaces::srv::TaskCommand::Response & response)
  {
    std::lock_guard<std::mutex> command_lock(command_mutex_);
    TransitionResult result;
    try {
      if (command == "pause") {
        result = begin_stable_transition(
          request, "running", "recording", "pausing", "pause_requested",
          "正在暂停正式测量记录");
      } else if (command == "resume") {
        result = begin_stable_transition(
          request, "paused", "paused", "resuming", "resume_requested",
          "正在继续正式测量记录");
      } else {
        result = begin_stop(request);
      }
      if (!result.accepted) {
        fill_response(response, result);
        return;
      }
      publish_task(result.task, request.command_id, result.message, "");

      const bool cancelling_start = command == "stop" && result.task.status == "pending";
      if (command == "stop" && !cancelling_start) {
        result.task = set_phase(
          request.task_id, "exit_rtk_capture", request.command_id,
          "停止时同步记录出口RTK快照，不等待坐标确认");
        publish_task(result.task, request.command_id, "正在记录出口RTK快照", "");
        result.task = set_phase(
          request.task_id, "finalizing", request.command_id,
          "正在完成任务测量文件");
        publish_task(result.task, request.command_id, "正在完成任务测量文件", "");
      }

      const std::string recorder_command = command == "stop" ?
        (cancelling_start ? "abort" : "finalize") : command;
      const auto recorder_requested_ns = command == "stop" && result.task.stop_requested_ns > 0 ?
        result.task.stop_requested_ns : system_now_ns();
      auto recorder_response = call_recorder_command(
        request.task_id, recorder_command, recorder_requested_ns);
      if (!recorder_response.has_value()) {
        result = cancelling_start ?
          recover_active_task(request.task_id, request.command_id, "recorder_unavailable",
            "记录器Service不可用，开始流程已由任务管理器解除") :
          fail_active_command(
            request.task_id, request.command_id,
            command == "stop" ? "interrupted" : stable_phase_for_command(command),
            "recorder_unavailable", "记录器Service不可用或响应超时",
            command == "stop");
        publish_task(result.task, request.command_id, result.message, result.error_code);
        fill_response(response, result);
        return;
      }
      if (!recorder_response->success) {
        result = cancelling_start && recorder_response->error_code == "recorder_not_active" ?
          TransitionResult{true, cancel_preparing_task(
            request.task_id, request.command_id, 0, "",
            "开始流程尚未进入正式记录，活动状态已解除"), "", "开始流程已取消"} :
          fail_active_command(
            request.task_id, request.command_id,
            command == "stop" ? "interrupted" : stable_phase_for_command(command),
            recorder_response->error_code.empty() ? "storage_error" : recorder_response->error_code,
            recorder_response->message,
            command == "stop");
        publish_task(result.task, request.command_id, result.message, result.error_code);
        fill_response(response, result);
        return;
      }

      if (cancelling_start) {
        result.task = cancel_preparing_task(
          request.task_id, request.command_id, recorder_response->total_samples,
          recorder_response->recording_path, "开始流程已取消");
      } else if (command == "pause") {
        result.task = finish_pause_resume(
          request.task_id, request.command_id, "paused", "paused", "paused",
          recorder_response->message);
      } else if (command == "resume") {
        result.task = finish_pause_resume(
          request.task_id, request.command_id, "running", "recording", "resumed",
          recorder_response->message);
      } else {
        result.task = finish_stop(request.task_id, request.command_id, *recorder_response);
      }
      result.accepted = true;
      result.message = recorder_response->message;
      publish_task(result.task, request.command_id, result.message, "");
    } catch (const std::exception & error) {
      try {
        const auto task = load_task_by_id(request.task_id);
        if (task.has_value() && task->active) {
          if (command == "pause") {
            result = fail_active_command(
              request.task_id, request.command_id, "recording", "control_exception",
              error.what(), false);
          } else if (command == "resume") {
            result = fail_active_command(
              request.task_id, request.command_id, "paused", "control_exception",
              error.what(), false);
          } else {
            result = recover_transition_task(
              *task, request.command_id, "control_exception", error.what());
          }
          result.accepted = false;
          publish_task(result.task, request.command_id, result.message, result.error_code);
        } else {
          result = reject_with_task(
            task.value_or(TaskRow{}), "task_database_unavailable", error.what());
        }
      } catch (...) {
        result = reject_without_task("task_database_unavailable", error.what());
      }
    }
    fill_response(response, result);
  }

  TransitionResult begin_start(const interfaces::srv::StartTask::Request & request)
  {
    if (request.task_id.empty() || request.command_id.empty() ||
      (request.lane != "left" && request.lane != "right") ||
      request.lidar_mount_height_m <= 0.0 || request.clearance_threshold_m <= 0.0)
    {
      return reject_without_task("invalid_parameters", "开始参数无效");
    }
    sqlite3 * database = open_database();
    try {
      execute(database, "BEGIN IMMEDIATE");
      const auto duplicate = load_duplicate_command(
        database, request.command_id, request.task_id, "start");
      if (duplicate.has_value()) {
        execute(database, "COMMIT");
        sqlite3_close(database);
        return *duplicate;
      }
      auto task = load_task(database, request.task_id);
      if (!task.has_value()) {
        execute(database, "ROLLBACK");
        sqlite3_close(database);
        return reject_without_task("task_not_found", "任务不存在");
      }
      if (task->batch_status != "active") {
        auto result = reject_with_task(*task, "batch_not_active", "任务所属作业已经结束，不能开始采集");
        store_control_request(database, request.command_id, request.task_id, "start", false, result);
        execute(database, "COMMIT");
        sqlite3_close(database);
        return result;
      }
      if (task->revision != request.expected_revision) {
        auto result = reject_with_task(*task, "revision_conflict", "任务状态已经变化，请刷新后重试");
        store_control_request(database, request.command_id, request.task_id, "start", false, result);
        execute(database, "COMMIT");
        sqlite3_close(database);
        return result;
      }
      if (task->status != "pending" ||
        (task->phase != "idle" && task->phase != "failed"))
      {
        auto result = reject_with_task(*task, "state_conflict", "当前任务状态不允许开始");
        store_control_request(database, request.command_id, request.task_id, "start", false, result);
        execute(database, "COMMIT");
        sqlite3_close(database);
        return result;
      }
      const auto active_count = scalar_int64(
        database,
        "SELECT COUNT(*) FROM tasks WHERE active_slot IS NOT NULL AND deleted_at IS NULL");
      if (active_count > 0) {
        auto result = reject_with_task(*task, "active_task_exists", "已有其他任务处于活动状态");
        store_control_request(database, request.command_id, request.task_id, "start", false, result);
        execute(database, "COMMIT");
        sqlite3_close(database);
        return result;
      }

      const auto now_ns = system_now_ns();
      const auto now_text = iso_utc_from_ns(now_ns);
      const auto session_id = random_identifier();
      sqlite3_stmt * update = nullptr;
      check_sqlite(
        sqlite3_prepare_v2(
          database,
          "UPDATE tasks SET operation_phase='radar_initializing', status_revision=status_revision+1, "
          "active_session_id=?, active_slot=1, start_requested_at=?, updated_at=?, "
          "transition_started_at=?, transition_deadline_at=?, "
          "entry_rtk_status='pending', exit_rtk_status='not_requested', last_error_code=NULL, "
          "last_error_message=NULL, warning_code=NULL WHERE task_id=?",
          -1, &update, nullptr), database, "准备开始状态更新失败");
      bind_text(update, 1, session_id);
      bind_text(update, 2, now_text);
      bind_text(update, 3, now_text);
      bind_text(update, 4, now_text);
      bind_text(update, 5, iso_utc_from_ns(now_ns + static_cast<std::int64_t>(start_transition_timeout_ms_) * 1'000'000LL));
      bind_text(update, 6, request.task_id);
      check_sqlite(sqlite3_step(update), database, "更新开始状态失败");
      sqlite3_finalize(update);

      sqlite3_stmt * parameters = nullptr;
      check_sqlite(
        sqlite3_prepare_v2(
          database,
          "INSERT INTO task_parameters (task_id, lane, lidar_mount_height_m, "
          "clearance_threshold_m, captured_at, parameter_schema_version) VALUES (?, ?, ?, ?, ?, 1) "
          "ON CONFLICT(task_id) DO UPDATE SET lane=excluded.lane, "
          "lidar_mount_height_m=excluded.lidar_mount_height_m, "
          "clearance_threshold_m=excluded.clearance_threshold_m, captured_at=excluded.captured_at",
          -1, &parameters, nullptr), database, "准备任务参数写入失败");
      bind_text(parameters, 1, request.task_id);
      bind_text(parameters, 2, request.lane);
      sqlite3_bind_double(parameters, 3, request.lidar_mount_height_m);
      sqlite3_bind_double(parameters, 4, request.clearance_threshold_m);
      bind_text(parameters, 5, now_text);
      check_sqlite(sqlite3_step(parameters), database, "写入任务参数失败");
      sqlite3_finalize(parameters);
      insert_event(
        database, request.task_id, "start_requested", task->status, task->status,
        "radar_initializing", request.command_id, now_text,
        "开始命令已接受，不等待雷达或RTK真实数据检查", "");
      auto updated = load_task(database, request.task_id).value();
      TransitionResult result{true, updated, "", "开始命令已接受"};
      store_control_request(database, request.command_id, request.task_id, "start", true, result);
      execute(database, "COMMIT");
      sqlite3_close(database);
      return result;
    } catch (...) {
      try {execute(database, "ROLLBACK");} catch (...) {}
      sqlite3_close(database);
      throw;
    }
  }

  TransitionResult begin_stable_transition(
    const interfaces::srv::TaskCommand::Request & request,
    const std::string & required_status, const std::string & required_phase,
    const std::string & transition_phase, const std::string & event_type,
    const std::string & message)
  {
    return begin_command(
      request, required_status, required_phase, transition_phase,
      event_type, message);
  }

  TransitionResult begin_stop(const interfaces::srv::TaskCommand::Request & request)
  {
    sqlite3 * database = open_database();
    try {
      execute(database, "BEGIN IMMEDIATE");
      const auto duplicate = load_duplicate_command(
        database, request.command_id, request.task_id, "stop");
      if (duplicate.has_value()) {
        execute(database, "COMMIT");
        sqlite3_close(database);
        return *duplicate;
      }
      auto task = load_task(database, request.task_id);
      if (!task.has_value()) {
        execute(database, "ROLLBACK");
        sqlite3_close(database);
        return reject_without_task("task_not_found", "任务不存在");
      }
      if (task->revision != request.expected_revision) {
        auto result = reject_with_task(*task, "revision_conflict", "任务状态已经变化，请刷新后重试");
        store_control_request(database, request.command_id, request.task_id, "stop", false, result);
        execute(database, "COMMIT");
        sqlite3_close(database);
        return result;
      }
      const bool stable_running = task->status == "running" && task->phase == "recording";
      const bool stable_paused = task->status == "paused" && task->phase == "paused";
      const bool preparing = task->status == "pending" && task->active;
      const bool transitioning = task->active && (
        task->phase == "pausing" || task->phase == "resuming" ||
        task->phase == "stop_requested" || task->phase == "exit_rtk_capture" ||
        task->phase == "finalizing");
      if (!stable_running && !stable_paused && !preparing && !transitioning) {
        auto result = reject_with_task(*task, "state_conflict", "当前任务状态不允许停止");
        store_control_request(database, request.command_id, request.task_id, "stop", false, result);
        execute(database, "COMMIT");
        sqlite3_close(database);
        return result;
      }
      const auto now_text = iso_utc_from_ns(system_now_ns());
      update_phase(database, request.task_id, "stop_requested", now_text, std::nullopt);
      execute_prepared(
        database,
        "UPDATE tasks SET stop_requested_at=? WHERE task_id=?",
        {now_text, request.task_id});
      insert_event(
        database, request.task_id, "stop_requested", task->status, task->status,
        "stop_requested", request.command_id, now_text,
        "停止命令已接受，净空记录边界固定为当前时刻", "");
      auto updated = load_task(database, request.task_id).value();
      TransitionResult result{true, updated, "", "正在停止任务"};
      store_control_request(database, request.command_id, request.task_id, "stop", true, result);
      execute(database, "COMMIT");
      sqlite3_close(database);
      return result;
    } catch (...) {
      try {execute(database, "ROLLBACK");} catch (...) {}
      sqlite3_close(database);
      throw;
    }
  }

  TransitionResult begin_command(
    const interfaces::srv::TaskCommand::Request & request,
    const std::string & required_status, const std::string & required_phase,
    const std::string & transition_phase, const std::string & event_type,
    const std::string & message)
  {
    const std::string command = transition_phase == "pausing" ? "pause" : "resume";
    sqlite3 * database = open_database();
    try {
      execute(database, "BEGIN IMMEDIATE");
      const auto duplicate = load_duplicate_command(
        database, request.command_id, request.task_id, command);
      if (duplicate.has_value()) {
        execute(database, "COMMIT");
        sqlite3_close(database);
        return *duplicate;
      }
      auto task = load_task(database, request.task_id);
      if (!task.has_value()) {
        execute(database, "ROLLBACK");
        sqlite3_close(database);
        return reject_without_task("task_not_found", "任务不存在");
      }
      if (task->revision != request.expected_revision) {
        auto result = reject_with_task(*task, "revision_conflict", "任务状态已经变化，请刷新后重试");
        store_control_request(database, request.command_id, request.task_id, command, false, result);
        execute(database, "COMMIT");
        sqlite3_close(database);
        return result;
      }
      if (task->status != required_status || task->phase != required_phase) {
        auto result = reject_with_task(*task, "state_conflict", "当前任务状态不允许该操作");
        store_control_request(database, request.command_id, request.task_id, command, false, result);
        execute(database, "COMMIT");
        sqlite3_close(database);
        return result;
      }
      const auto now_text = iso_utc_from_ns(system_now_ns());
      update_phase(database, request.task_id, transition_phase, now_text, std::nullopt);
      insert_event(
        database, request.task_id, event_type, task->status, task->status,
        transition_phase, request.command_id, now_text, message, "");
      auto updated = load_task(database, request.task_id).value();
      TransitionResult result{true, updated, "", message};
      store_control_request(database, request.command_id, request.task_id, command, true, result);
      execute(database, "COMMIT");
      sqlite3_close(database);
      return result;
    } catch (...) {
      try {execute(database, "ROLLBACK");} catch (...) {}
      sqlite3_close(database);
      throw;
    }
  }

  TaskRow set_phase(
    const std::string & task_id, const std::string & phase,
    const std::string & command_id, const std::string & message)
  {
    sqlite3 * database = open_database();
    try {
      execute(database, "BEGIN IMMEDIATE");
      const auto task = load_task(database, task_id).value();
      const auto now_text = iso_utc_from_ns(system_now_ns());
      update_phase(database, task_id, phase, now_text, std::nullopt);
      insert_event(
        database, task_id, phase, task.status, task.status, phase,
        command_id, now_text, message, "");
      const auto updated = load_task(database, task_id).value();
      execute(database, "COMMIT");
      sqlite3_close(database);
      return updated;
    } catch (...) {
      try {execute(database, "ROLLBACK");} catch (...) {}
      sqlite3_close(database);
      throw;
    }
  }

  TransitionResult fail_start(
    const std::string & task_id, const std::string & command_id,
    const std::string & error_code, const std::string & message)
  {
    sqlite3 * database = open_database();
    try {
      execute(database, "BEGIN IMMEDIATE");
      const auto task = load_task(database, task_id).value();
      const auto now_text = iso_utc_from_ns(system_now_ns());
      sqlite3_stmt * statement = nullptr;
      check_sqlite(
        sqlite3_prepare_v2(
          database,
          "UPDATE tasks SET operation_phase='failed', status_revision=status_revision+1, "
          "active_session_id=NULL, active_slot=NULL, entry_rtk_status='unconfirmed', "
          "last_error_code=?, last_error_message=?, updated_at=?, "
          "transition_started_at=NULL, transition_deadline_at=NULL WHERE task_id=?",
          -1, &statement, nullptr), database, "准备启动失败状态更新失败");
      bind_text(statement, 1, error_code);
      bind_text(statement, 2, message);
      bind_text(statement, 3, now_text);
      bind_text(statement, 4, task_id);
      check_sqlite(sqlite3_step(statement), database, "更新启动失败状态失败");
      sqlite3_finalize(statement);
      insert_event(
        database, task_id, "start_failed", task.status, "pending", "failed",
        command_id, now_text, message, error_code);
      const auto updated = load_task(database, task_id).value();
      execute(database, "COMMIT");
      sqlite3_close(database);
      return TransitionResult{false, updated, error_code, message};
    } catch (...) {
      try {execute(database, "ROLLBACK");} catch (...) {}
      sqlite3_close(database);
      throw;
    }
  }

  TaskRow complete_start(
    const std::string & task_id, const std::string & command_id,
    const std::string & recording_path, const std::string & entry_rtk_status)
  {
    sqlite3 * database = open_database();
    try {
      execute(database, "BEGIN IMMEDIATE");
      const auto previous = load_task(database, task_id).value();
      const auto now_ns = system_now_ns();
      const auto now_text = iso_utc_from_ns(now_ns);
      const auto started_text = previous.start_requested_ns > 0 ?
        iso_utc_from_ns(previous.start_requested_ns) : now_text;
      sqlite3_stmt * statement = nullptr;
      check_sqlite(
        sqlite3_prepare_v2(
          database,
          "UPDATE tasks SET status='running', operation_phase='recording', "
          "status_revision=status_revision+1, started_at=?, updated_at=?, "
          "entry_rtk_status=?, recording_path=?, last_error_code=NULL, "
          "last_error_message=NULL, transition_started_at=NULL, transition_deadline_at=NULL WHERE task_id=?",
          -1, &statement, nullptr), database, "准备采集状态更新失败");
      bind_text(statement, 1, started_text);
      bind_text(statement, 2, now_text);
      bind_text(statement, 3, entry_rtk_status.empty() ? "unconfirmed" : entry_rtk_status);
      bind_text(statement, 4, recording_path);
      bind_text(statement, 5, task_id);
      check_sqlite(sqlite3_step(statement), database, "更新采集状态失败");
      sqlite3_finalize(statement);
      insert_event(
        database, task_id, "recording_started", previous.status, "running", "recording",
        command_id, now_text,
        entry_rtk_status == "confirmed" ? "采集已开始，入口RTK已记录" :
        "采集已开始，入口RTK坐标未确认", "");
      const auto updated = load_task(database, task_id).value();
      execute(database, "COMMIT");
      sqlite3_close(database);
      return updated;
    } catch (...) {
      try {execute(database, "ROLLBACK");} catch (...) {}
      sqlite3_close(database);
      throw;
    }
  }

  TaskRow finish_pause_resume(
    const std::string & task_id, const std::string & command_id,
    const std::string & status, const std::string & phase,
    const std::string & event_type, const std::string & message)
  {
    sqlite3 * database = open_database();
    try {
      execute(database, "BEGIN IMMEDIATE");
      const auto previous = load_task(database, task_id).value();
      const auto now_text = iso_utc_from_ns(system_now_ns());
      sqlite3_stmt * statement = nullptr;
      check_sqlite(
        sqlite3_prepare_v2(
          database,
          "UPDATE tasks SET status=?, operation_phase=?, status_revision=status_revision+1, "
          "updated_at=?, last_error_code=NULL, last_error_message=NULL, transition_started_at=NULL, transition_deadline_at=NULL WHERE task_id=?",
          -1, &statement, nullptr), database, "准备暂停或继续状态更新失败");
      bind_text(statement, 1, status);
      bind_text(statement, 2, phase);
      bind_text(statement, 3, now_text);
      bind_text(statement, 4, task_id);
      check_sqlite(sqlite3_step(statement), database, "更新暂停或继续状态失败");
      sqlite3_finalize(statement);
      insert_event(
        database, task_id, event_type, previous.status, status, phase,
        command_id, now_text, message, "");
      const auto updated = load_task(database, task_id).value();
      execute(database, "COMMIT");
      sqlite3_close(database);
      return updated;
    } catch (...) {
      try {execute(database, "ROLLBACK");} catch (...) {}
      sqlite3_close(database);
      throw;
    }
  }

  TaskRow finish_stop(
    const std::string & task_id, const std::string & command_id,
    const interfaces::srv::RecordingCommand::Response & recorder)
  {
    sqlite3 * database = open_database();
    try {
      execute(database, "BEGIN IMMEDIATE");
      const auto previous = load_task(database, task_id).value();
      const auto now_ns = system_now_ns();
      const auto now_text = iso_utc_from_ns(now_ns);
      const std::string exit_status = recorder.rtk_status.empty() ? "unconfirmed" : recorder.rtk_status;
      const std::string warning =
        previous.entry_rtk_status != "confirmed" || exit_status != "confirmed" ?
        "rtk_endpoint_unconfirmed" : "";
      sqlite3_stmt * statement = nullptr;
      check_sqlite(
        sqlite3_prepare_v2(
          database,
          "UPDATE tasks SET status='completed', operation_phase='completed', "
          "status_revision=status_revision+1, active_session_id=NULL, active_slot=NULL, "
          "completed_at=?, updated_at=?, exit_rtk_status=?, has_measurements=?, "
          "recording_path=?, warning_code=?, last_error_code=NULL, last_error_message=NULL, "
          "transition_started_at=NULL, transition_deadline_at=NULL WHERE task_id=?",
          -1, &statement, nullptr), database, "准备停止状态更新失败");
      bind_text(statement, 1, now_text);
      bind_text(statement, 2, now_text);
      bind_text(statement, 3, exit_status);
      sqlite3_bind_int(statement, 4, recorder.total_samples > 0 ? 1 : 0);
      bind_text(statement, 5, recorder.recording_path);
      if (warning.empty()) {
        sqlite3_bind_null(statement, 6);
      } else {
        bind_text(statement, 6, warning);
      }
      bind_text(statement, 7, task_id);
      check_sqlite(sqlite3_step(statement), database, "更新停止状态失败");
      sqlite3_finalize(statement);
      insert_event(
        database, task_id, "completed", previous.status, "completed", "completed",
        command_id, now_text, recorder.message, "");
      const auto updated = load_task(database, task_id).value();
      execute(database, "COMMIT");
      sqlite3_close(database);
      return updated;
    } catch (...) {
      try {execute(database, "ROLLBACK");} catch (...) {}
      sqlite3_close(database);
      throw;
    }
  }

  TransitionResult fail_active_command(
    const std::string & task_id, const std::string & command_id,
    const std::string & phase, const std::string & error_code,
    const std::string & message, bool release_active)
  {
    sqlite3 * database = open_database();
    try {
      execute(database, "BEGIN IMMEDIATE");
      const auto previous = load_task(database, task_id).value();
      const auto now_text = iso_utc_from_ns(system_now_ns());
      sqlite3_stmt * statement = nullptr;
      const std::string status = release_active ? "interrupted" : previous.status;
      const std::string sql = release_active ?
        "UPDATE tasks SET status=?, operation_phase=?, status_revision=status_revision+1, "
        "active_session_id=NULL, active_slot=NULL, updated_at=?, last_error_code=?, "
        "last_error_message=?, transition_started_at=NULL, transition_deadline_at=NULL WHERE task_id=?" :
        "UPDATE tasks SET status=?, operation_phase=?, status_revision=status_revision+1, "
        "updated_at=?, last_error_code=?, last_error_message=?, transition_started_at=NULL, transition_deadline_at=NULL WHERE task_id=?";
      check_sqlite(sqlite3_prepare_v2(database, sql.c_str(), -1, &statement, nullptr), database, "准备失败状态更新失败");
      bind_text(statement, 1, status);
      bind_text(statement, 2, phase);
      bind_text(statement, 3, now_text);
      bind_text(statement, 4, error_code);
      bind_text(statement, 5, message);
      bind_text(statement, 6, task_id);
      check_sqlite(sqlite3_step(statement), database, "更新失败状态失败");
      sqlite3_finalize(statement);
      insert_event(
        database, task_id, release_active ? "interrupted" : "control_failed",
        previous.status, status, phase, command_id, now_text, message, error_code);
      const auto updated = load_task(database, task_id).value();
      execute(database, "COMMIT");
      sqlite3_close(database);
      return TransitionResult{false, updated, error_code, message};
    } catch (...) {
      try {execute(database, "ROLLBACK");} catch (...) {}
      sqlite3_close(database);
      throw;
    }
  }

  std::optional<interfaces::srv::PrepareRecording::Response> call_prepare_recorder(
    const interfaces::srv::StartTask::Request & request, const TaskRow & task)
  {
    if (!prepare_client_->wait_for_service(250ms)) {
      return std::nullopt;
    }
    auto recorder_request = std::make_shared<interfaces::srv::PrepareRecording::Request>();
    recorder_request->task_id = task.task_id;
    recorder_request->task_sequence = task.sequence;
    recorder_request->tunnel_code = task.tunnel_code;
    recorder_request->tunnel_name = task.tunnel_name;
    recorder_request->lane = request.lane;
    recorder_request->lidar_mount_height_m = request.lidar_mount_height_m;
    recorder_request->clearance_threshold_m = request.clearance_threshold_m;
    recorder_request->requested_at_ns = task.start_requested_ns > 0 ?
      task.start_requested_ns : system_now_ns();
    auto future = prepare_client_->async_send_request(recorder_request);
    if (future.wait_for(std::chrono::milliseconds(recorder_service_timeout_ms_)) != std::future_status::ready) {
      return std::nullopt;
    }
    return *future.get();
  }

  std::optional<interfaces::srv::RecordingCommand::Response> call_recorder_command(
    const std::string & task_id, const std::string & command, std::int64_t requested_at_ns)
  {
    if (!recorder_control_client_->wait_for_service(250ms)) {
      return std::nullopt;
    }
    auto recorder_request = std::make_shared<interfaces::srv::RecordingCommand::Request>();
    recorder_request->task_id = task_id;
    recorder_request->command = command;
    recorder_request->requested_at_ns = requested_at_ns;
    auto future = recorder_control_client_->async_send_request(recorder_request);
    if (future.wait_for(std::chrono::milliseconds(recorder_service_timeout_ms_)) != std::future_status::ready) {
      return std::nullopt;
    }
    return *future.get();
  }

  void on_recording_status(const interfaces::msg::RecordingStatus::SharedPtr message)
  {
    if (message->state != "error" || message->task_id.empty()) {
      return;
    }
    std::lock_guard<std::mutex> command_lock(command_mutex_);
    try {
      const auto task = load_task_by_id(message->task_id);
      if (!task.has_value() || !task->active) {
        return;
      }
      const auto result = fail_active_command(
        message->task_id, "recorder-status", "interrupted",
        message->error_code.empty() ? "recorder_error" : message->error_code,
        message->message, true);
      publish_task(result.task, "recorder-status", result.message, result.error_code);
    } catch (const std::exception & error) {
      RCLCPP_ERROR(get_logger(), "处理记录器错误状态失败：%s", error.what());
    }
  }

  sqlite3 * open_database()
  {
    sqlite3 * database = nullptr;
    const int result = sqlite3_open_v2(
      database_path_.c_str(), &database,
      SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (result != SQLITE_OK) {
      const std::string detail = database != nullptr ? sqlite3_errmsg(database) : "无法打开数据库";
      if (database != nullptr) {
        sqlite3_close(database);
      }
      throw SqliteError("任务数据库不可用：" + detail);
    }
    execute(database, "PRAGMA foreign_keys=ON");
    execute(database, "PRAGMA journal_mode=WAL");
    execute(database, "PRAGMA busy_timeout=5000");
    return database;
  }

  std::optional<TaskRow> load_task_by_id(const std::string & task_id)
  {
    sqlite3 * database = open_database();
    try {
      auto task = load_task(database, task_id);
      sqlite3_close(database);
      return task;
    } catch (...) {
      sqlite3_close(database);
      throw;
    }
  }

  std::optional<TaskRow> load_task(sqlite3 * database, const std::string & task_id)
  {
    sqlite3_stmt * statement = nullptr;
    check_sqlite(
      sqlite3_prepare_v2(
        database,
        "SELECT tasks.task_id, COALESCE(tasks.batch_sequence,tasks.sequence), tasks.tunnel_code, "
        "tasks.tunnel_name, tasks.status, tasks.operation_phase, tasks.status_revision, "
        "tasks.entry_rtk_status, tasks.exit_rtk_status, tasks.has_measurements, "
        "COALESCE(tasks.recording_path,''), COALESCE(tasks.start_requested_at,''), "
        "COALESCE(tasks.started_at,''), COALESCE(tasks.stop_requested_at,''), "
        "COALESCE(tasks.completed_at,''), tasks.active_slot, operation_batches.status, "
        "COALESCE(tasks.updated_at,''), COALESCE(tasks.transition_deadline_at,'') "
        "FROM tasks JOIN operation_batches ON operation_batches.batch_id=tasks.batch_id "
        "WHERE tasks.task_id=? AND tasks.deleted_at IS NULL",
        -1, &statement, nullptr), database, "准备读取任务失败");
    bind_text(statement, 1, task_id);
    const int result = sqlite3_step(statement);
    if (result == SQLITE_DONE) {
      sqlite3_finalize(statement);
      return std::nullopt;
    }
    check_sqlite(result, database, "读取任务失败");
    TaskRow row;
    row.task_id = column_text(statement, 0);
    row.sequence = static_cast<std::uint64_t>(sqlite3_column_int64(statement, 1));
    row.tunnel_code = column_text(statement, 2);
    row.tunnel_name = column_text(statement, 3);
    row.status = column_text(statement, 4);
    row.phase = column_text(statement, 5);
    row.revision = static_cast<std::uint64_t>(sqlite3_column_int64(statement, 6));
    row.entry_rtk_status = column_text(statement, 7);
    row.exit_rtk_status = column_text(statement, 8);
    row.has_measurements = sqlite3_column_int(statement, 9) != 0;
    row.recording_path = column_text(statement, 10);
    row.start_requested_ns = parse_iso_ns(column_text(statement, 11));
    row.started_at_ns = parse_iso_ns(column_text(statement, 12));
    row.stop_requested_ns = parse_iso_ns(column_text(statement, 13));
    row.completed_at_ns = parse_iso_ns(column_text(statement, 14));
    row.active = sqlite3_column_type(statement, 15) != SQLITE_NULL;
    row.batch_status = column_text(statement, 16);
    row.updated_at_ns = parse_iso_ns(column_text(statement, 17));
    row.transition_deadline_ns = parse_iso_ns(column_text(statement, 18));
    sqlite3_finalize(statement);
    return row;
  }

  static std::int64_t parse_iso_ns(const std::string & value)
  {
    if (value.size() < 20 || value.back() != 'Z') {
      return 0;
    }
    std::tm utc{};
    std::istringstream stream(value.substr(0, 19));
    stream >> std::get_time(&utc, "%Y-%m-%dT%H:%M:%S");
    if (stream.fail()) {
      return 0;
    }
    const std::time_t seconds = timegm(&utc);
    if (seconds < 0) {
      return 0;
    }
    std::int64_t fraction_ns = 0;
    if (value.size() > 20 && value[19] == '.') {
      const auto fraction = value.substr(20, value.size() - 21);
      std::int64_t multiplier = 100'000'000LL;
      for (const char character : fraction) {
        if (character < '0' || character > '9' || multiplier <= 0) {
          break;
        }
        fraction_ns += static_cast<std::int64_t>(character - '0') * multiplier;
        multiplier /= 10;
      }
    }
    return static_cast<std::int64_t>(seconds) * 1'000'000'000LL + fraction_ns;
  }

  void update_phase(
    sqlite3 * database, const std::string & task_id, const std::string & phase,
    const std::string & updated_at, const std::optional<std::string> & error_code)
  {
    sqlite3_stmt * statement = nullptr;
    check_sqlite(
      sqlite3_prepare_v2(
        database,
        "UPDATE tasks SET operation_phase=?, status_revision=status_revision+1, "
        "updated_at=?, transition_started_at=?, transition_deadline_at=?, "
        "last_error_code=? WHERE task_id=?",
        -1, &statement, nullptr), database, "准备阶段更新失败");
    bind_text(statement, 1, phase);
    bind_text(statement, 2, updated_at);
    bind_text(statement, 3, updated_at);
    const auto timeout_ms = transition_timeout_ms(phase);
    if (timeout_ms > 0) {
      bind_text(statement, 4, iso_utc_from_ns(system_now_ns() + static_cast<std::int64_t>(timeout_ms) * 1'000'000LL));
    } else {
      sqlite3_bind_null(statement, 4);
    }
    bind_optional_text(statement, 5, error_code);
    bind_text(statement, 6, task_id);
    check_sqlite(sqlite3_step(statement), database, "更新任务阶段失败");
    sqlite3_finalize(statement);
  }

  void execute_prepared(
    sqlite3 * database, const std::string & sql,
    const std::initializer_list<std::string> & values)
  {
    sqlite3_stmt * statement = nullptr;
    check_sqlite(sqlite3_prepare_v2(database, sql.c_str(), -1, &statement, nullptr), database, "准备SQL失败");
    int index = 1;
    for (const auto & value : values) {
      bind_text(statement, index++, value);
    }
    check_sqlite(sqlite3_step(statement), database, "执行SQL失败");
    sqlite3_finalize(statement);
  }

  void insert_event(
    sqlite3 * database, const std::string & task_id, const std::string & event_type,
    const std::string & status_before, const std::string & status_after,
    const std::string & phase, const std::string & command_id,
    const std::string & occurred_at, const std::string & message,
    const std::string & error_code)
  {
    sqlite3_stmt * statement = nullptr;
    check_sqlite(
      sqlite3_prepare_v2(
        database,
        "INSERT INTO task_events (task_id, event_type, status_before, status_after, phase, "
        "command_id, occurred_at, message, error_code) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
        -1, &statement, nullptr), database, "准备任务事件写入失败");
    bind_text(statement, 1, task_id);
    bind_text(statement, 2, event_type);
    bind_text(statement, 3, status_before);
    bind_text(statement, 4, status_after);
    bind_text(statement, 5, phase);
    bind_text(statement, 6, command_id);
    bind_text(statement, 7, occurred_at);
    bind_text(statement, 8, message);
    if (error_code.empty()) {
      sqlite3_bind_null(statement, 9);
    } else {
      bind_text(statement, 9, error_code);
    }
    check_sqlite(sqlite3_step(statement), database, "写入任务事件失败");
    sqlite3_finalize(statement);
  }

  std::optional<TransitionResult> load_duplicate_command(
    sqlite3 * database, const std::string & command_id,
    const std::string & task_id, const std::string & command)
  {
    sqlite3_stmt * statement = nullptr;
    check_sqlite(
      sqlite3_prepare_v2(
        database,
        "SELECT task_id, command, accepted FROM control_requests WHERE command_id=?",
        -1, &statement, nullptr), database, "准备读取控制幂等记录失败");
    bind_text(statement, 1, command_id);
    const int result = sqlite3_step(statement);
    if (result == SQLITE_DONE) {
      sqlite3_finalize(statement);
      return std::nullopt;
    }
    check_sqlite(result, database, "读取控制幂等记录失败");
    const std::string stored_task = column_text(statement, 0);
    const std::string stored_command = column_text(statement, 1);
    const bool accepted = sqlite3_column_int(statement, 2) != 0;
    sqlite3_finalize(statement);
    if (stored_task != task_id || stored_command != command) {
      return reject_without_task(
        "duplicate_command_conflict", "该幂等键已经用于不同的任务控制请求");
    }
    const auto task = load_task(database, task_id);
    if (!task.has_value()) {
      return reject_without_task("task_not_found", "任务不存在");
    }
    return TransitionResult{
      accepted, *task, accepted ? "" : "state_conflict",
      accepted ? "重复请求已返回当前任务状态" : "原控制请求未被接受"};
  }

  void store_control_request(
    sqlite3 * database, const std::string & command_id,
    const std::string & task_id, const std::string & command,
    bool accepted, const TransitionResult & result)
  {
    sqlite3_stmt * statement = nullptr;
    check_sqlite(
      sqlite3_prepare_v2(
        database,
        "INSERT OR IGNORE INTO control_requests (command_id, task_id, command, request_hash, "
        "accepted, response_json, created_at) VALUES (?, ?, ?, ?, ?, ?, ?)",
        -1, &statement, nullptr), database, "准备控制请求记录失败");
    bind_text(statement, 1, command_id);
    bind_text(statement, 2, task_id);
    bind_text(statement, 3, command);
    bind_text(statement, 4, task_id + "|" + command);
    sqlite3_bind_int(statement, 5, accepted ? 1 : 0);
    bind_text(statement, 6, result.message);
    bind_text(statement, 7, iso_utc_from_ns(system_now_ns()));
    check_sqlite(sqlite3_step(statement), database, "写入控制请求记录失败");
    sqlite3_finalize(statement);
  }

  std::int64_t scalar_int64(sqlite3 * database, const std::string & sql)
  {
    sqlite3_stmt * statement = nullptr;
    check_sqlite(sqlite3_prepare_v2(database, sql.c_str(), -1, &statement, nullptr), database, "准备计数查询失败");
    check_sqlite(sqlite3_step(statement), database, "执行计数查询失败");
    const auto value = sqlite3_column_int64(statement, 0);
    sqlite3_finalize(statement);
    return value;
  }

  void publish_task(
    const TaskRow & task, const std::string & command_id,
    const std::string & message, const std::string & error_code)
  {
    interfaces::msg::TaskStatus status;
    status.header.stamp = now();
    status.task_id = task.task_id;
    status.task_sequence = task.sequence;
    status.status = task.status;
    status.operation_phase = task.phase;
    status.status_revision = task.revision;
    status.command_id = command_id;
    status.message = message;
    status.error_code = error_code;
    status.entry_rtk_status = task.entry_rtk_status;
    status.exit_rtk_status = task.exit_rtk_status;
    status.has_measurements = task.has_measurements;
    status.recording_path = task.recording_path;
    status.started_at_ns = task.started_at_ns;
    status.completed_at_ns = task.completed_at_ns;
    status_publisher_->publish(status);
  }

  template<typename ResponseT>
  void fill_response(ResponseT & response, const TransitionResult & result)
  {
    response.accepted = result.accepted;
    response.status = result.task.status.empty() ? "pending" : result.task.status;
    response.operation_phase = result.task.phase.empty() ? "idle" : result.task.phase;
    response.status_revision = result.task.revision;
    response.error_code = result.error_code;
    response.message = result.message;
  }

  TransitionResult reject_without_task(
    const std::string & error_code, const std::string & message)
  {
    TransitionResult result;
    result.accepted = false;
    result.error_code = error_code;
    result.message = message;
    result.task.status = "pending";
    result.task.phase = "idle";
    return result;
  }

  TransitionResult reject_with_task(
    const TaskRow & task, const std::string & error_code, const std::string & message)
  {
    return TransitionResult{false, task, error_code, message};
  }

  std::string stable_phase_for_command(const std::string & command) const
  {
    if (command == "pause") {
      return "recording";
    }
    if (command == "resume") {
      return "paused";
    }
    return "interrupted";
  }

  int transition_timeout_ms(const std::string & phase) const
  {
    if (phase == "radar_initializing" || phase == "entry_rtk_capture" ||
      phase == "recorder_preparing") {
      return start_transition_timeout_ms_;
    }
    if (phase == "pausing" || phase == "resuming") {
      return pause_resume_timeout_ms_;
    }
    if (phase == "stop_requested" || phase == "exit_rtk_capture" || phase == "finalizing") {
      return stop_transition_timeout_ms_;
    }
    return 0;
  }

  TaskRow cancel_preparing_task(
    const std::string & task_id, const std::string & command_id,
    std::uint64_t total_samples, const std::string & recording_path, const std::string & message)
  {
    sqlite3 * database = open_database();
    try {
      execute(database, "BEGIN IMMEDIATE");
      const auto previous = load_task(database, task_id).value();
      const auto now_text = iso_utc_from_ns(system_now_ns());
      const bool has_samples = total_samples > 0;
      sqlite3_stmt * statement = nullptr;
      check_sqlite(sqlite3_prepare_v2(
        database,
        "UPDATE tasks SET status=?, operation_phase=?, status_revision=status_revision+1, "
        "active_session_id=NULL, active_slot=NULL, updated_at=?, has_measurements=?, "
        "recording_path=?, last_error_code=?, last_error_message=?, "
        "transition_started_at=NULL, transition_deadline_at=NULL WHERE task_id=?",
        -1, &statement, nullptr), database, "准备取消开始状态更新失败");
      bind_text(statement, 1, has_samples ? "interrupted" : "pending");
      bind_text(statement, 2, has_samples ? "interrupted" : "idle");
      bind_text(statement, 3, now_text);
      sqlite3_bind_int(statement, 4, has_samples ? 1 : 0);
      if (recording_path.empty()) sqlite3_bind_null(statement, 5); else bind_text(statement, 5, recording_path);
      if (has_samples) bind_text(statement, 6, "start_cancelled_with_samples"); else sqlite3_bind_null(statement, 6);
      if (has_samples) bind_text(statement, 7, message); else sqlite3_bind_null(statement, 7);
      bind_text(statement, 8, task_id);
      check_sqlite(sqlite3_step(statement), database, "更新取消开始状态失败");
      sqlite3_finalize(statement);
      insert_event(database, task_id, "start_cancelled", previous.status,
        has_samples ? "interrupted" : "pending", has_samples ? "interrupted" : "idle",
        command_id, now_text, message, has_samples ? "start_cancelled_with_samples" : "");
      const auto updated = load_task(database, task_id).value();
      execute(database, "COMMIT");
      sqlite3_close(database);
      return updated;
    } catch (...) {
      try {execute(database, "ROLLBACK");} catch (...) {}
      sqlite3_close(database);
      throw;
    }
  }

  TransitionResult recover_active_task(
    const std::string & task_id, const std::string & command_id,
    const std::string & error_code, const std::string & message)
  {
    std::uint64_t samples = 0;
    std::string recording_path;
    std::string rtk_status;
    try {
      const auto recorder = call_recorder_command(task_id, "abort", system_now_ns());
      if (recorder.has_value() && recorder->success) {
        samples = recorder->total_samples;
        recording_path = recorder->recording_path;
        rtk_status = recorder->rtk_status;
      }
    } catch (...) {}
    if (samples > 0) {
      const auto task = interrupt_active_task(
        task_id, command_id, error_code, message, samples, recording_path, rtk_status);
      return TransitionResult{true, task, error_code, message};
    }
    auto result = fail_start(task_id, command_id, error_code, message);
    result.accepted = true;
    return result;
  }

  TaskRow interrupt_active_task(
    const std::string & task_id, const std::string & command_id,
    const std::string & error_code, const std::string & message,
    std::uint64_t total_samples, const std::string & recording_path,
    const std::string & rtk_status)
  {
    sqlite3 * database = open_database();
    try {
      execute(database, "BEGIN IMMEDIATE");
      const auto previous = load_task(database, task_id).value();
      const auto now_text = iso_utc_from_ns(system_now_ns());
      sqlite3_stmt * statement = nullptr;
      check_sqlite(sqlite3_prepare_v2(
        database,
        "UPDATE tasks SET status='interrupted', operation_phase='interrupted', "
        "status_revision=status_revision+1, active_session_id=NULL, active_slot=NULL, "
        "updated_at=?, completed_at=?, has_measurements=?, recording_path=?, "
        "exit_rtk_status=?, last_error_code=?, last_error_message=?, "
        "transition_started_at=NULL, transition_deadline_at=NULL WHERE task_id=?",
        -1, &statement, nullptr), database, "准备异常收尾状态更新失败");
      bind_text(statement, 1, now_text);
      bind_text(statement, 2, now_text);
      sqlite3_bind_int(statement, 3, total_samples > 0 ? 1 : (previous.has_measurements ? 1 : 0));
      if (!recording_path.empty()) {
        bind_text(statement, 4, recording_path);
      } else if (!previous.recording_path.empty()) {
        bind_text(statement, 4, previous.recording_path);
      } else {
        sqlite3_bind_null(statement, 4);
      }
      bind_text(statement, 5, rtk_status.empty() ? "unconfirmed" : rtk_status);
      bind_text(statement, 6, error_code);
      bind_text(statement, 7, message);
      bind_text(statement, 8, task_id);
      check_sqlite(sqlite3_step(statement), database, "更新异常收尾状态失败");
      sqlite3_finalize(statement);
      insert_event(
        database, task_id, "interrupted", previous.status, "interrupted", "interrupted",
        command_id, now_text, message, error_code);
      const auto updated = load_task(database, task_id).value();
      execute(database, "COMMIT");
      sqlite3_close(database);
      return updated;
    } catch (...) {
      try {execute(database, "ROLLBACK");} catch (...) {}
      sqlite3_close(database);
      throw;
    }
  }

  TransitionResult recover_transition_task(
    const TaskRow & task, const std::string & command_id,
    const std::string & error_code, const std::string & message)
  {
    if (task.status == "pending" && (
      task.phase == "radar_initializing" || task.phase == "entry_rtk_capture" ||
      task.phase == "recorder_preparing" || task.phase == "stop_requested"))
    {
      return recover_active_task(task.task_id, command_id, error_code, message);
    }
    if (task.phase == "pausing") {
      return fail_active_command(
        task.task_id, command_id, "recording", error_code, message, false);
    }
    if (task.phase == "resuming") {
      return fail_active_command(
        task.task_id, command_id, "paused", error_code, message, false);
    }

    std::uint64_t total_samples = 0;
    std::string recording_path = task.recording_path;
    std::string rtk_status = task.exit_rtk_status;
    try {
      const auto recorder = call_recorder_command(task.task_id, "abort", system_now_ns());
      if (recorder.has_value() && recorder->success) {
        total_samples = recorder->total_samples;
        if (!recorder->recording_path.empty()) recording_path = recorder->recording_path;
        if (!recorder->rtk_status.empty()) rtk_status = recorder->rtk_status;
      }
    } catch (...) {}
    const auto updated = interrupt_active_task(
      task.task_id, command_id, error_code, message,
      total_samples, recording_path, rtk_status);
    return TransitionResult{true, updated, error_code, message};
  }

  void check_transition_watchdog()
  {
    std::lock_guard<std::mutex> command_lock(command_mutex_);
    try {
      sqlite3 * database = open_database();
      sqlite3_stmt * statement = nullptr;
      check_sqlite(sqlite3_prepare_v2(
        database,
        "SELECT task_id FROM tasks WHERE active_slot IS NOT NULL AND deleted_at IS NULL "
        "AND transition_deadline_at IS NOT NULL AND transition_deadline_at < ? LIMIT 1",
        -1, &statement, nullptr), database, "准备过渡状态看门狗查询失败");
      bind_text(statement, 1, iso_utc_from_ns(system_now_ns()));
      const int step = sqlite3_step(statement);
      std::string task_id;
      if (step == SQLITE_ROW) task_id = column_text(statement, 0);
      else if (step != SQLITE_DONE) check_sqlite(step, database, "执行过渡状态看门狗查询失败");
      sqlite3_finalize(statement);
      sqlite3_close(database);
      if (!task_id.empty()) {
        const auto task = load_task_by_id(task_id);
        if (task.has_value() && task->active) {
          const auto result = recover_transition_task(
            *task, "transition-watchdog", "transition_timeout",
            "任务控制阶段超时，系统已自动恢复到安全状态");
          publish_task(result.task, "transition-watchdog", result.message, result.error_code);
        }
      }
    } catch (const std::exception & error) {
      RCLCPP_ERROR(get_logger(), "任务过渡状态看门狗执行失败：%s", error.what());
    }
  }

  void recover_interrupted_tasks()
  {
    try {
      sqlite3 * database = open_database();
      execute(database, "BEGIN IMMEDIATE");
      const auto now_text = iso_utc_from_ns(system_now_ns());
      sqlite3_stmt * select = nullptr;
      check_sqlite(
        sqlite3_prepare_v2(
          database,
          "SELECT task_id, status, operation_phase FROM tasks "
          "WHERE active_slot IS NOT NULL AND deleted_at IS NULL",
          -1, &select, nullptr), database, "准备恢复活动任务失败");
      while (sqlite3_step(select) == SQLITE_ROW) {
        const auto task_id = column_text(select, 0);
        const auto old_status = column_text(select, 1);
        const auto old_phase = column_text(select, 2);
        sqlite3_stmt * update = nullptr;
        check_sqlite(
          sqlite3_prepare_v2(
            database,
            "UPDATE tasks SET status='interrupted', operation_phase='interrupted', "
            "status_revision=status_revision+1, active_session_id=NULL, active_slot=NULL, "
            "updated_at=?, last_error_code='task_manager_restarted', "
            "last_error_message='任务管理节点重启，活动任务已标记为异常中断', "
            "transition_started_at=NULL, transition_deadline_at=NULL WHERE task_id=?",
            -1, &update, nullptr), database, "准备恢复更新失败");
        bind_text(update, 1, now_text);
        bind_text(update, 2, task_id);
        check_sqlite(sqlite3_step(update), database, "更新异常中断任务失败");
        sqlite3_finalize(update);
        insert_event(
          database, task_id, "interrupted", old_status, "interrupted",
          "interrupted", "startup-recovery", now_text,
          "任务管理节点重启，原活动阶段 " + old_phase + " 已终止",
          "task_manager_restarted");
        recovered_task_ids_.push_back(task_id);
      }
      sqlite3_finalize(select);
      execute(database, "COMMIT");
      sqlite3_close(database);
    } catch (const std::exception & error) {
      RCLCPP_WARN(get_logger(), "任务启动恢复未完成：%s", error.what());
    }
  }

  struct RecoveredFileState
  {
    std::uint64_t total_samples{0};
    std::string exit_rtk_status{"unconfirmed"};
  };

  void recover_recording_files()
  {
    std::lock_guard<std::mutex> command_lock(command_mutex_);
    if (recovered_task_ids_.empty()) {
      recovery_timer_->cancel();
      return;
    }

    ++recovery_attempts_;
    std::vector<std::string> pending;
    pending.reserve(recovered_task_ids_.size());
    for (const auto & task_id : recovered_task_ids_) {
      try {
        // task_manager单独重启时，记录器可能仍持有活动文件。abort只执行异常收尾，
        // 不会将任务改回正常完成状态。
        if (recorder_control_client_->wait_for_service(250ms)) {
          const auto response = call_recorder_command(task_id, "abort", system_now_ns());
          if (response.has_value() && response->success) {
            update_recovered_task(
              task_id,
              RecoveredFileState{response->total_samples,
                response->rtk_status.empty() ? "unconfirmed" : response->rtk_status},
              response->recording_path);
            continue;
          }
        }

        const fs::path recording_file =
          fs::path(data_root_) / "tasks" / task_id / "measurements.db";
        if (fs::is_regular_file(recording_file)) {
          const auto state = inspect_recovered_file(recording_file);
          update_recovered_task(task_id, state, task_id + "/measurements.db");
          continue;
        }
      } catch (const std::exception & error) {
        RCLCPP_WARN(
          get_logger(), "恢复异常中断任务记录失败（%s）：%s",
          task_id.c_str(), error.what());
      }
      if (recovery_attempts_ < 10) {
        pending.push_back(task_id);
      } else {
        RCLCPP_WARN(
          get_logger(), "异常中断任务未找到可读取测量文件：%s", task_id.c_str());
      }
    }
    recovered_task_ids_ = std::move(pending);
    if (recovered_task_ids_.empty()) {
      recovery_timer_->cancel();
    }
  }

  RecoveredFileState inspect_recovered_file(const fs::path & path)
  {
    sqlite3 * database = nullptr;
    const int result = sqlite3_open_v2(
      path.c_str(), &database, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (result != SQLITE_OK) {
      const std::string detail = database != nullptr ? sqlite3_errmsg(database) : "无法打开数据库";
      if (database != nullptr) {
        sqlite3_close(database);
      }
      throw SqliteError("打开异常中断测量文件失败：" + detail);
    }
    try {
      const auto total = scalar_int64(database, "SELECT COUNT(*) FROM clearance_samples");
      sqlite3_stmt * metadata = nullptr;
      check_sqlite(
        sqlite3_prepare_v2(
          database,
          "SELECT COALESCE(exit_rtk_status,'unconfirmed') FROM recording_metadata WHERE id=1",
          -1, &metadata, nullptr),
        database, "准备读取异常记录元数据失败");
      check_sqlite(sqlite3_step(metadata), database, "读取异常记录元数据失败");
      const auto exit_status = column_text(metadata, 0);
      sqlite3_finalize(metadata);
      sqlite3_close(database);
      return RecoveredFileState{
        static_cast<std::uint64_t>(std::max<std::int64_t>(0, total)),
        exit_status.empty() ? "unconfirmed" : exit_status};
    } catch (...) {
      sqlite3_close(database);
      throw;
    }
  }

  void update_recovered_task(
    const std::string & task_id, const RecoveredFileState & state,
    const std::string & recording_path)
  {
    sqlite3 * database = open_database();
    try {
      execute(database, "BEGIN IMMEDIATE");
      const auto now_text = iso_utc_from_ns(system_now_ns());
      sqlite3_stmt * update = nullptr;
      check_sqlite(
        sqlite3_prepare_v2(
          database,
          "UPDATE tasks SET has_measurements=?, recording_path=?, completed_at=COALESCE(completed_at,?), "
          "exit_rtk_status=?, warning_code='task_manager_restarted', updated_at=? "
          "WHERE task_id=? AND status='interrupted'",
          -1, &update, nullptr),
        database, "准备异常记录索引更新失败");
      sqlite3_bind_int(update, 1, state.total_samples > 0 ? 1 : 0);
      bind_text(update, 2, recording_path);
      bind_text(update, 3, now_text);
      bind_text(update, 4, state.exit_rtk_status);
      bind_text(update, 5, now_text);
      bind_text(update, 6, task_id);
      check_sqlite(sqlite3_step(update), database, "更新异常记录索引失败");
      sqlite3_finalize(update);
      execute(database, "COMMIT");
      sqlite3_close(database);
      RCLCPP_WARN(
        get_logger(), "异常中断任务已关联可读取测量文件：%s（%lu条）",
        task_id.c_str(), static_cast<unsigned long>(state.total_samples));
    } catch (...) {
      try {execute(database, "ROLLBACK");} catch (...) {}
      sqlite3_close(database);
      throw;
    }
  }

  std::mutex command_mutex_;
  std::string data_root_;
  std::string database_path_;
  std::string recorder_prepare_service_;
  std::string recorder_control_service_;
  int recorder_service_timeout_ms_{5000};
  int start_transition_timeout_ms_{8000};
  int pause_resume_timeout_ms_{5000};
  int stop_transition_timeout_ms_{20000};
  rclcpp::CallbackGroup::SharedPtr callback_group_;
  rclcpp::Publisher<interfaces::msg::TaskStatus>::SharedPtr status_publisher_;
  rclcpp::Client<interfaces::srv::PrepareRecording>::SharedPtr prepare_client_;
  rclcpp::Client<interfaces::srv::RecordingCommand>::SharedPtr recorder_control_client_;
  rclcpp::Service<interfaces::srv::StartTask>::SharedPtr start_service_;
  rclcpp::Service<interfaces::srv::TaskCommand>::SharedPtr pause_service_;
  rclcpp::Service<interfaces::srv::TaskCommand>::SharedPtr resume_service_;
  rclcpp::Service<interfaces::srv::TaskCommand>::SharedPtr stop_service_;
  rclcpp::Service<interfaces::srv::TaskCommand>::SharedPtr recover_service_;
  rclcpp::Subscription<interfaces::msg::RecordingStatus>::SharedPtr recording_status_subscription_;
  rclcpp::TimerBase::SharedPtr recovery_timer_;
  rclcpp::TimerBase::SharedPtr transition_watchdog_timer_;
  std::vector<std::string> recovered_task_ids_;
  int recovery_attempts_{0};
};

int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);
  auto node = std::make_shared<TaskManagerNode>();
  rclcpp::executors::MultiThreadedExecutor executor(rclcpp::ExecutorOptions(), 4);
  executor.add_node(node);
  executor.spin();
  rclcpp::shutdown();
  return 0;
}

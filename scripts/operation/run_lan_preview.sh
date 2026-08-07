#!/usr/bin/env bash

set -Eeuo pipefail

project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
venv_dir="${project_root}/.venv"
network_config="${project_root}/config/network/web.env"
runtime_config="${project_root}/.build-state/runtime.env"
frontend_index="${project_root}/frontend/out/index.html"
declare -a child_pids=()
cleanup_started=0

print_error() {
  echo "[错误] $*" >&2
}

cleanup() {
  if (( cleanup_started )); then
    return
  fi
  cleanup_started=1

  if (( ${#child_pids[@]} == 0 )); then
    return
  fi

  echo
  echo "正在停止任务状态机、记录器、雷达驱动、RTK驱动、净空计算、系统监控、点云预览和网页服务……"

  # 每个组件运行在独立进程组中，向进程组发送信号可同时停止ROS子进程。
  for pid in "${child_pids[@]}"; do
    if kill -0 "${pid}" 2>/dev/null; then
      kill -INT -- "-${pid}" 2>/dev/null || true
    fi
  done

  # 给ROS Launch和FastAPI预留正常清理资源的时间。
  for _ in {1..50}; do
    all_stopped=1
    for pid in "${child_pids[@]}"; do
      if kill -0 "${pid}" 2>/dev/null; then
        all_stopped=0
        break
      fi
    done
    if (( all_stopped )); then
      break
    fi
    sleep 0.1
  done

  for pid in "${child_pids[@]}"; do
    if kill -0 "${pid}" 2>/dev/null; then
      kill -TERM -- "-${pid}" 2>/dev/null || true
    fi
    wait "${pid}" 2>/dev/null || true
  done
  echo "所有开发预览进程已停止。"
}

trap cleanup EXIT
trap 'exit 130' INT TERM

if [[ ! -f /opt/ros/humble/setup.bash ]]; then
  print_error "未找到ROS 2 Humble环境：/opt/ros/humble/setup.bash"
  exit 1
fi
if [[ ! -f "${project_root}/third_party/odin_ros_driver/install/setup.bash" ]]; then
  print_error "厂商驱动尚未构建，请先按根目录README完成厂商驱动构建。"
  exit 1
fi
if [[ ! -f "${project_root}/ros2_ws/install/setup.bash" ]]; then
  print_error "业务ROS 2工作空间尚未构建，请先运行colcon build。"
  exit 1
fi
if [[ ! -x "${venv_dir}/bin/uvicorn" ]]; then
  print_error "后端虚拟环境缺失，请先安装backend/requirements.txt。"
  exit 1
fi
if [[ ! -f "${frontend_index}" ]]; then
  print_error "前端静态页面缺失，请先运行scripts/build/build.sh web。"
  exit 1
fi
if ! command -v setsid >/dev/null 2>&1; then
  print_error "系统缺少setsid，无法可靠管理开发预览进程。"
  exit 1
fi

if [[ -f "${network_config}" ]]; then
  set -a
  source "${network_config}"
  set +a
fi

if [[ -f "${runtime_config}" ]]; then
  set -a
  source "${runtime_config}"
  set +a
fi
web_port="${UVICORN_PORT:-8000}"
data_root="${CAPTURE_DATA_ROOT:-/home/cat/.local/share/capture_system}"
if [[ "${data_root}" != /* ]]; then
  data_root="$(realpath -m -- "${project_root}/${data_root}")"
fi
mkdir -p "${data_root}/tasks"
if [[ ! -w "${data_root}" || ! -w "${data_root}/tasks" ]]; then
  print_error "任务数据目录不可写：${data_root}"
  exit 1
fi

if ! [[ "${web_port}" =~ ^[0-9]+$ ]] || (( web_port < 1 || web_port > 65535 )); then
  print_error "UVICORN_PORT必须是1至65535之间的整数，当前值：${web_port}"
  exit 1
fi
if ss -Hln "sport = :${web_port}" | grep -q .; then
  print_error "端口${web_port}已被占用，请先停止已有网页服务。"
  exit 1
fi

# ROS生成的环境脚本会读取未定义的可选变量，加载期间临时关闭nounset。
set +u
source /opt/ros/humble/setup.bash
source "${project_root}/third_party/odin_ros_driver/install/setup.bash"
# ODIN驱动使用catkin编译，setup.bash仅设置CMAKE_PREFIX_PATH，
# 需手动补全AMENT_PREFIX_PATH以支持ros2 pkg/launch发现包。
export AMENT_PREFIX_PATH="${project_root}/third_party/odin_ros_driver/install/odin_ros_driver_rev1:${AMENT_PREFIX_PATH}"
source "${project_root}/ros2_ws/install/setup.bash"
set -u

if ! ros2 pkg prefix sensor_adapter >/dev/null 2>&1; then
  print_error "未找到sensor_adapter包，请重新构建业务ROS 2工作空间。"
  exit 1
fi
if ! ros2 pkg prefix bringup >/dev/null 2>&1; then
  print_error "未找到bringup包，请重新构建业务ROS 2工作空间。"
  exit 1
fi
if ! ros2 pkg prefix rtk_driver >/dev/null 2>&1; then
  print_error "未找到rtk_driver包，请重新构建业务ROS 2工作空间。"
  exit 1
fi
if ! ros2 pkg prefix system_monitor >/dev/null 2>&1; then
  print_error "未找到system_monitor包，请重新构建业务ROS 2工作空间。"
  exit 1
fi
if ! ros2 pkg prefix clearance_engine >/dev/null 2>&1; then
  print_error "未找到clearance_engine包，请重新构建业务ROS 2工作空间。"
  exit 1
fi
if ! ros2 pkg prefix motion_compensation >/dev/null 2>&1; then
  print_error "未找到motion_compensation包，请重新构建业务ROS 2工作空间。"
  exit 1
fi
if ! ros2 pkg prefix task_manager >/dev/null 2>&1; then
  print_error "未找到task_manager包，请重新构建业务ROS 2工作空间。"
  exit 1
fi
if ! ros2 pkg prefix data_recorder >/dev/null 2>&1; then
  print_error "未找到data_recorder包，请重新构建业务ROS 2工作空间。"
  exit 1
fi

if [[ "${CAPTURE_DEVTOOLS_ENABLED:-0}" == "1" ]] && ! ros2 pkg prefix rosbag2_storage_mcap >/dev/null 2>&1; then
  echo "[警告] development构建已启用，但未找到rosbag2_storage_mcap。测试页原始点云和诊断MCAP录制将不可用。" >&2
  echo "       可安装：sudo apt install ros-humble-rosbag2-storage-mcap" >&2
fi

echo "正在启动ODIN雷达驱动……"
setsid --wait ros2 launch sensor_adapter odin_driver.launch.py \
  enable_slam_point:=false enable_slam_odom_sync:=false &
child_pids+=("$!")

echo "正在启动实时点云预览节点……"
setsid --wait ros2 launch bringup cloud_preview.launch.py &
child_pids+=("$!")

echo "正在启动单帧RANSAC风机底面检测节点……"
setsid --wait ros2 launch bringup clearance_preview.launch.py &
child_pids+=("$!")

echo "正在启动RTK驱动……"
setsid --wait ros2 launch rtk_driver rtk_driver.launch.py &
child_pids+=("$!")

echo "正在启动系统状态监控……"
setsid --wait ros2 launch bringup system_status.launch.py storage_data_path:="${data_root}" &
child_pids+=("$!")

echo "正在启动局域网网页服务……"
setsid --wait "${project_root}/scripts/operation/run_web.sh" &
child_pids+=("$!")

# 等待健康接口就绪，同时检查是否有组件提前退出。
web_ready=0
for _ in {1..100}; do
  for pid in "${child_pids[@]}"; do
    if ! kill -0 "${pid}" 2>/dev/null; then
      print_error "启动过程中有组件提前退出，请检查上方日志。"
      exit 1
    fi
  done

  if curl --silent --fail --max-time 1 \
    "http://127.0.0.1:${web_port}/api/health" >/dev/null; then
    web_ready=1
    break
  fi
  sleep 0.1
done

if (( ! web_ready )); then
  print_error "网页服务在10秒内未就绪，请检查上方日志。"
  exit 1
fi

# FastAPI先完成capture.db迁移，再启动直接访问同一数据库的设备端状态机。
echo "正在启动设备端任务状态机和50 Hz记录器……"
setsid --wait ros2 launch bringup task_control.launch.py data_root:="${data_root}" &
task_control_pid="$!"
child_pids+=("${task_control_pid}")

required_task_services=(
  /capture/task/start
  /capture/task/pause
  /capture/task/resume
  /capture/task/stop
)
optional_task_services=(/capture/task/recover)
task_control_ready=0
for _ in {1..100}; do
  if ! kill -0 "${task_control_pid}" 2>/dev/null; then
    print_error "任务状态机或记录器启动失败，请检查上方日志。"
    exit 1
  fi
  task_manager_info="$(ros2 node info /task_manager_node 2>/dev/null || true)"
  task_manager_servers="$(
    awk '
      /Service Servers:/ { in_servers=1; next }
      /Service Clients:/ { in_servers=0 }
      in_servers { sub(/:$/, "", $1); print $1 }
    ' <<<"${task_manager_info}"
  )"
  all_required_ready=1
  for service_name in "${required_task_services[@]}"; do
    if ! grep -qx -- "${service_name}" <<<"${task_manager_servers}"; then
      all_required_ready=0
      break
    fi
  done
  if (( all_required_ready )); then
    task_control_ready=1
    break
  fi
  sleep 0.1
done
if (( ! task_control_ready )); then
  print_error "核心任务控制Service在10秒内未全部就绪。"
  exit 1
fi

task_manager_info="$(ros2 node info /task_manager_node 2>/dev/null || true)"
task_manager_servers="$(
  awk '
    /Service Servers:/ { in_servers=1; next }
    /Service Clients:/ { in_servers=0 }
    in_servers { sub(/:$/, "", $1); print $1 }
  ' <<<"${task_manager_info}"
)"
for service_name in "${optional_task_services[@]}"; do
  if ! grep -qx -- "${service_name}" <<<"${task_manager_servers}"; then
    echo "[警告] 可选任务控制Service未就绪：${service_name}。开始、暂停、继续和停止仍可使用。" >&2
  fi
done

echo
echo "局域网页面已启动，请在同一局域网内打开："
while read -r interface_name address; do
  echo "  ${interface_name}: http://${address}:${web_port}/"
done < <(
  ip -o -4 addr show scope global |
    awk '{split($4, parts, "/"); print $2, parts[1]}'
)
echo
echo "按 Ctrl+C 可统一停止任务状态机、记录器、雷达驱动、RTK驱动、净空计算、点云预览和网页服务。"

set +e
wait -n "${child_pids[@]}"
child_status=$?
set -e

print_error "有组件已经退出，退出码：${child_status}"
exit "${child_status}"

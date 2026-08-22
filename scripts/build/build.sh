#!/usr/bin/env bash

set -Eeuo pipefail
umask 022

PROJECT_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
ROS_SETUP="/opt/ros/humble/setup.bash"
ROS2_WS="${PROJECT_ROOT}/ros2_ws"
THIRD_PARTY="${PROJECT_ROOT}/third_party/odin_ros_driver"
DRIVER_PACKAGE="odin_ros_driver_rev1"
DRIVER_PREFIX="${THIRD_PARTY}/install/${DRIVER_PACKAGE}"
SDK_SOURCE="${THIRD_PARTY}/src/odin_ros_driver2/module/sdk_api"
SDK_BUILD="${SDK_SOURCE}/build"
SDK_LIBRARY="${SDK_BUILD}/sdk/libodin_sdk.a"
BACKEND="${PROJECT_ROOT}/backend"
VENV="${PROJECT_ROOT}/.venv"
FRONTEND="${PROJECT_ROOT}/frontend"
STATE_DIR="${PROJECT_ROOT}/.build-state"
LOG_DIR="${PROJECT_ROOT}/.build-logs"

COMMAND="all"
BUILD_TYPE="Release"
VARIANT="customer"
AUTOSTART=""
AUTOSTART_EXPLICIT=0
CLEAN_FIRST=0
FORCE_DEPS=0
JOBS=""
LOG_FILE=""
CURRENT_STEP="初始化"

if [[ -t 1 && -z "${NO_COLOR:-}" ]]; then
  RED=$'\033[0;31m'; GREEN=$'\033[0;32m'; YELLOW=$'\033[1;33m'; CYAN=$'\033[0;36m'; NC=$'\033[0m'
else
  RED=""; GREEN=""; YELLOW=""; CYAN=""; NC=""
fi

info() { printf '%b[INFO]%b  %s\n' "$CYAN" "$NC" "$*"; }
ok()   { printf '%b[OK]%b    %s\n' "$GREEN" "$NC" "$*"; }
warn() { printf '%b[WARN]%b  %s\n' "$YELLOW" "$NC" "$*"; }
err()  { printf '%b[ERROR]%b %s\n' "$RED" "$NC" "$*" >&2; }
step() { CURRENT_STEP="$*"; printf '\n%b━━━ %s ━━━%b\n' "$CYAN" "$CURRENT_STEP" "$NC"; }

die() {
  err "$*"
  [[ -z "$LOG_FILE" ]] || err "日志  ${LOG_FILE}"
  exit 1
}

on_error() {
  local rc=$?
  trap - ERR
  printf '\n' >&2
  err "步骤失败  ${CURRENT_STEP}"
  err "命令      ${BASH_COMMAND:-unknown}"
  err "行号      ${BASH_LINENO[0]:-unknown}"
  err "退出码    ${rc}"
  [[ -z "$LOG_FILE" ]] || err "日志      ${LOG_FILE}"
  exit "$rc"
}
trap on_error ERR

usage() {
  cat <<'EOF_USAGE'
统一构建脚本

用法
  scripts/build/build.sh [all|ros|workspace|driver|sdk|backend|web|verify|doctor|clean|distclean] [选项]

常用命令
  scripts/build/build.sh doctor
  scripts/build/build.sh all --release
  scripts/build/build.sh all --release --variant development
  scripts/build/build.sh all --release --variant customer --autostart on
  scripts/build/build.sh all --release --clean
  scripts/build/build.sh workspace --release
  scripts/build/build.sh web

选项
  --release       Release 构建，默认
  --debug         Debug 构建
  --clean         构建前清理对应缓存
  --jobs N        并行度，默认 min(CPU 核心数, 2)
  --force-deps    强制重新安装 Python/npm 依赖
  --variant NAME  构建变体：customer（默认）或 development
  --autostart MODE 开机自启目标：on 或 off；新项目默认 off，未指定时保留已有构建目标
  -h, --help      显示帮助

命令说明
  all        SDK + 厂商驱动 + 业务 ROS 2 + 后端 + 前端
  ros        SDK + 厂商驱动 + 业务 ROS 2
  workspace  仅业务 ROS 2，要求厂商驱动已构建
  driver     SDK + 厂商驱动
  sdk        仅 ODIN SDK
  backend    后端 Python 环境
  web        前端设备静态页面
  verify     验证已有构建产物
  doctor     检查完整编译环境，不修改文件
  clean      清理编译产物，保留 .venv 和 node_modules
  distclean  清理编译产物和依赖环境

环境变量
  ALLOW_BUILD_WHILE_RUNNING=1  允许系统运行时编译 ROS 2，不推荐

开机自启
  --autostart 只记录构建期目标，不修改 systemd。
  构建完成后执行 sudo bash scripts/deploy/apply_autostart.sh 才会应用到下次开机。

变体说明
  customer      不编译测试工作台组件，不注册 /api/dev 与 /ws/dev
  development   编译测试工作台，运行时启用开发诊断接口

每次执行会写 .build-logs/。脚本不修改 third_party 上游源码。
EOF_USAGE
}

is_command() {
  case "$1" in
    all|ros|workspace|driver|sdk|backend|web|verify|doctor|clean|distclean) return 0 ;;
    *) return 1 ;;
  esac
}

parse_args() {
  if (( $# > 0 )) && is_command "$1"; then COMMAND="$1"; shift; fi
  while (( $# > 0 )); do
    case "$1" in
      debug|Debug|--debug) BUILD_TYPE="Debug" ;;
      release|Release|--release) BUILD_TYPE="Release" ;;
      --clean) CLEAN_FIRST=1 ;;
      --force-deps) FORCE_DEPS=1 ;;
      --variant)
        shift
        (( $# > 0 )) || die "--variant 缺少值"
        VARIANT="$1"
        ;;
      --variant=*) VARIANT="${1#*=}" ;;
      --autostart)
        shift
        (( $# > 0 )) || die "--autostart 缺少值"
        AUTOSTART="$1"
        AUTOSTART_EXPLICIT=1
        ;;
      --autostart=*) AUTOSTART="${1#*=}"; AUTOSTART_EXPLICIT=1 ;;
      --jobs)
        shift
        (( $# > 0 )) || die "--jobs 缺少数值"
        JOBS="$1"
        ;;
      --jobs=*) JOBS="${1#*=}" ;;
      -h|--help|help) usage; exit 0 ;;
      *) err "未知参数  $1"; usage >&2; exit 2 ;;
    esac
    shift
  done
  case "$VARIANT" in
    customer|development) ;;
    *) die "--variant 只能是 customer 或 development，当前为 ${VARIANT}" ;;
  esac

  if (( AUTOSTART_EXPLICIT )); then
    case "$AUTOSTART" in
      on|off) ;;
      *) die "--autostart 只能是 on 或 off，当前为 ${AUTOSTART}" ;;
    esac
  else
    AUTOSTART="$(awk -F= '$1=="CAPTURE_AUTOSTART_DESIRED" {print $2}' "${STATE_DIR}/build.env" 2>/dev/null | tail -n1 || true)"
    case "$AUTOSTART" in
      on|off) ;;
      *) AUTOSTART="off" ;;
    esac
  fi

}

cpu_count() {
  if command -v nproc >/dev/null 2>&1; then nproc; else getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1; fi
}

init_jobs() {
  if [[ -n "$JOBS" ]]; then
    [[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || die "--jobs 必须是正整数，当前值为 ${JOBS}"
    return
  fi
  JOBS="$(cpu_count)"
  [[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || JOBS=1
  (( JOBS <= 2 )) || JOBS=2
}

setup_runtime() {
  mkdir -p "$STATE_DIR" "$LOG_DIR"
  if command -v flock >/dev/null 2>&1; then
    exec 9>"${STATE_DIR}/build.lock"
    flock -n 9 || die "另一个构建进程正在运行"
  fi
  LOG_FILE="${LOG_DIR}/build_$(date '+%Y%m%d_%H%M%S')_${COMMAND}.log"
  exec > >(tee -a "$LOG_FILE") 2>&1
}

require_command() { command -v "$1" >/dev/null 2>&1 || die "未找到命令  $1"; }
require_file() { [[ -f "$1" ]] || die "缺少文件  $1"; }
require_dir() { [[ -d "$1" ]] || die "缺少目录  $1"; }

safe_source() {
  require_file "$1"
  set +u
  # shellcheck disable=SC1090
  source "$1"
  set -u
}

reset_ros_env() {
  unset AMENT_PREFIX_PATH COLCON_PREFIX_PATH CMAKE_PREFIX_PATH ROS_PACKAGE_PATH PYTHONPATH LD_LIBRARY_PATH || true
  unset ROS_VERSION ROS_PYTHON_VERSION ROS_DISTRO RMW_IMPLEMENTATION || true
}

source_ros() { reset_ros_env; safe_source "$ROS_SETUP"; }
source_driver() {
  safe_source "${THIRD_PARTY}/install/setup.bash"
  export AMENT_PREFIX_PATH="${DRIVER_PREFIX}:${AMENT_PREFIX_PATH:-}"
}
source_workspace() { safe_source "${ROS2_WS}/install/setup.bash"; }

sha256_file() { sha256sum "$1" | awk '{print $1}'; }

node_version_ok() {
  local actual
  actual="$(node --version | sed 's/^v//')"
  [[ "$(printf '%s\n%s\n' '22.13.0' "$actual" | sort -V | head -n1)" == '22.13.0' ]]
}

check_layout() {
  require_dir "$SDK_SOURCE"
  require_file "${SDK_SOURCE}/CMakeLists.txt"
  require_dir "${ROS2_WS}/src"
  require_file "${BACKEND}/requirements.txt"
  require_file "${FRONTEND}/package.json"
  require_file "${FRONTEND}/package-lock.json"
}

check_ros_tools() {
  require_file "$ROS_SETUP"
  for cmd in cmake colcon ros2 python3; do require_command "$cmd"; done
  source_ros
  [[ "${ROS_VERSION:-}" == "2" ]] || die "当前 ROS_VERSION 不是 2"
  [[ "${ROS_DISTRO:-}" == "humble" ]] || die "需要 ROS 2 Humble，当前为 ${ROS_DISTRO:-未设置}"
}

check_backend_tools() {
  require_command python3
  python3 -m venv --help >/dev/null 2>&1 || die "Python venv 不可用，请安装 python3-venv"
}

check_web_tools() {
  require_command node; require_command npm; require_command sort
  node_version_ok || die "Node.js 至少需要 22.13.0，当前为 $(node --version)"
}

check_frontend_source_imports() {
  require_command python3
  local report rc=0
  if report="$(python3 - "$FRONTEND" <<'PY_IMPORTS'
from pathlib import Path
import re
import sys

frontend = Path(sys.argv[1])
roots = [frontend / 'app', frontend / 'components', frontend / 'worker']
pattern = re.compile(r"(?:from\s+['\"][^'\"]+\.(?:ts|tsx)['\"]|import\s+['\"][^'\"]+\.(?:ts|tsx)['\"]|import\s*\(['\"][^'\"]+\.(?:ts|tsx)['\"]\))")
matches = []
for root in roots:
    if not root.exists():
        continue
    for path in sorted(root.rglob('*')):
        if path.suffix not in {'.ts', '.tsx'} or not path.is_file():
            continue
        for lineno, line in enumerate(path.read_text(encoding='utf-8').splitlines(), 1):
            if pattern.search(line):
                matches.append(f"{path.relative_to(frontend)}:{lineno}:{line.strip()}")
if matches:
    print('\n'.join(matches))
    sys.exit(4)
PY_IMPORTS
)"; then
    rc=0
  else
    rc=$?
  fi
  if (( rc == 4 )); then
    err "生产前端存在显式 .ts/.tsx 导入后缀，Next.js 类型检查会失败："
    printf '%s\n' "$report" >&2
    die "请移除生产代码 import 路径末尾的 .ts/.tsx；frontend/tests 中的 Node 测试导入不受此限制"
  elif (( rc != 0 )); then
    die "前端生产 import 路径检查失败"
  fi
  ok "前端生产 import 路径检查通过"
}

check_future_timestamps() {
  require_command python3
  local report rc=0
  if report="$(python3 - "$PROJECT_ROOT" <<'PY_TIME'
from pathlib import Path
import sys
import time

root = Path(sys.argv[1])
now = time.time()
limit = now + 120.0
skip_parts = {'.git', '.venv', 'node_modules', '.next', 'out', '.build-logs', '.build-state', 'build', 'install', 'log'}
future = []
for path in root.rglob('*'):
    if not path.is_file():
        continue
    rel = path.relative_to(root)
    if any(part in skip_parts for part in rel.parts):
        continue
    try:
        mtime = path.stat().st_mtime
    except OSError:
        continue
    if mtime > limit:
        future.append((mtime - now, rel.as_posix()))
future.sort(reverse=True)
if future:
    print(f"COUNT={len(future)}")
    for delta, rel in future[:8]:
        print(f"{delta:.0f}s\t{rel}")
    sys.exit(3)
PY_TIME
)"; then
    rc=0
  else
    rc=$?
  fi
  if (( rc == 3 )); then
    err "检测到源码文件时间晚于系统时钟 120 秒以上："
    printf '%s\n' "$report" >&2
    die "请先校准 RK3588 系统时间/NTP，或重新在 Linux 板端解压最新 tar.gz，再开始构建"
  elif (( rc != 0 )); then
    die "源码时间戳检查失败"
  fi
  ok "源码时间戳检查通过"
}

check_disk() {
  local kb
  kb="$(df -Pk "$PROJECT_ROOT" | awk 'NR==2 {print $4}')"
  [[ "$kb" =~ ^[0-9]+$ ]] || return
  info "磁盘可用  $(awk -v n="$kb" 'BEGIN {printf "%.1f GiB", n/1024/1024}')"
  (( kb >= 2 * 1024 * 1024 )) || warn "可用空间低于 2 GiB，全量构建可能失败"
}

check_not_running() {
  [[ "${ALLOW_BUILD_WHILE_RUNNING:-0}" == "1" ]] && return

  local process_pattern local_processes nodes
  if command -v systemctl >/dev/null 2>&1 && systemctl is-active --quiet capture-system.service 2>/dev/null; then
    die "检测到 capture-system.service 正在运行。请先执行 sudo bash scripts/operation/stop_capture_system.sh；该命令只停止当前实例，不会关闭下次开机自启"
  fi

  process_pattern='run_lan_preview\.sh|run_ros_stack\.sh|task_manager_node|data_recorder_node|clearance_engine_node|dead_reckoning_node|fusion_navigation_node|odometry_timestamp_adapter_node|imu_timestamp_adapter_node|enu_cloud_transform_node|cloud_visualization_node|rtk_driver_node|system_monitor_node|odin_driver_|odin_hotplug_manager|odin_param_reader|web_.*_bridge'
  local_processes="$(pgrep -af "${process_pattern}" 2>/dev/null || true)"
  if [[ -n "${local_processes}" ]]; then
    printf '%s\n' "${local_processes}" >&2
    die "检测到本机采集系统进程。若为前台 run_lan_preview.sh，请先在启动终端按 Ctrl+C；若为开机自启实例，请执行 sudo bash scripts/operation/stop_capture_system.sh"
  fi

  # ros2 node list 会发现同一 DDS Domain 内的远端节点，不能据此判断本机正在运行。
  # 保留该检查作为诊断提示，避免另一台 RK3588 导致本机编译被误拦截。
  nodes="$(ros2 node list 2>/dev/null || true)"
  if grep -Eq '^/(task_manager_node|data_recorder_node|clearance_engine_node|enu_cloud_transform_node|cloud_visualization_node|rtk_driver_node|system_monitor_node)$' <<<"$nodes"; then
    warn "ROS 图中发现 Capture System 节点，但本机未发现对应进程；可能来自同一 ROS_DOMAIN_ID 的远端设备，不阻止本机构建"
  fi
}

mode_state() { printf '%s/%s.mode\n' "$STATE_DIR" "$1"; }
legacy_mode() {
  case "$1" in
    sdk) printf '%s/.capture_build_mode\n' "$SDK_SOURCE" ;;
    driver) printf '%s/.capture_build_mode\n' "$THIRD_PARTY" ;;
    workspace) printf '%s/.capture_build_mode\n' "$ROS2_WS" ;;
  esac
}

prepare_mode() {
  local name="$1"; shift
  local current legacy path has_old=0
  current="$(cat "$(mode_state "$name")" 2>/dev/null || true)"
  legacy="$(legacy_mode "$name")"
  [[ -n "$current" ]] || current="$(cat "$legacy" 2>/dev/null || true)"
  for path in "$@"; do [[ ! -e "$path" ]] || has_old=1; done
  if [[ -n "$current" && "$current" != "$BUILD_TYPE" ]]; then
    warn "${name} 从 ${current} 切换为 ${BUILD_TYPE}，清理旧缓存"
    rm -rf -- "$@"
  elif [[ -z "$current" ]] && (( has_old )); then
    warn "${name} 存在来源不明的旧构建产物，清理一次"
    rm -rf -- "$@"
  fi
}

save_mode() {
  printf '%s\n' "$BUILD_TYPE" > "$(mode_state "$1")"
  rm -f -- "$(legacy_mode "$1")"
}

clean_sdk() { rm -rf -- "$SDK_BUILD"; rm -f -- "$(mode_state sdk)" "$(legacy_mode sdk)"; }
clean_driver() { rm -rf -- "${THIRD_PARTY}/build" "${THIRD_PARTY}/install" "${THIRD_PARTY}/log"; rm -f -- "$(mode_state driver)" "$(legacy_mode driver)"; }
clean_workspace() { rm -rf -- "${ROS2_WS}/build" "${ROS2_WS}/install" "${ROS2_WS}/log"; rm -f -- "$(mode_state workspace)" "$(legacy_mode workspace)"; }
clean_web() { rm -rf -- "${FRONTEND}/out" "${FRONTEND}/.next"; }

clean_all() {
  step "清理编译产物"
  clean_sdk; clean_driver; clean_workspace; clean_web
  find "$BACKEND" -type d -name '__pycache__' -prune -exec rm -rf {} + 2>/dev/null || true
  ok "构建产物已清理；.venv 和 node_modules 保留"
}

distclean_all() {
  clean_all
  step "清理依赖环境"
  rm -rf -- "$VENV" "${FRONTEND}/node_modules"
  rm -f -- "${STATE_DIR}/backend.fingerprint" "${STATE_DIR}/frontend.fingerprint"
  ok "依赖环境已清理"
}

run_doctor() {
  step "环境检查"
  check_layout
  for cmd in bash awk grep sed find sha256sum tee; do require_command "$cmd"; done
  check_ros_tools; check_backend_tools; check_web_tools
  check_frontend_source_imports
  check_future_timestamps
  check_disk
  info "ROS 2   ${ROS_DISTRO}"
  info "CMake   $(cmake --version | sed -n '1p')"
  info "Python  $(python3 --version 2>&1)"
  info "Node    $(node --version)"
  info "npm     $(npm --version)"
  info "并行度  ${JOBS}"
  ok "环境检查通过"
}

build_sdk() {
  step "ODIN SDK"
  require_command cmake
  prepare_mode sdk "$SDK_BUILD"
  (( CLEAN_FIRST == 0 )) || clean_sdk
  cmake -S "$SDK_SOURCE" -B "$SDK_BUILD" "-DCMAKE_BUILD_TYPE=${BUILD_TYPE}" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
  cmake --build "$SDK_BUILD" --target odin_sdk --parallel "$JOBS"
  [[ -s "$SDK_LIBRARY" ]] || die "SDK 静态库未生成  ${SDK_LIBRARY}"
  save_mode sdk
  ok "ODIN SDK 构建完成"
}

build_driver_only() {
  step "ODIN ROS 2 驱动"
  check_ros_tools; check_not_running
  [[ -s "$SDK_LIBRARY" ]] || die "ODIN SDK 未构建"
  prepare_mode driver "${THIRD_PARTY}/build" "${THIRD_PARTY}/install" "${THIRD_PARTY}/log"
  (( CLEAN_FIRST == 0 )) || clean_driver
  (
    source_ros
    cd "$THIRD_PARTY"
    colcon build --packages-select "$DRIVER_PACKAGE" --parallel-workers "$JOBS" \
      --cmake-args "-DCMAKE_BUILD_TYPE=${BUILD_TYPE}" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
  )
  require_file "${THIRD_PARTY}/install/setup.bash"
  (
    source_ros; source_driver
    [[ "$(ros2 pkg prefix "$DRIVER_PACKAGE" 2>/dev/null || true)" == "$DRIVER_PREFIX" ]]
  ) || die "厂商驱动构建后无法从预期 install 目录发现"
  save_mode driver
  ok "ODIN ROS 2 驱动构建完成"
}

build_driver() { build_sdk; build_driver_only; }

build_workspace() {
  step "业务 ROS 2 工作空间"
  check_ros_tools; check_not_running
  require_file "${THIRD_PARTY}/install/setup.bash"
  prepare_mode workspace "${ROS2_WS}/build" "${ROS2_WS}/install" "${ROS2_WS}/log"
  (( CLEAN_FIRST == 0 )) || clean_workspace
  (
    source_ros; source_driver
    cd "$ROS2_WS"
    colcon build --parallel-workers "$JOBS" \
      --cmake-args "-DCMAKE_BUILD_TYPE=${BUILD_TYPE}" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
  )
  require_file "${ROS2_WS}/install/setup.bash"
  save_mode workspace
  verify_ros
  ok "业务 ROS 2 工作空间构建完成"
}

backend_fingerprint() {
  { sha256_file "${BACKEND}/requirements.txt"; python3 --version 2>&1; } | sha256sum | awk '{print $1}'
}

setup_backend() {
  step "后端 Python 环境"
  check_backend_tools
  local fp old need=0
  fp="$(backend_fingerprint)"
  old="$(cat "${STATE_DIR}/backend.fingerprint" 2>/dev/null || true)"
  [[ -n "$old" ]] || old="$(cat "${VENV}/.capture_requirements_fingerprint" 2>/dev/null || true)"
  (( FORCE_DEPS )) && need=1
  [[ -x "${VENV}/bin/python" ]] || need=1
  [[ "$fp" == "$old" ]] || need=1
  [[ -x "${VENV}/bin/uvicorn" ]] || need=1
  [[ -x "${VENV}/bin/python" ]] || python3 -m venv "$VENV"
  if (( need )); then
    "${VENV}/bin/python" -m pip install --disable-pip-version-check -r "${BACKEND}/requirements.txt" || die "后端依赖安装失败"
    printf '%s\n' "$fp" > "${STATE_DIR}/backend.fingerprint"
    rm -f -- "${VENV}/.capture_requirements_fingerprint"
  else
    ok "后端依赖未变化，跳过安装"
  fi
  "${VENV}/bin/python" -m pip check
  "${VENV}/bin/python" -c 'import fastapi, reportlab, uvicorn'
  ok "后端环境可用"
}

frontend_fingerprint() {
  { sha256_file "${FRONTEND}/package.json"; sha256_file "${FRONTEND}/package-lock.json"; node --version; npm --version; } | sha256sum | awk '{print $1}'
}

generate_devtools_entry() {
  local entry="${FRONTEND}/components/devtools/devtoolsEntry.generated.tsx"
  mkdir -p "$(dirname "$entry")"
  if [[ "$VARIANT" == "development" ]]; then
    cat > "$entry" <<'EOF_DEVTOOLS'
// Generated by scripts/build/build.sh for the development variant.
export { default as DevToolsWorkspace } from "./DevToolsWorkspace";
export const DEVTOOLS_ENABLED = true as const;
EOF_DEVTOOLS
  else
    cat > "$entry" <<'EOF_DEVTOOLS'
// Generated by scripts/build/build.sh for the customer variant.
export const DEVTOOLS_ENABLED = false as const;
export function DevToolsWorkspace() { return null; }
EOF_DEVTOOLS
  fi
}

write_runtime_variant() {
  cat > "${STATE_DIR}/runtime.env" <<EOF_RUNTIME
CAPTURE_BUILD_VARIANT=${VARIANT}
CAPTURE_DEVTOOLS_ENABLED=$([[ "$VARIANT" == "development" ]] && echo 1 || echo 0)
EOF_RUNTIME
  printf '%s\n' "$VARIANT" > "${STATE_DIR}/variant"
}

write_build_metadata() {
  cat > "${STATE_DIR}/build.env" <<EOF_BUILD
CAPTURE_BUILD_VARIANT=${VARIANT}
CAPTURE_BUILD_TYPE=${BUILD_TYPE}
CAPTURE_AUTOSTART_DESIRED=${AUTOSTART}
EOF_BUILD
  printf '%s\n' "${AUTOSTART}" > "${STATE_DIR}/autostart"
}

verify_customer_frontend() {
  [[ "$VARIANT" != "customer" ]] && return
  if grep -R -a -E '/api/dev/|/ws/dev/|开发测试版本' "${FRONTEND}/out" >/dev/null 2>&1; then
    die "customer 前端产物仍包含开发测试接口或页面标识"
  fi
}

ensure_frontend_dependencies() {
  check_web_tools
  local fp old need=0
  fp="$(frontend_fingerprint)"
  old="$(cat "${STATE_DIR}/frontend.fingerprint" 2>/dev/null || true)"
  [[ -n "$old" ]] || old="$(cat "${FRONTEND}/node_modules/.capture_dependency_fingerprint" 2>/dev/null || true)"
  (( FORCE_DEPS )) && need=1
  [[ -d "${FRONTEND}/node_modules" ]] || need=1
  [[ "$fp" == "$old" ]] || need=1
  if (( need )); then
    (cd "$FRONTEND" && npm ci --no-audit --no-fund) || die "前端依赖安装失败，请检查 npm registry 和 package-lock.json"
    printf '%s\n' "$fp" > "${STATE_DIR}/frontend.fingerprint"
    rm -f -- "${FRONTEND}/node_modules/.capture_dependency_fingerprint"
  else
    ok "前端依赖未变化，跳过 npm ci"
  fi
}

frontend_typecheck() {
  step "前端 TypeScript 预检查"
  check_frontend_source_imports
  ensure_frontend_dependencies
  generate_devtools_entry
  (cd "$FRONTEND" && npm run typecheck) || die "前端 TypeScript 类型检查失败。请先修复上方错误，再执行耗时的 SDK/ROS 2 构建"
  ok "前端 TypeScript 类型检查通过"
}

build_web() {
  step "前端设备静态页面"
  check_frontend_source_imports
  ensure_frontend_dependencies
  generate_devtools_entry
  clean_web
  (cd "$FRONTEND" && CAPTURE_BUILD_VARIANT="$VARIANT" CAPTURE_DEVTOOLS_ENABLED="$([[ "$VARIANT" == "development" ]] && echo 1 || echo 0)" npm run build:device)
  require_file "${FRONTEND}/out/index.html"
  verify_customer_frontend
  write_runtime_variant
  ok "前端静态页面构建完成（${VARIANT}）"
}

verify_ros() {
  local -a packages=(interfaces localization motion_compensation clearance_engine cloud_visualization sensor_adapter rtk_driver system_monitor task_manager data_recorder bringup)
  (
    source_ros; source_driver; source_workspace
    local p expected actual failed=0
    for p in "${packages[@]}"; do
      expected="${ROS2_WS}/install/${p}"
      actual="$(ros2 pkg prefix "$p" 2>/dev/null || true)"
      if [[ "$actual" == "$expected" ]]; then
        printf '  [OK]   ros2:%s\n' "$p"
      else
        printf '  [FAIL] ros2:%s  actual=%s\n' "$p" "${actual:-missing}"
        failed=1
      fi
    done
    (( failed == 0 ))
  ) || die "业务 ROS 2 包验证失败，可能正在使用旧 overlay"
}

run_verify() {
  step "构建结果验证"
  [[ -s "$SDK_LIBRARY" ]] || die "ODIN SDK 缺失"
  require_file "${THIRD_PARTY}/install/setup.bash"
  require_file "${ROS2_WS}/install/setup.bash"
  verify_ros
  [[ -x "${VENV}/bin/uvicorn" ]] || die "后端虚拟环境缺失"
  [[ -f "${FRONTEND}/out/index.html" ]] || die "前端静态页面缺失"
  local built_variant
  built_variant="$(cat "${STATE_DIR}/variant" 2>/dev/null || true)"
  [[ -z "$built_variant" || "$built_variant" == "$VARIANT" ]] || die "前端构建变体为 ${built_variant}，当前验证请求为 ${VARIANT}"
  verify_customer_frontend
  ok "全部构建产物验证通过"
}

main() {
  parse_args "$@"
  init_jobs
  cd "$PROJECT_ROOT"
  setup_runtime
  info "命令      ${COMMAND}"
  info "构建类型  ${BUILD_TYPE}"
  info "构建变体  ${VARIANT}"
  info "自启目标  ${AUTOSTART}"
  info "并行度    ${JOBS}"

  case "$COMMAND" in
    all|ros|workspace|driver|sdk|backend|web|clean|distclean) check_not_running ;;
  esac

  case "$COMMAND" in
    doctor) run_doctor ;;
    clean) clean_all ;;
    distclean) distclean_all ;;
    sdk) check_layout; build_sdk ;;
    driver) check_layout; build_driver ;;
    workspace) check_layout; build_workspace ;;
    ros) check_layout; build_driver; build_workspace ;;
    backend) check_layout; setup_backend ;;
    web) check_layout; frontend_typecheck; build_web ;;
    verify) check_layout; check_ros_tools; run_verify ;;
    all)
      run_doctor
      frontend_typecheck
      build_driver
      build_workspace
      setup_backend
      build_web
      run_verify
      ;;
  esac

  case "$COMMAND" in
    all|ros|workspace|driver|sdk|backend|web|verify) write_build_metadata ;;
  esac

  printf '\n'
  ok "完成  ${COMMAND}"
  info "日志  ${LOG_FILE}"
  if [[ "$COMMAND" == "all" || "$COMMAND" == "verify" ]]; then
    info "手工启动  bash scripts/operation/run_lan_preview.sh"
    info "应用自启  sudo bash scripts/deploy/apply_autostart.sh"
  fi
}

main "$@"

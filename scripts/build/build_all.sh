#!/usr/bin/env bash

set -Eeuo pipefail
umask 022

# =============================================================================
# Capture System 全量构建脚本
#
# 用法
#   scripts/build/build_all.sh
#   scripts/build/build_all.sh debug
#   scripts/build/build_all.sh release
#   scripts/build/build_all.sh clean
#
# 可选环境变量
#   AUTO_FIX_CRLF=1         检测到 CRLF 时自动转换为 LF，默认 0
#   FORCE_REINSTALL_DEPS=1  强制重建 Python 虚拟环境并重装前端依赖，默认 0
#   BUILD_JOBS=N            SDK 编译并行度，默认使用 CPU 逻辑核心数
#
# 构建内容
#   1. CRLF 检查
#   2. ODIN SDK 静态库
#   3. 第三方 ODIN ROS 2 驱动
#   4. ROS 2 业务工作空间
#   5. Python 后端虚拟环境
#   6. Next.js 前端静态导出
#   7. 构建结果验证
# =============================================================================

# ---- 项目路径 ----
PROJECT_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
THIRD_PARTY_DIR="${PROJECT_ROOT}/third_party/odin_ros_driver"
SDK_SRC_DIR="${THIRD_PARTY_DIR}/src/odin_ros_driver2/module/sdk_api"
SDK_BUILD_DIR="${SDK_SRC_DIR}/build"
SDK_LIB="${SDK_BUILD_DIR}/sdk/libodin_sdk.a"
ROS2_WS="${PROJECT_ROOT}/ros2_ws"
BACKEND_DIR="${PROJECT_ROOT}/backend"
VENV_DIR="${PROJECT_ROOT}/.venv"
FRONTEND_DIR="${PROJECT_ROOT}/frontend"
ROS_SETUP="/opt/ros/humble/setup.bash"

DRIVER_MODE_MARKER="${THIRD_PARTY_DIR}/.capture_build_mode"
ROS_MODE_MARKER="${ROS2_WS}/.capture_build_mode"
SDK_MODE_MARKER="${SDK_SRC_DIR}/.capture_build_mode"
BACKEND_FINGERPRINT_FILE="${VENV_DIR}/.capture_requirements_fingerprint"
FRONTEND_FINGERPRINT_FILE="${FRONTEND_DIR}/node_modules/.capture_dependency_fingerprint"

AUTO_FIX_CRLF="${AUTO_FIX_CRLF:-0}"
FORCE_REINSTALL_DEPS="${FORCE_REINSTALL_DEPS:-0}"

cd "$PROJECT_ROOT"

# ---- 颜色输出 ----
if [[ -t 1 && -z "${NO_COLOR:-}" ]]; then
  RED=$'\033[0;31m'
  GREEN=$'\033[0;32m'
  YELLOW=$'\033[1;33m'
  CYAN=$'\033[0;36m'
  NC=$'\033[0m'
else
  RED=""
  GREEN=""
  YELLOW=""
  CYAN=""
  NC=""
fi

info() {
  printf '%b[INFO]%b  %s\n' "$CYAN" "$NC" "$*"
}

ok() {
  printf '%b[OK]%b    %s\n' "$GREEN" "$NC" "$*"
}

warn() {
  printf '%b[WARN]%b  %s\n' "$YELLOW" "$NC" "$*"
}

err() {
  printf '%b[ERROR]%b %s\n' "$RED" "$NC" "$*" >&2
}

CURRENT_STEP="初始化"

step() {
  CURRENT_STEP="$*"
  printf '\n%b━━━ %s ━━━%b\n' "$CYAN" "$CURRENT_STEP" "$NC"
}

die() {
  err "$*"
  exit 1
}

on_error() {
  local exit_code=$?
  local line_number="${BASH_LINENO[0]:-unknown}"
  local failed_command="${BASH_COMMAND:-unknown}"

  trap - ERR
  printf '\n' >&2
  err "步骤失败  ${CURRENT_STEP}"
  err "行号  ${line_number}"
  err "命令  ${failed_command}"
  err "退出码  ${exit_code}"
  exit "$exit_code"
}

trap on_error ERR

usage() {
  cat <<EOF_USAGE
用法
  $0 [debug|release|clean]

环境变量
  AUTO_FIX_CRLF=1         自动将 CRLF 转换为 LF
  FORCE_REINSTALL_DEPS=1  强制重装后端和前端依赖
  BUILD_JOBS=N            SDK 编译并行度
EOF_USAGE
}

safe_source() {
  local setup_file="$1"

  [[ -f "$setup_file" ]] || die "环境文件不存在  ${setup_file}"

  set +u
  # shellcheck disable=SC1090
  source "$setup_file"
  set -u
}

require_command() {
  local command_name="$1"
  command -v "$command_name" >/dev/null 2>&1 || die "未找到命令  ${command_name}"
}

require_file() {
  local file_path="$1"
  [[ -f "$file_path" ]] || die "缺少文件  ${file_path}"
}

require_directory() {
  local directory_path="$1"
  [[ -d "$directory_path" ]] || die "缺少目录  ${directory_path}"
}

sha256_file() {
  sha256sum "$1" | awk '{print $1}'
}

detect_build_jobs() {
  local detected=""

  if command -v nproc >/dev/null 2>&1; then
    detected="$(nproc)"
  elif command -v getconf >/dev/null 2>&1; then
    detected="$(getconf _NPROCESSORS_ONLN 2>/dev/null || true)"
  fi

  if [[ ! "$detected" =~ ^[1-9][0-9]*$ ]]; then
    detected="1"
  fi

  printf '%s\n' "$detected"
}

validate_boolean_option() {
  local name="$1"
  local value="$2"

  if [[ "$value" != "0" && "$value" != "1" ]]; then
    die "${name} 只能取 0 或 1，当前值为 ${value}"
  fi
}

prepare_colcon_workspace() {
  local workspace="$1"
  local marker="$2"
  local requested_mode="$3"
  local previous_mode=""
  local must_clean=0

  previous_mode="$(cat "$marker" 2>/dev/null || true)"

  if [[ -n "$previous_mode" && "$previous_mode" != "$requested_mode" ]]; then
    warn "构建模式由 ${previous_mode} 切换为 ${requested_mode}，清理旧工作空间产物"
    must_clean=1
  elif [[ -z "$previous_mode" ]] && \
       { [[ -d "${workspace}/build" ]] || [[ -d "${workspace}/install" ]] || [[ -d "${workspace}/log" ]]; }; then
    warn "检测到缺少模式记录的旧构建产物，执行一次完整清理"
    must_clean=1
  fi

  if (( must_clean )); then
    rm -rf -- \
      "${workspace}/build" \
      "${workspace}/install" \
      "${workspace}/log"
  fi
}

record_build_mode() {
  local marker="$1"
  local mode="$2"
  printf '%s\n' "$mode" > "$marker"
}

prepare_sdk_build_tree() {
  local requested_mode="$1"
  local previous_mode=""

  previous_mode="$(cat "$SDK_MODE_MARKER" 2>/dev/null || true)"

  if [[ -n "$previous_mode" && "$previous_mode" != "$requested_mode" ]]; then
    warn "ODIN SDK 构建模式由 ${previous_mode} 切换为 ${requested_mode}，清理旧缓存"
    rm -rf -- "$SDK_BUILD_DIR"
  elif [[ -z "$previous_mode" && -d "$SDK_BUILD_DIR" ]]; then
    warn "检测到缺少模式记录的 SDK 构建目录，执行一次完整清理"
    rm -rf -- "$SDK_BUILD_DIR"
  fi
}

remove_unsafe_driver_dsv_entries() {
  local driver_dsv="${THIRD_PARTY_DIR}/install/odin_ros_driver_rev1/share/odin_ros_driver_rev1/package.dsv"

  [[ -f "$driver_dsv" ]] || return 0

  if grep -qE '^source;share/odin_ros_driver_rev1/local_setup\.(bash|dsv|ps1|sh|zsh)$' "$driver_dsv"; then
    warn "发现旧脚本写入的自引用 package.dsv 条目，正在删除"
    sed -i -E \
      '\%^source;share/odin_ros_driver_rev1/local_setup\.(bash|dsv|ps1|sh|zsh)$%d' \
      "$driver_dsv"
  fi
}

package_prefix_matches() {
  local package_name="$1"
  local expected_prefix="$2"
  local actual_prefix=""

  actual_prefix="$(ros2 pkg prefix "$package_name" 2>/dev/null || true)"
  [[ "$actual_prefix" == "$expected_prefix" ]]
}

# ---- 参数解析 ----
if (( $# > 1 )); then
  usage >&2
  exit 2
fi

BUILD_MODE_INPUT="${1:-debug}"

case "$BUILD_MODE_INPUT" in
  debug|Debug|--debug|"")
    BUILD_MODE="Debug"
    ;;
  release|Release|--release)
    BUILD_MODE="Release"
    ;;
  clean|Clean|--clean)
    step "清理构建产物"
    rm -rf -- \
      "${ROS2_WS}/build" \
      "${ROS2_WS}/install" \
      "${ROS2_WS}/log" \
      "$ROS_MODE_MARKER" \
      "${THIRD_PARTY_DIR}/build" \
      "${THIRD_PARTY_DIR}/install" \
      "${THIRD_PARTY_DIR}/log" \
      "$DRIVER_MODE_MARKER" \
      "$SDK_BUILD_DIR" \
      "$SDK_MODE_MARKER" \
      "${FRONTEND_DIR}/out" \
      "${FRONTEND_DIR}/.next" \
      "$VENV_DIR"
    ok "清理完成"
    exit 0
    ;;
  -h|--help|help)
    usage
    exit 0
    ;;
  *)
    err "未知参数  ${BUILD_MODE_INPUT}"
    usage >&2
    exit 2
    ;;
esac

if [[ "$BUILD_MODE" == "Release" ]]; then
  info "构建模式  Release"
else
  info "构建模式  Debug"
fi

validate_boolean_option "AUTO_FIX_CRLF" "$AUTO_FIX_CRLF"
validate_boolean_option "FORCE_REINSTALL_DEPS" "$FORCE_REINSTALL_DEPS"

BUILD_JOBS="${BUILD_JOBS:-$(detect_build_jobs)}"
if [[ ! "$BUILD_JOBS" =~ ^[1-9][0-9]*$ ]]; then
  die "BUILD_JOBS 必须是正整数，当前值为 ${BUILD_JOBS}"
fi

# ---- 前置检查 ----
step "前置检查"

require_file "$ROS_SETUP"
require_directory "$THIRD_PARTY_DIR"
require_directory "$SDK_SRC_DIR"
require_file "${SDK_SRC_DIR}/CMakeLists.txt"
require_directory "${ROS2_WS}/src"
require_directory "$BACKEND_DIR"
require_file "${BACKEND_DIR}/requirements.txt"
require_directory "$FRONTEND_DIR"
require_file "${FRONTEND_DIR}/package.json"
require_file "${FRONTEND_DIR}/package-lock.json"

for command_name in \
  awk \
  bash \
  cmake \
  find \
  grep \
  node \
  npm \
  python3 \
  sed \
  sha256sum; do
  require_command "$command_name"
done

safe_source "$ROS_SETUP"
require_command colcon
require_command ros2

if [[ "${ROS_VERSION:-}" != "2" ]]; then
  die "当前 ROS_VERSION 不是 2"
fi

if [[ "${ROS_DISTRO:-}" != "humble" ]]; then
  die "当前 ROS_DISTRO 不是 humble，实际值为 ${ROS_DISTRO:-未设置}"
fi

if ! python3 -m venv --help >/dev/null 2>&1; then
  die "Python venv 模块不可用，请安装 python3-venv"
fi

info "ROS 2  ${ROS_DISTRO}"
info "CMake  $(cmake --version | sed -n '1p')"
info "Python  $(python3 --version 2>&1)"
info "Node.js  $(node --version)"
info "npm  $(npm --version)"
info "SDK 编译并行度  ${BUILD_JOBS}"
ok "前置检查通过"

# ---- Step 1  CRLF 检查 ----
step "1/6 CRLF 检查"

declare -a CRLF_FILES=()

while IFS= read -r -d '' candidate; do
  if LC_ALL=C grep -q $'\r$' "$candidate"; then
    CRLF_FILES+=("$candidate")
  fi
done < <(
  find "$PROJECT_ROOT" \
    -type f \
    \( \
      -name '*.sh' -o \
      -name '*.py' -o \
      -name '*.xml' -o \
      -name '*.yaml' -o \
      -name '*.yml' -o \
      -name '*.cfg' -o \
      -name '*.env' -o \
      -name '*.md' -o \
      -name '*.txt' -o \
      -name '*.cpp' -o \
      -name '*.hpp' -o \
      -name '*.h' -o \
      -name '*.c' -o \
      -name '*.cmake' -o \
      -name 'CMakeLists.txt' -o \
      -name '*.launch.py' -o \
      -name '*.ts' -o \
      -name '*.tsx' -o \
      -name '*.js' -o \
      -name '*.jsx' -o \
      -name '*.json' -o \
      -name '*.css' -o \
      -name '*.scss' -o \
      -name '*.html' \
    \) \
    ! -path '*/.git/*' \
    ! -path '*/.venv/*' \
    ! -path '*/build/*' \
    ! -path '*/install/*' \
    ! -path '*/log/*' \
    ! -path '*/node_modules/*' \
    ! -path '*/.next/*' \
    ! -path '*/out/*' \
    -print0
)

if (( ${#CRLF_FILES[@]} == 0 )); then
  ok "未发现 CRLF 文件"
elif (( AUTO_FIX_CRLF == 1 )); then
  warn "发现 ${#CRLF_FILES[@]} 个 CRLF 文件，正在转换为 LF"
  for candidate in "${CRLF_FILES[@]}"; do
    sed -i 's/\r$//' "$candidate"
  done
  ok "CRLF 转换完成"
else
  err "发现 ${#CRLF_FILES[@]} 个 CRLF 文件"
  for candidate in "${CRLF_FILES[@]:0:20}"; do
    err "  ${candidate#"$PROJECT_ROOT"/}"
  done
  if (( ${#CRLF_FILES[@]} > 20 )); then
    err "  其余 $(( ${#CRLF_FILES[@]} - 20 )) 个文件未列出"
  fi
  die "请修复换行符，或使用 AUTO_FIX_CRLF=1 重新执行"
fi

# ---- Step 2  ODIN SDK ----
step "2/6 ODIN SDK 静态库"

prepare_sdk_build_tree "$BUILD_MODE"

cmake \
  -S "$SDK_SRC_DIR" \
  -B "$SDK_BUILD_DIR" \
  "-DCMAKE_BUILD_TYPE=${BUILD_MODE}" \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

cmake \
  --build "$SDK_BUILD_DIR" \
  --target odin_sdk \
  --parallel "$BUILD_JOBS"

[[ -s "$SDK_LIB" ]] || die "SDK 静态库未生成  ${SDK_LIB}"
record_build_mode "$SDK_MODE_MARKER" "$BUILD_MODE"

SDK_SIZE="$(du -h "$SDK_LIB" | awk '{print $1}')"
ok "SDK 构建完成  ${SDK_LIB}  ${SDK_SIZE}"

# ---- 构建参数 ----
declare -a DRIVER_COLCON_ARGS=()
declare -a ROS_COLCON_ARGS=()

if [[ "$BUILD_MODE" == "Release" ]]; then
  DRIVER_COLCON_ARGS+=(
    --cmake-args
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
  )
  ROS_COLCON_ARGS+=(
    --cmake-args
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
  )
else
  DRIVER_COLCON_ARGS+=(
    --symlink-install
    --cmake-args
    -DCMAKE_BUILD_TYPE=Debug
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
  )
  ROS_COLCON_ARGS+=(
    --symlink-install
    --cmake-args
    -DCMAKE_BUILD_TYPE=Debug
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
  )
fi

# ---- Step 3  厂商驱动 ----
step "3/6 厂商 ODIN ROS 2 驱动"

prepare_colcon_workspace "$THIRD_PARTY_DIR" "$DRIVER_MODE_MARKER" "$BUILD_MODE"
safe_source "$ROS_SETUP"

pushd "$THIRD_PARTY_DIR" >/dev/null
colcon build \
  --packages-select odin_ros_driver_rev1 \
  "${DRIVER_COLCON_ARGS[@]}"
popd >/dev/null

record_build_mode "$DRIVER_MODE_MARKER" "$BUILD_MODE"
remove_unsafe_driver_dsv_entries

require_file "${THIRD_PARTY_DIR}/install/setup.bash"
safe_source "${THIRD_PARTY_DIR}/install/setup.bash"

EXPECTED_DRIVER_PREFIX="${THIRD_PARTY_DIR}/install/odin_ros_driver_rev1"
if ! package_prefix_matches odin_ros_driver_rev1 "$EXPECTED_DRIVER_PREFIX"; then
  ACTUAL_DRIVER_PREFIX="$(ros2 pkg prefix odin_ros_driver_rev1 2>/dev/null || true)"
  err "厂商驱动环境加载失败"
  err "实际前缀  ${ACTUAL_DRIVER_PREFIX:-未找到}"
  err "预期前缀  ${EXPECTED_DRIVER_PREFIX}"
  die "请检查厂商驱动的 ament_cmake 安装配置，禁止继续修改生成的 package.dsv"
fi

ok "厂商驱动构建完成"

# ---- Step 4  ROS 2 业务工作空间 ----
step "4/6 ROS 2 业务工作空间"

prepare_colcon_workspace "$ROS2_WS" "$ROS_MODE_MARKER" "$BUILD_MODE"
safe_source "$ROS_SETUP"
safe_source "${THIRD_PARTY_DIR}/install/setup.bash"

pushd "$ROS2_WS" >/dev/null
colcon build "${ROS_COLCON_ARGS[@]}"
popd >/dev/null

record_build_mode "$ROS_MODE_MARKER" "$BUILD_MODE"
require_file "${ROS2_WS}/install/setup.bash"
safe_source "${ROS2_WS}/install/setup.bash"
ok "ROS 2 工作空间构建完成"

# ---- Step 5  后端 Python 环境 ----
step "5/6 后端 Python 环境"

BACKEND_FINGERPRINT="$({
  sha256_file "${BACKEND_DIR}/requirements.txt"
  python3 --version 2>&1
} | sha256sum | awk '{print $1}')"

INSTALLED_BACKEND_FINGERPRINT="$(cat "$BACKEND_FINGERPRINT_FILE" 2>/dev/null || true)"
BACKEND_NEEDS_SETUP=0

if (( FORCE_REINSTALL_DEPS == 1 )); then
  BACKEND_NEEDS_SETUP=1
elif [[ ! -x "${VENV_DIR}/bin/python" ]]; then
  BACKEND_NEEDS_SETUP=1
elif [[ "$BACKEND_FINGERPRINT" != "$INSTALLED_BACKEND_FINGERPRINT" ]]; then
  BACKEND_NEEDS_SETUP=1
elif [[ ! -x "${VENV_DIR}/bin/uvicorn" ]]; then
  BACKEND_NEEDS_SETUP=1
fi

if (( BACKEND_NEEDS_SETUP )); then
  warn "重建后端 Python 虚拟环境"
  rm -rf -- "$VENV_DIR"
  python3 -m venv "$VENV_DIR"
  "${VENV_DIR}/bin/python" -m pip install \
    --disable-pip-version-check \
    -r "${BACKEND_DIR}/requirements.txt"
  printf '%s\n' "$BACKEND_FINGERPRINT" > "$BACKEND_FINGERPRINT_FILE"
else
  ok "后端依赖指纹未变化，跳过安装"
fi

"${VENV_DIR}/bin/python" -m pip check
"${VENV_DIR}/bin/python" -c 'import uvicorn'
ok "后端 Python 环境验证完成"

# ---- Step 6  前端静态页面 ----
step "6/6 前端静态页面"

pushd "$FRONTEND_DIR" >/dev/null

FRONTEND_FINGERPRINT="$({
  sha256_file "${FRONTEND_DIR}/package.json"
  sha256_file "${FRONTEND_DIR}/package-lock.json"
  node --version
  npm --version
} | sha256sum | awk '{print $1}')"

INSTALLED_FRONTEND_FINGERPRINT="$(cat "$FRONTEND_FINGERPRINT_FILE" 2>/dev/null || true)"
FRONTEND_NEEDS_INSTALL=0

if (( FORCE_REINSTALL_DEPS == 1 )); then
  FRONTEND_NEEDS_INSTALL=1
elif [[ ! -d "${FRONTEND_DIR}/node_modules" ]]; then
  FRONTEND_NEEDS_INSTALL=1
elif [[ "$FRONTEND_FINGERPRINT" != "$INSTALLED_FRONTEND_FINGERPRINT" ]]; then
  FRONTEND_NEEDS_INSTALL=1
fi

if (( FRONTEND_NEEDS_INSTALL )); then
  warn "安装前端依赖"
  npm ci --no-audit --no-fund
  printf '%s\n' "$FRONTEND_FINGERPRINT" > "$FRONTEND_FINGERPRINT_FILE"
else
  ok "前端依赖指纹未变化，跳过安装"
fi

rm -rf -- "${FRONTEND_DIR}/out" "${FRONTEND_DIR}/.next"
npm run build:device

popd >/dev/null

require_file "${FRONTEND_DIR}/out/index.html"
ok "前端静态页面构建完成"

# ---- 构建验证 ----
step "构建验证"

safe_source "$ROS_SETUP"
safe_source "${THIRD_PARTY_DIR}/install/setup.bash"
safe_source "${ROS2_WS}/install/setup.bash"

VERIFY_FAILED=0

echo ""
printf '  %-32s %s\n' "组件" "状态"
printf '  %-32s %s\n' "────────────────────────────────" "────"

print_success() {
  printf '  %-32s %b✓%b\n' "$1" "$GREEN" "$NC"
}

print_failure() {
  printf '  %-32s %b✗%b\n' "$1" "$RED" "$NC"
  VERIFY_FAILED=1
}

print_pending() {
  printf '  %-32s %b⏳%b\n' "$1" "$YELLOW" "$NC"
}

if [[ -s "$SDK_LIB" ]] && \
   grep -q "^CMAKE_BUILD_TYPE:STRING=${BUILD_MODE}$" "${SDK_BUILD_DIR}/CMakeCache.txt"; then
  print_success "ODIN SDK  ${BUILD_MODE}"
else
  print_failure "ODIN SDK  ${BUILD_MODE}"
fi

if package_prefix_matches odin_ros_driver_rev1 "$EXPECTED_DRIVER_PREFIX"; then
  print_success "ros2:odin_ros_driver_rev1"
else
  print_failure "ros2:odin_ros_driver_rev1"
fi

ROS2_PKGS=(
  interfaces
  localization
  motion_compensation
  clearance_engine
  cloud_visualization
  sensor_adapter
  rtk_driver
  system_monitor
  bringup
)

for package_name in "${ROS2_PKGS[@]}"; do
  expected_prefix="${ROS2_WS}/install/${package_name}"
  if package_prefix_matches "$package_name" "$expected_prefix"; then
    print_success "ros2:${package_name}"
  else
    print_failure "ros2:${package_name}"
    actual_prefix="$(ros2 pkg prefix "$package_name" 2>/dev/null || true)"
    warn "${package_name} 实际前缀  ${actual_prefix:-未找到}"
    warn "${package_name} 预期前缀  ${expected_prefix}"
  fi
done

print_pending "ros2:task_manager  未实现"
print_pending "ros2:data_recorder  未实现"

if [[ -x "${VENV_DIR}/bin/uvicorn" ]] && \
   [[ "$(cat "$BACKEND_FINGERPRINT_FILE" 2>/dev/null || true)" == "$BACKEND_FINGERPRINT" ]] && \
   "${VENV_DIR}/bin/python" -c 'import uvicorn' >/dev/null 2>&1; then
  print_success "backend venv"
else
  print_failure "backend venv"
fi

if [[ -f "${FRONTEND_DIR}/out/index.html" ]] && \
   [[ "$(cat "$FRONTEND_FINGERPRINT_FILE" 2>/dev/null || true)" == "$FRONTEND_FINGERPRINT" ]]; then
  print_success "frontend static export"
else
  print_failure "frontend static export"
fi

echo ""

if (( VERIFY_FAILED == 0 )); then
  ok "全部组件构建验证通过"
  printf '\n启动命令\n  %s\n\n' "scripts/operation/run_lan_preview.sh"
else
  die "部分组件验证未通过，请检查上方结果"
fi

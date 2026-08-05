#!/usr/bin/env bash

set -euo pipefail

# =============================================================================
# Capture System 全量构建脚本
#
# 用法:
#   scripts/build/build_all.sh          # Debug 构建（开发用，默认）
#   scripts/build/build_all.sh release  # Release 构建（部署/实机用）
#   scripts/build/build_all.sh clean    # 清理所有构建产物
#
# 构建内容:
#   1. CRLF 修复（预防 Windows 换行符问题）
#   2. ODIN SDK 库准备
#   3. 第三方厂商驱动 (third_party/odin_ros_driver)
#   4. ROS 2 业务工作空间 (ros2_ws, 9 个包)
#   5. Python 后端虚拟环境 (backend/)
#   6. Next.js 前端静态导出 (frontend/)
#   7. 全量验证
# =============================================================================

# ---- 颜色输出 ----
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

info()  { echo -e "${CYAN}[INFO]${NC}  $*"; }
ok()    { echo -e "${GREEN}[OK]${NC}    $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC}  $*"; }
err()   { echo -e "${RED}[ERROR]${NC} $*" >&2; }
step()  { echo ""; echo -e "${CYAN}━━━ $* ━━━${NC}"; }

# ---- 参数解析 ----
BUILD_MODE="${1:-debug}"

case "$BUILD_MODE" in
  release|Release|--release)
    BUILD_MODE="Release"
    info "构建模式: Release（优化编译，用于部署/实机）"
    ;;
  debug|Debug|--debug|"")
    BUILD_MODE="Debug"
    info "构建模式: Debug（带调试符号，用于开发）"
    ;;
  clean|Clean|--clean)
    step "清理所有构建产物"
    rm -rf ros2_ws/build ros2_ws/install ros2_ws/log
    rm -rf third_party/odin_ros_driver/build third_party/odin_ros_driver/install third_party/odin_ros_driver/log
    rm -rf frontend/out frontend/.next
    rm -rf .venv
    ok "清理完成"
    exit 0
    ;;
  *)
    err "未知参数: $BUILD_MODE"
    echo "用法: $0 [debug|release|clean]"
    exit 1
    ;;
esac

# ---- 项目路径 ----
PROJECT_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
THIRD_PARTY_DIR="${PROJECT_ROOT}/third_party/odin_ros_driver"
SDK_SRC_DIR="${THIRD_PARTY_DIR}/src/odin_ros_driver2/module/sdk_api"
SDK_LIB_DIR="${SDK_SRC_DIR}/build/sdk"
SDK_LIB="${SDK_LIB_DIR}/libodin_sdk.a"
ROS2_WS="${PROJECT_ROOT}/ros2_ws"
VENV_DIR="${PROJECT_ROOT}/.venv"
FRONTEND_DIR="${PROJECT_ROOT}/frontend"
ROS_SETUP="/opt/ros/humble/setup.bash"

# ---- 前置检查 ----
step "前置检查"

FAILED=0

if [[ ! -f "$ROS_SETUP" ]]; then
  err "未找到 ROS 2 Humble: $ROS_SETUP"
  FAILED=1
fi

if ! command -v cmake &>/dev/null; then
  err "未找到 cmake"
  FAILED=1
fi

if ! command -v python3 &>/dev/null; then
  err "未找到 python3"
  FAILED=1
fi

if ! command -v node &>/dev/null; then
  err "未找到 node"
  FAILED=1
fi

if (( FAILED )); then
  err "前置检查未通过，请先安装缺失的工具"
  exit 1
fi
ok "工具链就绪"

# ---- Step 1: CRLF 修复 ----
step "1/6 CRLF 修复"

crlf_count=$(find "$PROJECT_ROOT" \
  -type f \
  \( -name "*.sh" -o -name "*.py" -o -name "*.xml" -o -name "*.yaml" -o -name "*.yml" \
     -o -name "*.cfg" -o -name "*.env" -o -name "*.md" -o -name "*.txt" \
     -o -name "*.cpp" -o -name "*.hpp" -o -name "*.h" -o -name "*.c" \
     -o -name "*.cmake" -o -name "CMakeLists.txt" -o -name "*.launch.py" \) \
  ! -path "*/.venv/*" \
  ! -path "*/install/*" \
  ! -path "*/build/*" \
  ! -path "*/node_modules/*" \
  ! -path "*/.next/*" \
  ! -path "*/out/*" \
  -exec file {} \; 2>/dev/null | grep -c "CRLF" || true)

if (( crlf_count > 0 )); then
  warn "发现 ${crlf_count} 个文件含 CRLF 换行符，正在修复..."
  find "$PROJECT_ROOT" \
    -type f \
    \( -name "*.sh" -o -name "*.py" -o -name "*.xml" -o -name "*.yaml" -o -name "*.yml" \
       -o -name "*.cfg" -o -name "*.env" -o -name "*.md" -o -name "*.txt" \
       -o -name "*.cpp" -o -name "*.hpp" -o -name "*.h" -o -name "*.c" \
       -o -name "*.cmake" -o -name "CMakeLists.txt" -o -name "*.launch.py" \) \
    ! -path "*/.venv/*" \
    ! -path "*/install/*" \
    ! -path "*/build/*" \
    ! -path "*/node_modules/*" \
    ! -path "*/.next/*" \
    ! -path "*/out/*" \
    -exec sed -i 's/\r$//' {} \;
  ok "CRLF 修复完成 (${crlf_count} 个文件)"
else
  ok "无 CRLF 文件，跳过"
fi

# ---- 辅助: 安全 source ROS 环境 ----
safe_source() {
  set +u
  source "$1"
  set -u
}

# ---- Step 2: ODIN SDK 库 ----
step "2/6 ODIN SDK 库"

safe_source /opt/ros/humble/setup.bash

if [[ -f "$SDK_LIB" ]]; then
  ok "SDK 库已存在: $(du -h "$SDK_LIB" | cut -f1)"
else
  warn "SDK 库未编译，尝试本地编译..."
  if [[ -f "${SDK_SRC_DIR}/build.sh" ]]; then
    cd "${SDK_SRC_DIR}"
    if bash build.sh 2>&1; then
      ok "SDK 编译完成"
    else
      err "SDK 编译失败，请检查 sdk_api 源码完整性"
      exit 1
    fi
  else
    err "SDK 编译脚本不存在且无预编译库: ${SDK_LIB_DIR}"
    err "请将 libodin_sdk.a 放入: ${SDK_LIB_DIR}"
    exit 1
  fi
fi

# ---- Step 3: 厂商驱动 ----
step "3/6 厂商 ODIN 驱动"

cd "${THIRD_PARTY_DIR}"

COLCON_DRIVER_ARGS=""
if [[ "$BUILD_MODE" == "Release" ]]; then
  COLCON_DRIVER_ARGS="--cmake-args -DCMAKE_BUILD_TYPE=Release"
fi

if colcon build ${COLCON_DRIVER_ARGS} 2>&1; then
  ok "厂商驱动构建完成"
else
  err "厂商驱动构建失败"
  exit 1
fi

# ---- 厂商驱动 package.dsv 修复 ----
# 厂商驱动的 CMake 生成 catkin 风格 package.dsv，缺少 local_setup.* 条目，
# 导致 environment/ament_prefix_path.sh 钩子无法触发，ros2 launch 找不到包。
DRIVER_DSV="${THIRD_PARTY_DIR}/install/odin_ros_driver_rev1/share/odin_ros_driver_rev1/package.dsv"
DRIVER_LOCAL_SETUP=(
  "source;share/odin_ros_driver_rev1/local_setup.bash"
  "source;share/odin_ros_driver_rev1/local_setup.dsv"
  "source;share/odin_ros_driver_rev1/local_setup.ps1"
  "source;share/odin_ros_driver_rev1/local_setup.sh"
  "source;share/odin_ros_driver_rev1/local_setup.zsh"
)
MISSING=0
for entry in "${DRIVER_LOCAL_SETUP[@]}"; do
  if ! grep -qF "$entry" "$DRIVER_DSV" 2>/dev/null; then
    MISSING=1
    break
  fi
done
if (( MISSING )); then
  for entry in "${DRIVER_LOCAL_SETUP[@]}"; do
    if ! grep -qF "$entry" "$DRIVER_DSV" 2>/dev/null; then
      echo "$entry" >> "$DRIVER_DSV"
    fi
  done
  ok "已修复厂商驱动 package.dsv（补充 local_setup.* 条目）"
else
  ok "厂商驱动 package.dsv 无需修复"
fi

safe_source "${THIRD_PARTY_DIR}/install/setup.bash"

# ---- Step 4: ROS 2 业务工作空间 ----
step "4/6 ROS 2 业务工作空间"

cd "${ROS2_WS}"

# Release 构建参数:
#   - 不启用 --symlink-install（确保独立部署）
#   - CMAKE_BUILD_TYPE=Release（编译器优化 -O2/-O3）
#   - 并行编译利用多核
# Debug 构建参数:
#   - --symlink-install（开发时修改 Python 即时生效）
#   - 保留调试符号
COLCON_ROS_ARGS=""
if [[ "$BUILD_MODE" == "Release" ]]; then
  COLCON_ROS_ARGS="--cmake-args -DCMAKE_BUILD_TYPE=Release"
  info "Release: 关闭 symlink-install，启用编译器优化"
else
  COLCON_ROS_ARGS="--symlink-install"
  info "Debug: 启用 symlink-install，保留调试符号"
fi

if colcon build ${COLCON_ROS_ARGS} 2>&1; then
  ok "ROS 2 工作空间构建完成"
else
  err "ROS 2 工作空间构建失败"
  exit 1
fi

safe_source "${ROS2_WS}/install/setup.bash"

# ---- Step 5: 后端 Python 环境 ----
step "5/6 后端 Python 环境"

cd "${PROJECT_ROOT}"

VENV_NEEDS_SETUP=0
if [[ ! -x "${VENV_DIR}/bin/uvicorn" ]]; then
  VENV_NEEDS_SETUP=1
elif [[ "${VENV_DIR}/bin/python" -ot "${PROJECT_ROOT}/backend/requirements.txt" ]]; then
  warn "requirements.txt 已更新，重新安装依赖"
  VENV_NEEDS_SETUP=1
fi

if (( VENV_NEEDS_SETUP )); then
  if [[ ! -d "$VENV_DIR" ]]; then
    python3 -m venv "$VENV_DIR"
  fi
  "${VENV_DIR}/bin/python" -m pip install -q -r backend/requirements.txt 2>&1
  ok "后端依赖安装完成"
else
  ok "后端虚拟环境已是最新，跳过"
fi

# ---- Step 6: 前端静态页面 ----
step "6/6 前端静态页面"

cd "${FRONTEND_DIR}"

if [[ ! -d "node_modules" ]] || [[ "package.json" -nt "node_modules" ]] || [[ "package-lock.json" -nt "node_modules" ]]; then
  warn "安装/更新前端依赖..."
  npm ci 2>&1
fi

if npm run build:device 2>&1; then
  ok "前端构建完成"
else
  err "前端构建失败"
  exit 1
fi

# ---- 验证 ----
step "构建验证"

safe_source /opt/ros/humble/setup.bash
safe_source "${THIRD_PARTY_DIR}/install/setup.bash"
safe_source "${ROS2_WS}/install/setup.bash"

VERIFY_OK=0

echo ""
echo "  组件                    状态"
echo "  ───────────────────────  ──────"

# SDK
if [[ -f "$SDK_LIB" ]]; then
  printf "  %-24s ${GREEN}✓${NC}\n" "ODIN SDK"
else
  printf "  %-24s ${RED}✗${NC}\n" "ODIN SDK"
  VERIFY_OK=1
fi

# 厂商驱动
if [[ -f "${THIRD_PARTY_DIR}/install/odin_ros_driver_rev1/share/odin_ros_driver_rev1/package.xml" ]]; then
  printf "  %-24s ${GREEN}✓${NC}\n" "厂商驱动"
else
  printf "  %-24s ${RED}✗${NC}\n" "厂商驱动"
  VERIFY_OK=1
fi

# ROS2 业务包
ROS2_PKGS=(
  interfaces localization motion_compensation clearance_engine
  cloud_visualization sensor_adapter rtk_driver system_monitor bringup
)
for pkg in "${ROS2_PKGS[@]}"; do
  if ros2 pkg prefix "$pkg" &>/dev/null; then
    printf "  %-24s ${GREEN}✓${NC}\n" "ros2:${pkg}"
  else
    printf "  %-24s ${RED}✗${NC}\n" "ros2:${pkg}"
    VERIFY_OK=1
  fi
done

# 规划中模块
for pkg in task_manager data_recorder; do
  printf "  %-24s ${YELLOW}⏳${NC}\n" "ros2:${pkg} (未实现)"
done

# 后端
if [[ -x "${VENV_DIR}/bin/uvicorn" ]]; then
  printf "  %-24s ${GREEN}✓${NC}\n" "backend venv"
else
  printf "  %-24s ${RED}✗${NC}\n" "backend venv"
  VERIFY_OK=1
fi

# 前端
if [[ -f "${FRONTEND_DIR}/out/index.html" ]]; then
  printf "  %-24s ${GREEN}✓${NC}\n" "frontend"
else
  printf "  %-24s ${RED}✗${NC}\n" "frontend"
  VERIFY_OK=1
fi

echo ""

if (( VERIFY_OK == 0 )); then
  ok "全部组件构建验证通过！"
  echo ""
  echo "  启动命令:"
  echo "    scripts/operation/run_lan_preview.sh"
  echo ""
else
  err "部分组件验证未通过，请检查上方输出"
  exit 1
fi

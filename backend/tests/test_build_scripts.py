from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
BUILD_DIR = PROJECT_ROOT / "scripts" / "build"


def test_unified_build_entrypoint_and_compatibility_wrappers_exist() -> None:
    source = (BUILD_DIR / "build.sh").read_text()
    assert "all|ros|workspace|driver|sdk|backend|web|verify|doctor|clean|distclean" in source
    assert "--parallel-workers" in source
    assert "ALLOW_BUILD_WHILE_RUNNING" in source
    assert ".build-logs" in source
    assert ".build-state" in source

    assert 'build.sh" all' in (BUILD_DIR / "build_all.sh").read_text()
    assert 'build.sh" web' in (BUILD_DIR / "build_web.sh").read_text()


def test_clean_and_distclean_have_distinct_dependency_semantics() -> None:
    source = (BUILD_DIR / "build.sh").read_text()
    clean = source[source.index("clean_all()") : source.index("distclean_all()")]
    distclean = source[source.index("distclean_all()") : source.index("run_doctor()")]

    assert 'rm -rf -- "$VENV"' not in clean
    assert '"${FRONTEND}/node_modules"' not in clean
    assert 'rm -rf -- "$VENV" "${FRONTEND}/node_modules"' in distclean


def test_build_script_does_not_rewrite_third_party_source() -> None:
    source = (BUILD_DIR / "build.sh").read_text()
    assert "AUTO_FIX_CRLF" not in source
    assert "sed -i" not in source
    assert 'SDK_BUILD="${SDK_SOURCE}/build"' in source
    assert 'rm -rf -- "${THIRD_PARTY}/build"' in source


def test_build_variants_exclude_devtools_from_customer_frontend_graph() -> None:
    source = (BUILD_DIR / "build.sh").read_text()
    assert 'VARIANT="customer"' in source
    assert '--variant NAME' in source
    assert 'customer|development' in source
    assert 'export { default as DevToolsWorkspace } from "./DevToolsWorkspace";' in source
    assert 'export function DevToolsWorkspace() { return null; }' in source
    assert 'verify_customer_frontend' in source
    assert "/api/dev/|/ws/dev/|开发测试版本" in source
    assert "CAPTURE_DEVTOOLS_ENABLED" in source


def test_frontend_preflight_rejects_explicit_typescript_import_suffixes() -> None:
    source = (BUILD_DIR / "build.sh").read_text()
    assert "check_frontend_source_imports" in source
    assert "Next.js 类型检查会失败" in source
    assert "frontend/tests 中的 Node 测试导入不受此限制" in source
    assert "check_future_timestamps" in source
    assert "源码文件时间晚于系统时钟 120 秒以上" in source


def test_production_frontend_imports_do_not_use_ts_or_tsx_suffixes() -> None:
    import re

    frontend = PROJECT_ROOT / "frontend"
    roots = [frontend / "app", frontend / "components", frontend / "worker"]
    pattern = re.compile(
        r"(?:from\s+['\"][^'\"]+\.(?:ts|tsx)['\"]|"
        r"import\s+['\"][^'\"]+\.(?:ts|tsx)['\"]|"
        r"import\s*\(['\"][^'\"]+\.(?:ts|tsx)['\"]\))"
    )
    failures: list[str] = []
    for root in roots:
        if not root.exists():
            continue
        for path in root.rglob("*"):
            if path.suffix not in {".ts", ".tsx"} or not path.is_file():
                continue
            for lineno, line in enumerate(path.read_text().splitlines(), 1):
                if pattern.search(line):
                    failures.append(f"{path.relative_to(frontend)}:{lineno}:{line.strip()}")
    assert failures == []


def test_all_runs_frontend_typecheck_before_expensive_ros_builds() -> None:
    source = (BUILD_DIR / "build.sh").read_text()
    assert "frontend_typecheck()" in source
    all_block = source[source.index("    all)") : source.index("      ;;", source.index("    all)"))]
    assert all_block.index("frontend_typecheck") < all_block.index("build_driver")
    assert "npm run typecheck" in source
    assert "请先修复上方错误，再执行耗时的 SDK/ROS 2 构建" in source


def test_running_guard_uses_local_processes_and_does_not_block_on_remote_ros_nodes() -> None:
    source = (BUILD_DIR / "build.sh").read_text()
    block = source[source.index("check_not_running()") : source.index("mode_state()")]
    assert "pgrep -af" in block
    assert "ros2 node list" in block
    assert "不阻止本机构建" in block
    assert 'die "采集系统仍在运行' not in block

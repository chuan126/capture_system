#!/usr/bin/env bash
set -Eeuo pipefail
project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"

# 保留旧 build_all.sh 的参数语义：无参数=Debug，release=Release，clean=只清理。
case "${1:-}" in
  "")
    exec "${project_root}/scripts/build/build.sh" all --debug
    ;;
  clean|Clean|--clean)
    shift
    exec "${project_root}/scripts/build/build.sh" clean "$@"
    ;;
  debug|Debug|--debug)
    shift
    exec "${project_root}/scripts/build/build.sh" all --debug "$@"
    ;;
  release|Release|--release)
    shift
    exec "${project_root}/scripts/build/build.sh" all --release "$@"
    ;;
  -h|--help|help)
    exec "${project_root}/scripts/build/build.sh" --help
    ;;
  *)
    exec "${project_root}/scripts/build/build.sh" all "$@"
    ;;
esac

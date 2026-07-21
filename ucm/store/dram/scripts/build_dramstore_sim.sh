#!/usr/bin/env bash
set -euo pipefail

DRAM_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
UCM_ROOT="$(cd "${DRAM_ROOT}/../../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${DRAM_ROOT}/build}"
CXX="${CXX:-g++}"
PYTHON="${PYTHON:-python3}"

if [[ -z "${UCM_P2P_ROOT:-}" ]]; then
    UCM_P2P_ROOT="$(${PYTHON} - <<'PY'
from importlib.metadata import PackageNotFoundError, distribution

try:
    package = distribution("uc-manager")
except PackageNotFoundError as error:
    raise SystemExit("uc-manager is not installed") from error
print(package.locate_file("ucm/transport/p2p"))
PY
)"
fi

P2P_LIBRARY="${UCM_P2P_ROOT}/libucm_p2p_transport.so"
if [[ ! -f "${P2P_LIBRARY}" ]]; then
    echo "Cannot find ${P2P_LIBRARY}" >&2
    exit 1
fi

ASCEND_ROOT="${ASCEND_ROOT:-${ASCEND_HOME_PATH:-/usr/local/Ascend/ascend-toolkit/latest}}"
ASCEND_INCLUDE="${ASCEND_INCLUDE_DIR:-${ASCEND_ROOT}/include}"
ASCEND_LIB="${ASCEND_LIB_DIR:-${ASCEND_ROOT}/lib64}"
if [[ -d "${ASCEND_ROOT}/aarch64-linux/include" ]]; then
    ASCEND_INCLUDE="${ASCEND_ROOT}/aarch64-linux/include"
fi
if [[ -d "${ASCEND_ROOT}/aarch64-linux/lib64" ]]; then
    ASCEND_LIB="${ASCEND_ROOT}/aarch64-linux/lib64"
fi

mkdir -p "${BUILD_DIR}"
"${CXX}" -std=c++17 -Wall -Wextra -Wpedantic ${CXXFLAGS:-} \
    -I"${UCM_P2P_ROOT}/include" \
    -I"${UCM_P2P_ROOT}/include/one_sided" \
    -I"${UCM_ROOT}/ucm/shared/infra" \
    -I"${UCM_ROOT}/ucm/store" \
    -I"${UCM_ROOT}/ucm/store/detail" \
    -I"${DRAM_ROOT}/cc" \
    -I"${ASCEND_INCLUDE}" \
    "${DRAM_ROOT}/tests/dramstore_sim.cpp" \
    "${DRAM_ROOT}/cc/kv_protocol.cpp" \
    "${DRAM_ROOT}/cc/drampool/drampool_launch_config.cpp" \
    "${DRAM_ROOT}/cc/drampool/drampool_yaml_config.cpp" \
    -L"${UCM_P2P_ROOT}" -lucm_p2p_transport \
    -L"${ASCEND_ROOT}" -L"${ASCEND_LIB}" -lascendcl -lcann_hixl -lmetadef \
    -lfmt -lspdlog -lz -lrt -pthread \
    -Wl,-rpath,"${UCM_P2P_ROOT}" -Wl,-rpath,"${ASCEND_LIB}" \
    ${LDFLAGS:-} -o "${BUILD_DIR}/dramstore_sim"

echo "Built ${BUILD_DIR}/dramstore_sim"

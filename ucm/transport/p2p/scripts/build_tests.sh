#!/usr/bin/env bash
# export ASCEND_HOME_PATH=/usr/local/Ascend/cann-9.0.0
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"
PYTHON="${PYTHON:-python3}"
CXX="${CXX:-g++}"
CXXFLAGS="${CXXFLAGS:-}"
LDFLAGS="${LDFLAGS:-}"
HIXL_LIBS="${HIXL_LIBS:--lcann_hixl}"
ASCEND_LIBS="${ASCEND_LIBS:--lascendcl -lmetadef}"
LOGGER_LIBS="${LOGGER_LIBS:--lfmt -lspdlog -lz}"
EXTRA_LIBS="${EXTRA_LIBS:--lrt}"

mkdir -p "${BUILD_DIR}"
rm -f "${BUILD_DIR}/hixl_e2e" \
      "${BUILD_DIR}/hixl_multi_conn_bench" \
      "${BUILD_DIR}/hixl_multi_instance_verify"

if [[ -z "${UCM_P2P_ROOT:-}" ]]; then
  UCM_P2P_ROOT="$("${PYTHON}" - <<'PY'
from importlib.metadata import PackageNotFoundError, distribution

try:
    package = distribution("uc-manager")
except PackageNotFoundError as error:
    raise SystemExit(
        "uc-manager is not installed; install a wheel containing the P2P transport first"
    ) from error

print(package.locate_file("ucm/transport/p2p"))
PY
)"
fi

UCM_P2P_LIBRARY="${UCM_P2P_ROOT}/libucm_p2p_transport.so"
UCM_P2P_INCLUDE="${UCM_P2P_ROOT}/include"
if [[ ! -f "${UCM_P2P_LIBRARY}" || ! -d "${UCM_P2P_INCLUDE}" ]]; then
  cat >&2 <<EOF
The installed uc-manager package does not contain the P2P transport.

Expected:
  ${UCM_P2P_LIBRARY}
  ${UCM_P2P_INCLUDE}

Install a wheel built with PLATFORM=ascend and BUILD_UCM_ASU=1, or set
UCM_P2P_ROOT to an installed ucm/transport/p2p directory.
EOF
  exit 1
fi

UCM_PACKAGE_ROOT="$(cd "${UCM_P2P_ROOT}/../.." && pwd)"

HIXL_HEADER_FOUND=0
INCLUDES=(
  "-I${UCM_P2P_INCLUDE}"
  "-I${UCM_P2P_INCLUDE}/one_sided"
  "-I${ROOT_DIR}/tests/e2e"
)

LIB_DIRS=()

add_include_dir() {
  local dir="$1"
  if [[ -d "${dir}" ]]; then
    INCLUDES+=("-I${dir}")
  fi
}

add_lib_dir() {
  local dir="$1"
  if [[ -d "${dir}" ]]; then
    LIB_DIRS+=("${dir}")
    LDFLAGS+=" -L${dir} -Wl,-rpath-link,${dir} -Wl,-rpath,${dir}"
  fi
}

add_lib_dir "${UCM_P2P_ROOT}"
while IFS= read -r dir; do
  add_lib_dir "${dir}"
done < <(find "${UCM_PACKAGE_ROOT}" -type f -name '*.so' -printf '%h\n' | sort -u)

add_hixl_include_dir() {
  local dir="$1"
  if [[ -f "${dir}/hixl/hixl.h" ]]; then
    INCLUDES+=("-I${dir}")
    HIXL_HEADER_FOUND=1
  fi
}

if [[ -n "${HIXL_ROOT:-}" ]]; then
  add_hixl_include_dir "${HIXL_ROOT}/include"
  add_hixl_include_dir "${HIXL_ROOT}"
  add_hixl_include_dir "${HIXL_ROOT}/aarch64-linux/include"
  add_lib_dir "${HIXL_ROOT}/lib"
  add_lib_dir "${HIXL_ROOT}/lib64"
  add_lib_dir "${HIXL_ROOT}/aarch64-linux/lib64"
fi
if [[ -n "${HIXL_INCLUDE_DIR:-}" ]]; then
  add_hixl_include_dir "${HIXL_INCLUDE_DIR}"
fi
if [[ -n "${HIXL_LIB_DIR:-}" ]]; then
  add_lib_dir "${HIXL_LIB_DIR}"
fi
ASCEND_INSTALL_ROOT="${ASCEND_HOME_PATH:-${ASCEND_TOOLKIT_HOME:-/usr/local/Ascend/ascend-toolkit/latest}}"
add_hixl_include_dir "${ASCEND_INSTALL_ROOT}/include"
add_hixl_include_dir "${ASCEND_INSTALL_ROOT}/aarch64-linux/include"
add_hixl_include_dir "${ASCEND_INSTALL_ROOT}/arm64-linux/include"
add_lib_dir "${ASCEND_INSTALL_ROOT}"
add_lib_dir "${ASCEND_INSTALL_ROOT}/lib64"
add_lib_dir "${ASCEND_INSTALL_ROOT}/aarch64-linux/lib64"
add_lib_dir "${ASCEND_INSTALL_ROOT}/arm64-linux/lib64"
if [[ -n "${ASCEND_INCLUDE_DIR:-}" ]]; then
  add_include_dir "${ASCEND_INCLUDE_DIR}"
fi
if [[ -n "${ASCEND_LIB_DIRS:-}" ]]; then
  IFS=':' read -r -a ascend_dirs <<< "${ASCEND_LIB_DIRS}"
  for dir in "${ascend_dirs[@]}"; do
    add_lib_dir "${dir}"
  done
fi
if [[ -n "${FMT_INCLUDE_DIR:-}" ]]; then
  add_include_dir "${FMT_INCLUDE_DIR}"
fi
if [[ -n "${SPDLOG_INCLUDE_DIR:-}" ]]; then
  add_include_dir "${SPDLOG_INCLUDE_DIR}"
fi
if [[ -n "${LOGGER_LIB_DIRS:-}" ]]; then
  IFS=':' read -r -a logger_dirs <<< "${LOGGER_LIB_DIRS}"
  for dir in "${logger_dirs[@]}"; do
    add_lib_dir "${dir}"
  done
fi

if [[ "${HIXL_HEADER_FOUND}" -eq 0 ]]; then
  for dir in \
    /usr/include \
    /usr/local/include \
    /usr/local/hixl/include \
    /opt/hixl/include; do
    add_hixl_include_dir "${dir}"
  done
fi

if [[ "${HIXL_HEADER_FOUND}" -eq 0 ]]; then
  cat >&2 <<'EOF'
Cannot find hixl/hixl.h.

Set one of:
  HIXL_ROOT=/path/to/hixl-install-prefix
  HIXL_ROOT=/path/to/hixl-source-root
  HIXL_INCLUDE_DIR=/path/to/directory-containing-hixl-folder
EOF
  exit 1
fi

COMMON_FLAGS=(
  -std=c++17
  -Wall
  -Wextra
  -Wpedantic
)

TESTS=(
  hixl_e2e
  hixl_multi_conn_bench
  hixl_multi_instance_verify
)

for test_name in "${TESTS[@]}"; do
  "${CXX}" "${COMMON_FLAGS[@]}" ${CXXFLAGS} "${INCLUDES[@]}" \
    "${ROOT_DIR}/tests/e2e/${test_name}.cpp" \
    -o "${BUILD_DIR}/${test_name}" \
    -L"${UCM_P2P_ROOT}" -lucm_p2p_transport \
    ${LDFLAGS} ${HIXL_LIBS} ${ASCEND_LIBS} ${LOGGER_LIBS} ${EXTRA_LIBS} -pthread
done

echo "Built:"
for test_name in "${TESTS[@]}"; do
  echo "  ${BUILD_DIR}/${test_name}"
done
echo "Using:"
echo "  ${UCM_P2P_LIBRARY}"


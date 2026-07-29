#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
P2P_DIR=$(cd -- "${SCRIPT_DIR}/.." && pwd)

CANN_ROOT=${CANN_ROOT:-/usr/local/Ascend/cann-9.0.0}
HIXL_ROOT=${HIXL_ROOT:-${CANN_ROOT}}
SERVER_DEVICE=0
CLIENT_DEVICE=1
HOST=127.0.0.1
SERVER_PORT=26666
CLIENT_PORT=27777
CHANNEL_COUNT=1
SERVER_EID=
CLIENT_EID=
BACKEND=adxl
TRANSFER_MODE=async
REQUEST_SIZE=4096
REQUEST_COUNT=16
BATCH_SIZE=1
INFLIGHT=
ROUNDS=100
OPERATION=write
BUFFER_POOL=
BUILD_DIR=${HIXL_RAW_STRESS_BUILD_DIR:-/tmp/hixl_raw_stress}

usage() {
    cat <<EOF
Usage: $0 [options]
  --server-device ID       server device (default: ${SERVER_DEVICE})
  --client-device ID       client device (default: ${CLIENT_DEVICE})
  --host IP                engine host (default: ${HOST})
  --server-port PORT       server engine port (default: ${SERVER_PORT})
  --client-port PORT       client engine port (default: ${CLIENT_PORT})
  --channel-count N        server engines connected by one client HIXL instance (default: 1)
  --server-eid EID         server device EID for the HixlEngine UB_CTP endpoint
  --client-eid EID         client device EID for the HixlEngine UB_CTP endpoint
  --backend NAME           adxl or hixl-engine (default: ${BACKEND})
  --transfer-mode MODE     sync or async (default: ${TRANSFER_MODE})
  --request-size BYTES     bytes per request (default: ${REQUEST_SIZE})
  --request-count N        total descriptors per round (default: ${REQUEST_COUNT})
  --batch-size N           descriptors per TransferAsync call (default: ${BATCH_SIZE})
  --inflight N             keep N TransferAsync calls active; refill completed/failed slots
  --rounds N               rounds (default: ${ROUNDS})
  --operation MODE         read, write, or alternate (default: ${OPERATION})
  --buffer-pool NUM:SIZE   HIXL BufferPool option, for example 4:8; unset by default
  --cann-root DIR          CANN root providing ACL (default: ${CANN_ROOT})
  --hixl-root DIR          HIXL install root (default: ${HIXL_ROOT})
  --build-dir DIR          binary and log directory (default: ${BUILD_DIR})

Only ACL and the public HIXL API are linked. UCM P2P is not compiled or loaded.
The server/client engine ports are also configured as their HCOMM device listen ports.
The test uses adxl.LocalCommRes.version=1.3 and UB_CTP device endpoints. This selects
the native HixlEngine backend and exercises the same-node HCCS path.
WRITE transfers pinned host memory on the client to device memory on the server.
READ transfers device memory on the server to pinned host memory on the client.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --server-device) SERVER_DEVICE=$2; shift 2 ;;
        --client-device) CLIENT_DEVICE=$2; shift 2 ;;
        --host) HOST=$2; shift 2 ;;
        --server-port) SERVER_PORT=$2; shift 2 ;;
        --client-port) CLIENT_PORT=$2; shift 2 ;;
        --channel-count) CHANNEL_COUNT=$2; shift 2 ;;
        --server-eid) SERVER_EID=$2; shift 2 ;;
        --client-eid) CLIENT_EID=$2; shift 2 ;;
        --backend) BACKEND=$2; shift 2 ;;
        --transfer-mode) TRANSFER_MODE=$2; shift 2 ;;
        --request-size) REQUEST_SIZE=$2; shift 2 ;;
        --request-count) REQUEST_COUNT=$2; shift 2 ;;
        --batch-size) BATCH_SIZE=$2; shift 2 ;;
        --inflight) INFLIGHT=$2; shift 2 ;;
        --rounds) ROUNDS=$2; shift 2 ;;
        --operation) OPERATION=$2; shift 2 ;;
        --buffer-pool) BUFFER_POOL=$2; shift 2 ;;
        --cann-root) CANN_ROOT=$2; shift 2 ;;
        --hixl-root) HIXL_ROOT=$2; shift 2 ;;
        --build-dir) BUILD_DIR=$2; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "ERROR: unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done

if ! [[ "${CHANNEL_COUNT}" =~ ^[1-9][0-9]*$ ]]; then
    echo "ERROR: --channel-count must be a positive integer" >&2
    exit 2
fi
if (( SERVER_PORT + CHANNEL_COUNT - 1 > 65535 )); then
    echo "ERROR: server channel port range exceeds 65535" >&2
    exit 2
fi
if (( CLIENT_PORT >= SERVER_PORT && CLIENT_PORT < SERVER_PORT + CHANNEL_COUNT )); then
    echo "ERROR: client port overlaps the server channel port range" >&2
    exit 2
fi
if [[ "${BACKEND}" != "adxl" && "${CHANNEL_COUNT}" -gt 1 ]]; then
    echo "ERROR: multi-channel testing currently requires --backend adxl" >&2
    exit 2
fi

if [[ "${BACKEND}" == "hixl-engine" && ( -z "${SERVER_EID}" || -z "${CLIENT_EID}" ) ]]; then
    echo "ERROR: --server-eid and --client-eid are required for HixlEngine UB_CTP testing" >&2
    exit 2
fi

case "${OPERATION}" in
    read|write|alternate) ;;
    *) echo "ERROR: --operation must be read, write, or alternate" >&2; exit 2 ;;
esac

find_library() {
    local root=$1
    local name=$2
    local candidate
    local result
    for candidate in \
        "${root}/aarch64-linux/lib64/${name}" \
        "${root}/lib64/${name}" \
        "${root}/lib/${name}"; do
        if [[ -f "${candidate}" ]]; then
            printf '%s\n' "${candidate}"
            return 0
        fi
    done
    result=$(find "${root}" -type f -name "${name}" -print -quit)
    [[ -n "${result}" ]] || return 1
    printf '%s\n' "${result}"
}

find_include_dir() {
    local root=$1
    local header=$2
    local candidate
    local result
    for candidate in "${root}/aarch64-linux/include" "${root}/include"; do
        if [[ -f "${candidate}/${header}" ]]; then
            printf '%s\n' "${candidate}"
            return 0
        fi
    done
    result=$(find "${root}" -type f -path "*/${header}" -print -quit)
    [[ -n "${result}" ]] || return 1
    dirname "$(dirname "${result}")"
}

CANN_INCLUDE=$(find_include_dir "${CANN_ROOT}" acl/acl.h) || {
    echo "ERROR: acl/acl.h not found under ${CANN_ROOT}" >&2
    exit 1
}
HIXL_INCLUDE=$(find_include_dir "${HIXL_ROOT}" hixl/hixl.h) || {
    echo "ERROR: hixl/hixl.h not found under ${HIXL_ROOT}" >&2
    exit 1
}
CANN_ACL_SO=$(find_library "${CANN_ROOT}" libascendcl.so) || {
    echo "ERROR: libascendcl.so not found under ${CANN_ROOT}" >&2
    exit 1
}
CANN_METADEF_SO=$(find_library "${CANN_ROOT}" libmetadef.so) || {
    echo "ERROR: libmetadef.so not found under ${CANN_ROOT}" >&2
    exit 1
}
HIXL_SO=$(find_library "${HIXL_ROOT}" libcann_hixl.so) || {
    echo "ERROR: libcann_hixl.so not found under ${HIXL_ROOT}" >&2
    exit 1
}
CANN_LIB=$(dirname "${CANN_ACL_SO}")
HIXL_LIB=$(dirname "${HIXL_SO}")

mkdir -p "${BUILD_DIR}"
STATE_DIR=$(mktemp -d "${BUILD_DIR}/state.XXXXXX")
BINARY="${BUILD_DIR}/hixl_raw_stress"
CLIENT_LOG="${BUILD_DIR}/client.log"
SERVER_PIDS=()
SERVER_LOGS=()

cleanup() {
    local status=$?
    local pid
    for pid in "${SERVER_PIDS[@]}"; do
        if kill -0 "${pid}" 2>/dev/null; then kill "${pid}" 2>/dev/null || true; fi
        wait "${pid}" 2>/dev/null || true
    done
    rm -rf -- "${STATE_DIR}"
    exit "${status}"
}
trap cleanup EXIT INT TERM

echo "CANN_INCLUDE=${CANN_INCLUDE}"
echo "HIXL_INCLUDE=${HIXL_INCLUDE}"
echo "CANN_ACL_SO=${CANN_ACL_SO}"
echo "CANN_METADEF_SO=${CANN_METADEF_SO}"
echo "HIXL_SO=${HIXL_SO}"

set -x
g++ -std=c++17 -O2 -pthread -D_GLIBCXX_USE_CXX11_ABI=0 \
    -I"${HIXL_INCLUDE}" \
    -I"${CANN_INCLUDE}" \
    "${P2P_DIR}/tests/e2e/hixl_raw_stress.cpp" \
    -Wl,-rpath,"${HIXL_LIB}" \
    -Wl,-rpath,"${CANN_LIB}" \
    -Wl,-rpath-link,"${HIXL_LIB}" \
    -Wl,-rpath-link,"${CANN_LIB}" \
    -Wl,--no-as-needed \
    "${HIXL_SO}" \
    "${CANN_METADEF_SO}" \
    "${CANN_ACL_SO}" \
    -Wl,--as-needed \
    -ldl \
    -o "${BINARY}"
set +x

export LD_LIBRARY_PATH="${HIXL_LIB}:${CANN_LIB}:${LD_LIBRARY_PATH:-}"

echo "Resolved runtime libraries:"
LDD_OUTPUT=$(ldd "${BINARY}")
printf '%s\n' "${LDD_OUTPUT}" | grep -E 'cann_hixl|metadef|ascendcl|hcomm|not found' || true
if printf '%s\n' "${LDD_OUTPUT}" | grep -Fq 'not found'; then
    echo "ERROR: ${BINARY} has unresolved runtime libraries" >&2
    exit 1
fi
if ! printf '%s\n' "${LDD_OUTPUT}" | grep -Fq "${HIXL_SO}"; then
    echo "ERROR: ${BINARY} does not resolve libcann_hixl.so to ${HIXL_SO}" >&2
    exit 1
fi

INFLIGHT_ARGS=()
if [[ -n "${INFLIGHT}" ]]; then
    INFLIGHT_ARGS=(--inflight "${INFLIGHT}")
fi
BUFFER_POOL_ARGS=()
if [[ -n "${BUFFER_POOL}" ]]; then
    BUFFER_POOL_ARGS=(--buffer-pool "${BUFFER_POOL}")
fi

batch_count=$(( (REQUEST_COUNT + BATCH_SIZE - 1) / BATCH_SIZE ))
server_request_count=$(( ((batch_count + CHANNEL_COUNT - 1) / CHANNEL_COUNT) * BATCH_SIZE ))
REMOTE_ENGINES=()
for ((channel = 0; channel < CHANNEL_COUNT; ++channel)); do
    server_port=$((SERVER_PORT + channel))
    channel_state="${STATE_DIR}/channel-${channel}"
    server_log="${BUILD_DIR}/server-${channel}.log"
    mkdir -p "${channel_state}"
    REMOTE_ENGINES+=("${HOST}:${server_port}")
    SERVER_LOGS+=("${server_log}")
    echo "Starting server channel=${channel}: device=${SERVER_DEVICE}, engine=${HOST}:${server_port}"
    "${BINARY}" \
        --role server \
        --device "${SERVER_DEVICE}" \
        --local-engine "${HOST}:${server_port}" \
        --device-port "${server_port}" \
        --backend "${BACKEND}" \
        --transfer-mode "${TRANSFER_MODE}" \
        --local-eid "${SERVER_EID}" \
        --remote-eid "${CLIENT_EID}" \
        --state-dir "${channel_state}" \
        --request-size "${REQUEST_SIZE}" \
        --request-count "${server_request_count}" \
        --batch-size "${BATCH_SIZE}" \
        "${INFLIGHT_ARGS[@]}" \
        "${BUFFER_POOL_ARGS[@]}" \
        --rounds "${ROUNDS}" \
        > >(tee "${server_log}") 2>&1 &
    SERVER_PIDS+=("$!")
done

for ((channel = 0; channel < CHANNEL_COUNT; ++channel)); do
    while [[ ! -s "${STATE_DIR}/channel-${channel}/server.addr" ]]; do
        if ! kill -0 "${SERVER_PIDS[channel]}" 2>/dev/null; then
            wait "${SERVER_PIDS[channel]}" || true
            echo "FAILED: server channel=${channel} exited before publishing its address" >&2
            echo "Log: ${SERVER_LOGS[channel]}" >&2
            exit 1
        fi
        sleep 0.01
    done
done

REMOTE_ENGINE_CSV=$(IFS=,; echo "${REMOTE_ENGINES[*]}")

echo "Starting client: device=${CLIENT_DEVICE}, local=${HOST}:${CLIENT_PORT}, remotes=${REMOTE_ENGINE_CSV}"
echo "Test workload: rounds=${ROUNDS}, channels=${CHANNEL_COUNT}, descriptors/round=${REQUEST_COUNT}, descriptors/batch=${BATCH_SIZE}, inflight=${INFLIGHT:-all}, bytes/descriptor=${REQUEST_SIZE}, operation=${OPERATION}"
set +e
"${BINARY}" \
    --role client \
    --device "${CLIENT_DEVICE}" \
    --local-engine "${HOST}:${CLIENT_PORT}" \
    --remote-engines "${REMOTE_ENGINE_CSV}" \
    --device-port "${CLIENT_PORT}" \
    --backend "${BACKEND}" \
    --transfer-mode "${TRANSFER_MODE}" \
    --local-eid "${CLIENT_EID}" \
    --remote-eid "${SERVER_EID}" \
    --state-dir "${STATE_DIR}" \
    --request-size "${REQUEST_SIZE}" \
    --request-count "${REQUEST_COUNT}" \
    --batch-size "${BATCH_SIZE}" \
    "${INFLIGHT_ARGS[@]}" \
    "${BUFFER_POOL_ARGS[@]}" \
    --rounds "${ROUNDS}" \
    --operation "${OPERATION}" \
    > >(tee "${CLIENT_LOG}") 2>&1
CLIENT_STATUS=$?
set -e

if [[ ${CLIENT_STATUS} -ne 0 ]]; then
    for ((channel = 0; channel < CHANNEL_COUNT; ++channel)); do
        : > "${STATE_DIR}/channel-${channel}/client.failed"
    done
fi

SERVER_STATUS=0
set +e
for pid in "${SERVER_PIDS[@]}"; do
    wait "${pid}"
    status=$?
    if [[ ${status} -ne 0 ]]; then SERVER_STATUS=${status}; fi
done
set -e
SERVER_PIDS=()

if [[ ${CLIENT_STATUS} -ne 0 || ${SERVER_STATUS} -ne 0 ]]; then
    echo "FAILED: client=${CLIENT_STATUS}, server=${SERVER_STATUS}" >&2
    echo "Logs: ${CLIENT_LOG} ${SERVER_LOGS[*]}" >&2
    exit 1
fi

echo "PASS: rounds=${ROUNDS}, channels=${CHANNEL_COUNT}, descriptors/round=${REQUEST_COUNT}, descriptors/batch=${BATCH_SIZE}, inflight=${INFLIGHT:-all}, bytes/descriptor=${REQUEST_SIZE}, operation=${OPERATION}"
echo "Logs: ${CLIENT_LOG} ${SERVER_LOGS[*]}"

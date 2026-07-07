#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"
BIN="${BIN:-${BUILD_DIR}/hixl_multi_conn_bench}"

if [[ ! -x "${BIN}" ]]; then
  cat >&2 <<EOF
Cannot execute ${BIN}.

Build it first:
  bash ${ROOT_DIR}/scripts/build_tests.sh
EOF
  exit 1
fi

SERVER_LOG="${SERVER_LOG:-${BUILD_DIR}/hixl_multi_conn_bench_server.log}"
CLIENT_LOG="${CLIENT_LOG:-${BUILD_DIR}/hixl_multi_conn_bench_client.log}"
ROLE="${HIXL_BENCH_ROLE:-local}"
ARGS=()

while [[ "$#" -gt 0 ]]; do
  case "$1" in
    server|client|local)
      ROLE="$1"
      shift
      ;;
    --role)
      if [[ "$#" -lt 2 ]]; then
        echo "missing value for --role" >&2
        exit 1
      fi
      ROLE="$2"
      shift 2
      ;;
    --role=*)
      ROLE="${1#--role=}"
      shift
      ;;
    *)
      ARGS+=("$1")
      shift
      ;;
  esac
done

case "${ROLE}" in
  local|server|client) ;;
  *)
    echo "role must be local, server or client" >&2
    exit 1
    ;;
esac

server_pids=()
cleanup() {
  for pid in "${server_pids[@]}"; do
    if kill -0 "${pid}" 2>/dev/null; then
      kill "${pid}" 2>/dev/null || true
      wait "${pid}" 2>/dev/null || true
    fi
  done
}
trap cleanup EXIT

arg_value() {
  local key="$1"
  local fallback="$2"
  local index=0
  while [[ "${index}" -lt "${#ARGS[@]}" ]]; do
    local arg="${ARGS[${index}]}"
    if [[ "${arg}" == "--${key}" && "$((index + 1))" -lt "${#ARGS[@]}" ]]; then
      fallback="${ARGS[$((index + 1))]}"
      index=$((index + 2))
      continue
    fi
    if [[ "${arg}" == "--${key}="* ]]; then
      fallback="${arg#--${key}=}"
    fi
    index=$((index + 1))
  done
  echo "${fallback}"
}

server_worker_log() {
  local index="$1"
  if [[ "${SERVER_LOG}" == *.* ]]; then
    echo "${SERVER_LOG%.*}_${index}.${SERVER_LOG##*.}"
  else
    echo "${SERVER_LOG}_${index}"
  fi
}

server_devices_text="$(arg_value server-devices "${HIXL_BENCH_SERVER_DEVICES:-}")"
connections="$(arg_value connections "${HIXL_BENCH_CONNECTIONS:-1}")"
server_device="$(arg_value server-device "${HIXL_BENCH_DEVICE_A:-${HIXL_TEST_DEVICE_A:-0}}")"
server_control_port="$(arg_value server-control-port "${HIXL_BENCH_CONTROL_PORT_A:-${TRANSPORT_CONTROL_PORT_A:-4601}}")"
server_hixl_port="$(arg_value server-hixl-port "${HIXL_BENCH_HIXL_PORT_A:-${HIXL_TEST_PORT_A:-5501}}")"

server_devices=()
if [[ -n "${server_devices_text}" ]]; then
  IFS=',' read -r -a server_devices <<<"${server_devices_text}"
  connections="${#server_devices[@]}"
else
  for ((i = 0; i < connections; ++i)); do
    server_devices+=("$((server_device + i))")
  done
fi

start_server_workers() {
  for ((i = 0; i < connections; ++i)); do
    local worker_log
    worker_log="$(server_worker_log "${i}")"
    "${BIN}" server "${ARGS[@]}" \
      --connections 1 \
      --server-devices "${server_devices[${i}]}" \
      --server-control-port "$((server_control_port + i))" \
      --server-hixl-port "$((server_hixl_port + i))" \
      >"${worker_log}" 2>&1 &
    server_pids+=("$!")
  done
}

if [[ "${ROLE}" == "server" ]]; then
  start_server_workers
  status=0
  for pid in "${server_pids[@]}"; do
    if ! wait "${pid}"; then
      status=1
    fi
  done
  server_pids=()
  for ((i = 0; i < connections; ++i)); do
    echo "server_log_${i}=$(server_worker_log "${i}")"
  done
  exit "${status}"
fi

if [[ "${ROLE}" == "client" ]]; then
  "${BIN}" client "${ARGS[@]}" 2>&1 | tee "${CLIENT_LOG}"
  exit "${PIPESTATUS[0]}"
fi

start_server_workers

sleep "${SERVER_START_DELAY_SEC:-2}"

set +e
"${BIN}" client "${ARGS[@]}" 2>&1 | tee "${CLIENT_LOG}"
client_status="${PIPESTATUS[0]}"
set -e

if [[ "${client_status}" -ne 0 ]]; then
  cleanup
  for ((i = 0; i < connections; ++i)); do
    echo "server_log_${i}=$(server_worker_log "${i}")"
  done
  echo "client_log=${CLIENT_LOG}"
  echo "client failed with status ${client_status}" >&2
  exit "${client_status}"
fi

server_status=0
for pid in "${server_pids[@]}"; do
  if ! wait "${pid}"; then
    server_status=1
  fi
done
server_pids=()

for ((i = 0; i < connections; ++i)); do
  echo "server_log_${i}=$(server_worker_log "${i}")"
done
echo "client_log=${CLIENT_LOG}"

if [[ "${server_status}" -ne 0 ]]; then
  echo "server failed with status ${server_status}" >&2
  exit "${server_status}"
fi

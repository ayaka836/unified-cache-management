#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"
BIN="${BIN:-${BUILD_DIR}/hixl_multi_instance_verify}"

if [[ ! -x "${BIN}" ]]; then
  cat >&2 <<EOF
Cannot execute ${BIN}.

Build it first:
  bash ${ROOT_DIR}/scripts/build_tests.sh
EOF
  exit 1
fi

LOG="${HIXL_VERIFY_LOG:-${BUILD_DIR}/hixl_multi_instance_verify.log}"
ROLE="${HIXL_VERIFY_ROLE:-}"
ARGS=()

while [[ "$#" -gt 0 ]]; do
  case "$1" in
    server|client)
      ROLE="$1"
      ARGS+=("$1")
      shift
      ;;
    --role)
      if [[ "$#" -lt 2 ]]; then
        echo "missing value for --role" >&2
        exit 1
      fi
      ROLE="$2"
      ARGS+=("$1" "$2")
      shift 2
      ;;
    --role=*)
      ROLE="${1#--role=}"
      ARGS+=("$1")
      shift
      ;;
    *)
      ARGS+=("$1")
      shift
      ;;
  esac
done

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

worker_log() {
  local index="$1"
  if [[ "${LOG}" == *.* ]]; then
    echo "${LOG%.*}_${index}.${LOG##*.}"
  else
    echo "${LOG}_${index}"
  fi
}

cleanup() {
  for pid in "${worker_pids[@]:-}"; do
    if kill -0 "${pid}" 2>/dev/null; then
      kill "${pid}" 2>/dev/null || true
      wait "${pid}" 2>/dev/null || true
    fi
  done
}
trap cleanup EXIT

if [[ "${ROLE}" == "server" ]]; then
  devices_text="$(arg_value devices "${HIXL_VERIFY_DEVICES:-}")"
  instances="$(arg_value instances "${HIXL_VERIFY_INSTANCES:-1}")"
  base_device=0
  devices=()
  if [[ -n "${devices_text}" ]]; then
    IFS=',' read -r -a devices <<<"${devices_text}"
    instances="${#devices[@]}"
  else
    for ((i = 0; i < instances; ++i)); do
      devices+=("$((base_device + i))")
    done
  fi

  local_control_port="$(arg_value local-control-port "${HIXL_VERIFY_CONTROL_PORT:-4701}")"
  hixl_port="$(arg_value hixl-port "${HIXL_VERIFY_HIXL_PORT:-5701}")"
  worker_pids=()
  for ((i = 0; i < instances; ++i)); do
    log_file="$(worker_log "${i}")"
    control_port="$((local_control_port + i))"
    engine_port="$((hixl_port + i))"
    echo "start_worker_${i} device=${devices[${i}]} control_port=${control_port} hixl_port=${engine_port} log=${log_file}" >&2
    "${BIN}" "${ARGS[@]}" \
      --role server \
      --instances 1 \
      --devices "${devices[${i}]}" \
      --local-control-port "${control_port}" \
      --hixl-port "${engine_port}" \
      >"${log_file}" 2>&1 &
    pid="$!"
    worker_pids+=("${pid}")
    echo "worker_${i}_pid=${pid}" >&2
  done

  status=0
  for ((i = 0; i < instances; ++i)); do
    pid="${worker_pids[${i}]}"
    set +e
    wait "${pid}"
    worker_status="$?"
    set -e
    if [[ "${worker_status}" -ne 0 ]]; then
      echo "worker_${i}_failed status=${worker_status} log=$(worker_log "${i}")" >&2
      tail -80 "$(worker_log "${i}")" >&2 || true
      status=1
    else
      echo "worker_${i}_done log=$(worker_log "${i}")" >&2
    fi
  done
  worker_pids=()
  for ((i = 0; i < instances; ++i)); do
    echo "worker_log_${i}=$(worker_log "${i}")"
  done
  exit "${status}"
fi

"${BIN}" "${ARGS[@]}" 2>&1 | tee "${LOG}"
exit "${PIPESTATUS[0]}"

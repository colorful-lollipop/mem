#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR="${BUILD_DIR:-build_stress}"
OUT_DIR="${OUT_DIR:-stress_out}"
SAMPLE_INTERVAL_SEC="${SAMPLE_INTERVAL_SEC:-5}"

RUNNER="${BUILD_DIR}/tests/apps/minirpc/memrpc_minirpc_stress_runner"
if [[ ! -x "${RUNNER}" ]]; then
  echo "stress runner not found: ${RUNNER}"
  exit 1
fi

mkdir -p "${OUT_DIR}"
PID_FILE="${OUT_DIR}/pids.txt"
LOG_FILE="${OUT_DIR}/stress.log"
RSS_CSV="${OUT_DIR}/rss.csv"

: > "${RSS_CSV}"
echo "timestamp,parent_rss_kb,child_rss_kb" >> "${RSS_CSV}"

MEMRPC_STRESS_PID_FILE="${PID_FILE}" "${RUNNER}" > "${LOG_FILE}" 2>&1 &
RUNNER_PID=$!

for _ in $(seq 1 50); do
  if [[ -f "${PID_FILE}" ]]; then
    break
  fi
  sleep 0.1
done

CHILD_PID="$(grep -m1 '^child_pid=' "${PID_FILE}" | cut -d= -f2 || true)"

while kill -0 "${RUNNER_PID}" 2>/dev/null; do
  TS=$(date +%s)
  PARENT_RSS=$(grep -m1 '^VmRSS:' "/proc/${RUNNER_PID}/status" 2>/dev/null | awk '{print $2}' || echo 0)
  CHILD_RSS=0
  if [[ -n "${CHILD_PID}" && -e "/proc/${CHILD_PID}/status" ]]; then
    CHILD_RSS=$(grep -m1 '^VmRSS:' "/proc/${CHILD_PID}/status" 2>/dev/null | awk '{print $2}' || echo 0)
  fi
  echo "${TS},${PARENT_RSS},${CHILD_RSS}" >> "${RSS_CSV}"
  sleep "${SAMPLE_INTERVAL_SEC}"
done

wait "${RUNNER_PID}"
EXIT_CODE=$?

echo "exit_code=${EXIT_CODE}"
exit "${EXIT_CODE}"

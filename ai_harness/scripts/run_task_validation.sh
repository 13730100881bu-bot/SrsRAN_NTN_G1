#!/usr/bin/env bash
set -euo pipefail

TASK_ID="${TASK_ID:-}"
TASK_FILE="${TASK_FILE:-}"
BASE="${BASE:-ai/cucp-harness-base}"
BUILD_DIR="${BUILD_DIR:-build/ai-clean}"
CTEST_REGEX="${CTEST_REGEX:-}"
SKIP_BUILD="${SKIP_BUILD:-0}"

if [[ -z "$TASK_ID" ]]; then
  echo "TASK_ID is required" >&2
  exit 2
fi
if [[ -z "$TASK_FILE" ]]; then
  echo "TASK_FILE is required" >&2
  exit 2
fi

timestamp="$(date +%Y%m%d-%H%M%S)"
result_dir="ai_harness/results/${TASK_ID}-${timestamp}"
mkdir -p "$result_dir"

run_logged() {
  local name="$1"
  shift
  echo "Running ${name}..."
  "$@" 2>&1 | tee "${result_dir}/${name}.log"
}

run_logged path_guard \
  python3 ai_harness/scripts/guard_changed_paths.py --base "$BASE" --task-file "$TASK_FILE"

run_logged rejected_overlap \
  python3 ai_harness/scripts/check_rejected_overlap.py --base "$BASE"

run_logged task_metadata \
  python3 ai_harness/scripts/validate_task_metadata.py "$TASK_FILE"

if [[ "$SKIP_BUILD" != "1" ]]; then
  export BUILD_DIR
  run_logged configure bash ai_harness/scripts/configure_build.sh
  run_logged build bash ai_harness/scripts/build.sh
  if [[ -n "$CTEST_REGEX" ]]; then
    run_logged ctest_focused ctest --test-dir "$BUILD_DIR" --output-on-failure -R "$CTEST_REGEX"
  fi
  run_logged cucp_tests bash ai_harness/scripts/run_cucp_tests.sh
fi

cat >"${result_dir}/run_summary.md" <<EOF
# ${TASK_ID}-${timestamp}

## Base

\`${BASE}\`

## Task

\`${TASK_FILE}\`

## Validation results

- Path guard: see \`path_guard.log\`
- Rejected overlap: see \`rejected_overlap.log\`
- Task metadata: see \`task_metadata.log\`
- Build skipped: ${SKIP_BUILD}

EOF

echo "Task validation complete. Results: ${result_dir}"

#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT_DIR"

ENGINE="./engine"
RESULT_DIR="./experiments"
RESULT_CSV="$RESULT_DIR/task5_results.csv"
SUPERVISOR_LOG="$RESULT_DIR/supervisor.log"

ROOTFS_BASE=""
ROOTFS_E1_A="./rootfs-task5-e1-a"
ROOTFS_E1_B="./rootfs-task5-e1-b"
ROOTFS_E2_A="./rootfs-task5-e2-a"
ROOTFS_E2_B="./rootfs-task5-e2-b"

SUP_PID=""

require_root() {
    if [[ "$EUID" -ne 0 ]]; then
        echo "This script must run as root. Use: sudo ./task5_experiments.sh"
        exit 1
    fi
}

require_prereqs() {
    if [[ ! -x "$ENGINE" ]]; then
        echo "Missing executable: $ENGINE"
        echo "Run: make"
        exit 1
    fi

    if [[ -d ./rootfs-base ]]; then
        ROOTFS_BASE="./rootfs-base"
    elif [[ -d ./rootfs ]]; then
        ROOTFS_BASE="./rootfs"
    else
        echo "Missing rootfs template. Expected ./rootfs-base or ./rootfs"
        exit 1
    fi

    if [[ ! -x ./cpu_hog || ! -x ./io_pulse ]]; then
        echo "Missing workload binaries. Run: make"
        exit 1
    fi
}

prepare_rootfs() {
    local dst="$1"
    rm -rf "$dst"
    cp -a "$ROOTFS_BASE" "$dst"
    cp -f ./cpu_hog "$dst/cpu_hog"
    cp -f ./io_pulse "$dst/io_pulse"
    chmod +x "$dst/cpu_hog" "$dst/io_pulse"
}

start_supervisor() {
    mkdir -p "$RESULT_DIR"
    rm -f /tmp/mini_runtime.sock

    "$ENGINE" supervisor "$ROOTFS_BASE" >"$SUPERVISOR_LOG" 2>&1 &
    SUP_PID="$!"

    for _ in $(seq 1 50); do
        if [[ -S /tmp/mini_runtime.sock ]]; then
            return 0
        fi
        sleep 0.1
    done

    echo "Supervisor did not start. See $SUPERVISOR_LOG"
    exit 1
}

stop_supervisor() {
    if [[ -n "$SUP_PID" ]] && kill -0 "$SUP_PID" 2>/dev/null; then
        kill -TERM "$SUP_PID" || true
        wait "$SUP_PID" 2>/dev/null || true
    fi
}

cleanup() {
    "$ENGINE" stop e1_high >/dev/null 2>&1 || true
    "$ENGINE" stop e1_low >/dev/null 2>&1 || true
    "$ENGINE" stop e2_cpu >/dev/null 2>&1 || true
    "$ENGINE" stop e2_io >/dev/null 2>&1 || true
    stop_supervisor
}

container_state() {
    local id="$1"
    "$ENGINE" ps | awk -v target="$id" '$1 == target { print $3; exit }'
}

wait_for_finish_ms() {
    local id="$1"
    local start_ms="$2"

    while true; do
        local state
        state="$(container_state "$id")"

        if [[ -z "$state" ]]; then
            sleep 0.1
            continue
        fi

        if [[ "$state" != "running" && "$state" != "starting" ]]; then
            local end_ms
            end_ms="$(date +%s%3N)"
            echo $((end_ms - start_ms))
            return 0
        fi

        sleep 0.1
    done
}

run_experiment_cpu_vs_cpu_priority() {
    local start_ms high_ms low_ms

    prepare_rootfs "$ROOTFS_E1_A"
    prepare_rootfs "$ROOTFS_E1_B"

    "$ENGINE" start e1_high "$ROOTFS_E1_A" "/cpu_hog 12" --nice -5 >/dev/null
    "$ENGINE" start e1_low "$ROOTFS_E1_B" "/cpu_hog 12" --nice 10 >/dev/null

    start_ms="$(date +%s%3N)"
    high_ms="$(wait_for_finish_ms e1_high "$start_ms")"
    low_ms="$(wait_for_finish_ms e1_low "$start_ms")"

    echo "exp1_cpu_vs_cpu_priority,e1_high,-5,cpu_hog,$high_ms" >>"$RESULT_CSV"
    echo "exp1_cpu_vs_cpu_priority,e1_low,10,cpu_hog,$low_ms" >>"$RESULT_CSV"
}

run_experiment_cpu_vs_io_same_priority() {
    local start_ms cpu_ms io_ms

    prepare_rootfs "$ROOTFS_E2_A"
    prepare_rootfs "$ROOTFS_E2_B"

    "$ENGINE" start e2_cpu "$ROOTFS_E2_A" "/cpu_hog 12" --nice 0 >/dev/null
    "$ENGINE" start e2_io "$ROOTFS_E2_B" "/io_pulse 40 100" --nice 0 >/dev/null

    start_ms="$(date +%s%3N)"
    cpu_ms="$(wait_for_finish_ms e2_cpu "$start_ms")"
    io_ms="$(wait_for_finish_ms e2_io "$start_ms")"

    echo "exp2_cpu_vs_io_same_priority,e2_cpu,0,cpu_hog,$cpu_ms" >>"$RESULT_CSV"
    echo "exp2_cpu_vs_io_same_priority,e2_io,0,io_pulse,$io_ms" >>"$RESULT_CSV"
}

main() {
    require_root
    require_prereqs

    trap cleanup EXIT

    rm -rf "$RESULT_DIR"
    mkdir -p "$RESULT_DIR"
    : >"$RESULT_CSV"
    echo "experiment,container_id,nice,workload,completion_ms" >>"$RESULT_CSV"

    start_supervisor

    run_experiment_cpu_vs_cpu_priority
    run_experiment_cpu_vs_io_same_priority

    echo "Task 5 experiments complete. Results: $RESULT_CSV"
    echo "Sample logs:"
    echo "  logs/e1_high.log"
    echo "  logs/e1_low.log"
    echo "  logs/e2_cpu.log"
    echo "  logs/e2_io.log"
}

main "$@"

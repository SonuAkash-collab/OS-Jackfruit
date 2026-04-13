#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT_DIR"

ENGINE="./engine"
ROOTFS_BASE=""
ROOTFS_A="./rootfs-task6-a"
ROOTFS_B="./rootfs-task6-b"

SUP_PID=""

cleanup() {
    "$ENGINE" stop t6a >/dev/null 2>&1 || true
    "$ENGINE" stop t6b >/dev/null 2>&1 || true
    if [[ -n "$SUP_PID" ]] && kill -0 "$SUP_PID" 2>/dev/null; then
        kill -TERM "$SUP_PID" || true
        wait "$SUP_PID" 2>/dev/null || true
    fi
}

require_root() {
    if [[ "$EUID" -ne 0 ]]; then
        echo "Run as root: sudo ./task6_cleanup_check.sh"
        exit 1
    fi
}

prepare_rootfs() {
    local dst="$1"
    rm -rf "$dst"
    cp -a "$ROOTFS_BASE" "$dst"
    cp -f ./cpu_hog "$dst/cpu_hog"
    chmod +x "$dst/cpu_hog"
}

wait_for_socket() {
    for _ in $(seq 1 50); do
        [[ -S /tmp/mini_runtime.sock ]] && return 0
        sleep 0.1
    done
    return 1
}

start_supervisor() {
    rm -f /tmp/mini_runtime.sock
    "$ENGINE" supervisor "$ROOTFS_BASE" >/tmp/task6_supervisor.log 2>&1 &
    SUP_PID="$!"
    wait_for_socket
}

print_zombie_check() {
    local zombies
    zombies="$(ps -eo stat,ppid,pid,comm | awk '$1 ~ /^Z/ {print}' || true)"
    if [[ -n "$zombies" ]]; then
        echo "Zombie processes detected:"
        echo "$zombies"
        return 1
    fi
    echo "No zombie processes detected."
}

main() {
    require_root
    if [[ ! -x "$ENGINE" || ! -x ./cpu_hog ]]; then
        echo "Build first with: make"
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

    trap cleanup EXIT

    prepare_rootfs "$ROOTFS_A"
    prepare_rootfs "$ROOTFS_B"

    start_supervisor

    "$ENGINE" start t6a "$ROOTFS_A" "/cpu_hog 6" >/dev/null
    "$ENGINE" start t6b "$ROOTFS_B" "/cpu_hog 6" >/dev/null

    sleep 1
    "$ENGINE" stop t6a >/dev/null || true
    "$ENGINE" stop t6b >/dev/null || true

    # Give supervisor time to reap and flush log pipeline.
    sleep 2

    echo "Container table after stop:" 
    "$ENGINE" ps || true

    cleanup
    SUP_PID=""

    print_zombie_check

    echo "Socket present after supervisor exit?"
    if [[ -S /tmp/mini_runtime.sock ]]; then
        echo "Unexpected: /tmp/mini_runtime.sock still exists"
        exit 1
    fi
    echo "OK: control socket cleaned up"

    echo "Task 6 cleanup check passed."
}

main "$@"

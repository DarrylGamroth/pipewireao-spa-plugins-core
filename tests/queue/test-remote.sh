#!/bin/sh
set -eu

client=$1
daemon=$2
config=$3
queue_module_dir=$4
remote_name=pipewireao-queue-remote-test
runtime_dir=$(mktemp -d "${TMPDIR:-/tmp}/pipewireao-queue-remote.XXXXXX")
daemon_pid=

cleanup() {
    if [ -n "$daemon_pid" ]; then
        kill "$daemon_pid" 2>/dev/null || true
        wait "$daemon_pid" 2>/dev/null || true
    fi
    rm -r -- "$runtime_dir"
}
trap cleanup EXIT HUP INT TERM

export PIPEWIRE_RUNTIME_DIR=$runtime_dir
export PIPEWIREAO_RUNTIME_DIR=$runtime_dir
export XDG_RUNTIME_DIR=$runtime_dir
export PIPEWIREAO_MODULE_DIR=$queue_module_dir${PIPEWIREAO_MODULE_DIR:+:$PIPEWIREAO_MODULE_DIR}

"$daemon" -c "$config" >"$runtime_dir/daemon.log" 2>&1 &
daemon_pid=$!

attempt=0
while [ ! -S "$runtime_dir/$remote_name" ]; do
    if ! kill -0 "$daemon_pid" 2>/dev/null; then
        cat "$runtime_dir/daemon.log" >&2
        exit 1
    fi
    attempt=$((attempt + 1))
    if [ "$attempt" -ge 500 ]; then
        cat "$runtime_dir/daemon.log" >&2
        exit 1
    fi
    sleep 0.01
done

if ! "$client" "$remote_name"; then
    cat "$runtime_dir/daemon.log" >&2
    exit 1
fi

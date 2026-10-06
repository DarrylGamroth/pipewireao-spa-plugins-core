#!/bin/sh
set -eu

client=$1
daemon=$2
config=$3
queue_module_dir=$4
shift 4
remote_name=pipewireao-queue-remote-test
runtime_dir=$(mktemp -d "${TMPDIR:-/tmp}/pipewireao-queue-remote.XXXXXX")
daemon_pid=
if [ "${1:-}" = "--same-loop" ]; then
    shift
    sed 's/node.loop.name = queue-playback/node.loop.name = queue-capture/' \
        "$config" >"$runtime_dir/shared-loop.conf"
    config=$runtime_dir/shared-loop.conf
fi

cleanup() {
    result=$?
    trap - EXIT HUP INT TERM
    if [ -n "$daemon_pid" ]; then
        kill "$daemon_pid" 2>/dev/null || true
        daemon_status=0
        wait "$daemon_pid" 2>/dev/null || daemon_status=$?
        echo "daemon-stopped pid=$daemon_pid status=$daemon_status"
        if [ "$daemon_status" -ne 0 ]; then
            result=1
        fi
    fi
    cat "$runtime_dir/daemon.log"
    rm -r -- "$runtime_dir"
    exit "$result"
}
trap cleanup EXIT HUP INT TERM

export PIPEWIRE_RUNTIME_DIR=$runtime_dir
export XDG_RUNTIME_DIR=$runtime_dir
export PIPEWIREAO_MODULE_DIR=$queue_module_dir${PIPEWIREAO_MODULE_DIR:+:$PIPEWIREAO_MODULE_DIR}

if [ -n "${PIPEWIREAO_QUEUE_TEST_PRELOAD:-}" ]; then
    LD_PRELOAD=$PIPEWIREAO_QUEUE_TEST_PRELOAD "$daemon" -c "$config" >"$runtime_dir/daemon.log" 2>&1 &
else
    "$daemon" -c "$config" >"$runtime_dir/daemon.log" 2>&1 &
fi
daemon_pid=$!
echo "daemon-started pid=$daemon_pid"
sed -n '/Cpus_allowed_list/p' "/proc/$daemon_pid/status"

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

if ! "$client" "$remote_name" "$@"; then
    cat "$runtime_dir/daemon.log" >&2
    exit 1
fi

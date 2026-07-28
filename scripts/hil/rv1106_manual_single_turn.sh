#!/bin/sh
# Run one bounded manual audio turn on RV1106 while preserving an existing
# supervised voice loop. This script is intentionally board-side and narrow:
# it validates exact process identities, releases ALSA, runs one client, and
# resumes the same supervisor on every exit path.

set -eu
LC_ALL=C
export LC_ALL

SUPERVISOR_PID=
SUPERVISOR_START=
SUPERVISOR_STOPPED=false
HIL_CLIENT_PID=
HIL_CLIENT_START=
HIL_CLIENT_PPID=
HIL_CLIENT_EXE=
HIL_CLIENT_CMDLINE=
HIL_CLIENT_CMDLINE_SHA256=
HIL_CLIENT_SHA256=
HIL_CLIENT_ACTIVE=false
DEFERRED_SIGNAL_EXIT=

fail() {
    printf '%s\n' "rv1106_manual_single_turn: $1" >&2
    exit "${2:-2}"
}

require_env() {
    variable_name=$1
    eval "variable_value=\${$variable_name-}"
    [ -n "$variable_value" ] || fail "missing $variable_name"
}

cmdline_has_arg() {
    cmdline_file=$1
    expected_arg=$2
    tr '\000' '\n' <"$cmdline_file" | grep -F -x -e "$expected_arg" >/dev/null
}

cmdline_contains() {
    cmdline_file=$1
    expected_text=$2
    tr '\000' '\n' <"$cmdline_file" | grep -F -e "$expected_text" >/dev/null
}

validate_supervisor() {
    [ -n "$SUPERVISOR_PID" ] || return 1
    [ -r "/proc/$SUPERVISOR_PID/stat" ] || return 1
    [ "$(readlink "/proc/$SUPERVISOR_PID/exe")" = /bin/busybox ] || return 1
    [ "$(awk '{print $4}' "/proc/$SUPERVISOR_PID/stat")" = 1 ] || return 1
    [ "$(awk '{print $22}' "/proc/$SUPERVISOR_PID/stat")" = "$SUPERVISOR_START" ] || return 1
}

hil_client_instance_is_current() {
    [ "$HIL_CLIENT_ACTIVE" = true ] || return 1
    [ -n "$HIL_CLIENT_PID" ] || return 1
    [ -n "$HIL_CLIENT_START" ] || return 1
    [ -r "/proc/$HIL_CLIENT_PID/stat" ] || return 1
    [ "$(awk '{print $22}' "/proc/$HIL_CLIENT_PID/stat" 2>/dev/null || true)" = \
        "$HIL_CLIENT_START" ]
}

hil_client_is_zombie() {
    [ "$(awk '{print $3}' "/proc/$HIL_CLIENT_PID/stat" 2>/dev/null || true)" = Z ]
}

capture_hil_client_identity() {
    hil_client_instance_is_current || return 1

    captured_ppid=$(awk '{print $4}' "/proc/$HIL_CLIENT_PID/stat" 2>/dev/null || true)
    [ "$captured_ppid" = "$HIL_CLIENT_PPID" ] || return 1
    [ "$captured_ppid" = "$$" ] || return 1

    captured_exe=$(readlink "/proc/$HIL_CLIENT_PID/exe" 2>/dev/null || true)
    [ "$captured_exe" = "$BOOMPI_HIL_CLIENT" ] || return 1

    captured_argc=$(tr '\000' '\n' <"/proc/$HIL_CLIENT_PID/cmdline" | \
        awk 'END {print NR}')
    captured_argv0=$(tr '\000' '\n' <"/proc/$HIL_CLIENT_PID/cmdline" | \
        sed -n '1p')
    captured_argv1=$(tr '\000' '\n' <"/proc/$HIL_CLIENT_PID/cmdline" | \
        sed -n '2p')
    [ "$captured_argc" = 2 ] || return 1
    [ "$captured_argv0" = "$BOOMPI_HIL_CLIENT" ] || return 1
    [ "$captured_argv1" = --manual-single-turn ] || return 1

    captured_client_sha256=$(sha256sum "/proc/$HIL_CLIENT_PID/exe" 2>/dev/null | \
        awk '{print $1}')
    [ "$captured_client_sha256" = "$BOOMPI_HIL_CLIENT_SHA256" ] || return 1
    captured_cmdline_sha256=$(sha256sum "/proc/$HIL_CLIENT_PID/cmdline" 2>/dev/null | \
        awk '{print $1}')
    [ -n "$captured_cmdline_sha256" ] || return 1

    # Re-read the immutable process coordinates after the slower /proc reads.
    # This closes the PID-reuse/exec race before the snapshot becomes trusted.
    hil_client_instance_is_current || return 1
    [ "$(awk '{print $4}' "/proc/$HIL_CLIENT_PID/stat" 2>/dev/null || true)" = \
        "$HIL_CLIENT_PPID" ] || return 1

    HIL_CLIENT_EXE=$captured_exe
    HIL_CLIENT_CMDLINE="$captured_argv0 $captured_argv1"
    HIL_CLIENT_CMDLINE_SHA256=$captured_cmdline_sha256
    HIL_CLIENT_SHA256=$captured_client_sha256
}

validate_hil_client() {
    hil_client_instance_is_current || return 1
    [ -n "$HIL_CLIENT_EXE" ] || return 1
    [ "$(awk '{print $4}' "/proc/$HIL_CLIENT_PID/stat" 2>/dev/null || true)" = \
        "$HIL_CLIENT_PPID" ] || return 1
    [ "$(readlink "/proc/$HIL_CLIENT_PID/exe" 2>/dev/null || true)" = \
        "$HIL_CLIENT_EXE" ] || return 1
    [ "$(sha256sum "/proc/$HIL_CLIENT_PID/exe" 2>/dev/null | awk '{print $1}')" = \
        "$HIL_CLIENT_SHA256" ] || return 1
    [ "$(sha256sum "/proc/$HIL_CLIENT_PID/cmdline" 2>/dev/null | awk '{print $1}')" = \
        "$HIL_CLIENT_CMDLINE_SHA256" ] || return 1
}

clear_hil_client_identity() {
    HIL_CLIENT_PID=
    HIL_CLIENT_START=
    HIL_CLIENT_PPID=
    HIL_CLIENT_EXE=
    HIL_CLIENT_CMDLINE=
    HIL_CLIENT_CMDLINE_SHA256=
    HIL_CLIENT_SHA256=
    HIL_CLIENT_ACTIVE=false
}

defer_signal_exit() {
    [ -n "$DEFERRED_SIGNAL_EXIT" ] || DEFERRED_SIGNAL_EXIT=$1
}

terminate_hil_client() {
    [ "$HIL_CLIENT_ACTIVE" = true ] || return 0

    if ! hil_client_instance_is_current; then
        clear_hil_client_identity
        return 0
    fi
    if hil_client_is_zombie; then
        wait "$HIL_CLIENT_PID" 2>/dev/null || true
        clear_hil_client_identity
        return 0
    fi

    # A signal may arrive in the small interval between fork and the initial
    # full snapshot. Accept the process only after all expected fields match.
    if [ -z "$HIL_CLIENT_EXE" ]; then
        capture_hil_client_identity || return 1
    fi
    validate_hil_client || return 1
    kill -TERM "$HIL_CLIENT_PID" || return 1

    for terminate_wait_index in 1 2 3 4 5; do
        if ! hil_client_instance_is_current; then
            clear_hil_client_identity
            return 0
        fi
        if hil_client_is_zombie; then
            wait "$HIL_CLIENT_PID" 2>/dev/null || true
            clear_hil_client_identity
            return 0
        fi
        validate_hil_client || return 1
        sleep 1
    done

    # Revalidate the complete snapshot immediately before the only SIGKILL.
    validate_hil_client || return 1
    kill -KILL "$HIL_CLIENT_PID" || return 1
    for kill_wait_index in 1 2 3 4 5; do
        if ! hil_client_instance_is_current; then
            clear_hil_client_identity
            return 0
        fi
        if hil_client_is_zombie; then
            wait "$HIL_CLIENT_PID" 2>/dev/null || true
            clear_hil_client_identity
            return 0
        fi
        validate_hil_client || return 1
        sleep 1
    done
    return 1
}

restore_supervisor() {
    [ "$SUPERVISOR_STOPPED" = true ] || return 0
    validate_supervisor || return 1
    kill -CONT "$SUPERVISOR_PID" || return 1
    SUPERVISOR_STOPPED=false
}

cleanup() {
    exit_code=$?
    trap - 0 HUP INT TERM
    if ! terminate_hil_client; then
        printf '%s\n' \
            'rv1106_manual_single_turn: HIL client termination failed; supervisor remains stopped' >&2
        exit 7
    fi
    if ! restore_supervisor; then
        printf '%s\n' 'rv1106_manual_single_turn: supervisor restoration failed' >&2
        exit 6
    fi
    exit "$exit_code"
}

trap cleanup 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

for required_variable in \
    BOOMPI_HIL_SUPERVISOR_PIDFILE \
    BOOMPI_HIL_OLD_CLIENT \
    BOOMPI_HIL_OLD_CLIENT_SHA256 \
    BOOMPI_HIL_CLIENT \
    BOOMPI_HIL_CLIENT_SHA256 \
    BOOMPI_SERVER_IP \
    BOOMPI_SERVER_PORT \
    BOOMPI_SERVER_NAME \
    BOOMPI_SERVER_SPKI_SHA256 \
    BOOMPI_DEVICE_ID \
    BOOMPI_DEVICE_TOKEN \
    BOOMPI_CAPTURE_PCM \
    BOOMPI_PLAYBACK_PCM \
    BOOMPI_CAPTURE_MIC_SLOT \
    BOOMPI_CAPTURE_MIC_POLARITY \
    BOOMPI_MANUAL_RECORD_MS \
    BOOMPI_VOLUME_PERCENT \
    BOOMPI_SPEAKER_GAIN_PERCENT
do
    require_env "$required_variable"
done

[ "${BOOMPI_HIL_EXECUTE:-}" = true ] || fail 'set BOOMPI_HIL_EXECUTE=true'
[ "${BOOMPI_HIL_ALLOW_SUPERVISOR_PAUSE:-}" = true ] || \
    fail 'set BOOMPI_HIL_ALLOW_SUPERVISOR_PAUSE=true'
[ "$BOOMPI_SERVER_IP" = 127.0.0.1 ] || fail 'HIL server must use board loopback'
[ "$BOOMPI_MANUAL_RECORD_MS" = 3000 ] || fail 'manual record duration must be 3000 ms'
case "$BOOMPI_HIL_CLIENT" in
    /tmp/*) ;;
    *) fail 'HIL client must be deployed below /tmp' ;;
esac

[ -r "$BOOMPI_HIL_SUPERVISOR_PIDFILE" ] || fail 'supervisor pidfile is unreadable'
[ -x "$BOOMPI_HIL_OLD_CLIENT" ] || fail 'old client is not executable'
[ -x "$BOOMPI_HIL_CLIENT" ] || fail 'HIL client is not executable'
[ "$(sha256sum "$BOOMPI_HIL_OLD_CLIENT" | awk '{print $1}')" = \
    "$BOOMPI_HIL_OLD_CLIENT_SHA256" ] || fail 'old client SHA-256 mismatch'
[ "$(sha256sum "$BOOMPI_HIL_CLIENT" | awk '{print $1}')" = \
    "$BOOMPI_HIL_CLIENT_SHA256" ] || fail 'HIL client SHA-256 mismatch'

SUPERVISOR_PID=$(cat "$BOOMPI_HIL_SUPERVISOR_PIDFILE")
case "$SUPERVISOR_PID" in
    ''|*[!0-9]*) fail 'invalid supervisor PID' ;;
esac
[ "$SUPERVISOR_PID" -gt 1 ] || fail 'unsafe supervisor PID'
[ -r "/proc/$SUPERVISOR_PID/stat" ] || fail 'supervisor process is absent'
SUPERVISOR_START=$(awk '{print $22}' "/proc/$SUPERVISOR_PID/stat")
validate_supervisor || fail 'supervisor identity mismatch'
cmdline_has_arg "/proc/$SUPERVISOR_PID/cmdline" -c || \
    fail 'supervisor is not a shell loop'
cmdline_contains "/proc/$SUPERVISOR_PID/cmdline" \
    "$BOOMPI_HIL_OLD_CLIENT --voice-loop" || fail 'supervisor command mismatch'

kill -STOP "$SUPERVISOR_PID"
SUPERVISOR_STOPPED=true
sleep 1
[ "$(awk '{print $3}' "/proc/$SUPERVISOR_PID/stat")" = T ] || \
    fail 'supervisor did not enter stopped state'

old_child=
for proc_dir in /proc/[0-9]*; do
    [ -r "$proc_dir/stat" ] || continue
    candidate=$(basename "$proc_dir")
    parent=$(awk '{print $4}' "$proc_dir/stat" 2>/dev/null || true)
    [ "$parent" = "$SUPERVISOR_PID" ] || continue
    [ "$(readlink "$proc_dir/exe" 2>/dev/null || true)" = \
        "$BOOMPI_HIL_OLD_CLIENT" ] || continue
    cmdline_has_arg "$proc_dir/cmdline" --voice-loop || continue
    [ -z "$old_child" ] || fail 'multiple matching old client children'
    old_child=$candidate
done
[ -n "$old_child" ] || fail 'matching old client child not found'
[ "$(sha256sum "/proc/$old_child/exe" | awk '{print $1}')" = \
    "$BOOMPI_HIL_OLD_CLIENT_SHA256" ] || fail 'running old client SHA-256 mismatch'
printf '%s\n' \
    "rv1106_manual_single_turn: old loop paused supervisor_pid=$SUPERVISOR_PID child_pid=$old_child"

kill -TERM "$old_child"
for wait_index in 1 2 3 4 5; do
    [ ! -r "/proc/$old_child/stat" ] && break
    [ "$(awk '{print $3}' "/proc/$old_child/stat")" = Z ] && break
    sleep 1
done
if [ -r "/proc/$old_child/stat" ] && \
   [ "$(awk '{print $3}' "/proc/$old_child/stat")" != Z ]; then
    [ "$(readlink "/proc/$old_child/exe" 2>/dev/null || true)" = \
        "$BOOMPI_HIL_OLD_CLIENT" ] || fail 'old child identity changed before SIGKILL'
    kill -KILL "$old_child"
    sleep 1
fi

pcm_all_closed() {
    found=false
    for status_file in /proc/asound/card*/pcm*/sub*/status; do
        [ -r "$status_file" ] || continue
        found=true
        grep -q -x closed "$status_file" || return 1
    done
    [ "$found" = true ]
}

pcm_all_closed || fail 'PCM is occupied after old client stop' 3
sleep 2
pcm_all_closed || fail 'PCM did not remain idle' 3
printf '%s\n' 'rv1106_manual_single_turn: PCM idle snapshots=2'

# Signals in the fork/exec/snapshot window must not enter cleanup before the
# child can be identified. Record the first signal, finish the trusted process
# snapshot, restore the normal traps, and only then take the common exit path.
DEFERRED_SIGNAL_EXIT=
trap 'defer_signal_exit 129' HUP
trap 'defer_signal_exit 130' INT
trap 'defer_signal_exit 143' TERM

"$BOOMPI_HIL_CLIENT" --manual-single-turn &
HIL_CLIENT_PID=$!
HIL_CLIENT_ACTIVE=true
[ "$HIL_CLIENT_PID" -gt 1 ] || fail 'unsafe HIL client PID' 4
[ -r "/proc/$HIL_CLIENT_PID/stat" ] || fail 'HIL client exited before identity capture' 4
HIL_CLIENT_START=$(awk '{print $22}' "/proc/$HIL_CLIENT_PID/stat" 2>/dev/null || true)
HIL_CLIENT_PPID=$(awk '{print $4}' "/proc/$HIL_CLIENT_PID/stat" 2>/dev/null || true)
[ -n "$HIL_CLIENT_START" ] || fail 'HIL client start time is unavailable' 4
[ "$HIL_CLIENT_PPID" = "$$" ] || fail 'HIL client parent mismatch' 4

identity_captured=false
for identity_wait_index in 1 2 3 4 5; do
    if capture_hil_client_identity; then
        identity_captured=true
        break
    fi
    hil_client_instance_is_current || break
    hil_client_is_zombie && break
    sleep 1 || true
done
[ "$identity_captured" = true ] || fail 'could not capture HIL client identity' 4
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
if [ -n "$DEFERRED_SIGNAL_EXIT" ]; then
    exit "$DEFERRED_SIGNAL_EXIT"
fi
printf '%s\n' \
    "rv1106_manual_single_turn: HIL client started pid=$HIL_CLIENT_PID start=$HIL_CLIENT_START ppid=$HIL_CLIENT_PPID exe=$HIL_CLIENT_EXE cmdline=$HIL_CLIENT_CMDLINE sha256=$HIL_CLIENT_SHA256"

set +e
wait "$HIL_CLIENT_PID"
client_exit=$?
set -e
clear_hil_client_identity

restore_supervisor || fail 'could not resume supervisor' 6

old_returned=
for return_index in 1 2 3 4 5; do
    for proc_dir in /proc/[0-9]*; do
        [ -r "$proc_dir/stat" ] || continue
        candidate=$(basename "$proc_dir")
        [ "$(awk '{print $4}' "$proc_dir/stat" 2>/dev/null || true)" = \
            "$SUPERVISOR_PID" ] || continue
        [ "$(readlink "$proc_dir/exe" 2>/dev/null || true)" = \
            "$BOOMPI_HIL_OLD_CLIENT" ] || continue
        cmdline_has_arg "$proc_dir/cmdline" --voice-loop || continue
        old_returned=$candidate
    done
    [ -n "$old_returned" ] && break
    sleep 1
done
[ -n "$old_returned" ] || fail 'old client did not return after resume' 6
[ "$(sha256sum "/proc/$old_returned/exe" | awk '{print $1}')" = \
    "$BOOMPI_HIL_OLD_CLIENT_SHA256" ] || fail 'restored old client SHA-256 mismatch' 6
printf '%s\n' \
    "rv1106_manual_single_turn: old loop restored supervisor_pid=$SUPERVISOR_PID child_pid=$old_returned"

[ "$client_exit" -eq 0 ] || fail "manual client exit=$client_exit" "$client_exit"
printf '%s\n' \
    "rv1106_manual_single_turn: pass client_sha256=$BOOMPI_HIL_CLIENT_SHA256"

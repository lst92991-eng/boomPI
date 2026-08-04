#!/bin/sh
# Bounded direct-ALSA full-duplex HIL for an RV1106 target.
#
# This script is deliberately separate from the read-only P0 inventory probe.
# It opens PCM devices and writes one mixer control only after three explicit
# opt-ins. It never stops services, kills unrelated processes, or guesses card
# numbers. The first test uses digital silence while the analogue DAC is Off.

set -u
LC_ALL=C
export LC_ALL

RATE_HZ=48000
CHANNELS=2
FORMAT=S16_LE
PERIOD_FRAMES=480
BUFFER_FRAMES=1920
CAPTURE_SECONDS=6
PLAYBACK_SECONDS=4
MIN_OVERLAP_MS=3000
BYTES_PER_FRAME=4
PLAYBACK_BYTES=$((RATE_HZ * PLAYBACK_SECONDS * BYTES_PER_FRAME))
CAPTURE_BYTES=$((RATE_HZ * CAPTURE_SECONDS * BYTES_PER_FRAME))

CAPTURE_PCM=
PLAYBACK_PCM=
MIXER_CARD=
DAC_CONTROL=
ARTIFACT_DIR=
EXECUTE=false
ALLOW_PCM_IO=false
ALLOW_MIXER_WRITE=false

ARTIFACT_CREATED=false
CAPTURE_PID=
PLAYBACK_PID=
CAPTURE_PROCESS_PID=0
PLAYBACK_PROCESS_PID=0
MIXER_RESTORE_NEEDED=false
MIXER_NUMID=
MIXER_ORIGINAL_INDEX=
MIXER_RESTORED=false
CLEANUP_ACTIVE=false

usage() {
    cat <<'EOF'
Usage:
  rv1106_alsa_full_duplex.sh \
    --capture-pcm hw:C,D \
    --playback-pcm hw:C,D \
    --mixer-card N \
    --dac-control NAME \
    --artifact-dir ABS_NEW_DIR \
    [--execute --allow-pcm-io --allow-mixer-write]

Without --execute the command validates the plan and performs no writes, PCM
I/O, or mixer access. Execution requires all three opt-in flags shown above.

The execution contract is fixed for the first HIL gate:
  48000 Hz, S16_LE, 2 channels, 480-frame period, 1920-frame buffer
  6 seconds capture, 4 seconds digital-silence playback, >=3 seconds overlap

Safety:
  * The artifact directory must be a new absolute path. Existing paths and
    symlinks are refused; recordings are created with owner-only permissions.
  * The selected DAC enum control must expose an exact Off item. Its original
    numeric value is saved, restored, and read back on every exit path.
  * PCM occupancy is checked twice. The script never invokes killall or stops
    another process; it only terminates child PIDs that it created.

Exit codes:
  0 pass or validated dry-run
  2 invalid CLI or unmet precondition
  3 selected PCM is already occupied
  4 capability/open/configuration failure
  5 full-duplex evidence failed or is inconclusive
  6 mixer restoration failed
EOF
}

argument_error() {
    printf '%s\n' "rv1106_alsa_full_duplex: invalid arguments; use --help" >&2
    exit 2
}

require_value() {
    [ "$#" -ge 2 ] || argument_error
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --capture-pcm)
            require_value "$@"
            CAPTURE_PCM=$2
            shift 2
            ;;
        --playback-pcm)
            require_value "$@"
            PLAYBACK_PCM=$2
            shift 2
            ;;
        --mixer-card)
            require_value "$@"
            MIXER_CARD=$2
            shift 2
            ;;
        --dac-control)
            require_value "$@"
            DAC_CONTROL=$2
            shift 2
            ;;
        --artifact-dir)
            require_value "$@"
            ARTIFACT_DIR=$2
            shift 2
            ;;
        --execute)
            EXECUTE=true
            shift
            ;;
        --allow-pcm-io)
            ALLOW_PCM_IO=true
            shift
            ;;
        --allow-mixer-write)
            ALLOW_MIXER_WRITE=true
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --)
            shift
            [ "$#" -eq 0 ] || argument_error
            ;;
        *)
            argument_error
            ;;
    esac
done

validate_pcm_name() {
    case "$1" in
        hw:*) ;;
        *) return 1 ;;
    esac
    validated_pcm_spec=${1#hw:}
    validated_pcm_card=${validated_pcm_spec%%,*}
    validated_pcm_device=${validated_pcm_spec#*,}
    [ "$validated_pcm_spec" = "$validated_pcm_card,$validated_pcm_device" ] ||
        return 1
    is_json_uint "$validated_pcm_card" &&
        is_json_uint "$validated_pcm_device"
}

is_json_uint() {
    case "$1" in
        0) return 0 ;;
        [1-9]*)
            case "$1" in
                *[!0-9]*) return 1 ;;
                *) return 0 ;;
            esac
            ;;
        *) return 1 ;;
    esac
}

validate_artifact_path() {
    case "$1" in
        /*) ;;
        *) return 1 ;;
    esac
    case "$1" in
        /|*//*|*/../*|*/..|*/./*|*/.|*[!A-Za-z0-9_./-]*) return 1 ;;
    esac
    return 0
}

validate_control_name() {
    validated_control_remainder=$1
    [ -n "$validated_control_remainder" ] || return 1
    while [ -n "$validated_control_remainder" ]; do
        validated_control_tail=${validated_control_remainder#?}
        validated_control_character=${validated_control_remainder%"$validated_control_tail"}
        case "$validated_control_character" in
            [A-Za-z0-9_]|' '|-) ;;
            *) return 1 ;;
        esac
        validated_control_remainder=$validated_control_tail
    done
    return 0
}

[ -n "$CAPTURE_PCM" ] || argument_error
[ -n "$PLAYBACK_PCM" ] || argument_error
[ -n "$MIXER_CARD" ] || argument_error
[ -n "$DAC_CONTROL" ] || argument_error
[ -n "$ARTIFACT_DIR" ] || argument_error
validate_pcm_name "$CAPTURE_PCM" || argument_error
validate_pcm_name "$PLAYBACK_PCM" || argument_error
is_json_uint "$MIXER_CARD" || argument_error
validate_control_name "$DAC_CONTROL" || argument_error
validate_artifact_path "$ARTIFACT_DIR" || argument_error

if [ "$EXECUTE" != true ]; then
    printf '%s\n' '{"schema_version":1,"mode":"dry_run","validated":true,"mutated":false}'
    exit 0
fi

if [ "$ALLOW_PCM_IO" != true ] || [ "$ALLOW_MIXER_WRITE" != true ]; then
    printf '%s\n' \
        "rv1106_alsa_full_duplex: execution requires --allow-pcm-io and --allow-mixer-write" >&2
    exit 2
fi

if [ -e "$ARTIFACT_DIR" ] || [ -L "$ARTIFACT_DIR" ]; then
    printf '%s\n' "rv1106_alsa_full_duplex: artifact directory must not already exist" >&2
    exit 2
fi

ARTIFACT_PARENT=${ARTIFACT_DIR%/*}
[ -n "$ARTIFACT_PARENT" ] || ARTIFACT_PARENT=/
if [ ! -d "$ARTIFACT_PARENT" ] || [ ! -w "$ARTIFACT_PARENT" ]; then
    printf '%s\n' "rv1106_alsa_full_duplex: artifact parent is unavailable" >&2
    exit 2
fi

for REQUIRED_COMMAND in amixer aplay arecord cat cmp dd dmesg fuser grep \
        kill mkdir mv readlink sed usleep wc; do
    if ! command -v "$REQUIRED_COMMAND" >/dev/null 2>&1; then
        printf '%s\n' "rv1106_alsa_full_duplex: required command is unavailable" >&2
        exit 2
    fi
done

ARTIFACT_PARENT_CANONICAL=$(readlink -f "$ARTIFACT_PARENT" 2>/dev/null || :)
if [ -z "$ARTIFACT_PARENT_CANONICAL" ] ||
        [ "$ARTIFACT_PARENT_CANONICAL" != "$ARTIFACT_PARENT" ]; then
    printf '%s\n' "rv1106_alsa_full_duplex: artifact parent must not traverse a symlink" >&2
    exit 2
fi

capture_spec=${CAPTURE_PCM#hw:}
capture_card=${capture_spec%,*}
capture_device=${capture_spec#*,}
playback_spec=${PLAYBACK_PCM#hw:}
playback_card=${playback_spec%,*}
playback_device=${playback_spec#*,}
CAPTURE_NODE=/dev/snd/pcmC${capture_card}D${capture_device}c
PLAYBACK_NODE=/dev/snd/pcmC${playback_card}D${playback_device}p

mixer_value_index() {
    sed -n 's/^[[:space:]]*: values=\([0-9][0-9]*\)[[:space:]]*$/\1/p' "$1"
}

mixer_value_index_from_text() {
    printf '%s\n' "$1" |
        sed -n 's/^[[:space:]]*: values=\([0-9][0-9]*\)[[:space:]]*$/\1/p'
}

restore_mixer() {
    [ "$MIXER_RESTORE_NEEDED" = true ] || return 0
    # Restoration must not depend on the artifact directory remaining writable.
    # Capture small command output in memory and persist it only best effort.
    if ! mixer_restore_set_output=$(amixer -c "$MIXER_CARD" cset \
            "numid=$MIXER_NUMID" "$MIXER_ORIGINAL_INDEX" 2>&1); then
        return 1
    fi
    if ! mixer_restore_get_output=$(amixer -c "$MIXER_CARD" cget \
            "numid=$MIXER_NUMID" 2>&1); then
        return 1
    fi
    restored_index=$(mixer_value_index_from_text "$mixer_restore_get_output")
    if ! is_json_uint "$restored_index" ||
            [ "$restored_index" != "$MIXER_ORIGINAL_INDEX" ]; then
        return 1
    fi
    (printf '%s\n' "$mixer_restore_set_output" \
        >"$ARTIFACT_DIR/mixer-restore-set.txt") 2>/dev/null || :
    (printf '%s\n' "$mixer_restore_get_output" \
        >"$ARTIFACT_DIR/mixer-restored.txt") 2>/dev/null || :
    MIXER_RESTORE_NEEDED=false
    MIXER_RESTORED=true
    return 0
}

child_is_reapable() {
    reap_check_pid=$1
    if ! kill -0 "$reap_check_pid" 2>/dev/null; then
        return 0
    fi
    if [ -r "/proc/$reap_check_pid/stat" ]; then
        if IFS= read -r reap_check_stat <"/proc/$reap_check_pid/stat"; then
            case "$reap_check_stat" in
                *') Z '*) return 0 ;;
            esac
        fi
    fi
    return 1
}

terminate_child() {
    child_pid=$1
    [ -n "$child_pid" ] || return 0
    if child_is_reapable "$child_pid"; then
        wait "$child_pid" 2>/dev/null || :
        return 0
    fi
    kill -TERM "$child_pid" 2>/dev/null || :
    child_wait_count=0
    while [ "$child_wait_count" -lt 10 ]; do
        if child_is_reapable "$child_pid"; then
            wait "$child_pid" 2>/dev/null || :
            return 0
        fi
        usleep 100000 2>/dev/null || :
        child_wait_count=$((child_wait_count + 1))
    done
    kill -KILL "$child_pid" 2>/dev/null || :
    child_wait_count=0
    while [ "$child_wait_count" -lt 10 ]; do
        if child_is_reapable "$child_pid"; then
            wait "$child_pid" 2>/dev/null || :
            return 0
        fi
        usleep 100000 2>/dev/null || :
        child_wait_count=$((child_wait_count + 1))
    done
    # An uninterruptible child must not make cleanup or mixer restoration wait
    # forever. It remains a hard HIL failure and may require a board reboot.
    return 1
}

cleanup() {
    cleanup_status=$?
    trap - 0
    trap ':' HUP INT QUIT TERM
    if [ "$CLEANUP_ACTIVE" = true ]; then
        exit "$cleanup_status"
    fi
    CLEANUP_ACTIVE=true
    # Digital playback is silence, so restoring the original mixer state first
    # is safer than allowing an unkillable ALSA child to defer restoration.
    mixer_restore_retry=false
    if [ "$ARTIFACT_CREATED" = true ] && ! restore_mixer; then
        mixer_restore_retry=true
    fi
    terminate_child "$PLAYBACK_PID"
    terminate_child "$CAPTURE_PID"
    PLAYBACK_PID=
    CAPTURE_PID=
    if [ "$mixer_restore_retry" = true ] && ! restore_mixer; then
        cleanup_status=6
    fi
    exit "$cleanup_status"
}

trap cleanup 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 131' QUIT
trap 'exit 143' TERM

umask 077
if ! mkdir "$ARTIFACT_DIR"; then
    printf '%s\n' "rv1106_alsa_full_duplex: could not create artifact directory" >&2
    exit 2
fi
ARTIFACT_CREATED=true

pcm_is_busy() {
    if fuser "$CAPTURE_NODE" "$PLAYBACK_NODE" >/dev/null 2>&1; then
        return 0
    else
        fuser_status=$?
    fi
    [ "$fuser_status" -eq 1 ] && return 1
    return 2
}

check_pcm_occupancy() {
    pcm_is_busy
    occupancy_status=$?
    case "$occupancy_status" in
        0)
            printf '%s\n' "rv1106_alsa_full_duplex: selected PCM is already occupied" >&2
            exit 3
            ;;
        1) ;;
        *)
            printf '%s\n' "rv1106_alsa_full_duplex: PCM occupancy check failed" >&2
            exit 4
            ;;
    esac
}

check_pcm_occupancy

DMESG_BEFORE_AVAILABLE=false
if dmesg >"$ARTIFACT_DIR/dmesg-before.txt" 2>"$ARTIFACT_DIR/dmesg-before.stderr"; then
    DMESG_BEFORE_AVAILABLE=true
fi

if ! dd if=/dev/zero of="$ARTIFACT_DIR/playback-silence.raw" \
        bs=3840 count=200 >"$ARTIFACT_DIR/dd.stdout" 2>"$ARTIFACT_DIR/dd.stderr"; then
    printf '%s\n' "rv1106_alsa_full_duplex: could not create bounded silence input" >&2
    exit 4
fi
silence_bytes=$(wc -c <"$ARTIFACT_DIR/playback-silence.raw" |
    sed 's/[[:space:]]//g')
case "$silence_bytes" in *[!0-9]*|'') silence_bytes=0 ;; esac
if [ "$silence_bytes" -ne "$PLAYBACK_BYTES" ]; then
    printf '%s\n' "rv1106_alsa_full_duplex: silence input has an unexpected size" >&2
    exit 4
fi

if ! amixer -c "$MIXER_CARD" cget "name=$DAC_CONTROL" \
        >"$ARTIFACT_DIR/mixer-before.txt" 2>"$ARTIFACT_DIR/mixer-before.stderr"; then
    printf '%s\n' "rv1106_alsa_full_duplex: DAC control could not be read" >&2
    exit 4
fi
if ! grep -Eq 'type=ENUMERATED([,[:space:]]|$)' \
        "$ARTIFACT_DIR/mixer-before.txt" ||
        ! grep -Eq '(^|[,[:space:]])values=1([,[:space:]]|$)' \
        "$ARTIFACT_DIR/mixer-before.txt"; then
    printf '%s\n' "rv1106_alsa_full_duplex: DAC control is not a single enum" >&2
    exit 4
fi
MIXER_NUMID=$(sed -n 's/^numid=\([0-9][0-9]*\),.*/\1/p' \
    "$ARTIFACT_DIR/mixer-before.txt")
MIXER_ORIGINAL_INDEX=$(mixer_value_index "$ARTIFACT_DIR/mixer-before.txt")
MIXER_OFF_INDEX=$(sed -n \
    "s/^[[:space:]]*; Item #\([0-9][0-9]*\) 'Off'[[:space:]]*$/\1/p" \
    "$ARTIFACT_DIR/mixer-before.txt")
if ! is_json_uint "$MIXER_NUMID" || \
        ! is_json_uint "$MIXER_ORIGINAL_INDEX" || \
        ! is_json_uint "$MIXER_OFF_INDEX"; then
    printf '%s\n' "rv1106_alsa_full_duplex: DAC enum metadata is incomplete" >&2
    exit 4
fi

# Mark restoration necessary before the write: even a failed cset may have
# partially changed hardware state.
MIXER_RESTORE_NEEDED=true
if ! amixer -c "$MIXER_CARD" cset "numid=$MIXER_NUMID" "$MIXER_OFF_INDEX" \
        >"$ARTIFACT_DIR/mixer-off-set.txt" 2>&1; then
    printf '%s\n' "rv1106_alsa_full_duplex: DAC Off write failed" >&2
    exit 4
fi
if ! amixer -c "$MIXER_CARD" cget "numid=$MIXER_NUMID" \
        >"$ARTIFACT_DIR/mixer-off-readback.txt" 2>&1; then
    printf '%s\n' "rv1106_alsa_full_duplex: DAC Off readback failed" >&2
    exit 4
fi
MIXER_CURRENT_INDEX=$(mixer_value_index "$ARTIFACT_DIR/mixer-off-readback.txt")
if [ "$MIXER_CURRENT_INDEX" != "$MIXER_OFF_INDEX" ]; then
    printf '%s\n' "rv1106_alsa_full_duplex: DAC did not enter the verified Off enum" >&2
    exit 4
fi

check_pcm_occupancy

pid_has_pcm_fd() {
    checked_pid=$1
    expected_node=$2
    for checked_fd in /proc/"$checked_pid"/fd/*; do
        [ -e "$checked_fd" ] || [ -L "$checked_fd" ] || continue
        checked_target=$(readlink "$checked_fd" 2>/dev/null || :)
        [ "$checked_target" = "$expected_node" ] && return 0
    done
    return 1
}

wait_for_pcm_fd() {
    waited_pid=$1
    waited_node=$2
    wait_count=0
    while [ "$wait_count" -lt 40 ]; do
        kill -0 "$waited_pid" 2>/dev/null || return 1
        pid_has_pcm_fd "$waited_pid" "$waited_node" && return 0
        usleep 50000 || return 1
        wait_count=$((wait_count + 1))
    done
    return 1
}

monotonic_ms() {
    if ! IFS=' ' read -r uptime_value uptime_remainder </proc/uptime; then
        return 1
    fi
    case "$uptime_value" in
        *.*) ;;
        *) return 1 ;;
    esac
    uptime_whole=${uptime_value%%.*}
    uptime_fraction=${uptime_value#*.}
    case "$uptime_whole" in
        ''|*[!0-9]*) return 1 ;;
    esac
    uptime_fraction_first=${uptime_fraction%"${uptime_fraction#?}"}
    uptime_fraction_tail=${uptime_fraction#?}
    uptime_fraction_second=${uptime_fraction_tail%"${uptime_fraction_tail#?}"}
    case "$uptime_fraction_first$uptime_fraction_second" in
        [0-9][0-9]) ;;
        *) return 1 ;;
    esac
    printf '%s\n' "$((uptime_whole * 1000 +
        uptime_fraction_first * 100 + uptime_fraction_second * 10))"
}

CAPTURE_START_MONOTONIC_MS=$(monotonic_ms) || exit 4
# Verbose ALSA setup is retained in the artifact so requested values are not
# mistaken for the parameters that the kernel actually accepted.
arecord -v -D "$CAPTURE_PCM" -t raw -f "$FORMAT" -r "$RATE_HZ" \
    -c "$CHANNELS" --period-size="$PERIOD_FRAMES" \
    --buffer-size="$BUFFER_FRAMES" -d "$CAPTURE_SECONDS" \
    "$ARTIFACT_DIR/capture.raw" >"$ARTIFACT_DIR/arecord.stdout" \
    2>"$ARTIFACT_DIR/arecord.stderr" &
CAPTURE_PID=$!
CAPTURE_PROCESS_PID=$CAPTURE_PID
if ! wait_for_pcm_fd "$CAPTURE_PID" "$CAPTURE_NODE"; then
    printf '%s\n' "rv1106_alsa_full_duplex: capture PCM did not open in time" >&2
    exit 4
fi
CAPTURE_OPEN_MONOTONIC_MS=$(monotonic_ms) || exit 4

PLAYBACK_START_MONOTONIC_MS=$(monotonic_ms) || exit 4
aplay -v -D "$PLAYBACK_PCM" -t raw -f "$FORMAT" -r "$RATE_HZ" \
    -c "$CHANNELS" --period-size="$PERIOD_FRAMES" \
    --buffer-size="$BUFFER_FRAMES" "$ARTIFACT_DIR/playback-silence.raw" \
    >"$ARTIFACT_DIR/aplay.stdout" 2>"$ARTIFACT_DIR/aplay.stderr" &
PLAYBACK_PID=$!
PLAYBACK_PROCESS_PID=$PLAYBACK_PID
if ! wait_for_pcm_fd "$PLAYBACK_PID" "$PLAYBACK_NODE"; then
    printf '%s\n' "rv1106_alsa_full_duplex: playback PCM did not open in time" >&2
    exit 4
fi
PLAYBACK_OPEN_MONOTONIC_MS=$(monotonic_ms) || exit 4

CAPTURE_DONE=false
PLAYBACK_DONE=false
RUN_DEADLINE_MONOTONIC_MS=$((CAPTURE_START_MONOTONIC_MS + 12000))
while [ "$CAPTURE_DONE" != true ] || [ "$PLAYBACK_DONE" != true ]; do
    run_now_ms=$(monotonic_ms) || exit 4
    if [ "$PLAYBACK_DONE" != true ] && child_is_reapable "$PLAYBACK_PID"; then
        wait "$PLAYBACK_PID"
        PLAYBACK_RC=$?
        PLAYBACK_END_MONOTONIC_MS=$run_now_ms
        PLAYBACK_PID=
        PLAYBACK_DONE=true
    fi
    if [ "$CAPTURE_DONE" != true ] && child_is_reapable "$CAPTURE_PID"; then
        wait "$CAPTURE_PID"
        CAPTURE_RC=$?
        CAPTURE_END_MONOTONIC_MS=$run_now_ms
        CAPTURE_PID=
        CAPTURE_DONE=true
    fi
    if [ "$CAPTURE_DONE" = true ] && [ "$PLAYBACK_DONE" = true ]; then
        break
    fi
    if [ "$run_now_ms" -ge "$RUN_DEADLINE_MONOTONIC_MS" ]; then
        (printf '%s\n' "bounded PCM deadline expired" \
            >"$ARTIFACT_DIR/run-timeout.txt") 2>/dev/null || :
        printf '%s\n' "rv1106_alsa_full_duplex: bounded PCM deadline expired" >&2
        exit 5
    fi
    usleep 100000 || exit 4
done

DMESG_AFTER_AVAILABLE=false
if dmesg >"$ARTIFACT_DIR/dmesg-after.txt" 2>"$ARTIFACT_DIR/dmesg-after.stderr"; then
    DMESG_AFTER_AVAILABLE=true
fi

DMESG_STATUS=indeterminate
DMESG_DELTA_WRITABLE=false
if : >"$ARTIFACT_DIR/dmesg-delta.txt"; then
    DMESG_DELTA_WRITABLE=true
fi
if [ "$DMESG_DELTA_WRITABLE" = true ] &&
        [ "$DMESG_BEFORE_AVAILABLE" = true ] &&
        [ "$DMESG_AFTER_AVAILABLE" = true ] &&
        before_lines=$(wc -l <"$ARTIFACT_DIR/dmesg-before.txt" |
            sed 's/[[:space:]]//g') &&
        after_lines=$(wc -l <"$ARTIFACT_DIR/dmesg-after.txt" |
            sed 's/[[:space:]]//g'); then
    case "$before_lines:$after_lines" in
        *[!0-9:]*|:*) ;;
        *)
            if [ "$after_lines" -ge "$before_lines" ]; then
                DMESG_PREFIX_WRITTEN=false
                if [ "$before_lines" -eq 0 ]; then
                    if : >"$ARTIFACT_DIR/dmesg-prefix.txt"; then
                        DMESG_PREFIX_WRITTEN=true
                    fi
                elif sed -n "1,${before_lines}p" \
                        "$ARTIFACT_DIR/dmesg-after.txt" \
                        >"$ARTIFACT_DIR/dmesg-prefix.txt"; then
                    DMESG_PREFIX_WRITTEN=true
                fi
                if [ "$DMESG_PREFIX_WRITTEN" = true ] &&
                        cmp -s "$ARTIFACT_DIR/dmesg-before.txt" \
                        "$ARTIFACT_DIR/dmesg-prefix.txt"; then
                    delta_start=$((before_lines + 1))
                    if sed -n "${delta_start},\$p" \
                            "$ARTIFACT_DIR/dmesg-after.txt" \
                            >"$ARTIFACT_DIR/dmesg-delta.txt"; then
                        grep -Eiq 'xrun|underrun|overrun' \
                            "$ARTIFACT_DIR/dmesg-delta.txt"
                        dmesg_grep_status=$?
                        case "$dmesg_grep_status" in
                            0) DMESG_STATUS=xrun ;;
                            1) DMESG_STATUS=clean ;;
                            *) DMESG_STATUS=indeterminate ;;
                        esac
                    fi
                fi
            fi
            ;;
    esac
fi

capture_actual_bytes=0
if [ -f "$ARTIFACT_DIR/capture.raw" ]; then
    capture_actual_bytes=$(wc -c <"$ARTIFACT_DIR/capture.raw" |
        sed 's/[[:space:]]//g')
    case "$capture_actual_bytes" in *[!0-9]*|'') capture_actual_bytes=0 ;; esac
fi

later_open_ms=$CAPTURE_OPEN_MONOTONIC_MS
[ "$PLAYBACK_OPEN_MONOTONIC_MS" -gt "$later_open_ms" ] && \
    later_open_ms=$PLAYBACK_OPEN_MONOTONIC_MS
earlier_end_ms=$CAPTURE_END_MONOTONIC_MS
[ "$PLAYBACK_END_MONOTONIC_MS" -lt "$earlier_end_ms" ] && \
    earlier_end_ms=$PLAYBACK_END_MONOTONIC_MS
overlap_ms=$((earlier_end_ms - later_open_ms))
[ "$overlap_ms" -lt 0 ] && overlap_ms=0

if ! restore_mixer; then
    printf '%s\n' "rv1106_alsa_full_duplex: mixer restoration failed" >&2
    exit 6
fi

OVERALL=pass
FINAL_STATUS=0
if [ "$CAPTURE_RC" -ne 0 ] || [ "$PLAYBACK_RC" -ne 0 ] || \
        [ "$capture_actual_bytes" -ne "$CAPTURE_BYTES" ] || \
        [ "$overlap_ms" -lt "$MIN_OVERLAP_MS" ] || \
        [ "$DMESG_STATUS" = xrun ]; then
    OVERALL=fail
    FINAL_STATUS=5
elif [ "$DMESG_STATUS" != clean ]; then
    OVERALL=inconclusive
    FINAL_STATUS=5
fi

RESULT_TEMP_FILE="$ARTIFACT_DIR/.result.json.tmp.$$"
if ! cat >"$RESULT_TEMP_FILE" <<EOF
{
  "schema_version": 1,
  "mode": "execute",
  "configuration": {
    "capture_pcm": "$CAPTURE_PCM",
    "playback_pcm": "$PLAYBACK_PCM",
    "rate_hz": $RATE_HZ,
    "format": "$FORMAT",
    "channels": $CHANNELS,
    "requested_period_frames": $PERIOD_FRAMES,
    "requested_buffer_frames": $BUFFER_FRAMES,
    "capture_seconds": $CAPTURE_SECONDS,
    "playback_seconds": $PLAYBACK_SECONDS
  },
  "mixer": {
    "card": $MIXER_CARD,
    "numid": $MIXER_NUMID,
    "original_index": $MIXER_ORIGINAL_INDEX,
    "off_index": $MIXER_OFF_INDEX,
    "restored": $MIXER_RESTORED
  },
  "capture": {
    "pid": $CAPTURE_PROCESS_PID,
    "start_monotonic_ms": $CAPTURE_START_MONOTONIC_MS,
    "open_monotonic_ms": $CAPTURE_OPEN_MONOTONIC_MS,
    "end_monotonic_ms": $CAPTURE_END_MONOTONIC_MS,
    "exit_code": $CAPTURE_RC,
    "expected_bytes": $CAPTURE_BYTES,
    "actual_bytes": $capture_actual_bytes
  },
  "playback": {
    "pid": $PLAYBACK_PROCESS_PID,
    "start_monotonic_ms": $PLAYBACK_START_MONOTONIC_MS,
    "open_monotonic_ms": $PLAYBACK_OPEN_MONOTONIC_MS,
    "end_monotonic_ms": $PLAYBACK_END_MONOTONIC_MS,
    "exit_code": $PLAYBACK_RC,
    "silence_bytes": $silence_bytes
  },
  "overlap_ms": $overlap_ms,
  "minimum_overlap_ms": $MIN_OVERLAP_MS,
  "dmesg": "$DMESG_STATUS",
  "overall": "$OVERALL"
}
EOF
then
    printf '%s\n' "rv1106_alsa_full_duplex: could not write result artifact" >&2
    exit 5
fi
if ! mv "$RESULT_TEMP_FILE" "$ARTIFACT_DIR/result.json"; then
    printf '%s\n' "rv1106_alsa_full_duplex: could not publish result artifact" >&2
    exit 5
fi

printf '%s\n' "rv1106_alsa_full_duplex: $OVERALL"
exit "$FINAL_STATUS"

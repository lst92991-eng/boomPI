#!/bin/sh
# Read-only preflight for the RV1106 Rockchip MPI audio HIL probe.
#
# This script only inventories evidence. It never opens audio devices, invokes
# Rockchip MPI, accesses mixer controls, sends signals, controls services, or
# performs explicit filesystem writes. A successful inventory is not permission
# to execute the HIL probe: execution_gate.safe_to_execute is deliberately false.

set -u

usage() {
    printf '%s\n' \
        'Usage: rv1106_rockchip_mpi_audio_preflight.sh [--help]' \
        '' \
        'Emit a read-only Rockchip MPI audio preflight report as JSON.' \
        '' \
        'Environment:' \
        '  BOOMPI_HIL_PREFLIGHT_ROOT  Prefix used for /proc, /dev, /etc, and /oem.' \
        '' \
        'The probe never opens PCM, reads or writes mixer controls, invokes MPI,' \
        'signals processes, controls services, or explicitly writes, creates,' \
        'deletes, or renames files.'
}

argument_error() {
    printf '%s\n' \
        'rv1106_rockchip_mpi_audio_preflight: invalid arguments; use --help' >&2
    exit 2
}

# Argument handling deliberately precedes every environment or target probe so
# --help and invalid invocations have zero target-observation side effects.
if [ "$#" -eq 1 ]; then
    case "$1" in
        -h|--help)
            usage
            exit 0
            ;;
    esac
fi
[ "$#" -eq 0 ] || argument_error

PROBE_ROOT=${BOOMPI_HIL_PREFLIGHT_ROOT:-}
COLLECTION_COMPLETE=true
ROOT_AVAILABLE=true

if [ -n "$PROBE_ROOT" ] && [ ! -d "$PROBE_ROOT" ]; then
    COLLECTION_COMPLETE=false
    ROOT_AVAILABLE=false
fi

root_path() {
    printf '%s%s' "$PROBE_ROOT" "$1"
}

tool_status() {
    if command -v "$1" >/dev/null 2>&1; then
        printf '%s' available
    else
        printf '%s' missing
    fi
}

safe_token() {
    case "$1" in
        ''|*[!A-Za-z0-9._+-]*) printf '%s' unknown ;;
        *) printf '%s' "$1" ;;
    esac
}

is_readable_file() {
    [ -r "$1" ] && [ ! -d "$1" ]
}

path_exists() {
    [ -e "$1" ] || [ -L "$1" ]
}

json_bool() {
    [ "$1" = true ] && printf '%s' true || printf '%s' false
}

TOOL_DMESG=$(tool_status dmesg)
TOOL_READLINK=$(tool_status readlink)
TOOL_AWK=$(tool_status awk)
TOOL_SLEEP=$(tool_status sleep)
TOOL_USLEEP=$(tool_status usleep)
TOOL_KILL=$(tool_status kill)
TOOL_MKDIR=$(tool_status mkdir)
TOOL_MV=$(tool_status mv)
TOOL_CMP=$(tool_status cmp)
TOOL_SED=$(tool_status sed)
TOOL_WC=$(tool_status wc)
TOOL_SHA256SUM=$(tool_status sha256sum)
TOOL_FLOCK=$(tool_status flock)
TOOL_PS=$(tool_status ps)
TOOL_FUSER=$(tool_status fuser)
TOOL_SCP=$(tool_status scp)
TOOL_TIMEOUT=$(tool_status timeout)
TOOL_STAT=$(tool_status stat)

PROC_ROOT=$(root_path /proc)
PID1_COMM_PATH=$(root_path /proc/1/comm)
PID1_EXE_PATH=$(root_path /proc/1/exe)
PID1_COMM=unknown
PID1_EXE_FAMILY=unknown
BUSYBOX_VERSION=unknown

if is_readable_file "$PID1_COMM_PATH"; then
    if IFS= read -r pid1_comm_raw <"$PID1_COMM_PATH"; then
        PID1_COMM=$(safe_token "$pid1_comm_raw")
        [ "$PID1_COMM" != unknown ] || COLLECTION_COMPLETE=false
    else
        COLLECTION_COMPLETE=false
    fi
else
    COLLECTION_COMPLETE=false
fi

if [ "$TOOL_READLINK" = available ]; then
    if pid1_exe_target=$(readlink "$PID1_EXE_PATH" 2>/dev/null); then
        pid1_exe_name=${pid1_exe_target##*/}
        case "$pid1_exe_name" in
            busybox) PID1_EXE_FAMILY=busybox ;;
            systemd) PID1_EXE_FAMILY=systemd ;;
            init|sysvinit) PID1_EXE_FAMILY=sysvinit ;;
            '') PID1_EXE_FAMILY=unknown ;;
            *) PID1_EXE_FAMILY=other ;;
        esac
    else
        COLLECTION_COMPLETE=false
    fi
else
    COLLECTION_COMPLETE=false
fi

if command -v busybox >/dev/null 2>&1; then
    busybox_banner=$(busybox 2>&1 || :)
    case "$busybox_banner" in
        *'BusyBox v'*)
            busybox_version_tail=${busybox_banner#*BusyBox v}
            busybox_version_raw=${busybox_version_tail%% *}
            BUSYBOX_VERSION=$(safe_token "$busybox_version_raw")
            ;;
    esac
fi
if [ "$PID1_EXE_FAMILY" = busybox ] && [ "$BUSYBOX_VERSION" = unknown ]; then
    COLLECTION_COMPLETE=false
fi

DMESG_READABLE=false
DMESG_FOLLOW_OPTION_LISTED=false
if [ "$TOOL_DMESG" = available ]; then
    if dmesg >/dev/null 2>&1; then
        DMESG_READABLE=true
    fi
    dmesg_help=$(dmesg --help 2>&1 || :)
    case "$dmesg_help" in
        *'-w'*|*'follow'*) DMESG_FOLLOW_OPTION_LISTED=true ;;
    esac
fi

KMSG_PATH=$(root_path /dev/kmsg)
KMSG_PRESENT=false
KMSG_READABLE=false
if path_exists "$KMSG_PATH"; then
    KMSG_PRESENT=true
    [ -r "$KMSG_PATH" ] && KMSG_READABLE=true
fi

PROC_FD_SCAN_COMPLETE=true
SND_FD_COUNT=0
SND_OWNER_COUNT=0
MPI_FD_COUNT=0
MPI_OWNER_COUNT=0
RKIPC_PROCESS_COUNT=0
RKIPC_MPI_OWNER_COUNT=0
PID_DIRECTORY_SEEN=false

if [ "$ROOT_AVAILABLE" != true ] || [ "$TOOL_READLINK" != available ] ||
        [ ! -d "$PROC_ROOT" ] || [ ! -r "$PROC_ROOT" ]; then
    PROC_FD_SCAN_COMPLETE=false
else
    for process_dir in "$PROC_ROOT"/[0-9]*; do
        [ -d "$process_dir" ] || continue
        PID_DIRECTORY_SEEN=true
        process_name=unknown
        process_comm_path=$process_dir/comm
        if is_readable_file "$process_comm_path"; then
            if ! IFS= read -r process_name <"$process_comm_path"; then
                if [ -d "$process_dir" ]; then
                    PROC_FD_SCAN_COMPLETE=false
                fi
                process_name=unknown
            fi
        else
            if [ -d "$process_dir" ]; then
                PROC_FD_SCAN_COMPLETE=false
            fi
        fi

        process_is_rkipc=false
        if [ "$process_name" = rkipc ]; then
            process_is_rkipc=true
            RKIPC_PROCESS_COUNT=$((RKIPC_PROCESS_COUNT + 1))
        fi

        process_has_snd=false
        process_has_mpi=false
        process_fd_dir=$process_dir/fd
        if [ ! -d "$process_fd_dir" ] || [ ! -r "$process_fd_dir" ] ||
                [ ! -x "$process_fd_dir" ]; then
            if [ -d "$process_dir" ]; then
                PROC_FD_SCAN_COMPLETE=false
            fi
            continue
        fi

        for fd_link in "$process_fd_dir"/*; do
            if [ ! -e "$fd_link" ] && [ ! -L "$fd_link" ]; then
                continue
            fi
            if [ ! -L "$fd_link" ]; then
                PROC_FD_SCAN_COMPLETE=false
                continue
            fi
            if ! fd_target=$(readlink "$fd_link" 2>/dev/null); then
                if [ -d "$process_dir" ] && path_exists "$fd_link"; then
                    PROC_FD_SCAN_COMPLETE=false
                fi
                continue
            fi

            fd_kind=other
            case "$fd_target" in
                /dev/snd/*) fd_kind=snd ;;
                /dev/mpi/*) fd_kind=mpi ;;
                *)
                    if [ -n "$PROBE_ROOT" ]; then
                        case "$fd_target" in
                            "$PROBE_ROOT"/dev/snd/*) fd_kind=snd ;;
                            "$PROBE_ROOT"/dev/mpi/*) fd_kind=mpi ;;
                        esac
                    fi
                    ;;
            esac
            case "$fd_kind" in
                snd)
                    SND_FD_COUNT=$((SND_FD_COUNT + 1))
                    process_has_snd=true
                    ;;
                mpi)
                    MPI_FD_COUNT=$((MPI_FD_COUNT + 1))
                    process_has_mpi=true
                    ;;
            esac
        done

        [ "$process_has_snd" = true ] &&
            SND_OWNER_COUNT=$((SND_OWNER_COUNT + 1))
        if [ "$process_has_mpi" = true ]; then
            MPI_OWNER_COUNT=$((MPI_OWNER_COUNT + 1))
            [ "$process_is_rkipc" = true ] &&
                RKIPC_MPI_OWNER_COUNT=$((RKIPC_MPI_OWNER_COUNT + 1))
        fi
    done
fi

if [ "$PID_DIRECTORY_SEEN" != true ]; then
    PROC_FD_SCAN_COMPLETE=false
fi
[ "$PROC_FD_SCAN_COMPLETE" = true ] || COLLECTION_COMPLETE=false

S21APPINIT_PATH=$(root_path /etc/init.d/S21appinit)
RKLUNCH_START_PATH=$(root_path /oem/usr/bin/RkLunch.sh)
RKLUNCH_STOP_PATH_DASH=$(root_path /oem/usr/bin/RkLunch-stop.sh)
RKLUNCH_STOP_PATH_UNDERSCORE=$(root_path /oem/usr/bin/RkLunch_stop.sh)
RKLUNCH_STOP_PATH_CAMEL=$(root_path /oem/usr/bin/RkLunchStop.sh)

S21APPINIT_PRESENT=false
S21APPINIT_START_LEXICAL=false
S21APPINIT_STOP_LEXICAL=false
RKLUNCH_START_SCRIPT_PRESENT=false
RKLUNCH_STOP_SCRIPT_PRESENT=false
STOP_KILLALL_RKIPC_LEXICAL=false
STOP_UDHCPC_LEXICAL=false

inspect_service_script() {
    inspected_script=$1
    inspected_kind=$2
    [ -r "$inspected_script" ] && [ ! -d "$inspected_script" ] || return 0
    while IFS= read -r service_line || [ -n "$service_line" ]; do
        case "$inspected_kind:$service_line" in
            s21:*start\)*) S21APPINIT_START_LEXICAL=true ;;
        esac
        case "$inspected_kind:$service_line" in
            s21:*stop\)*) S21APPINIT_STOP_LEXICAL=true ;;
        esac
        if [ "$inspected_kind" = stop ]; then
            case "$service_line" in
                *killall*rkipc*) STOP_KILLALL_RKIPC_LEXICAL=true ;;
            esac
            case "$service_line" in
                *killall*udhcpc*) STOP_UDHCPC_LEXICAL=true ;;
            esac
        fi
    done <"$inspected_script"
}

if is_readable_file "$S21APPINIT_PATH"; then
    S21APPINIT_PRESENT=true
    inspect_service_script "$S21APPINIT_PATH" s21
elif path_exists "$S21APPINIT_PATH"; then
    COLLECTION_COMPLETE=false
fi

if is_readable_file "$RKLUNCH_START_PATH"; then
    RKLUNCH_START_SCRIPT_PRESENT=true
    inspect_service_script "$RKLUNCH_START_PATH" start
elif path_exists "$RKLUNCH_START_PATH"; then
    COLLECTION_COMPLETE=false
fi

for rklunch_stop_path in "$RKLUNCH_STOP_PATH_DASH" \
        "$RKLUNCH_STOP_PATH_UNDERSCORE" "$RKLUNCH_STOP_PATH_CAMEL"; do
    if is_readable_file "$rklunch_stop_path"; then
        RKLUNCH_STOP_SCRIPT_PRESENT=true
        inspect_service_script "$rklunch_stop_path" stop
    elif path_exists "$rklunch_stop_path"; then
        COLLECTION_COMPLETE=false
    fi
done

# Some images implement stop in RkLunch.sh itself rather than a second file.
if [ "$RKLUNCH_START_SCRIPT_PRESENT" = true ]; then
    rklunch_inline_stop=false
    while IFS= read -r rklunch_line || [ -n "$rklunch_line" ]; do
        case "$rklunch_line" in
            *stop\)*) rklunch_inline_stop=true ;;
        esac
    done <"$RKLUNCH_START_PATH"
    [ "$rklunch_inline_stop" = true ] && RKLUNCH_STOP_SCRIPT_PRESENT=true
fi

PROBE_STATUS=complete
[ "$COLLECTION_COMPLETE" = true ] || PROBE_STATUS=incomplete

REASON_CODES='"exclusive_audio_service_control_unproven","continuous_kernel_log_evidence_unproven","service_control_automation_disabled"'
append_reason_code() {
    REASON_CODES=$REASON_CODES',"'$1'"'
}

[ "$PROBE_STATUS" = complete ] || append_reason_code preflight_collection_incomplete
[ "$PROC_FD_SCAN_COMPLETE" = true ] || append_reason_code proc_fd_scan_incomplete
[ "$RKIPC_PROCESS_COUNT" -eq 0 ] || append_reason_code rkipc_process_present
[ "$RKIPC_MPI_OWNER_COUNT" -eq 0 ] || append_reason_code rkipc_mpi_device_owner_present
[ "$SND_OWNER_COUNT" -eq 0 ] || append_reason_code snd_device_owner_present
[ "$MPI_OWNER_COUNT" -eq 0 ] || append_reason_code mpi_device_owner_present
[ "$DMESG_READABLE" = true ] || append_reason_code dmesg_snapshot_unavailable
[ "$DMESG_FOLLOW_OPTION_LISTED" = true ] ||
    append_reason_code dmesg_follow_option_not_listed
[ "$TOOL_TIMEOUT" = available ] || append_reason_code target_timeout_tool_missing
if [ "$STOP_KILLALL_RKIPC_LEXICAL" = true ] ||
        [ "$STOP_UDHCPC_LEXICAL" = true ]; then
    append_reason_code service_stop_lexical_risk_detected
fi

cat <<EOF
{
  "schema_version": 1,
  "probe": "boompi-rv1106-rockchip-mpi-audio-preflight",
  "mode": "read_only",
  "probe_status": "$PROBE_STATUS",
  "init": {
    "pid1_comm": "$PID1_COMM",
    "pid1_exe_family": "$PID1_EXE_FAMILY",
    "busybox_version": "$BUSYBOX_VERSION"
  },
  "tools": {
    "dmesg": "$TOOL_DMESG",
    "readlink": "$TOOL_READLINK",
    "awk": "$TOOL_AWK",
    "sleep": "$TOOL_SLEEP",
    "usleep": "$TOOL_USLEEP",
    "kill": "$TOOL_KILL",
    "mkdir": "$TOOL_MKDIR",
    "mv": "$TOOL_MV",
    "cmp": "$TOOL_CMP",
    "sed": "$TOOL_SED",
    "wc": "$TOOL_WC",
    "sha256sum": "$TOOL_SHA256SUM",
    "flock": "$TOOL_FLOCK",
    "ps": "$TOOL_PS",
    "fuser": "$TOOL_FUSER",
    "scp": "$TOOL_SCP",
    "timeout": "$TOOL_TIMEOUT",
    "stat": "$TOOL_STAT"
  },
  "kernel_log": {
    "dmesg_readable": $(json_bool "$DMESG_READABLE"),
    "dmesg_follow_option_listed": $(json_bool "$DMESG_FOLLOW_OPTION_LISTED"),
    "kmsg_present": $(json_bool "$KMSG_PRESENT"),
    "kmsg_readable": $(json_bool "$KMSG_READABLE"),
    "kmsg_stream_semantics": "unverified",
    "dmesg_evidence_scope": "snapshot_only",
    "continuous_evidence_ready": false
  },
  "audio": {
    "proc_fd_scan_complete": $(json_bool "$PROC_FD_SCAN_COMPLETE"),
    "snd_fd_count": $SND_FD_COUNT,
    "snd_owner_count": $SND_OWNER_COUNT,
    "mpi_fd_count": $MPI_FD_COUNT,
    "mpi_owner_count": $MPI_OWNER_COUNT,
    "rkipc_process_count": $RKIPC_PROCESS_COUNT,
    "rkipc_mpi_owner_count": $RKIPC_MPI_OWNER_COUNT
  },
  "service_control": {
    "s21appinit_present": $(json_bool "$S21APPINIT_PRESENT"),
    "s21appinit_start_lexical": $(json_bool "$S21APPINIT_START_LEXICAL"),
    "s21appinit_stop_lexical": $(json_bool "$S21APPINIT_STOP_LEXICAL"),
    "rklunch_start_script_present": $(json_bool "$RKLUNCH_START_SCRIPT_PRESENT"),
    "rklunch_stop_script_present": $(json_bool "$RKLUNCH_STOP_SCRIPT_PRESENT"),
    "stop_killall_rkipc_lexical": $(json_bool "$STOP_KILLALL_RKIPC_LEXICAL"),
    "stop_udhcpc_lexical": $(json_bool "$STOP_UDHCPC_LEXICAL"),
    "automation_safe": false
  },
  "execution_gate": {
    "safe_to_execute": false,
    "exclusivity": "unproven",
    "kernel_log_continuity": "unproven",
    "snapshot_has_pcm_owner": $([ "$SND_OWNER_COUNT" -gt 0 ] && printf true || printf false),
    "snapshot_has_mpi_owner": $([ "$MPI_OWNER_COUNT" -gt 0 ] && printf true || printf false),
    "reason_codes": [$REASON_CODES]
  },
  "validation": {
    "files_created": false,
    "files_deleted": false,
    "files_renamed": false,
    "pcm_devices_opened": false,
    "mixer_accessed": false,
    "mpi_api_called": false,
    "signals_sent": false,
    "services_stopped": false,
    "processes_terminated": false,
    "network_identifiers_collected": false,
    "command_lines_collected": false,
    "environment_collected": false,
    "paths_emitted": false
  }
}
EOF

exit 0

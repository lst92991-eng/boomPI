#!/bin/sh
# Read-only P0 inventory probe for boomPI RV1106 targets.
#
# The probe intentionally does not open ALSA PCM devices, enumerate addresses,
# scan Wi-Fi networks, or print paths. It reports candidates that still require
# the explicit HIL checks documented by the project.

set -u

PROBE_ROOT=${BOOMPI_PROBE_ROOT:-}
ROCKCHIP_3A_LIB=
SNOWBOY_LIB=
SNOWBOY_MODEL=

usage() {
    cat <<'EOF'
Usage: rv1106_p0_probe.sh [OPTIONS]

Emit a read-only RV1106 P0 capability inventory as JSON on stdout.

Options:
  --rockchip-3a-lib PATH  Inspect this libaec_bf_process.so candidate.
  --snowboy-lib PATH      Inspect this Snowboy shared library.
  --snowboy-model PATH    Report whether this Snowboy model is readable.
  -h, --help              Show this help text.

Privacy and safety:
  * The probe never opens PCM devices, changes mixers, scans Wi-Fi, or writes.
  * IP addresses, MAC addresses, SSIDs, credentials, hostnames, and paths are
    neither collected nor emitted.
  * A detected candidate is not a functional or real-time validation result.
EOF
}

argument_error() {
    printf '%s\n' "rv1106_p0_probe: invalid arguments; use --help" >&2
    exit 2
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --rockchip-3a-lib)
            [ "$#" -ge 2 ] || argument_error
            ROCKCHIP_3A_LIB=$2
            shift 2
            ;;
        --snowboy-lib)
            [ "$#" -ge 2 ] || argument_error
            SNOWBOY_LIB=$2
            shift 2
            ;;
        --snowboy-model)
            [ "$#" -ge 2 ] || argument_error
            SNOWBOY_MODEL=$2
            shift 2
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

if [ -n "$PROBE_ROOT" ] && [ ! -d "$PROBE_ROOT" ]; then
    printf '%s\n' "rv1106_p0_probe: probe root is not a directory" >&2
    exit 2
fi

if [ -n "$ROCKCHIP_3A_LIB" ]; then
    case "${ROCKCHIP_3A_LIB##*/}" in
        libaec_bf_process.so*) ;;
        *) argument_error ;;
    esac
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

is_readable_entry() {
    [ -r "$1" ] && [ ! -d "$1" ]
}

count_entries() (
    probe_count=0
    for probe_entry in "$@"; do
        if [ -e "$probe_entry" ] || [ -L "$probe_entry" ]; then
            probe_count=$((probe_count + 1))
        fi
    done
    printf '%s' "$probe_count"
)

first_rockchip_3a_aec_library() {
    for probe_entry in \
        "$PROBE_ROOT"/oem/usr/lib/libaec_bf_process.so* \
        "$PROBE_ROOT"/usr/lib/libaec_bf_process.so* \
        "$PROBE_ROOT"/lib/libaec_bf_process.so*; do
        if is_readable_entry "$probe_entry"; then
            printf '%s' "$probe_entry"
            return 0
        fi
    done
    return 1
}

first_rockchip_3a_common_library() {
    first_readable_candidate \
        "$PROBE_ROOT"/oem/usr/lib/librkaudio_common.so* \
        "$PROBE_ROOT"/usr/lib/librkaudio_common.so* \
        "$PROBE_ROOT"/lib/librkaudio_common.so*
}

first_rockchip_3a_detect_library() {
    first_readable_candidate \
        "$PROBE_ROOT"/oem/usr/lib/librkaudio_detect.so* \
        "$PROBE_ROOT"/usr/lib/librkaudio_detect.so* \
        "$PROBE_ROOT"/lib/librkaudio_detect.so*
}

first_snowboy_library() {
    for probe_entry in \
        "$PROBE_ROOT"/oem/usr/lib/libsnowboy*.so* \
        "$PROBE_ROOT"/usr/lib/libsnowboy*.so* \
        "$PROBE_ROOT"/usr/local/lib/libsnowboy*.so* \
        "$PROBE_ROOT"/userdata/boompi/models/snowboy/libsnowboy*.a \
        "$PROBE_ROOT"/userdata/boompi/models/snowboy/libsnowboy*.so* \
        "$PROBE_ROOT"/userdata/boompi/models/snowboy/_snowboydetect.so*; do
        if is_readable_entry "$probe_entry"; then
            printf '%s' "$probe_entry"
            return 0
        fi
    done
    return 1
}

first_snowboy_model() {
    for probe_entry in \
        "$PROBE_ROOT"/userdata/boompi/models/snowboy/*.pmdl \
        "$PROBE_ROOT"/userdata/boompi/models/snowboy/*.umdl; do
        if is_readable_entry "$probe_entry"; then
            printf '%s' "$probe_entry"
            return 0
        fi
    done
    return 1
}

first_libstdcxx() {
    for probe_entry in \
        "$PROBE_ROOT"/oem/usr/lib/libstdc++.so.6* \
        "$PROBE_ROOT"/usr/lib/libstdc++.so.6* \
        "$PROBE_ROOT"/lib/libstdc++.so.6* \
        "$PROBE_ROOT"/usr/lib/arm-linux-gnueabihf/libstdc++.so.6* \
        "$PROBE_ROOT"/lib/arm-linux-gnueabihf/libstdc++.so.6*; do
        if is_readable_entry "$probe_entry"; then
            printf '%s' "$probe_entry"
            return 0
        fi
    done
    return 1
}

first_rockchip_header_present() {
    for probe_entry in \
        "$PROBE_ROOT"/oem/usr/include/rkaudio*.h \
        "$PROBE_ROOT"/oem/usr/include/rkaudio/*.h \
        "$PROBE_ROOT"/usr/include/rkaudio*.h \
        "$PROBE_ROOT"/usr/include/rkaudio/*.h \
        "$PROBE_ROOT"/usr/include/rockchip/*audio*.h; do
        if is_readable_entry "$probe_entry"; then
            printf '%s' true
            return 0
        fi
    done
    printf '%s' false
}

first_readable_candidate() {
    for probe_entry in "$@"; do
        if is_readable_entry "$probe_entry"; then
            printf '%s' "$probe_entry"
            return 0
        fi
    done
    return 1
}

candidate_present() {
    if [ -n "$1" ] && is_readable_entry "$1"; then
        printf '%s' true
    else
        printf '%s' false
    fi
}

candidate_executable() {
    if [ -n "$1" ] && [ -f "$1" ] && [ -x "$1" ]; then
        printf '%s' true
    else
        printf '%s' false
    fi
}

map_machine() {
    case "$1" in
        armv5*|armv6*|armv7*|armv8l) printf '%s' arm32 ;;
        aarch64|arm64) printf '%s' arm64 ;;
        x86_64|amd64) printf '%s' x86_64 ;;
        i?86) printf '%s' x86_32 ;;
        riscv64) printf '%s' riscv64 ;;
        *) printf '%s' unknown ;;
    esac
}

safe_numeric_version() {
    printf '%s\n' "$1" | sed -n 's/^[^0-9]*\([0-9][0-9]*\.[0-9][0-9]*\(\.[0-9][0-9]*\)\?\).*/\1/p' | sed -n '1p'
}

probe_elf() {
    ELF_INSPECTION=unknown
    ELF_CLASS_BITS=0
    ELF_MACHINE=unknown
    ELF_ENDIANNESS=unknown
    ELF_FLOAT_ABI=unknown
    ELF_NEEDED_COUNT=0

    if [ -z "$1" ] || ! is_readable_entry "$1"; then
        ELF_INSPECTION=missing
        return
    fi
    if ! command -v readelf >/dev/null 2>&1; then
        ELF_INSPECTION=missing_dependency
        return
    fi

    probe_elf_header=$(readelf -h "$1" 2>/dev/null) || {
        ELF_INSPECTION=failed
        return
    }
    ELF_INSPECTION=available
    case "$probe_elf_header" in
        *ELF32*) ELF_CLASS_BITS=32 ;;
        *ELF64*) ELF_CLASS_BITS=64 ;;
    esac
    case "$probe_elf_header" in
        *AArch64*) ELF_MACHINE=arm64 ;;
        *ARM*) ELF_MACHINE=arm32 ;;
        *X86-64*|*x86-64*) ELF_MACHINE=x86_64 ;;
        *80386*) ELF_MACHINE=x86_32 ;;
        *RISC-V*) ELF_MACHINE=riscv64 ;;
    esac
    case "$probe_elf_header" in
        *little\ endian*) ELF_ENDIANNESS=little ;;
        *big\ endian*) ELF_ENDIANNESS=big ;;
    esac

    probe_elf_attributes=$(readelf -A "$1" 2>/dev/null || :)
    case "$probe_elf_header$probe_elf_attributes" in
        *hard-float\ ABI*|*Tag_ABI_VFP_args*VFP\ registers*) ELF_FLOAT_ABI=hard ;;
        *soft-float\ ABI*) ELF_FLOAT_ABI=soft ;;
    esac
    ELF_NEEDED_COUNT=$(readelf -d "$1" 2>/dev/null | grep -c '(NEEDED)' || :)
    case "$ELF_NEEDED_COUNT" in
        ''|*[!0-9]*) ELF_NEEDED_COUNT=0 ;;
    esac
}

elf_compatibility() {
    if [ "$1" = available ] && [ "$2" != unknown ] && [ "$TARGET_MACHINE" != unknown ]; then
        if [ "$2" != "$TARGET_MACHINE" ]; then
            printf '%s' mismatch
        elif [ "$3" -gt 0 ] && [ "$TARGET_WORD_BITS" -gt 0 ] && [ "$3" -ne "$TARGET_WORD_BITS" ]; then
            printf '%s' mismatch
        elif [ "$4" != unknown ] && [ "$TARGET_FLOAT_ABI" != unknown ] && [ "$4" != "$TARGET_FLOAT_ABI" ]; then
            printf '%s' mismatch
        else
            printf '%s' candidate
        fi
    else
        printf '%s' unknown
    fi
}

UNAME_STATUS=$(tool_status uname)
TARGET_OS=unknown
TARGET_MACHINE=unknown
KERNEL_VERSION=unknown
if [ "$UNAME_STATUS" = available ]; then
    probe_uname_os=$(uname -s 2>/dev/null || :)
    [ "$probe_uname_os" = Linux ] && TARGET_OS=linux
    probe_uname_machine=$(uname -m 2>/dev/null || :)
    TARGET_MACHINE=$(map_machine "$probe_uname_machine")
    probe_kernel_raw=$(uname -r 2>/dev/null || :)
    probe_kernel_version=$(safe_numeric_version "$probe_kernel_raw")
    [ -n "$probe_kernel_version" ] && KERNEL_VERSION=$probe_kernel_version
fi

TARGET_WORD_BITS=0
if command -v getconf >/dev/null 2>&1; then
    probe_word_bits=$(getconf LONG_BIT 2>/dev/null || :)
    case "$probe_word_bits" in
        32|64) TARGET_WORD_BITS=$probe_word_bits ;;
    esac
fi

SHELL_ELF=$(root_path /bin/sh)
probe_elf "$SHELL_ELF"
TARGET_ELF_INSPECTION=$ELF_INSPECTION
TARGET_ELF_CLASS_BITS=$ELF_CLASS_BITS
TARGET_ENDIANNESS=$ELF_ENDIANNESS
TARGET_FLOAT_ABI=$ELF_FLOAT_ABI
[ "$TARGET_WORD_BITS" -eq 0 ] && TARGET_WORD_BITS=$TARGET_ELF_CLASS_BITS

DYNAMIC_LOADER_STATUS=unknown
DYNAMIC_LOADER_FAMILY=unknown
if command -v readelf >/dev/null 2>&1 && is_readable_entry "$SHELL_ELF"; then
    probe_loader=$(readelf -l "$SHELL_ELF" 2>/dev/null | sed -n 's/.*Requesting program interpreter: \([^]]*\).*/\1/p' | sed -n '1p')
    if [ -n "$probe_loader" ]; then
        DYNAMIC_LOADER_STATUS=available
        case "$probe_loader" in
            *musl*) DYNAMIC_LOADER_FAMILY=musl ;;
            *uclibc*|*uClibc*) DYNAMIC_LOADER_FAMILY=uclibc ;;
            *ld-linux*) DYNAMIC_LOADER_FAMILY=glibc ;;
        esac
    fi
fi

LIBC_STATUS=unknown
LIBC_FAMILY=unknown
LIBC_VERSION=unknown
if command -v getconf >/dev/null 2>&1; then
    probe_libc=$(getconf GNU_LIBC_VERSION 2>/dev/null || :)
    case "$probe_libc" in
        glibc\ *)
            LIBC_STATUS=available
            LIBC_FAMILY=glibc
            probe_libc_version=$(safe_numeric_version "$probe_libc")
            [ -n "$probe_libc_version" ] && LIBC_VERSION=$probe_libc_version
            ;;
    esac
fi
if [ "$LIBC_STATUS" = unknown ] && command -v ldd >/dev/null 2>&1; then
    probe_libc=$(ldd --version 2>&1 || :)
    case "$probe_libc" in
        *musl*) LIBC_STATUS=available; LIBC_FAMILY=musl ;;
        *uClibc*|*uclibc*) LIBC_STATUS=available; LIBC_FAMILY=uclibc ;;
        *GLIBC*|*glibc*|*GNU\ libc*) LIBC_STATUS=available; LIBC_FAMILY=glibc ;;
    esac
    probe_libc_version=$(safe_numeric_version "$probe_libc")
    [ -n "$probe_libc_version" ] && LIBC_VERSION=$probe_libc_version
fi
if [ "$LIBC_FAMILY" = unknown ] && [ "$DYNAMIC_LOADER_FAMILY" != unknown ]; then
    LIBC_FAMILY=$DYNAMIC_LOADER_FAMILY
fi

CPUINFO=$(root_path /proc/cpuinfo)
ARM_NEON=unknown
if is_readable_entry "$CPUINFO"; then
    if grep -qiE '(^|[[:space:]])(neon|asimd)([[:space:]]|$)' "$CPUINFO"; then
        ARM_NEON=yes
    else
        ARM_NEON=no
    fi
fi

LIBSTDCXX_PATH=$(first_libstdcxx || :)
LIBSTDCXX_STATUS=missing
LIBSTDCXX_MAX_GLIBCXX=unknown
if [ -n "$LIBSTDCXX_PATH" ]; then
    LIBSTDCXX_STATUS=available
    if command -v strings >/dev/null 2>&1 && command -v awk >/dev/null 2>&1; then
        probe_glibcxx=$(strings "$LIBSTDCXX_PATH" 2>/dev/null | awk '
            /^GLIBCXX_[0-9]+\.[0-9]+(\.[0-9]+)?$/ {
                value=$0; sub(/^GLIBCXX_/, "", value); count=split(value, part, ".")
                major=part[1]+0; minor=part[2]+0; patch=(count > 2 ? part[3]+0 : 0)
                if (!seen || major>best_major || (major==best_major && minor>best_minor) ||
                    (major==best_major && minor==best_minor && patch>best_patch)) {
                    seen=1; best=$0; best_major=major; best_minor=minor; best_patch=patch
                }
            }
            END { if (seen) print best }
        ')
        case "$probe_glibcxx" in
            GLIBCXX_[0-9]*.[0-9]*) LIBSTDCXX_MAX_GLIBCXX=$probe_glibcxx ;;
        esac
    fi
fi

ASOUND_CARDS=$(root_path /proc/asound/cards)
ASOUND_PCM=$(root_path /proc/asound/pcm)
ALSA_CARD_COUNT=0
ALSA_CAPTURE_ENDPOINT_COUNT=0
ALSA_PLAYBACK_ENDPOINT_COUNT=0
if is_readable_entry "$ASOUND_CARDS"; then
    ALSA_CARD_COUNT=$(grep -c '^[[:space:]]*[0-9][0-9]*[[:space:]]*\[' "$ASOUND_CARDS" || :)
fi
if is_readable_entry "$ASOUND_PCM"; then
    ALSA_CAPTURE_ENDPOINT_COUNT=$(grep -c 'capture' "$ASOUND_PCM" || :)
    ALSA_PLAYBACK_ENDPOINT_COUNT=$(grep -c 'playback' "$ASOUND_PCM" || :)
fi
ALSA_CAPTURE_NODE_COUNT=$(count_entries "$PROBE_ROOT"/dev/snd/pcmC*D*c)
ALSA_PLAYBACK_NODE_COUNT=$(count_entries "$PROBE_ROOT"/dev/snd/pcmC*D*p)
ALSA_CONTROL_NODE_COUNT=$(count_entries "$PROBE_ROOT"/dev/snd/controlC*)
ALSA_STATUS=missing
if [ "$ALSA_CARD_COUNT" -gt 0 ] || [ "$ALSA_CAPTURE_NODE_COUNT" -gt 0 ] || [ "$ALSA_PLAYBACK_NODE_COUNT" -gt 0 ]; then
    ALSA_STATUS=detected
fi
ALSA_FULL_DUPLEX_CANDIDATE=false
if { [ "$ALSA_CAPTURE_ENDPOINT_COUNT" -gt 0 ] || [ "$ALSA_CAPTURE_NODE_COUNT" -gt 0 ]; } &&
   { [ "$ALSA_PLAYBACK_ENDPOINT_COUNT" -gt 0 ] || [ "$ALSA_PLAYBACK_NODE_COUNT" -gt 0 ]; }; then
    ALSA_FULL_DUPLEX_CANDIDATE=true
fi

RK_MPI_AI_TEST=$(first_readable_candidate \
    "$PROBE_ROOT"/oem/usr/bin/rk_mpi_ai_test \
    "$PROBE_ROOT"/usr/bin/rk_mpi_ai_test \
    "$PROBE_ROOT"/bin/rk_mpi_ai_test || :)
RK_MPI_AO_TEST=$(first_readable_candidate \
    "$PROBE_ROOT"/oem/usr/bin/rk_mpi_ao_test \
    "$PROBE_ROOT"/usr/bin/rk_mpi_ao_test \
    "$PROBE_ROOT"/bin/rk_mpi_ao_test || :)
RK_MPI_PIPELINE_STRESS_TEST=$(first_readable_candidate \
    "$PROBE_ROOT"/oem/usr/bin/sample_ai_aenc_adec_ao_stresstest \
    "$PROBE_ROOT"/usr/bin/sample_ai_aenc_adec_ao_stresstest \
    "$PROBE_ROOT"/bin/sample_ai_aenc_adec_ao_stresstest || :)
ROCKIT_LIBRARY=$(first_readable_candidate \
    "$PROBE_ROOT"/oem/usr/lib/librockit.so \
    "$PROBE_ROOT"/usr/lib/librockit.so \
    "$PROBE_ROOT"/lib/librockit.so || :)
AI_VQE_CONFIG=$(first_readable_candidate \
    "$PROBE_ROOT"/oem/usr/share/vqefiles/config_aivqe.json \
    "$PROBE_ROOT"/usr/share/vqefiles/config_aivqe.json \
    "$PROBE_ROOT"/etc/vqefiles/config_aivqe.json || :)
AO_VQE_CONFIG=$(first_readable_candidate \
    "$PROBE_ROOT"/oem/usr/share/vqefiles/config_aovqe.json \
    "$PROBE_ROOT"/usr/share/vqefiles/config_aovqe.json \
    "$PROBE_ROOT"/etc/vqefiles/config_aovqe.json || :)

RK_MPI_AI_TEST_PRESENT=$(candidate_present "$RK_MPI_AI_TEST")
RK_MPI_AI_TEST_EXECUTABLE=$(candidate_executable "$RK_MPI_AI_TEST")
RK_MPI_AO_TEST_PRESENT=$(candidate_present "$RK_MPI_AO_TEST")
RK_MPI_AO_TEST_EXECUTABLE=$(candidate_executable "$RK_MPI_AO_TEST")
RK_MPI_PIPELINE_STRESS_TEST_PRESENT=$(candidate_present "$RK_MPI_PIPELINE_STRESS_TEST")
RK_MPI_PIPELINE_STRESS_TEST_EXECUTABLE=$(candidate_executable "$RK_MPI_PIPELINE_STRESS_TEST")
ROCKIT_LIBRARY_PRESENT=$(candidate_present "$ROCKIT_LIBRARY")
AI_VQE_CONFIG_PRESENT=$(candidate_present "$AI_VQE_CONFIG")
AO_VQE_CONFIG_PRESENT=$(candidate_present "$AO_VQE_CONFIG")

RK_MPI_STATUS=missing
if [ "$RK_MPI_AI_TEST_PRESENT" = true ] || [ "$RK_MPI_AO_TEST_PRESENT" = true ] ||
   [ "$ROCKIT_LIBRARY_PRESENT" = true ]; then
    RK_MPI_STATUS=candidate
fi
probe_elf "$ROCKIT_LIBRARY"
ROCKIT_ELF_INSPECTION=$ELF_INSPECTION
ROCKIT_ELF_CLASS_BITS=$ELF_CLASS_BITS
ROCKIT_ELF_MACHINE=$ELF_MACHINE
ROCKIT_ELF_FLOAT_ABI=$ELF_FLOAT_ABI
ROCKIT_ELF_NEEDED_COUNT=$ELF_NEEDED_COUNT
ROCKIT_ABI_COMPATIBILITY=$(elf_compatibility "$ROCKIT_ELF_INSPECTION" "$ROCKIT_ELF_MACHINE" "$ROCKIT_ELF_CLASS_BITS" "$ROCKIT_ELF_FLOAT_ABI")

FRAMEBUFFER_NODE_COUNT=$(count_entries "$PROBE_ROOT"/dev/fb*)
DRM_CARD_NODE_COUNT=$(count_entries "$PROBE_ROOT"/dev/dri/card*)
INPUT_EVENT_NODE_COUNT=$(count_entries "$PROBE_ROOT"/dev/input/event*)
VIDEO_NODE_COUNT=$(count_entries "$PROBE_ROOT"/dev/video*)
I2C_NODE_COUNT=$(count_entries "$PROBE_ROOT"/dev/i2c-*)
SPI_NODE_COUNT=$(count_entries "$PROBE_ROOT"/dev/spidev*)
WATCHDOG_PRESENT=false
if [ -e "$PROBE_ROOT/dev/watchdog" ] || [ -e "$PROBE_ROOT/dev/watchdog0" ]; then
    WATCHDOG_PRESENT=true
fi

GT911_DETECTED=false
INPUT_DEVICES=$(root_path /proc/bus/input/devices)
if is_readable_entry "$INPUT_DEVICES" && grep -qiE 'gt911|goodix[[:space:]-].*touch' "$INPUT_DEVICES"; then
    GT911_DETECTED=true
fi
ST7789_DETECTED=false
for probe_entry in "$PROBE_ROOT"/sys/bus/spi/devices/*/modalias; do
    if is_readable_entry "$probe_entry" && grep -qi 'st7789' "$probe_entry"; then
        ST7789_DETECTED=true
        break
    fi
done

if [ -z "$ROCKCHIP_3A_LIB" ]; then
    ROCKCHIP_3A_LIB=$(first_rockchip_3a_aec_library || :)
fi
ROCKCHIP_3A_COMMON_LIB=$(first_rockchip_3a_common_library || :)
ROCKCHIP_3A_DETECT_LIB=$(first_rockchip_3a_detect_library || :)
ROCKCHIP_3A_AEC_LIBRARY_PRESENT=$(candidate_present "$ROCKCHIP_3A_LIB")
ROCKCHIP_3A_COMMON_LIBRARY_PRESENT=$(candidate_present "$ROCKCHIP_3A_COMMON_LIB")
ROCKCHIP_3A_DETECT_LIBRARY_PRESENT=$(candidate_present "$ROCKCHIP_3A_DETECT_LIB")
ROCKCHIP_3A_HEADER_PRESENT=$(first_rockchip_header_present)
probe_elf "$ROCKCHIP_3A_LIB"
ROCKCHIP_ELF_INSPECTION=$ELF_INSPECTION
ROCKCHIP_ELF_CLASS_BITS=$ELF_CLASS_BITS
ROCKCHIP_ELF_MACHINE=$ELF_MACHINE
ROCKCHIP_ELF_FLOAT_ABI=$ELF_FLOAT_ABI
ROCKCHIP_ELF_NEEDED_COUNT=$ELF_NEEDED_COUNT
ROCKCHIP_ABI_COMPATIBILITY=$(elf_compatibility "$ROCKCHIP_ELF_INSPECTION" "$ROCKCHIP_ELF_MACHINE" "$ROCKCHIP_ELF_CLASS_BITS" "$ROCKCHIP_ELF_FLOAT_ABI")
probe_elf "$ROCKCHIP_3A_COMMON_LIB"
ROCKCHIP_COMMON_ELF_INSPECTION=$ELF_INSPECTION
ROCKCHIP_COMMON_ELF_CLASS_BITS=$ELF_CLASS_BITS
ROCKCHIP_COMMON_ELF_MACHINE=$ELF_MACHINE
ROCKCHIP_COMMON_ELF_FLOAT_ABI=$ELF_FLOAT_ABI
ROCKCHIP_COMMON_ELF_NEEDED_COUNT=$ELF_NEEDED_COUNT
ROCKCHIP_COMMON_ABI_COMPATIBILITY=$(elf_compatibility "$ROCKCHIP_COMMON_ELF_INSPECTION" "$ROCKCHIP_COMMON_ELF_MACHINE" "$ROCKCHIP_COMMON_ELF_CLASS_BITS" "$ROCKCHIP_COMMON_ELF_FLOAT_ABI")
ROCKCHIP_STATUS=missing
if [ "$ROCKCHIP_3A_AEC_LIBRARY_PRESENT" = true ] &&
   [ "$ROCKCHIP_3A_COMMON_LIBRARY_PRESENT" = true ]; then
    ROCKCHIP_STATUS=candidate
elif [ "$ROCKCHIP_3A_AEC_LIBRARY_PRESENT" = true ] ||
     [ "$ROCKCHIP_3A_COMMON_LIBRARY_PRESENT" = true ]; then
    ROCKCHIP_STATUS=incomplete
fi

if [ -z "$SNOWBOY_LIB" ]; then
    SNOWBOY_LIB=$(first_snowboy_library || :)
fi
if [ -z "$SNOWBOY_MODEL" ]; then
    SNOWBOY_MODEL=$(first_snowboy_model || :)
fi
SNOWBOY_LIBRARY_PRESENT=false
[ -n "$SNOWBOY_LIB" ] && is_readable_entry "$SNOWBOY_LIB" && SNOWBOY_LIBRARY_PRESENT=true
SNOWBOY_MODEL_PRESENT=false
[ -n "$SNOWBOY_MODEL" ] && is_readable_entry "$SNOWBOY_MODEL" && SNOWBOY_MODEL_PRESENT=true
probe_elf "$SNOWBOY_LIB"
SNOWBOY_ELF_INSPECTION=$ELF_INSPECTION
SNOWBOY_ELF_CLASS_BITS=$ELF_CLASS_BITS
SNOWBOY_ELF_MACHINE=$ELF_MACHINE
SNOWBOY_ELF_FLOAT_ABI=$ELF_FLOAT_ABI
SNOWBOY_ELF_NEEDED_COUNT=$ELF_NEEDED_COUNT
SNOWBOY_ABI_COMPATIBILITY=$(elf_compatibility "$SNOWBOY_ELF_INSPECTION" "$SNOWBOY_ELF_MACHINE" "$SNOWBOY_ELF_CLASS_BITS" "$SNOWBOY_ELF_FLOAT_ABI")
SNOWBOY_STATUS=missing
if [ "$SNOWBOY_LIBRARY_PRESENT" = true ] || [ "$SNOWBOY_MODEL_PRESENT" = true ]; then
    SNOWBOY_STATUS=candidate
fi

WIFI_INTERFACE_COUNT=0
WIFI_LINK_UP_COUNT=0
ETHERNET_CANDIDATE_COUNT=0
ETHERNET_LINK_UP_COUNT=0
for probe_entry in "$PROBE_ROOT"/sys/class/net/*; do
    [ -d "$probe_entry" ] || continue
    probe_if_type=$(cat "$probe_entry/type" 2>/dev/null || :)
    probe_if_state=$(cat "$probe_entry/operstate" 2>/dev/null || :)
    if [ -d "$probe_entry/wireless" ]; then
        WIFI_INTERFACE_COUNT=$((WIFI_INTERFACE_COUNT + 1))
        [ "$probe_if_state" = up ] && WIFI_LINK_UP_COUNT=$((WIFI_LINK_UP_COUNT + 1))
    elif [ "$probe_if_type" = 1 ] && [ "${probe_entry##*/}" != lo ]; then
        ETHERNET_CANDIDATE_COUNT=$((ETHERNET_CANDIDATE_COUNT + 1))
        [ "$probe_if_state" = up ] && ETHERNET_LINK_UP_COUNT=$((ETHERNET_LINK_UP_COUNT + 1))
    fi
done

WIFI_IW_QUERY=missing_dependency
WIFI_STA_MODE=unknown
WIFI_AP_MODE=unknown
if command -v iw >/dev/null 2>&1; then
    probe_iw_list=$(iw list 2>/dev/null)
    probe_iw_status=$?
    if [ "$probe_iw_status" -eq 0 ]; then
        WIFI_IW_QUERY=available
        if printf '%s\n' "$probe_iw_list" | grep -Eq '^[[:space:]]*\*[[:space:]]+managed([[:space:]]|$)'; then
            WIFI_STA_MODE=listed
        else
            WIFI_STA_MODE=not_listed
        fi
        if printf '%s\n' "$probe_iw_list" | grep -Eq '^[[:space:]]*\*[[:space:]]+AP([[:space:]]|$)'; then
            WIFI_AP_MODE=listed
        else
            WIFI_AP_MODE=not_listed
        fi
    else
        WIFI_IW_QUERY=failed
    fi
fi

DHCP_CLIENT_STATUS=missing
if command -v udhcpc >/dev/null 2>&1 || command -v dhclient >/dev/null 2>&1; then
    DHCP_CLIENT_STATUS=available
fi

UI_STATUS=missing
if [ "$FRAMEBUFFER_NODE_COUNT" -gt 0 ] || [ "$DRM_CARD_NODE_COUNT" -gt 0 ]; then
    UI_STATUS=candidate
fi
UI_TOUCH_CANDIDATE=false
[ "$INPUT_EVENT_NODE_COUNT" -gt 0 ] && UI_TOUCH_CANDIDATE=true

COLLECTED_AT_UTC=$(date -u '+%Y-%m-%dT%H:%M:%SZ' 2>/dev/null || :)
case "$COLLECTED_AT_UTC" in
    ????-??-??T??:??:??Z) ;;
    *) COLLECTED_AT_UTC=unknown ;;
esac

cat <<EOF
{
  "schema_version": 2,
  "probe": "boompi-rv1106-p0",
  "mode": "read_only",
  "collected_at_utc": "$COLLECTED_AT_UTC",
  "privacy": {
    "network_identifiers": "not_collected",
    "credentials": "not_collected",
    "hostnames": "not_collected",
    "paths": "not_emitted"
  },
  "target": {
    "os": "$TARGET_OS",
    "kernel_version": "$KERNEL_VERSION",
    "machine": "$TARGET_MACHINE",
    "word_bits": $TARGET_WORD_BITS,
    "endianness": "$TARGET_ENDIANNESS",
    "float_abi": "$TARGET_FLOAT_ABI",
    "arm_neon": "$ARM_NEON",
    "elf_inspection": "$TARGET_ELF_INSPECTION",
    "dynamic_loader": {"status": "$DYNAMIC_LOADER_STATUS", "family": "$DYNAMIC_LOADER_FAMILY"},
    "libc": {"status": "$LIBC_STATUS", "family": "$LIBC_FAMILY", "version": "$LIBC_VERSION"},
    "libstdcxx": {"status": "$LIBSTDCXX_STATUS", "max_glibcxx": "$LIBSTDCXX_MAX_GLIBCXX"}
  },
  "tools": {
    "readelf": "$(tool_status readelf)",
    "file": "$(tool_status file)",
    "strings": "$(tool_status strings)",
    "aplay": "$(tool_status aplay)",
    "arecord": "$(tool_status arecord)",
    "amixer": "$(tool_status amixer)",
    "iw": "$(tool_status iw)",
    "hostapd": "$(tool_status hostapd)",
    "wpa_supplicant": "$(tool_status wpa_supplicant)",
    "fbset": "$(tool_status fbset)",
    "modetest": "$(tool_status modetest)"
  },
  "alsa": {
    "status": "$ALSA_STATUS",
    "card_count": $ALSA_CARD_COUNT,
    "capture_endpoint_count": $ALSA_CAPTURE_ENDPOINT_COUNT,
    "playback_endpoint_count": $ALSA_PLAYBACK_ENDPOINT_COUNT,
    "capture_node_count": $ALSA_CAPTURE_NODE_COUNT,
    "playback_node_count": $ALSA_PLAYBACK_NODE_COUNT,
    "control_node_count": $ALSA_CONTROL_NODE_COUNT,
    "full_duplex_candidate": $ALSA_FULL_DUPLEX_CANDIDATE,
    "rate_48000_full_duplex_verified": "unknown",
    "capture_channel_count_verified": "unknown",
    "capture_channel_layout_verified": "unknown",
    "digital_reference_position_verified": "unknown",
    "microphone_polarity_verified": "unknown"
  },
  "devices": {
    "framebuffer_node_count": $FRAMEBUFFER_NODE_COUNT,
    "drm_card_node_count": $DRM_CARD_NODE_COUNT,
    "input_event_node_count": $INPUT_EVENT_NODE_COUNT,
    "video_node_count": $VIDEO_NODE_COUNT,
    "i2c_node_count": $I2C_NODE_COUNT,
    "spi_node_count": $SPI_NODE_COUNT,
    "watchdog_present": $WATCHDOG_PRESENT,
    "gt911_detected": $GT911_DETECTED,
    "st7789_detected": $ST7789_DETECTED
  },
  "vendor": {
    "rockchip_mpi_audio": {
      "status": "$RK_MPI_STATUS",
      "rockit_library_present": $ROCKIT_LIBRARY_PRESENT,
      "ai_test_present": $RK_MPI_AI_TEST_PRESENT,
      "ai_test_executable": $RK_MPI_AI_TEST_EXECUTABLE,
      "ao_test_present": $RK_MPI_AO_TEST_PRESENT,
      "ao_test_executable": $RK_MPI_AO_TEST_EXECUTABLE,
      "encoded_pipeline_stress_sample_present": $RK_MPI_PIPELINE_STRESS_TEST_PRESENT,
      "encoded_pipeline_stress_sample_executable": $RK_MPI_PIPELINE_STRESS_TEST_EXECUTABLE,
      "ai_vqe_config_present": $AI_VQE_CONFIG_PRESENT,
      "ao_vqe_config_present": $AO_VQE_CONFIG_PRESENT,
      "rockit_elf_inspection": "$ROCKIT_ELF_INSPECTION",
      "rockit_elf_class_bits": $ROCKIT_ELF_CLASS_BITS,
      "rockit_elf_machine": "$ROCKIT_ELF_MACHINE",
      "rockit_elf_float_abi": "$ROCKIT_ELF_FLOAT_ABI",
      "rockit_needed_count": $ROCKIT_ELF_NEEDED_COUNT,
      "rockit_target_abi_compatibility": "$ROCKIT_ABI_COMPATIBILITY",
      "rate_48000_full_duplex_verified": "unknown",
      "capture_layout_verified": "unknown",
      "vqe_initialized": "unknown"
    },
    "rockchip_3a": {
      "status": "$ROCKCHIP_STATUS",
      "aec_library_present": $ROCKCHIP_3A_AEC_LIBRARY_PRESENT,
      "common_library_present": $ROCKCHIP_3A_COMMON_LIBRARY_PRESENT,
      "detect_library_present": $ROCKCHIP_3A_DETECT_LIBRARY_PRESENT,
      "header_present": $ROCKCHIP_3A_HEADER_PRESENT,
      "elf_inspection": "$ROCKCHIP_ELF_INSPECTION",
      "elf_class_bits": $ROCKCHIP_ELF_CLASS_BITS,
      "elf_machine": "$ROCKCHIP_ELF_MACHINE",
      "elf_float_abi": "$ROCKCHIP_ELF_FLOAT_ABI",
      "needed_count": $ROCKCHIP_ELF_NEEDED_COUNT,
      "aec_target_abi_compatibility": "$ROCKCHIP_ABI_COMPATIBILITY",
      "common_elf_inspection": "$ROCKCHIP_COMMON_ELF_INSPECTION",
      "common_elf_class_bits": $ROCKCHIP_COMMON_ELF_CLASS_BITS,
      "common_elf_machine": "$ROCKCHIP_COMMON_ELF_MACHINE",
      "common_elf_float_abi": "$ROCKCHIP_COMMON_ELF_FLOAT_ABI",
      "common_needed_count": $ROCKCHIP_COMMON_ELF_NEEDED_COUNT,
      "common_target_abi_compatibility": "$ROCKCHIP_COMMON_ABI_COMPATIBILITY",
      "api_abi_verified": "unknown",
      "realtime_16khz_verified": "unknown"
    },
    "snowboy": {
      "status": "$SNOWBOY_STATUS",
      "library_present": $SNOWBOY_LIBRARY_PRESENT,
      "model_present": $SNOWBOY_MODEL_PRESENT,
      "elf_inspection": "$SNOWBOY_ELF_INSPECTION",
      "elf_class_bits": $SNOWBOY_ELF_CLASS_BITS,
      "elf_machine": "$SNOWBOY_ELF_MACHINE",
      "elf_float_abi": "$SNOWBOY_ELF_FLOAT_ABI",
      "needed_count": $SNOWBOY_ELF_NEEDED_COUNT,
      "target_abi_compatibility": "$SNOWBOY_ABI_COMPATIBILITY",
      "model_load_verified": "unknown",
      "redistribution_license_verified": "unknown"
    }
  },
  "ui": {
    "status": "$UI_STATUS",
    "framebuffer_candidate": $([ "$FRAMEBUFFER_NODE_COUNT" -gt 0 ] && printf true || printf false),
    "drm_candidate": $([ "$DRM_CARD_NODE_COUNT" -gt 0 ] && printf true || printf false),
    "touch_candidate": $UI_TOUCH_CANDIDATE,
    "pixel_format_verified": "unknown",
    "rotation_verified": "unknown",
    "refresh_path_verified": "unknown"
  },
  "network": {
    "identifiers": "not_collected",
    "ethernet": {
      "candidate_interface_count": $ETHERNET_CANDIDATE_COUNT,
      "link_up_count": $ETHERNET_LINK_UP_COUNT
    },
    "wifi": {
      "interface_count": $WIFI_INTERFACE_COUNT,
      "link_up_count": $WIFI_LINK_UP_COUNT,
      "iw_query": "$WIFI_IW_QUERY",
      "sta_mode": "$WIFI_STA_MODE",
      "ap_mode": "$WIFI_AP_MODE",
      "ap_sta_concurrency": "unknown",
      "hostapd": "$(tool_status hostapd)",
      "wpa_supplicant": "$(tool_status wpa_supplicant)",
      "dhcp_client": "$DHCP_CLIENT_STATUS"
    }
  },
  "validation": {
    "alsa_devices_opened": false,
    "vendor_audio_binaries_executed": false,
    "vqe_initialized": false,
    "wifi_scan_performed": false,
    "functional_hardware_validation_performed": false
  }
}
EOF

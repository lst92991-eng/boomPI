#ifndef BOOMPI_ROCKCHIP_MPI_HIL_PINSET_SHA256
#error "BOOMPI_ROCKCHIP_MPI_HIL_PINSET_SHA256 must be provided by CMake"
#endif

#ifndef BOOMPI_ROCKCHIP_MPI_HIL_SOURCE_SHA256
#error "BOOMPI_ROCKCHIP_MPI_HIL_SOURCE_SHA256 must be provided by CMake"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <exception>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>

#include "rk_comm_aio.h"
#include "rk_mpi_ai.h"
#include "rk_mpi_ao.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_sys.h"

namespace {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

constexpr RK_U32 kSampleRate = 48000;
constexpr RK_U32 kChannels = 2;
constexpr RK_U32 kPointsPerFrame = 1024;
constexpr RK_U32 kDeviceFrameCount = 4;
constexpr RK_U32 kExtraFlags = 0;
constexpr RK_U32 kChannelCount = 2;
constexpr RK_S32 kApiTimeoutMs = 100;
constexpr RK_S32 kWaitEosTimeoutMs = 500;
constexpr std::chrono::milliseconds kActivityBucketWidth(100);
constexpr std::size_t kActivityBucketCount = 64;
constexpr std::size_t kMinimumCommonSuccessBuckets = 30;
constexpr std::chrono::seconds kCaptureWindow(6);
constexpr std::chrono::seconds kPlaybackHardLimit(6);
constexpr std::size_t kPlaybackPayloadBytes = 1024;
constexpr std::uint64_t kPlaybackPayloadCount = 750;
constexpr std::uint64_t kPlaybackTotalBytes =
    kPlaybackPayloadBytes * kPlaybackPayloadCount;
constexpr unsigned kMaximumConsecutiveErrors = 10;

static_assert(kPlaybackTotalBytes == 768000, "fixed AO byte count changed");
static_assert(kActivityBucketWidth.count() == 100,
              "activity evidence must use fixed 100 ms buckets");
static_assert(kMinimumCommonSuccessBuckets * kActivityBucketWidth.count() ==
                  3000,
              "qualified common activity must cover three seconds");

volatile std::sig_atomic_t g_signal_requested = 0;

extern "C" void HandleSignal(int /*signal_number*/) {
  g_signal_requested = 1;
}

struct DirectPcmAddress {
  unsigned card = 0;
  unsigned device = 0;
};

struct Options {
  bool help = false;
  bool execute = false;
  bool allow_ai_capture = false;
  bool allow_ao_playback = false;
  std::string ai_card;
  std::string ao_card;
  DirectPcmAddress ai_pcm;
  DirectPcmAddress ao_pcm;
  RK_S32 ai_device = 0;
  RK_S32 ai_channel = 0;
  RK_S32 ao_device = 0;
  RK_S32 ao_channel = 0;
  std::string artifact_dir;
};

class ScopedFd {
 public:
  explicit ScopedFd(int fd) : fd_(fd) {}
  ~ScopedFd() {
    if (fd_ >= 0) {
      close(fd_);
    }
  }

  ScopedFd(const ScopedFd&) = delete;
  ScopedFd& operator=(const ScopedFd&) = delete;

  int get() const { return fd_; }

  bool Close(std::string* error) {
    if (fd_ < 0) {
      return true;
    }
    const int closing_fd = fd_;
    fd_ = -1;
    if (close(closing_fd) != 0) {
      *error = "cannot close artifact directory dirfd: " +
               std::string(std::strerror(errno));
      return false;
    }
    return true;
  }

 private:
  int fd_;
};

struct CallResult {
  bool called = false;
  RK_S32 rc = 0;
};

struct Lifecycle {
  bool sys_initialized = false;
  bool ai_enabled = false;
  bool ai_channel_enabled = false;
  bool ao_enabled = false;
  bool ao_channel_enabled = false;
};

struct InitResults {
  CallResult sys_init;
  CallResult ai_set_pub_attr;
  CallResult ai_enable;
  CallResult ai_set_channel_param;
  CallResult ai_enable_channel;
  CallResult ao_set_pub_attr;
  CallResult ao_enable;
  CallResult ao_set_channel_param;
  CallResult ao_enable_channel;
};

struct CleanupResults {
  CallResult ao_disable_channel;
  CallResult ao_disable;
  CallResult ai_disable_channel;
  CallResult ai_disable;
  CallResult sys_exit;
  bool passed = true;
  bool has_first_error = false;
  std::string first_error_step;
  RK_S32 first_error_code = 0;
};

struct ErrorCounter {
  std::uint64_t count = 0;
  unsigned consecutive = 0;
  unsigned maximum_consecutive = 0;
  bool has_code = false;
  RK_S32 first_code = 0;
  RK_S32 last_code = 0;
};

struct SignedRange {
  bool observed = false;
  std::int64_t minimum = 0;
  std::int64_t maximum = 0;
};

struct UnsignedRange {
  bool observed = false;
  std::uint64_t minimum = 0;
  std::uint64_t maximum = 0;
};

struct ActivityBuckets {
  std::array<RK_U8, kActivityBucketCount> success {};
  std::array<RK_U8, kActivityBucketCount> error {};
  std::uint64_t success_out_of_range = 0;
  std::uint64_t error_out_of_range = 0;
};

struct AiStats {
  bool thread_ready = false;
  bool start_gate_passed = false;
  bool thread_joined = false;
  bool completed_window = false;
  bool fatal = false;
  bool thread_exception = false;
  std::int64_t loop_start_us = 0;
  std::int64_t loop_end_us = 0;
  std::int64_t first_success_begin_us = 0;
  std::int64_t last_success_end_us = 0;
  bool has_success_interval = false;
  std::uint64_t get_calls = 0;
  std::uint64_t successful_gets = 0;
  std::uint64_t validated_frames = 0;
  std::uint64_t release_calls = 0;
  std::uint64_t successful_releases = 0;
  std::uint64_t invalid_handles = 0;
  std::uint64_t null_addresses = 0;
  std::uint64_t zero_capacities = 0;
  std::uint64_t zero_lengths = 0;
  std::uint64_t len_equals_capacity = 0;
  std::uint64_t len_times_channels_equals_capacity = 0;
  std::uint64_t other_len_capacity_relationship = 0;
  ErrorCounter get_errors;
  ErrorCounter release_errors;
  SignedRange bit_width;
  SignedRange sound_mode;
  SignedRange sample_rate;
  UnsignedRange sequence;
  UnsignedRange length;
  UnsignedRange capacity;
  ActivityBuckets activity;
};

struct AoStats {
  bool thread_ready = false;
  bool start_gate_passed = false;
  bool thread_joined = false;
  bool fatal = false;
  bool thread_exception = false;
  bool hard_limit_reached = false;
  std::int64_t loop_start_us = 0;
  std::int64_t loop_end_us = 0;
  std::int64_t first_success_begin_us = 0;
  std::int64_t last_success_end_us = 0;
  bool has_success_interval = false;
  std::uint64_t create_calls = 0;
  std::uint64_t created_blocks = 0;
  std::uint64_t invalid_created_handles = 0;
  std::uint64_t send_calls = 0;
  std::uint64_t successful_sends = 0;
  std::uint64_t successful_bytes = 0;
  std::uint64_t release_calls = 0;
  std::uint64_t successful_releases = 0;
  ErrorCounter create_errors;
  ErrorCounter send_errors;
  ErrorCounter release_errors;
  ActivityBuckets activity;
  CallResult eos_create;
  bool eos_block_created = false;
  CallResult eos_send;
  CallResult eos_release;
  CallResult wait_eos;
  bool eos_unsupported = false;
};

struct StartGate {
  std::mutex mutex;
  std::condition_variable condition;
  unsigned ready_threads = 0;
  bool go = false;
  bool cancelled = false;
  TimePoint start_time{};
};

struct ProcScan {
  bool attempted = false;
  bool complete = true;
  bool busy = false;
  long observed_pid = -1;
  std::string occupied_node;
  std::string error;
};

struct Report {
  std::string precondition_status = "not_run";
  std::string precondition_detail;
  ProcScan first_scan;
  ProcScan second_scan;
  InitResults init;
  CleanupResults cleanup;
  AiStats ai;
  AoStats ao;
  bool start_gate_failed = false;
  bool signal_observed = false;
  bool initialization_passed = false;
  std::size_t longest_common_success_buckets = 0;
  std::int64_t qualified_common_success_us = 0;
  std::string transport_status = "not_run";
  std::string eos_status = "not_attempted";
  std::string cleanup_status = "not_attempted";
  std::string probe_status = "fail";
};

void RecordError(ErrorCounter* counter, RK_S32 code) {
  ++counter->count;
  ++counter->consecutive;
  counter->maximum_consecutive =
      std::max(counter->maximum_consecutive, counter->consecutive);
  if (!counter->has_code) {
    counter->has_code = true;
    counter->first_code = code;
  }
  counter->last_code = code;
}

void ClearConsecutive(ErrorCounter* counter) {
  counter->consecutive = 0;
}

void Observe(SignedRange* range, std::int64_t value) {
  if (!range->observed) {
    range->observed = true;
    range->minimum = value;
    range->maximum = value;
    return;
  }
  range->minimum = std::min(range->minimum, value);
  range->maximum = std::max(range->maximum, value);
}

void Observe(UnsignedRange* range, std::uint64_t value) {
  if (!range->observed) {
    range->observed = true;
    range->minimum = value;
    range->maximum = value;
    return;
  }
  range->minimum = std::min(range->minimum, value);
  range->maximum = std::max(range->maximum, value);
}

std::int64_t ToMicroseconds(TimePoint point) {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             point.time_since_epoch())
      .count();
}

std::int64_t ElapsedMicroseconds(TimePoint start, TimePoint end) {
  return std::chrono::duration_cast<std::chrono::microseconds>(end - start)
      .count();
}

void MarkActivityBucket(ActivityBuckets* buckets,
                        TimePoint common_start,
                        TimePoint event_time,
                        bool success) {
  const std::int64_t elapsed_us =
      ElapsedMicroseconds(common_start, event_time);
  constexpr std::int64_t kBucketWidthUs =
      std::chrono::duration_cast<std::chrono::microseconds>(
          kActivityBucketWidth)
          .count();
  if (elapsed_us < 0) {
    if (success) {
      ++buckets->success_out_of_range;
    } else {
      ++buckets->error_out_of_range;
    }
    return;
  }
  const std::uint64_t bucket =
      static_cast<std::uint64_t>(elapsed_us / kBucketWidthUs);
  if (bucket >= kActivityBucketCount) {
    if (success) {
      ++buckets->success_out_of_range;
    } else {
      ++buckets->error_out_of_range;
    }
    return;
  }
  std::array<RK_U8, kActivityBucketCount>& destination =
      success ? buckets->success : buckets->error;
  destination[static_cast<std::size_t>(bucket)] = 1;
}

bool StopRequested(const std::atomic<bool>& stop_requested) {
  return stop_requested.load(std::memory_order_relaxed) ||
         g_signal_requested != 0;
}

bool IsCanonicalUnsigned(const std::string& text,
                         std::uint64_t maximum,
                         std::uint64_t* value) {
  if (text.empty() || (text.size() > 1 && text.front() == '0')) {
    return false;
  }
  std::uint64_t parsed = 0;
  for (char character : text) {
    if (character < '0' || character > '9') {
      return false;
    }
    const std::uint64_t digit = static_cast<unsigned>(character - '0');
    if (parsed > (maximum - digit) / 10U) {
      return false;
    }
    parsed = parsed * 10U + digit;
  }
  *value = parsed;
  return true;
}

bool ParseDirectPcm(const std::string& text, DirectPcmAddress* address) {
  constexpr char kPrefix[] = "hw:";
  if (text.compare(0, sizeof(kPrefix) - 1, kPrefix) != 0) {
    return false;
  }
  const std::size_t comma = text.find(',', sizeof(kPrefix) - 1);
  if (comma == std::string::npos ||
      text.find(',', comma + 1) != std::string::npos) {
    return false;
  }
  const std::string card =
      text.substr(sizeof(kPrefix) - 1, comma - (sizeof(kPrefix) - 1));
  const std::string device = text.substr(comma + 1);
  std::uint64_t parsed_card = 0;
  std::uint64_t parsed_device = 0;
  if (!IsCanonicalUnsigned(card, UINT_MAX, &parsed_card) ||
      !IsCanonicalUnsigned(device, UINT_MAX, &parsed_device)) {
    return false;
  }
  address->card = static_cast<unsigned>(parsed_card);
  address->device = static_cast<unsigned>(parsed_device);
  return true;
}

bool ParseI32(const std::string& text, RK_S32* value) {
  std::uint64_t parsed = 0;
  if (!IsCanonicalUnsigned(
          text, static_cast<std::uint64_t>(std::numeric_limits<RK_S32>::max()),
          &parsed)) {
    return false;
  }
  *value = static_cast<RK_S32>(parsed);
  return true;
}

bool IsLexicallySafeAbsolutePath(const std::string& path,
                                 std::string* error) {
  if (path.empty() || path.front() != '/') {
    *error = "artifact directory must be an absolute POSIX path";
    return false;
  }
  if (path == "/" || path.back() == '/' || path.size() >= PATH_MAX - 32U) {
    *error = "artifact directory must name a bounded, non-root leaf";
    return false;
  }
  for (char character : path) {
    const bool allowed =
        (character >= 'a' && character <= 'z') ||
        (character >= 'A' && character <= 'Z') ||
        (character >= '0' && character <= '9') || character == '_' ||
        character == '-' || character == '.' || character == '/';
    if (!allowed) {
      *error = "artifact directory contains a disallowed character";
      return false;
    }
  }
  std::size_t begin = 1;
  while (begin <= path.size()) {
    const std::size_t end = path.find('/', begin);
    const std::string component =
        path.substr(begin, end == std::string::npos ? std::string::npos
                                                    : end - begin);
    if (component.empty() || component == "." || component == "..") {
      *error = "artifact directory is not lexically canonical";
      return false;
    }
    if (end == std::string::npos) {
      break;
    }
    begin = end + 1;
  }
  return true;
}

std::string Usage(const char* executable) {
  std::ostringstream stream;
  stream
      << "Usage: " << executable
      << " --ai-card hw:C,D --ao-card hw:C,D"
         " --ai-device N --ai-channel N --ao-device N --ao-channel N"
         " --artifact-dir /absolute/new/directory"
         " [--execute --allow-ai-capture --allow-ao-playback]\n";
  return stream.str();
}

bool ParseArguments(int argc,
                    char** argv,
                    Options* options,
                    std::string* error) {
  bool ai_card_seen = false;
  bool ao_card_seen = false;
  bool ai_device_seen = false;
  bool ai_channel_seen = false;
  bool ao_device_seen = false;
  bool ao_channel_seen = false;
  bool artifact_dir_seen = false;

  for (int index = 1; index < argc; ++index) {
    const std::string argument(argv[index]);
    if (argument == "--help") {
      options->help = true;
      continue;
    }
    if (argument == "--execute" || argument == "--allow-ai-capture" ||
        argument == "--allow-ao-playback") {
      bool* flag = nullptr;
      if (argument == "--execute") {
        flag = &options->execute;
      } else if (argument == "--allow-ai-capture") {
        flag = &options->allow_ai_capture;
      } else {
        flag = &options->allow_ao_playback;
      }
      if (*flag) {
        *error = "duplicate flag: " + argument;
        return false;
      }
      *flag = true;
      continue;
    }

    bool* seen = nullptr;
    std::string* string_destination = nullptr;
    RK_S32* integer_destination = nullptr;
    if (argument == "--ai-card") {
      seen = &ai_card_seen;
      string_destination = &options->ai_card;
    } else if (argument == "--ao-card") {
      seen = &ao_card_seen;
      string_destination = &options->ao_card;
    } else if (argument == "--ai-device") {
      seen = &ai_device_seen;
      integer_destination = &options->ai_device;
    } else if (argument == "--ai-channel") {
      seen = &ai_channel_seen;
      integer_destination = &options->ai_channel;
    } else if (argument == "--ao-device") {
      seen = &ao_device_seen;
      integer_destination = &options->ao_device;
    } else if (argument == "--ao-channel") {
      seen = &ao_channel_seen;
      integer_destination = &options->ao_channel;
    } else if (argument == "--artifact-dir") {
      seen = &artifact_dir_seen;
      string_destination = &options->artifact_dir;
    } else {
      *error = "unknown argument: " + argument;
      return false;
    }

    if (*seen) {
      *error = "duplicate option: " + argument;
      return false;
    }
    *seen = true;
    if (++index >= argc) {
      *error = "missing value for: " + argument;
      return false;
    }
    const std::string value(argv[index]);
    if (string_destination != nullptr) {
      *string_destination = value;
    } else if (!ParseI32(value, integer_destination)) {
      *error = "invalid canonical non-negative integer for: " + argument;
      return false;
    }
  }

  if (options->help) {
    return true;
  }
  if (!ai_card_seen || !ao_card_seen || !ai_device_seen ||
      !ai_channel_seen || !ao_device_seen || !ao_channel_seen ||
      !artifact_dir_seen) {
    *error = "all card, device, channel, and artifact options are required";
    return false;
  }
  if (!ParseDirectPcm(options->ai_card, &options->ai_pcm) ||
      !ParseDirectPcm(options->ao_card, &options->ao_pcm)) {
    *error = "cards must be canonical direct ALSA addresses of the form hw:C,D";
    return false;
  }
  if (options->ai_card.size() >= sizeof(AIO_ATTR_S{}.u8CardName) ||
      options->ao_card.size() >= sizeof(AIO_ATTR_S{}.u8CardName)) {
    *error = "card address does not fit the vendor AIO attribute";
    return false;
  }
  if (!IsLexicallySafeAbsolutePath(options->artifact_dir, error)) {
    return false;
  }
  if (options->execute &&
      (!options->allow_ai_capture || !options->allow_ao_playback)) {
    *error = "execution requires both explicit AI capture and AO playback opt-ins";
    return false;
  }
  if (!options->execute &&
      (options->allow_ai_capture || options->allow_ao_playback)) {
    *error = "hardware opt-ins are only valid together with --execute";
    return false;
  }
  return true;
}

std::string CaptureDeviceNode(const DirectPcmAddress& address) {
  char buffer[96] = {};
  std::snprintf(buffer, sizeof(buffer), "/dev/snd/pcmC%uD%uc", address.card,
                address.device);
  return std::string(buffer);
}

std::string PlaybackDeviceNode(const DirectPcmAddress& address) {
  char buffer[96] = {};
  std::snprintf(buffer, sizeof(buffer), "/dev/snd/pcmC%uD%up", address.card,
                address.device);
  return std::string(buffer);
}

bool ValidateCharacterDevice(const std::string& path, std::string* error) {
  struct stat status {};
  if (lstat(path.c_str(), &status) != 0) {
    *error = "cannot lstat required PCM node " + path + ": " +
             std::strerror(errno);
    return false;
  }
  if (!S_ISCHR(status.st_mode)) {
    *error = "required PCM node is not a character device: " + path;
    return false;
  }
  return true;
}

bool PrecheckArtifactDirectory(const std::string& path, std::string* error) {
  const std::size_t slash = path.find_last_of('/');
  const std::string parent = slash == 0 ? "/" : path.substr(0, slash);
  char resolved[PATH_MAX] = {};
  if (realpath(parent.c_str(), resolved) == nullptr) {
    *error = "cannot resolve artifact parent: " + std::string(std::strerror(errno));
    return false;
  }
  if (parent != resolved) {
    *error = "artifact parent must already be canonical and contain no symlink hop";
    return false;
  }
  struct stat parent_status {};
  if (lstat(parent.c_str(), &parent_status) != 0 ||
      !S_ISDIR(parent_status.st_mode) || S_ISLNK(parent_status.st_mode)) {
    *error = "artifact parent is not a real directory";
    return false;
  }
  if (access(parent.c_str(), W_OK | X_OK) != 0) {
    *error = "artifact parent is not writable/searchable";
    return false;
  }
  struct stat target_status {};
  if (lstat(path.c_str(), &target_status) == 0) {
    *error = "artifact directory already exists";
    return false;
  }
  if (errno != ENOENT) {
    *error = "cannot prove artifact directory is new: " +
             std::string(std::strerror(errno));
    return false;
  }
  return true;
}

bool CreateArtifactDirectory(const std::string& path,
                             int* directory_fd,
                             std::string* error) {
  const std::size_t slash = path.find_last_of('/');
  const std::string parent = slash == 0 ? "/" : path.substr(0, slash);
  const std::string leaf = path.substr(slash + 1);
  const int parent_fd =
      open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (parent_fd < 0) {
    *error = "cannot open canonical artifact parent: " +
             std::string(std::strerror(errno));
    return false;
  }

  struct stat existing_status {};
  if (fstatat(parent_fd, leaf.c_str(), &existing_status,
              AT_SYMLINK_NOFOLLOW) == 0) {
    *error = "artifact directory already exists";
    close(parent_fd);
    return false;
  }
  if (errno != ENOENT) {
    *error = "cannot prove artifact directory is new via parent dirfd: " +
             std::string(std::strerror(errno));
    close(parent_fd);
    return false;
  }

  const mode_t previous_umask = umask(077);
  const int rc = mkdirat(parent_fd, leaf.c_str(), 0700);
  const int saved_errno = errno;
  umask(previous_umask);
  if (rc != 0) {
    *error = "cannot create artifact directory: " +
             std::string(std::strerror(saved_errno));
    close(parent_fd);
    return false;
  }
  const int opened_fd = openat(parent_fd, leaf.c_str(),
                               O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  const int open_errno = errno;
  const int parent_close_rc = close(parent_fd);
  if (opened_fd < 0) {
    *error = "cannot open the newly created artifact directory: " +
             std::string(std::strerror(open_errno));
    return false;
  }
  if (parent_close_rc != 0) {
    *error = "cannot close canonical artifact parent dirfd";
    close(opened_fd);
    return false;
  }
  struct stat status {};
  if (fstat(opened_fd, &status) != 0 || !S_ISDIR(status.st_mode) ||
      (status.st_mode & 0777) != 0700) {
    *error = "created artifact path failed directory verification";
    close(opened_fd);
    return false;
  }
  *directory_fd = opened_fd;
  return true;
}

bool IsDecimalName(const char* name) {
  if (name == nullptr || *name == '\0') {
    return false;
  }
  for (const char* cursor = name; *cursor != '\0'; ++cursor) {
    if (*cursor < '0' || *cursor > '9') {
      return false;
    }
  }
  return true;
}

ProcScan ScanProcForPcmUsers(const std::string& capture_node,
                            const std::string& playback_node) {
  ProcScan result;
  result.attempted = true;
  DIR* proc = opendir("/proc");
  if (proc == nullptr) {
    result.complete = false;
    result.error = "cannot open /proc: " + std::string(std::strerror(errno));
    return result;
  }

  while (result.complete && !result.busy) {
    errno = 0;
    dirent* process_entry = readdir(proc);
    if (process_entry == nullptr) {
      if (errno != 0) {
        result.complete = false;
        result.error = "failed while enumerating /proc: " +
                       std::string(std::strerror(errno));
      }
      break;
    }
    if (!IsDecimalName(process_entry->d_name)) {
      continue;
    }
    char fd_directory[PATH_MAX] = {};
    const int path_length = std::snprintf(fd_directory, sizeof(fd_directory),
                                          "/proc/%s/fd", process_entry->d_name);
    if (path_length < 0 ||
        static_cast<std::size_t>(path_length) >= sizeof(fd_directory)) {
      result.complete = false;
      result.error = "a /proc fd path exceeded PATH_MAX";
      break;
    }
    DIR* fds = opendir(fd_directory);
    if (fds == nullptr) {
      if (errno == ENOENT || errno == ESRCH) {
        continue;
      }
      result.complete = false;
      result.error = "cannot inspect " + std::string(fd_directory) + ": " +
                     std::strerror(errno);
      break;
    }

    while (result.complete && !result.busy) {
      errno = 0;
      dirent* fd_entry = readdir(fds);
      if (fd_entry == nullptr) {
        if (errno != 0) {
          result.complete = false;
          result.error = "failed while enumerating a /proc fd directory: " +
                         std::string(std::strerror(errno));
        }
        break;
      }
      if (!IsDecimalName(fd_entry->d_name)) {
        continue;
      }
      char fd_path[PATH_MAX] = {};
      const int fd_path_length =
          std::snprintf(fd_path, sizeof(fd_path), "%s/%s", fd_directory,
                        fd_entry->d_name);
      if (fd_path_length < 0 ||
          static_cast<std::size_t>(fd_path_length) >= sizeof(fd_path)) {
        result.complete = false;
        result.error = "a /proc fd symlink path exceeded PATH_MAX";
        break;
      }
      char target[PATH_MAX] = {};
      const ssize_t target_length = readlink(fd_path, target, sizeof(target) - 1);
      if (target_length < 0) {
        if (errno == ENOENT || errno == ESRCH) {
          continue;
        }
        result.complete = false;
        result.error = "cannot inspect a /proc fd symlink: " +
                       std::string(std::strerror(errno));
        break;
      }
      if (static_cast<std::size_t>(target_length) == sizeof(target) - 1U) {
        result.complete = false;
        result.error = "a /proc fd symlink target may have been truncated";
        break;
      }
      target[target_length] = '\0';
      if (capture_node == target || playback_node == target) {
        result.busy = true;
        result.occupied_node = target;
        result.observed_pid = std::strtol(process_entry->d_name, nullptr, 10);
        break;
      }
    }
    if (closedir(fds) != 0 && result.complete) {
      result.complete = false;
      result.error = "failed to close a /proc fd directory";
    }
  }
  if (closedir(proc) != 0 && result.complete) {
    result.complete = false;
    result.error = "failed to close /proc";
  }
  return result;
}

bool InstallSignalHandlers(std::string* error) {
  struct sigaction action {};
  action.sa_handler = HandleSignal;
  sigemptyset(&action.sa_mask);
  action.sa_flags = 0;
  const int signals[] = {SIGINT, SIGTERM, SIGHUP, SIGQUIT};
  for (int signal_number : signals) {
    if (sigaction(signal_number, &action, nullptr) != 0) {
      *error = "cannot install signal handler: " +
               std::string(std::strerror(errno));
      return false;
    }
  }
  return true;
}

AIO_ATTR_S MakeAioAttributes(const std::string& card) {
  AIO_ATTR_S attributes {};
  attributes.soundCard.channels = kChannels;
  attributes.soundCard.sampleRate = kSampleRate;
  attributes.soundCard.bitWidth = AUDIO_BIT_WIDTH_16;
  attributes.enSamplerate = AUDIO_SAMPLE_RATE_48000;
  attributes.enBitwidth = AUDIO_BIT_WIDTH_16;
  attributes.enSoundmode = AUDIO_SOUND_MODE_STEREO;
  attributes.u32FrmNum = kDeviceFrameCount;
  attributes.u32PtNumPerFrm = kPointsPerFrame;
  attributes.u32EXFlag = kExtraFlags;
  attributes.u32ChnCnt = kChannelCount;
  std::snprintf(reinterpret_cast<char*>(attributes.u8CardName),
                sizeof(attributes.u8CardName), "%s", card.c_str());
  return attributes;
}

bool InitializeMpi(const Options& options,
                   InitResults* results,
                   Lifecycle* lifecycle) {
  results->sys_init.called = true;
  results->sys_init.rc = RK_MPI_SYS_Init();
  if (results->sys_init.rc != 0) {
    return false;
  }
  lifecycle->sys_initialized = true;

  AIO_ATTR_S ai_attributes = MakeAioAttributes(options.ai_card);
  results->ai_set_pub_attr.called = true;
  results->ai_set_pub_attr.rc =
      RK_MPI_AI_SetPubAttr(options.ai_device, &ai_attributes);
  if (results->ai_set_pub_attr.rc != 0) {
    return false;
  }
  results->ai_enable.called = true;
  results->ai_enable.rc = RK_MPI_AI_Enable(options.ai_device);
  if (results->ai_enable.rc != 0) {
    return false;
  }
  lifecycle->ai_enabled = true;

  AI_CHN_PARAM_S ai_channel_parameters {};
  ai_channel_parameters.s32UsrFrmDepth = kDeviceFrameCount;
  ai_channel_parameters.enLoopbackMode = AUDIO_LOOPBACK_NONE;
  results->ai_set_channel_param.called = true;
  results->ai_set_channel_param.rc = RK_MPI_AI_SetChnParam(
      options.ai_device, options.ai_channel, &ai_channel_parameters);
  if (results->ai_set_channel_param.rc != 0) {
    return false;
  }
  results->ai_enable_channel.called = true;
  results->ai_enable_channel.rc =
      RK_MPI_AI_EnableChn(options.ai_device, options.ai_channel);
  if (results->ai_enable_channel.rc != 0) {
    return false;
  }
  lifecycle->ai_channel_enabled = true;

  AIO_ATTR_S ao_attributes = MakeAioAttributes(options.ao_card);
  results->ao_set_pub_attr.called = true;
  results->ao_set_pub_attr.rc =
      RK_MPI_AO_SetPubAttr(options.ao_device, &ao_attributes);
  if (results->ao_set_pub_attr.rc != 0) {
    return false;
  }
  results->ao_enable.called = true;
  results->ao_enable.rc = RK_MPI_AO_Enable(options.ao_device);
  if (results->ao_enable.rc != 0) {
    return false;
  }
  lifecycle->ao_enabled = true;

  AO_CHN_PARAM_S ao_channel_parameters {};
  ao_channel_parameters.enLoopbackMode = AUDIO_LOOPBACK_NONE;
  results->ao_set_channel_param.called = true;
  results->ao_set_channel_param.rc = RK_MPI_AO_SetChnParams(
      options.ao_device, options.ao_channel, &ao_channel_parameters);
  if (results->ao_set_channel_param.rc != 0) {
    return false;
  }
  results->ao_enable_channel.called = true;
  results->ao_enable_channel.rc =
      RK_MPI_AO_EnableChn(options.ao_device, options.ao_channel);
  if (results->ao_enable_channel.rc != 0) {
    return false;
  }
  lifecycle->ao_channel_enabled = true;
  return true;
}

void RecordCleanup(const char* step,
                   CallResult* result,
                   RK_S32 rc,
                   CleanupResults* cleanup) {
  result->called = true;
  result->rc = rc;
  if (rc != 0) {
    cleanup->passed = false;
    if (!cleanup->has_first_error) {
      cleanup->has_first_error = true;
      cleanup->first_error_step = step;
      cleanup->first_error_code = rc;
    }
  }
}

void CleanupMpi(const Options& options,
                Lifecycle* lifecycle,
                CleanupResults* cleanup) {
  if (lifecycle->ao_channel_enabled) {
    RecordCleanup("ao_disable_channel", &cleanup->ao_disable_channel,
                  RK_MPI_AO_DisableChn(options.ao_device, options.ao_channel),
                  cleanup);
    lifecycle->ao_channel_enabled = false;
  }
  if (lifecycle->ao_enabled) {
    RecordCleanup("ao_disable", &cleanup->ao_disable,
                  RK_MPI_AO_Disable(options.ao_device), cleanup);
    lifecycle->ao_enabled = false;
  }
  if (lifecycle->ai_channel_enabled) {
    RecordCleanup("ai_disable_channel", &cleanup->ai_disable_channel,
                  RK_MPI_AI_DisableChn(options.ai_device, options.ai_channel),
                  cleanup);
    lifecycle->ai_channel_enabled = false;
  }
  if (lifecycle->ai_enabled) {
    RecordCleanup("ai_disable", &cleanup->ai_disable,
                  RK_MPI_AI_Disable(options.ai_device), cleanup);
    lifecycle->ai_enabled = false;
  }
  if (lifecycle->sys_initialized) {
    RecordCleanup("sys_exit", &cleanup->sys_exit, RK_MPI_SYS_Exit(), cleanup);
    lifecycle->sys_initialized = false;
  }
}

bool WaitAtStartGate(StartGate* gate,
                     bool* ready_marker,
                     TimePoint* start_time) {
  std::unique_lock<std::mutex> lock(gate->mutex);
  *ready_marker = true;
  ++gate->ready_threads;
  gate->condition.notify_all();
  gate->condition.wait(lock,
                       [gate] { return gate->go || gate->cancelled; });
  if (gate->cancelled) {
    return false;
  }
  *start_time = gate->start_time;
  return true;
}

void CaptureThread(const Options& options,
                   StartGate* gate,
                   std::atomic<bool>* stop_requested,
                   AiStats* stats) {
  TimePoint start_time;
  if (!WaitAtStartGate(gate, &stats->thread_ready, &start_time)) {
    return;
  }
  stats->start_gate_passed = true;
  stats->loop_start_us = ToMicroseconds(Clock::now());
  const TimePoint deadline = start_time + kCaptureWindow;

  while (!StopRequested(*stop_requested) && Clock::now() < deadline) {
    AUDIO_FRAME_S frame {};
    const TimePoint call_begin = Clock::now();
    ++stats->get_calls;
    const RK_S32 get_rc = RK_MPI_AI_GetFrame(
        options.ai_device, options.ai_channel, &frame, nullptr, kApiTimeoutMs);
    const TimePoint call_end = Clock::now();
    if (get_rc != 0) {
      RecordError(&stats->get_errors, get_rc);
      MarkActivityBucket(&stats->activity, start_time, call_end, false);
      if (stats->get_errors.consecutive >= kMaximumConsecutiveErrors) {
        stats->fatal = true;
        stop_requested->store(true, std::memory_order_relaxed);
      }
      continue;
    }

    ClearConsecutive(&stats->get_errors);
    ++stats->successful_gets;

    bool metadata_valid = true;
    if (frame.pMbBlk == RK_NULL) {
      ++stats->invalid_handles;
      metadata_valid = false;
    } else {
      RK_VOID* const address = RK_MPI_MB_Handle2VirAddr(frame.pMbBlk);
      const RK_U64 capacity = RK_MPI_MB_GetSize(frame.pMbBlk);
      if (address == RK_NULL) {
        ++stats->null_addresses;
        metadata_valid = false;
      }
      if (capacity == 0) {
        ++stats->zero_capacities;
        metadata_valid = false;
      }
      Observe(&stats->capacity, static_cast<std::uint64_t>(capacity));
      if (capacity != 0) {
        if (static_cast<RK_U64>(frame.u32Len) == capacity) {
          ++stats->len_equals_capacity;
        } else if (static_cast<RK_U64>(frame.u32Len) * kChannels ==
                   capacity) {
          ++stats->len_times_channels_equals_capacity;
        } else {
          ++stats->other_len_capacity_relationship;
        }
      }
    }

    if (frame.u32Len == 0) {
      ++stats->zero_lengths;
      metadata_valid = false;
    }
    Observe(&stats->bit_width, static_cast<std::int64_t>(frame.enBitWidth));
    Observe(&stats->sound_mode, static_cast<std::int64_t>(frame.enSoundMode));
    Observe(&stats->sample_rate,
            static_cast<std::int64_t>(frame.s32SampleRate));
    Observe(&stats->sequence, static_cast<std::uint64_t>(frame.u32Seq));
    Observe(&stats->length, static_cast<std::uint64_t>(frame.u32Len));

    ++stats->release_calls;
    const RK_S32 release_rc = RK_MPI_AI_ReleaseFrame(
        options.ai_device, options.ai_channel, &frame, nullptr);
    const TimePoint release_end = Clock::now();
    if (release_rc == 0) {
      ++stats->successful_releases;
      ClearConsecutive(&stats->release_errors);
    } else {
      RecordError(&stats->release_errors, release_rc);
      stats->fatal = true;
    }
    if (!metadata_valid || release_rc != 0) {
      MarkActivityBucket(&stats->activity, start_time, release_end, false);
      stats->fatal = true;
      stop_requested->store(true, std::memory_order_relaxed);
    } else {
      ++stats->validated_frames;
      MarkActivityBucket(&stats->activity, start_time, release_end, true);
      if (!stats->has_success_interval) {
        stats->has_success_interval = true;
        stats->first_success_begin_us = ToMicroseconds(call_begin);
      }
      stats->last_success_end_us = ToMicroseconds(release_end);
    }
  }

  const TimePoint loop_end = Clock::now();
  stats->loop_end_us = ToMicroseconds(loop_end);
  stats->completed_window =
      !stats->fatal && g_signal_requested == 0 && loop_end >= deadline;
}

bool ReleaseAoBlock(MB_BLK block,
                    TimePoint common_start,
                    AoStats* stats) {
  ++stats->release_calls;
  const RK_S32 rc = RK_MPI_MB_ReleaseMB(block);
  if (rc == 0) {
    ++stats->successful_releases;
    ClearConsecutive(&stats->release_errors);
    return true;
  }
  RecordError(&stats->release_errors, rc);
  MarkActivityBucket(&stats->activity, common_start, Clock::now(), false);
  stats->fatal = true;
  return false;
}

void SendEos(const Options& options,
             std::array<RK_U8, kPlaybackPayloadBytes>* payload,
             AoStats* stats) {
  AUDIO_FRAME_S frame {};
  MB_EXT_CONFIG_S external {};
  frame.enBitWidth = AUDIO_BIT_WIDTH_16;
  frame.enSoundMode = AUDIO_SOUND_MODE_STEREO;
  frame.s32SampleRate = static_cast<RK_S32>(kSampleRate);
  frame.u32Len = 0;
  frame.bBypassMbBlk = RK_FALSE;
  external.pu8VirAddr = payload->data();
  external.u64Size = 0;
  external.pOpaque = payload->data();
  external.pFreeCB = nullptr;

  stats->eos_create.called = true;
  stats->eos_create.rc = RK_MPI_SYS_CreateMB(&frame.pMbBlk, &external);
  if (stats->eos_create.rc != 0 || frame.pMbBlk == RK_NULL) {
    stats->eos_unsupported = true;
    if (frame.pMbBlk != RK_NULL) {
      stats->eos_release.called = true;
      stats->eos_release.rc = RK_MPI_MB_ReleaseMB(frame.pMbBlk);
    }
    return;
  }
  stats->eos_block_created = true;

  stats->eos_send.called = true;
  stats->eos_send.rc = RK_MPI_AO_SendFrame(
      options.ao_device, options.ao_channel, &frame, kApiTimeoutMs);
  stats->eos_release.called = true;
  stats->eos_release.rc = RK_MPI_MB_ReleaseMB(frame.pMbBlk);
  if (stats->eos_send.rc != 0 || stats->eos_release.rc != 0) {
    return;
  }
  stats->wait_eos.called = true;
  stats->wait_eos.rc = RK_MPI_AO_WaitEos(
      options.ao_device, options.ao_channel, kWaitEosTimeoutMs);
}

void PlaybackThread(const Options& options,
                    StartGate* gate,
                    std::atomic<bool>* stop_requested,
                    AoStats* stats) {
  TimePoint start_time;
  if (!WaitAtStartGate(gate, &stats->thread_ready, &start_time)) {
    return;
  }
  stats->start_gate_passed = true;
  stats->loop_start_us = ToMicroseconds(Clock::now());
  const TimePoint hard_deadline = start_time + kPlaybackHardLimit;
  std::array<RK_U8, kPlaybackPayloadBytes> payload {};

  while (stats->successful_sends < kPlaybackPayloadCount &&
         !StopRequested(*stop_requested)) {
    if (Clock::now() >= hard_deadline) {
      stats->hard_limit_reached = true;
      stats->fatal = true;
      MarkActivityBucket(&stats->activity, start_time, Clock::now(), false);
      stop_requested->store(true, std::memory_order_relaxed);
      break;
    }

    AUDIO_FRAME_S frame {};
    MB_EXT_CONFIG_S external {};
    frame.enBitWidth = AUDIO_BIT_WIDTH_16;
    frame.enSoundMode = AUDIO_SOUND_MODE_STEREO;
    frame.s32SampleRate = static_cast<RK_S32>(kSampleRate);
    frame.u32Seq = static_cast<RK_U32>(stats->successful_sends);
    frame.u32Len = static_cast<RK_U32>(payload.size());
    frame.u64TimeStamp = static_cast<RK_U64>(
        std::max<std::int64_t>(0, ElapsedMicroseconds(start_time, Clock::now())));
    frame.bBypassMbBlk = RK_FALSE;
    external.pu8VirAddr = payload.data();
    external.u64Size = payload.size();
    external.pOpaque = payload.data();
    external.pFreeCB = nullptr;

    ++stats->create_calls;
    const RK_S32 create_rc = RK_MPI_SYS_CreateMB(&frame.pMbBlk, &external);
    if (create_rc != 0 || frame.pMbBlk == RK_NULL) {
      RecordError(&stats->create_errors, create_rc);
      if (create_rc == 0 && frame.pMbBlk == RK_NULL) {
        ++stats->invalid_created_handles;
      }
      MarkActivityBucket(&stats->activity, start_time, Clock::now(), false);
      if (frame.pMbBlk != RK_NULL) {
        ++stats->created_blocks;
        ReleaseAoBlock(frame.pMbBlk, start_time, stats);
      }
      if (stats->create_errors.consecutive >= kMaximumConsecutiveErrors ||
          stats->fatal) {
        stats->fatal = true;
        stop_requested->store(true, std::memory_order_relaxed);
      }
      continue;
    }
    ClearConsecutive(&stats->create_errors);
    ++stats->created_blocks;

    const TimePoint call_begin = Clock::now();
    ++stats->send_calls;
    const RK_S32 send_rc = RK_MPI_AO_SendFrame(
        options.ao_device, options.ao_channel, &frame, kApiTimeoutMs);
    const TimePoint call_end = Clock::now();
    const bool released = ReleaseAoBlock(frame.pMbBlk, start_time, stats);
    const TimePoint operation_end = Clock::now();
    if (send_rc != 0) {
      RecordError(&stats->send_errors, send_rc);
      MarkActivityBucket(&stats->activity, start_time, call_end, false);
      if (stats->send_errors.consecutive >= kMaximumConsecutiveErrors) {
        stats->fatal = true;
      }
    } else {
      ClearConsecutive(&stats->send_errors);
      ++stats->successful_sends;
      stats->successful_bytes += payload.size();
      if (released) {
        MarkActivityBucket(&stats->activity, start_time, operation_end, true);
        if (!stats->has_success_interval) {
          stats->has_success_interval = true;
          stats->first_success_begin_us = ToMicroseconds(call_begin);
        }
        stats->last_success_end_us = ToMicroseconds(operation_end);
      }
    }
    if (!released || stats->fatal) {
      stop_requested->store(true, std::memory_order_relaxed);
    }
  }

  if (!stats->fatal && !StopRequested(*stop_requested) &&
      stats->successful_sends == kPlaybackPayloadCount) {
    SendEos(options, &payload, stats);
  }
  stats->loop_end_us = ToMicroseconds(Clock::now());
}

std::string JsonEscape(const std::string& value) {
  std::ostringstream stream;
  for (unsigned char character : value) {
    switch (character) {
      case '"':
        stream << "\\\"";
        break;
      case '\\':
        stream << "\\\\";
        break;
      case '\b':
        stream << "\\b";
        break;
      case '\f':
        stream << "\\f";
        break;
      case '\n':
        stream << "\\n";
        break;
      case '\r':
        stream << "\\r";
        break;
      case '\t':
        stream << "\\t";
        break;
      default:
        if (character < 0x20U) {
          char escaped[7] = {};
          std::snprintf(escaped, sizeof(escaped), "\\u%04x", character);
          stream << escaped;
        } else {
          stream << static_cast<char>(character);
        }
    }
  }
  return stream.str();
}

const char* JsonBool(bool value) {
  return value ? "true" : "false";
}

std::string HexReturnCode(RK_S32 code) {
  char buffer[11] = {};
  std::snprintf(buffer, sizeof(buffer), "0x%08x",
                static_cast<unsigned>(static_cast<std::uint32_t>(code)));
  return std::string(buffer);
}

void AppendCall(std::ostringstream* stream,
                const char* name,
                const CallResult& result,
                bool comma) {
  *stream << "      \"" << name << "\": {\"called\": "
          << JsonBool(result.called) << ", \"rc\": " << result.rc
          << ", \"rc_hex\": \"" << HexReturnCode(result.rc) << "\"}";
  if (comma) {
    *stream << ',';
  }
  *stream << '\n';
}

void AppendErrorCounter(std::ostringstream* stream,
                        const char* name,
                        const ErrorCounter& counter,
                        bool comma) {
  *stream << "      \"" << name << "\": {\"count\": " << counter.count
          << ", \"maximum_consecutive\": " << counter.maximum_consecutive
          << ", \"has_code\": " << JsonBool(counter.has_code)
          << ", \"first_code\": " << counter.first_code
          << ", \"first_code_hex\": \""
          << HexReturnCode(counter.first_code) << "\""
          << ", \"last_code\": " << counter.last_code
          << ", \"last_code_hex\": \"" << HexReturnCode(counter.last_code)
          << "\"}";
  if (comma) {
    *stream << ',';
  }
  *stream << '\n';
}

void AppendSignedRange(std::ostringstream* stream,
                       const char* name,
                       const SignedRange& range,
                       bool comma) {
  *stream << "      \"" << name << "\": {\"observed\": "
          << JsonBool(range.observed) << ", \"minimum\": " << range.minimum
          << ", \"maximum\": " << range.maximum << "}";
  if (comma) {
    *stream << ',';
  }
  *stream << '\n';
}

void AppendUnsignedRange(std::ostringstream* stream,
                         const char* name,
                         const UnsignedRange& range,
                         bool comma) {
  *stream << "      \"" << name << "\": {\"observed\": "
          << JsonBool(range.observed) << ", \"minimum\": " << range.minimum
          << ", \"maximum\": " << range.maximum << "}";
  if (comma) {
    *stream << ',';
  }
  *stream << '\n';
}

std::size_t CountMarkedBuckets(
    const std::array<RK_U8, kActivityBucketCount>& buckets) {
  std::size_t count = 0;
  for (RK_U8 marked : buckets) {
    if (marked != 0) {
      ++count;
    }
  }
  return count;
}

void AppendBucketIndices(
    std::ostringstream* stream,
    const std::array<RK_U8, kActivityBucketCount>& buckets) {
  *stream << '[';
  bool first = true;
  for (std::size_t index = 0; index < buckets.size(); ++index) {
    if (buckets[index] == 0) {
      continue;
    }
    if (!first) {
      *stream << ',';
    }
    *stream << index;
    first = false;
  }
  *stream << ']';
}

void AppendActivityBuckets(std::ostringstream* stream,
                           const ActivityBuckets& activity,
                           bool comma) {
  *stream << "      \"activity_buckets\": {\n"
          << "        \"success_count\": "
          << CountMarkedBuckets(activity.success) << ",\n"
          << "        \"error_count\": "
          << CountMarkedBuckets(activity.error) << ",\n"
          << "        \"success_out_of_range\": "
          << activity.success_out_of_range << ",\n"
          << "        \"error_out_of_range\": "
          << activity.error_out_of_range << ",\n"
          << "        \"success_indices\": ";
  AppendBucketIndices(stream, activity.success);
  *stream << ",\n        \"error_indices\": ";
  AppendBucketIndices(stream, activity.error);
  *stream << "\n      }";
  if (comma) {
    *stream << ',';
  }
  *stream << '\n';
}

void AppendProcScan(std::ostringstream* stream,
                    const char* name,
                    const ProcScan& scan,
                    bool comma) {
  *stream << "    \"" << name << "\": {\"attempted\": "
          << JsonBool(scan.attempted) << ", \"complete\": "
          << JsonBool(scan.complete) << ", \"busy\": "
          << JsonBool(scan.busy) << ", \"observed_pid_at_scan\": "
          << scan.observed_pid
          << ", \"occupied_node_at_scan\": \""
          << JsonEscape(scan.occupied_node)
          << "\", \"error\": \"" << JsonEscape(scan.error) << "\"}";
  if (comma) {
    *stream << ',';
  }
  *stream << '\n';
}

std::string BuildDryRunJson(const Options& options) {
  std::ostringstream stream;
  stream << "{\n"
         << "  \"schema_version\": 1,\n"
         << "  \"mode\": \"dry_run\",\n"
         << "  \"mpi_calls\": 0,\n"
         << "  \"files_created\": 0,\n"
         << "  \"probe_scope\": \"raw_mpi_transport_eos_cleanup_only\",\n"
         << "  \"probe_status\": \"not_run\",\n"
         << "  \"full_hil_status\": \"not_evaluated\",\n"
         << "  \"external_dmesg_delta_required\": true,\n"
         << "  \"occupancy_scan_scope\": \"snapshot_only\",\n"
         << "  \"occupancy_scan_method\": \"read_only_proc_fd_snapshot\",\n"
         << "  \"occupancy_scan_is_exclusive_reservation\": false,\n"
         << "  \"processes_terminated\": 0,\n"
         << "  \"external_audio_service_lock_required\": true,\n"
         << "  \"provenance\": {\n"
         << "    \"pinset_sha256\": \""
         << JsonEscape(BOOMPI_ROCKCHIP_MPI_HIL_PINSET_SHA256) << "\",\n"
         << "    \"source_sha256\": \""
         << JsonEscape(BOOMPI_ROCKCHIP_MPI_HIL_SOURCE_SHA256) << "\"\n"
         << "  },\n"
         << "  \"fixed_configuration\": {\n"
         << "    \"sample_rate_hz\": " << kSampleRate << ",\n"
         << "    \"channels\": " << kChannels << ",\n"
         << "    \"vendor_bit_width\": \"AUDIO_BIT_WIDTH_16\",\n"
         << "    \"points_per_frame\": " << kPointsPerFrame << ",\n"
         << "    \"device_frame_count\": " << kDeviceFrameCount << ",\n"
         << "    \"api_timeout_ms\": " << kApiTimeoutMs << ",\n"
         << "    \"wait_eos_timeout_ms\": " << kWaitEosTimeoutMs << ",\n"
         << "    \"capture_seconds\": 6,\n"
         << "    \"ao_vendor_payload_bytes\": " << kPlaybackPayloadBytes
         << ",\n"
         << "    \"ao_vendor_payload_count\": " << kPlaybackPayloadCount
         << ",\n"
         << "    \"ao_vendor_byte_budget\": " << kPlaybackTotalBytes
         << ",\n"
         << "    \"ao_nominal_duration_seconds\": 4,\n"
         << "    \"ao_nominal_duration_basis\": "
            "\"vendor_sample_byte_budget; packing_unproven\",\n"
         << "    \"activity_bucket_width_ms\": "
         << kActivityBucketWidth.count() << ",\n"
         << "    \"activity_bucket_count\": " << kActivityBucketCount
         << ",\n"
         << "    \"minimum_consecutive_common_success_buckets\": "
         << kMinimumCommonSuccessBuckets << "\n"
         << "  },\n"
         << "  \"inputs\": {\n"
         << "    \"ai_card\": \"" << JsonEscape(options.ai_card) << "\",\n"
         << "    \"ao_card\": \"" << JsonEscape(options.ao_card) << "\",\n"
         << "    \"ai_pcm_node\": \""
         << JsonEscape(CaptureDeviceNode(options.ai_pcm)) << "\",\n"
         << "    \"ao_pcm_node\": \""
         << JsonEscape(PlaybackDeviceNode(options.ao_pcm)) << "\",\n"
         << "    \"ai_device\": " << options.ai_device << ",\n"
         << "    \"ai_channel\": " << options.ai_channel << ",\n"
         << "    \"ao_device\": " << options.ao_device << ",\n"
         << "    \"ao_channel\": " << options.ao_channel << "\n"
         << "  },\n"
         << "  \"claims_not_verified\": [\"s16_le\", "
            "\"channel_packing\", \"dual_microphone_layout\", "
            "\"digital_reference_slot\", \"polarity\", "
            "\"clock_synchronization\", \"audible_playback\", "
            "\"xrun_free\"]\n"
         << "}\n";
  return stream.str();
}

std::string BuildReportJson(const Options& options, const Report& report) {
  std::ostringstream stream;
  stream << "{\n"
         << "  \"schema_version\": 1,\n"
         << "  \"mode\": \"execute\",\n"
         << "  \"occupancy_scan_scope\": \"snapshot_only\",\n"
         << "  \"occupancy_scan_method\": \"read_only_proc_fd_snapshot\",\n"
         << "  \"occupancy_scan_is_exclusive_reservation\": false,\n"
         << "  \"processes_terminated\": 0,\n"
         << "  \"external_audio_service_lock_required\": true,\n"
         << "  \"provenance\": {\n"
         << "    \"pinset_sha256\": \""
         << JsonEscape(BOOMPI_ROCKCHIP_MPI_HIL_PINSET_SHA256) << "\",\n"
         << "    \"source_sha256\": \""
         << JsonEscape(BOOMPI_ROCKCHIP_MPI_HIL_SOURCE_SHA256) << "\"\n"
         << "  },\n"
         << "  \"fixed_configuration\": {\n"
         << "    \"sample_rate_hz\": " << kSampleRate << ",\n"
         << "    \"channels\": " << kChannels << ",\n"
         << "    \"vendor_bit_width\": \"AUDIO_BIT_WIDTH_16\",\n"
         << "    \"sound_mode\": \"AUDIO_SOUND_MODE_STEREO\",\n"
         << "    \"loopback_mode\": \"AUDIO_LOOPBACK_NONE\",\n"
         << "    \"vqe_enabled\": false,\n"
         << "    \"points_per_frame\": " << kPointsPerFrame << ",\n"
         << "    \"device_frame_count\": " << kDeviceFrameCount << ",\n"
         << "    \"api_timeout_ms\": " << kApiTimeoutMs << ",\n"
         << "    \"wait_eos_timeout_ms\": " << kWaitEosTimeoutMs << ",\n"
         << "    \"capture_seconds\": 6,\n"
         << "    \"ao_vendor_payload_bytes\": " << kPlaybackPayloadBytes
         << ",\n"
         << "    \"ao_vendor_payload_count\": " << kPlaybackPayloadCount
         << ",\n"
         << "    \"ao_vendor_byte_budget\": " << kPlaybackTotalBytes
         << ",\n"
         << "    \"ao_nominal_duration_seconds\": 4,\n"
         << "    \"ao_nominal_duration_basis\": "
            "\"vendor_sample_byte_budget; packing_unproven\",\n"
         << "    \"byte_packing_verified\": false,\n"
         << "    \"activity_bucket_width_ms\": "
         << kActivityBucketWidth.count() << ",\n"
         << "    \"activity_bucket_count\": " << kActivityBucketCount
         << ",\n"
         << "    \"minimum_consecutive_common_success_buckets\": "
         << kMinimumCommonSuccessBuckets << "\n"
         << "  },\n"
         << "  \"inputs\": {\n"
         << "    \"ai_card\": \"" << JsonEscape(options.ai_card) << "\",\n"
         << "    \"ao_card\": \"" << JsonEscape(options.ao_card) << "\",\n"
         << "    \"ai_pcm_node\": \""
         << JsonEscape(CaptureDeviceNode(options.ai_pcm)) << "\",\n"
         << "    \"ao_pcm_node\": \""
         << JsonEscape(PlaybackDeviceNode(options.ao_pcm)) << "\",\n"
         << "    \"ai_device\": " << options.ai_device << ",\n"
         << "    \"ai_channel\": " << options.ai_channel << ",\n"
         << "    \"ao_device\": " << options.ao_device << ",\n"
         << "    \"ao_channel\": " << options.ao_channel << "\n"
         << "  },\n"
         << "  \"precondition\": {\n"
         << "    \"status\": \"" << JsonEscape(report.precondition_status)
         << "\",\n"
         << "    \"detail\": \"" << JsonEscape(report.precondition_detail)
         << "\",\n";
  AppendProcScan(&stream, "first_proc_fd_scan", report.first_scan, true);
  AppendProcScan(&stream, "second_proc_fd_scan", report.second_scan, false);
  stream << "  },\n"
         << "  \"initialization\": {\n"
         << "    \"status\": \""
         << (report.initialization_passed ? "pass" : "fail") << "\",\n"
         << "    \"calls\": {\n";
  AppendCall(&stream, "sys_init", report.init.sys_init, true);
  AppendCall(&stream, "ai_set_pub_attr", report.init.ai_set_pub_attr, true);
  AppendCall(&stream, "ai_enable", report.init.ai_enable, true);
  AppendCall(&stream, "ai_set_channel_param",
             report.init.ai_set_channel_param, true);
  AppendCall(&stream, "ai_enable_channel", report.init.ai_enable_channel,
             true);
  AppendCall(&stream, "ao_set_pub_attr", report.init.ao_set_pub_attr, true);
  AppendCall(&stream, "ao_enable", report.init.ao_enable, true);
  AppendCall(&stream, "ao_set_channel_param",
             report.init.ao_set_channel_param, true);
  AppendCall(&stream, "ao_enable_channel", report.init.ao_enable_channel,
             false);
  stream << "    }\n"
         << "  },\n"
         << "  \"transport\": {\n"
         << "    \"status\": \"" << report.transport_status << "\",\n"
         << "    \"activity_bucket_width_ms\": "
         << kActivityBucketWidth.count() << ",\n"
         << "    \"longest_consecutive_common_success_buckets\": "
         << report.longest_common_success_buckets << ",\n"
         << "    \"qualified_common_success_us\": "
         << report.qualified_common_success_us << ",\n"
         << "    \"start_gate_failed\": "
         << JsonBool(report.start_gate_failed) << ",\n"
         << "    \"signal_observed\": " << JsonBool(report.signal_observed)
         << ",\n"
         << "    \"ai\": {\n"
         << "      \"thread_ready\": " << JsonBool(report.ai.thread_ready)
         << ",\n"
         << "      \"start_gate_passed\": "
         << JsonBool(report.ai.start_gate_passed) << ",\n"
         << "      \"thread_joined\": " << JsonBool(report.ai.thread_joined)
         << ",\n"
         << "      \"completed_window\": "
         << JsonBool(report.ai.completed_window) << ",\n"
         << "      \"fatal\": " << JsonBool(report.ai.fatal) << ",\n"
         << "      \"thread_exception\": "
         << JsonBool(report.ai.thread_exception) << ",\n"
         << "      \"loop_start_us\": " << report.ai.loop_start_us << ",\n"
         << "      \"loop_end_us\": " << report.ai.loop_end_us << ",\n"
         << "      \"first_success_begin_us\": "
         << report.ai.first_success_begin_us << ",\n"
         << "      \"last_success_end_us\": "
         << report.ai.last_success_end_us << ",\n"
         << "      \"get_calls\": " << report.ai.get_calls << ",\n"
         << "      \"successful_gets\": " << report.ai.successful_gets
         << ",\n"
         << "      \"validated_frames\": " << report.ai.validated_frames
         << ",\n"
         << "      \"release_calls\": " << report.ai.release_calls << ",\n"
         << "      \"successful_releases\": "
         << report.ai.successful_releases << ",\n"
         << "      \"invalid_handles\": " << report.ai.invalid_handles
         << ",\n"
         << "      \"null_addresses\": " << report.ai.null_addresses
         << ",\n"
         << "      \"zero_capacities\": " << report.ai.zero_capacities
         << ",\n"
         << "      \"zero_lengths\": " << report.ai.zero_lengths << ",\n"
         << "      \"len_capacity_relationship\": {\n"
         << "        \"len_equals_capacity\": "
         << report.ai.len_equals_capacity << ",\n"
         << "        \"len_times_channels_equals_capacity\": "
         << report.ai.len_times_channels_equals_capacity << ",\n"
         << "        \"other\": "
         << report.ai.other_len_capacity_relationship << "\n"
         << "      },\n";
  AppendErrorCounter(&stream, "get_errors", report.ai.get_errors, true);
  AppendErrorCounter(&stream, "release_errors", report.ai.release_errors,
                     true);
  AppendSignedRange(&stream, "bit_width_metadata", report.ai.bit_width, true);
  AppendSignedRange(&stream, "sound_mode_metadata", report.ai.sound_mode,
                    true);
  AppendSignedRange(&stream, "sample_rate_metadata", report.ai.sample_rate,
                    true);
  AppendUnsignedRange(&stream, "sequence_metadata", report.ai.sequence, true);
  AppendUnsignedRange(&stream, "length_metadata", report.ai.length, true);
  AppendUnsignedRange(&stream, "capacity_metadata", report.ai.capacity, true);
  AppendActivityBuckets(&stream, report.ai.activity, false);
  stream << "    },\n"
         << "    \"ao\": {\n"
         << "      \"thread_ready\": " << JsonBool(report.ao.thread_ready)
         << ",\n"
         << "      \"start_gate_passed\": "
         << JsonBool(report.ao.start_gate_passed) << ",\n"
         << "      \"thread_joined\": " << JsonBool(report.ao.thread_joined)
         << ",\n"
         << "      \"fatal\": " << JsonBool(report.ao.fatal) << ",\n"
         << "      \"thread_exception\": "
         << JsonBool(report.ao.thread_exception) << ",\n"
         << "      \"hard_limit_reached\": "
         << JsonBool(report.ao.hard_limit_reached) << ",\n"
         << "      \"loop_start_us\": " << report.ao.loop_start_us << ",\n"
         << "      \"loop_end_us\": " << report.ao.loop_end_us << ",\n"
         << "      \"first_success_begin_us\": "
         << report.ao.first_success_begin_us << ",\n"
         << "      \"last_success_end_us\": "
         << report.ao.last_success_end_us << ",\n"
         << "      \"create_calls\": " << report.ao.create_calls << ",\n"
         << "      \"created_blocks\": " << report.ao.created_blocks
         << ",\n"
         << "      \"invalid_created_handles\": "
         << report.ao.invalid_created_handles << ",\n"
         << "      \"send_calls\": " << report.ao.send_calls << ",\n"
         << "      \"successful_sends\": " << report.ao.successful_sends
         << ",\n"
         << "      \"successful_bytes\": " << report.ao.successful_bytes
         << ",\n"
         << "      \"release_calls\": " << report.ao.release_calls << ",\n"
         << "      \"successful_releases\": "
         << report.ao.successful_releases << ",\n";
  AppendErrorCounter(&stream, "create_errors", report.ao.create_errors, true);
  AppendErrorCounter(&stream, "send_errors", report.ao.send_errors, true);
  AppendErrorCounter(&stream, "release_errors", report.ao.release_errors,
                     true);
  AppendActivityBuckets(&stream, report.ao.activity, false);
  stream << "    }\n"
         << "  },\n"
         << "  \"eos\": {\n"
         << "    \"status\": \"" << report.eos_status << "\",\n"
         << "    \"zero_size_mb_unsupported\": "
         << JsonBool(report.ao.eos_unsupported) << ",\n"
         << "    \"calls\": {\n";
  AppendCall(&stream, "create_zero_size_mb", report.ao.eos_create, true);
  AppendCall(&stream, "send_zero_length_frame", report.ao.eos_send, true);
  AppendCall(&stream, "release_zero_size_mb", report.ao.eos_release, true);
  AppendCall(&stream, "wait_eos", report.ao.wait_eos, false);
  stream << "    }\n"
         << "  },\n"
         << "  \"cleanup\": {\n"
         << "    \"status\": \"" << report.cleanup_status << "\",\n"
         << "    \"has_first_error\": "
         << JsonBool(report.cleanup.has_first_error) << ",\n"
         << "    \"first_error_step\": \""
         << JsonEscape(report.cleanup.first_error_step) << "\",\n"
         << "    \"first_error_code\": "
         << report.cleanup.first_error_code << ",\n"
         << "    \"first_error_code_hex\": \""
         << HexReturnCode(report.cleanup.first_error_code) << "\",\n"
         << "    \"calls\": {\n";
  AppendCall(&stream, "ao_disable_channel",
             report.cleanup.ao_disable_channel, true);
  AppendCall(&stream, "ao_disable", report.cleanup.ao_disable, true);
  AppendCall(&stream, "ai_disable_channel",
             report.cleanup.ai_disable_channel, true);
  AppendCall(&stream, "ai_disable", report.cleanup.ai_disable, true);
  AppendCall(&stream, "sys_exit", report.cleanup.sys_exit, false);
  stream << "    }\n"
         << "  },\n"
         << "  \"claims_not_verified\": [\"s16_le\", "
            "\"channel_packing\", \"dual_microphone_layout\", "
            "\"digital_reference_slot\", \"polarity\", "
            "\"clock_synchronization\", \"audible_playback\", "
            "\"xrun_free\"],\n"
         << "  \"probe_scope\": \"raw_mpi_transport_eos_cleanup_only\",\n"
         << "  \"probe_status\": \"" << report.probe_status << "\",\n"
         << "  \"full_hil_status\": \"not_evaluated\",\n"
         << "  \"external_dmesg_delta_required\": true\n"
         << "}\n";
  return stream.str();
}

bool WriteAll(int fd, const std::string& data, std::string* error) {
  std::size_t offset = 0;
  while (offset < data.size()) {
    const ssize_t written =
        write(fd, data.data() + offset, data.size() - offset);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      *error = "cannot write result JSON: " +
               std::string(std::strerror(errno));
      return false;
    }
    if (written == 0) {
      *error = "result JSON write made no progress";
      return false;
    }
    offset += static_cast<std::size_t>(written);
  }
  return true;
}

bool WriteResultAtomically(int artifact_directory_fd,
                           const std::string& json,
                           std::string* error) {
  constexpr char kTemporaryName[] = ".result.json.tmp";
  constexpr char kFinalName[] = "result.json";
  const int fd = openat(
      artifact_directory_fd, kTemporaryName,
      O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) {
    *error = "cannot create result JSON temporary file: " +
             std::string(std::strerror(errno));
    return false;
  }

  struct stat temporary_status {};
  if (fstat(fd, &temporary_status) != 0 ||
      !S_ISREG(temporary_status.st_mode) ||
      (temporary_status.st_mode & 0777) != 0600) {
    *error = "result JSON temporary file failed regular-mode verification";
    close(fd);
    unlinkat(artifact_directory_fd, kTemporaryName, 0);
    return false;
  }

  bool ok = WriteAll(fd, json, error);
  if (ok && fsync(fd) != 0) {
    *error = "cannot fsync result JSON: " +
             std::string(std::strerror(errno));
    ok = false;
  }
  if (close(fd) != 0 && ok) {
    *error = "cannot close result JSON: " +
             std::string(std::strerror(errno));
    ok = false;
  }
  if (!ok) {
    unlinkat(artifact_directory_fd, kTemporaryName, 0);
    return false;
  }
  if (renameat(artifact_directory_fd, kTemporaryName, artifact_directory_fd,
               kFinalName) != 0) {
    *error = "cannot atomically rename result JSON: " +
             std::string(std::strerror(errno));
    unlinkat(artifact_directory_fd, kTemporaryName, 0);
    return false;
  }

  if (fsync(artifact_directory_fd) != 0) {
    *error = "cannot fsync artifact directory: " +
             std::string(std::strerror(errno));
    return false;
  }
  return true;
}

void FinalizeFacets(Report* report) {
  std::size_t current_common_success_buckets = 0;
  for (std::size_t index = 0; index < kActivityBucketCount; ++index) {
    const bool common_success = report->ai.activity.success[index] != 0 &&
                                report->ao.activity.success[index] != 0;
    const bool any_error = report->ai.activity.error[index] != 0 ||
                           report->ao.activity.error[index] != 0;
    if (common_success && !any_error) {
      ++current_common_success_buckets;
      report->longest_common_success_buckets =
          std::max(report->longest_common_success_buckets,
                   current_common_success_buckets);
    } else {
      current_common_success_buckets = 0;
    }
  }
  report->qualified_common_success_us = static_cast<std::int64_t>(
      report->longest_common_success_buckets *
      static_cast<std::size_t>(kActivityBucketWidth.count()) * 1000U);

  const bool ai_transport_pass =
      report->ai.start_gate_passed && report->ai.thread_joined &&
      report->ai.completed_window &&
      !report->ai.fatal && !report->ai.thread_exception &&
      report->ai.successful_gets > 0 && report->ai.validated_frames > 0 &&
      report->ai.get_errors.count == 0 &&
      report->ai.release_errors.count == 0 &&
      report->ai.activity.success_out_of_range == 0 &&
      report->ai.activity.error_out_of_range == 0 &&
      report->ai.release_calls == report->ai.successful_gets &&
      report->ai.successful_releases == report->ai.release_calls &&
      report->ai.invalid_handles == 0 && report->ai.null_addresses == 0 &&
      report->ai.zero_capacities == 0 && report->ai.zero_lengths == 0;
  const bool ao_transport_pass =
      report->ao.start_gate_passed && report->ao.thread_joined &&
      !report->ao.fatal &&
      !report->ao.thread_exception && !report->ao.hard_limit_reached &&
      report->ao.create_errors.count == 0 &&
      report->ao.send_errors.count == 0 &&
      report->ao.release_errors.count == 0 &&
      report->ao.invalid_created_handles == 0 &&
      report->ao.activity.success_out_of_range == 0 &&
      report->ao.activity.error_out_of_range == 0 &&
      report->ao.successful_sends == kPlaybackPayloadCount &&
      report->ao.successful_bytes == kPlaybackTotalBytes &&
      report->ao.created_blocks == report->ao.release_calls &&
      report->ao.successful_releases == report->ao.release_calls;
  const bool transport_pass =
      report->initialization_passed && !report->start_gate_failed &&
      !report->signal_observed && ai_transport_pass && ao_transport_pass &&
      report->longest_common_success_buckets >=
          kMinimumCommonSuccessBuckets;
  report->transport_status = transport_pass ? "pass" : "fail";

  if (!report->ao.eos_create.called) {
    report->eos_status = "not_attempted";
  } else if (report->ao.eos_unsupported &&
             (!report->ao.eos_release.called ||
              report->ao.eos_release.rc == 0)) {
    report->eos_status = "unsupported";
  } else if (report->ao.eos_create.rc == 0 && report->ao.eos_send.called &&
             report->ao.eos_send.rc == 0 && report->ao.eos_release.called &&
             report->ao.eos_release.rc == 0 && report->ao.wait_eos.called &&
             report->ao.wait_eos.rc == 0) {
    report->eos_status = "pass";
  } else {
    report->eos_status = "fail";
  }

  report->cleanup_status = report->cleanup.passed ? "pass" : "fail";
  if (report->transport_status == "pass" && report->eos_status == "pass" &&
      report->cleanup_status == "pass") {
    report->probe_status = "pass";
  } else if (report->transport_status == "pass" &&
             report->eos_status == "unsupported" &&
             report->cleanup_status == "pass") {
    report->probe_status = "inconclusive";
  } else {
    report->probe_status = "fail";
  }
}

int ExitCodeForReport(const Report& report) {
  if (report.precondition_status != "pass") {
    return 2;
  }
  if (!report.initialization_passed) {
    return 3;
  }
  if (report.cleanup_status == "fail") {
    return 6;
  }
  if (report.transport_status != "pass") {
    return 4;
  }
  if (report.eos_status != "pass") {
    return 5;
  }
  return 0;
}

bool PersistAndPrint(int artifact_directory_fd,
                     const Options& options,
                     const Report& report,
                     std::string* error) {
  const std::string json = BuildReportJson(options, report);
  if (!WriteResultAtomically(artifact_directory_fd, json, error)) {
    return false;
  }
  if (std::fwrite(json.data(), 1, json.size(), stdout) != json.size()) {
    *error = "cannot write complete result JSON to stdout";
    return false;
  }
  if (std::fflush(stdout) != 0) {
    *error = "cannot flush result JSON to stdout";
    return false;
  }
  return true;
}

int Run(int argc, char** argv) {
  Options options;
  std::string error;
  if (!ParseArguments(argc, argv, &options, &error)) {
    std::fprintf(stderr, "%s\n%s", error.c_str(), Usage(argv[0]).c_str());
    return 2;
  }
  if (options.help) {
    const std::string usage = Usage(argv[0]);
    return std::fwrite(usage.data(), 1, usage.size(), stdout) == usage.size() &&
                   std::fflush(stdout) == 0
               ? 0
               : 8;
  }
  if (!options.execute) {
    const std::string dry_run = BuildDryRunJson(options);
    return std::fwrite(dry_run.data(), 1, dry_run.size(), stdout) ==
                       dry_run.size() &&
                   std::fflush(stdout) == 0
               ? 0
               : 8;
  }

  const std::string capture_node = CaptureDeviceNode(options.ai_pcm);
  const std::string playback_node = PlaybackDeviceNode(options.ao_pcm);
  int artifact_directory_fd = -1;
  if (!ValidateCharacterDevice(capture_node, &error) ||
      !ValidateCharacterDevice(playback_node, &error) ||
      !PrecheckArtifactDirectory(options.artifact_dir, &error) ||
      !CreateArtifactDirectory(options.artifact_dir, &artifact_directory_fd,
                               &error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 2;
  }
  ScopedFd artifact_directory(artifact_directory_fd);

  Report report;
  if (!InstallSignalHandlers(&error)) {
    report.precondition_status = "fail";
    report.precondition_detail = error;
  } else {
    report.first_scan = ScanProcForPcmUsers(capture_node, playback_node);
    report.second_scan = ScanProcForPcmUsers(capture_node, playback_node);
  }
  if (report.precondition_status == "fail") {
    // Preserve the signal-installation failure selected above.
  } else if (!report.first_scan.complete || !report.second_scan.complete) {
    report.precondition_status = "fail";
    report.precondition_detail = !report.first_scan.complete
                                     ? report.first_scan.error
                                     : report.second_scan.error;
  } else if (report.first_scan.busy || report.second_scan.busy) {
    report.precondition_status = "fail";
    report.precondition_detail =
        "target PCM node is already open; no process was terminated";
  } else if (g_signal_requested != 0) {
    report.precondition_status = "fail";
    report.precondition_detail = "signal received before MPI initialization";
    report.signal_observed = true;
  } else {
    report.precondition_status = "pass";
  }

  if (report.precondition_status != "pass") {
    report.cleanup_status = "not_attempted";
    report.transport_status = "not_run";
    report.eos_status = "not_attempted";
    report.probe_status = "fail";
    report.signal_observed =
        report.signal_observed || g_signal_requested != 0;
    if (!PersistAndPrint(artifact_directory.get(), options, report, &error)) {
      std::fprintf(stderr, "%s\n", error.c_str());
      return 7;
    }
    if (!artifact_directory.Close(&error)) {
      std::fprintf(stderr, "%s\n", error.c_str());
      return 7;
    }
    return 2;
  }

  Lifecycle lifecycle;
  report.initialization_passed =
      InitializeMpi(options, &report.init, &lifecycle);
  if (report.initialization_passed) {
    StartGate gate;
    std::atomic<bool> stop_requested(false);
    std::thread capture_thread;
    std::thread playback_thread;
    bool capture_started = false;
    bool playback_started = false;

    try {
      capture_thread = std::thread([&] {
        try {
          CaptureThread(options, &gate, &stop_requested, &report.ai);
        } catch (...) {
          report.ai.thread_exception = true;
          report.ai.fatal = true;
          stop_requested.store(true, std::memory_order_relaxed);
        }
      });
      capture_started = true;
      playback_thread = std::thread([&] {
        try {
          PlaybackThread(options, &gate, &stop_requested, &report.ao);
        } catch (...) {
          report.ao.thread_exception = true;
          report.ao.fatal = true;
          stop_requested.store(true, std::memory_order_relaxed);
        }
      });
      playback_started = true;
    } catch (...) {
      stop_requested.store(true, std::memory_order_relaxed);
      report.start_gate_failed = true;
    }

    {
      std::unique_lock<std::mutex> lock(gate.mutex);
      const bool both_ready =
          capture_started && playback_started &&
          gate.condition.wait_for(
              lock, std::chrono::seconds(2),
              [&gate] { return gate.ready_threads == 2; });
      if (!both_ready || g_signal_requested != 0) {
        report.start_gate_failed = true;
        stop_requested.store(true, std::memory_order_relaxed);
        gate.cancelled = true;
      } else {
        gate.start_time = Clock::now();
        gate.go = true;
      }
      gate.condition.notify_all();
    }

    if (capture_started && capture_thread.joinable()) {
      capture_thread.join();
      report.ai.thread_joined = true;
    }
    if (playback_started && playback_thread.joinable()) {
      playback_thread.join();
      report.ao.thread_joined = true;
    }
    report.signal_observed = g_signal_requested != 0;
  }

  CleanupMpi(options, &lifecycle, &report.cleanup);
  report.signal_observed =
      report.signal_observed || g_signal_requested != 0;
  FinalizeFacets(&report);
  const int exit_code = ExitCodeForReport(report);
  if (!PersistAndPrint(artifact_directory.get(), options, report, &error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 7;
  }
  if (!artifact_directory.Close(&error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 7;
  }
  return exit_code;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return Run(argc, argv);
  } catch (const std::exception& exception) {
    std::fprintf(stderr, "fatal HIL exception: %s\n", exception.what());
    return 8;
  } catch (...) {
    std::fprintf(stderr, "fatal unknown HIL exception\n");
    return 8;
  }
}

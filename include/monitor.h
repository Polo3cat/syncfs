#pragma once

#include <cstdint>
#include <deque>
#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <sys/inotify.h>
#include <vector>

namespace monitor {
// The mask is the same for every watched directory. IN_CREATE and IN_MOVED_TO
// are what make a directory appearing after startup discoverable at all: a new
// directory never emits IN_CLOSE_WRITE. IN_MOVED_FROM keeps the watch
// descriptor to path mapping honest when a watched directory is renamed away.
inline constexpr uint32_t watch_mask = IN_CLOSE_WRITE | IN_DELETE | IN_CREATE |
                                       IN_MOVED_FROM | IN_MOVED_TO |
                                       IN_DONT_FOLLOW | IN_EXCL_UNLINK;

struct InotifyEvent {
  // Path of the affected entry relative to the sync root, "./a/b/file.txt".
  // Same shape as the keys of files::list(), which walks from ".".
  std::optional<std::string> name;
  uint32_t mask{};
};

class Monitor {
public:
  Monitor();
  ~Monitor();

  Monitor(Monitor &&m) noexcept;
  auto operator=(Monitor &&m) noexcept -> Monitor &;

  Monitor(const Monitor &m) = delete;
  auto operator=(const Monitor &m) -> Monitor = delete;

  // Returns when monitor is ready.
  [[nodiscard]] auto wait() -> bool;
  // Consumes the whole batch the last read drained. Callers that re-list the
  // tree afterwards have already accounted for all of it. A refill that failed
  // is reported rather than dropped: an inotify read error is the one way this
  // monitor stops seeing the tree, and a silent one leaves the daemon awake and
  // blind.
  [[nodiscard]] auto discard() -> std::expected<void, std::string>;

  // Directories currently watched, sorted. The sync root "." is always the
  // first element while the monitor owns its inotify descriptor.
  [[nodiscard]] auto watched() const -> std::vector<std::filesystem::path>;

  // Pops one event, refilling from the inotify descriptor when the batch ran
  // out. Refilling reads every event the kernel has queued, so none is lost.
  auto next_event() -> std::expected<InotifyEvent, std::string>;

private:
  // Adds a watch for the sync root and every subdirectory below it, and drops
  // the ones whose directory is gone. Idempotent: inotify_add_watch on an
  // already watched path returns the same descriptor.
  void resync_watches();
  void add_watch(const std::filesystem::path &dir);

  int fd = inotify_init();
  std::map<int, std::filesystem::path> watches;
  std::deque<InotifyEvent> batch;
};
} // namespace monitor

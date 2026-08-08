#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <expected>
#include <filesystem>
#include <format>
#include <linux/limits.h>
#include <map>
#include <monitor.h>
#include <optional>
#include <ranges>
#include <span>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <string>
#include <sys/inotify.h>
#include <sys/poll.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace monitor {
Monitor::Monitor() {
  if (fd == -1) {
    auto msg =
        std::format("Failed to initialize monitor {}", ::strerror(errno));
    throw std::runtime_error(msg);
  }
  int const root = inotify_add_watch(fd, ".", watch_mask);
  if (root == -1) {
    auto msg =
        std::format("Failed to initialize monitor {}", ::strerror(errno));
    throw std::runtime_error(msg);
  }
  watches.insert_or_assign(root, std::filesystem::path{"."});
  resync_watches();
}

Monitor::~Monitor() {
  if (fd != -1) {
    close(fd);
  }
}

Monitor::Monitor(Monitor &&m) noexcept { *this = std::move(m); }

auto Monitor::operator=(Monitor &&m) noexcept -> Monitor & {
  this->fd = m.fd;
  this->watches = std::move(m.watches);
  this->batch = std::move(m.batch);
  m.fd = -1;
  m.watches.clear();
  m.batch.clear();
  return *this;
}

void Monitor::add_watch(const std::filesystem::path &dir) {
  int const wd = inotify_add_watch(fd, dir.c_str(), watch_mask);
  if (wd == -1) {
    // Losing a race against a directory that disappeared between the traversal
    // and this call is normal; the next resync sees whatever is still there.
    spdlog::trace("Failed to watch {}. {}", dir.string(), ::strerror(errno));
    return;
  }
  watches.insert_or_assign(wd, dir);
}

void Monitor::resync_watches() {
  // A deleted directory has its watch removed by the kernel already, so a
  // failing inotify_rm_watch here is expected rather than an error. A renamed
  // one keeps a live descriptor under a stale path, and only this drop-then-add
  // pass maps it back to where it now lives.
  std::erase_if(watches, [this](const auto &entry) -> bool {
    std::error_code err;
    if (std::filesystem::is_directory(entry.second, err)) {
      return false;
    }
    static_cast<void>(inotify_rm_watch(fd, entry.first));
    return true;
  });

  add_watch(".");

  // Same options as files::list(), so a symlinked directory is neither
  // descended into nor watched, and the coverage of the two matches.
  std::error_code err;
  auto it = std::filesystem::recursive_directory_iterator(
      ".", std::filesystem::directory_options::none, err);
  auto const last = std::filesystem::recursive_directory_iterator{};
  while (!err && it != last) {
    std::error_code entry_err;
    if (it->is_directory(entry_err) && !it->is_symlink(entry_err)) {
      add_watch(it->path());
    }
    it.increment(err);
  }
}

auto Monitor::watched() const -> std::vector<std::filesystem::path> {
  auto dirs = watches | std::views::values |
              std::ranges::to<std::vector<std::filesystem::path>>();
  std::ranges::sort(dirs);
  return dirs;
}

namespace {
// The inotify constants are signed ints, so widen before masking.
constexpr auto ignored = static_cast<uint32_t>(IN_IGNORED);
constexpr auto is_dir = static_cast<uint32_t>(IN_ISDIR);
constexpr auto created = static_cast<uint32_t>(IN_CREATE);

auto _wait(int fd) -> int {
  constexpr int timeout_ms = 50;
  pollfd p{.fd = fd, .events = POLLIN, .revents = 0};
  int const avail = ::poll(&p, 1, timeout_ms);
  if (avail == -1) {
    // A signal handler ran during the poll; the caller checks its own
    // termination flag on the next iteration.
    if (errno == EINTR) {
      return 0;
    }
    auto msg =
        std::format("Failed to poll on inotify fd {}", ::strerror(errno));
    throw std::runtime_error(msg);
  }
  if (avail == 1) {
    if (static_cast<bool>(static_cast<unsigned short>(p.revents) & POLLERR)) {
      throw std::runtime_error("Error condition on inotify fd");
    }
    if (static_cast<bool>(static_cast<unsigned short>(p.revents) & POLLHUP)) {
      throw std::runtime_error("Hang  up on inotify fd");
    }
    if (static_cast<bool>(static_cast<unsigned short>(p.revents) & POLLNVAL)) {
      throw std::runtime_error("Inotify fd not open");
    }
  }
  spdlog::trace("{} events available", avail);
  return avail;
}

// An inotify event as it comes off the descriptor: the name is relative to the
// watched directory the descriptor stands for, so only Monitor can expand it.
struct RawEvent {
  std::string name;
  int wd{};
  uint32_t mask{};
};

// inotify(7): a read buffer must hold sizeof(inotify_event) + NAME_MAX + 1 or
// the read fails EINVAL as soon as a long enough filename shows up. Room for a
// batch on top of that, so a burst is drained in one syscall.
constexpr size_t max_event_size = sizeof(inotify_event) + NAME_MAX + 1;
constexpr size_t batch_size = 16;

auto _read(int fd) -> std::expected<std::vector<RawEvent>, std::string> {
  alignas(inotify_event) std::array<char, batch_size * max_event_size> buf{};
  auto const n = ::read(fd, buf.data(), buf.size());
  if (n == -1) {
    return std::unexpected(
        std::format("Failed to read on inotify fd. {}", ::strerror(errno)));
  }
  if (n == 0) {
    return std::unexpected("inotify fd closed");
  }
  if (std::cmp_less(n, sizeof(inotify_event))) {
    return std::unexpected("Read something smaller than an inotify event");
  }

  auto const filled = static_cast<size_t>(n);
  auto events = std::vector<RawEvent>{};
  size_t offset = 0;
  while (offset + sizeof(inotify_event) <= filled) {
    auto const rest = std::span<const char>(buf).subspan(offset);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const auto *e = reinterpret_cast<inotify_event const *>(rest.data());
    // The name field is NUL padded up to len, so it reads as a C string.
    events.push_back(RawEvent{
        .name = e->len == 0 ? std::string{}
                            : std::string{static_cast<const char *>(e->name)},
        .wd = e->wd,
        .mask = e->mask});
    offset += sizeof(inotify_event) + e->len;
  }
  return events;
}

// Whether an event means the tree may already look different to files::list().
// A freshly created regular file is still empty and its writer holds it open;
// the IN_CLOSE_WRITE that follows is the one worth acting on. Publishing the
// file in between costs a full hash of whatever is on disk so far, which for an
// in-flight download is both wrong and expensive. A directory gets no such
// second event, so IN_CREATE on one counts.
auto drives_sync(const InotifyEvent &ev) -> bool {
  return (ev.mask & created) == 0 || (ev.mask & is_dir) != 0;
}
} // namespace

auto Monitor::wait() -> bool {
  if (batch.empty() && _wait(fd) <= 0) {
    return false;
  }
  while (true) {
    auto event = next_event();
    if (!event.has_value()) {
      spdlog::debug(event.error());
      return false;
    }
    if (drives_sync(*event)) {
      // Put it back so the caller still sees the event that woke it.
      batch.push_front(std::move(*event));
      return true;
    }
    if (batch.empty()) {
      return false;
    }
  }
}

auto Monitor::next_event()
    -> std::expected<monitor::InotifyEvent, std::string> {
  if (batch.empty()) {
    auto const raw = _read(fd);
    if (!raw.has_value()) {
      return std::unexpected(raw.error());
    }

    // Names first: the watch descriptors they refer to are the ones that were
    // live when the kernel queued these events, before any resync below.
    for (const auto &r : *raw) {
      InotifyEvent ev{};
      ev.mask = r.mask;
      if (!r.name.empty()) {
        // inotify reports a name relative to the watched directory. Callers
        // diff against files::list(), keyed relative to the sync root.
        auto const watched_dir = watches.find(r.wd);
        auto const base = watched_dir == watches.end()
                              ? std::filesystem::path{"."}
                              : watched_dir->second;
        ev.name = (base / r.name).string();
      }
      batch.push_back(std::move(ev));
    }

    auto const reshapes_tree = [](const RawEvent &r) -> bool {
      return (r.mask & (is_dir | ignored)) != 0;
    };
    for (const auto &r : *raw) {
      if ((r.mask & ignored) != 0) {
        watches.erase(r.wd);
      }
    }
    if (std::ranges::any_of(*raw, reshapes_tree)) {
      resync_watches();
    }
  }

  if (batch.empty()) {
    return std::unexpected("Read no inotify event");
  }
  auto event = std::move(batch.front());
  batch.pop_front();
  return event;
}

void Monitor::discard() {
  [[maybe_unused]] auto _ = this->next_event();
  // The caller re-lists the whole tree after this, which already accounts for
  // the rest of the batch; keeping it would only force redundant traversals.
  batch.clear();
  // Cheap next to that traversal, and it keeps coverage correct even for a
  // directory event that arrived without its own IN_ISDIR flag.
  resync_watches();
}
} // namespace monitor

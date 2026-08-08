#include <array>
#include <cerrno>
#include <cstring>
#include <expected>
#include <format>
#include <monitor.h>
#include <optional>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <string>
#include <sys/inotify.h>
#include <sys/poll.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>

namespace monitor {
Monitor::Monitor() {
  if (fd == -1) {
    auto msg =
        std::format("Failed to initialize monitor {}", ::strerror(errno));
    throw std::runtime_error(msg);
  }
  w = inotify_add_watch(
      fd, ".", IN_CLOSE_WRITE | IN_DELETE | IN_DONT_FOLLOW | IN_EXCL_UNLINK);
  if (w == -1) {
    auto msg =
        std::format("Failed to initialize monitor {}", ::strerror(errno));
    throw std::runtime_error(msg);
  }
}

Monitor::~Monitor() {
  if (fd != -1) {
    close(fd);
  }
}

Monitor::Monitor(Monitor &&m) noexcept { *this = std::move(m); }

auto Monitor::operator=(Monitor &&m) noexcept -> Monitor & {
  this->fd = m.fd;
  this->w = m.w;
  m.fd = -1;
  m.w = -1;
  return *this;
}

namespace {
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

class event_wrap {
  static constexpr int buf_size = sizeof(inotify_event) * 4;
  alignas(inotify_event) std::array<char, buf_size> buf{};

public:
  [[nodiscard]] auto ev() const -> inotify_event const * {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return reinterpret_cast<inotify_event const *>(buf.data());
  }

  auto read(int fd) -> ssize_t { return ::read(fd, buf.data(), buf.size()); }
};

auto _read(int fd) -> std::expected<event_wrap, std::string> {
  event_wrap ev{};
  auto const n = ev.read(fd);
  if (n == -1) {
    return std::unexpected(
        std::format("Failed to read on inotify fd. {}", ::strerror(errno)));
  }
  if (n == 0) {
    return std::unexpected("inotify fd closed");
  }
  if (n < sizeof(inotify_event)) {
    return std::unexpected("Read something smaller than an inotify event");
  }
  return ev;
}
} // namespace

auto Monitor::wait() const -> bool { return _wait(fd) > 0; }

auto Monitor::read() const
    -> std::expected<monitor::InotifyEvent, std::string> {
  return _read(fd).transform([](const event_wrap &e) -> InotifyEvent {
    InotifyEvent ev{};
    ev.mask = e.ev()->mask;
    ev.name = e.ev()->len == 0 ? std::nullopt
                               : std::make_optional(std::string{
                                     static_cast<const char *>(e.ev()->name)});
    return ev;
  });
}

void Monitor::discard() const { [[maybe_unused]] auto _ = this->read(); }
} // namespace monitor
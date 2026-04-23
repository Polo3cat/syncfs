#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <sys/inotify.h>

namespace monitor {
struct InotifyEvent{
	std::optional<std::string> name;
	uint32_t mask{};
};

class Monitor {
public:
	Monitor();
	~Monitor();

	Monitor(Monitor&& m) noexcept ;
	auto operator=(Monitor&& m) noexcept -> Monitor&;

	Monitor(const Monitor& m) = delete;
	auto operator=(const Monitor& m) -> Monitor = delete;

	// Returns when monitor is ready
	[[nodiscard]] auto wait() const -> bool;
	[[nodiscard]] auto read() const -> std::expected<InotifyEvent, std::string>;
	void discard() const;
private:
	int fd = inotify_init();
	int w = -1;
};
} // namespace monitor

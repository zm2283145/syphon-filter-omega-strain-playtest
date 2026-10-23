// Port-only (checkpoint 132md15): the platform movie decoder's backend choice (platform_movie.hpp).
#include "platform_movie.hpp"

#include <algorithm>
#include <cctype>
#include <mutex>

namespace omega::movie {

#if defined(OMEGA_MEDIA_FOUNDATION)
PlatformMovieBackend media_foundation_movie_backend();   // platform_movie_mf.cpp
#endif

namespace {

struct Choice {
    std::mutex mutex;
    bool installed = false;   // set_platform_movie_backend / disable_platform_movie_backend was called
    std::optional<PlatformMovieBackend> backend;
    std::optional<PlatformMovieBackend> platform_default;
    bool default_made = false;
};

Choice& choice() {
    static Choice c;
    return c;
}

}  // namespace

void set_platform_movie_backend(std::optional<PlatformMovieBackend> backend) {
    auto& c = choice();
    std::lock_guard lock(c.mutex);
    c.installed = backend.has_value();
    c.backend = std::move(backend);
}

void disable_platform_movie_backend() {
    auto& c = choice();
    std::lock_guard lock(c.mutex);
    c.installed = true;
    c.backend.reset();
}

const PlatformMovieBackend* platform_movie_backend() {
    auto& c = choice();
    std::lock_guard lock(c.mutex);
    if (c.installed) return c.backend ? &*c.backend : nullptr;
    if (!c.default_made) {
        c.default_made = true;
#if defined(OMEGA_MEDIA_FOUNDATION)
        c.platform_default = media_foundation_movie_backend();
#endif
    }
    return c.platform_default ? &*c.platform_default : nullptr;
}

bool is_platform_movie_name(const std::string& name) {
    const auto dot = name.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string e = name.substr(dot);
    for (auto& ch : e) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return e == ".mp4" || e == ".m4v" || e == ".mov";
}

}  // namespace omega::movie

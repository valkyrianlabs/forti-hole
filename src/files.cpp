#include "files.hpp"

#include <forti/engine/profile.hpp>

#include <deque>
#include <format>
#include <fstream>
#include <sstream>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fh {

    namespace fs = std::filesystem;
    using json = nlohmann::json;

    Home Home::from_env() {
        auto paths = forti::engine::Paths::from_env();
        // Before `forti home migrate` (legacy XDG layout) there is no home; keep forti-hole's files
        // with the engine's state so they still sit beside the plans and audit log.
        fs::path base = paths.home.empty() ? paths.state : paths.home;
        return {base / "hole"};
    }

    void ensure_private_dir(const fs::path& dir) {
        if (dir.empty()) return;
        std::error_code error;
        fs::create_directories(dir, error);
        if (error) throw FileError(std::format("cannot create {}: {}", dir.string(), error.message()));
        ::chmod(dir.c_str(), 0700);
    }

    void write_private(const fs::path& path, std::string_view text) {
        ensure_private_dir(path.parent_path());
        auto temp = path;
        temp += ".tmp";
        int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (fd < 0) throw FileError(std::format("cannot write {}", temp.string()));
        std::size_t written = 0;
        while (written < text.size()) {
            auto n = ::write(fd, text.data() + written, text.size() - written);
            if (n <= 0) {
                ::close(fd);
                throw FileError(std::format("cannot write {}", temp.string()));
            }
            written += static_cast<std::size_t>(n);
        }
        bool synced = ::fsync(fd) == 0;
        ::close(fd);
        if (!synced) throw FileError(std::format("cannot write {}", temp.string()));
        std::error_code error;
        fs::rename(temp, path, error);
        if (error) throw FileError(std::format("cannot replace {}: {}", path.string(), error.message()));
    }

    std::optional<std::string> read_file(const fs::path& path) {
        std::error_code error;
        if (!fs::exists(path, error)) return std::nullopt;
        std::ifstream in(path, std::ios::binary);
        if (!in) throw FileError(std::format("cannot read {}", path.string()));
        std::stringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

    void remove_file(const fs::path& path) {
        std::error_code error;
        fs::remove(path, error);
    }

    void append_history(const fs::path& path, const json& entry) {
        ensure_private_dir(path.parent_path());
        int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
        if (fd < 0) throw FileError(std::format("cannot append to {}", path.string()));
        auto line = entry.dump() + "\n";
        auto n = ::write(fd, line.data(), line.size());
        ::close(fd);
        if (n != static_cast<ssize_t>(line.size())) throw FileError(std::format("cannot append to {}", path.string()));
    }

    std::vector<json> read_history(const fs::path& path, std::size_t last) {
        std::ifstream in(path);
        std::deque<json> window;
        std::string line;
        while (std::getline(in, line)) {
            auto entry = json::parse(line, nullptr, false);
            if (entry.is_discarded()) continue;
            window.push_back(std::move(entry));
            if (window.size() > last) window.pop_front();
        }
        return {window.begin(), window.end()};
    }

    RunLock::RunLock(const fs::path& path) {
        ensure_private_dir(path.parent_path());
        fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (fd_ < 0) throw FileError(std::format("cannot open {}", path.string()));
        if (::flock(fd_, LOCK_EX | LOCK_NB) != 0) {
            ::close(fd_);
            fd_ = -1;
            throw FileError("another forti-hole run is in progress");
        }
    }

    RunLock::~RunLock() {
        if (fd_ >= 0) ::close(fd_);
    }

}  // namespace fh

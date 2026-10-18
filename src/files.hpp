//
// forti-hole's files, all under one directory of the forti home:
//
//   $FORTI_HOME/hole/          (default ~/.forti/hole)
//     config.json              the operator's (setup writes it)
//     state.json               last confirmed device state, source cache metadata, measurements
//     feeds/<feed>.txt         exactly what each feed was last confirmed to hold on the device
//     cache/<source>[.<variant>].list   the last good copy of each upstream list
//     journal.json             present only while a sync is writing to the device
//     history.jsonl            one line per run
//     lock                     held while a run is in progress
//
// Every file is written 0600 in 0700 directories, atomically (write, fsync, rename): an interrupted
// run leaves either the old file or the new one, never half of either.
//

#ifndef FORTI_HOLE_FILES_HPP
#define FORTI_HOLE_FILES_HPP

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace fh {

    class FileError : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    struct Home {
        std::filesystem::path dir;
        [[nodiscard]] std::filesystem::path config() const { return dir / "config.json"; }
        [[nodiscard]] std::filesystem::path state() const { return dir / "state.json"; }
        [[nodiscard]] std::filesystem::path feeds() const { return dir / "feeds"; }
        [[nodiscard]] std::filesystem::path cache() const { return dir / "cache"; }
        [[nodiscard]] std::filesystem::path journal() const { return dir / "journal.json"; }
        [[nodiscard]] std::filesystem::path history() const { return dir / "history.jsonl"; }
        [[nodiscard]] std::filesystem::path lock() const { return dir / "lock"; }

        // The forti home the engine uses ($FORTI_HOME, else ~/.forti), plus "hole".
        static Home from_env();
    };

    void ensure_private_dir(const std::filesystem::path& dir);
    void write_private(const std::filesystem::path& path, std::string_view text);
    // nullopt when the file does not exist; FileError when it exists and cannot be read.
    std::optional<std::string> read_file(const std::filesystem::path& path);
    void remove_file(const std::filesystem::path& path);

    void append_history(const std::filesystem::path& path, const nlohmann::json& entry);
    std::vector<nlohmann::json> read_history(const std::filesystem::path& path, std::size_t last);

    // An exclusive, non-blocking flock for the object's lifetime. Throws FileError when another run
    // holds it.
    class RunLock {
        int fd_ = -1;

    public:
        explicit RunLock(const std::filesystem::path& path);
        ~RunLock();
        RunLock(const RunLock&) = delete;
        RunLock& operator=(const RunLock&) = delete;
    };

}  // namespace fh

#endif  // FORTI_HOLE_FILES_HPP

#include "cli.hpp"

#include <sys/resource.h>

#include <string>
#include <vector>

int main(int argc, char** argv) {
    // Never leave a core file with credentials in it (forti-cli does the same).
    rlimit no_core{0, 0};
    ::setrlimit(RLIMIT_CORE, &no_core);
    std::vector<std::string> args(argv + 1, argv + argc);
    auto environment = fh::production_environment();
    return fh::run(args, environment);
}

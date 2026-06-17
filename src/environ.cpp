#include <unistd.h>
#include <limits.h>
#include <cstdlib>
#include <cstring>
#include <string>
#include <postline/common.h>

namespace postline {

fs::path POSTLINE_HOME;

void setup_environ()
{
    if (const char* p = std::getenv("POSTLINE_HOME"); p && *p) {
        POSTLINE_HOME = fs::path(p);
        log::info("Found POSTLINE_HOME in environ.");
    }
    else {
        // find self path
        char buf[PATH_MAX + 1];
        ssize_t n = ::readlink("/proc/self/exe", buf, PATH_MAX);
        CHECK(n >= 0);
        CHECK(n < PATH_MAX);
        buf[n] = '\0';
        fs::path exe = fs::path(buf);
        log::info("exe path: {}", exe.string());
        POSTLINE_HOME = exe.parent_path().parent_path();
    }
    log::info("POSTLINE_HOME: {}", POSTLINE_HOME.string());
    {
        // Expected:
        //   .../bin/postline
        //
        // Therefore:
        //   prefix = .../
        //   python = .../python
        fs::path python_dir = POSTLINE_HOME / "python";

        std::string path_str = python_dir.string();
        char const* old = std::getenv("PYTHONPATH");
        std::string value;
        if (old && old[0] != '\0') {
            value = path_str + ":" + old;
        } else {
            value = path_str;
        }
        int rc = ::setenv("PYTHONPATH", value.c_str(), 1);
        CHECK(rc == 0, "errno: {} ({})", errno, std::strerror(errno));
        log::info("PYTHONPATH updated: {}", value);
    }
    {
        fs::path bin_dir = POSTLINE_HOME / "bin";
        fs::path shell_dir = POSTLINE_HOME / "bin" / "shell";

        std::string path_str = bin_dir.string() + ':' + shell_dir.string();

        char const* old = std::getenv("PATH");
        std::string value;
        if (old && old[0] != '\0') {
            value = path_str + ":" + old;
        } else {
            value = path_str;
        }
        int rc = ::setenv("PATH", value.c_str(), 1);
        CHECK(rc == 0, "errno: {} ({})", errno, std::strerror(errno));
        log::info("PATH updated: {}", value);
    }

}

#if 0
void init_logging()
{
#if 0
    auto console_sink =
        std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
#else
    auto console_sink =
        std::make_shared<spdlog::sinks::ansicolor_stdout_sink_mt>();

    console_sink->set_color_mode(spdlog::color_mode::always);    
#endif

    console_sink->set_level(spdlog::level::info);
    console_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");

    auto file_sink =
        std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        "postline.log",
        5 * 1024 * 1024,
        3
    );
    file_sink->set_level(spdlog::level::debug);
    file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%t] %v");

    // Combine both sinks
    std::vector<spdlog::sink_ptr> sinks { console_sink, file_sink };

    auto logger = std::make_shared<spdlog::logger>("postline", sinks.begin(), sinks.end());
    logger->set_level(spdlog::level::debug);  // overall level
    // Keep the file sink useful while the runtime is still running, and across
    // CHECK/abort paths that do not run atexit handlers.
    logger->flush_on(spdlog::level::debug);

    spdlog::set_default_logger(logger);

    // Ensure logs flush on normal program termination, even if std::exit() is called.
    std::atexit([]{
        spdlog::shutdown();
    });
}
#endif

} // namespace postline

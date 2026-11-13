#define main cdrip_cli_main
#include "../src/main.cpp"
#undef main

#include <fstream>

int main(int argc, char** argv) {
    if (argc > 1) {
        try {
            parse_args(argc, argv);
            return 0;
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
    const auto directory = std::filesystem::temp_directory_path() /
        ("cdrip-config-" + std::to_string(getpid()));
    std::filesystem::create_directories(directory);
    const auto file = directory / "cdrip.conf";
    auto require = [](bool condition, const char* message) {
        if (!condition) { std::cerr << message << '\n'; std::exit(1); }
    };
    { std::ofstream out(file); out << "[cdrip]\n"; }
    const char* error = nullptr;
    auto* config = cdrip_load_config(file.c_str(), &error);
    require(config && !error, "Default settings must load");
    require(config->accuraterip && config->replaygain && !config->speed_fast,
        "Verification, ReplayGain and speed defaults");
    require(std::string(config->cover_art) == "auto" && std::string(config->discogs) == "always",
        "Default cover preview and source");
    cdrip_release_config(config);
    { std::ofstream out(file); out << "[cdrip]\ncover_art=kitty\naccuraterip=false\n"
        "speed=fast\nreplaygain=false\ndiscogs=fallback\nrecrawl_percent=3\ncompression=auto\n"; }
    config = cdrip_load_config(file.c_str(), &error);
    require(config && !error, "Explicit settings must load");
    require(std::string(config->cover_art) == "kitty" && !config->accuraterip &&
        config->speed_fast && !config->replaygain && std::string(config->discogs) == "fallback" &&
        config->recrawl_percent == 3 && config->compression_level == -1, "Typed settings must retain values");
    cdrip_release_config(config);

    std::vector<std::string> args{"cdrip", "-nv", "-v", "kitty", "-na", "-ro", "0", "-c", "auto"};
    std::vector<char*> raw;
    for (auto& arg : args) raw.push_back(arg.data());
    auto options = parse_args(static_cast<int>(raw.size()), raw.data());
    require(options.cover_art == "kitty" && options.accuraterip == false &&
        options.read_offset == 0 && options.compression_level == -1, "CLI aliases and last cover mode must apply");
    args = {"cdrip", "--cover-art", "kitty", "--no-cover-art"};
    raw.clear();
    for (auto& arg : args) raw.push_back(arg.data());
    options = parse_args(static_cast<int>(raw.size()), raw.data());
    require(options.cover_art == "none" && !options.accuraterip.has_value() &&
        !options.read_offset.has_value(), "Preview disable must not change verification or offset");
    require(!options.measure_tracks, "Normal CLI must not measure offsets");
    args = {"cdrip", "--measure-offset", "-d", "/dev/sr0"};
    raw.clear();
    for (auto& arg : args) raw.push_back(arg.data());
    options = parse_args(static_cast<int>(raw.size()), raw.data());
    require(options.measure_tracks == 3 && options.device == "/dev/sr0", "Measurement defaults to three without consuming next option");
    args = {"cdrip", "--measure-offset", "5"};
    raw.clear();
    for (auto& arg : args) raw.push_back(arg.data());
    require(parse_args(static_cast<int>(raw.size()), raw.data()).measure_tracks == 5, "Explicit measurement count");
    for (const auto* setting : {"cover_art=invalid", "accuraterip=invalid",
            "compression=3extra", "compression=9", "mode=typo", "unknown_key=true"}) {
        { std::ofstream out(file); out << "[cdrip]\n" << setting << '\n'; }
        error = nullptr;
        config = cdrip_load_config(file.c_str(), &error);
        const bool rejected = !config && error;
        cdrip_release_config(config);
        cdrip_release_error(error);
        if (!rejected) {
            std::cerr << "Invalid setting accepted: " << setting << '\n';
            std::filesystem::remove_all(directory);
            return 1;
        }
    }
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    for (const auto* entry : {"/dev/sr0=bad", "relative=6", "/dev/sr0=2147483648"}) {
        { std::ofstream out(file); out << "[read_offset]\n" << entry << '\n'; }
        error = nullptr;
        config = cdrip_load_config(file.c_str(), &error);
        const bool rejected = !config && error;
        cdrip_release_config(config);
        cdrip_release_error(error);
        require(rejected, "Invalid device offset entry must be rejected");
    }
    const auto first = (directory / "drive0").string();
    const auto second = (directory / "drive1").string();
    { std::ofstream out(first); }
    { std::ofstream out(second); }
    const auto ids = directory / "by-id";
    std::filesystem::create_directory(ids);
    std::filesystem::create_symlink(first, ids / "z-drive");
    std::filesystem::create_symlink(first, ids / "a-drive");
    std::filesystem::create_symlink(second, ids / "other-drive");
    std::filesystem::create_symlink(directory / "absent", ids / "disconnected");
    const auto paths = cdrip::detail::device_id_paths(first, ids.string());
    require(paths == std::vector<std::string>{(ids / "a-drive").string(), (ids / "z-drive").string()},
        "Stable paths must identify only the selected drive in sorted order");
    require(cdrip::detail::device_id_paths((ids / "a-drive").string(), ids.string()) == paths,
        "An explicitly selected stable path must identify the same drive");
    require(cdrip::detail::device_id_paths(first, (directory / "missing").string()).empty(),
        "Stable paths may be unavailable");
    std::ostringstream display;
    print_device_identity(display, first, paths);
    require(display.str().find(paths[0]) != std::string::npos && display.str().find(paths[1]) != std::string::npos,
        "Startup display must include the selected drive's stable paths");
    display.str("");
    print_device_identity(display, first, {});
    require(display.str().find("(unavailable)") != std::string::npos, "Missing identity must be explicit");
    { std::ofstream out(file); out << "[read_offset]\n" << first << "=21\n"
        << paths[0] << "=21\n" << second << "=-6\n" << (directory / "offline").string() << "=0\n"; }
    error = nullptr;
    config = cdrip_load_config(file.c_str(), &error);
    require(config && !error, "Device offsets must load");
    auto offset = cdrip::detail::select_read_offset(*config, paths[1], std::nullopt);
    require(offset.error.empty() && offset.samples == 21 && offset.source != "default", "Device aliases must select offset");
    require(cdrip::detail::select_read_offset(*config, second, std::nullopt).samples == -6, "Drives have independent offsets");
    require(cdrip::detail::select_read_offset(*config, "/unconfigured", std::nullopt).samples == 0, "Missing offset defaults to zero");
    config->read_offsets[1].samples = -6;
    require(!cdrip::detail::select_read_offset(*config, first, std::nullopt).error.empty(), "Conflicting aliases must fail");
    offset = cdrip::detail::select_read_offset(*config, first, 0);
    require(offset.error.empty() && offset.samples == 0 && offset.source == "CLI", "Explicit zero overrides device entries");
    cdrip_release_config(config);
    std::filesystem::remove_all(directory);
    std::cout << "CLI/config tests passed\n";
}

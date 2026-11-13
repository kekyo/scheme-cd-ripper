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
    std::cout << "CLI/config tests passed\n";
}

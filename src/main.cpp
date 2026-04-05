#include "app.hpp"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <argparse/argparse.hpp>
#include <exception>
#include <iostream>

int main(int argc, char* argv[]) {
    spdlog::set_pattern("[%H:%M:%S.%e] [%^%l%$] [%n] %v");

    argparse::ArgumentParser program("vulkan-fragment-renderer", "0.1.0");

    program.add_argument("shader")
        .help("path to GLSL fragment shader");

    program.add_argument("--low-dpi")
        .default_value(false)
        .implicit_value(true)
        .help("disable high-DPI scaling");

    try {
        program.parse_args(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        std::cerr << program;
        return 1;
    }

    try {
        vfr::App app(
            program.get<std::string>("shader"),
            !program.get<bool>("--low-dpi"));
        app.run();
    } catch (const std::exception& e) {
        spdlog::critical("fatal: {}", e.what());
        return 1;
    }

    return 0;
}

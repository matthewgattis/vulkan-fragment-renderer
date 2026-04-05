#pragma once

#include <spdlog/spdlog.h>

#define LOG_TRACE(...) spdlog::get(LOG_MODULE_NAME)->trace(__VA_ARGS__)
#define LOG_DEBUG(...) spdlog::get(LOG_MODULE_NAME)->debug(__VA_ARGS__)
#define LOG_INFO(...)  spdlog::get(LOG_MODULE_NAME)->info(__VA_ARGS__)
#define LOG_WARN(...)  spdlog::get(LOG_MODULE_NAME)->warn(__VA_ARGS__)
#define LOG_ERROR(...) spdlog::get(LOG_MODULE_NAME)->error(__VA_ARGS__)

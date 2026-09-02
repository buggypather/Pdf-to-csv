#pragma once
#include <cstdint>
namespace pdfcsv {
inline constexpr uint32_t PLUGIN_API_VERSION = 1;
struct PluginInfo { const char* name; const char* version; const char* description; uint32_t api_version; };
struct PluginHost { void (*log)(int level, const char* message); };
using plugin_init_fn = const PluginInfo* (*)(PluginHost*);
using plugin_shutdown_fn = void (*)();
}

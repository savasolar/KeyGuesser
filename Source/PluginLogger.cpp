// PluginLogger.cpp
#include "PluginLogger.h"
#include <JuceHeader.h>
#include <cstdio>
#include <cstdarg>

void plugin_log(const char* fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    DBG(buf);
}
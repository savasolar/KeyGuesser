// PluginLogger.cpp
#include "PluginLogger.h"
#include <JuceHeader.h>
#include <cstdio>
#include <cstdarg>

namespace
{
    juce::CriticalSection logLock;

    const juce::File& getLogFile()
    {
        static juce::File logFile = []
        {
            auto dir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                           .getChildFile("KeyGuesser");
            dir.createDirectory();
            return dir.getChildFile("ppd_debug.log");
        }();
        return logFile;
    }
}

void plugin_log(const char* fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    const juce::ScopedLock sl(logLock);
    auto ts = juce::Time::getCurrentTime().toString(true, true, true, true);
    getLogFile().appendText("[" + ts + "] " + juce::String(buf) + "\n", false, false);

   #if JUCE_DEBUG
    DBG(buf);
   #endif
}
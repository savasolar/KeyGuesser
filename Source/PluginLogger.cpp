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



//#include "PluginLogger.h"
//#include <JuceHeader.h>
//#include <cstdio>
//#include <cstdarg>
//#include <ctime>
//
//static juce::File getLogFile()
//{
//    juce::File dir("C:/PPDDEBUG");
//    if (!dir.exists())
//        dir.createDirectory();
//
//    return dir.getChildFile("ppd.log");
//}
//
//void plugin_log(const char* fmt, ...)
//{
//    char msg[1024];
//    va_list args;
//    va_start(args, fmt);
//    vsnprintf(msg, sizeof(msg), fmt, args);
//    va_end(args);
//
//    // timestamp
//    auto now = juce::Time::getCurrentTime();
//    juce::String ts = now.formatted("%Y-%m-%d %H:%M:%S.")
//        + juce::String(now.getMilliseconds()).paddedLeft('0', 3);
//
//    juce::String line = "[" + ts + "] " + msg + "\n";
//
//    // always write to file
//    juce::File logFile = getLogFile();
//    logFile.appendText(line, false, false);
//
//    // also keep DBG for Debug builds
//    DBG(line.trimEnd());
//}


// // PluginLogger.cpp
// #include "PluginLogger.h"
// #include <JuceHeader.h>
// #include <cstdio>
// #include <cstdarg>

// namespace
// {
    // juce::CriticalSection logLock;

    // const juce::File& getLogFile()
    // {
        // static juce::File logFile = []
        // {
            // auto dir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                           // .getChildFile("KeyGuesser");
            // dir.createDirectory();
            // return dir.getChildFile("ppd_debug.log");
        // }();
        // return logFile;
    // }
// }

// void plugin_log(const char* fmt, ...)
// {
    // char buf[512];
    // va_list args;
    // va_start(args, fmt);
    // vsnprintf(buf, sizeof(buf), fmt, args);
    // va_end(args);

    // const juce::ScopedLock sl(logLock);
    // auto ts = juce::Time::getCurrentTime().toString(true, true, true, true);
    // getLogFile().appendText("[" + ts + "] " + juce::String(buf) + "\n", false, false);

   // #if JUCE_DEBUG
    // DBG(buf);
   // #endif
// }

#include "Core/Log.h"
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace cherry
{
	void Log(LogLevel level, const char* fmt, ...)
	{
		static std::mutex sMutex;

		char message[2048];
		va_list args;
		va_start(args, fmt);
		vsnprintf(message, sizeof(message), fmt, args);
		va_end(args);

		const char* prefix = "[info] ";
		if (level == LogLevel::Warning) prefix = "[warn] ";
		if (level == LogLevel::Error)   prefix = "[error] ";

		std::lock_guard lock(sMutex);
		std::fprintf(level == LogLevel::Error ? stderr : stdout, "%s%s\n", prefix, message);
		std::fflush(stdout);
		OutputDebugStringA(prefix);
		OutputDebugStringA(message);
		OutputDebugStringA("\n");
	}
}

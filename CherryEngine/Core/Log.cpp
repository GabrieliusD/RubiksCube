#include "Core/Log.h"
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <deque>

namespace cherry
{
	namespace
	{
		constexpr size_t kMaxHistory = 4000;
		std::mutex sMutex;
		std::deque<LogEntry> sHistory;
		uint64_t sNextSequence = 1;
	}

	void ReadLog(uint64_t after, std::vector<LogEntry>& out)
	{
		std::lock_guard lock(sMutex);
		for (const LogEntry& entry : sHistory)
		{
			if (entry.sequence > after) out.push_back(entry);
		}
	}

	void Log(LogLevel level, const char* fmt, ...)
	{
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

		sHistory.push_back({ sNextSequence++, level, message });
		if (sHistory.size() > kMaxHistory) sHistory.pop_front();
	}
}

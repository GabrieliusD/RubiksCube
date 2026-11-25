#pragma once
#include "CherryAPI.h"
#include <cstdint>
#include <string>
#include <vector>

namespace cherry
{
	enum class LogLevel { Info, Warning, Error };

	// printf-style logging to stdout and the debugger output window.
	CHERRY_API void Log(LogLevel level, const char* fmt, ...);

	// The last few thousand log lines, for the editor's Output panel.
	struct LogEntry
	{
		uint64_t sequence = 0;   // increases by one per line, never reused
		LogLevel level = LogLevel::Info;
		std::string text;
	};
	// Appends the kept entries with sequence > `after` to `out`.
	CHERRY_API void ReadLog(uint64_t after, std::vector<LogEntry>& out);

	// Logs an error and shows it to the user in a message box where there is one
	// (`owner`: the native window it belongs to, or null).
	CHERRY_API void ShowError(const char* title, const char* text, void* owner = nullptr);
}

#define CHERRY_LOG(...)   ::cherry::Log(::cherry::LogLevel::Info, __VA_ARGS__)
#define CHERRY_WARN(...)  ::cherry::Log(::cherry::LogLevel::Warning, __VA_ARGS__)
#define CHERRY_ERROR(...) ::cherry::Log(::cherry::LogLevel::Error, __VA_ARGS__)

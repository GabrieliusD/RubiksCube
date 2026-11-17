#pragma once
#include "CherryAPI.h"

namespace cherry
{
	enum class LogLevel { Info, Warning, Error };

	// printf-style logging to stdout and the debugger output window.
	CHERRY_API void Log(LogLevel level, const char* fmt, ...);
}

#define CHERRY_LOG(...)   ::cherry::Log(::cherry::LogLevel::Info, __VA_ARGS__)
#define CHERRY_WARN(...)  ::cherry::Log(::cherry::LogLevel::Warning, __VA_ARGS__)
#define CHERRY_ERROR(...) ::cherry::Log(::cherry::LogLevel::Error, __VA_ARGS__)

#include "Core/BuildProcess.h"
#include "Core/Log.h"
#include <windows.h>
#include <mutex>
#include <thread>
#include <vector>

struct BuildProcess::Impl
{
	HANDLE job = nullptr;
	HANDLE process = nullptr;
	HANDLE readPipe = nullptr;
	std::thread reader;
	std::mutex mutex;
	std::string pending;   // output not yet split into lines
	bool running = false;

	void FlushLines(bool all)
	{
		std::vector<std::string> lines;
		{
			std::lock_guard lock(mutex);
			size_t start = 0;
			for (size_t i = 0; i < pending.size(); ++i)
			{
				if (pending[i] != '\n') continue;
				size_t end = i;
				if (end > start && pending[end - 1] == '\r') --end;
				lines.emplace_back(pending, start, end - start);
				start = i + 1;
			}
			pending.erase(0, start);
			if (all && !pending.empty())
			{
				lines.push_back(pending);
				pending.clear();
			}
		}
		for (const std::string& line : lines)
		{
			if (line.find_first_not_of(" \t") == std::string::npos) continue;
			const bool isError = line.find(" error ") != std::string::npos || line.find(": error") != std::string::npos;
			const bool isWarning = line.find(": warning") != std::string::npos;
			cherry::Log(isError ? cherry::LogLevel::Error : isWarning ? cherry::LogLevel::Warning : cherry::LogLevel::Info,
				"[build] %s", line.c_str());
		}
	}

	void StopReader()
	{
		if (reader.joinable())
		{
			// A child that outlived the build (an MSBuild node) may still hold the pipe open.
			CancelSynchronousIo(reader.native_handle());
			reader.join();
		}
		if (readPipe) { CloseHandle(readPipe); readPipe = nullptr; }
	}

	void Close()
	{
		StopReader();
		if (process) { CloseHandle(process); process = nullptr; }
		if (job) { CloseHandle(job); job = nullptr; }   // KILL_ON_JOB_CLOSE ends anything left
		running = false;
	}
};

BuildProcess::BuildProcess() : mImpl(std::make_unique<Impl>()) {}
BuildProcess::~BuildProcess() { mImpl->Close(); }

bool BuildProcess::IsRunning() const { return mImpl->running; }

bool BuildProcess::Start(const std::wstring& commandLine, std::string& error)
{
	Impl& b = *mImpl;
	if (b.running)
	{
		error = "A build is already running";
		return false;
	}
	b.Close();

	SECURITY_ATTRIBUTES inherit{ sizeof(inherit), nullptr, TRUE };
	HANDLE writePipe = nullptr;
	if (!CreatePipe(&b.readPipe, &writePipe, &inherit, 0))
	{
		error = "CreatePipe failed";
		return false;
	}
	SetHandleInformation(b.readPipe, HANDLE_FLAG_INHERIT, 0);

	b.job = CreateJobObjectW(nullptr, nullptr);
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
	limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
	SetInformationJobObject(b.job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));

	STARTUPINFOW startup{ sizeof(startup) };
	startup.dwFlags = STARTF_USESTDHANDLES;
	startup.hStdOutput = writePipe;
	startup.hStdError = writePipe;
	startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

	std::wstring mutableCommand = commandLine;
	PROCESS_INFORMATION info{};
	const BOOL created = CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, TRUE,
		CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup, &info);
	CloseHandle(writePipe);
	if (!created)
	{
		error = "CreateProcess failed (error " + std::to_string(GetLastError()) + ")";
		b.Close();
		return false;
	}
	AssignProcessToJobObject(b.job, info.hProcess);
	ResumeThread(info.hThread);
	CloseHandle(info.hThread);
	b.process = info.hProcess;
	b.running = true;

	b.reader = std::thread([&b]()
	{
		char buffer[4096];
		DWORD read = 0;
		while (ReadFile(b.readPipe, buffer, sizeof(buffer), &read, nullptr) && read > 0)
		{
			std::lock_guard lock(b.mutex);
			b.pending.append(buffer, read);
		}
	});
	return true;
}

bool BuildProcess::Poll(int& exitCode)
{
	Impl& b = *mImpl;
	if (!b.running) return false;
	b.FlushLines(false);
	if (WaitForSingleObject(b.process, 0) != WAIT_OBJECT_0) return false;

	DWORD code = 1;
	GetExitCodeProcess(b.process, &code);
	exitCode = static_cast<int>(code);
	// Give the reader a moment to drain the pipe before cancelling it.
	for (int i = 0; i < 20; ++i)
	{
		DWORD available = 0;
		if (!PeekNamedPipe(b.readPipe, nullptr, 0, nullptr, &available, nullptr) || available == 0) break;
		Sleep(5);
	}
	b.StopReader();
	b.FlushLines(true);
	b.Close();
	return true;
}

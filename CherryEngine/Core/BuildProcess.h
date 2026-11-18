#pragma once
#include <memory>
#include <string>

// Runs a command (the game module build) in the background. Its output goes to the
// engine log line by line, prefixed with "[build]". The process and everything it
// starts are killed if the editor closes first.
class BuildProcess
{
public:
	BuildProcess();
	~BuildProcess();
	BuildProcess(const BuildProcess&) = delete;
	BuildProcess& operator=(const BuildProcess&) = delete;

	bool Start(const std::wstring& commandLine, std::string& error);
	bool IsRunning() const;

	// Call once per frame: forwards new output to the log. Returns true on the frame the
	// process finished, with its exit code.
	bool Poll(int& exitCode);

private:
	struct Impl;
	std::unique_ptr<Impl> mImpl;
};

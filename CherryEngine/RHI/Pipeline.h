#pragma once
#include "RHI/RHI.h"
#include "ShaderSystem/ShaderCompiler.h"
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#include <memory>

namespace rhi
{
	// Named PSO cache with shader hot reload. Pipelines are looked up by name;
	// when any file under the shader root changes, every pipeline is rebuilt and
	// pipelines whose shaders fail to compile keep their previous PSO.
	class PipelineLibrary
	{
	public:
		void Initialize(Device& device, const std::filesystem::path& shaderRoot);
		void Shutdown();

		// Compiles immediately. Throws if the very first compile fails.
		void AddGraphics(const std::string& name, const GraphicsPipelineDesc& desc);
		void AddCompute(const std::string& name, const ComputePipelineDesc& desc);
		Pipeline* Get(const std::string& name) const;

		// Adds or replaces a pipeline without throwing (for shaders made at runtime). When the
		// build fails, a pipeline that already existed keeps its previous PSO and a new one is
		// stored without a PSO (Get returns nullptr); either way the new desc is kept, so the
		// next reload retries it. Returns false with the compiler output in `error`.
		bool SetGraphics(const std::string& name, const GraphicsPipelineDesc& desc, std::string* error = nullptr);
		// The same, built on a worker thread: the pipeline keeps its previous PSO (or none) until
		// PumpAsync applies the result. `done` then runs on the main thread with whether the
		// build worked. A newer request for the same name makes an older one moot (its `done`
		// never runs). Such pipelines also rebuild in the background on ReloadAll.
		void SetGraphicsAsync(const std::string& name, const GraphicsPipelineDesc& desc, std::function<void(bool ok)> done);
		// A background build of this pipeline hasn't finished yet.
		bool IsPending(const std::string& name) const;
		// Background builds that haven't finished.
		size_t PendingCount() const;
		// Applies finished background builds; call once per frame on the main thread.
		void PumpAsync();
		// Removes a pipeline (its PSO is released once in-flight frames are done with it).
		void Remove(const std::string& name);
		// The last build error of one pipeline ("" if its last build succeeded).
		const std::string& Error(const std::string& name) const;

		// Polls shader timestamps (cheap; rate-limited). Returns true if pipelines were rebuilt.
		bool PollHotReload();
		void ReloadAll();

		const std::filesystem::path& ShaderRoot() const { return mShaderRoot; }
		const std::string& LastError() const { return mLastError; }
		uint32_t ReloadCount() const { return mReloadCount; }

	private:
		struct Entry
		{
			bool compute = false;
			GraphicsPipelineDesc graphics;
			ComputePipelineDesc computeDesc;
			std::shared_ptr<Pipeline> pso;
			std::string error;
			bool async = false;          // built with SetGraphicsAsync
			bool pending = false;        // a background build is on its way
			uint64_t generation = 0;     // of the newest background build
		};

		// Compiles and creates the PSO; touches no shared state, so worker threads can call it
		// (each with its own compiler).
		bool CreatePso(ShaderCompiler& compiler, const std::string& name, const Entry& entry, std::shared_ptr<Pipeline>& pso, std::string& error) const;
		bool Build(const std::string& name, Entry& entry);
		void Adopt(Entry& entry, std::shared_ptr<Pipeline> pso);
		void StartWorkers();
		void StopWorkers();

		struct Job
		{
			std::string name;
			uint64_t generation = 0;
			Entry entry;                 // the desc to build
			std::function<void(bool)> done;
		};
		struct Result
		{
			std::string name;
			uint64_t generation = 0;
			bool ok = false;
			std::shared_ptr<Pipeline> pso;
			std::string error;
			std::function<void(bool)> done;
		};
		std::vector<std::thread> mWorkers;
		std::mutex mJobMutex;
		std::condition_variable mJobReady;
		std::deque<Job> mJobs;
		std::vector<Result> mResults;
		bool mStopping = false;
		uint64_t ShaderTreeSignature() const;

		Device* mDevice = nullptr;
		ShaderCompiler mCompiler;
		std::filesystem::path mShaderRoot;
		std::unordered_map<std::string, Entry> mEntries;
		uint64_t mShaderSignature = 0;
		uint64_t mLastPollTick = 0;
		std::string mLastError;
		std::string mNoError;
		uint32_t mReloadCount = 0;
	};
}

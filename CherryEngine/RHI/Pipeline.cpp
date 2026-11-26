#include "RHI/Pipeline.h"
#include "Core/Log.h"
#include <chrono>
#include <algorithm>

namespace rhi
{
	void PipelineLibrary::Initialize(Device& device, const std::filesystem::path& shaderRoot)
	{
		mDevice = &device;
		mShaderRoot = shaderRoot;
		mCompiler.Initialize();
		mShaderSignature = ShaderTreeSignature();
		CHERRY_LOG("Shader root: %s", mShaderRoot.string().c_str());
	}

	void PipelineLibrary::Shutdown()
	{
		StopWorkers();
		mEntries.clear();
	}

	void PipelineLibrary::AddGraphics(const std::string& name, const GraphicsPipelineDesc& desc)
	{
		Entry entry;
		entry.compute = false;
		entry.graphics = desc;
		if (!Build(name, entry))
		{
			throw std::runtime_error("Failed to build pipeline '" + name + "':\n" + mLastError);
		}
		mEntries[name] = std::move(entry);
	}

	void PipelineLibrary::AddCompute(const std::string& name, const ComputePipelineDesc& desc)
	{
		Entry entry;
		entry.compute = true;
		entry.computeDesc = desc;
		if (!Build(name, entry))
		{
			throw std::runtime_error("Failed to build pipeline '" + name + "':\n" + mLastError);
		}
		mEntries[name] = std::move(entry);
	}

	Pipeline* PipelineLibrary::Get(const std::string& name) const
	{
		auto it = mEntries.find(name);
		return it != mEntries.end() ? it->second.pso.get() : nullptr;
	}

	bool PipelineLibrary::SetGraphics(const std::string& name, const GraphicsPipelineDesc& desc, std::string* error)
	{
		Entry& entry = mEntries[name];   // a new entry, or the existing one (keeps its PSO on failure)
		entry.compute = false;
		entry.graphics = desc;
		const bool ok = Build(name, entry);
		if (error) *error = entry.error;
		return ok;
	}

	void PipelineLibrary::Remove(const std::string& name)
	{
		auto it = mEntries.find(name);
		if (it == mEntries.end()) return;
		if (it->second.pso) mDevice->DeferRelease(it->second.pso);
		mEntries.erase(it);
	}

	const std::string& PipelineLibrary::Error(const std::string& name) const
	{
		auto it = mEntries.find(name);
		return it != mEntries.end() ? it->second.error : mNoError;
	}

	bool PipelineLibrary::CreatePso(ShaderCompiler& compiler, const std::string& name, const Entry& entry, std::shared_ptr<Pipeline>& out, std::string& error) const
	{
		const bool spirv = mDevice->GetShaderTarget() == ShaderTarget::Spirv;
		auto compile = [&](const std::string& file, const std::string& entryPoint, ShaderType type, const std::vector<std::string>& defines, CompiledShader& out)
		{
			ShaderDesc sd;
			sd.type = type;
			sd.entryPoint = entryPoint;
			sd.filePath = mShaderRoot / file;
			sd.source = entry.compute ? std::string() : entry.graphics.source;
			sd.defines = defines;
			sd.includeDirs.push_back(mShaderRoot);
			sd.spirv = spirv;
			out = compiler.Compile(sd);
			if (!out.success)
			{
				error = out.errors.empty() ? file + " (" + entryPoint + "): no output from the compiler" : out.errors;
				return false;
			}
			return true;
		};

		if (entry.compute)
		{
			const ComputePipelineDesc& desc = entry.computeDesc;
			CompiledShader cs;
			if (!compile(desc.shaderFile, desc.entry, ShaderType::Compute, desc.defines, cs)) return false;
			out = mDevice->CreateComputePipeline(cs.bytecode, name, error);
		}
		else
		{
			const GraphicsPipelineDesc& desc = entry.graphics;
			CompiledShader vs, ps;
			if (!compile(desc.shaderFile, desc.vsEntry, ShaderType::Vertex, desc.defines, vs)) return false;
			if (!desc.psEntry.empty() && !compile(desc.shaderFile, desc.psEntry, ShaderType::Pixel, desc.defines, ps)) return false;
			out = mDevice->CreateGraphicsPipeline(desc, vs.bytecode, ps.bytecode, name, error);
		}
		return out != nullptr;
	}

	bool PipelineLibrary::Build(const std::string& name, Entry& entry)
	{
		std::shared_ptr<Pipeline> pso;
		std::string error;
		if (!CreatePso(mCompiler, name, entry, pso, error))
		{
			entry.error = error;
			mLastError = "[" + name + "] " + error;
			CHERRY_ERROR("Shader compile failed: %s", mLastError.c_str());
			return false;
		}
		Adopt(entry, pso);
		return true;
	}

	void PipelineLibrary::Adopt(Entry& entry, std::shared_ptr<Pipeline> pso)
	{
		entry.error.clear();
		// The old PSO may still be referenced by in-flight frames.
		if (entry.pso) mDevice->DeferRelease(entry.pso);
		entry.pso = std::move(pso);
	}

	// ---- Background builds ---------------------------------------------------------------

	void PipelineLibrary::SetGraphicsAsync(const std::string& name, const GraphicsPipelineDesc& desc, std::function<void(bool)> done)
	{
		Entry& entry = mEntries[name];
		entry.compute = false;
		entry.graphics = desc;
		entry.async = true;
		entry.pending = true;
		const uint64_t generation = ++entry.generation;

		StartWorkers();
		Job job;
		job.name = name;
		job.generation = generation;
		job.entry.graphics = desc;
		job.done = std::move(done);
		{
			std::lock_guard lock(mJobMutex);
			// A queued build of the same pipeline is out of date: drop it.
			mJobs.erase(std::remove_if(mJobs.begin(), mJobs.end(), [&](const Job& queued) { return queued.name == name; }), mJobs.end());
			mJobs.push_back(std::move(job));
		}
		mJobReady.notify_one();
	}

	bool PipelineLibrary::IsPending(const std::string& name) const
	{
		auto it = mEntries.find(name);
		return it != mEntries.end() && it->second.pending;
	}

	void PipelineLibrary::StartWorkers()
	{
		if (!mWorkers.empty()) return;
		const unsigned cores = std::max(2u, std::thread::hardware_concurrency());
		const unsigned count = std::clamp(cores / 4u, 1u, 4u);
		mStopping = false;
		for (unsigned i = 0; i < count; ++i)
		{
			mWorkers.emplace_back([this]
			{
				ShaderCompiler compiler;   // DXC objects aren't shared between threads
				compiler.Initialize();
				for (;;)
				{
					Job job;
					{
						std::unique_lock lock(mJobMutex);
						mJobReady.wait(lock, [this] { return mStopping || !mJobs.empty(); });
						if (mStopping) return;
						job = std::move(mJobs.front());
						mJobs.pop_front();
					}
					Result result;
					result.name = std::move(job.name);
					result.generation = job.generation;
					result.done = std::move(job.done);
					result.ok = CreatePso(compiler, result.name, job.entry, result.pso, result.error);
					std::lock_guard lock(mJobMutex);
					mResults.push_back(std::move(result));
				}
			});
		}
	}

	void PipelineLibrary::StopWorkers()
	{
		{
			std::lock_guard lock(mJobMutex);
			mStopping = true;
			mJobs.clear();
		}
		mJobReady.notify_all();
		for (std::thread& worker : mWorkers) worker.join();
		mWorkers.clear();
		mResults.clear();
	}

	void PipelineLibrary::PumpAsync()
	{
		std::vector<Result> results;
		{
			std::lock_guard lock(mJobMutex);
			results.swap(mResults);
		}
		for (Result& result : results)
		{
			auto it = mEntries.find(result.name);
			if (it == mEntries.end() || it->second.generation != result.generation)
			{
				// Removed, or a newer build is on its way: this one is out of date.
				if (result.pso) mDevice->DeferRelease(result.pso);
				continue;
			}
			Entry& entry = it->second;
			entry.pending = false;
			if (result.ok)
			{
				Adopt(entry, result.pso);
			}
			else
			{
				entry.error = result.error;
				mLastError = "[" + result.name + "] " + result.error;
				CHERRY_ERROR("Shader compile failed: %s", mLastError.c_str());
			}
			if (result.done) result.done(result.ok);
		}
	}



	uint64_t PipelineLibrary::ShaderTreeSignature() const
	{
		// Hash of every file's path and timestamp: catches edits, new/deleted files and
		// files restored with an older timestamp.
		uint64_t hash = 1469598103934665603ull;
		auto mix = [&hash](uint64_t value)
		{
			hash ^= value;
			hash *= 1099511628211ull;
		};
		std::error_code ec;
		for (auto it = std::filesystem::recursive_directory_iterator(mShaderRoot, ec);
			!ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
		{
			if (it->is_regular_file(ec))
			{
				mix(std::hash<std::wstring>{}(it->path().wstring()));
				mix(static_cast<uint64_t>(it->last_write_time(ec).time_since_epoch().count()));
			}
		}
		return hash;
	}

	bool PipelineLibrary::PollHotReload()
	{
		const uint64_t now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
		if (now - mLastPollTick < 500)
		{
			return false;
		}
		mLastPollTick = now;

		const uint64_t signature = ShaderTreeSignature();
		if (signature == mShaderSignature)
		{
			return false;
		}
		mShaderSignature = signature;
		ReloadAll();
		return true;
	}

	void PipelineLibrary::ReloadAll()
	{
		mLastError.clear();
		uint32_t failed = 0;
		uint32_t queued = 0;
		for (auto& [name, entry] : mEntries)
		{
			if (entry.async)
			{
				// Runtime-made pipelines (surface shaders) rebuild in the background.
				SetGraphicsAsync(name, entry.graphics, {});
				++queued;
			}
			else if (!Build(name, entry))
			{
				++failed;
			}
		}
		if (queued) CHERRY_LOG("Rebuilding %u surface shader pipeline(s) in the background", queued);
		++mReloadCount;
		if (failed == 0)
		{
			CHERRY_LOG("Shaders reloaded (%zu pipelines)", mEntries.size());
		}
		else
		{
			CHERRY_WARN("Shader reload: %u pipeline(s) failed, keeping previous versions", failed);
		}
	}
}

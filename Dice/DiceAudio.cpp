#include "DiceAudio.h"
#include "DiceSim.h"
#include <windows.h>
#include <xaudio2.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <random>

#pragma comment(lib, "xaudio2.lib")

namespace dice
{
	namespace synth
	{
		std::vector<float> NoiseBuffer(uint32_t seed)
		{
			std::mt19937 rng(seed);
			std::uniform_real_distribution<float> uniform(-1.0f, 1.0f);
			std::vector<float> noise(static_cast<size_t>(kSampleRate * 0.2));
			for (float& s : noise) s = uniform(rng);
			return noise;
		}

		std::vector<float> NoiseHit(const std::vector<float>& noise, double freq, double q, double gain, double decay)
		{
			// RBJ band-pass, constant 0 dB peak gain (WebAudio BiquadFilterNode "bandpass").
			const double w0 = 2 * kPi * freq / kSampleRate, alpha = std::sin(w0) / (2 * q);
			const double a0 = 1 + alpha;
			const double b0 = alpha / a0, b2 = -alpha / a0, a1 = -2 * std::cos(w0) / a0, a2 = (1 - alpha) / a0;
			const size_t length = std::min(noise.size(), static_cast<size_t>((decay + 0.02) * kSampleRate));
			std::vector<float> out(length);
			double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
			for (size_t n = 0; n < length; ++n)
			{
				const double x = noise[n];
				const double y = b0 * x + b2 * x2 - a1 * y1 - a2 * y2;
				x2 = x1; x1 = x; y2 = y1; y1 = y;
				const double t = static_cast<double>(n) / kSampleRate;
				const double g = t < decay ? gain * std::pow(0.0001 / gain, t / decay) : 0.0001;
				out[n] = static_cast<float>(y * g);
			}
			return out;
		}

		std::vector<float> Clack(const std::vector<float>& noise)
		{
			std::vector<float> out = NoiseHit(noise, 2600, 2.5, 0.9, 0.07);
			const size_t length = static_cast<size_t>(0.1 * kSampleRate);
			if (out.size() < length) out.resize(length, 0.0f);
			double phase = 0;   // in cycles
			for (size_t n = 0; n < length; ++n)
			{
				const double t = static_cast<double>(n) / kSampleRate;
				const double f = t < 0.06 ? 2100 * std::pow(900.0 / 2100.0, t / 0.06) : 900;
				const double tri = 4 * std::fabs(phase - std::floor(phase + 0.5)) - 1;   // triangle in [-1, 1]
				const double g = t < 0.08 ? 0.35 * std::pow(0.0001 / 0.35, t / 0.08) : 0.0001;
				out[n] += static_cast<float>(tri * g);
				phase += f / kSampleRate;
			}
			return out;
		}
	}

	struct Audio::Impl
	{
		IXAudio2* engine = nullptr;
		IXAudio2MasteringVoice* master = nullptr;
		std::array<IXAudio2SourceVoice*, 32> voices{};
		std::vector<float> clack;
		std::vector<std::vector<float>> knocks, ticks;   // variants at the reference's random frequencies
		std::mt19937 rng{ 0x5eed };
		std::chrono::steady_clock::time_point lastKnock{}, lastTick{};
		bool comInitialised = false;

		void PlayBuffer(const std::vector<float>& samples, float volume)
		{
			for (IXAudio2SourceVoice* voice : voices)
			{
				if (!voice) continue;
				XAUDIO2_VOICE_STATE state{};
				voice->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
				if (state.BuffersQueued > 0) continue;
				XAUDIO2_BUFFER buffer{};
				buffer.AudioBytes = static_cast<UINT32>(samples.size() * sizeof(float));
				buffer.pAudioData = reinterpret_cast<const BYTE*>(samples.data());
				buffer.Flags = XAUDIO2_END_OF_STREAM;
				master->SetVolume(volume);
				voice->SubmitSourceBuffer(&buffer);
				voice->Start();
				return;
			}
		}

		void Release()
		{
			for (IXAudio2SourceVoice*& voice : voices)
				if (voice) { voice->DestroyVoice(); voice = nullptr; }
			if (master) { master->DestroyVoice(); master = nullptr; }
			if (engine) { engine->Release(); engine = nullptr; }
			if (comInitialised) { CoUninitialize(); comInitialised = false; }
		}
	};

	Audio::Audio() : mImpl(std::make_unique<Impl>()) {}
	Audio::~Audio() { mImpl->Release(); }

	bool Audio::Running() const { return mImpl->engine != nullptr; }

	void Audio::Start()
	{
		Impl& a = *mImpl;
		if (a.engine) return;
		const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		a.comInitialised = SUCCEEDED(com);
		if (FAILED(XAudio2Create(&a.engine, 0, XAUDIO2_DEFAULT_PROCESSOR))) { a.Release(); return; }
		if (FAILED(a.engine->CreateMasteringVoice(&a.master, 1, synth::kSampleRate))) { a.Release(); return; }
		WAVEFORMATEX format{};
		format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
		format.nChannels = 1;
		format.nSamplesPerSec = synth::kSampleRate;
		format.wBitsPerSample = 32;
		format.nBlockAlign = 4;
		format.nAvgBytesPerSec = synth::kSampleRate * 4;
		for (IXAudio2SourceVoice*& voice : a.voices)
			if (FAILED(a.engine->CreateSourceVoice(&voice, &format))) voice = nullptr;

		const std::vector<float> noise = synth::NoiseBuffer(0xd1ce);
		a.clack = synth::Clack(noise);
		std::uniform_real_distribution<double> unit(0.0, 1.0);
		for (int k = 0; k < 8; ++k)
		{
			a.knocks.push_back(synth::NoiseHit(noise, 1500 + unit(a.rng) * 500, 4, 0.45, 0.05));
			a.ticks.push_back(synth::NoiseHit(noise, 1100 + unit(a.rng) * 900, 5, 0.12, 0.035));
		}
	}

	void Audio::Play(Sound sound, float volume, bool landTicks)
	{
		Impl& a = *mImpl;
		if (!a.engine) return;
		const auto now = std::chrono::steady_clock::now();
		std::uniform_int_distribution<int> variant(0, 7);
		switch (sound)
		{
		case Sound::Clack:
			a.PlayBuffer(a.clack, volume);
			break;
		case Sound::Knock:
			if (now - a.lastKnock < std::chrono::milliseconds(40)) return;
			a.lastKnock = now;
			a.PlayBuffer(a.knocks[variant(a.rng)], volume);
			break;
		case Sound::Tick:
			if (!landTicks) return;
			if (now - a.lastTick < std::chrono::milliseconds(35)) return;   // mass landings don't become noise
			a.lastTick = now;
			a.PlayBuffer(a.ticks[variant(a.rng)], volume);
			break;
		}
	}
}

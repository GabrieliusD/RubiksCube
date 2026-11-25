#pragma once
// The prototype's three sounds (PROTOTYPE_EXPORT.md B15 "Audio"), synthesised once the way its
// WebAudio graph makes them (band-passed noise with an exponential decay; the clack adds a
// falling triangle tone) and played through XAudio2. The engine has no audio, so this lives in
// the game module.
#include <cstdint>
#include <memory>
#include <vector>

namespace dice
{
	enum class Sound : uint8_t;

	// The synthesis, separate from the device so tests can check it.
	namespace synth
	{
		constexpr int kSampleRate = 48000;
		// A 0.2 s buffer of white noise in [-1, 1] (the reference reuses one for every hit).
		std::vector<float> NoiseBuffer(uint32_t seed);
		// noiseHit(freq, q, gain, decay): the noise through a band-pass biquad (WebAudio's
		// "bandpass"), gain ramping exponentially from `gain` to 0.0001 over `decay` seconds,
		// stopped 20 ms after.
		std::vector<float> NoiseHit(const std::vector<float>& noise, double freq, double q, double gain, double decay);
		// clack(): noiseHit(2600, 2.5, 0.9, 0.07) plus a triangle 2100 -> 900 Hz over 60 ms
		// (gain 0.35 decaying over 80 ms, stopped at 100 ms).
		std::vector<float> Clack(const std::vector<float>& noise);
	}

	class Audio
	{
	public:
		Audio();
		~Audio();
		Audio(const Audio&) = delete;
		Audio& operator=(const Audio&) = delete;

		// Starts the audio device (the reference waits for the first click or tap). Safe to call again.
		void Start();
		bool Running() const;
		// Plays one sound at `volume` (master gain). Knocks are throttled to one per 40 ms and
		// land ticks to one per 35 ms (real time), as in the reference; `landTicks` off mutes ticks.
		void Play(Sound sound, float volume, bool landTicks);

	private:
		struct Impl;
		std::unique_ptr<Impl> mImpl;
	};
}

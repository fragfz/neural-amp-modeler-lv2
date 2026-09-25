#pragma once

#ifdef ENABLE_CAB

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "3rdparty/FFTConvolver/FFTConvolver.h"
#include "3rdparty/FFTConvolver/TwoStageFFTConvolver.h"

namespace NAM {

	// Cab IR convolver using two-stage partitioned FFT convolution (same
	// engine as the MOD CabinetLoader, HiFi-LoFi FFTConvolver): a small head
	// partition runs on the audio thread, the long tail partitions are
	// processed on a background thread, keeping the audio-thread cost low and
	// steady for any IR length. IRs too short for the two-stage split fall
	// back to a single non-threaded FFTConvolver.
	class CabConvolver : private fftconvolver::TwoStageFFTConvolver
	{
	public:
		// Builds a convolver for the given IR. Call off the audio thread.
		// Returns nullptr if the IR cannot be used.
		static CabConvolver* Create(const std::vector<float>& ir);

		~CabConvolver() override;

		// audio thread; in and out must not overlap
		void Process(const float* in, float* out, uint32_t nSamples);

	private:
		CabConvolver() = default;

		void startBackgroundProcessing() override;
		void waitForBackgroundProcessing() override;

		void BackgroundThread();

		fftconvolver::FFTConvolver mShortIrConvolver; // used when the IR is too short for the two-stage split
		bool mUseTwoStage = false;

		std::thread mBackgroundThread;
		std::mutex mMutex;
		std::condition_variable mCond;
		bool mWorkPending = false;
		bool mWorkRunning = false;
		bool mStopping = false;
	};

	// Loads a cab IR from a .wav file, downmixing to mono and resampling to
	// the plugin sample rate if needed (linear interpolation). Call off the
	// audio thread. Returns false if the file cannot be read or has no audio.
	bool LoadCabIr(const char* path, double sampleRate, std::vector<float>& ir);

} // namespace NAM

#endif // ENABLE_CAB

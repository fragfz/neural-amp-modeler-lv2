#include "cab_convolver.h"

#ifdef ENABLE_CAB

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>

namespace NAM {

	// partition sizes used by the MOD CabinetLoader (mono cabinet variant)
	static constexpr size_t kHeadBlockSize = 256;
	static constexpr size_t kTailBlockSize = 4096;

	CabConvolver* CabConvolver::Create(const std::vector<float>& ir)
	{
		if (ir.empty())
			return nullptr;

		std::unique_ptr<CabConvolver> conv(new CabConvolver());

		// the two-stage split only pays off for longer IRs; short IRs run
		// just as well (and without a thread) on a single FFTConvolver
		if (ir.size() > 2 * kTailBlockSize)
		{
			if (!conv->init(kHeadBlockSize, kTailBlockSize, ir.data(), ir.size()))
				return nullptr;

			conv->mUseTwoStage = true;

			conv->mBackgroundThread = std::thread(&CabConvolver::BackgroundThread, conv.get());
		}
		else
		{
			if (!conv->mShortIrConvolver.init(kHeadBlockSize, ir.data(), ir.size()))
				return nullptr;

			conv->mUseTwoStage = false;
		}

		return conv.release();
	}

	CabConvolver::~CabConvolver()
	{
		if (mUseTwoStage)
		{
			{
				std::lock_guard<std::mutex> lock(mMutex);

				mStopping = true;
			}

			mCond.notify_all();

			if (mBackgroundThread.joinable())
				mBackgroundThread.join();
		}
	}

	void CabConvolver::Process(const float* in, float* out, uint32_t nSamples)
	{
		if (mUseTwoStage)
			process(in, out, nSamples);
		else
			mShortIrConvolver.process(in, out, nSamples);
	}

	void CabConvolver::startBackgroundProcessing()
	{
		{
			std::lock_guard<std::mutex> lock(mMutex);

			mWorkPending = true;
		}

		mCond.notify_one();
	}

	void CabConvolver::waitForBackgroundProcessing()
	{
		std::unique_lock<std::mutex> lock(mMutex);

		mCond.wait(lock, [this]{ return !mWorkPending && !mWorkRunning; });
	}

	void CabConvolver::BackgroundThread()
	{
		for (;;)
		{
			std::unique_lock<std::mutex> lock(mMutex);

			mCond.wait(lock, [this]{ return mWorkPending || mStopping; });

			if (mStopping)
				return;

			mWorkPending = false;
			mWorkRunning = true;

			lock.unlock();

			doBackgroundProcessing();

			lock.lock();

			mWorkRunning = false;

			lock.unlock();

			mCond.notify_all();
		}
	}

	// --- minimal WAV reader (RIFF PCM + IEEE float, mono downmix, resample) ---

	namespace
	{
		uint32_t ReadU32LE(const uint8_t* p)
		{
			return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
		}

		uint16_t ReadU16LE(const uint8_t* p)
		{
			return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
		}

		bool ReadWav(const char* path, uint32_t& sampleRate, uint32_t& channels, std::vector<float>& samples)
		{
			FILE* f = fopen(path, "rb");

			if (f == nullptr)
				return false;

			std::unique_ptr<FILE, int(*)(FILE*)> file(f, &fclose);

			uint8_t header[12];

			if (fread(header, 1, 12, f) != 12 ||
				memcmp(header, "RIFF", 4) != 0 ||
				memcmp(header + 8, "WAVE", 4) != 0)
			{
				return false;
			}

			bool haveFmt = false;
			bool haveData = false;

			uint16_t audioFormat = 0;
			uint16_t bitsPerSample = 0;

			std::vector<uint8_t> data;

			while (!haveFmt || !haveData)
			{
				uint8_t chunkHeader[8];

				if (fread(chunkHeader, 1, 8, f) != 8)
					return false;

				const uint32_t chunkSize = ReadU32LE(chunkHeader + 4);

				if (memcmp(chunkHeader, "fmt ", 4) == 0 && chunkSize >= 16)
				{
					std::vector<uint8_t> fmt(chunkSize);

					if (fread(fmt.data(), 1, chunkSize, f) != chunkSize)
						return false;

					audioFormat = ReadU16LE(fmt.data());
					channels = ReadU16LE(fmt.data() + 2);
					sampleRate = ReadU32LE(fmt.data() + 4);
					bitsPerSample = ReadU16LE(fmt.data() + 14);

					if (channels == 0 || sampleRate == 0)
						return false;

					haveFmt = true;
				}
				else if (memcmp(chunkHeader, "data", 4) == 0)
				{
					// sanity cap: 256 MB of raw data
					if (chunkSize > (256u << 20))
						return false;

					data.resize(chunkSize);

					if (chunkSize > 0 && fread(data.data(), 1, chunkSize, f) != chunkSize)
						return false;

					if (chunkSize % 2 == 1)
						fgetc(f); // chunks are word-aligned

					haveData = true;
				}
				else
				{
					// skip unknown chunk
					if (fseek(f, chunkSize + (chunkSize % 2), SEEK_CUR) != 0)
						return false;
				}
			}

			const size_t bytesPerSample = (bitsPerSample + 7) / 8;

			if (bytesPerSample == 0 || bytesPerSample > 8)
				return false;

			const size_t frameSize = bytesPerSample * channels;

			if (frameSize == 0 || data.size() % frameSize != 0)
				return false;

			const size_t numFrames = data.size() / frameSize;

			samples.resize(numFrames * channels);

			for (size_t frame = 0; frame < numFrames; ++frame)
			{
				for (size_t ch = 0; ch < channels; ++ch)
				{
					const uint8_t* p = data.data() + frame * frameSize + ch * bytesPerSample;

					float value = 0.0f;

					if (audioFormat == 3 && bitsPerSample == 32)
					{
						memcpy(&value, p, 4);
					}
					else if (audioFormat == 3 && bitsPerSample == 64)
					{
						double d;

						memcpy(&d, p, 8);

						value = (float)d;
					}
					else if (audioFormat == 1 && bitsPerSample == 8)
					{
						value = ((int)p[0] - 128) / 128.0f;
					}
					else if (audioFormat == 1 && bitsPerSample == 16)
					{
						int16_t s;

						memcpy(&s, p, 2);

						value = s / 32768.0f;
					}
					else if (audioFormat == 1 && bitsPerSample == 24)
					{
						const int32_t raw = (int32_t)p[0] | ((int32_t)p[1] << 8) | ((int32_t)p[2] << 16);

						value = (raw - ((raw & 0x800000) << 1)) / 8388608.0f;
					}
					else if (audioFormat == 1 && bitsPerSample == 32)
					{
						int32_t s;

						memcpy(&s, p, 4);

						value = s / 2147483648.0f;
					}
					else
					{
						return false; // unsupported encoding
					}

					samples[frame * channels + ch] = value;
				}
			}

			return numFrames > 0;
		}
	}

	bool LoadCabIr(const char* path, double sampleRate, std::vector<float>& ir)
	{
		ir.clear();

		uint32_t wavRate = 0;
		uint32_t channels = 0;

		std::vector<float> interleaved;

		if (!ReadWav(path, wavRate, channels, interleaved))
			return false;

		const size_t numFrames = interleaved.size() / channels;

		// downmix to mono
		std::vector<float> mono(numFrames);

		for (size_t frame = 0; frame < numFrames; ++frame)
		{
			float sum = 0.0f;

			for (size_t ch = 0; ch < channels; ++ch)
				sum += interleaved[frame * channels + ch];

			mono[frame] = sum / (float)channels;
		}

		if (wavRate == (uint32_t)sampleRate)
		{
			ir.swap(mono);
		}
		else
		{
			// linear resample to the plugin sample rate (fine for cab IRs)
			const double step = (double)wavRate / sampleRate;

			const size_t outLen = (size_t)((double)numFrames / step);

			ir.resize(outLen ? outLen : 1);

			for (size_t i = 0; i < ir.size(); ++i)
			{
				const double pos = (double)i * step;

				const size_t i0 = (size_t)pos;

				const size_t i1 = (i0 + 1 < numFrames) ? i0 + 1 : i0;

				const float frac = (float)(pos - (double)i0);

				ir[i] = (1.0f - frac) * mono[i0] + frac * mono[i1];
			}
		}

		return !ir.empty();
	}

} // namespace NAM

#endif // ENABLE_CAB

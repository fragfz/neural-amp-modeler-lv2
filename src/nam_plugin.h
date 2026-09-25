#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string_view>
#include <string>
#include <vector>

// LV2
#include <lv2/core/lv2.h>
#include <lv2/core/lv2_util.h>
#include <lv2/atom/atom.h>
#include <lv2/log/log.h>
#include <lv2/log/logger.h>
#include <lv2/urid/urid.h>
#include <lv2/atom/forge.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/options/options.h>
#include <lv2/patch/patch.h>
#include <lv2/worker/worker.h>
#include <lv2/state/state.h>
#include <lv2/units/units.h>

#include <NeuralAudio/NeuralModel.h>

#define PlUGIN_URI "http://github.com/fragfz/neural-amp-modeler-dual-chain"
#define MODEL1_URI PlUGIN_URI "#model1"
#define MODEL2_URI PlUGIN_URI "#model2"
#define MODEL_URI PlUGIN_URI "#model"	// legacy state key, restored into slot 0
#define CAB_URI PlUGIN_URI "#cab"

#ifdef ENABLE_EQ
// Global 3-band EQ (low shelf, peaking mid, high shelf) at the end of the
// chain, after the DC blocker. RBJ audio-cookbook biquads, settings matching
// the tone3000-plugin tone stack (based on tone-3000/nam-pedal eq3band.h).
class EqBiquad
{
public:
	void Reset()
	{
		b0 = 1.0f;
		b1 = b2 = a1 = a2 = 0.0f;
		z1 = z2 = 0.0f;
	}

	// transposed direct form II
	inline float Process(float x)
	{
		float y = b0 * x + z1;
		z1 = b1 * x - a1 * y + z2;
		z2 = b2 * x - a2 * y;
		return y;
	}

	// store coefficients normalized by a0
	void SetCoeffs(float B0, float B1, float B2, float A0, float A1, float A2)
	{
		const float inv = 1.0f / A0;
		b0 = B0 * inv;
		b1 = B1 * inv;
		b2 = B2 * inv;
		a1 = A1 * inv;
		a2 = A2 * inv;
	}

private:
	float b0 = 1.0f;
	float b1 = 0.0f;
	float b2 = 0.0f;
	float a1 = 0.0f;
	float a2 = 0.0f;
	float z1 = 0.0f;
	float z2 = 0.0f;
};

class Eq3Band
{
public:
	void Init(float sampleRate)
	{
		sr = sampleRate;
		low.Reset();
		mid.Reset();
		high.Reset();
		SetBass(0.0f);
		SetMid(0.0f);
		SetTreble(0.0f);
	}

	// gain_db is the boost/cut for each band (0 dB == flat)
	void SetBass(float gainDb) { LowShelf(low, kBassFreq, gainDb); }

	void SetMid(float gainDb)
	{
		// wider bell on boost, narrower notch on cut
		const float q = (gainDb >= 0.0f) ? kMidQBoost : kMidQCut;
		Peaking(mid, kMidFreq, q, gainDb);
	}

	void SetTreble(float gainDb) { HighShelf(high, kTrebleFreq, gainDb); }

	inline float Process(float x)
	{
		return high.Process(mid.Process(low.Process(x)));
	}

private:
	// band parameters matched to the NAM tone stack (tone3000-plugin)
	static constexpr float kBassFreq = 150.0f;
	static constexpr float kMidFreq = 425.0f;
	static constexpr float kMidQBoost = 0.7f;
	static constexpr float kMidQCut = 1.5f;
	static constexpr float kTrebleFreq = 1800.0f;
	// S=1 gives alpha = sw/sqrt(2), i.e. Q=0.707 shelves
	static constexpr float kShelfSlope = 1.0f;
	static constexpr float kPi = 3.14159265358979323846f;

	void Peaking(EqBiquad& bq, float f0, float q, float db)
	{
		const float A = powf(10.0f, db / 40.0f);
		const float w0 = 2.0f * kPi * f0 / sr;
		const float cw = cosf(w0);
		const float sw = sinf(w0);
		const float alpha = sw / (2.0f * q);

		bq.SetCoeffs(
			1.0f + alpha * A,
			-2.0f * cw,
			1.0f - alpha * A,
			1.0f + alpha / A,
			-2.0f * cw,
			1.0f - alpha / A
		);
	}

	void LowShelf(EqBiquad& bq, float f0, float db)
	{
		const float A = powf(10.0f, db / 40.0f);
		const float w0 = 2.0f * kPi * f0 / sr;
		const float cw = cosf(w0);
		const float sw = sinf(w0);
		const float alpha = sw / 2.0f * sqrtf((A + 1.0f / A) * (1.0f / kShelfSlope - 1.0f) + 2.0f);
		const float beta = 2.0f * sqrtf(A) * alpha;
		const float Ap1 = A + 1.0f;
		const float Am1 = A - 1.0f;

		bq.SetCoeffs(
			A * (Ap1 - Am1 * cw + beta),
			2.0f * A * (Am1 - Ap1 * cw),
			A * (Ap1 - Am1 * cw - beta),
			Ap1 + Am1 * cw + beta,
			-2.0f * (Am1 + Ap1 * cw),
			Ap1 + Am1 * cw - beta
		);
	}

	void HighShelf(EqBiquad& bq, float f0, float db)
	{
		const float A = powf(10.0f, db / 40.0f);
		const float w0 = 2.0f * kPi * f0 / sr;
		const float cw = cosf(w0);
		const float sw = sinf(w0);
		const float alpha = sw / 2.0f * sqrtf((A + 1.0f / A) * (1.0f / kShelfSlope - 1.0f) + 2.0f);
		const float beta = 2.0f * sqrtf(A) * alpha;
		const float Ap1 = A + 1.0f;
		const float Am1 = A - 1.0f;

		bq.SetCoeffs(
			A * (Ap1 + Am1 * cw + beta),
			-2.0f * A * (Am1 + Ap1 * cw),
			A * (Ap1 + Am1 * cw - beta),
			Ap1 - Am1 * cw + beta,
			2.0f * (Am1 - Ap1 * cw),
			Ap1 - Am1 * cw - beta
		);
	}

	float sr = 48000.0f;
	EqBiquad low;
	EqBiquad mid;
	EqBiquad high;
};
#endif

namespace NAM {
	static constexpr unsigned int MAX_FILE_NAME = 1024;

	static constexpr uint32_t kNumSlots = 2;

	enum LV2WorkType {
		kWorkTypeLoad,
		kWorkTypeSwitch,
		kWorkTypeFree,
		kWorkTypeLoadCab,
		kWorkTypeSwitchCab,
		kWorkTypeFreeCab
	};

	struct LV2LoadModelMsg {
		LV2WorkType type;
		uint32_t slot;
		char path[MAX_FILE_NAME];
	};

	struct LV2SwitchModelMsg {
		LV2WorkType type;
		uint32_t slot;
		char path[MAX_FILE_NAME];
		NeuralAudio::NeuralModel* model;
	};

	struct LV2FreeModelMsg {
		LV2WorkType type;
		NeuralAudio::NeuralModel* model;
	};

	class CabConvolver;

	struct LV2LoadCabMsg {
		LV2WorkType type;
		char path[MAX_FILE_NAME];
	};

	struct LV2SwitchCabMsg {
		LV2WorkType type;
		char path[MAX_FILE_NAME];
		CabConvolver* convolver;
	};

	struct LV2FreeCabMsg {
		LV2WorkType type;
		CabConvolver* convolver;
	};

	class Plugin {
	public:
		// order matches lv2:index in the bundle ttl
		struct Ports {
			const LV2_Atom_Sequence* control;
			LV2_Atom_Sequence* notify;
			const float* audio_in;
			float* audio_out;
			float* enable1;
			float* input_level1;
			float* output_level1;
			float* enable2;
			float* input_level2;
			float* output_level2;
#ifdef ENABLE_CAB
			float* cab_enable;
#endif
#ifdef ENABLE_EQ
			float* eq_bass;
			float* eq_mid;
			float* eq_treble;
#endif
			float* quality_scale;
		// Stage-2 wet-loop audio ports (appended after all control ports in
		// the ttl, indices computed by CMake after QUALITY_INDEX)
		float* output1;      // send: NAM1 block output tap (loop 1 send)
		const float* input2; // return: summed into the NAM2 block input (loop 1 return)
		float* output2;      // send: NAM2 block output tap (loop 2 send)
		const float* input3; // return: summed into the cab input (loop 2 return)
		};

		Ports ports = {};

		double sampleRate;

		LV2_URID_Map* map = nullptr;
		LV2_Log_Logger logger = {};
		LV2_Worker_Schedule* schedule = nullptr;

		NeuralAudio::NeuralModelLoader loader;
		NeuralAudio::NeuralModel* currentModels[kNumSlots] = { nullptr, nullptr };
		std::string currentModelPaths[kNumSlots];

#ifdef ENABLE_CAB
		CabConvolver* cabConvolver = nullptr;
		std::string cabPath;
#endif

		// global DC blocker state (end of chain, before the EQ)
		float dcPrevInput = 0;
		float dcPrevOutput = 0;
		float dcCoefficient = 0;

		Plugin();
		~Plugin();

		bool initialize(double rate, const LV2_Feature* const* features) noexcept;
		void set_max_buffer_size(int size) noexcept;
		void activate() noexcept;
		void process(uint32_t n_samples) noexcept;

		void write_current_path(uint32_t slot);

#ifdef ENABLE_CAB
		void write_cab_path();
#endif

		static uint32_t options_get(LV2_Handle instance, LV2_Options_Option* options);
		static uint32_t options_set(LV2_Handle instance, const LV2_Options_Option* options);

		static LV2_Worker_Status work(LV2_Handle instance, LV2_Worker_Respond_Function respond, LV2_Worker_Respond_Handle handle,
			uint32_t size, const void* data);
		static LV2_Worker_Status work_response(LV2_Handle instance, uint32_t size, const void* data);

		static LV2_State_Status save(LV2_Handle instance, LV2_State_Store_Function store, LV2_State_Handle handle, uint32_t flags, 
			const LV2_Feature* const* features);
		static LV2_State_Status restore(LV2_Handle instance, LV2_State_Retrieve_Function retrieve, LV2_State_Handle handle, uint32_t flags,
			const LV2_Feature* const* features);

	private:
		struct URIs {
			LV2_URID atom_Object;
			LV2_URID atom_Float;
			LV2_URID atom_Int;
			LV2_URID atom_Path;
			LV2_URID atom_URID;
			LV2_URID bufSize_maxBlockLength;
			LV2_URID patch_Set;
			LV2_URID patch_Get;
			LV2_URID patch_property;
			LV2_URID patch_value;
			LV2_URID units_frame;
			LV2_URID model_Path;
			LV2_URID model1_Path;
			LV2_URID model2_Path;
#ifdef ENABLE_CAB
			LV2_URID cab_Path;
#endif
		};

		URIs uris = {};

		LV2_Atom_Forge atom_forge = {};
		LV2_Atom_Forge_Frame sequence_frame;

		// internal staging buffers (bufA: block 1 input, bufB: between blocks)
		std::vector<float> bufA;
		std::vector<float> bufB;

		float inputLevel[kNumSlots] = { 0, 0 };
		float outputLevel[kNumSlots] = { 0, 0 };
		int32_t maxBufferSize = 512;

#ifdef ENABLE_EQ
		Eq3Band eq;
		float lastEqBass = 0;
		float lastEqMid = 0;
		float lastEqTreble = 0;
#endif
	};
}

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>
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

#ifdef ENABLE_EQ
#include "eq.h"
#endif

// Full plugin URI is injected by CMake (-DPLUGIN_URI) so it follows the
// dynamically computed name suffix (_T + C/D/E per enabled DSP block).
// The fallback keeps standalone builds working.
#ifndef PLUGIN_URI
#define PLUGIN_URI "http://github.com/fragfz/neural-amp-modeler-lv2-t"
#endif

#define MODEL_URI PLUGIN_URI "#model"
#define CAB_URI PLUGIN_URI "#cab"

namespace NAM {
	static constexpr unsigned int MAX_FILE_NAME = 1024;

	class CabConvolver;

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
		char path[MAX_FILE_NAME];
	};

	struct LV2SwitchModelMsg {
		LV2WorkType type;
		char path[MAX_FILE_NAME];
		NeuralAudio::NeuralModel* model;
	};

	struct LV2FreeModelMsg {
		LV2WorkType type;
		NeuralAudio::NeuralModel* model;
	};

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
		struct Ports {
			const LV2_Atom_Sequence* control;
			LV2_Atom_Sequence* notify;
			const float* audio_in;
			float* audio_out;
			float* input_level;
			float* output_level;
			float* quality_scale;
#ifdef ENABLE_CAB
			float* cab_enable;
#endif
#ifdef ENABLE_EQ
			float* eq_bass;
			float* eq_mid;
			float* eq_treble;
#endif
			float* out_calibrated;
		};

		Ports ports = {};

		double sampleRate;

		LV2_URID_Map* map = nullptr;
		LV2_Log_Logger logger = {};
		LV2_Worker_Schedule* schedule = nullptr;

		NeuralAudio::NeuralModelLoader loader;
		NeuralAudio::NeuralModel* currentModel = nullptr;
		std::string currentModelPath;

		// cached "Out Calibrated" output adjustment for the current model
		// (parsed once per loaded model in process(), never per audio block)
		const NeuralAudio::NeuralModel* calibratedAdjustmentModel = nullptr;
		float cachedCalibratedAdjustmentDB = 0;
		bool cachedCalibratedValid = false;
#ifdef ENABLE_CAB
		CabConvolver* cabConvolver = nullptr;
		std::string cabPath;
#endif
		float prevDCInput = 0;
		float prevDCOutput = 0;

		Plugin();
		~Plugin();

		bool initialize(double rate, const LV2_Feature* const* features) noexcept;
		void set_max_buffer_size(int size) noexcept;
		void activate() noexcept;
		void process(uint32_t n_samples) noexcept;

		void write_current_path();
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
#ifdef ENABLE_CAB
			LV2_URID cab_Path;
#endif
		};

		URIs uris = {};

		LV2_Atom_Forge atom_forge = {};
		LV2_Atom_Forge_Frame sequence_frame;

		float inputLevel = 0;
		float outputLevel = 0;
		int32_t maxBufferSize = 512;
		float bypassThresholdLinear = 0;
		uint32_t silentSamples = 0;
		bool smartBypassed = true;

		// staging buffers (cab convolver must not run in place; grown off-thread only)
		std::vector<float> scratch;
		std::vector<float> scratch2;

		// DC blocker coefficient (y[n] = x[n] - x[n-1] + R * y[n-1])
		float dcCoefficient = 1.0f;

#ifdef ENABLE_EQ
		Eq eq;
		float lastEqBass = 0;
		float lastEqMid = 0;
		float lastEqTreble = 0;
#endif
	};
}

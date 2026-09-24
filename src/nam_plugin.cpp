#include <algorithm>
#include <cmath>
#include <utility>
#include <cassert>

#include "nam_plugin.h"

#define SMOOTH_EPSILON .0001f

#ifndef DC_BLOCKER_HZ
#define DC_BLOCKER_HZ 5.0f
#endif

#ifndef BYPASS_DB_THRESHOLD
#define BYPASS_DB_THRESHOLD -100
#endif

namespace NAM {
	Plugin::Plugin()
	{
		// prevent allocations on the audio thread
		for (uint32_t slot = 0; slot < kNumSlots; ++slot)
		{
			currentModelPaths[slot].reserve(MAX_FILE_NAME + 1);
		}

		bufA.reserve(maxBufferSize);
		bufB.reserve(maxBufferSize);

		bufA.resize(maxBufferSize);
		bufB.resize(maxBufferSize);

		bypassThresholdLinear = powf(10, BYPASS_DB_THRESHOLD * 0.05f);

//		NeuralAudio::NeuralModel::SetLSTMLoadMode(
//#ifdef LSTM_PREFER_NAM
//			NeuralAudio::PreferNAMCore
//#else
//			NeuralAudio::PreferRTNeural
//#endif
//		);
//
//		NeuralAudio::NeuralModel::SetWaveNetLoadMode(
//#ifdef WAVENET_PREFER_NAM
//			NeuralAudio::PreferNAMCore
//#else
//			NeuralAudio::PreferRTNeural
//#endif
		//);
	}

	Plugin::~Plugin()
	{
		for (uint32_t slot = 0; slot < kNumSlots; ++slot)
		{
			delete currentModels[slot];
		}
	}

	bool Plugin::initialize(double sampleRate, const LV2_Feature* const* features) noexcept
	{
		this->sampleRate = sampleRate;

		dcCoefficient = 1.0f - ((float)(2.0 * M_PI * DC_BLOCKER_HZ) / (float)sampleRate);

		loader.SetExternalSampleRate((int)sampleRate);

		// for fetching initial options, can be null
		LV2_Options_Option* options = nullptr;

		for (size_t i = 0; features[i]; ++i)
		{
			if (std::string(features[i]->URI) == std::string(LV2_URID__map))
				map = static_cast<LV2_URID_Map*>(features[i]->data);
			else if (std::string(features[i]->URI) == std::string(LV2_WORKER__schedule))
				schedule = static_cast<LV2_Worker_Schedule*>(features[i]->data);
			else if (std::string(features[i]->URI) == std::string(LV2_LOG__log))
				logger.log = static_cast<LV2_Log_Log*>(features[i]->data);
			else if (std::string(features[i]->URI) == std::string(LV2_OPTIONS__options))
				options = static_cast<LV2_Options_Option*>(features[i]->data);
		}
	
		lv2_log_logger_set_map(&logger, map);

		if (!map)
		{
			lv2_log_error(&logger, "Missing required feature: `%s`", LV2_URID__map);

			return false;
		}

		if (!schedule)
		{
			lv2_log_error(&logger, "Missing required feature: `%s`", LV2_WORKER__schedule);

			return false;
		}

		lv2_atom_forge_init(&atom_forge, map);

		uris.atom_Object = map->map(map->handle, LV2_ATOM__Object);
		uris.atom_Float = map->map(map->handle, LV2_ATOM__Float);
		uris.atom_Int = map->map(map->handle, LV2_ATOM__Int);
		uris.atom_Path = map->map(map->handle, LV2_ATOM__Path);
		uris.atom_URID = map->map(map->handle, LV2_ATOM__URID);
		uris.bufSize_maxBlockLength = map->map(map->handle, LV2_BUF_SIZE__maxBlockLength);
		uris.patch_Set = map->map(map->handle, LV2_PATCH__Set);
		uris.patch_Get = map->map(map->handle, LV2_PATCH__Get);
		uris.patch_property = map->map(map->handle, LV2_PATCH__property);
		uris.patch_value = map->map(map->handle, LV2_PATCH__value);
		uris.units_frame = map->map(map->handle, LV2_UNITS__frame);

		uris.model_Path = map->map(map->handle, MODEL_URI);
		uris.model1_Path = map->map(map->handle, MODEL1_URI);
		uris.model2_Path = map->map(map->handle, MODEL2_URI);

		if (options != nullptr)
			options_set(this, options);

		return true;
	}

	// runs on non-RT, can block or use [de]allocations
	LV2_Worker_Status Plugin::work(LV2_Handle instance, LV2_Worker_Respond_Function respond, LV2_Worker_Respond_Handle handle,
		uint32_t size, const void* data)
	{
		switch (*(const LV2WorkType*)data)
		{
			case kWorkTypeLoad:
			{
				auto msg = static_cast<const LV2LoadModelMsg*>(data);
				auto nam = static_cast<NAM::Plugin*>(instance);

				if (msg->slot >= kNumSlots)
					return LV2_WORKER_ERR_UNKNOWN;

				NeuralAudio::NeuralModel* model = nullptr;
				LV2SwitchModelMsg response = { kWorkTypeSwitch, msg->slot, {}, {} };
				LV2_Worker_Status result = LV2_WORKER_SUCCESS;

				try
				{
					// load model from path
					const size_t pathlen = strlen(msg->path);

					if (pathlen == 0 || pathlen >= MAX_FILE_NAME)
					{
						// avoid logging an error on an empty path.
						// but do clear the model.
						model = nullptr;
					}
					else
					{
						lv2_log_trace(&nam->logger, "Staging model change: `%s`\n", msg->path);

						model = nam->loader.CreateFromFile(msg->path);
					}

					if (model != nullptr)
					{
						response.model = model;

						memcpy(response.path, msg->path, pathlen);
					}
				}
				catch (const std::exception&)
				{
				}

				if (model == nullptr)
				{
					response.path[0] = '\0';

					lv2_log_error(&nam->logger, "Unable to load model from: '%s'\n", msg->path);
				}

				respond(handle, sizeof(response), &response);

				return result;
			}

			case kWorkTypeFree:
			{
				auto msg = static_cast<const LV2FreeModelMsg*>(data);
				delete msg->model;

				return LV2_WORKER_SUCCESS;
			}

			case kWorkTypeSwitch:
				// should not happen!
				break;
		}

		return LV2_WORKER_ERR_UNKNOWN;
	}

	// runs on RT, right after process(), must not block or [de]allocate memory
	LV2_Worker_Status Plugin::work_response(LV2_Handle instance, uint32_t size,	const void* data)
	{
		if (*(const LV2WorkType*)data != kWorkTypeSwitch)
			return LV2_WORKER_ERR_UNKNOWN;

		auto msg = static_cast<const LV2SwitchModelMsg*>(data);
		auto nam = static_cast<NAM::Plugin*>(instance);

		if (msg->slot >= kNumSlots)
			return LV2_WORKER_ERR_UNKNOWN;

		const uint32_t slot = msg->slot;

		// prepare reply for deleting old model
		LV2FreeModelMsg reply = { kWorkTypeFree, nam->currentModels[slot] };

		// swap current model with new one
		nam->currentModels[slot] = msg->model;
		nam->currentModelPaths[slot] = msg->path;
		assert(nam->currentModelPaths[slot].capacity() >= MAX_FILE_NAME + 1);

		if (nam->currentModels[slot] != nullptr)
		{
			int receptiveFieldSize = nam->currentModels[slot]->GetReceptiveFieldSize();

			if (receptiveFieldSize > -1)
			{
				// A newly loaded model is prewarmed to have a silent sample history
				nam->silentSamples[slot] = receptiveFieldSize;
				nam->smartBypassed[slot] = true;
			}
		}

		// send reply
		nam->schedule->schedule_work(nam->schedule->handle, sizeof(reply), &reply);

		// report change to host/ui
		nam->write_current_path(slot);

		return LV2_WORKER_SUCCESS;
	}

	void Plugin::set_max_buffer_size(int size) noexcept
	{
		maxBufferSize = size;

		loader.SetDefaultMaxAudioBufferSize(size);

		// grow (never shrink) the staging buffers so process() never allocates
		if ((int)bufA.size() < size)
		{
			bufA.resize(size);
			bufB.resize(size);
		}
	}

	void Plugin::process(uint32_t n_samples) noexcept
	{
		lv2_atom_forge_set_buffer(&atom_forge, (uint8_t*)ports.notify, ports.notify->atom.size);
		lv2_atom_forge_sequence_head(&atom_forge, &sequence_frame, uris.units_frame);

		if (*(ports.quality_scale) != loader.GetDefaultQualityScaleFactor())
		{
			// Do this before checking the model path parameter so we make sure to set the quality first
			loader.SetDefaultQualityScaleFactor(*(ports.quality_scale));
		}

		LV2_ATOM_SEQUENCE_FOREACH(ports.control, event)
		{
			if (event->body.type == uris.atom_Object)
			{
				const auto obj = reinterpret_cast<LV2_Atom_Object*>(&event->body);
				if (obj->body.otype == uris.patch_Get)
				{
					write_current_path(0);
					write_current_path(1);
				}
				else if (obj->body.otype == uris.patch_Set)
				{
					const LV2_Atom* property = NULL;
					const LV2_Atom* file_path = NULL;

					lv2_atom_object_get(obj,
					                    uris.patch_property, &property,
					                    uris.patch_value, &file_path,
					                    0);

					if (property && property->type == uris.atom_URID &&
						file_path && file_path->type == uris.atom_Path &&
						file_path->size > 0 && file_path->size < MAX_FILE_NAME)
					{
						uint32_t slot = kNumSlots;
						if (((const LV2_Atom_URID*)property)->body == uris.model1_Path)
							slot = 0;
						else if (((const LV2_Atom_URID*)property)->body == uris.model2_Path)
							slot = 1;
						else if (((const LV2_Atom_URID*)property)->body == uris.model_Path)
							slot = 0;	// legacy model parameter maps to slot 0
						if (slot < kNumSlots)
						{
							LV2LoadModelMsg msg = { kWorkTypeLoad, slot, {} };

							memcpy(msg.path, file_path + 1, file_path->size);

							schedule->schedule_work(schedule->handle, sizeof(msg), &msg);
						}
					}
				}
			}
		}

		float level;

		float model1InputAdjustmentDB = 0;
		float model1LoudnessAdjustmentDB = 0;
		float model2InputAdjustmentDB = 0;
		float model2LoudnessAdjustmentDB = 0;

		if (currentModels[0] != nullptr)
		{
			if (*(ports.quality_scale) != currentModels[0]->GetQualityScaleFactor())
			{
				currentModels[0]->SetQualityScaleFactor(*(ports.quality_scale));
			}

			model1InputAdjustmentDB = currentModels[0]->GetRecommendedInputDBAdjustment();
			model1LoudnessAdjustmentDB = currentModels[0]->GetRecommendedOutputDBAdjustment();
		}

		if (currentModels[1] != nullptr)
		{
			if (*(ports.quality_scale) != currentModels[1]->GetQualityScaleFactor())
			{
				currentModels[1]->SetQualityScaleFactor(*(ports.quality_scale));
			}

			model2InputAdjustmentDB = currentModels[1]->GetRecommendedInputDBAdjustment();
			model2LoudnessAdjustmentDB = currentModels[1]->GetRecommendedOutputDBAdjustment();
		}

		// --- Stage 1: input level 1 (audio_in -> bufA) ---

		float desiredInputLevel = powf(10, (*(ports.input_level1) + model1InputAdjustmentDB) * 0.05f);

		if (fabs(desiredInputLevel - inputLevel[0]) > SMOOTH_EPSILON)
		{
			level = inputLevel[0];

			for (unsigned int i = 0; i < n_samples; i++)
			{
				// do very basic smoothing
				level = (.99f * level) + (.01f * desiredInputLevel);

				bufA[i] = ports.audio_in[i] * level;
			}

			inputLevel[0] = level;
		}
		else
		{
			level = inputLevel[0] = desiredInputLevel;

			for (unsigned int i = 0; i < n_samples; i++)
			{
				bufA[i] = ports.audio_in[i] * level;
			}
		}

		// --- Stage 2: NAM 1 (bufA in place) ---

		bool bypass1 = false;

#ifdef SMART_BYPASS_ENABLED
		if (currentModels[0] != nullptr)
		{
			int receptiveFieldSamples = currentModels[0]->GetReceptiveFieldSize();

			if (receptiveFieldSamples > -1)
			{
				for (unsigned int i = 0; i < n_samples; i++)
				{
					if (abs(bufA[i]) <= bypassThresholdLinear)
					{
						silentSamples[0]++;
					}
					else
					{
						silentSamples[0] = 0;
					}
				}

				if (silentSamples[0] >= (uint32_t)receptiveFieldSamples)
				{
					silentSamples[0] = (uint32_t)receptiveFieldSamples;	// Prevent silentSamples growing and eventually overflowing uint32

					bypass1 = smartBypassed[0];
					smartBypassed[0] = true;	// If we aren't already, we'll be bypassed on the next process call
				}
				else
					smartBypassed[0] = false;
			}
		}
#endif

		if (currentModels[0] != nullptr && *(ports.enable1) > 0.5f && !bypass1)
		{
			currentModels[0]->Process(bufA.data(), bufA.data(), n_samples);
		}

		// --- Stage 3: DC blocker + output level 1 + input level 2 (bufA -> bufB) ---

		// DC blocking only makes sense when NAM 1 actually ran
		const bool useDC = currentModels[0] != nullptr && *(ports.enable1) > 0.5f;

		float desiredOut1Level = powf(10, (*(ports.output_level1) + model1LoudnessAdjustmentDB) * 0.05f);
		float desiredIn2Level = powf(10, (*(ports.input_level2) + model2InputAdjustmentDB) * 0.05f);

		if (fabs(desiredOut1Level - outputLevel[0]) > SMOOTH_EPSILON || fabs(desiredIn2Level - inputLevel[1]) > SMOOTH_EPSILON)
		{
			float level1 = outputLevel[0];
			float level2 = inputLevel[1];
			for (unsigned int i = 0; i < n_samples; i++)
			{
				// do very basic smoothing
				level1 = (.99f * level1) + (.01f * desiredOut1Level);
				level2 = (.99f * level2) + (.01f * desiredIn2Level);

				float sample = bufA[i];

				if (useDC)
				{
					// dc blocker
					float dcInput = sample;

					sample = sample - dcPrevInput + dcCoefficient * dcPrevOutput;

					dcPrevInput = dcInput;
					dcPrevOutput = sample;
				}

				bufB[i] = sample * level1 * level2;
			}

			outputLevel[0] = level1;
			inputLevel[1] = level2;
		}
		else
		{
			float level1 = outputLevel[0] = desiredOut1Level;
			float level2 = inputLevel[1] = desiredIn2Level;

			for (unsigned int i = 0; i < n_samples; i++)
			{
				float sample = bufA[i];

				if (useDC)
				{
					// dc blocker
					float dcInput = sample;

					sample = sample - dcPrevInput + dcCoefficient * dcPrevOutput;

					dcPrevInput = dcInput;
					dcPrevOutput = sample;
				}

				bufB[i] = sample * level1 * level2;
			}
		}

		// --- Stage 4: NAM 2 (bufB in place) ---

		bool bypass2 = false;

#ifdef SMART_BYPASS_ENABLED
		if (currentModels[1] != nullptr)
		{
			int receptiveFieldSamples = currentModels[1]->GetReceptiveFieldSize();

			if (receptiveFieldSamples > -1)
			{
				for (unsigned int i = 0; i < n_samples; i++)
				{
					if (abs(bufB[i]) <= bypassThresholdLinear)
					{
						silentSamples[1]++;
					}
					else
					{
						silentSamples[1] = 0;
					}
				}

				if (silentSamples[1] >= (uint32_t)receptiveFieldSamples)
				{
					silentSamples[1] = (uint32_t)receptiveFieldSamples;	// Prevent silentSamples growing and eventually overflowing uint32

					bypass2 = smartBypassed[1];
					smartBypassed[1] = true;	// If we aren't already, we'll be bypassed on the next process call
				}
				else
					smartBypassed[1] = false;
			}
		}
#endif

		if (currentModels[1] != nullptr && *(ports.enable2) > 0.5f && !bypass2)
		{
			currentModels[1]->Process(bufB.data(), bufB.data(), n_samples);
		}

		// --- Stage 5: output level 2 (bufB -> audio_out) ---

		float desiredOutputLevel = powf(10, (*(ports.output_level2) + model2LoudnessAdjustmentDB) * 0.05f);

		if (fabs(desiredOutputLevel - outputLevel[1]) > SMOOTH_EPSILON)
		{
			level = outputLevel[1];

			for (unsigned int i = 0; i < n_samples; i++)
			{
				// do very basic smoothing
				level = (.99f * level) + (.01f * desiredOutputLevel);

				ports.audio_out[i] = bufB[i] * level;
			}

			outputLevel[1] = level;
		}
		else
		{
			level = outputLevel[1] = desiredOutputLevel;

			for (unsigned int i = 0; i < n_samples; i++)
			{
				ports.audio_out[i] = bufB[i] * level;
			}
		}
	}

	uint32_t Plugin::options_get(LV2_Handle, LV2_Options_Option*)
	{
		// currently unused
		return LV2_OPTIONS_ERR_UNKNOWN;
	}

	uint32_t Plugin::options_set(LV2_Handle instance, const LV2_Options_Option* options)
	{
		auto nam = static_cast<NAM::Plugin*>(instance);

		for (int i=0; options[i].key && options[i].type; ++i)
		{
			if (options[i].key == nam->uris.bufSize_maxBlockLength && options[i].type == nam->uris.atom_Int)
			{
				nam->set_max_buffer_size(*(const int32_t*)options[i].value);
				break;
			}
		}

		return LV2_OPTIONS_SUCCESS;
	}

	LV2_State_Status Plugin::save(LV2_Handle instance, LV2_State_Store_Function store, LV2_State_Handle handle, 
		uint32_t flags, const LV2_Feature* const* features)
	{
		auto nam = static_cast<NAM::Plugin*>(instance);

		lv2_log_trace(&nam->logger, "Saving state\n");

		if (!nam->currentModels[0] && !nam->currentModels[1])
		{
			return LV2_STATE_SUCCESS;
		}

		LV2_State_Map_Path* map_path = (LV2_State_Map_Path*)lv2_features_data(features, LV2_STATE__mapPath);

		if (map_path == nullptr)
		{
			lv2_log_error(&nam->logger, "LV2_STATE__mapPath unsupported by host\n");

			return LV2_STATE_ERR_NO_FEATURE;
		}

		const LV2_URID modelPathKeys[kNumSlots] = { nam->uris.model1_Path, nam->uris.model2_Path };

		for (uint32_t slot = 0; slot < kNumSlots; ++slot)
		{
			if (!nam->currentModels[slot])
			{
				continue;
			}

			// Map absolute sample path to an abstract state path
			char* apath = map_path->abstract_path(map_path->handle, nam->currentModelPaths[slot].c_str());

			store(handle, modelPathKeys[slot], apath, strlen(apath) + 1, nam->uris.atom_Path,
				LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);

			LV2_State_Free_Path* free_path = (LV2_State_Free_Path *)lv2_features_data(features, LV2_STATE__freePath);

			if (free_path != nullptr)
			{
				free_path->free_path(free_path->handle, apath);
			}
			else
			{
#ifndef _WIN32	// Can't free host-allocated memory on plugin side under Windows
				free(apath);
#endif
			}
		}

		return LV2_STATE_SUCCESS;
	}

	LV2_State_Status Plugin::restore(LV2_Handle instance, LV2_State_Retrieve_Function retrieve, LV2_State_Handle handle,
		uint32_t flags, const LV2_Feature* const* features)
	{
		auto nam = static_cast<NAM::Plugin*>(instance);

		size_t      size     = 0;
		uint32_t    type     = 0;
		uint32_t    valflags = 0;

		const LV2_URID modelPathKeys[kNumSlots] = { nam->uris.model1_Path, nam->uris.model2_Path };

		bool haveSlot[kNumSlots] = { false, false };
		const void* values[kNumSlots] = { nullptr, nullptr };
		uint32_t types[kNumSlots] = { 0, 0 };

		// Get model path for each slot. Fall back to the legacy single-model
		// state key, restored into slot 0.
		values[0] = retrieve(handle, modelPathKeys[0], &size, &types[0], &valflags);
		haveSlot[0] = values[0] != nullptr && types[0] == nam->uris.atom_Path;

		values[1] = retrieve(handle, modelPathKeys[1], &size, &types[1], &valflags);
		haveSlot[1] = values[1] != nullptr && types[1] == nam->uris.atom_Path;

		if (!haveSlot[0] && !haveSlot[1])
		{
			values[0] = retrieve(handle, nam->uris.model_Path, &size, &types[0], &valflags);
			haveSlot[0] = values[0] != nullptr && types[0] == nam->uris.atom_Path;
		}

		LV2_State_Status result = LV2_STATE_SUCCESS;

		for (uint32_t slot = 0; slot < kNumSlots && result == LV2_STATE_SUCCESS; ++slot)
		{
			if (!haveSlot[slot])
			{
				continue;
			}

			lv2_log_trace(&nam->logger, "Restoring model %u: '%s'\n", slot + 1, (const char*)values[slot]);

			NAM::LV2LoadModelMsg msg = { NAM::kWorkTypeLoad, slot, {} };

			// Check if a path is set
			if (!values[slot] || (types[slot] != nam->uris.atom_Path))
			{
				msg.path[0] = '\0';
			}
			else
			{
				LV2_State_Map_Path* map_path = (LV2_State_Map_Path*)lv2_features_data(features, LV2_STATE__mapPath);

				if (map_path == nullptr)
				{
					lv2_log_error(&nam->logger, "LV2_STATE__mapPath unsupported by host\n");

					return LV2_STATE_ERR_NO_FEATURE;
				}

				// Map abstract state path to absolute path
				char* path = map_path->absolute_path(map_path->handle, (const char *)values[slot]);

				size_t pathLen = strlen(path);

				if (pathLen >= MAX_FILE_NAME)
				{
					lv2_log_error(&nam->logger, "Model path is too long (max %u chars)\n", MAX_FILE_NAME);

					result = LV2_STATE_ERR_UNKNOWN;
				}
				else
				{
					memcpy(msg.path, path, pathLen);
				}

				LV2_State_Free_Path* free_path = (LV2_State_Free_Path *)lv2_features_data(features, LV2_STATE__freePath);

				if (free_path != nullptr)
				{
					free_path->free_path(free_path->handle, path);
				}
				else
				{
#ifndef _WIN32	// Can't free host-allocated memory on plugin side under Windows
					free(path);
#endif
				}
			}

			if (result == LV2_STATE_SUCCESS)
			{
				// Schedule model to be loaded by the provided worker
				nam->schedule->schedule_work(nam->schedule->handle, sizeof(msg), &msg);

				nam->currentModelPaths[slot] = msg.path;
			}
		}

		return result;
	}

	void Plugin::write_current_path()
	{
		LV2_Atom_Forge_Frame frame;

		lv2_atom_forge_frame_time(&atom_forge, 0);
		lv2_atom_forge_object(&atom_forge, &frame, 0, uris.patch_Set);

		lv2_atom_forge_key(&atom_forge, uris.patch_property);
		lv2_atom_forge_urid(&atom_forge, uris.model_Path);
		lv2_atom_forge_key(&atom_forge, uris.patch_value);
		lv2_atom_forge_path(&atom_forge, currentModelPath.c_str(), (uint32_t)currentModelPath.length() + 1);

		lv2_atom_forge_pop(&atom_forge, &frame);
	}
}

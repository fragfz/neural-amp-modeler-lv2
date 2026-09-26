#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <utility>
#include <cassert>

#include "nam_plugin.h"

#ifdef ENABLE_CAB
#include "cab_convolver.h"
#endif

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
		currentModelPath.reserve(MAX_FILE_NAME + 1);

		scratch.reserve(maxBufferSize);
		scratch2.reserve(maxBufferSize);

		scratch.resize(maxBufferSize);
		scratch2.resize(maxBufferSize);

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
		delete currentModel;

#ifdef ENABLE_CAB
		delete cabConvolver;
#endif
	}

	bool Plugin::initialize(double sampleRate, const LV2_Feature* const* features) noexcept
	{
		this->sampleRate = sampleRate;

		dcCoefficient = 1.0f - ((float)(2.0 * M_PI * DC_BLOCKER_HZ) / (float)sampleRate);

#ifdef ENABLE_EQ
		eq.Init((float)sampleRate);
#endif

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

#ifdef ENABLE_CAB
		uris.cab_Path = map->map(map->handle, CAB_URI);
#endif

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

				NeuralAudio::NeuralModel* model = nullptr;
				LV2SwitchModelMsg response = { kWorkTypeSwitch, {}, {} };
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

#ifdef ENABLE_CAB
			case kWorkTypeLoadCab:
			{
				auto msg = static_cast<const LV2LoadCabMsg*>(data);
				auto nam = static_cast<NAM::Plugin*>(instance);

				LV2SwitchCabMsg response = { kWorkTypeSwitchCab, {}, nullptr };

				if (msg->path[0] != 0)
				{
					std::vector<float> ir;

					if (LoadCabIr(msg->path, nam->sampleRate, ir) && !ir.empty())
					{
						response.convolver = CabConvolver::Create(ir);

						if (response.convolver != nullptr)
							memcpy(response.path, msg->path, strlen(msg->path));
					}

					if (response.convolver == nullptr)
						lv2_log_error(&nam->logger, "Unable to load cab IR from: '%s'\n", msg->path);
				}

				respond(handle, sizeof(response), &response);

				return LV2_WORKER_SUCCESS;
			}

			case kWorkTypeFreeCab:
			{
				auto msg = static_cast<const LV2FreeCabMsg*>(data);
				delete msg->convolver;

				return LV2_WORKER_SUCCESS;
			}
#endif

			case kWorkTypeSwitch:
			case kWorkTypeSwitchCab:
				// should not happen!
				break;
		}

		return LV2_WORKER_ERR_UNKNOWN;
	}

	// runs on RT, right after process(), must not block or [de]allocate memory
	LV2_Worker_Status Plugin::work_response(LV2_Handle instance, uint32_t size,	const void* data)
	{
		switch (*(const LV2WorkType*)data)
		{
#ifdef ENABLE_CAB
			case kWorkTypeSwitchCab:
			{
				auto cabMsg = static_cast<const LV2SwitchCabMsg*>(data);
				auto namCab = static_cast<NAM::Plugin*>(instance);

				// prepare reply for deleting the old convolver
				LV2FreeCabMsg reply = { kWorkTypeFreeCab, namCab->cabConvolver };

				// swap current convolver with the new one
				namCab->cabConvolver = cabMsg->convolver;
				namCab->cabPath = cabMsg->path;

				namCab->schedule->schedule_work(namCab->schedule->handle, sizeof(reply), &reply);

				// report change to host/ui
				namCab->write_cab_path();

				return LV2_WORKER_SUCCESS;
			}
#endif

			case kWorkTypeSwitch:
				break;

			default:
				return LV2_WORKER_ERR_UNKNOWN;
		}

		auto msg = static_cast<const LV2SwitchModelMsg*>(data);
		auto nam = static_cast<NAM::Plugin*>(instance);

		// prepare reply for deleting old model
		LV2FreeModelMsg reply = { kWorkTypeFree, nam->currentModel };

		// swap current model with new one
		nam->currentModel = msg->model;
		nam->currentModelPath = msg->path;
		assert(nam->currentModelPath.capacity() >= MAX_FILE_NAME + 1);

		if (nam->currentModel != nullptr)
		{
			int receptiveFieldSize = nam->currentModel->GetReceptiveFieldSize();

			if (receptiveFieldSize > -1)
			{
				// A newly loaded model is prewarmed to have a silent sample history
				nam->silentSamples = receptiveFieldSize;
				nam->smartBypassed = true;
			}
		}

		// send reply
		nam->schedule->schedule_work(nam->schedule->handle, sizeof(reply), &reply);

		// report change to host/ui
		nam->write_current_path();

		return LV2_WORKER_SUCCESS;
	}

	void Plugin::set_max_buffer_size(int size) noexcept
	{
		maxBufferSize = size;

		loader.SetDefaultMaxAudioBufferSize(size);

		// grow (never shrink) the staging buffers so process() never allocates
		if ((int)scratch.size() < size)
		{
			scratch.resize(size);
			scratch2.resize(size);
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
					write_current_path();
#ifdef ENABLE_CAB
					write_cab_path();
#endif
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
						if (((const LV2_Atom_URID*)property)->body == uris.model_Path)
						{
							LV2LoadModelMsg msg = { kWorkTypeLoad, {} };
							memcpy(msg.path, file_path + 1, file_path->size);
							schedule->schedule_work(schedule->handle, sizeof(msg), &msg);
						}
#ifdef ENABLE_CAB
						else if (((const LV2_Atom_URID*)property)->body == uris.cab_Path)
						{
							LV2LoadCabMsg msg = { kWorkTypeLoadCab, {} };
							memcpy(msg.path, file_path + 1, file_path->size);
							schedule->schedule_work(schedule->handle, sizeof(msg), &msg);
						}
#endif
					}
				}
			}
		}

		float level;

		float modelInputAdjustmentDB = 0;

		if (currentModel != nullptr)
		{
			if (*(ports.quality_scale) != currentModel->GetQualityScaleFactor())
			{
				currentModel->SetQualityScaleFactor(*(ports.quality_scale));
			}

			modelInputAdjustmentDB = currentModel->GetRecommendedInputDBAdjustment();

#ifdef SMART_BYPASS_ENABLED
			int receptiveFieldSamples = currentModel->GetReceptiveFieldSize();

			if (receptiveFieldSamples > -1)
			{
				for (unsigned int i = 0; i < n_samples; i++)
				{
					if (abs(ports.audio_in[i]) <= bypassThresholdLinear)
					{
						silentSamples++;
					}
					else
					{
						silentSamples = 0;
					}
				}

				if (silentSamples >= (uint32_t)receptiveFieldSamples)
				{
					silentSamples = (uint32_t)receptiveFieldSamples;	// Prevent silentSamples growing and eventually overflowing uint32

					if (smartBypassed)
					{
						for (unsigned int i = 0; i < n_samples; i++)
						{
							ports.audio_out[i] = ports.audio_in[i];
						}

						return;
					}

					smartBypassed = true; // If we aren't already, we'll be bypassed on the next process call
				}
				else
					smartBypassed = false;
			}
#endif
		}

		// convert input level from db
		float desiredInputLevel = powf(10, (*(ports.input_level) + modelInputAdjustmentDB) * 0.05f);

		if (fabs(desiredInputLevel - inputLevel) > SMOOTH_EPSILON)
		{
			level = inputLevel;
			for (unsigned int i = 0; i < n_samples; i++)
			{
				// do very basic smoothing
				level = (.99f * level) + (.01f * desiredInputLevel);

				ports.audio_out[i] = ports.audio_in[i] * level;
			}

			inputLevel = level;
		}
		else
		{
			level = inputLevel = desiredInputLevel;

			for (unsigned int i = 0; i < n_samples; i++)
			{
				ports.audio_out[i] = ports.audio_in[i] * level;
			}
		}

		float modelLoudnessAdjustmentDB = 0;

		if (currentModel != nullptr)
		{
			currentModel->Process(ports.audio_out, ports.audio_out, n_samples);

			modelLoudnessAdjustmentDB = currentModel->GetRecommendedOutputDBAdjustment();

			if (*(ports.out_calibrated) > 0.5f)
			{
				// "Out Calibrated" on: use the model's output_level_dbu calibration
				// (true analog capture output level) instead of loudness normalization,
				// like the tone3000 / official NAM plugin "Calibrated" output mode:
				// adjustment = model_output_level_dBu - interface_dBu.
				const std::string modelOutputLevelDbu = currentModel->GetMetadata("output_level_dbu");

				if (!modelOutputLevelDbu.empty())
				{
					char* parseEnd = nullptr;

					const float outputLevelDbu = strtof(modelOutputLevelDbu.c_str(), &parseEnd);

					if (parseEnd != nullptr && *parseEnd == '\0' && outputLevelDbu > -60.0f && outputLevelDbu < 60.0f)
					{
						modelLoudnessAdjustmentDB = outputLevelDbu - currentModel->GetAudioInputLevelDBu();
					}
				}
			}
		}

		// Convert output level from db
		float desiredOutputLevel = powf(10, (*(ports.output_level) + modelLoudnessAdjustmentDB) * 0.05f);

		if (fabs(desiredOutputLevel - outputLevel) > SMOOTH_EPSILON)
		{
			level = outputLevel;

			for (unsigned int i = 0; i < n_samples; i++)
			{
				// do very basic smoothing
				level = (.99f * level) + (.01f * desiredOutputLevel);

				ports.audio_out[i] = ports.audio_out[i] * outputLevel;
			}

			outputLevel = level;
		}
		else
		{
			level = outputLevel = desiredOutputLevel;

			for (unsigned int i = 0; i < n_samples; i++)
			{
				ports.audio_out[i] = ports.audio_out[i] * level;
			}
		}

#ifdef ENABLE_CAB
		// --- Cab IR (after the NAM output level, before the DC blocker / EQ) ---

		if (cabConvolver != nullptr)
		{
			if (*(ports.cab_enable) > 0.5f)
			{
				// process through a staging buffer (the convolver must not run in place)
				memcpy(scratch.data(), ports.audio_out, n_samples * sizeof(float));

				cabConvolver->Process(scratch.data(), ports.audio_out, n_samples);
			}
			else
			{
				// cab disabled: audio passes through untouched, but keep the CPU
				// load steady by still running the convolver on a scratch copy
				// and discarding the result
				memcpy(scratch.data(), ports.audio_out, n_samples * sizeof(float));

				cabConvolver->Process(scratch.data(), scratch2.data(), n_samples);
			}
		}
#endif

#ifdef ENABLE_DC_BLOCK
		// --- Global DC blocker (end of chain, before the EQ) ---

		for (unsigned int i = 0; i < n_samples; i++)
		{
			const float dcInput = ports.audio_out[i];

			ports.audio_out[i] = dcInput - prevDCInput + dcCoefficient * prevDCOutput;

			prevDCInput = dcInput;
			prevDCOutput = ports.audio_out[i];
		}
#endif

#ifdef ENABLE_EQ
		// --- Global 3-band EQ (after the DC blocker) ---

		if (*(ports.eq_bass) != lastEqBass)
		{
			eq.SetBass(*(ports.eq_bass));
			lastEqBass = *(ports.eq_bass);
		}

		if (*(ports.eq_mid) != lastEqMid)
		{
			eq.SetMid(*(ports.eq_mid));
			lastEqMid = *(ports.eq_mid);
		}

		if (*(ports.eq_treble) != lastEqTreble)
		{
			eq.SetTreble(*(ports.eq_treble));
			lastEqTreble = *(ports.eq_treble);
		}

		for (unsigned int i = 0; i < n_samples; i++)
		{
			ports.audio_out[i] = eq.Process(ports.audio_out[i]);
		}
#endif
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

#ifdef ENABLE_CAB
		if (!nam->currentModel && !nam->cabConvolver)
#else
		if (!nam->currentModel)
#endif
		{
			return LV2_STATE_SUCCESS;
		}

		LV2_State_Map_Path* map_path = (LV2_State_Map_Path*)lv2_features_data(features, LV2_STATE__mapPath);

		if (map_path == nullptr)
		{
			lv2_log_error(&nam->logger, "LV2_STATE__mapPath unsupported by host\n");

			return LV2_STATE_ERR_NO_FEATURE;
		}

		// Map absolute sample path to an abstract state path
		char* apath = map_path->abstract_path(map_path->handle, nam->currentModelPath.c_str());

		store(handle, nam->uris.model_Path, apath, strlen(apath) + 1, nam->uris.atom_Path,
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

#ifdef ENABLE_CAB
		if (nam->cabConvolver != nullptr && !nam->cabPath.empty())
		{
			// Map absolute cab IR path to an abstract state path
			char* apath = map_path->abstract_path(map_path->handle, nam->cabPath.c_str());

			store(handle, nam->uris.cab_Path, apath, strlen(apath) + 1, nam->uris.atom_Path,
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
#endif

		return LV2_STATE_SUCCESS;
	}

	LV2_State_Status Plugin::restore(LV2_Handle instance, LV2_State_Retrieve_Function retrieve, LV2_State_Handle handle, 
		uint32_t flags, const LV2_Feature* const* features)
	{
		auto nam = static_cast<NAM::Plugin*>(instance);

		// Get model_Path from state
		size_t      size     = 0;
		uint32_t    type     = 0;
		uint32_t    valflags = 0;
		const void* value = retrieve(handle, nam->uris.model_Path, &size, &type, &valflags);

		lv2_log_trace(&nam->logger, "Restoring model '%s'\n", (const char*)value);

		NAM::LV2LoadModelMsg msg = { NAM::kWorkTypeLoad, {} };

		LV2_State_Status result = LV2_STATE_SUCCESS;

		// Check if a path is set
		if (!value || (type != nam->uris.atom_Path))
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
			char* path = map_path->absolute_path(map_path->handle, (const char *)value);

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

			nam->currentModelPath = msg.path;
		}

#ifdef ENABLE_CAB
		const void* cabValue = retrieve(handle, nam->uris.cab_Path, &size, &type, &valflags);

		if (cabValue != nullptr && type == nam->uris.atom_Path)
		{
			LV2_State_Map_Path* map_path = (LV2_State_Map_Path*)lv2_features_data(features, LV2_STATE__mapPath);

			if (map_path == nullptr)
			{
				lv2_log_error(&nam->logger, "LV2_STATE__mapPath unsupported by host\n");

				return LV2_STATE_ERR_NO_FEATURE;
			}

			// Map abstract state path to absolute path
			char* path = map_path->absolute_path(map_path->handle, (const char *)cabValue);

			LV2LoadCabMsg msg = { kWorkTypeLoadCab, {} };

			size_t pathLen = strlen(path);

			if (pathLen >= MAX_FILE_NAME)
			{
				lv2_log_error(&nam->logger, "Cab IR path is too long (max %u chars)\n", MAX_FILE_NAME);
				result = LV2_STATE_ERR_UNKNOWN;
			}
			else
			{
				memcpy(msg.path, path, pathLen);

				// Schedule cab IR to be loaded by the provided worker
				nam->schedule->schedule_work(nam->schedule->handle, sizeof(msg), &msg);

				nam->cabPath = msg.path;
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
#endif

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

#ifdef ENABLE_CAB
	void Plugin::write_cab_path()
	{
		LV2_Atom_Forge_Frame frame;

		lv2_atom_forge_frame_time(&atom_forge, 0);
		lv2_atom_forge_object(&atom_forge, &frame, 0, uris.patch_Set);

		lv2_atom_forge_key(&atom_forge, uris.patch_property);
		lv2_atom_forge_urid(&atom_forge, uris.cab_Path);
		lv2_atom_forge_key(&atom_forge, uris.patch_value);
		lv2_atom_forge_path(&atom_forge, cabPath.c_str(), (uint32_t)cabPath.length() + 1);

		lv2_atom_forge_pop(&atom_forge, &frame);
	}
#endif
}

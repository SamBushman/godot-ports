/**************************************************************************/
/*  audio_driver_dsound.cpp                                               */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#ifdef DSOUND_ENABLED

#include "audio_driver_dsound.h"

#include "core/os/os.h"
#include "core/project_settings.h"

#define SAFE_RELEASE(p) \
	if (p) {            \
		(p)->Release(); \
		(p) = NULL;     \
	}

BOOL CALLBACK AudioDriverDSound::dsound_enum_callback(LPGUID lpGuid, LPCSTR lpcstrDescription, LPCSTR lpcstrModule, LPVOID lpContext) {
	Vector<AudioDeviceDSound> *devices = (Vector<AudioDeviceDSound> *)lpContext;
	AudioDeviceDSound device;

	if (lpGuid == NULL) {
		device.name = "Default";
		device.is_default = true;
	} else {
		device.guid = *lpGuid;
		device.name = String(lpcstrDescription);
		device.is_default = false;
	}
	
	if (lpcstrDescription) {
		device.description = String(lpcstrDescription);
	}

	devices->push_back(device);
	return TRUE;
}

BOOL CALLBACK AudioDriverDSound::dsound_capture_enum_callback(LPGUID lpGuid, LPCSTR lpcstrDescription, LPCSTR lpcstrModule, LPVOID lpContext) {
	Vector<AudioDeviceDSound> *devices = (Vector<AudioDeviceDSound> *)lpContext;
	AudioDeviceDSound device;

	if (lpGuid == NULL) {
		device.name = "Default";
		device.is_default = true;
	} else {
		device.guid = *lpGuid;
		device.name = String(lpcstrDescription);
		device.is_default = false;
	}
	
	if (lpcstrDescription) {
		device.description = String(lpcstrDescription);
	}

	devices->push_back(device);
	return TRUE;
}

void AudioDriverDSound::enumerate_output_devices() {
	output_devices.clear();
	DirectSoundEnumerate(dsound_enum_callback, &output_devices);
}

void AudioDriverDSound::enumerate_input_devices() {
	input_devices.clear();
	DirectSoundCaptureEnumerate(dsound_capture_enum_callback, &input_devices);
}

GUID *AudioDriverDSound::find_output_device_guid(const String &name) {
	for (int i = 0; i < output_devices.size(); i++) {
		if (output_devices[i].name == name) {
			return output_devices[i].is_default ? NULL : const_cast<GUID*>(&output_devices[i].guid);
		}
	}
	return NULL;
}

GUID *AudioDriverDSound::find_input_device_guid(const String &name) {
	for (int i = 0; i < input_devices.size(); i++) {
		if (input_devices[i].name == name) {
			return input_devices[i].is_default ? NULL : const_cast<GUID*>(&input_devices[i].guid);
		}
	}
	return NULL;
}

Error AudioDriverDSound::init_output_device(bool reinit) {
	HRESULT hr;
	HWND hwnd = GetDesktopWindow();

	enumerate_output_devices();
	GUID *device_guid = find_output_device_guid(audio_output.device_name);

	hr = DirectSoundCreate(device_guid, &audio_output.ds, NULL);
	if (FAILED(hr)) {
		ERR_PRINT("DirectSound: DirectSoundCreate failed");
		return ERR_CANT_OPEN;
	}

	hr = audio_output.ds->SetCooperativeLevel(hwnd, DSSCL_PRIORITY);
	if (FAILED(hr)) {
		ERR_PRINT("DirectSound: SetCooperativeLevel failed");
		return ERR_CANT_OPEN;
	}

	// Set up wave format
	audio_output.wave_format.wFormatTag = WAVE_FORMAT_PCM;
	audio_output.wave_format.nChannels = channels;
	audio_output.wave_format.nSamplesPerSec = mix_rate;
	audio_output.wave_format.wBitsPerSample = 16;
	audio_output.wave_format.nBlockAlign = (audio_output.wave_format.wBitsPerSample / 8) * audio_output.wave_format.nChannels;
	audio_output.wave_format.nAvgBytesPerSec = audio_output.wave_format.nSamplesPerSec * audio_output.wave_format.nBlockAlign;
	audio_output.wave_format.cbSize = 0;

	// Create primary buffer
	DSBUFFERDESC dsbd;
	ZeroMemory(&dsbd, sizeof(DSBUFFERDESC));
	dsbd.dwSize = sizeof(DSBUFFERDESC);
	dsbd.dwFlags = DSBCAPS_PRIMARYBUFFER;
	dsbd.dwBufferBytes = 0;
	dsbd.lpwfxFormat = NULL;

	hr = audio_output.ds->CreateSoundBuffer(&dsbd, &audio_output.primary_buffer, NULL);
	if (FAILED(hr)) {
		ERR_PRINT("DirectSound: CreateSoundBuffer (primary) failed");
		return ERR_CANT_OPEN;
	}

	hr = audio_output.primary_buffer->SetFormat(&audio_output.wave_format);
	if (FAILED(hr)) {
		ERR_PRINT("DirectSound: SetFormat failed");
	}

	// Create secondary buffer
	buffer_frames = mix_rate * latency_ms / 1000;
	audio_output.buffer_size = buffer_frames * audio_output.wave_format.nBlockAlign;

	ZeroMemory(&dsbd, sizeof(DSBUFFERDESC));
	dsbd.dwSize = sizeof(DSBUFFERDESC);
	dsbd.dwFlags = DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_GLOBALFOCUS;
	dsbd.dwBufferBytes = audio_output.buffer_size;
	dsbd.lpwfxFormat = &audio_output.wave_format;

	hr = audio_output.ds->CreateSoundBuffer(&dsbd, &audio_output.secondary_buffer, NULL);
	if (FAILED(hr)) {
		ERR_PRINT("DirectSound: CreateSoundBuffer (secondary) failed");
		return ERR_CANT_OPEN;
	}

	// Clear the buffer
	LPVOID ptr1, ptr2;
	DWORD bytes1, bytes2;
	hr = audio_output.secondary_buffer->Lock(0, audio_output.buffer_size, &ptr1, &bytes1, &ptr2, &bytes2, DSBLOCK_ENTIREBUFFER);
	if (SUCCEEDED(hr)) {
		ZeroMemory(ptr1, bytes1);
		if (ptr2) {
			ZeroMemory(ptr2, bytes2);
		}
		audio_output.secondary_buffer->Unlock(ptr1, bytes1, ptr2, bytes2);
	}

	audio_output.write_pos = 0;

	samples_in.resize(buffer_frames * channels);

	print_verbose("DirectSound: detected " + itos(channels) + " channels");
	print_verbose("DirectSound: audio buffer frames: " + itos(buffer_frames) + " calculated latency: " + itos(latency_ms) + "ms");

	return OK;
}

Error AudioDriverDSound::init_input_device(bool reinit) {
	HRESULT hr;

	enumerate_input_devices();
	GUID *device_guid = find_input_device_guid(audio_input.device_name);

	hr = DirectSoundCaptureCreate(device_guid, &audio_input.dsc, NULL);
	if (FAILED(hr)) {
		ERR_PRINT("DirectSound: DirectSoundCaptureCreate failed");
		return ERR_CANT_OPEN;
	}

	// Set up wave format for capture (always stereo for input)
	audio_input.wave_format.wFormatTag = WAVE_FORMAT_PCM;
	audio_input.wave_format.nChannels = 2;
	audio_input.wave_format.nSamplesPerSec = mix_rate;
	audio_input.wave_format.wBitsPerSample = 16;
	audio_input.wave_format.nBlockAlign = (audio_input.wave_format.wBitsPerSample / 8) * audio_input.wave_format.nChannels;
	audio_input.wave_format.nAvgBytesPerSec = audio_input.wave_format.nSamplesPerSec * audio_input.wave_format.nBlockAlign;
	audio_input.wave_format.cbSize = 0;

	// Create capture buffer
	audio_input.buffer_size = mix_rate * latency_ms / 1000 * audio_input.wave_format.nBlockAlign;

	DSCBUFFERDESC dscbd;
	ZeroMemory(&dscbd, sizeof(DSCBUFFERDESC));
	dscbd.dwSize = sizeof(DSCBUFFERDESC);
	dscbd.dwFlags = 0;
	dscbd.dwBufferBytes = audio_input.buffer_size;
	dscbd.lpwfxFormat = &audio_input.wave_format;

	hr = audio_input.dsc->CreateCaptureBuffer(&dscbd, &audio_input.capture_buffer, NULL);
	if (FAILED(hr)) {
		ERR_PRINT("DirectSound: CreateCaptureBuffer failed");
		return ERR_CANT_OPEN;
	}

	audio_input.read_pos = 0;

	input_buffer_init(mix_rate * latency_ms / 1000);

	return OK;
}

Error AudioDriverDSound::finish_output_device() {
	if (audio_output.active.is_set()) {
		if (audio_output.secondary_buffer) {
			audio_output.secondary_buffer->Stop();
		}
		audio_output.active.clear();
	}

	SAFE_RELEASE(audio_output.secondary_buffer);
	SAFE_RELEASE(audio_output.primary_buffer);
	SAFE_RELEASE(audio_output.ds);

	return OK;
}

Error AudioDriverDSound::finish_input_device() {
	if (audio_input.active.is_set()) {
		if (audio_input.capture_buffer) {
			audio_input.capture_buffer->Stop();
		}
		audio_input.active.clear();
	}

	SAFE_RELEASE(audio_input.capture_buffer);
	SAFE_RELEASE(audio_input.dsc);

	return OK;
}

void AudioDriverDSound::write_sample(const WAVEFORMATEX *format, BYTE *buffer, int i, int32_t sample) {
	if (format->wBitsPerSample == 16) {
		((int16_t *)buffer)[i] = sample >> 16;
	} else if (format->wBitsPerSample == 8) {
		((int8_t *)buffer)[i] = sample >> 24;
	} else if (format->wBitsPerSample == 32) {
		((int32_t *)buffer)[i] = sample;
	}
}

int32_t AudioDriverDSound::read_sample(const WAVEFORMATEX *format, BYTE *buffer, int i) {
	if (format->wBitsPerSample == 16) {
		return int32_t(((int16_t *)buffer)[i]) << 16;
	} else if (format->wBitsPerSample == 8) {
		return int32_t(((int8_t *)buffer)[i]) << 24;
	} else if (format->wBitsPerSample == 32) {
		return ((int32_t *)buffer)[i];
	}
	return 0;
}

void AudioDriverDSound::thread_func(void *p_udata) {
	AudioDriverDSound *ad = (AudioDriverDSound *)p_udata;

	while (!ad->exit_thread.is_set()) {
		ad->lock();
		ad->start_counting_ticks();

		// Handle output
		if (ad->audio_output.active.is_set() && ad->audio_output.secondary_buffer) {
			DWORD play_pos, write_cursor;
			HRESULT hr = ad->audio_output.secondary_buffer->GetCurrentPosition(&play_pos, &write_cursor);
			
			if (SUCCEEDED(hr)) {
				// Calculate how much data we can safely write
				DWORD bytes_free;
				if (ad->audio_output.write_pos <= play_pos) {
					bytes_free = play_pos - ad->audio_output.write_pos;
				} else {
					bytes_free = (ad->audio_output.buffer_size - ad->audio_output.write_pos) + play_pos;
				}
				
				// Convert to frames
				DWORD frames_free = bytes_free / ad->audio_output.wave_format.nBlockAlign;
				
				// Only write if we have a reasonable amount of free space
				// and don't write more than our processing buffer size
				if (frames_free >= (DWORD)(ad->buffer_frames / 2)) {
					DWORD frames_to_write = MIN(frames_free - (ad->buffer_frames / 4), (DWORD)ad->buffer_frames);
					DWORD bytes_to_write = frames_to_write * ad->audio_output.wave_format.nBlockAlign;
					
					// Make sure we don't exceed buffer boundaries
					if (ad->audio_output.write_pos + bytes_to_write > ad->audio_output.buffer_size) {
						bytes_to_write = ad->audio_output.buffer_size - ad->audio_output.write_pos;
						frames_to_write = bytes_to_write / ad->audio_output.wave_format.nBlockAlign;
					}
					
					if (frames_to_write > 0) {
						// Process audio - make sure samples_in is large enough
						if (ad->samples_in.size() < (int)(frames_to_write * ad->channels)) {
							ad->samples_in.resize(frames_to_write * ad->channels);
						}
						
						ad->audio_server_process(frames_to_write, ad->samples_in.ptrw());

						LPVOID ptr1, ptr2;
						DWORD bytes1, bytes2;

						hr = ad->audio_output.secondary_buffer->Lock(ad->audio_output.write_pos, bytes_to_write, 
																   &ptr1, &bytes1, &ptr2, &bytes2, 0);
						if (SUCCEEDED(hr)) {
							// Write samples to first buffer segment
							DWORD samples_written = 0;
							DWORD samples1 = bytes1 / (ad->audio_output.wave_format.wBitsPerSample / 8);
							
							for (DWORD i = 0; i < samples1 && samples_written < ad->samples_in.size(); i++) {
								ad->write_sample(&ad->audio_output.wave_format, (BYTE *)ptr1, i, ad->samples_in[samples_written]);
								samples_written++;
							}

							// Write samples to second buffer segment if it exists
							if (ptr2 && bytes2 > 0) {
								DWORD samples2 = bytes2 / (ad->audio_output.wave_format.wBitsPerSample / 8);
								for (DWORD i = 0; i < samples2 && samples_written < ad->samples_in.size(); i++) {
									ad->write_sample(&ad->audio_output.wave_format, (BYTE *)ptr2, i, ad->samples_in[samples_written]);
									samples_written++;
								}
							}

							ad->audio_output.secondary_buffer->Unlock(ptr1, bytes1, ptr2, bytes2);
							
							// Update write position with proper wraparound
							ad->audio_output.write_pos = (ad->audio_output.write_pos + bytes_to_write) % ad->audio_output.buffer_size;
						}
					}
				}
			}
		}

		// Handle input
		if (ad->audio_input.active.is_set() && ad->audio_input.capture_buffer) {
			DWORD capture_pos, read_cursor;
			HRESULT hr = ad->audio_input.capture_buffer->GetCurrentPosition(&capture_pos, &read_cursor);
			
			if (SUCCEEDED(hr)) {
				// Calculate how much new data is available to read
				DWORD bytes_available;
				if (capture_pos >= ad->audio_input.read_pos) {
					bytes_available = capture_pos - ad->audio_input.read_pos;
				} else {
					bytes_available = (ad->audio_input.buffer_size - ad->audio_input.read_pos) + capture_pos;
				}

				// Only read if we have a reasonable amount of data
				DWORD min_bytes = ad->audio_input.wave_format.nBlockAlign * 64; // At least 64 samples
				
				if (bytes_available >= min_bytes) {
					// Don't read more than what's available or what fits in one chunk
					DWORD bytes_to_read = bytes_available;
					
					// Make sure we don't exceed buffer boundaries in one read
					if (ad->audio_input.read_pos + bytes_to_read > ad->audio_input.buffer_size) {
						bytes_to_read = ad->audio_input.buffer_size - ad->audio_input.read_pos;
					}

					if (bytes_to_read > 0) {
						LPVOID ptr1, ptr2;
						DWORD bytes1, bytes2;

						hr = ad->audio_input.capture_buffer->Lock(ad->audio_input.read_pos, bytes_to_read, 
																&ptr1, &bytes1, &ptr2, &bytes2, 0);
						if (SUCCEEDED(hr)) {
							// Read first buffer segment
							DWORD samples1 = bytes1 / (ad->audio_input.wave_format.wBitsPerSample / 8);
							for (DWORD i = 0; i < samples1; i += 2) {
								if (i + 1 < samples1) { // Make sure we have both L and R samples
									int32_t l = ad->read_sample(&ad->audio_input.wave_format, (BYTE *)ptr1, i);
									int32_t r = ad->read_sample(&ad->audio_input.wave_format, (BYTE *)ptr1, i + 1);
									ad->input_buffer_write(l);
									ad->input_buffer_write(r);
								}
							}

							// Read second buffer segment if it exists
							if (ptr2 && bytes2 > 0) {
								DWORD samples2 = bytes2 / (ad->audio_input.wave_format.wBitsPerSample / 8);
								for (DWORD i = 0; i < samples2; i += 2) {
									if (i + 1 < samples2) { // Make sure we have both L and R samples
										int32_t l = ad->read_sample(&ad->audio_input.wave_format, (BYTE *)ptr2, i);
										int32_t r = ad->read_sample(&ad->audio_input.wave_format, (BYTE *)ptr2, i + 1);
										ad->input_buffer_write(l);
										ad->input_buffer_write(r);
									}
								}
							}

							ad->audio_input.capture_buffer->Unlock(ptr1, bytes1, ptr2, bytes2);
							
							// Update read position with proper wraparound
							ad->audio_input.read_pos = (ad->audio_input.read_pos + bytes_to_read) % ad->audio_input.buffer_size;
						}
					}
				}
			}
		}

		// Check for device changes (simple polling approach)
		if (ad->audio_output.device_name != ad->audio_output.new_device) {
			ad->audio_output.device_name = ad->audio_output.new_device;
			Error err = ad->finish_output_device();
			if (err != OK) {
				ERR_PRINT("DirectSound: finish_output_device error");
			}
		}

		if (ad->audio_input.device_name != ad->audio_input.new_device) {
			ad->audio_input.device_name = ad->audio_input.new_device;
			Error err = ad->finish_input_device();
			if (err != OK) {
				ERR_PRINT("DirectSound: finish_input_device error");
			}
		}

		// Reinitialize devices if needed
		if (!ad->audio_output.ds) {
			Error err = ad->init_output_device(true);
			if (err == OK) {
				ad->start();
			}
		}

		if (ad->audio_input.active.is_set() && !ad->audio_input.dsc) {
			Error err = ad->init_input_device(true);
			if (err == OK) {
				ad->capture_start();
			}
		}

		ad->stop_counting_ticks();
		ad->unlock();

		OS::get_singleton()->delay_usec(5000);
	}
}

Error AudioDriverDSound::init() {
	mix_rate = GLOBAL_GET("audio/mix_rate");
	channels = 2;
	latency_ms = 100; // 100 works

	exit_thread.clear();

	Error err = init_output_device();
	ERR_FAIL_COND_V_MSG(err != OK, err, "DirectSound: init_output_device error.");

	thread.start(thread_func, this);

	return OK;
}

void AudioDriverDSound::start() {
	if (audio_output.secondary_buffer) {
		HRESULT hr = audio_output.secondary_buffer->Play(0, 0, DSBPLAY_LOOPING);
		if (SUCCEEDED(hr)) {
			audio_output.active.set();
		} else {
			ERR_PRINT("DirectSound: Play failed");
		}
	}
}

int AudioDriverDSound::get_mix_rate() const {
	return mix_rate;
}

AudioDriver::SpeakerMode AudioDriverDSound::get_speaker_mode() const {
	return get_speaker_mode_by_total_channels(channels);
}

Array AudioDriverDSound::get_device_list() {
	Array list;
	enumerate_output_devices();
	
	for (int i = 0; i < output_devices.size(); i++) {
		list.push_back(output_devices[i].name);
	}
	
	return list;
}

String AudioDriverDSound::get_device() {
	lock();
	String name = audio_output.device_name;
	unlock();
	return name;
}

void AudioDriverDSound::set_device(String device) {
	lock();
	audio_output.new_device = device;
	unlock();
}

void AudioDriverDSound::lock() {
	mutex.lock();
}

void AudioDriverDSound::unlock() {
	mutex.unlock();
}

void AudioDriverDSound::finish() {
	exit_thread.set();
	thread.wait_to_finish();

	finish_input_device();
	finish_output_device();
}

Error AudioDriverDSound::capture_start() {
	Error err = init_input_device();
	if (err != OK) {
		ERR_PRINT("DirectSound: init_input_device error");
		return err;
	}

	if (audio_input.active.is_set()) {
		return FAILED;
	}

	HRESULT hr = audio_input.capture_buffer->Start(DSCBSTART_LOOPING);
	if (SUCCEEDED(hr)) {
		audio_input.active.set();
		return OK;
	}

	return FAILED;
}

Error AudioDriverDSound::capture_stop() {
	if (audio_input.active.is_set()) {
		audio_input.capture_buffer->Stop();
		audio_input.active.clear();
		return OK;
	}
	return FAILED;
}

void AudioDriverDSound::capture_set_device(const String &p_name) {
	lock();
	audio_input.new_device = p_name;
	unlock();
}

Array AudioDriverDSound::capture_get_device_list() {
	Array list;
	enumerate_input_devices();
	
	for (int i = 0; i < input_devices.size(); i++) {
		list.push_back(input_devices[i].name);
	}
	
	return list;
}

String AudioDriverDSound::capture_get_device() {
	lock();
	String name = audio_input.device_name;
	unlock();
	return name;
}

AudioDriverDSound::AudioDriverDSound() {
	samples_in.clear();
	channels = 0;
	mix_rate = 0;
	buffer_frames = 0;
	latency_ms = 100;
}

#endif // DSOUND_ENABLED
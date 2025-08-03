/**************************************************************************/
/*  audio_driver_dsound.h                                                 */
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

#ifndef AUDIO_DRIVER_DSOUND_H
#define AUDIO_DRIVER_DSOUND_H

#ifdef DSOUND_ENABLED

#include "core/os/mutex.h"
#include "core/os/thread.h"
#include "core/safe_refcount.h"
#include "servers/audio_server.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <dsound.h>

class AudioDriverDSound : public AudioDriver {
	struct AudioDeviceDSound {
		GUID guid;
		String name;
		String description;
		bool is_default;

		AudioDeviceDSound() :
				is_default(false) {
			memset(&guid, 0, sizeof(GUID));
		}
	};

	class AudioOutputDSound {
	public:
		LPDIRECTSOUND ds;
		LPDIRECTSOUNDBUFFER primary_buffer;
		LPDIRECTSOUNDBUFFER secondary_buffer;
		SafeFlag active;

		WAVEFORMATEX wave_format;
		DWORD buffer_size;
		DWORD write_pos;
		
		String device_name;
		String new_device;

		AudioOutputDSound() :
				ds(NULL),
				primary_buffer(NULL),
				secondary_buffer(NULL),
				buffer_size(0),
				write_pos(0),
				device_name("Default"),
				new_device("Default") {
			memset(&wave_format, 0, sizeof(WAVEFORMATEX));
		}
	};

	class AudioInputDSound {
	public:
		LPDIRECTSOUNDCAPTURE dsc;
		LPDIRECTSOUNDCAPTUREBUFFER capture_buffer;
		SafeFlag active;

		WAVEFORMATEX wave_format;
		DWORD buffer_size;
		DWORD read_pos;
		
		String device_name;
		String new_device;

		AudioInputDSound() :
				dsc(NULL),
				capture_buffer(NULL),
				buffer_size(0),
				read_pos(0),
				device_name("Default"),
				new_device("Default") {
			memset(&wave_format, 0, sizeof(WAVEFORMATEX));
		}
	};

	AudioOutputDSound audio_output;
	AudioInputDSound audio_input;

	Mutex mutex;
	Thread thread;

	Vector<int32_t> samples_in;
	Vector<AudioDeviceDSound> output_devices;
	Vector<AudioDeviceDSound> input_devices;

	unsigned int channels;
	int mix_rate;
	int buffer_frames;
	int latency_ms;

	SafeFlag exit_thread;

	static BOOL CALLBACK dsound_enum_callback(LPGUID lpGuid, LPCSTR lpcstrDescription, LPCSTR lpcstrModule, LPVOID lpContext);
	static BOOL CALLBACK dsound_capture_enum_callback(LPGUID lpGuid, LPCSTR lpcstrDescription, LPCSTR lpcstrModule, LPVOID lpContext);
	static void thread_func(void *p_udata);

	Error init_output_device(bool reinit = false);
	Error init_input_device(bool reinit = false);

	Error finish_output_device();
	Error finish_input_device();

	void enumerate_output_devices();
	void enumerate_input_devices();
	GUID *find_output_device_guid(const String &name);
	GUID *find_input_device_guid(const String &name);

	static _FORCE_INLINE_ void write_sample(const WAVEFORMATEX *format, BYTE *buffer, int i, int32_t sample);
	static _FORCE_INLINE_ int32_t read_sample(const WAVEFORMATEX *format, BYTE *buffer, int i);

public:
	virtual const char *get_name() const {
		return "DirectSound";
	}

	virtual Error init();
	virtual void start();
	virtual int get_mix_rate() const;
	virtual SpeakerMode get_speaker_mode() const;
	virtual Array get_device_list();
	virtual String get_device();
	virtual void set_device(String device);
	virtual void lock();
	virtual void unlock();
	virtual void finish();

	virtual Error capture_start();
	virtual Error capture_stop();
	virtual Array capture_get_device_list();
	virtual void capture_set_device(const String &p_name);
	virtual String capture_get_device();

	AudioDriverDSound();
};

#endif // DSOUND_ENABLED

#endif // AUDIO_DRIVER_DSOUND_H
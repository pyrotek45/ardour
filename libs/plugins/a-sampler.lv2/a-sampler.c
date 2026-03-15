/*
 * a-sampler.lv2 — ACE One-Shot Sampler
 *
 * Copyright (C) 2024 Ardour Community
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Any incoming MIDI note-on triggers playback of the loaded sample from
 * the start position to the end position, shaped by an ADSR amplitude
 * envelope.  Note-off begins the release phase.
 *
 * File loading is done off the RT thread via the LV2 Worker extension.
 * The file path is persisted via the LV2 State extension.
 *
 * An inline Cairo display renders the sample waveform so the loaded file
 * is visible in Ardour's mixer strip.
 */

#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* sndfile for decoding any libsndfile-supported format */
#include <sndfile.h>

/* LV2 */
#ifdef HAVE_LV2_1_18_6
#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/core/lv2.h>
#include <lv2/log/log.h>
#include <lv2/log/logger.h>
#include <lv2/midi/midi.h>
#include <lv2/state/state.h>
#include <lv2/urid/urid.h>
#include <lv2/worker/worker.h>
#else
#include <lv2/lv2plug.in/ns/lv2core/lv2.h>
#include <lv2/lv2plug.in/ns/ext/atom/atom.h>
#include <lv2/lv2plug.in/ns/ext/atom/util.h>
#include <lv2/lv2plug.in/ns/ext/log/log.h>
#include <lv2/lv2plug.in/ns/ext/log/logger.h>
#include <lv2/lv2plug.in/ns/ext/midi/midi.h>
#include <lv2/lv2plug.in/ns/ext/state/state.h>
#include <lv2/lv2plug.in/ns/ext/urid/urid.h>
#include <lv2/lv2plug.in/ns/ext/worker/worker.h>
#endif

/* Ardour inline-display extension */
#include "ardour/lv2_extensions.h"

/* ------------------------------------------------------------------ */
/* Plugin URI                                                           */
/* ------------------------------------------------------------------ */
#define ASAMPLER_URI "https://ardour.org/plugins/a-sampler"

/* ------------------------------------------------------------------ */
/* Port indices                                                         */
/* ------------------------------------------------------------------ */
typedef enum {
	AS_MIDI_IN = 0,
	AS_OUT_L   = 1,
	AS_OUT_R   = 2,
	AS_START   = 3,
	AS_END     = 4,
	AS_ATTACK  = 5,
	AS_DECAY   = 6,
	AS_SUSTAIN = 7,
	AS_RELEASE = 8,
} PortIndex;

/* ------------------------------------------------------------------ */
/* ADSR state machine                                                   */
/* ------------------------------------------------------------------ */
typedef enum {
	ENV_IDLE    = 0,
	ENV_ATTACK  = 1,
	ENV_DECAY   = 2,
	ENV_SUSTAIN = 3,
	ENV_RELEASE = 4,
} EnvState;

/* ------------------------------------------------------------------ */
/* Voice: one active MIDI note being played                            */
/* ------------------------------------------------------------------ */
#define MAX_VOICES 32

typedef struct {
	int       active;
	double    read_pos;     /* fractional sample position in decoded buffer */
	double    read_start;   /* sample position = start * n_samples            */
	double    read_end;     /* sample position = end   * n_samples            */
	float     velocity;     /* 0..1 */
	EnvState  env_state;
	double    env_level;    /* current amplitude 0..1 */
	double    env_release_level; /* amplitude when release started */
} Voice;

/* ------------------------------------------------------------------ */
/* Worker message types                                                 */
/* ------------------------------------------------------------------ */
#define WORKER_LOAD_FILE  1   /* main thread → worker: load path */
#define WORKER_FILE_READY 2   /* worker → run: new buffer ready  */

typedef struct {
	int      type;
	char     path[4096];
} WorkerLoad;

typedef struct {
	int      type;
	float*   data;          /* interleaved stereo (or mono duped) */
	uint32_t n_frames;      /* number of audio frames             */
	uint32_t n_channels;    /* 1 or 2                             */
	double   sample_rate;
} WorkerReady;

/* ------------------------------------------------------------------ */
/* Plugin instance                                                      */
/* ------------------------------------------------------------------ */
typedef struct {
	/* Ports */
	const LV2_Atom_Sequence* midi_in;
	float*  out_l;
	float*  out_r;
	const float* p_start;
	const float* p_end;
	const float* p_attack;
	const float* p_decay;
	const float* p_sustain;
	const float* p_release;

	/* LV2 features */
	LV2_URID_Map*        map;
	LV2_Worker_Schedule* schedule;
	LV2_Log_Logger       logger;
	LV2_Inline_Display*  queue_draw;

	/* URIDs */
	LV2_URID midi_MidiEvent;
	LV2_URID atom_Path;
	LV2_URID as_sample_file;

	/* Sample data (RT-safe pointer swap via worker) */
	float*   sample_data;       /* interleaved stereo frames      */
	uint32_t sample_frames;     /* total frames in buffer         */
	uint32_t sample_channels;   /* 1 or 2                         */
	double   sample_rate_orig;  /* sample rate of loaded file     */

	/* Pending new buffer (delivered by worker response) */
	float*   pending_data;
	uint32_t pending_frames;
	uint32_t pending_channels;

	/* Voices */
	Voice    voices[MAX_VOICES];

	/* Host sample rate */
	double   sample_rate;

	/* Current file path (used by state save) */
	char     current_path[4096];

	/* Inline display surface */
	LV2_Inline_Display_Image_Surface* display;
	bool     need_expose;
	bool     file_changed;
} ASampler;

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

static Voice*
find_free_voice (ASampler* self)
{
	for (int i = 0; i < MAX_VOICES; ++i) {
		if (!self->voices[i].active) return &self->voices[i];
	}
	/* steal oldest (first) active voice */
	return &self->voices[0];
}

static void
note_on (ASampler* self, int note, int vel)
{
	(void)note;
	if (!self->sample_data || self->sample_frames == 0) return;

	Voice* v = find_free_voice (self);
	v->active      = 1;
	v->velocity    = vel / 127.0f;
	v->env_state   = ENV_ATTACK;
	v->env_level   = 0.0;

	float start = *self->p_start;
	float end   = *self->p_end;
	if (start < 0.0f) start = 0.0f;
	if (end   > 1.0f) end   = 1.0f;
	if (start > end)  { float t = start; start = end; end = t; }

	v->read_start = start * (double)(self->sample_frames - 1);
	v->read_end   = end   * (double)(self->sample_frames - 1);
	v->read_pos   = v->read_start;
}

static void
note_off (ASampler* self, int note)
{
	(void)note;
	/* trigger release on all active non-releasing voices */
	for (int i = 0; i < MAX_VOICES; ++i) {
		Voice* v = &self->voices[i];
		if (v->active && v->env_state != ENV_RELEASE && v->env_state != ENV_IDLE) {
			v->env_state         = ENV_RELEASE;
			v->env_release_level = v->env_level;
		}
	}
}

/* Render one sample of ADSR envelope for a voice, returns amplitude */
static float
adsr_tick (ASampler* self, Voice* v)
{
	double sr = self->sample_rate;
	float  A  = *self->p_attack;
	float  D  = *self->p_decay;
	float  S  = *self->p_sustain;
	float  R  = *self->p_release;
	if (A < 1e-6f) A = 1e-6f;
	if (D < 1e-6f) D = 1e-6f;
	if (R < 1e-6f) R = 1e-6f;

	switch (v->env_state) {
	case ENV_ATTACK:
		v->env_level += 1.0 / (A * sr);
		if (v->env_level >= 1.0) {
			v->env_level = 1.0;
			v->env_state = ENV_DECAY;
		}
		break;
	case ENV_DECAY:
		v->env_level -= (1.0 - S) / (D * sr);
		if (v->env_level <= S) {
			v->env_level = S;
			v->env_state = ENV_SUSTAIN;
		}
		break;
	case ENV_SUSTAIN:
		v->env_level = S;
		break;
	case ENV_RELEASE:
		v->env_level -= v->env_release_level / (R * sr);
		if (v->env_level <= 0.0) {
			v->env_level = 0.0;
			v->env_state = ENV_IDLE;
			v->active    = 0;
		}
		break;
	case ENV_IDLE:
		return 0.0f;
	}
	return (float)v->env_level * v->velocity;
}

/* ------------------------------------------------------------------ */
/* LV2 lifecycle                                                        */
/* ------------------------------------------------------------------ */

static LV2_Handle
instantiate (const LV2_Descriptor*     descriptor,
             double                    rate,
             const char*               bundle_path,
             const LV2_Feature* const* features)
{
	(void)descriptor;
	(void)bundle_path;

	ASampler* self = (ASampler*)calloc (1, sizeof (ASampler));
	if (!self) return NULL;

	self->sample_rate = rate;

	for (int i = 0; features[i]; ++i) {
		if (!strcmp (features[i]->URI, LV2_URID__map)) {
			self->map = (LV2_URID_Map*)features[i]->data;
		} else if (!strcmp (features[i]->URI, LV2_WORKER__schedule)) {
			self->schedule = (LV2_Worker_Schedule*)features[i]->data;
		} else if (!strcmp (features[i]->URI, LV2_LOG__log)) {
			lv2_log_logger_init (&self->logger, self->map,
			                     (LV2_Log_Log*)features[i]->data);
		} else if (!strcmp (features[i]->URI, LV2_INLINEDISPLAY__queue_draw)) {
			self->queue_draw = (LV2_Inline_Display*)features[i]->data;
		}
	}

	if (!self->map) {
		fprintf (stderr, "a-sampler: missing feature urid:map\n");
		free (self);
		return NULL;
	}
	if (!self->schedule) {
		fprintf (stderr, "a-sampler: missing feature work:schedule\n");
		free (self);
		return NULL;
	}

	self->midi_MidiEvent = self->map->map (self->map->handle, LV2_MIDI__MidiEvent);
	self->atom_Path      = self->map->map (self->map->handle, LV2_ATOM__Path);
	self->as_sample_file = self->map->map (self->map->handle,
	                                       ASAMPLER_URI "#sampleFile");

	self->need_expose = false;
	self->file_changed = false;
	return (LV2_Handle)self;
}

static void
connect_port (LV2_Handle instance, uint32_t port, void* data)
{
	ASampler* self = (ASampler*)instance;
	switch ((PortIndex)port) {
	case AS_MIDI_IN: self->midi_in  = (const LV2_Atom_Sequence*)data; break;
	case AS_OUT_L:   self->out_l    = (float*)data; break;
	case AS_OUT_R:   self->out_r    = (float*)data; break;
	case AS_START:   self->p_start  = (const float*)data; break;
	case AS_END:     self->p_end    = (const float*)data; break;
	case AS_ATTACK:  self->p_attack = (const float*)data; break;
	case AS_DECAY:   self->p_decay  = (const float*)data; break;
	case AS_SUSTAIN: self->p_sustain= (const float*)data; break;
	case AS_RELEASE: self->p_release= (const float*)data; break;
	}
}

static void
activate (LV2_Handle instance)
{
	ASampler* self = (ASampler*)instance;
	memset (self->voices, 0, sizeof (self->voices));
}

static void
run (LV2_Handle instance, uint32_t n_samples)
{
	ASampler* self = (ASampler*)instance;

	/* Accept a freshly loaded sample from the worker */
	if (self->pending_data) {
		free (self->sample_data);
		self->sample_data     = self->pending_data;
		self->sample_frames   = self->pending_frames;
		self->sample_channels = self->pending_channels;
		self->pending_data    = NULL;
		/* silence all voices — old positions are invalid */
		memset (self->voices, 0, sizeof (self->voices));
		self->file_changed = true;
		self->need_expose  = true;
	}

	/* Clear output */
	memset (self->out_l, 0, n_samples * sizeof (float));
	memset (self->out_r, 0, n_samples * sizeof (float));

	/* Process MIDI */
	LV2_ATOM_SEQUENCE_FOREACH (self->midi_in, ev) {
		if (ev->body.type != self->midi_MidiEvent) continue;
		const uint8_t* msg = (const uint8_t*)(ev + 1);
		uint8_t type = msg[0] & 0xf0;
		if (type == 0x90 && msg[2] > 0) {        /* note on  */
			note_on  (self, msg[1], msg[2]);
		} else if (type == 0x80 ||                 /* note off */
		           (type == 0x90 && msg[2] == 0)) {
			note_off (self, msg[1]);
		} else if (type == 0xb0 && msg[1] == 123) { /* all notes off */
			for (int i = 0; i < MAX_VOICES; ++i) {
				if (self->voices[i].active) {
					self->voices[i].env_state         = ENV_RELEASE;
					self->voices[i].env_release_level = self->voices[i].env_level;
				}
			}
		}
	}

	/* Synthesise all active voices */
	for (uint32_t s = 0; s < n_samples; ++s) {
		float L = 0.0f, R = 0.0f;
		for (int vi = 0; vi < MAX_VOICES; ++vi) {
			Voice* v = &self->voices[vi];
			if (!v->active) continue;

			float amp = adsr_tick (self, v);

			if (v->active && self->sample_data) {
				uint32_t pos = (uint32_t)v->read_pos;
				if (pos >= self->sample_frames) {
					/* reached end of sample */
					v->env_state = ENV_RELEASE;
					v->env_release_level = v->env_level;
					continue;
				}

				float sl, sr;
				if (self->sample_channels == 1) {
					sl = sr = self->sample_data[pos];
				} else {
					sl = self->sample_data[pos * 2 + 0];
					sr = self->sample_data[pos * 2 + 1];
				}

				L += sl * amp;
				R += sr * amp;

				v->read_pos += 1.0;

				if (v->read_pos >= v->read_end) {
					/* reached end marker — go to release */
					v->env_state         = ENV_RELEASE;
					v->env_release_level = v->env_level;
				}
			}
		}
		self->out_l[s] = L;
		self->out_r[s] = R;
	}

	/* Trigger redraw if needed */
	if (self->need_expose && self->queue_draw) {
		self->need_expose = false;
		self->queue_draw->queue_draw (self->queue_draw->handle);
	}
}

static void
deactivate (LV2_Handle instance)
{
	(void)instance;
}

static void
cleanup (LV2_Handle instance)
{
	ASampler* self = (ASampler*)instance;
	free (self->sample_data);
	free (self->pending_data);
	if (self->display) {
		free (self->display->data);
		free (self->display);
	}
	free (self);
}

/* ------------------------------------------------------------------ */
/* LV2 Worker                                                           */
/* ------------------------------------------------------------------ */

static LV2_Worker_Status
work (LV2_Handle                  instance,
      LV2_Worker_Respond_Function respond,
      LV2_Worker_Respond_Handle   handle,
      uint32_t                    size,
      const void*                 data)
{
	ASampler* self = (ASampler*)instance;
	(void)size;

	const WorkerLoad* msg = (const WorkerLoad*)data;
	if (msg->type != WORKER_LOAD_FILE) return LV2_WORKER_ERR_UNKNOWN;

	/* Open the audio file with libsndfile */
	SF_INFO info;
	memset (&info, 0, sizeof (info));
	SNDFILE* sf = sf_open (msg->path, SFM_READ, &info);
	if (!sf) {
		lv2_log_error (&self->logger, "a-sampler: cannot open '%s': %s\n",
		               msg->path, sf_strerror (NULL));
		return LV2_WORKER_ERR_UNKNOWN;
	}

	uint32_t n_channels = (uint32_t)info.channels;
	if (n_channels > 2) n_channels = 2;   /* cap at stereo */
	uint32_t n_frames   = (uint32_t)info.frames;

	/* Read all frames as interleaved float */
	float* raw = (float*)malloc (sizeof (float) * (uint32_t)info.channels * n_frames);
	if (!raw) {
		sf_close (sf);
		return LV2_WORKER_ERR_NO_SPACE;
	}
	sf_count_t got = sf_readf_float (sf, raw, info.frames);
	sf_close (sf);

	/* Build our (possibly channel-reduced) buffer */
	float* buf = (float*)malloc (sizeof (float) * n_channels * n_frames);
	if (!buf) { free (raw); return LV2_WORKER_ERR_NO_SPACE; }

	for (uint32_t f = 0; f < (uint32_t)got; ++f) {
		if (n_channels == 1) {
			buf[f] = raw[f * (uint32_t)info.channels];
		} else {
			buf[f * 2 + 0] = raw[f * (uint32_t)info.channels + 0];
			buf[f * 2 + 1] = raw[f * (uint32_t)info.channels + 1];
		}
	}
	free (raw);

	/* Send the loaded buffer back to run() via work_response */
	WorkerReady resp;
	resp.type       = WORKER_FILE_READY;
	resp.data       = buf;
	resp.n_frames   = (uint32_t)got;
	resp.n_channels = n_channels;
	resp.sample_rate = info.samplerate;

	respond (handle, sizeof (resp), &resp);
	return LV2_WORKER_SUCCESS;
}

static LV2_Worker_Status
work_response (LV2_Handle  instance,
               uint32_t    size,
               const void* data)
{
	ASampler* self = (ASampler*)instance;
	(void)size;

	const WorkerReady* resp = (const WorkerReady*)data;
	if (resp->type != WORKER_FILE_READY) return LV2_WORKER_ERR_UNKNOWN;

	/* Stash the new buffer; run() will swap it in at next cycle */
	free (self->pending_data);
	self->pending_data     = resp->data;
	self->pending_frames   = resp->n_frames;
	self->pending_channels = resp->n_channels;
	self->sample_rate_orig = resp->sample_rate;

	return LV2_WORKER_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* LV2 State                                                            */
/* ------------------------------------------------------------------ */

static LV2_State_Status
state_save (LV2_Handle                instance,
            LV2_State_Store_Function  store,
            LV2_State_Handle          handle,
            uint32_t                  flags,
            const LV2_Feature* const* features)
{
	ASampler* self = (ASampler*)instance;
	if (self->current_path[0] == '\0') return LV2_STATE_SUCCESS;

	LV2_State_Map_Path* map_path = NULL;
#ifdef LV2_STATE__freePath
	LV2_State_Free_Path* free_path = NULL;
#endif
	for (int i = 0; features[i]; ++i) {
		if (!strcmp (features[i]->URI, LV2_STATE__mapPath)) {
			map_path = (LV2_State_Map_Path*)features[i]->data;
		}
#ifdef LV2_STATE__freePath
		else if (!strcmp (features[i]->URI, LV2_STATE__freePath)) {
			free_path = (LV2_State_Free_Path*)features[i]->data;
		}
#endif
	}
	if (!map_path) return LV2_STATE_ERR_NO_FEATURE;

	char* apath = map_path->abstract_path (map_path->handle, self->current_path);
	store (handle, self->as_sample_file,
	       apath, strlen (apath) + 1,
	       self->atom_Path, LV2_STATE_IS_POD);
#ifdef LV2_STATE__freePath
	if (free_path) {
		free_path->free_path (free_path->handle, apath);
	} else
#endif
	{
#ifndef _WIN32
		free (apath);
#endif
	}
	return LV2_STATE_SUCCESS;
}

static LV2_State_Status
state_restore (LV2_Handle                  instance,
               LV2_State_Retrieve_Function retrieve,
               LV2_State_Handle            handle,
               uint32_t                    flags,
               const LV2_Feature* const*   features)
{
	ASampler* self = (ASampler*)instance;

	LV2_State_Map_Path* map_path = NULL;
	for (int i = 0; features[i]; ++i) {
		if (!strcmp (features[i]->URI, LV2_STATE__mapPath)) {
			map_path = (LV2_State_Map_Path*)features[i]->data;
		}
	}

	size_t      size  = 0;
	uint32_t    type  = 0;
	uint32_t    fflags = 0;
	const void* val   = retrieve (handle, self->as_sample_file, &size, &type, &fflags);
	if (!val || size == 0) return LV2_STATE_SUCCESS;

	const char* abstract = (const char*)val;
	const char* path = abstract;
	char*       mapped = NULL;

	if (map_path) {
		mapped = map_path->absolute_path (map_path->handle, abstract);
		if (mapped) path = mapped;
	}

	strncpy (self->current_path, path, sizeof (self->current_path) - 1);
	self->current_path[sizeof (self->current_path) - 1] = '\0';

	/* Schedule file load */
	WorkerLoad msg;
	msg.type = WORKER_LOAD_FILE;
	strncpy (msg.path, self->current_path, sizeof (msg.path) - 1);
	msg.path[sizeof (msg.path) - 1] = '\0';
	self->schedule->schedule_work (self->schedule->handle, sizeof (msg), &msg);

#ifndef _WIN32
	free (mapped);
#endif
	return LV2_STATE_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* LV2 Inline Display                                                   */
/* ------------------------------------------------------------------ */

#ifdef HAVE_LV2_1_2_0
#include <cairo/cairo.h>

static LV2_Inline_Display_Image_Surface*
render_inline (LV2_Handle instance, uint32_t w, uint32_t max_h)
{
	ASampler* self = (ASampler*)instance;

	uint32_t h = w / 3;
	if (h < 40)  h = 40;
	if (h > max_h) h = max_h;

	/* Allocate or resize surface */
	if (!self->display || self->display->width != (int)w || self->display->height != (int)h) {
		if (self->display) {
			free (self->display->data);
			free (self->display);
		}
		self->display = (LV2_Inline_Display_Image_Surface*)calloc (1, sizeof (LV2_Inline_Display_Image_Surface));
		if (!self->display) return NULL;
		self->display->data   = (unsigned char*)malloc (4 * w * h);
		if (!self->display->data) { free (self->display); self->display = NULL; return NULL; }
		self->display->width  = (int)w;
		self->display->height = (int)h;
		self->display->stride = (int)(4 * w);
	}

	cairo_surface_t* surface = cairo_image_surface_create_for_data (
		self->display->data,
		CAIRO_FORMAT_ARGB32,
		(int)w, (int)h, (int)(4 * w));

	cairo_t* cr = cairo_create (surface);

	/* Background */
	cairo_set_source_rgb (cr, 0.15, 0.15, 0.15);
	cairo_paint (cr);

	if (!self->sample_data || self->sample_frames == 0) {
		/* No sample — draw placeholder text */
		cairo_set_source_rgb (cr, 0.5, 0.5, 0.5);
		cairo_set_font_size (cr, 11.0);
		cairo_move_to (cr, 6, h / 2.0 + 4);
		cairo_show_text (cr, "Drop sample here");
	} else {
		/* Draw waveform — downsample to pixel width */
		float* data    = self->sample_data;
		uint32_t total = self->sample_frames;
		uint32_t ch    = self->sample_channels;

		/* Start / end markers in sample space */
		float start_n = *self->p_start;
		float end_n   = *self->p_end;
		if (start_n < 0.f) start_n = 0.f;
		if (end_n   > 1.f) end_n   = 1.f;

		/* Shaded region outside start–end */
		cairo_set_source_rgba (cr, 0.1, 0.1, 0.1, 0.6);
		cairo_rectangle (cr, 0, 0, (double)w * start_n, h);
		cairo_fill (cr);
		cairo_rectangle (cr, (double)w * end_n, 0, (double)w * (1.f - end_n), h);
		cairo_fill (cr);

		/* Waveform */
		cairo_set_source_rgb (cr, 0.2, 0.7, 0.3);
		cairo_set_line_width (cr, 1.0);

		double mid  = h / 2.0;
		double gain = mid * 0.9;

		for (uint32_t x = 0; x < w; ++x) {
			uint32_t i0 = (uint32_t)((double)x       / w * total);
			uint32_t i1 = (uint32_t)((double)(x + 1) / w * total);
			if (i1 > total) i1 = total;
			if (i0 >= i1)   i1 = i0 + 1;
			if (i1 > total) i1 = total;

			float mn =  1.f, mx = -1.f;
			for (uint32_t i = i0; i < i1; ++i) {
				float s = (ch == 1) ? data[i] : 0.5f * (data[i*2] + data[i*2+1]);
				if (s < mn) mn = s;
				if (s > mx) mx = s;
			}

			cairo_move_to (cr, x + 0.5, mid - mx * gain);
			cairo_line_to (cr, x + 0.5, mid - mn * gain);
		}
		cairo_stroke (cr);

		/* Start marker (green line) */
		cairo_set_source_rgb (cr, 0.3, 1.0, 0.3);
		cairo_set_line_width (cr, 1.5);
		double sx = (double)w * start_n;
		cairo_move_to (cr, sx, 0);
		cairo_line_to (cr, sx, h);
		cairo_stroke (cr);

		/* End marker (red line) */
		cairo_set_source_rgb (cr, 1.0, 0.3, 0.3);
		double ex = (double)w * end_n;
		cairo_move_to (cr, ex, 0);
		cairo_line_to (cr, ex, h);
		cairo_stroke (cr);
	}

	/* Border */
	cairo_set_source_rgb (cr, 0.4, 0.4, 0.4);
	cairo_set_line_width (cr, 1.0);
	cairo_rectangle (cr, 0.5, 0.5, w - 1, h - 1);
	cairo_stroke (cr);

	cairo_destroy (cr);
	cairo_surface_destroy (surface);

	return self->display;
}
#endif /* HAVE_LV2_1_2_0 */

/* ------------------------------------------------------------------ */
/* Public load helper called by Ardour drag-and-drop                    */
/* ------------------------------------------------------------------ */

/**
 * load_sample_file:
 *   Called by Ardour when a file is dropped onto the plugin inline
 *   display.  Schedules the file load via the LV2 worker so the RT
 *   thread is never blocked.
 */
static void __attribute__((unused))
load_sample_file (LV2_Handle instance, const char* path)
{
	ASampler* self = (ASampler*)instance;

	strncpy (self->current_path, path, sizeof (self->current_path) - 1);
	self->current_path[sizeof (self->current_path) - 1] = '\0';

	WorkerLoad msg;
	msg.type = WORKER_LOAD_FILE;
	strncpy (msg.path, path, sizeof (msg.path) - 1);
	self->schedule->schedule_work (self->schedule->handle, sizeof (msg), &msg);
}

/* ------------------------------------------------------------------ */
/* Extension interfaces                                                 */
/* ------------------------------------------------------------------ */

static const void*
extension_data (const char* uri)
{
	static const LV2_State_Interface  state  = { state_save, state_restore };
	static const LV2_Worker_Interface worker = { work, work_response, NULL };

	if (!strcmp (uri, LV2_STATE__interface))  return &state;
	if (!strcmp (uri, LV2_WORKER__interface)) return &worker;

#ifdef HAVE_LV2_1_2_0
	static const LV2_Inline_Display_Interface display = { render_inline };
	if (!strcmp (uri, LV2_INLINEDISPLAY__interface)) return &display;
#endif

	return NULL;
}

/* ------------------------------------------------------------------ */
/* Descriptor                                                           */
/* ------------------------------------------------------------------ */

static const LV2_Descriptor descriptor = {
	ASAMPLER_URI,
	instantiate,
	connect_port,
	activate,
	run,
	deactivate,
	cleanup,
	extension_data,
};

LV2_SYMBOL_EXPORT
const LV2_Descriptor*
lv2_descriptor (uint32_t index)
{
	return (index == 0) ? &descriptor : NULL;
}

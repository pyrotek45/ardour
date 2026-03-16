/*
 * a-sampler.lv2 -- ACE One-Shot Sampler
 *
 * Copyright (C) 2024 Ardour Community
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Gate mode ON  (default): classic gate -- note-off starts Release phase.
 * Gate mode OFF (one-shot): note-on fires the sample from the beginning;
 *   the playback continues even after note-off.  A new note-on (any pitch)
 *   immediately stops the previous voice and starts a fresh one.
 *
 * Stop port: a momentary trigger that silences all active voices immediately
 *   (fast release, ~5ms).
 *
 * Prev/Next Sample: trigger ports that browse the directory of the currently
 *   loaded file and load the adjacent audio file (alphabetically).
 *
 * Gain: output gain slider in dB (-24 to +12).
 * HP/LP: TPT state-variable highpass and lowpass filters.
 * Distortion: drive + flavour selector (Off, Soft Clip, Hard Clip, Fold,
 *   Bit Crush).
 *
 * File loading uses the LV2 Worker extension so the RT thread is never blocked.
 * File path is communicated via patch:Set / patch:Get on the atom ports.
 * Path is saved / restored via LV2 State.
 */

#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#include <sndfile.h>

#ifdef HAVE_LV2_1_18_6
#include <lv2/atom/atom.h>
#include <lv2/atom/forge.h>
#include <lv2/atom/util.h>
#include <lv2/core/lv2.h>
#include <lv2/log/log.h>
#include <lv2/log/logger.h>
#include <lv2/midi/midi.h>
#include <lv2/patch/patch.h>
#include <lv2/state/state.h>
#include <lv2/urid/urid.h>
#include <lv2/worker/worker.h>
#else
#include <lv2/lv2plug.in/ns/lv2core/lv2.h>
#include <lv2/lv2plug.in/ns/ext/atom/atom.h>
#include <lv2/lv2plug.in/ns/ext/atom/forge.h>
#include <lv2/lv2plug.in/ns/ext/atom/util.h>
#include <lv2/lv2plug.in/ns/ext/log/log.h>
#include <lv2/lv2plug.in/ns/ext/log/logger.h>
#include <lv2/lv2plug.in/ns/ext/midi/midi.h>
#include <lv2/lv2plug.in/ns/ext/patch/patch.h>
#include <lv2/lv2plug.in/ns/ext/state/state.h>
#include <lv2/lv2plug.in/ns/ext/urid/urid.h>
#include <lv2/lv2plug.in/ns/ext/worker/worker.h>
#endif

#include "ardour/lv2_extensions.h"
#include <cairo/cairo.h>

/* ------------------------------------------------------------------ */
/* Defines                                                              */
/* ------------------------------------------------------------------ */

#define ASAMPLER_URI      "https://ardour.org/plugins/a-sampler"
#define ASAMPLER_FILE_URI "urn:ardour:a-sampler:sampleFile"

#ifdef LV2_ATOM__Object
#define x_forge_object lv2_atom_forge_object
#else
#define x_forge_object lv2_atom_forge_blank
#endif

/* fast-stop release in samples (~5ms at any SR) */
#define STOP_RELEASE_SAMPLES 256

/* ------------------------------------------------------------------ */
/* Port indices  (must match a-sampler.ttl.in exactly)                 */
/* ------------------------------------------------------------------ */
typedef enum {
	AS_CONTROL     = 0,
	AS_NOTIFY      = 1,
	AS_OUT_L       = 2,
	AS_OUT_R       = 3,
	AS_START       = 4,
	AS_END         = 5,
	AS_ATTACK      = 6,
	AS_DECAY       = 7,
	AS_SUSTAIN     = 8,
	AS_RELEASE     = 9,
	AS_GATE        = 10,
	AS_STOP        = 11,
	AS_PREV_SAMPLE = 12,
	AS_NEXT_SAMPLE = 13,
	AS_GAIN         = 14,
	AS_HP_FREQ      = 15,
	AS_LP_FREQ      = 16,
	AS_DIST_DRIVE   = 17,
	AS_DIST_TYPE    = 18,
	AS_PITCH_ENABLE = 19,
	AS_ROOT_NOTE    = 20,
	AS_LEGATO       = 21,
} PortIndex;

/* ------------------------------------------------------------------ */
/* ADSR                                                                 */
/* ------------------------------------------------------------------ */
typedef enum {
	ENV_IDLE    = 0,
	ENV_ATTACK  = 1,
	ENV_DECAY   = 2,
	ENV_SUSTAIN = 3,
	ENV_RELEASE = 4,
	ENV_FASTOFF = 5, /* used by Stop button -- very short fade */
} EnvState;

/* ------------------------------------------------------------------ */
/* Voices                                                               */
/* ------------------------------------------------------------------ */
#define MAX_VOICES 32

typedef struct {
	int      active;
	double   read_pos;
	double   read_end;
	double   read_step;  /* playback speed ratio (1.0 = normal, 2.0 = +1 oct) */
	float    velocity;
	int      midi_note;  /* the MIDI note number that triggered this voice */
	EnvState env_state;
	double   env_level;
	double   env_release_level;
	int      oneshot;   /* 1 = play to end regardless of note-off */
} Voice;

/* ------------------------------------------------------------------ */
/* Worker messages                                                      */
/* ------------------------------------------------------------------ */
#define MSG_LOAD_FILE    1
#define MSG_FILE_READY   2
#define MSG_BROWSE       3   /* prev/next: direction -1 or +1          */
#define MSG_BROWSE_DONE  4   /* response: new path + decoded audio     */

typedef struct {
	int  type;
	char path[4096];
} MsgLoad;

typedef struct {
	int  type;
	int  direction;   /* -1 = prev, +1 = next */
	char path[4096];  /* current file path     */
} MsgBrowse;

typedef struct {
	int      type;
	float*   data;
	uint32_t n_frames;
	uint32_t n_channels;
	char     new_path[4096]; /* the file that was actually loaded */
} MsgReady;

/* ------------------------------------------------------------------ */
/* Plugin instance                                                      */
/* ------------------------------------------------------------------ */
typedef struct {
	/* ports */
	const LV2_Atom_Sequence* control;
	LV2_Atom_Sequence*       notify;
	float*       out_l;
	float*       out_r;
	const float* p_start;
	const float* p_end;
	const float* p_attack;
	const float* p_decay;
	const float* p_sustain;
	const float* p_release;
	const float* p_gate;
	const float* p_stop;
	const float* p_prev;
	const float* p_next;
	/* new FX ports */
	const float* p_gain;
	const float* p_hp_freq;
	const float* p_lp_freq;
	const float* p_dist_drive;
	const float* p_dist_type;
	/* pitch ports */
	const float* p_pitch_enable;
	const float* p_root_note;
	const float* p_legato;

	/* features */
	LV2_URID_Map*        map;
	LV2_Worker_Schedule* schedule;
	LV2_Log_Logger       logger;
	LV2_Inline_Display*  queue_draw;

	/* forge (for notify port) */
	LV2_Atom_Forge       forge;
	LV2_Atom_Forge_Frame forge_frame;

	/* URIDs */
	LV2_URID atom_Blank;
	LV2_URID atom_Object;
	LV2_URID atom_Path;
	LV2_URID atom_URID;
	LV2_URID midi_MidiEvent;
	LV2_URID patch_Get;
	LV2_URID patch_Set;
	LV2_URID patch_property;
	LV2_URID patch_value;
	LV2_URID as_sample_file;

	/* sample (RT double-buffer via worker) */
	float*   sample_data;
	uint32_t sample_frames;
	uint32_t sample_channels;
	float*   pending_data;
	uint32_t pending_frames;
	uint32_t pending_channels;
	char     pending_path[4096]; /* path of the pending sample */

	/* voices */
	Voice    voices[MAX_VOICES];

	/* host state */
	double   sample_rate;
	char     current_path[4096];
	bool     inform_ui;

	/* trigger edge tracking */
	float    prev_stop;
	float    prev_prev;
	float    prev_next;

	/* TPT SVF filter state (stereo: _l and _r) */
	float    hp_ic1eq_l, hp_ic1eq_r;
	float    hp_ic2eq_l, hp_ic2eq_r;
	float    lp_ic1eq_l, lp_ic1eq_r;
	float    lp_ic2eq_l, lp_ic2eq_r;

	/* inline display */
	LV2_Inline_Display_Image_Surface* display;
	bool     need_expose;
} ASampler;

/* ------------------------------------------------------------------ */
/* Audio file extension check                                           */
/* ------------------------------------------------------------------ */
static int
is_audio_file (const char* name)
{
	const char* dot = strrchr (name, '.');
	if (!dot) return 0;
	/* case-insensitive compare of a few common extensions */
	char ext[16];
	int i;
	for (i = 0; i < 15 && dot[1 + i]; ++i)
		ext[i] = (char)((dot[1 + i] >= 'A' && dot[1 + i] <= 'Z')
		                ? dot[1 + i] + 32 : dot[1 + i]);
	ext[i] = '\0';
	return (!strcmp (ext, "wav")  || !strcmp (ext, "flac") ||
	        !strcmp (ext, "aif")  || !strcmp (ext, "aiff") ||
	        !strcmp (ext, "ogg")  || !strcmp (ext, "mp3")  ||
	        !strcmp (ext, "w64")  || !strcmp (ext, "caf"));
}

static int
cmp_str (const void* a, const void* b)
{
	/* qsort passes pointers to the array elements (char*), so cast via
	 * void* to avoid -Wcast-qual on the intermediate char** */
	char* const* pa = (char* const*)a;
	char* const* pb = (char* const*)b;
	return strcmp (*pa, *pb);
}

/* ------------------------------------------------------------------ */
/* DSP helpers: TPT State-Variable Filter (Cytomic/Andy Simper form)  */
/* ------------------------------------------------------------------ */

/* Correct 2-pole TPT SVF.
 * Reference: "Solving the Continuous SVF Equations Using Trapezoidal
 *             Integration and Equivalent Circuits" — Andy Simper, 2013.
 *
 * k = sqrt(2) gives Butterworth (maximally flat) response.
 * g = tan(pi * freq / samplerate)
 *
 * Pre-computed coefficients (only need to be recalculated when g changes):
 *   a1 = 1 / (1 + g * (g + k))
 *   a2 = g * a1
 *   a3 = g * a2
 *
 * Per-sample update:
 *   v3 = x - ic2eq
 *   v1 = a1*ic1eq + a2*v3
 *   v2 = ic2eq + a2*ic1eq + a3*v3
 *   ic1eq = 2*v1 - ic1eq
 *   ic2eq = 2*v2 - ic2eq
 *   hp = x - k*v1 - v2
 *   lp = v2
 */

/* k = sqrt(2) for Butterworth */
#define SVF_K 1.41421356237f

static inline float
svf_hp (float* ic1eq, float* ic2eq, float g, float x)
{
	float a1 = 1.f / (1.f + g * (g + SVF_K));
	float a2 = g * a1;
	float a3 = g * a2;
	float v3 = x - *ic2eq;
	float v1 = a1 * *ic1eq + a2 * v3;
	float v2 = *ic2eq + a2 * *ic1eq + a3 * v3;
	*ic1eq = 2.f * v1 - *ic1eq;
	*ic2eq = 2.f * v2 - *ic2eq;
	return x - SVF_K * v1 - v2;  /* hp = x - k*bp - lp */
}

static inline float
svf_lp (float* ic1eq, float* ic2eq, float g, float x)
{
	float a1 = 1.f / (1.f + g * (g + SVF_K));
	float a2 = g * a1;
	float a3 = g * a2;
	float v3 = x - *ic2eq;
	float v1 = a1 * *ic1eq + a2 * v3;
	float v2 = *ic2eq + a2 * *ic1eq + a3 * v3;
	*ic1eq = 2.f * v1 - *ic1eq;
	*ic2eq = 2.f * v2 - *ic2eq;
	return v2;  /* lp = v2 */
}

/* ------------------------------------------------------------------ */
/* DSP helpers: Distortion                                             */
/* ------------------------------------------------------------------ */

/* Soft clip: tanh saturation (type 1) */
static inline float
dist_soft_clip (float x, float drive)
{
	float d = 1.f + drive * 15.f;   /* 1x to 16x */
	float denom = tanhf (d);
	if (denom < 1e-9f) denom = 1e-9f;
	return tanhf (x * d) / denom;
}

/* Hard clip (type 2) */
static inline float
dist_hard_clip (float x, float drive)
{
	float thresh = 1.f - drive * 0.85f;  /* 1.0 down to 0.15 */
	if (thresh < 0.01f) thresh = 0.01f;
	float out = x;
	if (out >  thresh) out =  thresh;
	if (out < -thresh) out = -thresh;
	return out / thresh;  /* normalise back to ~[-1,1] */
}

/* Wavefold (type 3): sin-based fold */
static inline float
dist_fold (float x, float drive)
{
	float d = 1.f + drive * 5.f;   /* 1x to 6x */
	return sinf (x * d * (float)M_PI);
}

/* Bit crush (type 4): quantise */
static inline float
dist_bitcrush (float x, float drive)
{
	/* drive=0 -> 16384 steps (basically transparent)
	 * drive=1 -> 4 steps    (very harsh)                */
	float steps = powf (2.f, 14.f - drive * 12.f);
	if (steps < 1.f) steps = 1.f;
	return floorf (x * steps + 0.5f) / steps;
}

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

static Voice*
find_free_voice (ASampler* self)
{
	for (int i = 0; i < MAX_VOICES; ++i) {
		if (!self->voices[i].active) return &self->voices[i];
	}
	return &self->voices[0]; /* steal oldest */
}

/* In one-shot mode, kill all currently playing voices instantly (fast fade). */
static void
oneshot_kill_all (ASampler* self)
{
	for (int i = 0; i < MAX_VOICES; ++i) {
		Voice* v = &self->voices[i];
		if (v->active) {
			v->env_state         = ENV_FASTOFF;
			v->env_release_level = v->env_level;
		}
	}
}

static void
note_on (ASampler* self, int midi_note, int vel)
{
	if (!self->sample_data || self->sample_frames == 0) return;

	int gate         = (self->p_gate         && *self->p_gate         > 0.5f) ? 1 : 0;
	int pitch_on     = (self->p_pitch_enable && *self->p_pitch_enable > 0.5f) ? 1 : 0;
	int legato_on    = (self->p_legato       && *self->p_legato       > 0.5f) ? 1 : 0;
	int root_note    = self->p_root_note ? (int)(*self->p_root_note + 0.5f) : 60;

	/* Compute playback speed ratio.
	 * pitch_on = 0  → always 1.0 (original speed, same pitch for every note)
	 * pitch_on = 1  → 2^((midi_note - root_note) / 12) semitone ratio           */
	double read_step = 1.0;
	if (pitch_on) {
		read_step = pow (2.0, (midi_note - root_note) / 12.0);
	}

	float s = self->p_start ? *self->p_start : 0.f;
	float e = self->p_end   ? *self->p_end   : 1.f;
	if (s < 0.f) s = 0.f;
	if (e > 1.f) e = 1.f;
	if (s > e) { float t = s; s = e; e = t; }
	if (e - s < 1e-4f) e = s + 1e-4f;
	if (e > 1.f) e = 1.f;

	double start_frame = s * (double)(self->sample_frames - 1);
	double end_frame   = e * (double)(self->sample_frames - 1);
	if (end_frame <= start_frame) end_frame = start_frame + 1;

	/* Legato: if a voice is already playing (in gate mode), just update its
	 * pitch and don't restart the envelope or position. */
	if (legato_on && gate) {
		for (int i = 0; i < MAX_VOICES; ++i) {
			Voice* v = &self->voices[i];
			if (v->active && v->env_state != ENV_RELEASE
			              && v->env_state != ENV_FASTOFF
			              && v->env_state != ENV_IDLE) {
				v->midi_note = midi_note;
				v->read_step = read_step;
				v->read_end  = end_frame;
				return;  /* legato: don't spawn a new voice */
			}
		}
	}

	if (!gate) {
		/* one-shot mode: stop any previous voice immediately */
		oneshot_kill_all (self);
	}

	Voice* v       = find_free_voice (self);
	v->active      = 1;
	v->midi_note   = midi_note;
	v->velocity    = vel / 127.0f;
	v->env_state   = ENV_ATTACK;
	v->env_level   = 0.0;
	v->oneshot     = gate ? 0 : 1;
	v->read_step   = read_step;
	v->read_pos    = start_frame;
	v->read_end    = end_frame;
}

/* Gate note-off: only trigger release if gate mode is on */
static void
note_off_gate (ASampler* self)
{
	int gate = (*self->p_gate > 0.5f) ? 1 : 0;
	if (!gate) return; /* one-shot: ignore note-off */

	for (int i = 0; i < MAX_VOICES; ++i) {
		Voice* v = &self->voices[i];
		if (v->active && v->env_state != ENV_RELEASE
		              && v->env_state != ENV_FASTOFF
		              && v->env_state != ENV_IDLE) {
			v->env_state         = ENV_RELEASE;
			v->env_release_level = v->env_level;
		}
	}
}

/* Stop all voices with a very fast fade (used by Stop button and CC123) */
static void
stop_all (ASampler* self)
{
	for (int i = 0; i < MAX_VOICES; ++i) {
		Voice* v = &self->voices[i];
		if (v->active) {
			v->env_state         = ENV_FASTOFF;
			v->env_release_level = v->env_level;
		}
	}
}

static float
adsr_tick (ASampler* self, Voice* v)
{
	double sr = self->sample_rate;
	float  A  = *self->p_attack;
	float  D  = *self->p_decay;
	float  S  = *self->p_sustain;
	float  R  = *self->p_release;
	/* clamp to minimum meaningful values */
	if (A < 0.001f) A = 0.001f;
	if (D < 0.001f) D = 0.001f;
	if (R < 0.001f) R = 0.001f;

	switch (v->env_state) {
	case ENV_ATTACK:
		v->env_level += 1.0 / (A * sr);
		if (v->env_level >= 1.0) {
			v->env_level = 1.0;
			v->env_state = ENV_DECAY;
		}
		break;
	case ENV_DECAY:
		v->env_level -= (1.0 - (double)S) / (D * sr);
		if (v->env_level <= (double)S) {
			v->env_level = (double)S;
			v->env_state = ENV_SUSTAIN;
		}
		break;
	case ENV_SUSTAIN:
		v->env_level = (double)S;
		break;
	case ENV_RELEASE:
		if (v->env_release_level < 1e-9) {
			v->env_level = 0.0;
			v->env_state = ENV_IDLE;
			v->active    = 0;
		} else {
			v->env_level -= v->env_release_level / (R * sr);
			if (v->env_level <= 0.0) {
				v->env_level = 0.0;
				v->env_state = ENV_IDLE;
				v->active    = 0;
			}
		}
		break;
	case ENV_FASTOFF:
		/* ~5ms fade regardless of R setting */
		v->env_level -= v->env_release_level / (double)STOP_RELEASE_SAMPLES;
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

static void
inform_ui (ASampler* self)
{
	if (self->current_path[0] == '\0') return;
	LV2_Atom_Forge_Frame frame;
	lv2_atom_forge_frame_time (&self->forge, 0);
	x_forge_object (&self->forge, &frame, 1, self->patch_Set);
	lv2_atom_forge_key (&self->forge, self->patch_property);
	lv2_atom_forge_urid (&self->forge, self->as_sample_file);
	lv2_atom_forge_key (&self->forge, self->patch_value);
	lv2_atom_forge_path (&self->forge, self->current_path, strlen (self->current_path));
	lv2_atom_forge_pop (&self->forge, &frame);
}

static const LV2_Atom*
parse_patch_set (ASampler* self, const LV2_Atom_Object* obj)
{
	if (obj->body.otype != self->patch_Set) return NULL;

	const LV2_Atom* property = NULL;
	const LV2_Atom* value    = NULL;
	lv2_atom_object_get (obj, self->patch_property, &property, 0);
	if (!property || property->type != self->atom_URID) return NULL;
	if (((const LV2_Atom_URID*)property)->body != self->as_sample_file) return NULL;

	lv2_atom_object_get (obj, self->patch_value, &value, 0);
	if (!value || value->type != self->atom_Path) return NULL;
	return value;
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
	(void)descriptor; (void)bundle_path;

	ASampler* self = (ASampler*)calloc (1, sizeof (ASampler));
	if (!self) return NULL;
	self->sample_rate = rate;
	self->prev_stop   = 0.f;
	self->prev_prev   = 0.f;
	self->prev_next   = 0.f;

	for (int i = 0; features[i]; ++i) {
		if (!strcmp (features[i]->URI, LV2_URID__map))
			self->map = (LV2_URID_Map*)features[i]->data;
		else if (!strcmp (features[i]->URI, LV2_WORKER__schedule))
			self->schedule = (LV2_Worker_Schedule*)features[i]->data;
		else if (!strcmp (features[i]->URI, LV2_LOG__log))
			lv2_log_logger_init (&self->logger, self->map, (LV2_Log_Log*)features[i]->data);
		else if (!strcmp (features[i]->URI, LV2_INLINEDISPLAY__queue_draw))
			self->queue_draw = (LV2_Inline_Display*)features[i]->data;
	}

	if (!self->map || !self->schedule) {
		fprintf (stderr, "a-sampler: missing required features\n");
		free (self);
		return NULL;
	}

	LV2_URID_Map* map = self->map;
	self->atom_Blank     = map->map (map->handle, LV2_ATOM__Blank);
	self->atom_Object    = map->map (map->handle, LV2_ATOM__Object);
	self->atom_Path      = map->map (map->handle, LV2_ATOM__Path);
	self->atom_URID      = map->map (map->handle, LV2_ATOM__URID);
	self->midi_MidiEvent = map->map (map->handle, LV2_MIDI__MidiEvent);
	self->patch_Get      = map->map (map->handle, LV2_PATCH__Get);
	self->patch_Set      = map->map (map->handle, LV2_PATCH__Set);
	self->patch_property = map->map (map->handle, LV2_PATCH__property);
	self->patch_value    = map->map (map->handle, LV2_PATCH__value);
	self->as_sample_file = map->map (map->handle, ASAMPLER_FILE_URI);

	lv2_atom_forge_init (&self->forge, map);
	return (LV2_Handle)self;
}

static void
connect_port (LV2_Handle instance, uint32_t port, void* data)
{
	ASampler* self = (ASampler*)instance;
	switch ((PortIndex)port) {
	case AS_CONTROL:     self->control      = (const LV2_Atom_Sequence*)data; break;
	case AS_NOTIFY:      self->notify       = (LV2_Atom_Sequence*)data;       break;
	case AS_OUT_L:       self->out_l        = (float*)data;                   break;
	case AS_OUT_R:       self->out_r        = (float*)data;                   break;
	case AS_START:       self->p_start      = (const float*)data;             break;
	case AS_END:         self->p_end        = (const float*)data;             break;
	case AS_ATTACK:      self->p_attack     = (const float*)data;             break;
	case AS_DECAY:       self->p_decay      = (const float*)data;             break;
	case AS_SUSTAIN:     self->p_sustain    = (const float*)data;             break;
	case AS_RELEASE:     self->p_release    = (const float*)data;             break;
	case AS_GATE:        self->p_gate       = (const float*)data;             break;
	case AS_STOP:        self->p_stop       = (const float*)data;             break;
	case AS_PREV_SAMPLE: self->p_prev       = (const float*)data;             break;
	case AS_NEXT_SAMPLE: self->p_next       = (const float*)data;             break;
	case AS_GAIN:         self->p_gain         = (const float*)data;             break;
	case AS_HP_FREQ:      self->p_hp_freq       = (const float*)data;             break;
	case AS_LP_FREQ:      self->p_lp_freq       = (const float*)data;             break;
	case AS_DIST_DRIVE:   self->p_dist_drive    = (const float*)data;             break;
	case AS_DIST_TYPE:    self->p_dist_type     = (const float*)data;             break;
	case AS_PITCH_ENABLE: self->p_pitch_enable  = (const float*)data;             break;
	case AS_ROOT_NOTE:    self->p_root_note     = (const float*)data;             break;
	case AS_LEGATO:       self->p_legato        = (const float*)data;             break;
	}
}

static void
activate (LV2_Handle instance)
{
	ASampler* self = (ASampler*)instance;
	memset (self->voices, 0, sizeof (self->voices));
	self->inform_ui  = true;
	self->prev_stop  = 0.f;
	self->prev_prev  = 0.f;
	self->prev_next  = 0.f;
	/* Zero SVF filter state */
	self->hp_ic1eq_l = self->hp_ic1eq_r = 0.f;
	self->hp_ic2eq_l = self->hp_ic2eq_r = 0.f;
	self->lp_ic1eq_l = self->lp_ic1eq_r = 0.f;
	self->lp_ic2eq_l = self->lp_ic2eq_r = 0.f;
}

static void
run (LV2_Handle instance, uint32_t n_samples)
{
	ASampler* self = (ASampler*)instance;

	/* Swap in freshly-loaded sample buffer from worker */
	if (self->pending_data) {
		free (self->sample_data);
		self->sample_data     = self->pending_data;
		self->sample_frames   = self->pending_frames;
		self->sample_channels = self->pending_channels;
		self->pending_data    = NULL;
		/* Update current path if a browse result set a new path */
		if (self->pending_path[0] != '\0') {
			strncpy (self->current_path, self->pending_path, sizeof (self->current_path) - 1);
			self->current_path[sizeof (self->current_path) - 1] = '\0';
			self->pending_path[0] = '\0';
		}
		memset (self->voices, 0, sizeof (self->voices));
		self->inform_ui   = true;
		self->need_expose = true;
	}

	/* Check Stop trigger (rising edge) */
	float cur_stop = self->p_stop ? *self->p_stop : 0.f;
	if (cur_stop > 0.5f && self->prev_stop <= 0.5f) {
		stop_all (self);
	}
	self->prev_stop = cur_stop;

	/* Check Prev Sample trigger (rising edge) */
	float cur_prev = self->p_prev ? *self->p_prev : 0.f;
	if (cur_prev > 0.5f && self->prev_prev <= 0.5f) {
		if (self->current_path[0] != '\0') {
			MsgBrowse msg;
			msg.type      = MSG_BROWSE;
			msg.direction = -1;
			strncpy (msg.path, self->current_path, sizeof (msg.path) - 1);
			msg.path[sizeof (msg.path) - 1] = '\0';
			self->schedule->schedule_work (self->schedule->handle, sizeof (msg), &msg);
		}
	}
	self->prev_prev = cur_prev;

	/* Check Next Sample trigger (rising edge) */
	float cur_next = self->p_next ? *self->p_next : 0.f;
	if (cur_next > 0.5f && self->prev_next <= 0.5f) {
		if (self->current_path[0] != '\0') {
			MsgBrowse msg;
			msg.type      = MSG_BROWSE;
			msg.direction = +1;
			strncpy (msg.path, self->current_path, sizeof (msg.path) - 1);
			msg.path[sizeof (msg.path) - 1] = '\0';
			self->schedule->schedule_work (self->schedule->handle, sizeof (msg), &msg);
		}
	}
	self->prev_next = cur_next;

	/* Set up notify forge */
	const uint32_t capacity = self->notify->atom.size;
	lv2_atom_forge_set_buffer (&self->forge, (uint8_t*)self->notify, capacity);
	lv2_atom_forge_sequence_head (&self->forge, &self->forge_frame, 0);

	/* Reply to UI with current file path if needed */
	if (self->inform_ui) {
		self->inform_ui = false;
		inform_ui (self);
	}

	/* Clear audio output */
	memset (self->out_l, 0, n_samples * sizeof (float));
	memset (self->out_r, 0, n_samples * sizeof (float));

	/* Process incoming control / MIDI events */
	LV2_ATOM_SEQUENCE_FOREACH (self->control, ev) {
		const LV2_Atom_Object* obj = (const LV2_Atom_Object*)&ev->body;

		if (ev->body.type == self->atom_Blank ||
		    ev->body.type == self->atom_Object) {
			if (obj->body.otype == self->patch_Get) {
				inform_ui (self);
			} else if (obj->body.otype == self->patch_Set) {
				const LV2_Atom* file_path = parse_patch_set (self, obj);
				if (file_path) {
					const char* fn = (const char*)(file_path + 1);
					strncpy (self->current_path, fn, sizeof (self->current_path) - 1);
					self->current_path[sizeof (self->current_path) - 1] = '\0';
					MsgLoad msg;
					msg.type = MSG_LOAD_FILE;
					strncpy (msg.path, self->current_path, sizeof (msg.path) - 1);
					msg.path[sizeof (msg.path) - 1] = '\0';
					self->schedule->schedule_work (self->schedule->handle, sizeof (msg), &msg);
				}
			}
		} else if (ev->body.type == self->midi_MidiEvent) {
			const uint8_t* midi = (const uint8_t*)(ev + 1);
			uint8_t        type = midi[0] & 0xf0;
			if (type == 0x90 && midi[2] > 0) {
				note_on (self, (int)midi[1], (int)midi[2]);
			} else if (type == 0x80 || (type == 0x90 && midi[2] == 0)) {
				note_off_gate (self);
			} else if (type == 0xb0) {
				if (midi[1] == 123 || midi[1] == 120) {
					/* All Notes Off / All Sound Off */
					stop_all (self);
				}
			}
		}
	}

	/* Synthesise voices */
	for (uint32_t s = 0; s < n_samples; ++s) {
		float L = 0.f, R = 0.f;
		for (int vi = 0; vi < MAX_VOICES; ++vi) {
			Voice* v = &self->voices[vi];
			if (!v->active) continue;
			float amp = adsr_tick (self, v);
			if (!v->active || !self->sample_data) continue;

			/* Linear interpolation for non-integer read positions */
			uint32_t pos0 = (uint32_t)v->read_pos;
			if (pos0 >= self->sample_frames) {
				v->env_state         = ENV_RELEASE;
				v->env_release_level = v->env_level;
				continue;
			}
			uint32_t pos1 = pos0 + 1;
			if (pos1 >= self->sample_frames) pos1 = self->sample_frames - 1;
			float frac = (float)(v->read_pos - (double)pos0);

			float sl, sr, sl1, sr1;
			if (self->sample_channels == 1) {
				sl  = sr  = self->sample_data[pos0];
				sl1 = sr1 = self->sample_data[pos1];
			} else {
				sl  = self->sample_data[pos0 * 2 + 0];
				sr  = self->sample_data[pos0 * 2 + 1];
				sl1 = self->sample_data[pos1 * 2 + 0];
				sr1 = self->sample_data[pos1 * 2 + 1];
			}
			/* Linear interpolation */
			L += (sl + frac * (sl1 - sl)) * amp;
			R += (sr + frac * (sr1 - sr)) * amp;

			v->read_pos += v->read_step;
			if (v->read_pos >= v->read_end) {
				/* Reached the user-defined End marker */
				v->env_state         = ENV_RELEASE;
				v->env_release_level = v->env_level;
			}
		}
		self->out_l[s] = L;
		self->out_r[s] = R;
	}

	/* ----------------------------------------------------------------
	 * FX chain: gain -> HP filter -> LP filter -> distortion
	 * ---------------------------------------------------------------- */

	/* 1. Gain */
	if (self->p_gain) {
		float gain_lin = powf (10.f, (*self->p_gain) / 20.f);
		for (uint32_t s = 0; s < n_samples; ++s) {
			self->out_l[s] *= gain_lin;
			self->out_r[s] *= gain_lin;
		}
	}

	/* 2. Highpass filter */
	if (self->p_hp_freq) {
		float hp_freq = *self->p_hp_freq;
		/* Only apply when meaningfully above 20 Hz floor */
		if (hp_freq > 21.f) {
			float g = tanf ((float)M_PI * hp_freq / (float)self->sample_rate);
			for (uint32_t s = 0; s < n_samples; ++s) {
				self->out_l[s] = svf_hp (&self->hp_ic1eq_l, &self->hp_ic2eq_l, g, self->out_l[s]);
				self->out_r[s] = svf_hp (&self->hp_ic1eq_r, &self->hp_ic2eq_r, g, self->out_r[s]);
			}
		}
	}

	/* 3. Lowpass filter */
	if (self->p_lp_freq) {
		float lp_freq = *self->p_lp_freq;
		/* Only apply when meaningfully below 20 kHz ceiling */
		if (lp_freq < 19999.f) {
			float g = tanf ((float)M_PI * lp_freq / (float)self->sample_rate);
			for (uint32_t s = 0; s < n_samples; ++s) {
				self->out_l[s] = svf_lp (&self->lp_ic1eq_l, &self->lp_ic2eq_l, g, self->out_l[s]);
				self->out_r[s] = svf_lp (&self->lp_ic1eq_r, &self->lp_ic2eq_r, g, self->out_r[s]);
			}
		}
	}

	/* 4. Distortion */
	if (self->p_dist_drive && self->p_dist_type) {
		float drive = *self->p_dist_drive;
		int   dtype = (int)(*self->p_dist_type + 0.5f);
		if (dtype != 0 && drive > 0.001f) {
			for (uint32_t s = 0; s < n_samples; ++s) {
				float l = self->out_l[s];
				float r = self->out_r[s];
				switch (dtype) {
				case 1: /* Soft Clip */
					l = dist_soft_clip (l, drive);
					r = dist_soft_clip (r, drive);
					break;
				case 2: /* Hard Clip */
					l = dist_hard_clip (l, drive);
					r = dist_hard_clip (r, drive);
					break;
				case 3: /* Fold */
					l = dist_fold (l, drive);
					r = dist_fold (r, drive);
					break;
				case 4: /* Bit Crush */
					l = dist_bitcrush (l, drive);
					r = dist_bitcrush (r, drive);
					break;
				default:
					break;
				}
				self->out_l[s] = l;
				self->out_r[s] = r;
			}
		}
	}

	lv2_atom_forge_pop (&self->forge, &self->forge_frame);

	if (self->need_expose && self->queue_draw) {
		self->need_expose = false;
		self->queue_draw->queue_draw (self->queue_draw->handle);
	}
}

static void
deactivate (LV2_Handle instance)
{
	ASampler* self = (ASampler*)instance;
	stop_all (self);
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

/* Load audio from path, fill in resp.  Returns 0 on success. */
static int
load_audio_file (const char* path, MsgReady* resp)
{
	SF_INFO info;
	memset (&info, 0, sizeof (info));
	SNDFILE* sf = sf_open (path, SFM_READ, &info);
	if (!sf) return -1;

	uint32_t n_ch = (uint32_t)(info.channels > 2 ? 2 : info.channels);

	float* raw = (float*)malloc (sizeof (float) * (uint32_t)info.channels * (uint32_t)info.frames);
	if (!raw) { sf_close (sf); return -1; }

	sf_count_t got = sf_readf_float (sf, raw, info.frames);
	sf_close (sf);

	if (got <= 0) { free (raw); return -1; }

	float* buf = (float*)malloc (sizeof (float) * n_ch * (uint32_t)got);
	if (!buf) { free (raw); return -1; }

	for (uint32_t f = 0; f < (uint32_t)got; ++f) {
		if (n_ch == 1) {
			buf[f] = raw[f * (uint32_t)info.channels];
		} else {
			buf[f * 2 + 0] = raw[f * (uint32_t)info.channels + 0];
			buf[f * 2 + 1] = raw[f * (uint32_t)info.channels + 1];
		}
	}
	free (raw);

	resp->data       = buf;
	resp->n_frames   = (uint32_t)got;
	resp->n_channels = n_ch;
	return 0;
}

static LV2_Worker_Status
work (LV2_Handle                  instance,
      LV2_Worker_Respond_Function respond,
      LV2_Worker_Respond_Handle   handle,
      uint32_t                    size,
      const void*                 data)
{
	ASampler*      self = (ASampler*)instance;
	const int*     type = (const int*)data;
	(void)size;

	if (*type == MSG_LOAD_FILE) {
		const MsgLoad* msg = (const MsgLoad*)data;
		MsgReady resp;
		memset (&resp, 0, sizeof (resp));
		resp.type = MSG_FILE_READY;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-truncation"
		strncpy (resp.new_path, msg->path, sizeof (resp.new_path) - 1);
#pragma GCC diagnostic pop

		if (load_audio_file (msg->path, &resp) != 0) {
			lv2_log_error (&self->logger, "a-sampler: cannot open '%s'\n", msg->path);
			return LV2_WORKER_ERR_UNKNOWN;
		}
		respond (handle, sizeof (resp), &resp);
		return LV2_WORKER_SUCCESS;

	} else if (*type == MSG_BROWSE) {
		const MsgBrowse* msg = (const MsgBrowse*)data;

		/* Split current path into directory + basename */
		char dir[4096];
		strncpy (dir, msg->path, sizeof (dir) - 1);
		dir[sizeof (dir) - 1] = '\0';
		char* slash = strrchr (dir, '/');
		if (!slash) return LV2_WORKER_ERR_UNKNOWN; /* no directory info */
		*slash = '\0'; /* dir now holds the directory, slash+1 = current basename */
		const char* cur_name = slash + 1;

		/* Collect audio files in directory */
		DIR* dp = opendir (dir);
		if (!dp) return LV2_WORKER_ERR_UNKNOWN;

		char** names  = NULL;
		int    n_names = 0;
		int    cap     = 0;

		struct dirent* de;
		while ((de = readdir (dp)) != NULL) {
			if (de->d_name[0] == '.') continue;
			if (!is_audio_file (de->d_name)) continue;
			if (n_names >= cap) {
				int new_cap = cap ? cap * 2 : 64;
				char** tmp = (char**)realloc (names, (size_t)new_cap * sizeof (char*));
				if (!tmp) break;
				names = tmp;
				cap   = new_cap;
			}
			names[n_names++] = strdup (de->d_name);
		}
		closedir (dp);

		if (n_names == 0) {
			/* free whatever we got */
			for (int i = 0; i < n_names; ++i) free (names[i]);
			free (names);
			return LV2_WORKER_ERR_UNKNOWN;
		}

		/* Sort alphabetically */
		qsort (names, (size_t)n_names, sizeof (char*), cmp_str);

		/* Find the current file */
		int cur_idx = -1;
		for (int i = 0; i < n_names; ++i) {
			if (!strcmp (names[i], cur_name)) { cur_idx = i; break; }
		}
		if (cur_idx < 0) cur_idx = 0; /* current not found — start from beginning */

		/* Step */
		int new_idx = cur_idx + msg->direction;
		/* Wrap around */
		if (new_idx < 0)        new_idx = n_names - 1;
		if (new_idx >= n_names) new_idx = 0;

		/* Build full path of new file */
		char new_path[4096];
		/* Use strncat to avoid snprintf format-truncation warning */
		new_path[0] = '\0';
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-truncation"
		strncat (new_path, dir, sizeof (new_path) - 2);
		strncat (new_path, "/", 1);
		strncat (new_path, names[new_idx], sizeof (new_path) - strlen (new_path) - 1);
#pragma GCC diagnostic pop

		/* Free name list */
		for (int i = 0; i < n_names; ++i) free (names[i]);
		free (names);

		/* Load the new file */
		MsgReady resp;
		memset (&resp, 0, sizeof (resp));
		resp.type = MSG_FILE_READY;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-truncation"
		strncpy (resp.new_path, new_path, sizeof (resp.new_path) - 1);
#pragma GCC diagnostic pop

		if (load_audio_file (new_path, &resp) != 0) {
			lv2_log_error (&self->logger, "a-sampler: cannot browse to '%s'\n", new_path);
			return LV2_WORKER_ERR_UNKNOWN;
		}
		respond (handle, sizeof (resp), &resp);
		return LV2_WORKER_SUCCESS;
	}

	return LV2_WORKER_ERR_UNKNOWN;
}

static LV2_Worker_Status
work_response (LV2_Handle  instance,
               uint32_t    size,
               const void* data)
{
	ASampler*       self = (ASampler*)instance;
	const MsgReady* resp = (const MsgReady*)data;
	(void)size;
	if (resp->type != MSG_FILE_READY) return LV2_WORKER_ERR_UNKNOWN;
	free (self->pending_data);
	self->pending_data     = resp->data;
	self->pending_frames   = resp->n_frames;
	self->pending_channels = resp->n_channels;
	/* Store the new path so run() can update current_path */
	strncpy (self->pending_path, resp->new_path, sizeof (self->pending_path) - 1);
	self->pending_path[sizeof (self->pending_path) - 1] = '\0';
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
	(void)flags;
	if (self->current_path[0] == '\0') return LV2_STATE_SUCCESS;

	LV2_State_Map_Path* map_path = NULL;
	for (int i = 0; features[i]; ++i) {
		if (!strcmp (features[i]->URI, LV2_STATE__mapPath))
			map_path = (LV2_State_Map_Path*)features[i]->data;
	}
	if (!map_path) return LV2_STATE_ERR_NO_FEATURE;

	char* apath = map_path->abstract_path (map_path->handle, self->current_path);
	store (handle, self->as_sample_file,
	       apath, strlen (apath) + 1,
	       self->atom_Path, LV2_STATE_IS_POD);
	free (apath);
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
	(void)flags;

	LV2_State_Map_Path* map_path = NULL;
	for (int i = 0; features[i]; ++i) {
		if (!strcmp (features[i]->URI, LV2_STATE__mapPath))
			map_path = (LV2_State_Map_Path*)features[i]->data;
	}

	size_t   sz     = 0;
	uint32_t type   = 0;
	uint32_t fflags = 0;
	const void* val = retrieve (handle, self->as_sample_file, &sz, &type, &fflags);
	if (!val || sz == 0) return LV2_STATE_SUCCESS;

	const char* abstract = (const char*)val;
	char*       mapped   = NULL;
	if (map_path)
		mapped = map_path->absolute_path (map_path->handle, abstract);

	const char* path = mapped ? mapped : abstract;
	strncpy (self->current_path, path, sizeof (self->current_path) - 1);
	self->current_path[sizeof (self->current_path) - 1] = '\0';
	free (mapped);

	MsgLoad msg;
	msg.type = MSG_LOAD_FILE;
	strncpy (msg.path, self->current_path, sizeof (msg.path) - 1);
	msg.path[sizeof (msg.path) - 1] = '\0';
	self->schedule->schedule_work (self->schedule->handle, sizeof (msg), &msg);
	return LV2_STATE_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* LV2 Inline Display (Cairo waveform)                                  */
/* ------------------------------------------------------------------ */

static LV2_Inline_Display_Image_Surface*
render_inline (LV2_Handle instance, uint32_t w, uint32_t max_h)
{
	ASampler* self = (ASampler*)instance;

	/* Always render at a fixed height so the widget stays visible for drops */
	uint32_t h = w / 3;
	if (h < 60)    h = 60;
	if (h > max_h) h = max_h;

	/* (Re)allocate surface if size changed */
	if (!self->display
	    || self->display->width  != (int)w
	    || self->display->height != (int)h) {
		if (self->display) { free (self->display->data); free (self->display); }
		self->display = (LV2_Inline_Display_Image_Surface*)calloc (1, sizeof (LV2_Inline_Display_Image_Surface));
		if (!self->display) return NULL;
		self->display->data = (unsigned char*)malloc (4 * w * h);
		if (!self->display->data) { free (self->display); self->display = NULL; return NULL; }
		self->display->width  = (int)w;
		self->display->height = (int)h;
		self->display->stride = (int)(4 * w);
	}

	cairo_surface_t* surface = cairo_image_surface_create_for_data (
		self->display->data, CAIRO_FORMAT_ARGB32, (int)w, (int)h, (int)(4 * w));
	cairo_t* cr = cairo_create (surface);

	/* Background gradient */
	{
		cairo_pattern_t* pat = cairo_pattern_create_linear (0, 0, 0, h);
		cairo_pattern_add_color_stop_rgb (pat, 0.0, 0.18, 0.18, 0.18);
		cairo_pattern_add_color_stop_rgb (pat, 1.0, 0.10, 0.10, 0.10);
		cairo_set_source (cr, pat);
		cairo_paint (cr);
		cairo_pattern_destroy (pat);
	}

	if (!self->sample_data || self->sample_frames == 0) {
		/* Placeholder: dashed border + down-arrow + label */
		cairo_set_source_rgba (cr, 0.55, 0.55, 0.55, 0.6);
		double dash[] = { 4.0, 4.0 };
		cairo_set_dash (cr, dash, 2, 0);
		cairo_set_line_width (cr, 1.5);
		cairo_rectangle (cr, 4, 4, w - 8, h - 8);
		cairo_stroke (cr);
		cairo_set_dash (cr, NULL, 0, 0);

		/* Down-arrow icon */
		cairo_set_source_rgba (cr, 0.6, 0.6, 0.6, 0.8);
		double cx = w / 2.0;
		double cy = h / 2.0 - 10;
		cairo_move_to (cr, cx - 10, cy - 6);
		cairo_line_to (cr, cx + 10, cy - 6);
		cairo_line_to (cr, cx + 10, cy + 2);
		cairo_line_to (cr, cx + 17, cy + 2);
		cairo_line_to (cr, cx,      cy + 14);
		cairo_line_to (cr, cx - 17, cy + 2);
		cairo_line_to (cr, cx - 10, cy + 2);
		cairo_close_path (cr);
		cairo_fill (cr);

		cairo_set_source_rgba (cr, 0.7, 0.7, 0.7, 0.9);
		cairo_set_font_size (cr, 10.5);
		cairo_text_extents_t te;
		const char* label = "Drop audio file here";
		cairo_text_extents (cr, label, &te);
		cairo_move_to (cr, cx - te.width / 2.0 - te.x_bearing, h / 2.0 + 16);
		cairo_show_text (cr, label);
	} else {
		float*   data  = self->sample_data;
		uint32_t total = self->sample_frames;
		uint32_t ch    = self->sample_channels;
		float    sn    = *self->p_start;
		float    en    = *self->p_end;
		if (sn < 0.f) sn = 0.f;
		if (en > 1.f) en = 1.f;
		if (sn > en) { float t = sn; sn = en; en = t; }

		/* Shade regions outside start..end */
		cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 0.55);
		if (sn > 0.f) {
			cairo_rectangle (cr, 0, 0, (double)w * sn, h);
			cairo_fill (cr);
		}
		if (en < 1.f) {
			cairo_rectangle (cr, (double)w * en, 0, (double)w * (1.0 - en) + 1, h);
			cairo_fill (cr);
		}

		/* Waveform columns */
		cairo_set_source_rgba (cr, 0.25, 0.75, 0.35, 0.9);
		cairo_set_line_width (cr, 1.0);
		double mid  = h / 2.0;
		double gain = mid * 0.88;

		for (uint32_t x = 0; x < w; ++x) {
			uint32_t i0 = (uint32_t)((double)x       / w * total);
			uint32_t i1 = (uint32_t)((double)(x + 1) / w * total);
			if (i1 > total) i1 = total;
			if (i0 >= i1)   i1 = i0 + 1;
			if (i1 > total) i1 = total;
			float mn =  1.f, mx = -1.f;
			for (uint32_t i = i0; i < i1; ++i) {
				float s = (ch == 1) ? data[i] : 0.5f * (data[i * 2] + data[i * 2 + 1]);
				if (s < mn) mn = s;
				if (s > mx) mx = s;
			}
			cairo_move_to (cr, x + 0.5, mid - mx * gain);
			cairo_line_to (cr, x + 0.5, mid - mn * gain);
		}
		cairo_stroke (cr);

		/* Center line */
		cairo_set_source_rgba (cr, 0.3, 0.8, 0.4, 0.3);
		cairo_set_line_width (cr, 0.5);
		cairo_move_to (cr, 0, mid);
		cairo_line_to (cr, w, mid);
		cairo_stroke (cr);

		/* Start marker (green) */
		cairo_set_source_rgba (cr, 0.3, 1.0, 0.3, 0.9);
		cairo_set_line_width (cr, 1.5);
		cairo_move_to (cr, (double)w * sn, 0);
		cairo_line_to (cr, (double)w * sn, h);
		cairo_stroke (cr);

		/* End marker (red) */
		cairo_set_source_rgba (cr, 1.0, 0.35, 0.35, 0.9);
		cairo_set_line_width (cr, 1.5);
		cairo_move_to (cr, (double)w * en, 0);
		cairo_line_to (cr, (double)w * en, h);
		cairo_stroke (cr);

		/* Filename label */
		{
			cairo_set_source_rgba (cr, 0.85, 0.85, 0.85, 0.7);
			cairo_set_font_size (cr, 9.0);
			const char* slash = strrchr (self->current_path, '/');
			const char* fname = slash ? slash + 1 : self->current_path;
			cairo_move_to (cr, 5, h - 5);
			cairo_show_text (cr, fname);
		}
	}

	/* Outer border */
	cairo_set_source_rgba (cr, 0.45, 0.45, 0.45, 1.0);
	cairo_set_line_width (cr, 1.0);
	cairo_set_dash (cr, NULL, 0, 0);
	cairo_rectangle (cr, 0.5, 0.5, w - 1, h - 1);
	cairo_stroke (cr);

	cairo_destroy (cr);
	cairo_surface_destroy (surface);
	return self->display;
}

/* ------------------------------------------------------------------ */
/* Extension data                                                       */
/* ------------------------------------------------------------------ */

static const void*
extension_data (const char* uri)
{
	static const LV2_State_Interface  state  = { state_save, state_restore };
	static const LV2_Worker_Interface worker = { work, work_response, NULL };
	static const LV2_Inline_Display_Interface display = { render_inline };

	if (!strcmp (uri, LV2_STATE__interface))         return &state;
	if (!strcmp (uri, LV2_WORKER__interface))        return &worker;
	if (!strcmp (uri, LV2_INLINEDISPLAY__interface)) return &display;
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

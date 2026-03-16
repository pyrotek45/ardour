/*
 * Copyright (C) 2006-2014 David Robillard <d@drobilla.net>
 * Copyright (C) 2007 Doug McLain <doug@nostar.net>
 * Copyright (C) 2008-2017 Paul Davis <paul@linuxaudiosystems.com>
 * Copyright (C) 2009-2012 Carl Hetherington <carl@carlh.net>
 * Copyright (C) 2016-2017 Robin Gareus <robin@gareus.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "ardour/smf_source.h"
#include "ardour/midi_region.h"

#include "pianoroll.h"
#include "pianoroll_background.h"
#include "midi_view.h"

PianorollMidiBackground::PianorollMidiBackground (ArdourCanvas::Item* parent, Pianoroll& pr)
	: MidiViewBackground (parent, pr)
	, view (nullptr)
	, pianoroll (pr)
	, _width (0)
	, _height (0)
{
}

PianorollMidiBackground::~PianorollMidiBackground ()
{
}

void
PianorollMidiBackground::set_size (int w, int h)
{
	_width = w;

	if (_height > 0 && note_range_set && h > 0) {
		/* Height is changing while a note range is already established.
		 * Maintain the current note_height (pixels-per-note) by expanding or
		 * contracting the visible pitch range symmetrically around its centre.
		 * This means dragging the panel taller reveals more notes rather than
		 * stretching the same notes to fill the new space.
		 */
		int old_nh = note_height ();
		if (old_nh > 0) {
			int center  = (int)_lowest_note + (int)(contents_note_range() / 2);
			int new_range = h / old_nh;
			if (new_range < 12) { new_range = 12; }
			int new_low  = center - new_range / 2;
			int new_high = new_low + new_range;
			if (new_low < 0)   { new_low = 0;         new_high = new_range; }
			if (new_high > 127){ new_high = 127;       new_low  = std::max (0, 127 - new_range); }
			_height = h;
			apply_note_range ((uint8_t)new_low, (uint8_t)new_high, true,
			                  RangeCanMove (CanMoveTop | CanMoveBottom));
			HeightChanged (); /* EMIT SIGNAL */
			return;
		}
	}

	_height = h;

	update_contents_height ();

	HeightChanged (); /* EMIT SIGNAL */
}

int
PianorollMidiBackground::contents_height() const
{
	return _height;
}

int
PianorollMidiBackground::height() const
{
	return _height;
}

int
PianorollMidiBackground::width() const
{
	return _width;
}

ARDOUR::InstrumentInfo*
PianorollMidiBackground::instrument_info () const
{
	return pianoroll.instrument_info ();
}

uint8_t
PianorollMidiBackground::get_preferred_midi_channel () const
{
	return pianoroll.visible_channel ();
}

void
PianorollMidiBackground::set_note_highlight (uint8_t note)
{
	pianoroll.set_note_highlight (note);
}

void
PianorollMidiBackground::record_layer_check (std::shared_ptr<ARDOUR::Region>, samplepos_t)
{
}

void
PianorollMidiBackground::set_view (MidiView* mv)
{
	view = mv;
}

void
PianorollMidiBackground::apply_note_range_to_children ()
{
	if (view) {
		view->apply_note_range (lowest_note(), highest_note());
	}
}

void
PianorollMidiBackground::display_region (MidiView& mv)
{
	std::shared_ptr<ARDOUR::SMFSource> smf (std::dynamic_pointer_cast<ARDOUR::SMFSource> (mv.midi_region()->source()));
	assert (smf);
	(void) update_data_note_range (smf->model()->lowest_note(), smf->model()->highest_note());
	apply_note_range (smf->model()->lowest_note(), smf->model()->highest_note(), true, RangeCanMove (CanMoveTop|CanMoveBottom));
}

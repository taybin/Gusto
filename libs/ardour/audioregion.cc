/*
    Copyright (C) 2000-2006 Paul Davis

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

*/

#include <cmath>
#include <climits>
#include <cfloat>
#include <algorithm>

#include <set>


#include <glibmm/thread.h>

#include "pbd/basename.h"
#include "pbd/xml++.h"
#include "pbd/stacktrace.h"
#include "pbd/enumwriter.h"
#include "pbd/convert.h"

#include "evoral/Curve.hpp"

#include "ardour/audioregion.h"
#include "ardour/debug.h"
#include "ardour/session.h"
#include "ardour/gain.h"
#include "ardour/dB.h"
#include "ardour/playlist.h"
#include "ardour/audiofilesource.h"
#include "ardour/region_factory.h"
#include "ardour/runtime_functions.h"
#include "ardour/transient_detector.h"

#include "i18n.h"
#include <locale.h>

using namespace std;
using namespace ARDOUR;
using namespace PBD;

namespace ARDOUR {
	namespace Properties {
		PBD::PropertyDescriptor<bool> envelope_active;
		PBD::PropertyDescriptor<bool> default_fade_in;
		PBD::PropertyDescriptor<bool> default_fade_out;
		PBD::PropertyDescriptor<bool> fade_in_active;
		PBD::PropertyDescriptor<bool> fade_out_active;
		PBD::PropertyDescriptor<float> scale_amplitude;
	}
}

void
AudioRegion::make_property_quarks ()
{
	Properties::envelope_active.property_id = g_quark_from_static_string (X_("envelope-active"));
        DEBUG_TRACE (DEBUG::Properties, string_compose ("quark for envelope-active = %1\n", 	Properties::envelope_active.property_id));
	Properties::default_fade_in.property_id = g_quark_from_static_string (X_("default-fade-in"));
        DEBUG_TRACE (DEBUG::Properties, string_compose ("quark for default-fade-in = %1\n", 	Properties::default_fade_in.property_id));
	Properties::default_fade_out.property_id = g_quark_from_static_string (X_("default-fade-out"));
        DEBUG_TRACE (DEBUG::Properties, string_compose ("quark for default-fade-out = %1\n", 	Properties::default_fade_out.property_id));
	Properties::fade_in_active.property_id = g_quark_from_static_string (X_("fade-in-active"));
        DEBUG_TRACE (DEBUG::Properties, string_compose ("quark for fade-in-active = %1\n", 	Properties::fade_in_active.property_id));
	Properties::fade_out_active.property_id = g_quark_from_static_string (X_("fade-out-active"));
        DEBUG_TRACE (DEBUG::Properties, string_compose ("quark for fade-out-active = %1\n", 	Properties::fade_out_active.property_id));
	Properties::scale_amplitude.property_id = g_quark_from_static_string (X_("scale-amplitude"));
        DEBUG_TRACE (DEBUG::Properties, string_compose ("quark for scale-amplitude = %1\n", 	Properties::scale_amplitude.property_id));
}

void
AudioRegion::register_properties ()
{
	/* no need to register parent class properties */

	add_property (_envelope_active);
	add_property (_default_fade_in);
	add_property (_default_fade_out);
	add_property (_fade_in_active);
	add_property (_fade_out_active);
	add_property (_scale_amplitude);
}

#define AUDIOREGION_STATE_DEFAULT \
	_envelope_active (Properties::envelope_active, false) \
	, _default_fade_in (Properties::default_fade_in, true) \
	, _default_fade_out (Properties::default_fade_out, true) \
	, _fade_in_active (Properties::fade_in_active, true) \
	, _fade_out_active (Properties::fade_out_active, true) \
	, _scale_amplitude (Properties::scale_amplitude, 1.0)

#define AUDIOREGION_COPY_STATE(other) \
	 _envelope_active (other->_envelope_active) \
	, _default_fade_in (other->_default_fade_in) \
	, _default_fade_out (other->_default_fade_out) \
        , _fade_in_active (other->_fade_in_active) \
        , _fade_out_active (other->_fade_out_active) \
	, _scale_amplitude (other->_scale_amplitude) 

/* a Session will reset these to its chosen defaults by calling AudioRegion::set_default_fade() */

void
AudioRegion::init ()
{
	register_properties ();

	set_default_fades ();
	set_default_envelope ();

	listen_to_my_curves ();
	connect_to_analysis_changed ();
	connect_to_header_position_offset_changed ();
}

/** Constructor for use by derived types only */
AudioRegion::AudioRegion (Session& s, framepos_t start, framecnt_t len, std::string name)
	: Region (s, start, len, name, DataType::AUDIO)
	, AUDIOREGION_STATE_DEFAULT
	, _automatable (s)
	, _fade_in (new AutomationList(Evoral::Parameter(FadeInAutomation)))
	, _fade_out (new AutomationList(Evoral::Parameter(FadeOutAutomation)))
	, _envelope (new AutomationList(Evoral::Parameter(EnvelopeAutomation)))
	, _fade_in_suspended (0)
	, _fade_out_suspended (0)
{
	init ();
	assert (_sources.size() == _master_sources.size());
}

/** Basic AudioRegion constructor */
AudioRegion::AudioRegion (const SourceList& srcs)
	: Region (srcs)
	, AUDIOREGION_STATE_DEFAULT
	, _automatable(srcs[0]->session())
	, _fade_in (new AutomationList(Evoral::Parameter(FadeInAutomation)))
	, _fade_out (new AutomationList(Evoral::Parameter(FadeOutAutomation)))
	, _envelope (new AutomationList(Evoral::Parameter(EnvelopeAutomation)))
	, _fade_in_suspended (0)
	, _fade_out_suspended (0)
{
	init ();
	assert (_sources.size() == _master_sources.size());
}

AudioRegion::AudioRegion (boost::shared_ptr<const AudioRegion> other, nframes64_t offset, bool offset_relative)
	: Region (other, offset, offset_relative)
	, AUDIOREGION_COPY_STATE (other)
	, _automatable (other->session())
	, _fade_in (new AutomationList (*other->_fade_in))
	, _fade_out (new AutomationList (*other->_fade_out))
	  /* XXX is this guaranteed to work for all values of offset+offset_relative? */
	, _envelope (new AutomationList (*other->_envelope, _start, _start + _length))
	, _fade_in_suspended (0)
	, _fade_out_suspended (0)
{
	/* don't use init here, because we got fade in/out from the other region
	*/
	register_properties ();
	listen_to_my_curves ();
	connect_to_analysis_changed ();
	connect_to_header_position_offset_changed ();

	assert(_type == DataType::AUDIO);
	assert (_sources.size() == _master_sources.size());
}

AudioRegion::AudioRegion (boost::shared_ptr<const AudioRegion> other, const SourceList& srcs)
	: Region (boost::static_pointer_cast<const Region>(other), srcs)
	, AUDIOREGION_COPY_STATE (other)
	, _automatable (other->session())
	, _fade_in (new AutomationList (*other->_fade_in))
	, _fade_out (new AutomationList (*other->_fade_out))
	, _envelope (new AutomationList (*other->_envelope))
	, _fade_in_suspended (0)
	, _fade_out_suspended (0)
{
	/* make-a-sort-of-copy-with-different-sources constructor (used by audio filter) */

	register_properties ();

	listen_to_my_curves ();
	connect_to_analysis_changed ();
	connect_to_header_position_offset_changed ();

	assert (_sources.size() == _master_sources.size());
}

AudioRegion::AudioRegion (SourceList& srcs)
	: Region (srcs)
	, AUDIOREGION_STATE_DEFAULT
	, _automatable(srcs[0]->session())
	, _fade_in (new AutomationList(Evoral::Parameter(FadeInAutomation)))
	, _fade_out (new AutomationList(Evoral::Parameter(FadeOutAutomation)))
	, _envelope (new AutomationList(Evoral::Parameter(EnvelopeAutomation)))
	, _fade_in_suspended (0)
	, _fade_out_suspended (0)
{
	init ();

	assert(_type == DataType::AUDIO);
	assert (_sources.size() == _master_sources.size());
}

AudioRegion::~AudioRegion ()
{
}

void
AudioRegion::post_set ()
{
	if (!_sync_marked) {
		_sync_position = _start;
	}

	/* return to default fades if the existing ones are too long */

	if (_left_of_split) {
		if (_fade_in->back()->when >= _length) {
			set_default_fade_in ();
		} 
		set_default_fade_out ();
		_left_of_split = false;
	}

	if (_right_of_split) {
		if (_fade_out->back()->when >= _length) {
			set_default_fade_out ();
		} 

		set_default_fade_in ();
		_right_of_split = false;
	}

	/* If _length changed, adjust our gain envelope accordingly */
	_envelope->truncate_end (_length);
}

void
AudioRegion::connect_to_analysis_changed ()
{
	for (SourceList::const_iterator i = _sources.begin(); i != _sources.end(); ++i) {
		(*i)->AnalysisChanged.connect_same_thread (*this, boost::bind (&AudioRegion::invalidate_transients, this));
	}
}

void
AudioRegion::connect_to_header_position_offset_changed ()
{
	set<boost::shared_ptr<Source> > unique_srcs;

	for (SourceList::const_iterator i = _sources.begin(); i != _sources.end(); ++i) {

                /* connect only once to HeaderPositionOffsetChanged, even if sources are replicated
                 */

		if (unique_srcs.find (*i) == unique_srcs.end ()) {
			unique_srcs.insert (*i);
			boost::shared_ptr<AudioFileSource> afs = boost::dynamic_pointer_cast<AudioFileSource> (*i);
			if (afs) {
				afs->HeaderPositionOffsetChanged.connect_same_thread (*this, boost::bind (&AudioRegion::source_offset_changed, this));
			}
		}
	}
}

void
AudioRegion::listen_to_my_curves ()
{
	_envelope->StateChanged.connect_same_thread (*this, boost::bind (&AudioRegion::envelope_changed, this));
	_fade_in->StateChanged.connect_same_thread (*this, boost::bind (&AudioRegion::fade_in_changed, this));
	_fade_out->StateChanged.connect_same_thread (*this, boost::bind (&AudioRegion::fade_out_changed, this));
}

void
AudioRegion::set_envelope_active (bool yn)
{
	if (envelope_active() != yn) {
		_envelope_active = yn;
		send_change (PropertyChange (Properties::envelope_active));
	}
}

ARDOUR::nframes_t
AudioRegion::read_peaks (PeakData *buf, nframes_t npeaks, nframes_t offset, nframes_t cnt, uint32_t chan_n, double samples_per_unit) const
{
	if (chan_n >= _sources.size()) {
		return 0;
	}

	if (audio_source(chan_n)->read_peaks (buf, npeaks, offset, cnt, samples_per_unit)) {
		return 0;
	} else {
		if (_scale_amplitude != 1.0f) {
			for (nframes_t n = 0; n < npeaks; ++n) {
				buf[n].max *= _scale_amplitude;
				buf[n].min *= _scale_amplitude;
			}
		}
		return cnt;
	}
}

framecnt_t
AudioRegion::read (Sample* buf, framepos_t timeline_position, framecnt_t cnt, int channel) const
{
	/* raw read, no fades, no gain, nada */
	return _read_at (_sources, _length, buf, 0, 0, _position + timeline_position, cnt, channel, 0, 0, ReadOps (0));
}

framecnt_t
AudioRegion::read_at (Sample *buf, Sample *mixdown_buffer, float *gain_buffer,
		      framepos_t file_position, framecnt_t cnt, uint32_t chan_n,
		      framecnt_t read_frames, framecnt_t skip_frames) const
{
	/* regular diskstream/butler read complete with fades etc */
	return _read_at (_sources, _length, buf, mixdown_buffer, gain_buffer,
			file_position, cnt, chan_n, read_frames, skip_frames, ReadOps (~0));
}

framecnt_t
AudioRegion::master_read_at (Sample *buf, Sample *mixdown_buffer, float *gain_buffer,
			     framepos_t position, framecnt_t cnt, uint32_t chan_n) const
{
	/* do not read gain/scaling/fades and do not count this disk i/o in statistics */

	return _read_at (_master_sources, _master_sources.front()->length(_master_sources.front()->timeline_position()),
			 buf, mixdown_buffer, gain_buffer, position, cnt, chan_n, 0, 0, ReadOps (0));
}

framecnt_t
AudioRegion::_read_at (const SourceList& /*srcs*/, framecnt_t limit,
		       Sample *buf, Sample *mixdown_buffer, float *gain_buffer,
		       framepos_t position, 
		       framecnt_t cnt,
		       uint32_t chan_n,
		       framecnt_t /*read_frames*/,
		       framecnt_t /*skip_frames*/,
		       ReadOps rops) const
{
	frameoffset_t internal_offset;
	frameoffset_t buf_offset;
	framecnt_t to_read;
	bool raw = (rops == ReadOpsNone);

	if (muted() && !raw) {
		return 0; /* read nothing */
	}

	/* precondition: caller has verified that we cover the desired section */

	if (position < _position) {
		internal_offset = 0;
		buf_offset = _position - position;
		cnt -= buf_offset;
	} else {
		internal_offset = position - _position;
		buf_offset = 0;
	}

	if (internal_offset >= limit) {
		return 0; /* read nothing */
	}

	if ((to_read = min (cnt, limit - internal_offset)) == 0) {
		return 0; /* read nothing */
	}

	if (opaque() || raw) {
		/* overwrite whatever is there */
		mixdown_buffer = buf + buf_offset;
	} else {
		mixdown_buffer += buf_offset;
	}

	if (rops & ReadOpsCount) {
		_read_data_count = 0;
	}

	if (chan_n < n_channels()) {

		boost::shared_ptr<AudioSource> src = audio_source(chan_n);
		if (src->read (mixdown_buffer, _start + internal_offset, to_read) != to_read) {
			return 0; /* "read nothing" */
		}

		if (rops & ReadOpsCount) {
			_read_data_count += src->read_data_count();
		}

	} else {

		/* track is N-channel, this region has less channels; silence the ones
		   we don't have.
		*/

		memset (mixdown_buffer, 0, sizeof (Sample) * cnt);
	}

	if (rops & ReadOpsFades) {

		/* fade in */

		if (_fade_in_active && _session.config.get_use_region_fades()) {

			nframes_t fade_in_length = (nframes_t) _fade_in->back()->when;

			/* see if this read is within the fade in */

			if (internal_offset < fade_in_length) {

				nframes_t fi_limit;

				fi_limit = min (to_read, fade_in_length - internal_offset);


				_fade_in->curve().get_vector (internal_offset, internal_offset+fi_limit, gain_buffer, fi_limit);

				for (nframes_t n = 0; n < fi_limit; ++n) {
					mixdown_buffer[n] *= gain_buffer[n];
				}
			}
		}

		/* fade out */

		if (_fade_out_active && _session.config.get_use_region_fades()) {

			/* see if some part of this read is within the fade out */

		/* .................        >|            REGION
		                             limit

                                 {           }            FADE
				             fade_out_length
                                 ^
                                 limit - fade_out_length
                        |--------------|
                        ^internal_offset
                                       ^internal_offset + to_read

				       we need the intersection of [internal_offset,internal_offset+to_read] with
				       [limit - fade_out_length, limit]

		*/


			nframes_t fade_out_length = (nframes_t) _fade_out->back()->when;
			nframes_t fade_interval_start = max(internal_offset, limit-fade_out_length);
			nframes_t fade_interval_end   = min(internal_offset + to_read, limit);

			if (fade_interval_end > fade_interval_start) {
				/* (part of the) the fade out is  in this buffer */

				nframes_t fo_limit = fade_interval_end - fade_interval_start;
				nframes_t curve_offset = fade_interval_start - (limit-fade_out_length);
				nframes_t fade_offset = fade_interval_start - internal_offset;

				_fade_out->curve().get_vector (curve_offset, curve_offset+fo_limit, gain_buffer, fo_limit);

				for (nframes_t n = 0, m = fade_offset; n < fo_limit; ++n, ++m) {
					mixdown_buffer[m] *= gain_buffer[n];
				}
			}

		}
	}

	/* Regular gain curves and scaling */

	if ((rops & ReadOpsOwnAutomation) && envelope_active())  {
		_envelope->curve().get_vector (internal_offset, internal_offset + to_read, gain_buffer, to_read);

		if ((rops & ReadOpsOwnScaling) && _scale_amplitude != 1.0f) {
			for (nframes_t n = 0; n < to_read; ++n) {
				mixdown_buffer[n] *= gain_buffer[n] * _scale_amplitude;
			}
		} else {
			for (nframes_t n = 0; n < to_read; ++n) {
				mixdown_buffer[n] *= gain_buffer[n];
			}
		}
	} else if ((rops & ReadOpsOwnScaling) && _scale_amplitude != 1.0f) {

		// XXX this should be using what in 2.0 would have been:
		// Session::apply_gain_to_buffer (mixdown_buffer, to_read, _scale_amplitude);

		for (nframes_t n = 0; n < to_read; ++n) {
			mixdown_buffer[n] *= _scale_amplitude;
		}
	}

	if (!opaque()) {

		/* gack. the things we do for users.
		 */

		buf += buf_offset;

		for (nframes_t n = 0; n < to_read; ++n) {
			buf[n] += mixdown_buffer[n];
		}
	}

	return to_read;
}

XMLNode&
AudioRegion::state (bool full)
{
	XMLNode& node (Region::state (full));
	XMLNode *child;
	char buf[64];
	LocaleGuard lg (X_("POSIX"));

	snprintf (buf, sizeof (buf), "%u", (uint32_t) _sources.size());
	node.add_property ("channels", buf);

	if (full) {
		Stateful::add_properties (node);
	}

	child = node.add_child ("Envelope");

	if (full) {
		bool default_env = false;

		// If there are only two points, the points are in the start of the region and the end of the region
		// so, if they are both at 1.0f, that means the default region.

		if (_envelope->size() == 2 &&
		    _envelope->front()->value == 1.0f &&
		    _envelope->back()->value==1.0f) {
			if (_envelope->front()->when == 0 && _envelope->back()->when == _length) {
				default_env = true;
			}
		}

		if (default_env) {
			child->add_property ("default", "yes");
		} else {
			child->add_child_nocopy (_envelope->get_state ());
		}

	} else {
		child->add_property ("default", "yes");
	}

	return node;
}

int
AudioRegion::_set_state (const XMLNode& node, int version, PropertyChange& what_changed, bool send)
{
	const XMLNodeList& nlist = node.children();
	const XMLProperty *prop;
	LocaleGuard lg (X_("POSIX"));
	boost::shared_ptr<Playlist> the_playlist (_playlist.lock());	

	suspend_property_changes ();

	if (the_playlist) {
		the_playlist->freeze ();
	}


	/* this will set all our State members and stuff controlled by the Region.
	   It should NOT send any changed signals - that is our responsibility.
	*/

	Region::_set_state (node, version, what_changed, false);

	if ((prop = node.property ("scale-gain")) != 0) {
		float a = atof (prop->value().c_str());
		if (a != _scale_amplitude) {
			_scale_amplitude = a;
			what_changed.add (Properties::scale_amplitude);
		}
	}

	/* Now find envelope description and other related child items */

        _envelope->freeze ();

	for (XMLNodeConstIterator niter = nlist.begin(); niter != nlist.end(); ++niter) {
		XMLNode *child;
		XMLProperty *prop;

		child = (*niter);

		if (child->name() == "Envelope") {

			_envelope->clear ();

			if ((prop = child->property ("default")) != 0 || _envelope->set_state (*child, version)) {
				set_default_envelope ();
			}

			_envelope->set_max_xval (_length);
			_envelope->truncate_end (_length);


		} else if (child->name() == "FadeIn") {

			_fade_in->clear ();

			if ((prop = child->property ("default")) != 0 || (prop = child->property ("steepness")) != 0) {
				set_default_fade_in ();
			} else {
				XMLNode* grandchild = child->child ("AutomationList");
				if (grandchild) {
					_fade_in->set_state (*grandchild, version);
				}
			}

			if ((prop = child->property ("active")) != 0) {
				if (string_is_affirmative (prop->value())) {
					set_fade_in_active (true);
				} else {
					set_fade_in_active (false);
				}
			}

		} else if (child->name() == "FadeOut") {

			_fade_out->clear ();

			if ((prop = child->property ("default")) != 0 || (prop = child->property ("steepness")) != 0) {
				set_default_fade_out ();
			} else {
				XMLNode* grandchild = child->child ("AutomationList");
				if (grandchild) {
					_fade_out->set_state (*grandchild, version);
				}
			}

			if ((prop = child->property ("active")) != 0) {
				if (string_is_affirmative (prop->value())) {
					set_fade_out_active (true);
				} else {
					set_fade_out_active (false);
				}
			}

		}
	}

        _envelope->thaw ();
	resume_property_changes ();
        
	if (send) {
		send_change (what_changed);
	}

	if (the_playlist) {
		the_playlist->thaw ();
	}

	return 0;
}

int
AudioRegion::set_state (const XMLNode& node, int version)
{
	PropertyChange what_changed;
	return _set_state (node, version, what_changed, true);
}

void
AudioRegion::set_fade_in_shape (FadeShape shape)
{
	set_fade_in (shape, (nframes_t) _fade_in->back()->when);
}

void
AudioRegion::set_fade_out_shape (FadeShape shape)
{
	set_fade_out (shape, (nframes_t) _fade_out->back()->when);
}

void
AudioRegion::set_fade_in (boost::shared_ptr<AutomationList> f)
{
        _fade_in->freeze ();
	*_fade_in = *f;
        _fade_in->thaw ();
	
	send_change (PropertyChange (Properties::fade_in));
}

void
AudioRegion::set_fade_in (FadeShape shape, framecnt_t len)
{
        _fade_in->freeze ();
	_fade_in->clear ();

	switch (shape) {
	case Linear:
		_fade_in->fast_simple_add (0.0, 0.0);
		_fade_in->fast_simple_add (len, 1.0);
		break;

	case Fast:
		_fade_in->fast_simple_add (0, 0);
		_fade_in->fast_simple_add (len * 0.389401, 0.0333333);
		_fade_in->fast_simple_add (len * 0.629032, 0.0861111);
		_fade_in->fast_simple_add (len * 0.829493, 0.233333);
		_fade_in->fast_simple_add (len * 0.9447, 0.483333);
		_fade_in->fast_simple_add (len * 0.976959, 0.697222);
		_fade_in->fast_simple_add (len, 1);
		break;

	case Slow:
		_fade_in->fast_simple_add (0, 0);
		_fade_in->fast_simple_add (len * 0.0207373, 0.197222);
		_fade_in->fast_simple_add (len * 0.0645161, 0.525);
		_fade_in->fast_simple_add (len * 0.152074, 0.802778);
		_fade_in->fast_simple_add (len * 0.276498, 0.919444);
		_fade_in->fast_simple_add (len * 0.481567, 0.980556);
		_fade_in->fast_simple_add (len * 0.767281, 1);
		_fade_in->fast_simple_add (len, 1);
		break;

	case LogA:
		_fade_in->fast_simple_add (0, 0);
		_fade_in->fast_simple_add (len * 0.0737327, 0.308333);
		_fade_in->fast_simple_add (len * 0.246544, 0.658333);
		_fade_in->fast_simple_add (len * 0.470046, 0.886111);
		_fade_in->fast_simple_add (len * 0.652074, 0.972222);
		_fade_in->fast_simple_add (len * 0.771889, 0.988889);
		_fade_in->fast_simple_add (len, 1);
		break;

	case LogB:
		_fade_in->fast_simple_add (0, 0);
		_fade_in->fast_simple_add (len * 0.304147, 0.0694444);
		_fade_in->fast_simple_add (len * 0.529954, 0.152778);
		_fade_in->fast_simple_add (len * 0.725806, 0.333333);
		_fade_in->fast_simple_add (len * 0.847926, 0.558333);
		_fade_in->fast_simple_add (len * 0.919355, 0.730556);
		_fade_in->fast_simple_add (len, 1);
		break;
	}

        _fade_in->thaw ();
}

void
AudioRegion::set_fade_out (boost::shared_ptr<AutomationList> f)
{
        _fade_out->freeze ();
	*_fade_out = *f;
        _fade_out->thaw ();

	send_change (PropertyChange (Properties::fade_in));
}

void
AudioRegion::set_fade_out (FadeShape shape, framecnt_t len)
{
        _fade_out->freeze ();
	_fade_out->clear ();

	switch (shape) {
	case Fast:
		_fade_out->fast_simple_add (len * 0, 1);
		_fade_out->fast_simple_add (len * 0.023041, 0.697222);
		_fade_out->fast_simple_add (len * 0.0553,   0.483333);
		_fade_out->fast_simple_add (len * 0.170507, 0.233333);
		_fade_out->fast_simple_add (len * 0.370968, 0.0861111);
		_fade_out->fast_simple_add (len * 0.610599, 0.0333333);
		_fade_out->fast_simple_add (len * 1, 0);
		break;

	case LogA:
		_fade_out->fast_simple_add (len * 0, 1);
		_fade_out->fast_simple_add (len * 0.228111, 0.988889);
		_fade_out->fast_simple_add (len * 0.347926, 0.972222);
		_fade_out->fast_simple_add (len * 0.529954, 0.886111);
		_fade_out->fast_simple_add (len * 0.753456, 0.658333);
		_fade_out->fast_simple_add (len * 0.9262673, 0.308333);
		_fade_out->fast_simple_add (len * 1, 0);
		break;

	case Slow:
		_fade_out->fast_simple_add (len * 0, 1);
		_fade_out->fast_simple_add (len * 0.305556, 1);
		_fade_out->fast_simple_add (len * 0.548611, 0.991736);
		_fade_out->fast_simple_add (len * 0.759259, 0.931129);
		_fade_out->fast_simple_add (len * 0.918981, 0.68595);
		_fade_out->fast_simple_add (len * 0.976852, 0.22865);
		_fade_out->fast_simple_add (len * 1, 0);
		break;

	case LogB:
		_fade_out->fast_simple_add (len * 0, 1);
		_fade_out->fast_simple_add (len * 0.080645, 0.730556);
		_fade_out->fast_simple_add (len * 0.277778, 0.289256);
		_fade_out->fast_simple_add (len * 0.470046, 0.152778);
		_fade_out->fast_simple_add (len * 0.695853, 0.0694444);
		_fade_out->fast_simple_add (len * 1, 0);
		break;

	case Linear:
		_fade_out->fast_simple_add (len * 0, 1);
		_fade_out->fast_simple_add (len * 1, 0);
		break;
	}

        _fade_out->thaw ();
}

void
AudioRegion::set_fade_in_length (framecnt_t len)
{
	if (len > _length) {
		len = _length - 1;
	}

	bool changed = _fade_in->extend_to (len);

	if (changed) {
		_default_fade_in = false;
		send_change (PropertyChange (Properties::fade_in));
	}
}

void
AudioRegion::set_fade_out_length (framecnt_t len)
{
	if (len > _length) {
		len = _length - 1;
	}

	bool changed =	_fade_out->extend_to (len);

	if (changed) {
		_default_fade_out = false;
		send_change (PropertyChange (Properties::fade_out));
	}
}

void
AudioRegion::set_fade_in_active (bool yn)
{
	if (yn == _fade_in_active) {
		return;
	}

	_fade_in_active = yn;
	send_change (PropertyChange (Properties::fade_in_active));
}

void
AudioRegion::set_fade_out_active (bool yn)
{
	if (yn == _fade_out_active) {
		return;
	}
	_fade_out_active = yn;
	send_change (PropertyChange (Properties::fade_out_active));
}

bool
AudioRegion::fade_in_is_default () const
{
	return _fade_in->size() == 2 && _fade_in->front()->when == 0 && _fade_in->back()->when == 64;
}

bool
AudioRegion::fade_out_is_default () const
{
	return _fade_out->size() == 2 && _fade_out->front()->when == 0 && _fade_out->back()->when == 64;
}

void
AudioRegion::set_default_fade_in ()
{
	_fade_in_suspended = 0;
	set_fade_in (Linear, 64);
}

void
AudioRegion::set_default_fade_out ()
{
	_fade_out_suspended = 0;
	set_fade_out (Linear, 64);
}

void
AudioRegion::set_default_fades ()
{
	set_default_fade_in ();
	set_default_fade_out ();
}

void
AudioRegion::set_default_envelope ()
{
        _envelope->freeze ();
	_envelope->clear ();
	_envelope->fast_simple_add (0, 1.0f);
	_envelope->fast_simple_add (_length, 1.0f);
        _envelope->thaw ();
}

void
AudioRegion::recompute_at_end ()
{
	/* our length has changed. recompute a new final point by interpolating
	   based on the the existing curve.
	*/

        _envelope->freeze ();
	_envelope->truncate_end (_length);
	_envelope->set_max_xval (_length);
        _envelope->thaw ();

        if (_left_of_split) {
                set_default_fade_out ();
                _left_of_split = false;
        } else if (_fade_out->back()->when > _length) {
                _fade_out->extend_to (_length);
                send_change (PropertyChange (Properties::fade_out));
        }
        
	if (_fade_in->back()->when > _length) {
		_fade_in->extend_to (_length);
		send_change (PropertyChange (Properties::fade_in));
	}
}

void
AudioRegion::recompute_at_start ()
{
	/* as above, but the shift was from the front */

	_envelope->truncate_start (_length);

        if (_right_of_split) {
                set_default_fade_in ();
                _right_of_split = false;
        } else if (_fade_in->back()->when > _length) {
		_fade_in->extend_to (_length);
		send_change (PropertyChange (Properties::fade_in));
	}

	if (_fade_out->back()->when > _length) {
		_fade_out->extend_to (_length);
		send_change (PropertyChange (Properties::fade_out));
	}
}

int
AudioRegion::separate_by_channel (Session& /*session*/, vector<boost::shared_ptr<Region> >& v) const
{
	SourceList srcs;
	string new_name;
	int n = 0;

	if (_sources.size() < 2) {
		return 0;
	}

	for (SourceList::const_iterator i = _sources.begin(); i != _sources.end(); ++i) {
		srcs.clear ();
		srcs.push_back (*i);

		new_name = _name;

		if (_sources.size() == 2) {
			if (n == 0) {
				new_name += "-L";
			} else {
				new_name += "-R";
			}
		} else {
			new_name += '-';
			new_name += ('0' + n + 1);
		}

		/* create a copy with just one source. prevent if from being thought of as
		   "whole file" even if it covers the entire source file(s).
		 */

		PropertyList plist;
		
		plist.add (Properties::start, _start.val());
		plist.add (Properties::length, _length.val());
		plist.add (Properties::name, new_name);
		plist.add (Properties::layer, _layer.val());

		v.push_back(RegionFactory::create (srcs, plist));
		v.back()->set_whole_file (false);

		++n;
	}

	return 0;
}

framecnt_t
AudioRegion::read_raw_internal (Sample* buf, framepos_t pos, framecnt_t cnt, int channel) const
{
	return audio_source()->read (buf, pos, cnt, channel);
}

int
AudioRegion::exportme (Session& /*session*/, ARDOUR::ExportSpecification& /*spec*/)
{
	// TODO EXPORT
//	const nframes_t blocksize = 4096;
//	nframes_t to_read;
//	int status = -1;
//
//	spec.channels = _sources.size();
//
//	if (spec.prepare (blocksize, session.frame_rate())) {
//		goto out;
//	}
//
//	spec.pos = 0;
//	spec.total_frames = _length;
//
//	while (spec.pos < _length && !spec.stop) {
//
//
//		/* step 1: interleave */
//
//		to_read = min (_length - spec.pos, blocksize);
//
//		if (spec.channels == 1) {
//
//			if (read_raw_internal (spec.dataF, _start + spec.pos, to_read) != to_read) {
//				goto out;
//			}
//
//		} else {
//
//			Sample buf[blocksize];
//
//			for (uint32_t chan = 0; chan < spec.channels; ++chan) {
//
//				if (audio_source(chan)->read (buf, _start + spec.pos, to_read) != to_read) {
//					goto out;
//				}
//
//				for (nframes_t x = 0; x < to_read; ++x) {
//					spec.dataF[chan+(x*spec.channels)] = buf[x];
//				}
//			}
//		}
//
//		if (spec.process (to_read)) {
//			goto out;
//		}
//
//		spec.pos += to_read;
//		spec.progress = (double) spec.pos /_length;
//
//	}
//
//	status = 0;
//
//  out:
//	spec.running = false;
//	spec.status = status;
//	spec.clear();
//
//	return status;
	return 0;
}

void
AudioRegion::set_scale_amplitude (gain_t g)
{
	boost::shared_ptr<Playlist> pl (playlist());

	_scale_amplitude = g;

	/* tell the diskstream we're in */

	if (pl) {
		pl->ContentsChanged();
	}

	/* tell everybody else */

	send_change (PropertyChange (Properties::scale_amplitude));
}

void
AudioRegion::normalize_to (float target_dB)
{
	const framecnt_t blocksize = 64 * 1024;
	Sample buf[blocksize];
	framepos_t fpos;
	framepos_t fend;
	framecnt_t to_read;
	double maxamp = 0;
	gain_t target = dB_to_coefficient (target_dB);

	if (target == 1.0f) {
		/* do not normalize to precisely 1.0 (0 dBFS), to avoid making it appear
		   that we may have clipped.
		*/
		target -= FLT_EPSILON;
	}

	fpos = _start;
	fend = _start + _length;

	/* first pass: find max amplitude */

	while (fpos < fend) {

		uint32_t n;

		to_read = min (fend - fpos, blocksize);

		for (n = 0; n < n_channels(); ++n) {

			/* read it in */

			if (read_raw_internal (buf, fpos, to_read, 0) != to_read) {
				return;
			}

			maxamp = compute_peak (buf, to_read, maxamp);
		}

		fpos += to_read;
	};

	if (maxamp == 0.0f) {
		/* don't even try */
		return;
	}

	if (maxamp == target) {
		/* we can't do anything useful */
		return;
	}

	/* compute scale factor */

	_scale_amplitude = target/maxamp;

	/* tell the diskstream we're in */

	boost::shared_ptr<Playlist> pl (playlist());

	if (pl) {
		pl->ContentsChanged();
	}

	/* tell everybody else */

	send_change (PropertyChange (Properties::scale_amplitude));
}

void
AudioRegion::fade_in_changed ()
{
	send_change (PropertyChange (Properties::fade_in));
}

void
AudioRegion::fade_out_changed ()
{
	send_change (PropertyChange (Properties::fade_out));
}

void
AudioRegion::envelope_changed ()
{
	send_change (PropertyChange (Properties::envelope));
}

void
AudioRegion::suspend_fade_in ()
{
	if (++_fade_in_suspended == 1) {
		if (fade_in_is_default()) {
			set_fade_in_active (false);
		}
	}
}

void
AudioRegion::resume_fade_in ()
{
	if (--_fade_in_suspended == 0 && _fade_in_suspended) {
		set_fade_in_active (true);
	}
}

void
AudioRegion::suspend_fade_out ()
{
	if (++_fade_out_suspended == 1) {
		if (fade_out_is_default()) {
			set_fade_out_active (false);
		}
	}
}

void
AudioRegion::resume_fade_out ()
{
	if (--_fade_out_suspended == 0 &&_fade_out_suspended) {
		set_fade_out_active (true);
	}
}

bool
AudioRegion::speed_mismatch (float sr) const
{
	if (_sources.empty()) {
		/* impossible, but ... */
		return false;
	}

	float fsr = audio_source()->sample_rate();

	return fsr != sr;
}

void
AudioRegion::source_offset_changed ()
{
	/* XXX this fixes a crash that should not occur. It does occur
	   becauses regions are not being deleted when a session
	   is unloaded. That bug must be fixed.
	*/

	if (_sources.empty()) {
		return;
	}

	boost::shared_ptr<AudioFileSource> afs = boost::dynamic_pointer_cast<AudioFileSource>(_sources.front());

	if (afs && afs->destructive()) {
		// set_start (source()->natural_position(), this);
		set_position (source()->natural_position(), this);
	}
}

boost::shared_ptr<AudioSource>
AudioRegion::audio_source (uint32_t n) const
{
	// Guaranteed to succeed (use a static cast for speed?)
	return boost::dynamic_pointer_cast<AudioSource>(source(n));
}

int
AudioRegion::get_transients (AnalysisFeatureList& results, bool force_new)
{
	boost::shared_ptr<Playlist> pl = playlist();

	if (!pl) {
		return -1;
	}

	if (_valid_transients && !force_new) {
		results = _transients;
		return 0;
	}

	SourceList::iterator s;

	for (s = _sources.begin() ; s != _sources.end(); ++s) {
		if (!(*s)->has_been_analysed()) {
			cerr << "For " << name() << " source " << (*s)->name() << " has not been analyzed\n";
			break;
		}
	}

	if (s == _sources.end()) {
		/* all sources are analyzed, merge data from each one */

		for (s = _sources.begin() ; s != _sources.end(); ++s) {

			/* find the set of transients within the bounds of this region */

			AnalysisFeatureList::iterator low = lower_bound ((*s)->transients.begin(),
									 (*s)->transients.end(),
									 _start);

			AnalysisFeatureList::iterator high = upper_bound ((*s)->transients.begin(),
									  (*s)->transients.end(),
									  _start + _length);

			/* and add them */

			results.insert (results.end(), low, high);
		}

		TransientDetector::cleanup_transients (results, pl->session().frame_rate(), 3.0);

		/* translate all transients to current position */

		for (AnalysisFeatureList::iterator x = results.begin(); x != results.end(); ++x) {
			(*x) -= _start;
			(*x) += _position;
		}

		_transients = results;
		_valid_transients = true;

		return 0;
	}

	/* no existing/complete transient info */

        static bool analyse_dialog_shown = false; /* global per instance of Ardour */

	if (!Config->get_auto_analyse_audio()) {
                if (!analyse_dialog_shown) {
                        pl->session().Dialog (_("\
You have requested an operation that requires audio analysis.\n\n       \
You currently have \"auto-analyse-audio\" disabled, which means\n\
that transient data must be generated every time it is required.\n\n\
If you are doing work that will require transient data on a\n\
regular basis, you should probably enable \"auto-analyse-audio\"\n\
+then quit ardour and restart.\n\n\
+This dialog will not display again.  But you may notice a slight delay\n\
+in this and future transient-detection operations.\n\
+"));
                        analyse_dialog_shown = true;
                }
	}

	TransientDetector t (pl->session().frame_rate());
	bool existing_results = !results.empty();

	_transients.clear ();
	_valid_transients = false;

	for (uint32_t i = 0; i < n_channels(); ++i) {

		AnalysisFeatureList these_results;

		t.reset ();

		if (t.run ("", this, i, these_results)) {
			return -1;
		}

		/* translate all transients to give absolute position */

		for (AnalysisFeatureList::iterator i = these_results.begin(); i != these_results.end(); ++i) {
			(*i) += _position;
		}

		/* merge */

		_transients.insert (_transients.end(), these_results.begin(), these_results.end());
	}

	if (!results.empty()) {
		if (existing_results) {

			/* merge our transients into the existing ones, then clean up
			   those.
			*/

			results.insert (results.end(), _transients.begin(), _transients.end());
			TransientDetector::cleanup_transients (results, pl->session().frame_rate(), 3.0);
		}

		/* make sure ours are clean too */

		TransientDetector::cleanup_transients (_transients, pl->session().frame_rate(), 3.0);

	} else {

		TransientDetector::cleanup_transients (_transients, pl->session().frame_rate(), 3.0);
		results = _transients;
	}

	_valid_transients = true;

	return 0;
}

/** Find areas of `silence' within a region.
 *
 *  @param threshold Threshold below which signal is considered silence (as a sample value)
 *  @param min_length Minimum length of silent period to be reported.
 *  @return Silent periods; first of pair is the offset within the region, second is the length of the period
 */

std::list<std::pair<frameoffset_t, framecnt_t> >
AudioRegion::find_silence (Sample threshold, framecnt_t min_length, InterThreadInfo& itt) const
{
	framecnt_t const block_size = 64 * 1024;
	Sample loudest[block_size];
	Sample buf[block_size];

	framepos_t pos = _start;
	framepos_t const end = _start + _length - 1;

	std::list<std::pair<frameoffset_t, framecnt_t> > silent_periods;

	bool in_silence = false;
	frameoffset_t silence_start = 0;
	bool silence;

	while (pos < end && !itt.cancel) {

		/* fill `loudest' with the loudest absolute sample at each instant, across all channels */
		memset (loudest, 0, sizeof (Sample) * block_size);
		for (uint32_t n = 0; n < n_channels(); ++n) {

			read_raw_internal (buf, pos, block_size, n);
			for (framecnt_t i = 0; i < block_size; ++i) {
				loudest[i] = max (loudest[i], abs (buf[i]));
			}
		}

		/* now look for silence */
		for (framecnt_t i = 0; i < block_size; ++i) {
			silence = abs (loudest[i]) < threshold;
			if (silence && !in_silence) {
				/* non-silence to silence */
				in_silence = true;
				silence_start = pos + i;
			} else if (!silence && in_silence) {
				/* silence to non-silence */
				in_silence = false;
				if (pos + i - 1 - silence_start >= min_length) {
					silent_periods.push_back (std::make_pair (silence_start, pos + i - 1));
				}
			}
		}

		pos += block_size;
                itt.progress = (end-pos)/(double)_length;
	}

	if (in_silence && end - 1 - silence_start >= min_length) {
		/* last block was silent, so finish off the last period */
		silent_periods.push_back (std::make_pair (silence_start, end));
	}

        itt.done = true;

	return silent_periods;
}



extern "C" {

	int region_read_peaks_from_c (void *arg, uint32_t npeaks, uint32_t start, uint32_t cnt, intptr_t data, uint32_t n_chan, double samples_per_unit)
{
	return ((AudioRegion *) arg)->read_peaks ((PeakData *) data, (framecnt_t) npeaks, (framepos_t) start, (framecnt_t) cnt, n_chan,samples_per_unit);
}

uint32_t region_length_from_c (void *arg)
{

	return ((AudioRegion *) arg)->length();
}

uint32_t sourcefile_length_from_c (void *arg, double zoom_factor)
{
	return ( (AudioRegion *) arg)->audio_source()->available_peaks (zoom_factor) ;
}

} /* extern "C" */

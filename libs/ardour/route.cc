/*
    Copyright (C) 2000 Paul Davis

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
#include <fstream>
#include <cassert>
#include <algorithm>

#include "pbd/xml++.h"
#include "pbd/enumwriter.h"
#include "pbd/memento_command.h"
#include "pbd/stacktrace.h"
#include "pbd/convert.h"

#include "evoral/Curve.hpp"

#include "ardour/amp.h"
#include "ardour/audio_port.h"
#include "ardour/audioengine.h"
#include "ardour/buffer.h"
#include "ardour/buffer_set.h"
#include "ardour/configuration.h"
#include "ardour/cycle_timer.h"
#include "ardour/debug.h"
#include "ardour/delivery.h"
#include "ardour/dB.h"
#include "ardour/internal_send.h"
#include "ardour/internal_return.h"
#include "ardour/ladspa_plugin.h"
#include "ardour/meter.h"
#include "ardour/mix.h"
#include "ardour/monitor_processor.h"
#include "ardour/panner.h"
#include "ardour/plugin_insert.h"
#include "ardour/port.h"
#include "ardour/port_insert.h"
#include "ardour/processor.h"
#include "ardour/profile.h"
#include "ardour/route.h"
#include "ardour/route_group.h"
#include "ardour/send.h"
#include "ardour/session.h"
#include "ardour/timestamps.h"
#include "ardour/utils.h"
#include "ardour/graph.h"

#include "i18n.h"

using namespace std;
using namespace ARDOUR;
using namespace PBD;

uint32_t Route::order_key_cnt = 0;
PBD::Signal1<void,string const&> Route::SyncOrderKeys;
PBD::Signal0<void> Route::RemoteControlIDChange;

Route::Route (Session& sess, string name, Flag flg, DataType default_type)
	: SessionObject (sess, name)
	, AutomatableControls (sess)
	, GraphNode( sess.route_graph )
        , _active (true)
        , _initial_delay (0)
        , _roll_delay (0)
	, _flags (flg)
        , _pending_declick (true)
        , _meter_point (MeterPostFader)
        , _phase_invert (0)
        , _self_solo (false)
        , _soloed_by_others_upstream (0)
        , _soloed_by_others_downstream (0)
        , _solo_isolated (0)
        , _denormal_protection (false)
        , _recordable (true)
        , _silent (false)
        , _declickable (false)
	, _solo_control (new SoloControllable (X_("solo"), *this))
	, _mute_control (new MuteControllable (X_("mute"), *this))
	, _mute_master (new MuteMaster (sess, name))
        , _have_internal_generator (false)
        , _solo_safe (false)
	, _default_type (default_type)
        , _remote_control_id (0)
        , _in_configure_processors (false)
{
	processor_max_streams.reset();
	order_keys[N_("signal")] = order_key_cnt++;
}

int
Route::init ()
{
	/* add standard controls */

	_solo_control->set_flags (Controllable::Flag (_solo_control->flags() | Controllable::Toggle));
	_mute_control->set_flags (Controllable::Flag (_solo_control->flags() | Controllable::Toggle));
	
	add_control (_solo_control);
	add_control (_mute_control);

	/* input and output objects */

	_input.reset (new IO (_session, _name, IO::Input, _default_type));
	_output.reset (new IO (_session, _name, IO::Output, _default_type));

	_input->changed.connect_same_thread (*this, boost::bind (&Route::input_change_handler, this, _1, _2));
	_output->changed.connect_same_thread (*this, boost::bind (&Route::output_change_handler, this, _1, _2));

	/* add amp processor  */

	_amp.reset (new Amp (_session));
	add_processor (_amp, PostFader);

	/* add standard processors: meter, main outs, monitor out */

	_meter.reset (new PeakMeter (_session));
	_meter->set_display_to_user (false);

	add_processor (_meter, PostFader);

	_main_outs.reset (new Delivery (_session, _output, _mute_master, _name, Delivery::Main));

        add_processor (_main_outs, PostFader);

	if (is_monitor()) {
		/* where we listen to tracks */
		_intreturn.reset (new InternalReturn (_session));
		add_processor (_intreturn, PreFader);

                ProcessorList::iterator i;

                for (i = _processors.begin(); i != _processors.end(); ++i) {
                        if (*i == _intreturn) {
                                ++i;
                                break;
                        }
                }

                /* the thing that provides proper control over a control/monitor/listen bus 
                   (such as per-channel cut, dim, solo, invert, etc).
                   It always goes right after the internal return;
                 */
                _monitor_control.reset (new MonitorProcessor (_session));
                add_processor (_monitor_control, i);

                /* no panning on the monitor main outs */

                _main_outs->panner()->set_bypassed (true);
	}

        if (is_master() || is_monitor() || is_hidden()) {
                _mute_master->set_solo_ignore (true);
        }

	/* now that we have _meter, its safe to connect to this */

	Metering::Meter.connect_same_thread (*this, (boost::bind (&Route::meter, this)));

        return 0;
}

Route::~Route ()
{
	DEBUG_TRACE (DEBUG::Destruction, string_compose ("route %1 destructor\n", _name));

	/* do this early so that we don't get incoming signals as we are going through destruction 
	 */

	drop_connections ();

	/* don't use clear_processors here, as it depends on the session which may
	   be half-destroyed by now 
	*/

	Glib::RWLock::WriterLock lm (_processor_lock);
	for (ProcessorList::iterator i = _processors.begin(); i != _processors.end(); ++i) {
		(*i)->drop_references ();
	}

	_processors.clear ();
}

void
Route::set_remote_control_id (uint32_t id, bool notify_class_listeners)
{
	if (id != _remote_control_id) {
		_remote_control_id = id;
		RemoteControlIDChanged ();
		if (notify_class_listeners) {
			RemoteControlIDChange ();
		}
	}
}

uint32_t
Route::remote_control_id() const
{
	return _remote_control_id;
}

long
Route::order_key (std::string const & name) const
{
	OrderKeys::const_iterator i = order_keys.find (name);
	if (i == order_keys.end()) {
		return -1;
	}

	return i->second;
}

void
Route::set_order_key (std::string const & name, long n)
{
	order_keys[name] = n;

	if (Config->get_sync_all_route_ordering()) {
		for (OrderKeys::iterator x = order_keys.begin(); x != order_keys.end(); ++x) {
			x->second = n;
		}
	}

	_session.set_dirty ();
}

/** Set all order keys to be the same as that for `base', if such a key
 *  exists in this route.
 *  @param base Base key.
 */
void
Route::sync_order_keys (std::string const & base)
{
	if (order_keys.empty()) {
		return;
	}

	OrderKeys::iterator i;
	uint32_t key;

	if ((i = order_keys.find (base)) == order_keys.end()) {
		/* key doesn't exist, use the first existing key (during session initialization) */
		i = order_keys.begin();
		key = i->second;
		++i;
	} else {
		/* key exists - use it and reset all others (actually, itself included) */
		key = i->second;
		i = order_keys.begin();
	}

	for (; i != order_keys.end(); ++i) {
		i->second = key;
	}
}

string
Route::ensure_track_or_route_name(string name, Session &session)
{
	string newname = name;

	while (!session.io_name_is_legal (newname)) {
		newname = bump_name_once (newname, '.');
	}

	return newname;
}


void
Route::inc_gain (gain_t fraction, void *src)
{
	_amp->inc_gain (fraction, src);
}

void
Route::set_gain (gain_t val, void *src)
{
	if (src != 0 && _route_group && src != _route_group && _route_group->is_active() && _route_group->is_gain()) {

		if (_route_group->is_relative()) {

			gain_t usable_gain = _amp->gain();
			if (usable_gain < 0.000001f) {
				usable_gain = 0.000001f;
			}

			gain_t delta = val;
			if (delta < 0.000001f) {
				delta = 0.000001f;
			}

			delta -= usable_gain;

			if (delta == 0.0f)
				return;

			gain_t factor = delta / usable_gain;

			if (factor > 0.0f) {
				factor = _route_group->get_max_factor(factor);
				if (factor == 0.0f) {
					_amp->gain_control()->Changed(); /* EMIT SIGNAL */
					return;
				}
			} else {
				factor = _route_group->get_min_factor(factor);
				if (factor == 0.0f) {
					_amp->gain_control()->Changed(); /* EMIT SIGNAL */
					return;
				}
			}

			_route_group->apply (&Route::inc_gain, factor, _route_group);

		} else {

			_route_group->apply (&Route::set_gain, val, _route_group);
		}

		return;
	}

	if (val == _amp->gain()) {
		return;
	}

	_amp->set_gain (val, src);
}

/** Process this route for one (sub) cycle (process thread)
 *
 * @param bufs Scratch buffers to use for the signal path
 * @param start_frame Initial transport frame
 * @param end_frame Final transport frame
 * @param nframes Number of frames to output (to ports)
 *
 * Note that (end_frame - start_frame) may not be equal to nframes when the
 * transport speed isn't 1.0 (eg varispeed).
 */
void
Route::process_output_buffers (BufferSet& bufs,
			       sframes_t start_frame, sframes_t end_frame, nframes_t nframes,
			       bool /*with_processors*/, int declick)
{
	bool monitor;

	bufs.is_silent (false);

	switch (Config->get_monitoring_model()) {
	case HardwareMonitoring:
	case ExternalMonitoring:
		monitor = !record_enabled() || (_session.config.get_auto_input() && !_session.actively_recording());
		break;
	default:
		monitor = true;
	}

	if (!declick) {
		declick = _pending_declick;
	}

	/* figure out if we're going to use gain automation */
	_amp->setup_gain_automation (start_frame, end_frame, nframes);


	/* tell main outs what to do about monitoring */
	_main_outs->no_outs_cuz_we_no_monitor (!monitor);


	/* -------------------------------------------------------------------------------------------
	   GLOBAL DECLICK (for transport changes etc.)
	   ----------------------------------------------------------------------------------------- */

	if (declick > 0) {
		Amp::apply_gain (bufs, nframes, 0.0, 1.0);
	} else if (declick < 0) {
		Amp::apply_gain (bufs, nframes, 1.0, 0.0);
	}

	_pending_declick = 0;

	/* -------------------------------------------------------------------------------------------
	   DENORMAL CONTROL/PHASE INVERT
	   ----------------------------------------------------------------------------------------- */

	if (_phase_invert) {

		int chn = 0;

		if (_denormal_protection || Config->get_denormal_protection()) {

			for (BufferSet::audio_iterator i = bufs.audio_begin(); i != bufs.audio_end(); ++i, ++chn) {
				Sample* const sp = i->data();

				if (_phase_invert & chn) {
					for (nframes_t nx = 0; nx < nframes; ++nx) {
						sp[nx]  = -sp[nx];
						sp[nx] += 1.0e-27f;
					}
				} else {
					for (nframes_t nx = 0; nx < nframes; ++nx) {
						sp[nx] += 1.0e-27f;
					}
				}
			}

		} else {

			for (BufferSet::audio_iterator i = bufs.audio_begin(); i != bufs.audio_end(); ++i, ++chn) {
				Sample* const sp = i->data();

				if (_phase_invert & (1<<chn)) {
					for (nframes_t nx = 0; nx < nframes; ++nx) {
						sp[nx] = -sp[nx];
					}
				}
			}
		}

	} else {

		if (_denormal_protection || Config->get_denormal_protection()) {

			for (BufferSet::audio_iterator i = bufs.audio_begin(); i != bufs.audio_end(); ++i) {
				Sample* const sp = i->data();
				for (nframes_t nx = 0; nx < nframes; ++nx) {
					sp[nx] += 1.0e-27f;
				}
			}

		}
	}

	/* -------------------------------------------------------------------------------------------
	   and go ....
	   ----------------------------------------------------------------------------------------- */

	for (ProcessorList::iterator i = _processors.begin(); i != _processors.end(); ++i) {

		if (bufs.count() != (*i)->input_streams()) {
			cerr << _name << " bufs = " << bufs.count()
			     << " input for " << (*i)->name() << " = " << (*i)->input_streams()
			     << endl;
		}
		assert (bufs.count() == (*i)->input_streams());
		
		(*i)->run (bufs, start_frame, end_frame, nframes, *i != _processors.back());
		bufs.set_count ((*i)->output_streams());
	}
}

ChanCount
Route::n_process_buffers ()
{
	return max (_input->n_ports(), processor_max_streams);
}

void
Route::passthru (sframes_t start_frame, sframes_t end_frame, nframes_t nframes, int declick)
{
	BufferSet& bufs = _session.get_scratch_buffers (n_process_buffers());

	_silent = false;

	assert (bufs.available() >= input_streams());

	if (_input->n_ports() == ChanCount::ZERO) {
		silence_unlocked (nframes);
	}

	bufs.set_count (input_streams());

	if (is_monitor() && _session.listening() && !_session.is_auditioning()) {

		/* control/monitor bus ignores input ports when something is
		   feeding the listen "stream". data will "arrive" into the
		   route from the intreturn processor element.
		*/
		bufs.silence (nframes, 0);

	} else {

		for (DataType::iterator t = DataType::begin(); t != DataType::end(); ++t) {

			BufferSet::iterator o = bufs.begin(*t);
			PortSet& ports (_input->ports());

			for (PortSet::iterator i = ports.begin(*t); i != ports.end(*t); ++i, ++o) {
				o->read_from (i->get_buffer(nframes), nframes);
			}
		}
	}

	write_out_of_band_data (bufs, start_frame, end_frame, nframes);
	process_output_buffers (bufs, start_frame, end_frame, nframes, true, declick);
}

void
Route::passthru_silence (sframes_t start_frame, sframes_t end_frame, nframes_t nframes, int declick)
{
	BufferSet& bufs (_session.get_silent_buffers (n_process_buffers()));
	bufs.set_count (_input->n_ports());
	write_out_of_band_data (bufs, start_frame, end_frame, nframes);
	process_output_buffers (bufs, start_frame, end_frame, nframes, true, declick);
}

void
Route::set_listen (bool yn, void* src)
{
        if (_solo_safe) {
                return;
        }

	if (_monitor_send) {
		if (yn != _monitor_send->active()) {
			if (yn) {
				_monitor_send->activate ();
                                _mute_master->set_soloed (true);
                        } else {
				_monitor_send->deactivate ();
                                _mute_master->set_soloed (false);
			}

			listen_changed (src); /* EMIT SIGNAL */
		}
	}
}

bool
Route::listening () const
{
	if (_monitor_send) {
		return _monitor_send->active ();
	} else {
		return false;
	}
}

void
Route::set_solo_safe (bool yn, void *src)
{
	if (_solo_safe != yn) {
		_solo_safe = yn;
		solo_safe_changed (src);
	} 
}

bool
Route::solo_safe() const
{
	return _solo_safe;
}

void
Route::set_solo (bool yn, void *src)
{
	if (_solo_safe) {
		return;
	}

	if (_route_group && src != _route_group && _route_group->is_active() && _route_group->is_solo()) {
		_route_group->apply (&Route::set_solo, yn, _route_group);
		return;
	}

	if (self_soloed() != yn) {
		set_self_solo (yn);
                set_mute_master_solo ();
		solo_changed (true, src); /* EMIT SIGNAL */
		_solo_control->Changed (); /* EMIT SIGNAL */
	}
}

void
Route::set_self_solo (bool yn)
{
        _self_solo = yn;
}

void
Route::mod_solo_by_others_upstream (int32_t delta)
{
        if (_solo_safe) {
                return;
        }

        uint32_t old_sbu = _soloed_by_others_upstream;

	if (delta < 0) {
		if (_soloed_by_others_upstream >= (uint32_t) abs (delta)) {
			_soloed_by_others_upstream += delta;
		} else {
			_soloed_by_others_upstream = 0;
		}
	} else {
		_soloed_by_others_upstream += delta;
	}

        DEBUG_TRACE (DEBUG::Solo, string_compose ("%1 SbU delta %2 = %3 old = %4 sbd %5 ss %6 exclusive %7\n",
                                                  name(), delta, _soloed_by_others_upstream, old_sbu, 
                                                  _soloed_by_others_downstream, _self_solo, Config->get_exclusive_solo()));

        /* push the inverse solo change to everything that feeds us. 
           
           This is important for solo-within-group. When we solo 1 track out of N that
           feed a bus, that track will cause mod_solo_by_upstream (+1) to be called
           on the bus. The bus then needs to call mod_solo_by_downstream (-1) on all
           tracks that feed it. This will silence them if they were audible because
           of a bus solo, but the newly soloed track will still be audible (because 
           it is self-soloed).
           
           but .. do this only when we are being told to solo-by-upstream (i.e delta = +1),
           not in reverse.
         */

        if ((_self_solo || _soloed_by_others_downstream) &&
            ((old_sbu == 0 && _soloed_by_others_upstream > 0) || 
             (old_sbu > 0 && _soloed_by_others_upstream == 0))) {
                
                if (delta > 0 || !Config->get_exclusive_solo()) {
                        DEBUG_TRACE (DEBUG::Solo, "\t ... INVERT push\n");
                        for (FedBy::iterator i = _fed_by.begin(); i != _fed_by.end(); ++i) {
                                boost::shared_ptr<Route> sr = i->r.lock();
                                if (sr) {
                                        sr->mod_solo_by_others_downstream (-delta);
                                }
                        }
                } 
        }

        set_mute_master_solo ();
        solo_changed (false, this);
}

void
Route::mod_solo_by_others_downstream (int32_t delta)
{
        if (_solo_safe) {
                return;
        }

        if (delta < 0) {
		if (_soloed_by_others_downstream >= (uint32_t) abs (delta)) {
			_soloed_by_others_downstream += delta;
		} else {
			_soloed_by_others_downstream = 0;
		}
	} else {
		_soloed_by_others_downstream += delta;
	}

        DEBUG_TRACE (DEBUG::Solo, string_compose ("%1 SbD delta %2 = %3\n", name(), delta, _soloed_by_others_downstream));

        set_mute_master_solo ();
        solo_changed (false, this);
}

void
Route::set_mute_master_solo ()
{
        _mute_master->set_soloed (self_soloed() || soloed_by_others_downstream() || soloed_by_others_upstream());
}

void
Route::set_solo_isolated (bool yn, void *src)
{
	if (is_master() || is_monitor() || is_hidden()) {
		return;
	}

	if (_route_group && src != _route_group && _route_group->is_active() && _route_group->is_solo()) {
		_route_group->apply (&Route::set_solo_isolated, yn, _route_group);
		return;
	}
	
	/* forward propagate solo-isolate status to everything fed by this route, but not those via sends only */

	boost::shared_ptr<RouteList> routes = _session.get_routes ();
	for (RouteList::iterator i = routes->begin(); i != routes->end(); ++i) {

		if ((*i).get() == this || (*i)->is_master() || (*i)->is_monitor() || (*i)->is_hidden()) {
                        continue;
                }

		bool sends_only;
		bool does_feed = direct_feeds (*i, &sends_only); // we will recurse anyway, so don't use ::feeds()
		
		if (does_feed && !sends_only) {
			(*i)->set_solo_isolated (yn, (*i)->route_group());
		}
	}

        /* XXX should we back-propagate as well? (April 2010: myself and chris goddard think not) */

        bool changed = false;

	if (yn) {
                if (_solo_isolated == 0) {
                        _mute_master->set_solo_ignore (true);
                        changed = true;
                }
		_solo_isolated++;
	} else {
		if (_solo_isolated > 0) {
			_solo_isolated--;
                        if (_solo_isolated == 0) {
                                _mute_master->set_solo_ignore (false);
                                changed = true;
                        }
		}
	}

        if (changed) {
                solo_isolated_changed (src);
        }
}

bool
Route::solo_isolated () const
{
	return _solo_isolated > 0;
}

void
Route::set_mute_points (MuteMaster::MutePoint mp)
{
        _mute_master->set_mute_points (mp);
        mute_points_changed (); /* EMIT SIGNAL */
        
        if (_mute_master->muted_by_self()) {
                mute_changed (this); /* EMIT SIGNAL */
        }
}

void
Route::set_mute (bool yn, void *src)
{
	if (_route_group && src != _route_group && _route_group->is_active() && _route_group->is_mute()) {
		_route_group->apply (&Route::set_mute, yn, _route_group);
		return;
	}

	if (muted() != yn) {
                _mute_master->set_muted_by_self (yn);
		mute_changed (src); /* EMIT SIGNAL */
	}
}

bool
Route::muted () const
{
        return _mute_master->muted_by_self();
}

#if 0
static void
dump_processors(const string& name, const list<boost::shared_ptr<Processor> >& procs)
{
	cerr << name << " {" << endl;
	for (list<boost::shared_ptr<Processor> >::const_iterator p = procs.begin();
			p != procs.end(); ++p) {
		cerr << "\t" << (*p)->name() << " ID = " << (*p)->id() << endl;
	}
	cerr << "}" << endl;
}
#endif

int
Route::add_processor (boost::shared_ptr<Processor> processor, Placement placement, ProcessorStreams* err)
{
	ProcessorList::iterator loc;

	/* XXX this is not thread safe - we don't hold the lock across determining the iter
	   to add before and actually doing the insertion. dammit.
	*/

	if (placement == PreFader) {
		/* generic pre-fader: insert immediately before the amp */
		loc = find (_processors.begin(), _processors.end(), _amp);
	} else {
		/* generic post-fader: insert right before the main outs */
		loc = find (_processors.begin(), _processors.end(), _main_outs);
	}

	return add_processor (processor, loc, err);
}


/** Add a processor to the route.
 * @a iter must point to an iterator in _processors and the new
 * processor will be inserted immediately before this location.  Otherwise,
 * @a position is used.
 */
int
Route::add_processor (boost::shared_ptr<Processor> processor, ProcessorList::iterator iter, ProcessorStreams* err, bool activation_allowed)
{
	ChanCount old_pms = processor_max_streams;

	if (!_session.engine().connected() || !processor) {
		return 1;
	}

	{
		Glib::RWLock::WriterLock lm (_processor_lock);

		boost::shared_ptr<PluginInsert> pi;
		boost::shared_ptr<PortInsert> porti;

		ProcessorList::iterator loc = find(_processors.begin(), _processors.end(), processor);

		if (processor == _amp || processor == _meter || processor == _main_outs) {
			// Ensure only one of these are in the list at any time
			if (loc != _processors.end()) {
				if (iter == loc) { // Already in place, do nothing
					return 0;
				} else { // New position given, relocate
					_processors.erase (loc);
				}
			}

		} else {
			if (loc != _processors.end()) {
				cerr << "ERROR: Processor added to route twice!" << endl;
				return 1;
			}

			loc = iter;
		}

		_processors.insert (loc, processor);

		// Set up processor list channels.  This will set processor->[input|output]_streams(),
		// configure redirect ports properly, etc.

		if (configure_processors_unlocked (err)) {
			ProcessorList::iterator ploc = loc;
			--ploc;
			_processors.erase(ploc);
			configure_processors_unlocked (0); // it worked before we tried to add it ...
			cerr << "configure failed\n";
			return -1;
		}

		if ((pi = boost::dynamic_pointer_cast<PluginInsert>(processor)) != 0) {

			if (pi->natural_input_streams() == ChanCount::ZERO) {
				/* generator plugin */
				_have_internal_generator = true;
			}

		}

                /* is this the monitor send ? if so, make sure we keep track of it */

                boost::shared_ptr<InternalSend> isend = boost::dynamic_pointer_cast<InternalSend> (processor);

                if (isend && _session.monitor_out() && (isend->target_id() == _session.monitor_out()->id())) {
                        _monitor_send = isend;
                }

		if (activation_allowed && (processor != _monitor_send)) {
			processor->activate ();
		}

		processor->ActiveChanged.connect_same_thread (*this, boost::bind (&Session::update_latency_compensation, &_session, false, false));

		_output->set_user_latency (0);
	}

	processors_changed (RouteProcessorChange ()); /* EMIT SIGNAL */
	set_processor_positions ();

	return 0;
}

bool
Route::add_processor_from_xml_2X (const XMLNode& node, int version, ProcessorList::iterator iter)
{
	const XMLProperty *prop;

	try {
		boost::shared_ptr<Processor> processor;

		if (node.name() == "Insert") {

			if ((prop = node.property ("type")) != 0) {

				if (prop->value() == "ladspa" || prop->value() == "Ladspa" || 
						prop->value() == "lv2" ||
						prop->value() == "vst" ||
						prop->value() == "audiounit") {

					processor.reset (new PluginInsert (_session));

				} else {

					processor.reset (new PortInsert (_session, _mute_master));
				}

			}

		} else if (node.name() == "Send") {

			processor.reset (new Send (_session, _mute_master));

		} else {

			error << string_compose(_("unknown Processor type \"%1\"; ignored"), node.name()) << endmsg;
			return false;
		}

                if (processor->set_state (node, version)) {
                        return false;
                }

		if (iter == _processors.end() && processor->display_to_user() && !_processors.empty()) {
			/* check for invisible processors stacked at the end and leave them there */
			ProcessorList::iterator p;
			p = _processors.end();
			--p;
			while (!(*p)->display_to_user() && p != _processors.begin()) {
				--p;
			}
			++p;
			iter = p;
		}

		return (add_processor (processor, iter) == 0);
	}

	catch (failed_constructor &err) {
		warning << _("processor could not be created. Ignored.") << endmsg;
		return false;
	}
}

int
Route::add_processors (const ProcessorList& others, boost::shared_ptr<Processor> before, ProcessorStreams* err)
{
	ProcessorList::iterator loc;

	if (before) {
		loc = find(_processors.begin(), _processors.end(), before);
	} else {
		/* nothing specified - at end but before main outs */
		loc = find (_processors.begin(), _processors.end(), _main_outs);
	}

	return add_processors (others, loc, err);
}

int
Route::add_processors (const ProcessorList& others, ProcessorList::iterator iter, ProcessorStreams* err)
{
	/* NOTE: this is intended to be used ONLY when copying
	   processors from another Route. Hence the subtle
	   differences between this and ::add_processor()
	*/

	ChanCount old_pms = processor_max_streams;

	if (!_session.engine().connected()) {
		return 1;
	}

	if (others.empty()) {
		return 0;
	}

	{
		Glib::RWLock::WriterLock lm (_processor_lock);

		ChanCount potential_max_streams = ChanCount::max (_input->n_ports(), _output->n_ports());

		for (ProcessorList::const_iterator i = others.begin(); i != others.end(); ++i) {

			// Ensure meter only appears in the list once
			if (*i == _meter) {
				ProcessorList::iterator m = find(_processors.begin(), _processors.end(), *i);
				if (m != _processors.end()) {
					_processors.erase(m);
				}
			}

			boost::shared_ptr<PluginInsert> pi;

			if ((pi = boost::dynamic_pointer_cast<PluginInsert>(*i)) != 0) {
				pi->set_count (1);

				ChanCount m = max (pi->input_streams(), pi->output_streams());

				if (m > potential_max_streams) {
					potential_max_streams = m;
				}
			}

			ProcessorList::iterator inserted = _processors.insert (iter, *i);

			if ((*i)->active()) {
				(*i)->activate ();
			}

			if (configure_processors_unlocked (err)) {
				_processors.erase (inserted);
				configure_processors_unlocked (0); // it worked before we tried to add it ...
				return -1;
			}

			(*i)->ActiveChanged.connect_same_thread (*this, boost::bind (&Session::update_latency_compensation, &_session, false, false));
		}

		_output->set_user_latency (0);
	}

	processors_changed (RouteProcessorChange ()); /* EMIT SIGNAL */
	set_processor_positions ();

	return 0;
}

void
Route::placement_range(Placement p, ProcessorList::iterator& start, ProcessorList::iterator& end)
{
	if (p == PreFader) {
		start = _processors.begin();
		end = find(_processors.begin(), _processors.end(), _amp);
	} else {
		start = find(_processors.begin(), _processors.end(), _amp);
		++start;
		end = _processors.end();
	}
}

/** Turn off all processors with a given placement
 * @param p Placement of processors to disable
 */
void
Route::disable_processors (Placement p)
{
	Glib::RWLock::ReaderLock lm (_processor_lock);

	ProcessorList::iterator start, end;
	placement_range(p, start, end);

	for (ProcessorList::iterator i = start; i != end; ++i) {
		(*i)->deactivate ();
	}

	_session.set_dirty ();
}

/** Turn off all redirects
 */
void
Route::disable_processors ()
{
	Glib::RWLock::ReaderLock lm (_processor_lock);

	for (ProcessorList::iterator i = _processors.begin(); i != _processors.end(); ++i) {
		(*i)->deactivate ();
	}

	_session.set_dirty ();
}

/** Turn off all redirects with a given placement
 * @param p Placement of redirects to disable
 */
void
Route::disable_plugins (Placement p)
{
	Glib::RWLock::ReaderLock lm (_processor_lock);

	ProcessorList::iterator start, end;
	placement_range(p, start, end);

	for (ProcessorList::iterator i = start; i != end; ++i) {
		if (boost::dynamic_pointer_cast<PluginInsert> (*i)) {
			(*i)->deactivate ();
		}
	}

	_session.set_dirty ();
}

/** Turn off all plugins
 */
void
Route::disable_plugins ()
{
	Glib::RWLock::ReaderLock lm (_processor_lock);

	for (ProcessorList::iterator i = _processors.begin(); i != _processors.end(); ++i) {
		if (boost::dynamic_pointer_cast<PluginInsert> (*i)) {
			(*i)->deactivate ();
		}
	}

	_session.set_dirty ();
}


void
Route::ab_plugins (bool forward)
{
	Glib::RWLock::ReaderLock lm (_processor_lock);

	if (forward) {

		/* forward = turn off all active redirects, and mark them so that the next time
		   we go the other way, we will revert them
		*/

		for (ProcessorList::iterator i = _processors.begin(); i != _processors.end(); ++i) {
			if (!boost::dynamic_pointer_cast<PluginInsert> (*i)) {
				continue;
			}

			if ((*i)->active()) {
				(*i)->deactivate ();
				(*i)->set_next_ab_is_active (true);
			} else {
				(*i)->set_next_ab_is_active (false);
			}
		}

	} else {

		/* backward = if the redirect was marked to go active on the next ab, do so */

		for (ProcessorList::iterator i = _processors.begin(); i != _processors.end(); ++i) {

			if (!boost::dynamic_pointer_cast<PluginInsert> (*i)) {
				continue;
			}

			if ((*i)->get_next_ab_is_active()) {
				(*i)->activate ();
			} else {
				(*i)->deactivate ();
			}
		}
	}

	_session.set_dirty ();
}


/** Remove processors with a given placement.
 * @param p Placement of processors to remove.
 */
void
Route::clear_processors (Placement p)
{
	const ChanCount old_pms = processor_max_streams;

	if (!_session.engine().connected()) {
		return;
	}

	bool already_deleting = _session.deletion_in_progress();
	if (!already_deleting) {
		_session.set_deletion_in_progress();
	}

	{
		Glib::RWLock::WriterLock lm (_processor_lock);
		ProcessorList new_list;
		ProcessorStreams err;
		bool seen_amp = false;

		for (ProcessorList::iterator i = _processors.begin(); i != _processors.end(); ++i) {

			if (*i == _amp) {
				seen_amp = true;
			}

			if ((*i) == _amp || (*i) == _meter || (*i) == _main_outs) {

				/* you can't remove these */

				new_list.push_back (*i);

			} else {
				if (seen_amp) {

					switch (p) {
					case PreFader:
						new_list.push_back (*i);
						break;
					case PostFader:
						(*i)->drop_references ();
						break;
					}

				} else {

					switch (p) {
					case PreFader:
						(*i)->drop_references ();
						break;
					case PostFader:
						new_list.push_back (*i);
						break;
					}
				}
			}
		}

		_processors = new_list;
		configure_processors_unlocked (&err); // this can't fail
	}

	processor_max_streams.reset();
	_have_internal_generator = false;
	processors_changed (RouteProcessorChange ()); /* EMIT SIGNAL */
	set_processor_positions ();

	if (!already_deleting) {
		_session.clear_deletion_in_progress();
	}
}

int
Route::remove_processor (boost::shared_ptr<Processor> processor, ProcessorStreams* err)
{
	/* these can never be removed */

	if (processor == _amp || processor == _meter || processor == _main_outs) {
		return 0;
	}

	ChanCount old_pms = processor_max_streams;

	if (!_session.engine().connected()) {
		return 1;
	}

	processor_max_streams.reset();

	{
		Glib::RWLock::WriterLock lm (_processor_lock);
		ProcessorList::iterator i;
		bool removed = false;

		for (i = _processors.begin(); i != _processors.end(); ) {
			if (*i == processor) {

				/* move along, see failure case for configure_processors()
				   where we may need to reconfigure the processor.
				*/

				/* stop redirects that send signals to JACK ports
				   from causing noise as a result of no longer being
				   run.
				*/

				boost::shared_ptr<IOProcessor> iop;

				if ((iop = boost::dynamic_pointer_cast<IOProcessor> (*i)) != 0) {
					if (iop->input()) {
						iop->input()->disconnect (this);
					}
					if (iop->output()) {
						iop->output()->disconnect (this);
					}
				}

				i = _processors.erase (i);
				removed = true;
				break;

			} else {
				++i;
			}

			_output->set_user_latency (0);
		}

		if (!removed) {
			/* what? */
			return 1;
		}

		if (configure_processors_unlocked (err)) {
			/* get back to where we where */
			_processors.insert (i, processor);
			/* we know this will work, because it worked before :) */
			configure_processors_unlocked (0);
			return -1;
		}

		_have_internal_generator = false;

		for (i = _processors.begin(); i != _processors.end(); ++i) {
			boost::shared_ptr<PluginInsert> pi;

			if ((pi = boost::dynamic_pointer_cast<PluginInsert>(*i)) != 0) {
				if (pi->is_generator()) {
					_have_internal_generator = true;
					break;
				}
			}
		}
	}

	processor->drop_references ();
	processors_changed (RouteProcessorChange ()); /* EMIT SIGNAL */
	set_processor_positions ();

	return 0;
}

int
Route::remove_processors (const ProcessorList& to_be_deleted, ProcessorStreams* err)
{
	ProcessorList deleted;

	if (!_session.engine().connected()) {
		return 1;
	}

	processor_max_streams.reset();

	{
		Glib::RWLock::WriterLock lm (_processor_lock);
		ProcessorList::iterator i;
		boost::shared_ptr<Processor> processor;

		ProcessorList as_we_were = _processors;

		for (i = _processors.begin(); i != _processors.end(); ) {

			processor = *i;

			/* these can never be removed */

			if (processor == _amp || processor == _meter || processor == _main_outs) {
				++i;
				continue;
			}

			/* see if its in the list of processors to delete */

			if (find (to_be_deleted.begin(), to_be_deleted.end(), processor) == to_be_deleted.end()) {
				++i;
				continue;
			}

			/* stop IOProcessors that send to JACK ports
			   from causing noise as a result of no longer being
			   run.
			*/

			boost::shared_ptr<IOProcessor> iop;

			if ((iop = boost::dynamic_pointer_cast<IOProcessor> (processor)) != 0) {
				iop->disconnect ();
			}

			deleted.push_back (processor);
			i = _processors.erase (i);
		}

		if (deleted.empty()) {
			/* none of those in the requested list were found */
			return 0;
		}

		_output->set_user_latency (0);

		if (configure_processors_unlocked (err)) {
			/* get back to where we where */
			_processors = as_we_were;
			/* we know this will work, because it worked before :) */
			configure_processors_unlocked (0);
			return -1;
		}

		_have_internal_generator = false;

		for (i = _processors.begin(); i != _processors.end(); ++i) {
			boost::shared_ptr<PluginInsert> pi;

			if ((pi = boost::dynamic_pointer_cast<PluginInsert>(*i)) != 0) {
				if (pi->is_generator()) {
					_have_internal_generator = true;
					break;
				}
			}
		}
	}

	/* now try to do what we need to so that those that were removed will be deleted */

	for (ProcessorList::iterator i = deleted.begin(); i != deleted.end(); ++i) {
		(*i)->drop_references ();
	}

	processors_changed (RouteProcessorChange ()); /* EMIT SIGNAL */
	set_processor_positions ();

	return 0;
}


int
Route::configure_processors (ProcessorStreams* err)
{
	if (!_in_configure_processors) {
		Glib::RWLock::WriterLock lm (_processor_lock);
		return configure_processors_unlocked (err);
	}
	return 0;
}

ChanCount
Route::input_streams () const
{
        return _input->n_ports ();
}

/** Configure the input/output configuration of each processor in the processors list.
 * Return 0 on success, otherwise configuration is impossible.
 */
int
Route::configure_processors_unlocked (ProcessorStreams* err)
{
	if (_in_configure_processors) {
	   return 0;
	}

	_in_configure_processors = true;

	// Check each processor in order to see if we can configure as requested
	ChanCount in = input_streams ();
	ChanCount out;
	list< pair<ChanCount,ChanCount> > configuration;
	uint32_t index = 0;

	DEBUG_TRACE (DEBUG::Processors, string_compose ("%1: configure processors\n", _name));
	DEBUG_TRACE (DEBUG::Processors, "{\n");
	for (list<boost::shared_ptr<Processor> >::const_iterator p = _processors.begin(); p != _processors.end(); ++p) {
		DEBUG_TRACE (DEBUG::Processors, string_compose ("\t%1 ID = %2\n", (*p)->name(), (*p)->id()));
	}
	DEBUG_TRACE (DEBUG::Processors, "}\n");

	for (ProcessorList::iterator p = _processors.begin(); p != _processors.end(); ++p, ++index) {

		if ((*p)->can_support_io_configuration(in, out)) {
			DEBUG_TRACE (DEBUG::Processors, string_compose ("\t%1 in = %2 out = %3\n",(*p)->name(), in, out));
			configuration.push_back(make_pair(in, out));
			in = out;
		} else {
			if (err) {
				err->index = index;
				err->count = in;
			}
			_in_configure_processors = false;
			return -1;
		}
	}

	// We can, so configure everything
	list< pair<ChanCount,ChanCount> >::iterator c = configuration.begin();
	for (ProcessorList::iterator p = _processors.begin(); p != _processors.end(); ++p, ++c) {
		(*p)->configure_io(c->first, c->second);
		processor_max_streams = ChanCount::max(processor_max_streams, c->first);
		processor_max_streams = ChanCount::max(processor_max_streams, c->second);
		out = c->second;
	}

	if (_meter) {
		_meter->reset_max_channels (processor_max_streams);
	}

	/* make sure we have sufficient scratch buffers to cope with the new processor
	   configuration */
	{
		Glib::Mutex::Lock em (_session.engine().process_lock ());
		_session.ensure_buffers (n_process_buffers ());
	}

	DEBUG_TRACE (DEBUG::Processors, string_compose ("%1: configuration complete\n", _name));

	_in_configure_processors = false;
	return 0;
}

void
Route::all_processors_flip ()
{
	Glib::RWLock::ReaderLock lm (_processor_lock);

	if (_processors.empty()) {
		return;
	}

	bool first_is_on = _processors.front()->active();

	for (ProcessorList::iterator i = _processors.begin(); i != _processors.end(); ++i) {
		if (first_is_on) {
			(*i)->deactivate ();
		} else {
			(*i)->activate ();
		}
	}

	_session.set_dirty ();
}

/** Set all processors with a given placement to a given active state.
 * @param p Placement of processors to change.
 * @param state New active state for those processors.
 */
void
Route::all_processors_active (Placement p, bool state)
{
	Glib::RWLock::ReaderLock lm (_processor_lock);

	if (_processors.empty()) {
		return;
	}
	ProcessorList::iterator start, end;
	placement_range(p, start, end);

	bool before_amp = true;
	for (ProcessorList::iterator i = _processors.begin(); i != _processors.end(); ++i) {
		if ((*i) == _amp) {
			before_amp = false;
			continue;
		}
		if (p == PreFader && before_amp) {
			if (state) {
				(*i)->activate ();
			} else {
				(*i)->deactivate ();
			}
		}
	}

	_session.set_dirty ();
}

bool
Route::processor_is_prefader (boost::shared_ptr<Processor> p)
{
	bool pre_fader = true;
	Glib::RWLock::ReaderLock lm (_processor_lock);

	for (ProcessorList::iterator i = _processors.begin(); i != _processors.end(); ++i) {

		/* semantic note: if p == amp, we want to return true, so test
		   for equality before checking if this is the amp
		*/

		if ((*i) == p) {
			break;
		}

		if ((*i) == _amp) {
			pre_fader = false;
			break;
		}
	}

	return pre_fader;
}

int
Route::reorder_processors (const ProcessorList& new_order, ProcessorStreams* err)
{
	/* "new_order" is an ordered list of processors to be positioned according to "placement".
	   NOTE: all processors in "new_order" MUST be marked as display_to_user(). There maybe additional
	   processors in the current actual processor list that are hidden. Any visible processors
	   in the current list but not in "new_order" will be assumed to be deleted.
	*/

	{
		Glib::RWLock::WriterLock lm (_processor_lock);
		ChanCount old_pms = processor_max_streams;
		ProcessorList::iterator oiter;
		ProcessorList::const_iterator niter;
		ProcessorList as_it_was_before = _processors;
		ProcessorList as_it_will_be;

		oiter = _processors.begin();
		niter = new_order.begin();

		while (niter !=  new_order.end()) {

			/* if the next processor in the old list is invisible (i.e. should not be in the new order)
			   then append it to the temp list.

			   Otherwise, see if the next processor in the old list is in the new list. if not,
			   its been deleted. If its there, append it to the temp list.
			*/

			if (oiter == _processors.end()) {

				/* no more elements in the old list, so just stick the rest of
				   the new order onto the temp list.
				*/

				as_it_will_be.insert (as_it_will_be.end(), niter, new_order.end());
				while (niter != new_order.end()) {
					++niter;
				}
				break;

			} else {

				if (!(*oiter)->display_to_user()) {

					as_it_will_be.push_back (*oiter);

				} else {

					/* visible processor: check that its in the new order */

					if (find (new_order.begin(), new_order.end(), (*oiter)) == new_order.end()) {
						/* deleted: do nothing, shared_ptr<> will clean up */
					} else {
						/* ignore this one, and add the next item from the new order instead */
						as_it_will_be.push_back (*niter);
						++niter;
					}
				}

                                /* now remove from old order - its taken care of no matter what */
				oiter = _processors.erase (oiter);
			}

		}

		_processors.insert (oiter, as_it_will_be.begin(), as_it_will_be.end());

		if (configure_processors_unlocked (err)) {
			_processors = as_it_was_before;
			processor_max_streams = old_pms;
			return -1;
		}
	}

        if (true) {
                processors_changed (RouteProcessorChange ()); /* EMIT SIGNAL */
		set_processor_positions ();
        }

	return 0;
}

XMLNode&
Route::get_state()
{
	return state(true);
}

XMLNode&
Route::get_template()
{
	return state(false);
}

XMLNode&
Route::state(bool full_state)
{
	XMLNode *node = new XMLNode("Route");
	ProcessorList::iterator i;
	char buf[32];

	id().print (buf, sizeof (buf));
	node->add_property("id", buf);
	node->add_property ("name", _name);
	node->add_property("default-type", _default_type.to_string());

	if (_flags) {
		node->add_property("flags", enum_2_string (_flags));
	}

	node->add_property("active", _active?"yes":"no");
	node->add_property("phase-invert", _phase_invert?"yes":"no");
	node->add_property("denormal-protection", _denormal_protection?"yes":"no");
	node->add_property("meter-point", enum_2_string (_meter_point));

	if (_route_group) {
		node->add_property("route-group", _route_group->name());
	}

	string order_string;
	OrderKeys::iterator x = order_keys.begin();

	while (x != order_keys.end()) {
		order_string += string ((*x).first);
		order_string += '=';
		snprintf (buf, sizeof(buf), "%ld", (*x).second);
		order_string += buf;

		++x;

		if (x == order_keys.end()) {
			break;
		}

		order_string += ':';
	}
	node->add_property ("order-keys", order_string);
	node->add_property ("self-solo", (_self_solo ? "yes" : "no"));
	snprintf (buf, sizeof (buf), "%d", _soloed_by_others_upstream);
	node->add_property ("soloed-by-upstream", buf);
	snprintf (buf, sizeof (buf), "%d", _soloed_by_others_downstream);
	node->add_property ("soloed-by-downstream", buf);

	node->add_child_nocopy (_input->state (full_state));
	node->add_child_nocopy (_output->state (full_state));
	node->add_child_nocopy (_solo_control->get_state ());
	node->add_child_nocopy (_mute_master->get_state ());

	XMLNode* remote_control_node = new XMLNode (X_("RemoteControl"));
	snprintf (buf, sizeof (buf), "%d", _remote_control_id);
	remote_control_node->add_property (X_("id"), buf);
	node->add_child_nocopy (*remote_control_node);

	if (_comment.length()) {
		XMLNode *cmt = node->add_child ("Comment");
		cmt->add_content (_comment);
	}

	for (i = _processors.begin(); i != _processors.end(); ++i) {
		node->add_child_nocopy((*i)->state (full_state));
	}

	if (_extra_xml){
		node->add_child_copy (*_extra_xml);
	}

	return *node;
}

int
Route::set_state (const XMLNode& node, int version)
{
	return _set_state (node, version, true);
}

int
Route::_set_state (const XMLNode& node, int version, bool /*call_base*/)
{
	if (version < 3000) {
		return _set_state_2X (node, version);
	}

	XMLNodeList nlist;
	XMLNodeConstIterator niter;
	XMLNode *child;
	XMLPropertyList plist;
	const XMLProperty *prop;

	if (node.name() != "Route"){
		error << string_compose(_("Bad node sent to Route::set_state() [%1]"), node.name()) << endmsg;
		return -1;
	}

	if ((prop = node.property (X_("name"))) != 0) {
		Route::set_name (prop->value());
	}

	if ((prop = node.property ("id")) != 0) {
		_id = prop->value ();
	}

	if ((prop = node.property (X_("flags"))) != 0) {
		_flags = Flag (string_2_enum (prop->value(), _flags));
	} else {
		_flags = Flag (0);
	}

        if (is_master() || is_monitor() || is_hidden()) {
                _mute_master->set_solo_ignore (true);
        }

	/* add all processors (except amp, which is always present) */

	nlist = node.children();
	XMLNode processor_state (X_("processor_state"));

	for (niter = nlist.begin(); niter != nlist.end(); ++niter){

		child = *niter;

		if (child->name() == IO::state_node_name) {
			if ((prop = child->property (X_("direction"))) == 0) {
				continue;
			}

			if (prop->value() == "Input") {
				_input->set_state (*child, version);
			} else if (prop->value() == "Output") {
				_output->set_state (*child, version);
			}
		}

		if (child->name() == X_("Processor")) {
			processor_state.add_child_copy (*child);
		}
	}

	set_processor_state (processor_state);

	if ((prop = node.property ("self-solo")) != 0) {
		set_self_solo (string_is_affirmative (prop->value()));
	}

	if ((prop = node.property ("soloed-by-upstream")) != 0) {
		_soloed_by_others_upstream = 0; // needed for mod_.... () to work
		mod_solo_by_others_upstream (atoi (prop->value()));
	}

	if ((prop = node.property ("soloed-by-downstream")) != 0) {
		_soloed_by_others_downstream = 0; // needed for mod_.... () to work
		mod_solo_by_others_downstream (atoi (prop->value()));
	}

	if ((prop = node.property ("solo-isolated")) != 0) {
		set_solo_isolated (string_is_affirmative (prop->value()), this);
	}

	if ((prop = node.property (X_("phase-invert"))) != 0) {
		set_phase_invert (string_is_affirmative (prop->value()));
	}

	if ((prop = node.property (X_("denormal-protection"))) != 0) {
		set_denormal_protection (string_is_affirmative (prop->value()));
	}

	if ((prop = node.property (X_("active"))) != 0) {
		bool yn = string_is_affirmative (prop->value());
		_active = !yn; // force switch
		set_active (yn);
	}

	if ((prop = node.property (X_("meter-point"))) != 0) {
		MeterPoint mp = MeterPoint (string_2_enum (prop->value (), _meter_point));
                set_meter_point (mp);
		if (_meter) {
			_meter->set_display_to_user (_meter_point == MeterCustom);
		}
	}

	if ((prop = node.property (X_("order-keys"))) != 0) {

		long n;

		string::size_type colon, equal;
		string remaining = prop->value();

		while (remaining.length()) {

			if ((equal = remaining.find_first_of ('=')) == string::npos || equal == remaining.length()) {
				error << string_compose (_("badly formed order key string in state file! [%1] ... ignored."), remaining)
				      << endmsg;
			} else {
				if (sscanf (remaining.substr (equal+1).c_str(), "%ld", &n) != 1) {
					error << string_compose (_("badly formed order key string in state file! [%1] ... ignored."), remaining)
					      << endmsg;
				} else {
					set_order_key (remaining.substr (0, equal), n);
				}
			}

			colon = remaining.find_first_of (':');

			if (colon != string::npos) {
				remaining = remaining.substr (colon+1);
			} else {
				break;
			}
		}
	}

	for (niter = nlist.begin(); niter != nlist.end(); ++niter){
		child = *niter;

		if (child->name() == X_("Comment")) {

			/* XXX this is a terrible API design in libxml++ */

			XMLNode *cmt = *(child->children().begin());
			_comment = cmt->content();

		} else if (child->name() == X_("Extra")) {

			_extra_xml = new XMLNode (*child);

		} else if (child->name() == X_("Controllable") && (prop = child->property("name")) != 0) {

			if (prop->value() == "solo") {
				_solo_control->set_state (*child, version);
				_session.add_controllable (_solo_control);
			}

		} else if (child->name() == X_("RemoteControl")) {
			if ((prop = child->property (X_("id"))) != 0) {
				int32_t x;
				sscanf (prop->value().c_str(), "%d", &x);
				set_remote_control_id (x);
			}

		} else if (child->name() == X_("MuteMaster")) {
			_mute_master->set_state (*child, version);
		}
	}

	return 0;
}

int
Route::_set_state_2X (const XMLNode& node, int version)
{
	XMLNodeList nlist;
	XMLNodeConstIterator niter;
	XMLNode *child;
	XMLPropertyList plist;
	const XMLProperty *prop;

	/* 2X things which still remain to be handled:
	 * default-type
	 * automation
	 * controlouts
	 */

	if (node.name() != "Route") {
		error << string_compose(_("Bad node sent to Route::set_state() [%1]"), node.name()) << endmsg;
		return -1;
	}

	if ((prop = node.property (X_("flags"))) != 0) {
		_flags = Flag (string_2_enum (prop->value(), _flags));
	} else {
		_flags = Flag (0);
	}
	
	if ((prop = node.property (X_("phase-invert"))) != 0) {
		set_phase_invert (string_is_affirmative (prop->value()));
	}

	if ((prop = node.property (X_("denormal-protection"))) != 0) {
		set_denormal_protection (string_is_affirmative (prop->value()));
	}

	if ((prop = node.property (X_("soloed"))) != 0) {
		bool yn = string_is_affirmative (prop->value());

		/* XXX force reset of solo status */

		set_solo (yn, this);
	}

	if ((prop = node.property (X_("muted"))) != 0) {
		
		bool first = true;
		bool muted = string_is_affirmative (prop->value());
		
		if (muted){
		  
			string mute_point;
			
			if ((prop = node.property (X_("mute-affects-pre-fader"))) != 0) {
			  
				if (string_is_affirmative (prop->value())){
					mute_point = mute_point + "PreFader";
					first = false;
				}
			}
			
			if ((prop = node.property (X_("mute-affects-post-fader"))) != 0) {
			  
				if (string_is_affirmative (prop->value())){
				  
					if (!first) {
						mute_point = mute_point + ",";
					}
					
					mute_point = mute_point + "PostFader";
					first = false;
				}
			}

			if ((prop = node.property (X_("mute-affects-control-outs"))) != 0) {
			  
				if (string_is_affirmative (prop->value())){
				  
					if (!first) {
						mute_point = mute_point + ",";
					}
					
					mute_point = mute_point + "Listen";
					first = false;
				}
			}

			if ((prop = node.property (X_("mute-affects-main-outs"))) != 0) {
			  
				if (string_is_affirmative (prop->value())){
				  
					if (!first) {
						mute_point = mute_point + ",";
					}
					
					mute_point = mute_point + "Main";
				}
			}
			
			_mute_master->set_mute_points (mute_point);
		}
	}

	if ((prop = node.property (X_("meter-point"))) != 0) {
		_meter_point = MeterPoint (string_2_enum (prop->value (), _meter_point));
	}

	/* do not carry over edit/mix groups from 2.X because (a) its hard (b) they
	   don't mean the same thing.
	*/

	if ((prop = node.property (X_("order-keys"))) != 0) {

		long n;

		string::size_type colon, equal;
		string remaining = prop->value();

		while (remaining.length()) {

			if ((equal = remaining.find_first_of ('=')) == string::npos || equal == remaining.length()) {
				error << string_compose (_("badly formed order key string in state file! [%1] ... ignored."), remaining)
					<< endmsg;
			} else {
				if (sscanf (remaining.substr (equal+1).c_str(), "%ld", &n) != 1) {
					error << string_compose (_("badly formed order key string in state file! [%1] ... ignored."), remaining)
						<< endmsg;
				} else {
					set_order_key (remaining.substr (0, equal), n);
				}
			}

			colon = remaining.find_first_of (':');

			if (colon != string::npos) {
				remaining = remaining.substr (colon+1);
			} else {
				break;
			}
		}
	}

	/* add standard processors */

	//_meter.reset (new PeakMeter (_session));
	//add_processor (_meter, PreFader);

	if (is_monitor()) {
		/* where we listen to tracks */
		_intreturn.reset (new InternalReturn (_session));
		add_processor (_intreturn, PreFader);

                _monitor_control.reset (new MonitorProcessor (_session));
                add_processor (_monitor_control, PostFader);
	}

	_main_outs.reset (new Delivery (_session, _output, _mute_master, _name, Delivery::Main));
	add_processor (_main_outs, PostFader);

	/* IOs */

	nlist = node.children ();
	for (niter = nlist.begin(); niter != nlist.end(); ++niter) {

		child = *niter;

		if (child->name() == IO::state_node_name) {

			/* there is a note in IO::set_state_2X() about why we have to call
			   this directly.
			   */

			_input->set_state_2X (*child, version, true);
			_output->set_state_2X (*child, version, false);

			if ((prop = child->property (X_("name"))) != 0) {
				Route::set_name (prop->value ());
			}

			if ((prop = child->property (X_("id"))) != 0) {
				_id = prop->value ();
			}

			if ((prop = child->property (X_("active"))) != 0) {
				bool yn = string_is_affirmative (prop->value());
				_active = !yn; // force switch
				set_active (yn);
			}
			
			if ((prop = child->property (X_("gain"))) != 0) {
				gain_t val;

				if (sscanf (prop->value().c_str(), "%f", &val) == 1) {
					_amp->gain_control()->set_value (val);
				}
			}
			
			/* Set up Panners in the IO */
			XMLNodeList io_nlist = child->children ();
			
			XMLNodeConstIterator io_niter;
			XMLNode *io_child;
			
			for (io_niter = io_nlist.begin(); io_niter != io_nlist.end(); ++io_niter) {

				io_child = *io_niter;
				
				if (io_child->name() == X_("Panner")) {
					_main_outs->panner()->set_state(*io_child, version);
				}
			}
		}
	}

	XMLNodeList redirect_nodes;

	for (niter = nlist.begin(); niter != nlist.end(); ++niter){

		child = *niter;

		if (child->name() == X_("Send") || child->name() == X_("Insert")) {
			redirect_nodes.push_back(child);
		}

	}

	set_processor_state_2X (redirect_nodes, version);

	for (niter = nlist.begin(); niter != nlist.end(); ++niter){
		child = *niter;

		if (child->name() == X_("Comment")) {

			/* XXX this is a terrible API design in libxml++ */

			XMLNode *cmt = *(child->children().begin());
			_comment = cmt->content();

		} else if (child->name() == X_("Extra")) {

			_extra_xml = new XMLNode (*child);

		} else if (child->name() == X_("Controllable") && (prop = child->property("name")) != 0) {

			if (prop->value() == "solo") {
				_solo_control->set_state (*child, version);
				_session.add_controllable (_solo_control);
			}

		} else if (child->name() == X_("RemoteControl")) {
			if ((prop = child->property (X_("id"))) != 0) {
				int32_t x;
				sscanf (prop->value().c_str(), "%d", &x);
				set_remote_control_id (x);
			}

		} 
	}

	return 0;
}

XMLNode&
Route::get_processor_state ()
{
	XMLNode* root = new XMLNode (X_("redirects"));
	for (ProcessorList::iterator i = _processors.begin(); i != _processors.end(); ++i) {
		root->add_child_nocopy ((*i)->state (true));
	}

	return *root;
}

void
Route::set_processor_state_2X (XMLNodeList const & nList, int version)
{
	/* We don't bother removing existing processors not in nList, as this
	   method will only be called when creating a Route from scratch, not
	   for undo purposes.  Just put processors in at the appropriate place
	   in the list.
	*/

	for (XMLNodeConstIterator i = nList.begin(); i != nList.end(); ++i) {
		add_processor_from_xml_2X (**i, version, _processors.begin ());
	}
}

void
Route::set_processor_state (const XMLNode& node)
{
	const XMLNodeList &nlist = node.children();
	XMLNodeConstIterator niter;
        ProcessorList new_order;
        bool must_configure = false;

	for (niter = nlist.begin(); niter != nlist.end(); ++niter) {

		XMLProperty* prop = (*niter)->property ("type");

		if (prop->value() == "amp") {
                        _amp->set_state (**niter, Stateful::current_state_version);
                        new_order.push_back (_amp);
                } else if (prop->value() == "meter") {
                        _meter->set_state (**niter, Stateful::current_state_version);
                        new_order.push_back (_meter);
                } else if (prop->value() == "main-outs") {
                        _main_outs->set_state (**niter, Stateful::current_state_version);
                        new_order.push_back (_main_outs);
                } else if (prop->value() == "intreturn") {
                        if (!_intreturn) {
                                _intreturn.reset (new InternalReturn (_session));
                                must_configure = true;
                        }
                        _intreturn->set_state (**niter, Stateful::current_state_version);
                        new_order.push_back (_intreturn);
                } else if (is_monitor() && prop->value() == "monitor") {
                        if (!_monitor_control) {
                                _monitor_control.reset (new MonitorProcessor (_session));
                                must_configure = true;
                        }
                        _monitor_control->set_state (**niter, Stateful::current_state_version);
                        new_order.push_back (_monitor_control);
                } else {
                        ProcessorList::iterator o;

			for (o = _processors.begin(); o != _processors.end(); ++o) {
				XMLProperty* id_prop = (*niter)->property(X_("id"));
				if (id_prop && (*o)->id() == id_prop->value()) {
                                        (*o)->set_state (**niter, Stateful::current_state_version);
                                        new_order.push_back (*o);
					break;
				}
			}

                        // If the processor (*niter) is not on the route then create it 
                        
                        if (o == _processors.end()) {
                                
                                boost::shared_ptr<Processor> processor;

                                if (prop->value() == "intsend") {
                                        
                                        processor.reset (new InternalSend (_session, _mute_master, boost::shared_ptr<Route>(), Delivery::Role (0)));
                                        
                                } else if (prop->value() == "ladspa" || prop->value() == "Ladspa" ||
                                           prop->value() == "lv2" ||
                                           prop->value() == "vst" ||
                                           prop->value() == "audiounit") {
                                        
                                        processor.reset (new PluginInsert(_session));
                                        
                                } else if (prop->value() == "port") {
                                        
                                        processor.reset (new PortInsert (_session, _mute_master));
                                        
                                } else if (prop->value() == "send") {
                                        
                                        processor.reset (new Send (_session, _mute_master));
                                        
                                } else {
                                        error << string_compose(_("unknown Processor type \"%1\"; ignored"), prop->value()) << endmsg;
                                        continue;
                                }

                                processor->set_state (**niter, Stateful::current_state_version);
                                new_order.push_back (processor);
                                must_configure = true;
                        }
                }
        }

        { 
		Glib::RWLock::WriterLock lm (_processor_lock);
                _processors = new_order;
                if (must_configure) {
                        configure_processors_unlocked (0);
                }
        }

        processors_changed (RouteProcessorChange ());
	set_processor_positions ();
}

void
Route::curve_reallocate ()
{
//	_gain_automation_curve.finish_resize ();
//	_pan_automation_curve.finish_resize ();
}

void
Route::silence (nframes_t nframes)
{
	Glib::RWLock::ReaderLock lm (_processor_lock, Glib::TRY_LOCK);
	if (!lm.locked()) {
		return;
	}

	silence_unlocked (nframes);
}

void
Route::silence_unlocked (nframes_t nframes)
{
	/* Must be called with the processor lock held */
	
	if (!_silent) {

		_output->silence (nframes);

		for (ProcessorList::iterator i = _processors.begin(); i != _processors.end(); ++i) {
			boost::shared_ptr<PluginInsert> pi;
			
			if (!_active && (pi = boost::dynamic_pointer_cast<PluginInsert> (*i)) != 0) {
				// skip plugins, they don't need anything when we're not active
				continue;
			}
			
			(*i)->silence (nframes);
		}
		
		if (nframes == _session.get_block_size()) {
			// _silent = true;
		}
	}
}

void
Route::add_internal_return ()
{
	if (!_intreturn) {
		_intreturn.reset (new InternalReturn (_session));
		add_processor (_intreturn, PreFader);
	}
}

BufferSet*
Route::get_return_buffer () const
{
	Glib::RWLock::ReaderLock rm (_processor_lock);

	for (ProcessorList::const_iterator x = _processors.begin(); x != _processors.end(); ++x) {
		boost::shared_ptr<InternalReturn> d = boost::dynamic_pointer_cast<InternalReturn>(*x);

		if (d) {
			BufferSet* bs = d->get_buffers ();
			return bs;
		}
	}

	return 0;
}

void
Route::release_return_buffer () const
{
	Glib::RWLock::ReaderLock rm (_processor_lock);

	for (ProcessorList::const_iterator x = _processors.begin(); x != _processors.end(); ++x) {
		boost::shared_ptr<InternalReturn> d = boost::dynamic_pointer_cast<InternalReturn>(*x);

		if (d) {
			return d->release_buffers ();
		}
	}
}

int
Route::listen_via (boost::shared_ptr<Route> route, Placement placement, bool /*active*/, bool aux)
{
	vector<string> ports;
	vector<string>::const_iterator i;

	{
		Glib::RWLock::ReaderLock rm (_processor_lock);

		for (ProcessorList::iterator x = _processors.begin(); x != _processors.end(); ++x) {

			boost::shared_ptr<InternalSend> d = boost::dynamic_pointer_cast<InternalSend>(*x);

			if (d && d->target_route() == route) {

				/* if the target is the control outs, then make sure
				   we take note of which i-send is doing that.
				*/

				if (route == _session.monitor_out()) {
					_monitor_send = boost::dynamic_pointer_cast<Delivery>(d);
				}

				/* already listening via the specified IO: do nothing */

				return 0;
			}
		}
	}

	boost::shared_ptr<InternalSend> listener;

	try {

                if (is_master()) {
                        
                        if (route == _session.monitor_out()) {
                                /* master never sends to control outs */
                                return 0;
                        } else {
                                listener.reset (new InternalSend (_session, _mute_master, route, (aux ? Delivery::Aux : Delivery::Listen)));
                        }

                } else {
                        listener.reset (new InternalSend (_session, _mute_master, route, (aux ? Delivery::Aux : Delivery::Listen)));
                }

	} catch (failed_constructor& err) {
		return -1;
	}

	if (route == _session.monitor_out()) {
		_monitor_send = listener;
	}

        if (placement == PostFader) {
                /* put it *really* at the end, not just after the panner (main outs)
                 */
                add_processor (listener, _processors.end());
        } else {
                add_processor (listener, PreFader);
        }

	return 0;
}

void
Route::drop_listen (boost::shared_ptr<Route> route)
{
	ProcessorStreams err;
	ProcessorList::iterator tmp;

	Glib::RWLock::ReaderLock rl(_processor_lock);
	rl.acquire ();

  again:
	for (ProcessorList::iterator x = _processors.begin(); x != _processors.end(); ) {

		boost::shared_ptr<InternalSend> d = boost::dynamic_pointer_cast<InternalSend>(*x);

		if (d && d->target_route() == route) {
			rl.release ();
			remove_processor (*x, &err);
			rl.acquire ();

                        /* list could have been demolished while we dropped the lock
			   so start over.
			*/

			goto again;
		}
	}

	rl.release ();

	if (route == _session.monitor_out()) {
		_monitor_send.reset ();
	}
}

void
Route::set_comment (string cmt, void *src)
{
	_comment = cmt;
	comment_changed (src);
	_session.set_dirty ();
}

bool
Route::add_fed_by (boost::shared_ptr<Route> other, bool via_sends_only)
{
        FeedRecord fr (other, via_sends_only);

        pair<FedBy::iterator,bool> result =  _fed_by.insert (fr);

        if (!result.second) {

                /* already a record for "other" - make sure sends-only information is correct */
                if (!via_sends_only && result.first->sends_only) {
                        FeedRecord* frp = const_cast<FeedRecord*>(&(*result.first));
                        frp->sends_only = false;
                }
        }
        
        return result.second;
}

void
Route::clear_fed_by ()
{
        _fed_by.clear ();
}

bool
Route::feeds (boost::shared_ptr<Route> other, bool* via_sends_only)
{
        const FedBy& fed_by (other->fed_by());

        for (FedBy::iterator f = fed_by.begin(); f != fed_by.end(); ++f) {
                boost::shared_ptr<Route> sr = f->r.lock();

                if (sr && (sr.get() == this)) {

                        if (via_sends_only) {
                                *via_sends_only = f->sends_only;
                        }

                        return true;
                }
        }

        return false;
}

bool
Route::direct_feeds (boost::shared_ptr<Route> other, bool* only_send)
{
	DEBUG_TRACE (DEBUG::Graph, string_compose ("Feeds? %1\n", _name));

	if (_output->connected_to (other->input())) {
		DEBUG_TRACE (DEBUG::Graph, string_compose ("\tdirect FEEDS %2\n", other->name()));
		if (only_send) {
			*only_send = false;
		}

		return true;
	}

	
	for (ProcessorList::iterator r = _processors.begin(); r != _processors.end(); ++r) {

		boost::shared_ptr<IOProcessor> iop;

		if ((iop = boost::dynamic_pointer_cast<IOProcessor>(*r)) != 0) {
			if (iop->feeds (other)) {
				DEBUG_TRACE (DEBUG::Graph,  string_compose ("\tIOP %1 does feed %2\n", iop->name(), other->name()));
				if (only_send) {
					*only_send = true;
				}
				return true;
			} else {
				DEBUG_TRACE (DEBUG::Graph,  string_compose ("\tIOP %1 does NOT feed %2\n", iop->name(), other->name()));
			}
		} else {
			DEBUG_TRACE (DEBUG::Graph,  string_compose ("\tPROC %1 is not an IOP\n", (*r)->name()));
		}
			
	}

	DEBUG_TRACE (DEBUG::Graph,  string_compose ("\tdoes NOT feed %1\n", other->name()));
	return false;
}

void
Route::handle_transport_stopped (bool /*abort_ignored*/, bool did_locate, bool can_flush_processors)
{
	nframes_t now = _session.transport_frame();

	{
		Glib::RWLock::ReaderLock lm (_processor_lock);

		if (!did_locate) {
			automation_snapshot (now, true);
		}

		for (ProcessorList::iterator i = _processors.begin(); i != _processors.end(); ++i) {

			if (Config->get_plugins_stop_with_transport() && can_flush_processors) {
				(*i)->deactivate ();
				(*i)->activate ();
			}

			(*i)->transport_stopped (now);
		}
	}

	_roll_delay = _initial_delay;
}

void
Route::input_change_handler (IOChange change, void * /*src*/)
{
	if ((change & ConfigurationChanged)) {
		configure_processors (0);
	}
}

void
Route::output_change_handler (IOChange change, void * /*src*/)
{
	if ((change & ConfigurationChanged)) {

		/* XXX resize all listeners to match _main_outs? */

		// configure_processors (0);
	}
}

uint32_t
Route::pans_required () const
{
	if (n_outputs().n_audio() < 2) {
		return 0;
	}

	return max (n_inputs ().n_audio(), processor_max_streams.n_audio());
}

int
Route::no_roll (nframes_t nframes, sframes_t start_frame, sframes_t end_frame,
		bool session_state_changing, bool /*can_record*/, bool /*rec_monitors_input*/)
{
	Glib::RWLock::ReaderLock lm (_processor_lock, Glib::TRY_LOCK);
	if (!lm.locked()) {
		return 0;
	}

	if (n_outputs().n_total() == 0) {
		return 0;
	}

	if (!_active || n_inputs() == ChanCount::ZERO)  {
		silence_unlocked (nframes);
		return 0;
	}
	if (session_state_changing) {
		if (_session.transport_speed() != 0.0f) {
			/* we're rolling but some state is changing (e.g. our diskstream contents)
			   so we cannot use them. Be silent till this is over.
			   
			   XXX note the absurdity of ::no_roll() being called when we ARE rolling!
			*/
			silence_unlocked (nframes);
			return 0;
		}
		/* we're really not rolling, so we're either delivery silence or actually
		   monitoring, both of which are safe to do while session_state_changing is true.
		*/
	}

	_amp->apply_gain_automation (false);
	passthru (start_frame, end_frame, nframes, 0);

	return 0;
}

nframes_t
Route::check_initial_delay (nframes_t nframes, nframes_t& transport_frame)
{
	if (_roll_delay > nframes) {

		_roll_delay -= nframes;
		silence_unlocked (nframes);
		/* transport frame is not legal for caller to use */
		return 0;

	} else if (_roll_delay > 0) {

		nframes -= _roll_delay;
		silence_unlocked (_roll_delay);
		/* we've written _roll_delay of samples into the
		   output ports, so make a note of that for
		   future reference.
		*/
		_main_outs->increment_output_offset (_roll_delay);
		transport_frame += _roll_delay;

		_roll_delay = 0;
	}

	return nframes;
}

int
Route::roll (nframes_t nframes, sframes_t start_frame, sframes_t end_frame, int declick,
	     bool /*can_record*/, bool /*rec_monitors_input*/, bool& /* need_butler */)
{
	Glib::RWLock::ReaderLock lm (_processor_lock, Glib::TRY_LOCK);
	if (!lm.locked()) {
		return 0;
	}
	
	automation_snapshot (_session.transport_frame(), false);

	if (n_outputs().n_total() == 0) {
		return 0;
	}

	if (!_active || n_inputs().n_total() == 0) {
		silence_unlocked (nframes);
		return 0;
	}

	nframes_t unused = 0;

	if ((nframes = check_initial_delay (nframes, unused)) == 0) {
		return 0;
	}

	_silent = false;

	passthru (start_frame, end_frame, nframes, declick);

	return 0;
}

int
Route::silent_roll (nframes_t nframes, sframes_t /*start_frame*/, sframes_t /*end_frame*/,
		    bool /*can_record*/, bool /*rec_monitors_input*/, bool& /* need_butler */)
{
	silence (nframes);
	return 0;
}

void
Route::toggle_monitor_input ()
{
	for (PortSet::iterator i = _input->ports().begin(); i != _input->ports().end(); ++i) {
		i->ensure_monitor_input( ! i->monitoring_input());
	}
}

bool
Route::has_external_redirects () const
{
	// FIXME: what about sends? - they don't return a signal back to ardour?

	boost::shared_ptr<const PortInsert> pi;

	for (ProcessorList::const_iterator i = _processors.begin(); i != _processors.end(); ++i) {

		if ((pi = boost::dynamic_pointer_cast<const PortInsert>(*i)) != 0) {

			for (PortSet::const_iterator port = pi->output()->ports().begin(); port != pi->output()->ports().end(); ++port) {

				string port_name = port->name();
				string client_name = port_name.substr (0, port_name.find(':'));

				/* only say "yes" if the redirect is actually in use */

				if (client_name != "ardour" && pi->active()) {
					return true;
				}
			}
		}
	}

	return false;
}

void
Route::flush_processors ()
{
	/* XXX shouldn't really try to take this lock, since
	   this is called from the RT audio thread.
	*/

	Glib::RWLock::ReaderLock lm (_processor_lock);

	for (ProcessorList::iterator i = _processors.begin(); i != _processors.end(); ++i) {
		(*i)->deactivate ();
		(*i)->activate ();
	}
}

void
Route::set_meter_point (MeterPoint p)
{
	/* CAN BE CALLED FROM PROCESS CONTEXT */

	if (_meter_point == p) {
		return;
	}

	bool meter_was_visible_to_user = _meter->display_to_user ();

	{
		Glib::RWLock::WriterLock lm (_processor_lock);
	
		if (p != MeterCustom) {
			// Move meter in the processors list to reflect the new position
			ProcessorList::iterator loc = find (_processors.begin(), _processors.end(), _meter);
			_processors.erase(loc);
			switch (p) {
			case MeterInput:
				loc = _processors.begin();
				break;
			case MeterPreFader:
				loc = find (_processors.begin(), _processors.end(), _amp);
				break;
			case MeterPostFader:
				loc = _processors.end();
				break;
			default:
				break;
			}
			
			ChanCount m_in;
			
			if (loc == _processors.begin()) {
				m_in = _input->n_ports();
			} else {
				ProcessorList::iterator before = loc;
				--before;
				m_in = (*before)->output_streams ();
			}
			
			_meter->reflect_inputs (m_in);
			
			_processors.insert (loc, _meter);
			
			/* we do not need to reconfigure the processors, because the meter
			   (a) is always ready to handle processor_max_streams
			   (b) is always an N-in/N-out processor, and thus moving
			   it doesn't require any changes to the other processors.
			*/
			
			_meter->set_display_to_user (false);
			
		} else {
			
			// just make it visible and let the user move it
			
			_meter->set_display_to_user (true);
		}
	}

	_meter_point = p;
	meter_change (); /* EMIT SIGNAL */

	bool const meter_visibly_changed = (_meter->display_to_user() != meter_was_visible_to_user);
	
	processors_changed (RouteProcessorChange (RouteProcessorChange::MeterPointChange, meter_visibly_changed)); /* EMIT SIGNAL */
}

void
Route::put_monitor_send_at (Placement p)
{
	if (!_monitor_send) {
		return;
	}

	{
		Glib::RWLock::WriterLock lm (_processor_lock);
		ProcessorList as_it_was (_processors);
		ProcessorList::iterator loc = find(_processors.begin(), _processors.end(), _monitor_send);
		_processors.erase(loc);
		
		switch (p) {
		case PreFader:
			loc = find(_processors.begin(), _processors.end(), _amp);
			if (loc != _processors.begin()) {
				--loc;
			}
			break;
		case PostFader:
			loc = _processors.end();
			break;
		}
		
		_processors.insert (loc, _monitor_send);

		if (configure_processors_unlocked (0)) {
			_processors = as_it_was;
			configure_processors_unlocked (0); // it worked before we tried to add it ...
			return;
		}
	}

	processors_changed (RouteProcessorChange ()); /* EMIT SIGNAL */
	_session.set_dirty ();
}

nframes_t
Route::update_total_latency ()
{
	nframes_t old = _output->effective_latency();
	nframes_t own_latency = _output->user_latency();

	for (ProcessorList::iterator i = _processors.begin(); i != _processors.end(); ++i) {
		if ((*i)->active ()) {
			own_latency += (*i)->signal_latency ();
		}
	}

	DEBUG_TRACE (DEBUG::Latency, string_compose ("%1: internal redirect latency = %2\n", _name, own_latency));

	_output->set_port_latency (own_latency);

	if (_output->user_latency() == 0) {

		/* this (virtual) function is used for pure Routes,
		   not derived classes like AudioTrack.  this means
		   that the data processed here comes from an input
		   port, not prerecorded material, and therefore we
		   have to take into account any input latency.
		*/

		own_latency += _input->signal_latency ();
	}

	if (old != own_latency) {
		_output->set_latency_delay (own_latency);
		signal_latency_changed (); /* EMIT SIGNAL */
	}

	DEBUG_TRACE (DEBUG::Latency, string_compose ("%1: input latency = %2 total = %3\n", _name, _input->signal_latency(), own_latency));

	return _output->effective_latency ();
}

void
Route::set_user_latency (nframes_t nframes)
{
	_output->set_user_latency (nframes);
	_session.update_latency_compensation (false, false);
}

void
Route::set_latency_delay (nframes_t longest_session_latency)
{
	nframes_t old = _initial_delay;

	if (_output->effective_latency() < longest_session_latency) {
		_initial_delay = longest_session_latency - _output->effective_latency();
	} else {
		_initial_delay = 0;
	}

	if (_initial_delay != old) {
		initial_delay_changed (); /* EMIT SIGNAL */
	}

	if (_session.transport_stopped()) {
		_roll_delay = _initial_delay;
	}
}

void
Route::automation_snapshot (nframes_t now, bool force)
{
	panner()->automation_snapshot (now, force);
	
	for (ProcessorList::iterator i = _processors.begin(); i != _processors.end(); ++i) {
		(*i)->automation_snapshot (now, force);
	}
}

Route::SoloControllable::SoloControllable (std::string name, Route& r)
	: AutomationControl (r.session(), Evoral::Parameter (SoloAutomation),
			     boost::shared_ptr<AutomationList>(), name)
	, route (r)
{
	boost::shared_ptr<AutomationList> gl(new AutomationList(Evoral::Parameter(SoloAutomation)));
	set_list (gl);
}

void
Route::SoloControllable::set_value (float val)
{
	bool bval = ((val >= 0.5f) ? true: false);
# if 0
	this is how it should be done 

	boost::shared_ptr<RouteList> rl (new RouteList);
	rl->push_back (route);

	if (Config->get_solo_control_is_listen_control()) {
		_session.set_listen (rl, bval);
	} else {
		_session.set_solo (rl, bval);
	}
#else
	route.set_solo (bval, this);
#endif
}

float
Route::SoloControllable::get_value (void) const
{
	if (Config->get_solo_control_is_listen_control()) {
		return route.listening() ? 1.0f : 0.0f;
	} else {
		return route.self_soloed() ? 1.0f : 0.0f;
	}
}

Route::MuteControllable::MuteControllable (std::string name, Route& r)
	: AutomationControl (r.session(), Evoral::Parameter (MuteAutomation),
			     boost::shared_ptr<AutomationList>(), name)
	, route (r)
{
	boost::shared_ptr<AutomationList> gl(new AutomationList(Evoral::Parameter(MuteAutomation)));
	set_list (gl);
}

void
Route::MuteControllable::set_value (float val)
{
	bool bval = ((val >= 0.5f) ? true: false);
# if 0
	this is how it should be done 

	boost::shared_ptr<RouteList> rl (new RouteList);
	rl->push_back (route);
	_session.set_mute (rl, bval);
#else
	route.set_mute (bval, this);
#endif
}

float
Route::MuteControllable::get_value (void) const
{
	return route.muted() ? 1.0f : 0.0f;
}

void
Route::set_block_size (nframes_t nframes)
{
	for (ProcessorList::iterator i = _processors.begin(); i != _processors.end(); ++i) {
		(*i)->set_block_size (nframes);
	}
	
	_session.ensure_buffers (n_process_buffers ());
}

void
Route::protect_automation ()
{
	for (ProcessorList::iterator i = _processors.begin(); i != _processors.end(); ++i)
		(*i)->protect_automation();
}

void
Route::set_pending_declick (int declick)
{
	if (_declickable) {
		/* this call is not allowed to turn off a pending declick unless "force" is true */
		if (declick) {
			_pending_declick = declick;
		}
		// cerr << _name << ": after setting to " << declick << " pending declick = " << _pending_declick << endl;
	} else {
		_pending_declick = 0;
	}

}

/** Shift automation forwards from a particular place, thereby inserting time.
 *  Adds undo commands for any shifts that are performed.
 *
 * @param pos Position to start shifting from.
 * @param frames Amount to shift forwards by.
 */

void
Route::shift (nframes64_t /*pos*/, nframes64_t /*frames*/)
{
#ifdef THIS_NEEDS_FIXING_FOR_V3

	/* gain automation */
	XMLNode &before = _gain_control->get_state ();
	_gain_control->shift (pos, frames);
	XMLNode &after = _gain_control->get_state ();
	_session.add_command (new MementoCommand<AutomationList> (_gain_automation_curve, &before, &after));

	/* pan automation */
	for (std::vector<StreamPanner*>::iterator i = _panner->begin (); i != _panner->end (); ++i) {
		Curve & c = (*i)->automation ();
		XMLNode &before = c.get_state ();
		c.shift (pos, frames);
		XMLNode &after = c.get_state ();
		_session.add_command (new MementoCommand<AutomationList> (c, &before, &after));
	}

	/* redirect automation */
	{
		Glib::RWLock::ReaderLock lm (redirect_lock);
		for (RedirectList::iterator i = _redirects.begin (); i != _redirects.end (); ++i) {

			set<uint32_t> a;
			(*i)->what_has_automation (a);

			for (set<uint32_t>::const_iterator j = a.begin (); j != a.end (); ++j) {
				AutomationList & al = (*i)->automation_list (*j);
				XMLNode &before = al.get_state ();
				al.shift (pos, frames);
				XMLNode &after = al.get_state ();
				_session.add_command (new MementoCommand<AutomationList> (al, &before, &after));
			}
		}
	}
#endif

}


int
Route::save_as_template (const string& path, const string& name)
{
	XMLNode& node (state (false));
	XMLTree tree;

	IO::set_name_in_state (*node.children().front(), name);

	tree.set_root (&node);
	return tree.write (path.c_str());
}


bool
Route::set_name (const string& str)
{
	bool ret;
	string ioproc_name;
	string name;

	name = Route::ensure_track_or_route_name (str, _session);
	SessionObject::set_name (name);

	ret = (_input->set_name(name) && _output->set_name(name));

	if (ret) {

		Glib::RWLock::ReaderLock lm (_processor_lock);

		for (ProcessorList::iterator i = _processors.begin(); i != _processors.end(); ++i) {

			/* rename all I/O processors that have inputs or outputs */

			boost::shared_ptr<IOProcessor> iop = boost::dynamic_pointer_cast<IOProcessor> (*i);

			if (iop && (iop->output() || iop->input())) {
				if (!iop->set_name (name)) {
					ret = false;
				}
			}
		}

	}

	return ret;
}

boost::shared_ptr<Send>
Route::internal_send_for (boost::shared_ptr<const Route> target) const
{
	Glib::RWLock::ReaderLock lm (_processor_lock);

	for (ProcessorList::const_iterator i = _processors.begin(); i != _processors.end(); ++i) {
		boost::shared_ptr<InternalSend> send;

		if ((send = boost::dynamic_pointer_cast<InternalSend>(*i)) != 0) {
			if (send->target_route() == target) {
				return send;
			}
		}
	}

	return boost::shared_ptr<Send>();
}

void
Route::set_phase_invert (bool yn)
{
	if (_phase_invert != yn) {
                if (yn) {
                        _phase_invert = 0xffff; // XXX all channels
                } else {
                        _phase_invert = 0; // XXX no channels
                }

		phase_invert_changed (); /* EMIT SIGNAL */
                _session.set_dirty ();
	}
}

bool
Route::phase_invert () const
{
	return _phase_invert != 0;
}

void
Route::set_denormal_protection (bool yn)
{
	if (_denormal_protection != yn) {
		_denormal_protection = yn;
		denormal_protection_changed (); /* EMIT SIGNAL */
	}
}

bool
Route::denormal_protection () const
{
	return _denormal_protection;
}

void
Route::set_active (bool yn)
{
	if (_active != yn) {
		_active = yn;
		_input->set_active (yn);
		_output->set_active (yn);
		active_changed (); // EMIT SIGNAL
	}
}

void
Route::meter ()
{
	Glib::RWLock::ReaderLock rm (_processor_lock, Glib::TRY_LOCK);

	assert (_meter);

	_meter->meter ();

	for (ProcessorList::iterator i = _processors.begin(); i != _processors.end(); ++i) {

		boost::shared_ptr<Send> s;
		boost::shared_ptr<Return> r;

		if ((s = boost::dynamic_pointer_cast<Send> (*i)) != 0) {
			s->meter()->meter();
		} else if ((r = boost::dynamic_pointer_cast<Return> (*i)) != 0) {
			r->meter()->meter ();
		}
	}
}

boost::shared_ptr<Panner>
Route::panner() const
{
	return _main_outs->panner();
}

boost::shared_ptr<AutomationControl>
Route::gain_control() const
{
	return _amp->gain_control();
}

boost::shared_ptr<AutomationControl>
Route::get_control (const Evoral::Parameter& param)
{
	/* either we own the control or .... */

	boost::shared_ptr<AutomationControl> c = boost::dynamic_pointer_cast<AutomationControl>(control (param));

	if (!c) {

		/* maybe one of our processors does or ... */

		Glib::RWLock::ReaderLock rm (_processor_lock, Glib::TRY_LOCK);
		for (ProcessorList::iterator i = _processors.begin(); i != _processors.end(); ++i) {
			if ((c = boost::dynamic_pointer_cast<AutomationControl>((*i)->control (param))) != 0) {
				break;
			}
		}
	}

	if (!c) {

		/* nobody does so we'll make a new one */

		c = boost::dynamic_pointer_cast<AutomationControl>(control_factory(param));
		add_control(c);
	}

	return c;
}

boost::shared_ptr<Processor>
Route::nth_plugin (uint32_t n)
{
	Glib::RWLock::ReaderLock lm (_processor_lock);
	ProcessorList::iterator i;

	for (i = _processors.begin(); i != _processors.end(); ++i) {
		if (boost::dynamic_pointer_cast<PluginInsert> (*i)) {
			if (n-- == 0) {
				return *i;
			}
		}
	}

	return boost::shared_ptr<Processor> ();
}

boost::shared_ptr<Processor>
Route::nth_send (uint32_t n)
{
	Glib::RWLock::ReaderLock lm (_processor_lock);
	ProcessorList::iterator i;

	for (i = _processors.begin(); i != _processors.end(); ++i) {
		if (boost::dynamic_pointer_cast<Send> (*i)) {
			if (n-- == 0) {
				return *i;
			}
		} 
	}

	return boost::shared_ptr<Processor> ();
}

bool
Route::has_io_processor_named (const string& name)
{
        Glib::RWLock::ReaderLock lm (_processor_lock);
        ProcessorList::iterator i;
        
        for (i = _processors.begin(); i != _processors.end(); ++i) {
                if (boost::dynamic_pointer_cast<Send> (*i) ||
                    boost::dynamic_pointer_cast<PortInsert> (*i)) {
                        if ((*i)->name() == name) {
                                return true;
                        }
                }
        }
        
        return false;
}

MuteMaster::MutePoint
Route::mute_points () const
{
	return _mute_master->mute_points ();
}

void
Route::set_processor_positions ()
{
	Glib::RWLock::ReaderLock lm (_processor_lock);

	bool had_amp = false;
	for (ProcessorList::iterator i = _processors.begin(); i != _processors.end(); ++i) {
		(*i)->set_pre_fader (!had_amp);
		if (boost::dynamic_pointer_cast<Amp> (*i)) {
			had_amp = true;
		}
	}
}

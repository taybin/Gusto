/*
  Copyright (C) 1999-2002 Paul Davis

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


#ifdef WAF_BUILD
#include "libardour-config.h"
#endif

#define __STDC_FORMAT_MACROS 1
#include <stdint.h>

#include <algorithm>
#include <fstream>
#include <string>
#include <cerrno>


#include <cstdio> /* snprintf(3) ... grrr */
#include <cmath>
#include <unistd.h>
#include <sys/stat.h>
#include <climits>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <dirent.h>

#ifdef HAVE_SYS_VFS_H
#include <sys/vfs.h>
#else
#include <sys/param.h>
#include <sys/mount.h>
#endif

#include <glibmm.h>
#include <glibmm/thread.h>

#include "midi++/mmc.h"
#include "midi++/port.h"

#include "pbd/boost_debug.h"
#include "pbd/controllable_descriptor.h"
#include "pbd/enumwriter.h"
#include "pbd/error.h"
#include "pbd/pathscanner.h"
#include "pbd/pthread_utils.h"
#include "pbd/search_path.h"
#include "pbd/stacktrace.h"
#include "pbd/convert.h"

#include "ardour/amp.h"
#include "ardour/audio_diskstream.h"
#include "ardour/audio_track.h"
#include "ardour/audioengine.h"
#include "ardour/audiofilesource.h"
#include "ardour/audioplaylist.h"
#include "ardour/audioregion.h"
#include "ardour/auditioner.h"
#include "ardour/buffer.h"
#include "ardour/butler.h"
#include "ardour/configuration.h"
#include "ardour/control_protocol_manager.h"
#include "ardour/crossfade.h"
#include "ardour/cycle_timer.h"
#include "ardour/directory_names.h"
#include "ardour/filename_extensions.h"
#include "ardour/io_processor.h"
#include "ardour/location.h"
#include "ardour/midi_diskstream.h"
#include "ardour/midi_patch_manager.h"
#include "ardour/midi_playlist.h"
#include "ardour/midi_region.h"
#include "ardour/midi_source.h"
#include "ardour/midi_track.h"
#include "ardour/named_selection.h"
#include "ardour/processor.h"
#include "ardour/port.h"
#include "ardour/region_factory.h"
#include "ardour/route_group.h"
#include "ardour/send.h"
#include "ardour/session.h"
#include "ardour/session_directory.h"
#include "ardour/session_metadata.h"
#include "ardour/session_state_utils.h"
#include "ardour/session_playlists.h"
#include "ardour/session_utils.h"
#include "ardour/silentfilesource.h"
#include "ardour/slave.h"
#include "ardour/smf_source.h"
#include "ardour/sndfile_helpers.h"
#include "ardour/sndfilesource.h"
#include "ardour/source_factory.h"
#include "ardour/template_utils.h"
#include "ardour/tempo.h"
#include "ardour/ticker.h"
#include "ardour/user_bundle.h"
#include "ardour/utils.h"
#include "ardour/utils.h"
#include "ardour/version.h"
#include "ardour/playlist_factory.h"

#include "control_protocol/control_protocol.h"

#include "i18n.h"
#include <locale.h>

using namespace std;
using namespace ARDOUR;
using namespace PBD;

void
Session::first_stage_init (string fullpath, string snapshot_name)
{
	if (fullpath.length() == 0) {
		destroy ();
		throw failed_constructor();
	}

	char buf[PATH_MAX+1];
	if (!realpath (fullpath.c_str(), buf) && (errno != ENOENT)) {
		error << string_compose(_("Could not use path %1 (%s)"), buf, strerror(errno)) << endmsg;
		destroy ();
		throw failed_constructor();
	}

	_path = string(buf);

	if (_path[_path.length()-1] != '/') {
		_path += '/';
	}

	if (Glib::file_test (_path, Glib::FILE_TEST_EXISTS) && ::access (_path.c_str(), W_OK)) {
		_writable = false;
	} else {
		_writable = true;
	}

	/* these two are just provisional settings. set_state()
	   will likely override them.
	*/

	_name = _current_snapshot_name = snapshot_name;

	set_history_depth (Config->get_history_depth());

	_current_frame_rate = _engine.frame_rate ();
	_nominal_frame_rate = _current_frame_rate;
	_base_frame_rate = _current_frame_rate;

	_tempo_map = new TempoMap (_current_frame_rate);
	_tempo_map->PropertyChanged.connect_same_thread (*this, boost::bind (&Session::tempo_map_changed, this, _1));


	_non_soloed_outs_muted = false;
	_listen_cnt = 0;
	_solo_isolated_cnt = 0;
	g_atomic_int_set (&processing_prohibited, 0);
	_transport_speed = 0;
	_last_transport_speed = 0;
	_target_transport_speed = 0;
	auto_play_legal = false;
	transport_sub_state = 0;
	_transport_frame = 0;
	_requested_return_frame = -1;
	_session_range_location = 0;
	g_atomic_int_set (&_record_status, Disabled);
	loop_changing = false;
	play_loop = false;
	have_looped = false;
	_last_roll_location = 0;
	_last_roll_or_reversal_location = 0;
	_last_record_location = 0;
	pending_locate_frame = 0;
	pending_locate_roll = false;
	pending_locate_flush = false;
	state_was_pending = false;
	set_next_event ();
	outbound_mtc_timecode_frame = 0;
	next_quarter_frame_to_send = -1;
	current_block_size = 0;
	solo_update_disabled = false;
	_have_captured = false;
	_worst_output_latency = 0;
	_worst_input_latency = 0;
 	_worst_track_latency = 0;
	_state_of_the_state = StateOfTheState(CannotSave|InitialConnecting|Loading);
	_was_seamless = Config->get_seamless_loop ();
	_slave = 0;
	session_send_mtc = false;
	g_atomic_int_set (&_playback_load, 100);
	g_atomic_int_set (&_capture_load, 100);
	_play_range = false;
	_exporting = false;
	pending_abort = false;
	destructive_index = 0;
	first_file_data_format_reset = true;
	first_file_header_format_reset = true;
	post_export_sync = false;
	midi_control_ui = 0;

	AudioDiskstream::allocate_working_buffers();

	/* default short fade = 15ms */

	Crossfade::set_short_xfade_length ((nframes_t) floor (config.get_short_xfade_seconds() * frame_rate()));
	SndFileSource::setup_standard_crossfades (*this, frame_rate());

	last_mmc_step.tv_sec = 0;
	last_mmc_step.tv_usec = 0;
	step_speed = 0.0;

	/* click sounds are unset by default, which causes us to internal
	   waveforms for clicks.
	*/

	click_length = 0;
	click_emphasis_length = 0;
	_clicking = false;

	process_function = &Session::process_with_events;

	if (config.get_use_video_sync()) {
		waiting_for_sync_offset = true;
	} else {
		waiting_for_sync_offset = false;
	}

	last_timecode_when = 0;
	_timecode_offset = 0;
	_timecode_offset_negative = true;
	last_timecode_valid = false;

	sync_time_vars ();

	last_rr_session_dir = session_dirs.begin();
	refresh_disk_space ();

	// set_default_fade (0.2, 5.0); /* steepness, millisecs */

	/* slave stuff */

	average_slave_delta = 1800; // !!! why 1800 ????
	have_first_delta_accumulator = false;
	delta_accumulator_cnt = 0;
	_slave_state = Stopped;

	_engine.GraphReordered.connect_same_thread (*this, boost::bind (&Session::graph_reordered, this));

	/* These are all static "per-class" signals */

	SourceFactory::SourceCreated.connect_same_thread (*this, boost::bind (&Session::add_source, this, _1));
	PlaylistFactory::PlaylistCreated.connect_same_thread (*this, boost::bind (&Session::add_playlist, this, _1, _2));
	AutomationList::AutomationListCreated.connect_same_thread (*this, boost::bind (&Session::add_automation_list, this, _1));
	Controllable::Destroyed.connect_same_thread (*this, boost::bind (&Session::remove_controllable, this, _1));
	IO::PortCountChanged.connect_same_thread (*this, boost::bind (&Session::ensure_buffers, this, _1));

	/* stop IO objects from doing stuff until we're ready for them */

	Delivery::disable_panners ();
	IO::disable_connecting ();
}

int
Session::second_stage_init ()
{
	AudioFileSource::set_peak_dir (_session_dir->peak_path().to_string());

	if (!_is_new) {
		if (load_state (_current_snapshot_name)) {
			return -1;
		}
		remove_empty_sounds ();
	}

	if (_butler->start_thread()) {
		return -1;
	}

	if (start_midi_thread ()) {
		return -1;
	}

	setup_midi_machine_control ();

	// set_state() will call setup_raid_path(), but if it's a new session we need
	// to call setup_raid_path() here.

	if (state_tree) {
		if (set_state (*state_tree->root(), Stateful::loading_state_version)) {
			return -1;
		}
	} else {
		setup_raid_path(_path);
	}

	/* we can't save till after ::when_engine_running() is called,
	   because otherwise we save state with no connections made.
	   therefore, we reset _state_of_the_state because ::set_state()
	   will have cleared it.

	   we also have to include Loading so that any events that get
	   generated between here and the end of ::when_engine_running()
	   will be processed directly rather than queued.
	*/

	_state_of_the_state = StateOfTheState (_state_of_the_state|CannotSave|Loading);

	_locations.changed.connect_same_thread (*this, boost::bind (&Session::locations_changed, this));
	_locations.added.connect_same_thread (*this, boost::bind (&Session::locations_added, this, _1));
	setup_click_sounds (0);
	setup_midi_control ();

	/* Pay attention ... */

	_engine.Halted.connect_same_thread (*this, boost::bind (&Session::engine_halted, this));
	_engine.Xrun.connect_same_thread (*this, boost::bind (&Session::xrun_recovery, this));

	try {
		when_engine_running ();
	}

	/* handle this one in a different way than all others, so that its clear what happened */

	catch (AudioEngine::PortRegistrationFailure& err) {
		error << err.what() << endmsg;
		return -1;
	}

	catch (...) {
		return -1;
	}

	BootMessage (_("Reset Remote Controls"));

	send_full_time_code (0);
	_engine.transport_locate (0);

	_mmc->send (MIDI::MachineControlCommand (MIDI::MachineControl::cmdMmcReset));
	_mmc->send (MIDI::MachineControlCommand (Timecode::Time ()));

	MidiClockTicker::instance().set_session (this);
	MIDI::Name::MidiPatchManager::instance().set_session (this);

	/* initial program change will be delivered later; see ::config_changed() */

	BootMessage (_("Reset Control Protocols"));

	ControlProtocolManager::instance().set_session (this);

	_state_of_the_state = Clean;

	Port::set_connecting_blocked (false);

	DirtyChanged (); /* EMIT SIGNAL */

	if (state_was_pending) {
		save_state (_current_snapshot_name);
		remove_pending_capture_state ();
		state_was_pending = false;
	}

	BootMessage (_("Session loading complete"));

	return 0;
}

string
Session::raid_path () const
{
	SearchPath raid_search_path;

	for (vector<space_and_path>::const_iterator i = session_dirs.begin(); i != session_dirs.end(); ++i) {
		raid_search_path += sys::path((*i).path);
	}

	return raid_search_path.to_string ();
}

void
Session::setup_raid_path (string path)
{
	if (path.empty()) {
		return;
	}

	space_and_path sp;
	string fspath;

	session_dirs.clear ();

	SearchPath search_path(path);
	SearchPath sound_search_path;
	SearchPath midi_search_path;

	for (SearchPath::const_iterator i = search_path.begin(); i != search_path.end(); ++i) {
		sp.path = (*i).to_string ();
		sp.blocks = 0; // not needed
		session_dirs.push_back (sp);

		SessionDirectory sdir(sp.path);

		sound_search_path += sdir.sound_path ();
		midi_search_path += sdir.midi_path ();
	}

	// set the search path for each data type
	FileSource::set_search_path (DataType::AUDIO, sound_search_path.to_string ());
	SMFSource::set_search_path (DataType::MIDI, midi_search_path.to_string ());

	// reset the round-robin soundfile path thingie
	last_rr_session_dir = session_dirs.begin();
}

bool
Session::path_is_within_session (const std::string& path)
{
	for (vector<space_and_path>::const_iterator i = session_dirs.begin(); i != session_dirs.end(); ++i) {
		if (path.find ((*i).path) == 0) {
			return true;
		}
	}
	return false;
}

int
Session::ensure_subdirs ()
{
	string dir;

	dir = session_directory().peak_path().to_string();

	if (g_mkdir_with_parents (dir.c_str(), 0755) < 0) {
		error << string_compose(_("Session: cannot create session peakfile folder \"%1\" (%2)"), dir, strerror (errno)) << endmsg;
		return -1;
	}

	dir = session_directory().sound_path().to_string();

	if (g_mkdir_with_parents (dir.c_str(), 0755) < 0) {
		error << string_compose(_("Session: cannot create session sounds dir \"%1\" (%2)"), dir, strerror (errno)) << endmsg;
		return -1;
	}

	dir = session_directory().midi_path().to_string();

	if (g_mkdir_with_parents (dir.c_str(), 0755) < 0) {
		error << string_compose(_("Session: cannot create session midi dir \"%1\" (%2)"), dir, strerror (errno)) << endmsg;
		return -1;
	}

	dir = session_directory().dead_sound_path().to_string();

	if (g_mkdir_with_parents (dir.c_str(), 0755) < 0) {
		error << string_compose(_("Session: cannot create session dead sounds folder \"%1\" (%2)"), dir, strerror (errno)) << endmsg;
		return -1;
	}

	dir = session_directory().export_path().to_string();

	if (g_mkdir_with_parents (dir.c_str(), 0755) < 0) {
		error << string_compose(_("Session: cannot create session export folder \"%1\" (%2)"), dir, strerror (errno)) << endmsg;
		return -1;
	}

	dir = analysis_dir ();

	if (g_mkdir_with_parents (dir.c_str(), 0755) < 0) {
		error << string_compose(_("Session: cannot create session analysis folder \"%1\" (%2)"), dir, strerror (errno)) << endmsg;
		return -1;
	}

	return 0;
}

int
Session::create (const string& mix_template, BusProfile* bus_profile)
{

	if (g_mkdir_with_parents (_path.c_str(), 0755) < 0) {
		error << string_compose(_("Session: cannot create session folder \"%1\" (%2)"), _path, strerror (errno)) << endmsg;
		return -1;
	}

	if (ensure_subdirs ()) {
		return -1;
	}

	if (!mix_template.empty()) {
		std::string in_path = mix_template;

		ifstream in(in_path.c_str());

		if (in){
			string out_path = _path;
			out_path += _name;
			out_path += statefile_suffix;

			ofstream out(out_path.c_str());

			if (out){
				out << in.rdbuf();
                                _is_new = false;
				return 0;

			} else {
				error << string_compose (_("Could not open %1 for writing mix template"), out_path)
					<< endmsg;
				return -1;
			}

		} else {
			error << string_compose (_("Could not open mix template %1 for reading"), in_path)
				<< endmsg;
			return -1;
		}

	}

	/* Instantiate metadata */

	_metadata = new SessionMetadata ();

	/* set initial start + end point */

	_state_of_the_state = Clean;
        
        /* set up Master Out and Control Out if necessary */

        if (bus_profile) {

		RouteList rl;
		int control_id = 1;
                ChanCount count(DataType::AUDIO, bus_profile->master_out_channels);

		if (bus_profile->master_out_channels) {
			Route* rt = new Route (*this, _("master"), Route::MasterOut, DataType::AUDIO);
                        if (rt->init ()) {
                                delete rt;
                                return -1;
                        }
			boost_debug_shared_ptr_mark_interesting (rt, "Route");
			boost::shared_ptr<Route> r (rt);
			r->input()->ensure_io (count, false, this);
			r->output()->ensure_io (count, false, this);
			r->set_remote_control_id (control_id++);

			rl.push_back (r);

                        if (Config->get_use_monitor_bus()) {
                                Route* rt = new Route (*this, _("monitor"), Route::MonitorOut, DataType::AUDIO);
                                if (rt->init ()) {
                                        delete rt;
                                        return -1;
                                }
                                boost_debug_shared_ptr_mark_interesting (rt, "Route");
                                boost::shared_ptr<Route> r (rt);
                                r->input()->ensure_io (count, false, this);
                                r->output()->ensure_io (count, false, this);
                                r->set_remote_control_id (control_id);
                                
                                rl.push_back (r);
                        }

		} else {
			/* prohibit auto-connect to master, because there isn't one */
			bus_profile->output_ac = AutoConnectOption (bus_profile->output_ac & ~AutoConnectMaster);
		}

		if (!rl.empty()) {
			add_routes (rl, false);
		}

                /* this allows the user to override settings with an environment variable.
                 */

                if (no_auto_connect()) {
                        bus_profile->input_ac = AutoConnectOption (0);
                        bus_profile->output_ac = AutoConnectOption (0);
                }
                
                Config->set_input_auto_connect (bus_profile->input_ac);
                Config->set_output_auto_connect (bus_profile->output_ac);
        }

	save_state ("");

	return 0;
}

void
Session::maybe_write_autosave()
{
        if (dirty() && record_status() != Recording) {
                save_state("", true);
        }
}

void
Session::remove_pending_capture_state ()
{
	sys::path pending_state_file_path(_session_dir->root_path());

	pending_state_file_path /= legalize_for_path (_current_snapshot_name) + pending_suffix;

	try
	{
		sys::remove (pending_state_file_path);
	}
	catch(sys::filesystem_error& ex)
	{
		error << string_compose(_("Could remove pending capture state at path \"%1\" (%2)"),
				pending_state_file_path.to_string(), ex.what()) << endmsg;
	}
}

/** Rename a state file.
 * @param snapshot_name Snapshot name.
 */
void
Session::rename_state (string old_name, string new_name)
{
	if (old_name == _current_snapshot_name || old_name == _name) {
		/* refuse to rename the current snapshot or the "main" one */
		return;
	}

	const string old_xml_filename = legalize_for_path (old_name) + statefile_suffix;
	const string new_xml_filename = legalize_for_path (new_name) + statefile_suffix;

	const sys::path old_xml_path = _session_dir->root_path() / old_xml_filename;
	const sys::path new_xml_path = _session_dir->root_path() / new_xml_filename;

	try
	{
		sys::rename (old_xml_path, new_xml_path);
	}
	catch (const sys::filesystem_error& err)
	{
		error << string_compose(_("could not rename snapshot %1 to %2 (%3)"),
				old_name, new_name, err.what()) << endmsg;
	}
}

/** Remove a state file.
 * @param snapshot_name Snapshot name.
 */
void
Session::remove_state (string snapshot_name)
{
	if (snapshot_name == _current_snapshot_name || snapshot_name == _name) {
		// refuse to remove the current snapshot or the "main" one
		return;
	}

	sys::path xml_path(_session_dir->root_path());

	xml_path /= legalize_for_path (snapshot_name) + statefile_suffix;

	if (!create_backup_file (xml_path)) {
		// don't remove it if a backup can't be made
		// create_backup_file will log the error.
		return;
	}

	// and delete it
	sys::remove (xml_path);
}

#ifdef HAVE_JACK_SESSION
void
Session::jack_session_event (jack_session_event_t * event)
{
        char timebuf[128];
        time_t n;
        struct tm local_time;

        time (&n);
        localtime_r (&n, &local_time);
        strftime (timebuf, sizeof(timebuf), "JS_%FT%T", &local_time);

        if (event->type == JackSessionSaveTemplate)
        {
                if (save_template( timebuf )) {
                        event->flags = JackSessionSaveError; 
                } else {
                        string cmd ("ardour3 -P -U ");
                        cmd += event->client_uuid;
                        cmd += " -T ";
                        cmd += timebuf;

                        event->command_line = strdup (cmd.c_str());
                }
        }
        else
        {
                if (save_state (timebuf)) {
                        event->flags = JackSessionSaveError; 
                } else {
                        sys::path xml_path (_session_dir->root_path());
                        xml_path /= legalize_for_path (timebuf) + statefile_suffix;

                        string cmd ("ardour3 -P -U ");
                        cmd += event->client_uuid;
                        cmd += " \"";
                        cmd += xml_path.to_string();
                        cmd += '\"';

                        event->command_line = strdup (cmd.c_str());
                }
        }

	jack_session_reply (_engine.jack(), event);

	if (event->type == JackSessionSaveAndQuit) {
                // TODO: make ardour quit.
	}

	jack_session_event_free( event );
}
#endif

int
Session::save_state (string snapshot_name, bool pending, bool switch_to_snapshot)
{
	XMLTree tree;
	sys::path xml_path(_session_dir->root_path());

	if (!_writable || (_state_of_the_state & CannotSave)) {
		return 1;
	}

	if (!_engine.connected ()) {
		error << string_compose (_("the %1 audio engine is not connected and state saving would lose all I/O connections. Session not saved"),
                                         PROGRAM_NAME)
		      << endmsg;
		return 1;
	}

	/* tell sources we're saving first, in case they write out to a new file
	 * which should be saved with the state rather than the old one */
	for (SourceMap::const_iterator i = sources.begin(); i != sources.end(); ++i) {
		i->second->session_saved();
        }

	tree.set_root (&get_state());

	if (snapshot_name.empty()) {
		snapshot_name = _current_snapshot_name;
	} else if (switch_to_snapshot) {
                _current_snapshot_name = snapshot_name;
        }

	if (!pending) {

		/* proper save: use statefile_suffix (.ardour in English) */

		xml_path /= legalize_for_path (snapshot_name) + statefile_suffix;

		/* make a backup copy of the old file */

		if (sys::exists(xml_path) && !create_backup_file (xml_path)) {
			// create_backup_file will log the error
			return -1;
		}

	} else {

		/* pending save: use pending_suffix (.pending in English) */
		xml_path /= legalize_for_path (snapshot_name) + pending_suffix;
	}

	sys::path tmp_path(_session_dir->root_path());

	tmp_path /= legalize_for_path (snapshot_name) + temp_suffix;

	// cerr << "actually writing state to " << xml_path.to_string() << endl;

	if (!tree.write (tmp_path.to_string())) {
		error << string_compose (_("state could not be saved to %1"), tmp_path.to_string()) << endmsg;
		sys::remove (tmp_path);
		return -1;

	} else {

		if (rename (tmp_path.to_string().c_str(), xml_path.to_string().c_str()) != 0) {
			error << string_compose (_("could not rename temporary session file %1 to %2"),
					tmp_path.to_string(), xml_path.to_string()) << endmsg;
			sys::remove (tmp_path);
			return -1;
		}
	}

	if (!pending) {

		save_history (snapshot_name);

		bool was_dirty = dirty();

		_state_of_the_state = StateOfTheState (_state_of_the_state & ~Dirty);

		if (was_dirty) {
			DirtyChanged (); /* EMIT SIGNAL */
		}

		StateSaved (snapshot_name); /* EMIT SIGNAL */
	}

	return 0;
}

int
Session::restore_state (string snapshot_name)
{
	if (load_state (snapshot_name) == 0) {
		set_state (*state_tree->root(), Stateful::loading_state_version);
	}

	return 0;
}

int
Session::load_state (string snapshot_name)
{
	delete state_tree;
	state_tree = 0;

	state_was_pending = false;

	/* check for leftover pending state from a crashed capture attempt */

	sys::path xmlpath(_session_dir->root_path());
	xmlpath /= legalize_for_path (snapshot_name) + pending_suffix;

	if (sys::exists (xmlpath)) {

		/* there is pending state from a crashed capture attempt */

                boost::optional<int> r = AskAboutPendingState();
		if (r.get_value_or (1)) {
			state_was_pending = true;
		}
	}

	if (!state_was_pending) {
		xmlpath = _session_dir->root_path();
		xmlpath /= snapshot_name;
	}

        if (!sys::exists (xmlpath)) {
                xmlpath = _session_dir->root_path();
                xmlpath /= legalize_for_path (snapshot_name) + statefile_suffix;
                if (!sys::exists (xmlpath)) {
                        error << string_compose(_("%1: session state information file \"%2\" doesn't exist!"), _name, xmlpath.to_string()) << endmsg;
                        return 1;
                }
        }

	state_tree = new XMLTree;

	set_dirty();

	/* writable() really reflects the whole folder, but if for any
	   reason the session state file can't be written to, still
	   make us unwritable.
	*/

	if (::access (xmlpath.to_string().c_str(), W_OK) != 0) {
		_writable = false;
	}

	if (!state_tree->read (xmlpath.to_string())) {
		error << string_compose(_("Could not understand ardour file %1"), xmlpath.to_string()) << endmsg;
		delete state_tree;
		state_tree = 0;
		return -1;
	}

	XMLNode& root (*state_tree->root());

	if (root.name() != X_("Session")) {
		error << string_compose (_("Session file %1 is not a session"), xmlpath.to_string()) << endmsg;
		delete state_tree;
		state_tree = 0;
		return -1;
	}

	const XMLProperty* prop;

	if ((prop = root.property ("version")) == 0) {
		/* no version implies very old version of Ardour */
		Stateful::loading_state_version = 1000;
	} else {
		int major;
		int minor;
		int micro;

		sscanf (prop->value().c_str(), "%d.%d.%d", &major, &minor, &micro);
		Stateful::loading_state_version = (major * 1000) + minor;
	}
		
	if (Stateful::loading_state_version < CURRENT_SESSION_FILE_VERSION) {

		sys::path backup_path(_session_dir->root_path());

		backup_path /= legalize_for_path (snapshot_name) + "-1" + statefile_suffix;

		// only create a backup once
		if (sys::exists (backup_path)) {
			return 0;
		}

		info << string_compose (_("Copying old session file %1 to %2\nUse %2 with %3 versions before 2.0 from now on"),
					xmlpath.to_string(), backup_path.to_string(), PROGRAM_NAME)
		     << endmsg;

		try
		{
			sys::copy_file (xmlpath, backup_path);
		}
		catch(sys::filesystem_error& ex)
		{
			error << string_compose (_("Unable to make backup of state file %1 (%2)"),
					xmlpath.to_string(), ex.what())
				<< endmsg;
			return -1;
		}
	}

	return 0;
}

int
Session::load_options (const XMLNode& node)
{
	LocaleGuard lg (X_("POSIX"));
	config.set_variables (node);
	return 0;
}

XMLNode&
Session::get_state()
{
	return state(true);
}

XMLNode&
Session::get_template()
{
	/* if we don't disable rec-enable, diskstreams
	   will believe they need to store their capture
	   sources in their state node.
	*/

	disable_record (false);

	return state(false);
}

XMLNode&
Session::state(bool full_state)
{
	XMLNode* node = new XMLNode("Session");
	XMLNode* child;

	// store libardour version, just in case
	char buf[16];
	snprintf(buf, sizeof(buf), "%d.%d.%d", libardour3_major_version, libardour3_minor_version, libardour3_micro_version);
	node->add_property("version", string(buf));

	/* store configuration settings */

	if (full_state) {

		node->add_property ("name", _name);
		snprintf (buf, sizeof (buf), "%" PRId32, _nominal_frame_rate);
		node->add_property ("sample-rate", buf);

		if (session_dirs.size() > 1) {

			string p;

			vector<space_and_path>::iterator i = session_dirs.begin();
			vector<space_and_path>::iterator next;

			++i; /* skip the first one */
			next = i;
			++next;

			while (i != session_dirs.end()) {

				p += (*i).path;

				if (next != session_dirs.end()) {
					p += ':';
				} else {
					break;
				}

				++next;
				++i;
			}

			child = node->add_child ("Path");
			child->add_content (p);
		}
	}

	/* save the ID counter */

	snprintf (buf, sizeof (buf), "%" PRIu64, ID::counter());
	node->add_property ("id-counter", buf);

	/* various options */

	node->add_child_nocopy (config.get_variables ());

	node->add_child_nocopy (_metadata->get_state());

	child = node->add_child ("Sources");

	if (full_state) {
		Glib::Mutex::Lock sl (source_lock);

		for (SourceMap::iterator siter = sources.begin(); siter != sources.end(); ++siter) {

			/* Don't save information about non-destructive file sources that are empty
                           and unused by any regions.
                        */

			boost::shared_ptr<FileSource> fs;
			if ((fs = boost::dynamic_pointer_cast<FileSource> (siter->second)) != 0) {
				if (!fs->destructive()) {
					if (fs->empty() && !fs->used()) {
						continue;
					}
				}
			}

			child->add_child_nocopy (siter->second->get_state());
		}
	}

	child = node->add_child ("Regions");

	if (full_state) {
		Glib::Mutex::Lock rl (region_lock);
                const RegionFactory::RegionMap& region_map (RegionFactory::all_regions());
                for (RegionFactory::RegionMap::const_iterator i = region_map.begin(); i != region_map.end(); ++i) {
                        boost::shared_ptr<Region> r = i->second;
                        /* only store regions not attached to playlists */
                        if (r->playlist() == 0) {
                                child->add_child_nocopy (r->state (true));
                        }
                }
	}

	if (full_state) {
		node->add_child_nocopy (_locations.get_state());
	} else {
		// for a template, just create a new Locations, populate it
		// with the default start and end, and get the state for that.
		Locations loc;
		Location* range = new Location (0, 0, _("session"), Location::IsSessionRange);
		range->set (max_frames, 0);
		loc.add (range);
		node->add_child_nocopy (loc.get_state());
	}

	child = node->add_child ("Bundles");
	{
		boost::shared_ptr<BundleList> bundles = _bundles.reader ();
		for (BundleList::iterator i = bundles->begin(); i != bundles->end(); ++i) {
			boost::shared_ptr<UserBundle> b = boost::dynamic_pointer_cast<UserBundle> (*i);
			if (b) {
				child->add_child_nocopy (b->get_state());
			}
		}
	}

	child = node->add_child ("Routes");
	{
		boost::shared_ptr<RouteList> r = routes.reader ();

		RoutePublicOrderSorter cmp;
		RouteList public_order (*r);
		public_order.sort (cmp);

                /* the sort should have put control outs first */

                if (_monitor_out) {
                        assert (_monitor_out == public_order.front());
                }

		for (RouteList::iterator i = public_order.begin(); i != public_order.end(); ++i) {
			if (!(*i)->is_hidden()) {
				if (full_state) {
					child->add_child_nocopy ((*i)->get_state());
				} else {
					child->add_child_nocopy ((*i)->get_template());
				}
			}
		}
	}

	playlists->add_state (node, full_state);

	child = node->add_child ("RouteGroups");
	for (list<RouteGroup *>::iterator i = _route_groups.begin(); i != _route_groups.end(); ++i) {
		child->add_child_nocopy ((*i)->get_state());
	}

	if (_click_io) {
		child = node->add_child ("Click");
		child->add_child_nocopy (_click_io->state (full_state));
	}

	if (full_state) {
		child = node->add_child ("NamedSelections");
		for (NamedSelectionList::iterator i = named_selections.begin(); i != named_selections.end(); ++i) {
			if (full_state) {
				child->add_child_nocopy ((*i)->get_state());
			}
		}
	}

	node->add_child_nocopy (_tempo_map->get_state());

	node->add_child_nocopy (get_control_protocol_state());

	if (_extra_xml) {
		node->add_child_copy (*_extra_xml);
	}

	return *node;
}

XMLNode&
Session::get_control_protocol_state ()
{
	ControlProtocolManager& cpm (ControlProtocolManager::instance());
	return cpm.get_state();
}

int
Session::set_state (const XMLNode& node, int version)
{
	XMLNodeList nlist;
	XMLNode* child;
	const XMLProperty* prop;
	int ret = -1;

	_state_of_the_state = StateOfTheState (_state_of_the_state|CannotSave);

	if (node.name() != X_("Session")){
		fatal << _("programming error: Session: incorrect XML node sent to set_state()") << endmsg;
		return -1;
	}

	if ((prop = node.property ("version")) != 0) {
		version = atoi (prop->value ()) * 1000;
	}

	if ((prop = node.property ("name")) != 0) {
		_name = prop->value ();
	}

	if ((prop = node.property (X_("sample-rate"))) != 0) {

		_nominal_frame_rate = atoi (prop->value());

		if (_nominal_frame_rate != _current_frame_rate) {
                        boost::optional<int> r = AskAboutSampleRateMismatch (_nominal_frame_rate, _current_frame_rate);
			if (r.get_value_or (0)) {
				return -1;
			}
		}
	}

	setup_raid_path(_session_dir->root_path().to_string());

	if ((prop = node.property (X_("id-counter"))) != 0) {
		uint64_t x;
		sscanf (prop->value().c_str(), "%" PRIu64, &x);
		ID::init_counter (x);
	} else {
		/* old sessions used a timebased counter, so fake
		   the startup ID counter based on a standard
		   timestamp.
		*/
		time_t now;
		time (&now);
		ID::init_counter (now);
	}


	IO::disable_connecting ();

	/* Object loading order:

	Path
	Extra
	Options/Config
	MIDI Control // relies on data from Options/Config
	Metadata
	Locations
	Sources
	AudioRegions
	Connections
	Routes
	RouteGroups
	MixGroups
	Click
	ControlProtocols
	*/

	if ((child = find_named_node (node, "Extra")) != 0) {
		_extra_xml = new XMLNode (*child);
	}

	if (((child = find_named_node (node, "Options")) != 0)) { /* old style */
		load_options (*child);
	} else if ((child = find_named_node (node, "Config")) != 0) { /* new style */
		load_options (*child);
	} else {
		error << _("Session: XML state has no options section") << endmsg;
	}

	use_config_midi_ports ();

	if (version >= 3000) {
		if ((child = find_named_node (node, "Metadata")) == 0) {
			warning << _("Session: XML state has no metadata section") << endmsg;
		} else if (_metadata->set_state (*child, version)) {
			goto out;
		}
	}

	if ((child = find_named_node (node, "Locations")) == 0) {
		error << _("Session: XML state has no locations section") << endmsg;
		goto out;
	} else if (_locations.set_state (*child, version)) {
		goto out;
	}

	Location* location;

	if ((location = _locations.auto_loop_location()) != 0) {
		set_auto_loop_location (location);
	}

	if ((location = _locations.auto_punch_location()) != 0) {
		set_auto_punch_location (location);
	}

	if ((location = _locations.session_range_location()) != 0) {
		delete _session_range_location;
		_session_range_location = location;
	}

	if (_session_range_location) {
		AudioFileSource::set_header_position_offset (_session_range_location->start());
	}

	if ((child = find_named_node (node, "Sources")) == 0) {
		error << _("Session: XML state has no sources section") << endmsg;
		goto out;
	} else if (load_sources (*child)) {
		goto out;
	}

	if ((child = find_named_node (node, "Regions")) == 0) {
		error << _("Session: XML state has no Regions section") << endmsg;
		goto out;
	} else if (load_regions (*child)) {
		goto out;
	}

	if ((child = find_named_node (node, "Playlists")) == 0) {
		error << _("Session: XML state has no playlists section") << endmsg;
		goto out;
	} else if (playlists->load (*this, *child)) {
		goto out;
	}

	if ((child = find_named_node (node, "UnusedPlaylists")) == 0) {
		// this is OK
	} else if (playlists->load_unused (*this, *child)) {
		goto out;
	}
	
	if ((child = find_named_node (node, "NamedSelections")) != 0) {
		if (load_named_selections (*child)) {
			goto out;
		}
	}

	if (version >= 3000) {
		if ((child = find_named_node (node, "Bundles")) == 0) {
			warning << _("Session: XML state has no bundles section") << endmsg;
			//goto out;
		} else {
			/* We can't load Bundles yet as they need to be able
			   to convert from port names to Port objects, which can't happen until
			   later */
			_bundle_xml_node = new XMLNode (*child);
		}
	}
	
	if ((child = find_named_node (node, "TempoMap")) == 0) {
		error << _("Session: XML state has no Tempo Map section") << endmsg;
		goto out;
	} else if (_tempo_map->set_state (*child, version)) {
		goto out;
	}

	if (version < 3000) {
		if ((child = find_named_node (node, X_("DiskStreams"))) == 0) {
			error << _("Session: XML state has no diskstreams section") << endmsg;
			goto out;
		} else if (load_diskstreams_2X (*child, version)) {
			goto out;
		}
	}

	if ((child = find_named_node (node, "Routes")) == 0) {
		error << _("Session: XML state has no routes section") << endmsg;
		goto out;
	} else if (load_routes (*child, version)) {
		goto out;
	}

	/* our diskstreams list is no longer needed as they are now all owned by their Route */
	_diskstreams_2X.clear ();

	if (version >= 3000) {
		
		if ((child = find_named_node (node, "RouteGroups")) == 0) {
			error << _("Session: XML state has no route groups section") << endmsg;
			goto out;
		} else if (load_route_groups (*child, version)) {
			goto out;
		}
		
	} else if (version < 3000) {
		
		if ((child = find_named_node (node, "EditGroups")) == 0) {
			error << _("Session: XML state has no edit groups section") << endmsg;
			goto out;
		} else if (load_route_groups (*child, version)) {
			goto out;
		}

		if ((child = find_named_node (node, "MixGroups")) == 0) {
			error << _("Session: XML state has no mix groups section") << endmsg;
			goto out;
		} else if (load_route_groups (*child, version)) {
			goto out;
		}
	}

	if ((child = find_named_node (node, "Click")) == 0) {
		warning << _("Session: XML state has no click section") << endmsg;
	} else if (_click_io) {
		_click_io->set_state (*child, version);
	}

	if ((child = find_named_node (node, "ControlProtocols")) != 0) {
		ControlProtocolManager::instance().set_protocol_states (*child);
	}

	/* here beginneth the second phase ... */

	StateReady (); /* EMIT SIGNAL */

	return 0;

  out:
	return ret;
}

int
Session::load_routes (const XMLNode& node, int version)
{
	XMLNodeList nlist;
	XMLNodeConstIterator niter;
	RouteList new_routes;

	nlist = node.children();

	set_dirty();

	for (niter = nlist.begin(); niter != nlist.end(); ++niter) {

		boost::shared_ptr<Route> route;
		if (version < 3000) {
			route = XMLRouteFactory_2X (**niter, version);
		} else {
			route = XMLRouteFactory (**niter, version);
		}
		
		if (route == 0) {
			error << _("Session: cannot create Route from XML description.") << endmsg;
			return -1;
		}

		BootMessage (string_compose (_("Loaded track/bus %1"), route->name()));

		new_routes.push_back (route);
	}

	add_routes (new_routes, false);

	return 0;
}

boost::shared_ptr<Route>
Session::XMLRouteFactory (const XMLNode& node, int version)
{
	boost::shared_ptr<Route> ret;

	if (node.name() != "Route") {
		return ret;
	}

	XMLNode* ds_child = find_named_node (node, X_("Diskstream"));

	DataType type = DataType::AUDIO;
	const XMLProperty* prop = node.property("default-type");

	if (prop) {
		type = DataType (prop->value());
	}

	assert (type != DataType::NIL);

	if (ds_child) {

                Track* track;
                
                if (type == DataType::AUDIO) {
                        track = new AudioTrack (*this, X_("toBeResetFroXML"));
                        
                } else {
                        track = new MidiTrack (*this, X_("toBeResetFroXML"));
                }
                
                if (track->init()) {
                        delete track;
                        return ret;
                }
                
                if (track->set_state (node, version)) {
                        delete track;
                        return ret;
                }
                
                boost_debug_shared_ptr_mark_interesting (track, "Track");
                ret.reset (track);
                
	} else {
		Route* rt = new Route (*this, X_("toBeResetFroXML"));

                if (rt->init () == 0 && rt->set_state (node, version) == 0) {
                        boost_debug_shared_ptr_mark_interesting (rt, "Route");
                        ret.reset (rt);
                } else {
                        delete rt;
                }
	}

	return ret;
}

boost::shared_ptr<Route>
Session::XMLRouteFactory_2X (const XMLNode& node, int version)
{
	boost::shared_ptr<Route> ret;

	if (node.name() != "Route") {
		return ret;
	}

	XMLProperty const * ds_prop = node.property (X_("diskstream-id"));
	if (!ds_prop) {
		ds_prop = node.property (X_("diskstream"));
	}

	DataType type = DataType::AUDIO;
	const XMLProperty* prop = node.property("default-type");

	if (prop) {
		type = DataType (prop->value());
	}

	assert (type != DataType::NIL);

	if (ds_prop) {

		list<boost::shared_ptr<Diskstream> >::iterator i = _diskstreams_2X.begin ();
		while (i != _diskstreams_2X.end() && (*i)->id() != ds_prop->value()) {
			++i;
		}

		if (i == _diskstreams_2X.end()) {
			error << _("Could not find diskstream for route") << endmsg;
			return boost::shared_ptr<Route> ();
		}

                Track* track;
                
                if (type == DataType::AUDIO) {
                        track = new AudioTrack (*this, X_("toBeResetFroXML"));
                        
                } else {
                        track = new MidiTrack (*this, X_("toBeResetFroXML"));
                }
                
                if (track->init()) {
                        delete track;
                        return ret;
                }
                
                if (track->set_state (node, version)) {
                        delete track;
                        return ret;
                }

		track->set_diskstream (*i);
                
                boost_debug_shared_ptr_mark_interesting (track, "Track");
                ret.reset (track);
                
	} else {
		Route* rt = new Route (*this, X_("toBeResetFroXML"));

                if (rt->init () == 0 && rt->set_state (node, version) == 0) {
                        boost_debug_shared_ptr_mark_interesting (rt, "Route");
                        ret.reset (rt);
                } else {
                        delete rt;
                }
	}

	return ret;
}

int
Session::load_regions (const XMLNode& node)
{
	XMLNodeList nlist;
	XMLNodeConstIterator niter;
	boost::shared_ptr<Region> region;

	nlist = node.children();

	set_dirty();

	for (niter = nlist.begin(); niter != nlist.end(); ++niter) {
		if ((region = XMLRegionFactory (**niter, false)) == 0) {
			error << _("Session: cannot create Region from XML description.");
			const XMLProperty *name = (**niter).property("name");

			if (name) {
				error << " " << string_compose (_("Can not load state for region '%1'"), name->value());
			}

			error << endmsg;
		}
	}

	return 0;
}

boost::shared_ptr<Region>
Session::XMLRegionFactory (const XMLNode& node, bool full)
{
	const XMLProperty* type = node.property("type");

	try {

	if ( !type || type->value() == "audio" ) {

		return boost::shared_ptr<Region>(XMLAudioRegionFactory (node, full));

	} else if (type->value() == "midi") {

		return boost::shared_ptr<Region>(XMLMidiRegionFactory (node, full));

	}

	} catch (failed_constructor& err) {
		return boost::shared_ptr<Region> ();
	}

	return boost::shared_ptr<Region> ();
}

boost::shared_ptr<AudioRegion>
Session::XMLAudioRegionFactory (const XMLNode& node, bool /*full*/)
{
	const XMLProperty* prop;
	boost::shared_ptr<Source> source;
	boost::shared_ptr<AudioSource> as;
	SourceList sources;
	SourceList master_sources;
	uint32_t nchans = 1;
	char buf[128];

	if (node.name() != X_("Region")) {
		return boost::shared_ptr<AudioRegion>();
	}

	if ((prop = node.property (X_("channels"))) != 0) {
		nchans = atoi (prop->value().c_str());
	}

	if ((prop = node.property ("name")) == 0) {
		cerr << "no name for this region\n";
		abort ();
	}

	if ((prop = node.property (X_("source-0"))) == 0) {
		if ((prop = node.property ("source")) == 0) {
			error << _("Session: XMLNode describing a AudioRegion is incomplete (no source)") << endmsg;
			return boost::shared_ptr<AudioRegion>();
		}
	}

	PBD::ID s_id (prop->value());

	if ((source = source_by_id (s_id)) == 0) {
		error << string_compose(_("Session: XMLNode describing a AudioRegion references an unknown source id =%1"), s_id) << endmsg;
		return boost::shared_ptr<AudioRegion>();
	}

	as = boost::dynamic_pointer_cast<AudioSource>(source);
	if (!as) {
		error << string_compose(_("Session: XMLNode describing a AudioRegion references a non-audio source id =%1"), s_id) << endmsg;
		return boost::shared_ptr<AudioRegion>();
	}

	sources.push_back (as);

	/* pickup other channels */

	for (uint32_t n=1; n < nchans; ++n) {
		snprintf (buf, sizeof(buf), X_("source-%d"), n);
		if ((prop = node.property (buf)) != 0) {

			PBD::ID id2 (prop->value());

			if ((source = source_by_id (id2)) == 0) {
				error << string_compose(_("Session: XMLNode describing a AudioRegion references an unknown source id =%1"), id2) << endmsg;
				return boost::shared_ptr<AudioRegion>();
			}

			as = boost::dynamic_pointer_cast<AudioSource>(source);
			if (!as) {
				error << string_compose(_("Session: XMLNode describing a AudioRegion references a non-audio source id =%1"), id2) << endmsg;
				return boost::shared_ptr<AudioRegion>();
			}
			sources.push_back (as);
		}
	}

	for (uint32_t n = 0; n < nchans; ++n) {
		snprintf (buf, sizeof(buf), X_("master-source-%d"), n);
		if ((prop = node.property (buf)) != 0) {

			PBD::ID id2 (prop->value());

			if ((source = source_by_id (id2)) == 0) {
				error << string_compose(_("Session: XMLNode describing a AudioRegion references an unknown source id =%1"), id2) << endmsg;
				return boost::shared_ptr<AudioRegion>();
			}

			as = boost::dynamic_pointer_cast<AudioSource>(source);
			if (!as) {
				error << string_compose(_("Session: XMLNode describing a AudioRegion references a non-audio source id =%1"), id2) << endmsg;
				return boost::shared_ptr<AudioRegion>();
			}
			master_sources.push_back (as);
		}
	}

	try {
		boost::shared_ptr<AudioRegion> region (boost::dynamic_pointer_cast<AudioRegion> (RegionFactory::create (sources, node)));

		/* a final detail: this is the one and only place that we know how long missing files are */

		if (region->whole_file()) {
			for (SourceList::iterator sx = sources.begin(); sx != sources.end(); ++sx) {
				boost::shared_ptr<SilentFileSource> sfp = boost::dynamic_pointer_cast<SilentFileSource> (*sx);
				if (sfp) {
					sfp->set_length (region->length());
				}
			}
		}

		if (!master_sources.empty()) {
			if (master_sources.size() != nchans) {
				error << _("Session: XMLNode describing an AudioRegion is missing some master sources; ignored") << endmsg;
			} else {
				region->set_master_sources (master_sources);
			}
		}

		return region;

	}

	catch (failed_constructor& err) {
		return boost::shared_ptr<AudioRegion>();
	}
}

boost::shared_ptr<MidiRegion>
Session::XMLMidiRegionFactory (const XMLNode& node, bool /*full*/)
{
	const XMLProperty* prop;
	boost::shared_ptr<Source> source;
	boost::shared_ptr<MidiSource> ms;
	SourceList sources;
	uint32_t nchans = 1;

	if (node.name() != X_("Region")) {
		return boost::shared_ptr<MidiRegion>();
	}

	if ((prop = node.property (X_("channels"))) != 0) {
		nchans = atoi (prop->value().c_str());
	}

	if ((prop = node.property ("name")) == 0) {
		cerr << "no name for this region\n";
		abort ();
	}

	// Multiple midi channels?  that's just crazy talk
	assert(nchans == 1);

	if ((prop = node.property (X_("source-0"))) == 0) {
		if ((prop = node.property ("source")) == 0) {
			error << _("Session: XMLNode describing a MidiRegion is incomplete (no source)") << endmsg;
			return boost::shared_ptr<MidiRegion>();
		}
	}

	PBD::ID s_id (prop->value());

	if ((source = source_by_id (s_id)) == 0) {
		error << string_compose(_("Session: XMLNode describing a MidiRegion references an unknown source id =%1"), s_id) << endmsg;
		return boost::shared_ptr<MidiRegion>();
	}

	ms = boost::dynamic_pointer_cast<MidiSource>(source);
	if (!ms) {
		error << string_compose(_("Session: XMLNode describing a MidiRegion references a non-midi source id =%1"), s_id) << endmsg;
		return boost::shared_ptr<MidiRegion>();
	}

	sources.push_back (ms);

	try {
		boost::shared_ptr<MidiRegion> region (boost::dynamic_pointer_cast<MidiRegion> (RegionFactory::create (sources, node)));
		/* a final detail: this is the one and only place that we know how long missing files are */

		if (region->whole_file()) {
			for (SourceList::iterator sx = sources.begin(); sx != sources.end(); ++sx) {
				boost::shared_ptr<SilentFileSource> sfp = boost::dynamic_pointer_cast<SilentFileSource> (*sx);
				if (sfp) {
					sfp->set_length (region->length());
				}
			}
		}

		return region;
	}

	catch (failed_constructor& err) {
		return boost::shared_ptr<MidiRegion>();
	}
}

XMLNode&
Session::get_sources_as_xml ()

{
	XMLNode* node = new XMLNode (X_("Sources"));
	Glib::Mutex::Lock lm (source_lock);

	for (SourceMap::iterator i = sources.begin(); i != sources.end(); ++i) {
		node->add_child_nocopy (i->second->get_state());
	}

	return *node;
}

string
Session::path_from_region_name (DataType type, string name, string identifier)
{
	char buf[PATH_MAX+1];
	uint32_t n;
	SessionDirectory sdir(get_best_session_directory_for_new_source());
	sys::path source_dir = ((type == DataType::AUDIO)
		? sdir.sound_path() : sdir.midi_path());

	string ext = ((type == DataType::AUDIO) ? ".wav" : ".mid");

	for (n = 0; n < 999999; ++n) {
		if (identifier.length()) {
			snprintf (buf, sizeof(buf), "%s%s%" PRIu32 "%s", name.c_str(),
				  identifier.c_str(), n, ext.c_str());
		} else {
			snprintf (buf, sizeof(buf), "%s-%" PRIu32 "%s", name.c_str(),
					n, ext.c_str());
		}

		sys::path source_path = source_dir / buf;

		if (!sys::exists (source_path)) {
			return source_path.to_string();
		}
	}

	error << string_compose (_("cannot create new file from region name \"%1\" with ident = \"%2\": too many existing files with similar names"),
				 name, identifier)
	      << endmsg;

	return "";
}


int
Session::load_sources (const XMLNode& node)
{
	XMLNodeList nlist;
	XMLNodeConstIterator niter;
	boost::shared_ptr<Source> source;

	nlist = node.children();

	set_dirty();

	for (niter = nlist.begin(); niter != nlist.end(); ++niter) {
		try {
			if ((source = XMLSourceFactory (**niter)) == 0) {
				error << _("Session: cannot create Source from XML description.") << endmsg;
			}
		} catch (MissingSource& err) {
			warning << _("A sound file is missing. It will be replaced by silence.") << endmsg;
			source = SourceFactory::createSilent (*this, **niter, max_frames, _current_frame_rate);
		}
	}

	return 0;
}

boost::shared_ptr<Source>
Session::XMLSourceFactory (const XMLNode& node)
{
	if (node.name() != "Source") {
		return boost::shared_ptr<Source>();
	}

	try {
		/* note: do peak building in another thread when loading session state */
		return SourceFactory::create (*this, node, true);
	}

	catch (failed_constructor& err) {
		error << string_compose (_("Found a sound file that cannot be used by %1. Talk to the progammers."), PROGRAM_NAME) << endmsg;
		return boost::shared_ptr<Source>();
	}
}

int
Session::save_template (string template_name)
{
	XMLTree tree;

	if (_state_of_the_state & CannotSave) {
		return -1;
	}

	sys::path user_template_dir(user_template_directory());

	try
	{
		sys::create_directories (user_template_dir);
	}
	catch(sys::filesystem_error& ex)
	{
		error << string_compose(_("Could not create mix templates directory \"%1\" (%2)"),
				user_template_dir.to_string(), ex.what()) << endmsg;
		return -1;
	}

	tree.set_root (&get_template());

	sys::path template_file_path(user_template_dir);
	template_file_path /= template_name + template_suffix;

	if (sys::exists (template_file_path))
	{
		warning << string_compose(_("Template \"%1\" already exists - new version not created"),
				template_file_path.to_string()) << endmsg;
		return -1;
	}

	if (!tree.write (template_file_path.to_string())) {
		error << _("mix template not saved") << endmsg;
		return -1;
	}

	return 0;
}

int
Session::rename_template (string old_name, string new_name)
{
	sys::path old_path (user_template_directory());
	old_path /= old_name + template_suffix;

	sys::path new_path(user_template_directory());
	new_path /= new_name + template_suffix;

	if (sys::exists (new_path)) {
		warning << string_compose(_("Template \"%1\" already exists - template not renamed"),
					  new_path.to_string()) << endmsg;
		return -1;
	}

	try {
		sys::rename (old_path, new_path);
		return 0;
	} catch (...) {
		return -1;
	}
}

int
Session::delete_template (string name)
{
	sys::path path = user_template_directory();
	path /= name + template_suffix;

	try {
		sys::remove (path);
		return 0;
	} catch (...) {
		return -1;
	}
}

void
Session::refresh_disk_space ()
{
#if HAVE_SYS_VFS_H
	struct statfs statfsbuf;
	vector<space_and_path>::iterator i;
	Glib::Mutex::Lock lm (space_lock);
	double scale;

	/* get freespace on every FS that is part of the session path */

	_total_free_4k_blocks = 0;

	for (i = session_dirs.begin(); i != session_dirs.end(); ++i) {
		statfs ((*i).path.c_str(), &statfsbuf);

		scale = statfsbuf.f_bsize/4096.0;

		(*i).blocks = (uint32_t) floor (statfsbuf.f_bavail * scale);
		_total_free_4k_blocks += (*i).blocks;
	}
#endif
}

string
Session::get_best_session_directory_for_new_source ()
{
	vector<space_and_path>::iterator i;
	string result = _session_dir->root_path().to_string();

	/* handle common case without system calls */

	if (session_dirs.size() == 1) {
		return result;
	}

	/* OK, here's the algorithm we're following here:

	We want to select which directory to use for
	the next file source to be created. Ideally,
	we'd like to use a round-robin process so as to
	get maximum performance benefits from splitting
	the files across multiple disks.

	However, in situations without much diskspace, an
	RR approach may end up filling up a filesystem
	with new files while others still have space.
	Its therefore important to pay some attention to
	the freespace in the filesystem holding each
	directory as well. However, if we did that by
	itself, we'd keep creating new files in the file
	system with the most space until it was as full
	as all others, thus negating any performance
	benefits of this RAID-1 like approach.

	So, we use a user-configurable space threshold. If
	there are at least 2 filesystems with more than this
	much space available, we use RR selection between them.
	If not, then we pick the filesystem with the most space.

	This gets a good balance between the two
	approaches.
	*/

	refresh_disk_space ();

	int free_enough = 0;

	for (i = session_dirs.begin(); i != session_dirs.end(); ++i) {
		if ((*i).blocks * 4096 >= Config->get_disk_choice_space_threshold()) {
			free_enough++;
		}
	}

	if (free_enough >= 2) {
		/* use RR selection process, ensuring that the one
		   picked works OK.
		*/

		i = last_rr_session_dir;

		do {
			if (++i == session_dirs.end()) {
				i = session_dirs.begin();
			}

			if ((*i).blocks * 4096 >= Config->get_disk_choice_space_threshold()) {
				if (create_session_directory ((*i).path)) {
					result = (*i).path;
					last_rr_session_dir = i;
					return result;
				}
			}

		} while (i != last_rr_session_dir);

	} else {

		/* pick FS with the most freespace (and that
		   seems to actually work ...)
		*/

		vector<space_and_path> sorted;
		space_and_path_ascending_cmp cmp;

		sorted = session_dirs;
		sort (sorted.begin(), sorted.end(), cmp);

		for (i = sorted.begin(); i != sorted.end(); ++i) {
			if (create_session_directory ((*i).path)) {
				result = (*i).path;
				last_rr_session_dir = i;
				return result;
			}
		}
	}

	return result;
}

int
Session::load_named_selections (const XMLNode& node)
{
	XMLNodeList nlist;
	XMLNodeConstIterator niter;
	NamedSelection *ns;

	nlist = node.children();

	set_dirty();

	for (niter = nlist.begin(); niter != nlist.end(); ++niter) {

		if ((ns = XMLNamedSelectionFactory (**niter)) == 0) {
			error << _("Session: cannot create Named Selection from XML description.") << endmsg;
		}
	}

	return 0;
}

NamedSelection *
Session::XMLNamedSelectionFactory (const XMLNode& node)
{
	try {
		return new NamedSelection (*this, node);
	}

	catch (failed_constructor& err) {
		return 0;
	}
}

string
Session::automation_dir () const
{
	return Glib::build_filename (_path, "automation");
}

string
Session::analysis_dir () const
{
	return Glib::build_filename (_path, "analysis");
}

int
Session::load_bundles (XMLNode const & node)
{
	XMLNodeList nlist = node.children();
	XMLNodeConstIterator niter;

	set_dirty();

	for (niter = nlist.begin(); niter != nlist.end(); ++niter) {
		if ((*niter)->name() == "InputBundle") {
			add_bundle (boost::shared_ptr<UserBundle> (new UserBundle (**niter, true)));
		} else if ((*niter)->name() == "OutputBundle") {
			add_bundle (boost::shared_ptr<UserBundle> (new UserBundle (**niter, false)));
		} else {
			error << string_compose(_("Unknown node \"%1\" found in Bundles list from state file"), (*niter)->name()) << endmsg;
			return -1;
		}
	}

	return 0;
}

int
Session::load_route_groups (const XMLNode& node, int version)
{
	XMLNodeList nlist = node.children();
	XMLNodeConstIterator niter;

	set_dirty ();

	if (version >= 3000) {
		
		for (niter = nlist.begin(); niter != nlist.end(); ++niter) {
			if ((*niter)->name() == "RouteGroup") {
				RouteGroup* rg = new RouteGroup (*this, "");
				add_route_group (rg);
				rg->set_state (**niter, version);
			}
		}

	} else if (version < 3000) {

		for (niter = nlist.begin(); niter != nlist.end(); ++niter) {
			if ((*niter)->name() == "EditGroup" || (*niter)->name() == "MixGroup") {
				RouteGroup* rg = new RouteGroup (*this, "");
				add_route_group (rg);
				rg->set_state (**niter, version);
			}
		}
	}

	return 0;
}

void
Session::auto_save()
{
	save_state (_current_snapshot_name);
}

static bool
state_file_filter (const string &str, void */*arg*/)
{
	return (str.length() > strlen(statefile_suffix) &&
		str.find (statefile_suffix) == (str.length() - strlen (statefile_suffix)));
}

struct string_cmp {
	bool operator()(const string* a, const string* b) {
		return *a < *b;
	}
};

static string*
remove_end(string* state)
{
	string statename(*state);

	string::size_type start,end;
	if ((start = statename.find_last_of ('/')) != string::npos) {
		statename = statename.substr (start+1);
	}

	if ((end = statename.rfind(".ardour")) == string::npos) {
		end = statename.length();
	}

	return new string(statename.substr (0, end));
}

vector<string *> *
Session::possible_states (string path)
{
	PathScanner scanner;
	vector<string*>* states = scanner (path, state_file_filter, 0, false, false);

	transform(states->begin(), states->end(), states->begin(), remove_end);

	string_cmp cmp;
	sort (states->begin(), states->end(), cmp);

	return states;
}

vector<string *> *
Session::possible_states () const
{
	return possible_states(_path);
}

void
Session::add_route_group (RouteGroup* g)
{
	_route_groups.push_back (g);
	route_group_added (g); /* EMIT SIGNAL */

	g->MembershipChanged.connect_same_thread (*this, boost::bind (&Session::route_group_changed, this));
	g->PropertyChanged.connect_same_thread (*this, boost::bind (&Session::route_group_changed, this));
	
	set_dirty ();
}

void
Session::remove_route_group (RouteGroup& rg)
{
	list<RouteGroup*>::iterator i;

	if ((i = find (_route_groups.begin(), _route_groups.end(), &rg)) != _route_groups.end()) {
		_route_groups.erase (i);
		delete &rg;

		route_group_removed (); /* EMIT SIGNAL */
	}

}

RouteGroup *
Session::route_group_by_name (string name)
{
	list<RouteGroup *>::iterator i;

	for (i = _route_groups.begin(); i != _route_groups.end(); ++i) {
		if ((*i)->name() == name) {
			return* i;
		}
	}
	return 0;
}

UndoTransaction*
Session::start_reversible_command (const string& name)
{
	UndoTransaction* trans = new UndoTransaction();
	trans->set_name(name);
	return trans;
}

void
Session::finish_reversible_command (UndoTransaction& ut)
{
	struct timeval now;
	gettimeofday(&now, 0);
	ut.set_timestamp(now);
	_history.add (&ut);
}

void
Session::begin_reversible_command(const string& name)
{
	UndoTransaction* trans = new UndoTransaction();
	trans->set_name(name);

	if (!_current_trans.empty()) {
		_current_trans.top()->add_command (trans);
	} else {
		_current_trans.push(trans);
	}
}

void
Session::commit_reversible_command(Command *cmd)
{
	assert(!_current_trans.empty());
	struct timeval now;

	if (cmd) {
		_current_trans.top()->add_command(cmd);
	}

	if (_current_trans.top()->empty()) {
		_current_trans.pop();
		return;
	}

	gettimeofday(&now, 0);
	_current_trans.top()->set_timestamp(now);

	_history.add(_current_trans.top());
	_current_trans.pop();
}

static bool
accept_all_non_peak_files (const string& path, void */*arg*/)
{
	return (path.length() > 5 && path.find (peakfile_suffix) != (path.length() - 5));
}

static bool
accept_all_state_files (const string& path, void */*arg*/)
{
	return (path.length() > 7 && path.find (".ardour") == (path.length() - 7));
}

int
Session::find_all_sources (string path, set<string>& result)
{
	XMLTree tree;
	XMLNode* node;

	if (!tree.read (path)) {
		return -1;
	}

	if ((node = find_named_node (*tree.root(), "Sources")) == 0) {
		return -2;
	}

	XMLNodeList nlist;
	XMLNodeConstIterator niter;

	nlist = node->children();

	set_dirty();

	for (niter = nlist.begin(); niter != nlist.end(); ++niter) {

		XMLProperty* prop;

		if ((prop = (*niter)->property (X_("type"))) == 0) {
			continue;
		}

		DataType type (prop->value());

		if ((prop = (*niter)->property (X_("name"))) == 0) {
			continue;
		}

		if (prop->value()[0] == '/') {
			/* external file, ignore */
			continue;
		}

		Glib::ustring found_path;
		bool is_new;
		uint16_t chan;

		if (FileSource::find (type, prop->value(), true, is_new, chan, found_path)) {
			result.insert (found_path);
		}
	}

	return 0;
}

int
Session::find_all_sources_across_snapshots (set<string>& result, bool exclude_this_snapshot)
{
	PathScanner scanner;
	vector<string*>* state_files;
	string ripped;
	string this_snapshot_path;

	result.clear ();

	ripped = _path;

	if (ripped[ripped.length()-1] == '/') {
		ripped = ripped.substr (0, ripped.length() - 1);
	}

	state_files = scanner (ripped, accept_all_state_files, (void *) 0, false, true);

	if (state_files == 0) {
		/* impossible! */
		return 0;
	}

	this_snapshot_path = _path;
	this_snapshot_path += legalize_for_path (_current_snapshot_name);
	this_snapshot_path += statefile_suffix;

	for (vector<string*>::iterator i = state_files->begin(); i != state_files->end(); ++i) {

		if (exclude_this_snapshot && **i == this_snapshot_path) {
			continue;
		}

		if (find_all_sources (**i, result) < 0) {
			return -1;
		}
	}

	return 0;
}

struct RegionCounter {
    typedef std::map<PBD::ID,boost::shared_ptr<AudioSource> > AudioSourceList;
    AudioSourceList::iterator iter;
    boost::shared_ptr<Region> region;
    uint32_t count;

    RegionCounter() : count (0) {}
};

int
Session::ask_about_playlist_deletion (boost::shared_ptr<Playlist> p)
{
        boost::optional<int> r = AskAboutPlaylistDeletion (p);
	return r.get_value_or (1);
}

int
Session::cleanup_sources (CleanupReport& rep)
{
	// FIXME: needs adaptation to midi

	vector<boost::shared_ptr<Source> > dead_sources;
	PathScanner scanner;
	string sound_path;
	vector<space_and_path>::iterator i;
	vector<space_and_path>::iterator nexti;
	vector<string*>* soundfiles;
	vector<string> unused;
	set<string> all_sources;
	bool used;
	string spath;
	int ret = -1;

	_state_of_the_state = (StateOfTheState) (_state_of_the_state | InCleanup);

	/* step 1: consider deleting all unused playlists */
	
	if (playlists->maybe_delete_unused (boost::bind (Session::ask_about_playlist_deletion, _1))) {
		ret = 0;
		goto out;
	}

	/* step 2: find all un-used sources */

	rep.paths.clear ();
	rep.space = 0;

	for (SourceMap::iterator i = sources.begin(); i != sources.end(); ) {

		SourceMap::iterator tmp;

		tmp = i;
		++tmp;

		/* do not bother with files that are zero size, otherwise we remove the current "nascent"
		   capture files.
		*/

		if (!i->second->used() && (i->second->length(i->second->timeline_position() > 0))) {
			dead_sources.push_back (i->second);
			i->second->drop_references ();
		}

		i = tmp;
	}

	/* build a list of all the possible sound directories for the session */

	for (i = session_dirs.begin(); i != session_dirs.end(); ) {

		nexti = i;
		++nexti;

		SessionDirectory sdir ((*i).path);
		sound_path += sdir.sound_path().to_string();

		if (nexti != session_dirs.end()) {
			sound_path += ':';
		}

		i = nexti;
	}

	/* now do the same thing for the files that ended up in the sounds dir(s)
	   but are not referenced as sources in any snapshot.
	*/

	soundfiles = scanner (sound_path, accept_all_non_peak_files, (void *) 0, false, true);

	if (soundfiles == 0) {
		return 0;
	}

	/* find all sources, but don't use this snapshot because the
	   state file on disk still references sources we may have already
	   dropped.
	*/

	find_all_sources_across_snapshots (all_sources, true);

	/*  add our current source list
	 */

	for (SourceMap::iterator i = sources.begin(); i != sources.end(); ++i) {
		boost::shared_ptr<FileSource> fs;

		if ((fs = boost::dynamic_pointer_cast<FileSource> (i->second)) != 0) {
			all_sources.insert (fs->path());
		}
	}

	char tmppath1[PATH_MAX+1];
	char tmppath2[PATH_MAX+1];

	for (vector<string*>::iterator x = soundfiles->begin(); x != soundfiles->end(); ++x) {

		used = false;
		spath = **x;

		for (set<string>::iterator i = all_sources.begin(); i != all_sources.end(); ++i) {

			if (realpath(spath.c_str(), tmppath1) == 0) {
                                error << string_compose (_("Cannot expand path %1 (%2)"),
                                                         spath, strerror (errno)) << endmsg;
                                continue;
                        }

                        if (realpath((*i).c_str(),  tmppath2) == 0) {
                                error << string_compose (_("Cannot expand path %1 (%2)"),
                                                         (*i), strerror (errno)) << endmsg;
                                continue;
                        }

			if (strcmp(tmppath1, tmppath2) == 0) {
				used = true;
				break;
			}
		}

		if (!used) {
			unused.push_back (spath);
		}
	}

	/* now try to move all unused files into the "dead_sounds" directory(ies) */

	for (vector<string>::iterator x = unused.begin(); x != unused.end(); ++x) {
		struct stat statbuf;

		rep.paths.push_back (*x);
		if (stat ((*x).c_str(), &statbuf) == 0) {
			rep.space += statbuf.st_size;
		}

		string newpath;

		/* don't move the file across filesystems, just
		   stick it in the `dead_sound_dir_name' directory
		   on whichever filesystem it was already on.
		*/

		if ((*x).find ("/sounds/") != string::npos) {

			/* old school, go up 1 level */

			newpath = Glib::path_get_dirname (*x);      // "sounds"
			newpath = Glib::path_get_dirname (newpath); // "session-name"

		} else {

			/* new school, go up 4 levels */

			newpath = Glib::path_get_dirname (*x);      // "audiofiles"
			newpath = Glib::path_get_dirname (newpath); // "session-name"
			newpath = Glib::path_get_dirname (newpath); // "interchange"
			newpath = Glib::path_get_dirname (newpath); // "session-dir"
		}

		newpath += '/';
		newpath += dead_sound_dir_name;

		if (g_mkdir_with_parents (newpath.c_str(), 0755) < 0) {
			error << string_compose(_("Session: cannot create session peakfile folder \"%1\" (%2)"), newpath, strerror (errno)) << endmsg;
			return -1;
		}

		newpath += '/';
		newpath += Glib::path_get_basename ((*x));

		if (access (newpath.c_str(), F_OK) == 0) {

			/* the new path already exists, try versioning */

			char buf[PATH_MAX+1];
			int version = 1;
			string newpath_v;

			snprintf (buf, sizeof (buf), "%s.%d", newpath.c_str(), version);
			newpath_v = buf;

			while (access (newpath_v.c_str(), F_OK) == 0 && version < 999) {
				snprintf (buf, sizeof (buf), "%s.%d", newpath.c_str(), ++version);
				newpath_v = buf;
			}

			if (version == 999) {
				error << string_compose (_("there are already 1000 files with names like %1; versioning discontinued"),
						  newpath)
				      << endmsg;
			} else {
				newpath = newpath_v;
			}

		} else {

			/* it doesn't exist, or we can't read it or something */

		}

		if (::rename ((*x).c_str(), newpath.c_str()) != 0) {
			error << string_compose (_("cannot rename audio file source from %1 to %2 (%3)"),
					  (*x), newpath, strerror (errno))
			      << endmsg;
			goto out;
		}

		/* see if there an easy to find peakfile for this file, and remove it.
		 */

		string peakpath = (*x).substr (0, (*x).find_last_of ('.'));
		peakpath += peakfile_suffix;

		if (access (peakpath.c_str(), W_OK) == 0) {
			if (::unlink (peakpath.c_str()) != 0) {
				error << string_compose (_("cannot remove peakfile %1 for %2 (%3)"),
						  peakpath, _path, strerror (errno))
				      << endmsg;
				/* try to back out */
				rename (newpath.c_str(), _path.c_str());
				goto out;
			}
		}
	}

	ret = 0;

	/* dump the history list */

	_history.clear ();

	/* save state so we don't end up a session file
	   referring to non-existent sources.
	*/

	save_state ("");

  out:
	_state_of_the_state = (StateOfTheState) (_state_of_the_state & ~InCleanup);

	return ret;
}

int
Session::cleanup_trash_sources (CleanupReport& rep)
{
	// FIXME: needs adaptation for MIDI

	vector<space_and_path>::iterator i;
	string dead_sound_dir;
	struct dirent* dentry;
	struct stat statbuf;
	DIR* dead;

	rep.paths.clear ();
	rep.space = 0;

	for (i = session_dirs.begin(); i != session_dirs.end(); ++i) {

		dead_sound_dir = (*i).path;
		dead_sound_dir += dead_sound_dir_name;

		if ((dead = opendir (dead_sound_dir.c_str())) == 0) {
			continue;
		}

		while ((dentry = readdir (dead)) != 0) {

			/* avoid '.' and '..' */

			if ((dentry->d_name[0] == '.' && dentry->d_name[1] == '\0') ||
			    (dentry->d_name[2] == '\0' && dentry->d_name[0] == '.' && dentry->d_name[1] == '.')) {
				continue;
			}

			string fullpath;

			fullpath = dead_sound_dir;
			fullpath += '/';
			fullpath += dentry->d_name;

			if (stat (fullpath.c_str(), &statbuf)) {
				continue;
			}

			if (!S_ISREG (statbuf.st_mode)) {
				continue;
			}

			if (unlink (fullpath.c_str())) {
				error << string_compose (_("cannot remove dead sound file %1 (%2)"),
						  fullpath, strerror (errno))
				      << endmsg;
			}

			rep.paths.push_back (dentry->d_name);
			rep.space += statbuf.st_size;
		}

		closedir (dead);

	}

	return 0;
}

void
Session::set_dirty ()
{
	bool was_dirty = dirty();

	_state_of_the_state = StateOfTheState (_state_of_the_state | Dirty);


	if (!was_dirty) {
		DirtyChanged(); /* EMIT SIGNAL */
	}
}


void
Session::set_clean ()
{
	bool was_dirty = dirty();

	_state_of_the_state = Clean;


	if (was_dirty) {
		DirtyChanged(); /* EMIT SIGNAL */
	}
}

void
Session::set_deletion_in_progress ()
{
	_state_of_the_state = StateOfTheState (_state_of_the_state | Deletion);
}

void
Session::clear_deletion_in_progress ()
{
	_state_of_the_state = StateOfTheState (_state_of_the_state & (~Deletion));
}

void
Session::add_controllable (boost::shared_ptr<Controllable> c)
{
	/* this adds a controllable to the list managed by the Session.
	   this is a subset of those managed by the Controllable class
	   itself, and represents the only ones whose state will be saved
	   as part of the session.
	*/

	Glib::Mutex::Lock lm (controllables_lock);
	controllables.insert (c);
}

struct null_deleter { void operator()(void const *) const {} };

void
Session::remove_controllable (Controllable* c)
{
	if (_state_of_the_state | Deletion) {
		return;
	}

	Glib::Mutex::Lock lm (controllables_lock);

	Controllables::iterator x = controllables.find (boost::shared_ptr<Controllable>(c, null_deleter()));

	if (x != controllables.end()) {
		controllables.erase (x);
	}
}

boost::shared_ptr<Controllable>
Session::controllable_by_id (const PBD::ID& id)
{
	Glib::Mutex::Lock lm (controllables_lock);

	for (Controllables::iterator i = controllables.begin(); i != controllables.end(); ++i) {
		if ((*i)->id() == id) {
			return *i;
		}
	}

	return boost::shared_ptr<Controllable>();
}

boost::shared_ptr<Controllable>
Session::controllable_by_descriptor (const ControllableDescriptor& desc)
{
	boost::shared_ptr<Controllable> c;
	boost::shared_ptr<Route> r;

	switch (desc.top_level_type()) {
	case ControllableDescriptor::NamedRoute:
	{
		std::string str = desc.top_level_name();
		if (str == "master") {
			r = _master_out;
		} else if (str == "control" || str == "listen") {
			r = _monitor_out;
		} else {
			r = route_by_name (desc.top_level_name());
		}
		break;
	}

	case ControllableDescriptor::RemoteControlID:
		r = route_by_remote_id (desc.rid());
		break;
	}
	
	if (!r) {
		return c;
	}

	switch (desc.subtype()) {
	case ControllableDescriptor::Gain:
		c = r->gain_control ();
		break;

	case ControllableDescriptor::Solo:
		c = r->solo_control();
		break;

	case ControllableDescriptor::Mute:
		c = r->mute_control();
		break;

	case ControllableDescriptor::Recenable:
	{
		boost::shared_ptr<Track> t = boost::dynamic_pointer_cast<Track>(r);
		
		if (t) {
			c = t->rec_enable_control ();
		}
		break;
	}

	case ControllableDescriptor::Pan:
		/* XXX pan control */
		break;

	case ControllableDescriptor::Balance:
		/* XXX simple pan control */
		break;

	case ControllableDescriptor::PluginParameter:
	{
		uint32_t plugin = desc.target (0);
		uint32_t parameter_index = desc.target (1);

		/* revert to zero based counting */
		
		if (plugin > 0) {
			--plugin;
		}
		
		if (parameter_index > 0) {
			--parameter_index;
		}

		boost::shared_ptr<Processor> p = r->nth_plugin (plugin);
		
		if (p) {
			c = boost::dynamic_pointer_cast<ARDOUR::AutomationControl>(
				p->control(Evoral::Parameter(PluginAutomation, 0, parameter_index)));
		}
		break;
	}

	case ControllableDescriptor::SendGain: 
	{
		uint32_t send = desc.target (0);

		/* revert to zero-based counting */
		
		if (send > 0) {
			--send;
		}
		
		boost::shared_ptr<Processor> p = r->nth_send (send);
		
		if (p) {
			boost::shared_ptr<Send> s = boost::dynamic_pointer_cast<Send>(p);
			boost::shared_ptr<Amp> a = s->amp();

			if (a) {
				c = s->amp()->gain_control();
			}
		}
		break;
	}

	default:
		/* relax and return a null pointer */
		break;
	}

	return c;
}

void
Session::add_instant_xml (XMLNode& node, bool write_to_config)
{
	if (_writable) {
		Stateful::add_instant_xml (node, _path);
	}

	if (write_to_config) {
		Config->add_instant_xml (node);
	}
}

XMLNode*
Session::instant_xml (const string& node_name)
{
	return Stateful::instant_xml (node_name, _path);
}

int
Session::save_history (string snapshot_name)
{
	XMLTree tree;

	if (!_writable) {
	        return 0;
	}

	if (snapshot_name.empty()) {
		snapshot_name = _current_snapshot_name;
	}

	const string history_filename = legalize_for_path (snapshot_name) + history_suffix;
	const string backup_filename = history_filename + backup_suffix;
	const sys::path xml_path = _session_dir->root_path() / history_filename;
	const sys::path backup_path = _session_dir->root_path() / backup_filename;

	if (sys::exists (xml_path)) {
		try
		{
			sys::rename (xml_path, backup_path);
		}
		catch (const sys::filesystem_error& err)
		{
			error << _("could not backup old history file, current history not saved") << endmsg;
			return -1;
		}
	}

	if (!Config->get_save_history() || Config->get_saved_history_depth() < 0) {
		return 0;
	}

	tree.set_root (&_history.get_state (Config->get_saved_history_depth()));

	if (!tree.write (xml_path.to_string()))
	{
		error << string_compose (_("history could not be saved to %1"), xml_path.to_string()) << endmsg;

		try
		{
			sys::remove (xml_path);
			sys::rename (backup_path, xml_path);
		}
		catch (const sys::filesystem_error& err)
		{
			error << string_compose (_("could not restore history file from backup %1 (%2)"),
					backup_path.to_string(), err.what()) << endmsg;
		}

		return -1;
	}

	return 0;
}

int
Session::restore_history (string snapshot_name)
{
	XMLTree tree;

	if (snapshot_name.empty()) {
		snapshot_name = _current_snapshot_name;
	}

	const string xml_filename = legalize_for_path (snapshot_name) + history_suffix;
	const sys::path xml_path = _session_dir->root_path() / xml_filename;

	info << "Loading history from " << xml_path.to_string() << endmsg;

	if (!sys::exists (xml_path)) {
		info << string_compose (_("%1: no history file \"%2\" for this session."),
				_name, xml_path.to_string()) << endmsg;
		return 1;
	}

	if (!tree.read (xml_path.to_string())) {
		error << string_compose (_("Could not understand session history file \"%1\""),
				xml_path.to_string()) << endmsg;
		return -1;
	}

	// replace history
	_history.clear();

	for (XMLNodeConstIterator it  = tree.root()->children().begin(); it != tree.root()->children().end(); it++) {

		XMLNode *t = *it;
		UndoTransaction* ut = new UndoTransaction ();
		struct timeval tv;

		ut->set_name(t->property("name")->value());
		stringstream ss(t->property("tv-sec")->value());
		ss >> tv.tv_sec;
		ss.str(t->property("tv-usec")->value());
		ss >> tv.tv_usec;
		ut->set_timestamp(tv);

		for (XMLNodeConstIterator child_it  = t->children().begin();
				child_it != t->children().end(); child_it++)
		{
			XMLNode *n = *child_it;
			Command *c;

			if (n->name() == "MementoCommand" ||
					n->name() == "MementoUndoCommand" ||
					n->name() == "MementoRedoCommand") {

				if ((c = memento_command_factory(n))) {
					ut->add_command(c);
				}

			} else if (n->name() == "DiffCommand") {
				PBD::ID  id(n->property("midi-source")->value());
				boost::shared_ptr<MidiSource> midi_source =
					boost::dynamic_pointer_cast<MidiSource, Source>(source_by_id(id));
				if (midi_source) {
					ut->add_command(new MidiModel::DiffCommand(midi_source->model(), *n));
				} else {
					error << _("Failed to downcast MidiSource for DiffCommand") << endmsg;
				}

			} else if (n->name() == "StatefulDiffCommand") {
				if ((c = stateful_diff_command_factory (n))) {
					ut->add_command (c);
				}
			} else {
				error << string_compose(_("Couldn't figure out how to make a Command out of a %1 XMLNode."), n->name()) << endmsg;
			}
		}

		_history.add (ut);
	}

	return 0;
}

void
Session::config_changed (std::string p, bool ours)
{
	if (ours) {
		set_dirty ();
	}

	if (p == "seamless-loop") {

	} else if (p == "rf-speed") {

	} else if (p == "auto-loop") {

	} else if (p == "auto-input") {

		if (Config->get_monitoring_model() == HardwareMonitoring && transport_rolling()) {
			/* auto-input only makes a difference if we're rolling */

			boost::shared_ptr<RouteList> rl = routes.reader ();
			for (RouteList::iterator i = rl->begin(); i != rl->end(); ++i) {
				boost::shared_ptr<Track> tr = boost::dynamic_pointer_cast<Track> (*i);
				if (tr && tr->record_enabled ()) {
					tr->monitor_input (!config.get_auto_input());
				}
			}
		}

	} else if (p == "punch-in") {

		Location* location;

		if ((location = _locations.auto_punch_location()) != 0) {

			if (config.get_punch_in ()) {
				replace_event (SessionEvent::PunchIn, location->start());
			} else {
				remove_event (location->start(), SessionEvent::PunchIn);
			}
		}

	} else if (p == "punch-out") {

		Location* location;

		if ((location = _locations.auto_punch_location()) != 0) {

			if (config.get_punch_out()) {
				replace_event (SessionEvent::PunchOut, location->end());
			} else {
				clear_events (SessionEvent::PunchOut);
			}
		}

	} else if (p == "edit-mode") {

		Glib::Mutex::Lock lm (playlists->lock);

		for (SessionPlaylists::List::iterator i = playlists->playlists.begin(); i != playlists->playlists.end(); ++i) {
			(*i)->set_edit_mode (Config->get_edit_mode ());
		}

	} else if (p == "use-video-sync") {

		waiting_for_sync_offset = config.get_use_video_sync();

	} else if (p == "mmc-control") {

		//poke_midi_thread ();

	} else if (p == "mmc-device-id" || p == "mmc-receive-id") {

		_mmc->set_receive_device_id (Config->get_mmc_receive_device_id());

	} else if (p == "mmc-send-id") {

		_mmc->set_send_device_id (Config->get_mmc_send_device_id());

	} else if (p == "midi-control") {

		//poke_midi_thread ();

	} else if (p == "raid-path") {

		setup_raid_path (config.get_raid_path());

	} else if (p == "timecode-format") {

		sync_time_vars ();

	} else if (p == "video-pullup") {

		sync_time_vars ();

	} else if (p == "seamless-loop") {

		if (play_loop && transport_rolling()) {
			// to reset diskstreams etc
			request_play_loop (true);
		}

	} else if (p == "rf-speed") {

		cumulative_rf_motion = 0;
		reset_rf_scale (0);

	} else if (p == "click-sound") {

		setup_click_sounds (1);

	} else if (p == "click-emphasis-sound") {

		setup_click_sounds (-1);

	} else if (p == "clicking") {

		if (Config->get_clicking()) {
			if (_click_io && click_data) { // don't require emphasis data
				_clicking = true;
			}
		} else {
			_clicking = false;
		}

	} else if (p == "send-mtc") {

		/* only set the internal flag if we have
		   a port.
		*/

		if (_mtc_port != 0) {
			session_send_mtc = Config->get_send_mtc();
			if (session_send_mtc) {
				/* mark us ready to send */
				next_quarter_frame_to_send = 0;
			}
		} else {
			session_send_mtc = false;
		}

	} else if (p == "send-mmc") {

		_mmc->enable_send (Config->get_send_mmc ());

	} else if (p == "midi-feedback") {

		/* only set the internal flag if we have
		   a port.
		*/

		if (_mtc_port != 0) {
			session_midi_feedback = Config->get_midi_feedback();
		}

	} else if (p == "jack-time-master") {

		engine().reset_timebase ();

	} else if (p == "native-file-header-format") {

		if (!first_file_header_format_reset) {
			reset_native_file_format ();
		}

		first_file_header_format_reset = false;

	} else if (p == "native-file-data-format") {

		if (!first_file_data_format_reset) {
			reset_native_file_format ();
		}

		first_file_data_format_reset = false;

	} else if (p == "external-sync") {
		if (!config.get_external_sync()) {
			drop_sync_source ();
		} else {
			switch_to_sync_source (config.get_sync_source());
		}
	} else if (p == "remote-model") {
		set_remote_control_ids ();
	}  else if (p == "denormal-model") {
		setup_fpu ();
	} else if (p == "history-depth") {
		set_history_depth (Config->get_history_depth());
	} else if (p == "sync-all-route-ordering") {
		sync_order_keys ("session");
	} else if (p == "initial-program-change") {

		if (_mmc->port() && Config->get_initial_program_change() >= 0) {
			MIDI::byte buf[2];

			buf[0] = MIDI::program; // channel zero by default
			buf[1] = (Config->get_initial_program_change() & 0x7f);

			_mmc->port()->midimsg (buf, sizeof (buf), 0);
		}
	} else if (p == "initial-program-change") {
                
		if (_mmc->port() && Config->get_initial_program_change() >= 0) {
			MIDI::byte* buf = new MIDI::byte[2];

			buf[0] = MIDI::program; // channel zero by default
			buf[1] = (Config->get_initial_program_change() & 0x7f);
			// deliver_midi (_mmc_port, buf, 2);
		}
	} else if (p == "solo-mute-override") {
		// catch_up_on_solo_mute_override ();
	} else if (p == "listen-position") {
		listen_position_changed ();
	} else if (p == "solo-control-is-listen-control") {
		solo_control_mode_changed ();
	}


	set_dirty ();
}

void
Session::set_history_depth (uint32_t d)
{
	_history.set_depth (d);
}

int
Session::load_diskstreams_2X (XMLNode const & node, int)
{
        XMLNodeList          clist;
        XMLNodeConstIterator citer;

        clist = node.children();

        for (citer = clist.begin(); citer != clist.end(); ++citer) {

                try {
                        /* diskstreams added automatically by DiskstreamCreated handler */
                        if ((*citer)->name() == "AudioDiskstream" || (*citer)->name() == "DiskStream") {
				boost::shared_ptr<AudioDiskstream> dsp (new AudioDiskstream (*this, **citer));
				_diskstreams_2X.push_back (dsp);
                        } else {
                                error << _("Session: unknown diskstream type in XML") << endmsg;
                        }
                }

                catch (failed_constructor& err) {
                        error << _("Session: could not load diskstream via XML state") << endmsg;
                        return -1;
                }
        }

        return 0;
}

/** Create our MachineControl object and connect things to it */
void
Session::setup_midi_machine_control ()
{
	_mmc = new MIDI::MachineControl;
	
	_mmc->Play.connect_same_thread (*this, boost::bind (&Session::mmc_deferred_play, this, _1));
	_mmc->DeferredPlay.connect_same_thread (*this, boost::bind (&Session::mmc_deferred_play, this, _1));
	_mmc->Stop.connect_same_thread (*this, boost::bind (&Session::mmc_stop, this, _1));
	_mmc->FastForward.connect_same_thread (*this, boost::bind (&Session::mmc_fast_forward, this, _1));
	_mmc->Rewind.connect_same_thread (*this, boost::bind (&Session::mmc_rewind, this, _1));
	_mmc->Pause.connect_same_thread (*this, boost::bind (&Session::mmc_pause, this, _1));
	_mmc->RecordPause.connect_same_thread (*this, boost::bind (&Session::mmc_record_pause, this, _1));
	_mmc->RecordStrobe.connect_same_thread (*this, boost::bind (&Session::mmc_record_strobe, this, _1));
	_mmc->RecordExit.connect_same_thread (*this, boost::bind (&Session::mmc_record_exit, this, _1));
	_mmc->Locate.connect_same_thread (*this, boost::bind (&Session::mmc_locate, this, _1, _2));
	_mmc->Step.connect_same_thread (*this, boost::bind (&Session::mmc_step, this, _1, _2));
	_mmc->Shuttle.connect_same_thread (*this, boost::bind (&Session::mmc_shuttle, this, _1, _2, _3));
	_mmc->TrackRecordStatusChange.connect_same_thread (*this, boost::bind (&Session::mmc_record_enable, this, _1, _2, _3));

	/* also handle MIDI SPP because its so common */

	_mmc->SPPStart.connect_same_thread (*this, boost::bind (&Session::spp_start, this, _1, _2));
	_mmc->SPPContinue.connect_same_thread (*this, boost::bind (&Session::spp_continue, this, _1, _2));
	_mmc->SPPStop.connect_same_thread (*this, boost::bind (&Session::spp_stop, this, _1, _2));
}

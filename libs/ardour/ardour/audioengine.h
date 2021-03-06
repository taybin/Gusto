/*
    Copyright (C) 2002-2004 Paul Davis

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

#ifndef __ardour_audioengine_h__
#define __ardour_audioengine_h__

#ifdef WAF_BUILD
#include "libardour-config.h"
#endif

#include <iostream>
#include <list>
#include <set>
#include <cmath>
#include <exception>
#include <string>


#include <glibmm/thread.h>

#include "pbd/rcu.h"
#include "pbd/signals.h"

#include "ardour/ardour.h"
#include <jack/jack.h>
#include <jack/transport.h>

#include "ardour/data_type.h"
#include "ardour/session_handle.h"
#include "ardour/types.h"

#ifdef HAVE_JACK_SESSION
#include <jack/session.h>
#endif

namespace ARDOUR {

class InternalPort;
class MidiPort;
class Port;
class Session;
class ProcessThread;

class AudioEngine : public SessionHandlePtr
{
   public:
	typedef std::set<Port*> Ports;

	AudioEngine (std::string client_name, std::string session_uuid);
	virtual ~AudioEngine ();

	jack_client_t* jack() const;
	bool connected() const { return _jack != 0; }

	bool is_realtime () const;

        ProcessThread* main_thread() const { return _main_thread; }

	std::string client_name() const { return jack_client_name; }

	int reconnect_to_jack ();
	int disconnect_from_jack();

	bool will_reconnect_at_halt ();
	void set_reconnect_at_halt (bool);

	int stop (bool forever = false);
	int start ();
	bool running() const { return _running; }

	Glib::Mutex& process_lock() { return _process_lock; }

	nframes_t frame_rate() const;
	nframes_t frames_per_cycle() const;

	size_t raw_buffer_size(DataType t);

	int usecs_per_cycle () const { return _usecs_per_cycle; }

	bool get_sync_offset (nframes_t& offset) const;

	nframes_t frames_since_cycle_start () {
  	        jack_client_t* _priv_jack = _jack;
		if (!_running || !_priv_jack) return 0;
		return jack_frames_since_cycle_start (_priv_jack);
	}
	nframes_t frame_time () {
  	        jack_client_t* _priv_jack = _jack;
		if (!_running || !_priv_jack) return 0;
		return jack_frame_time (_priv_jack);
	}

	nframes_t frame_time_at_cycle_start () {
  	        jack_client_t* _priv_jack = _jack;
		if (!_running || !_priv_jack) return 0;
		return jack_last_frame_time (_priv_jack);
	}

	nframes_t transport_frame () const {
  	        const jack_client_t* _priv_jack = _jack;
		if (!_running || !_priv_jack) return 0;
		return jack_get_current_transport_frame (_priv_jack);
	}

	int request_buffer_size (nframes_t);

	nframes_t set_monitor_check_interval (nframes_t);
	nframes_t processed_frames() const { return _processed_frames; }

	float get_cpu_load() {
  	        jack_client_t* _priv_jack = _jack;
		if (!_running || !_priv_jack) return 0;
		return jack_cpu_load (_priv_jack);
	}

	void set_session (Session *);
	void remove_session (); // not a replacement for SessionHandle::session_going_away()

	class PortRegistrationFailure : public std::exception {
	public:
		PortRegistrationFailure (const char* why = "") {
			reason = why;
		}
		virtual const char *what() const throw() { return reason; }

	private:
		const char* reason;
	};

	class NoBackendAvailable : public std::exception {
	public:
		virtual const char *what() const throw() { return "could not connect to engine backend"; }
	};

	Port *register_input_port (DataType, const std::string& portname);
	Port *register_output_port (DataType, const std::string& portname);
	int   unregister_port (Port &);

	void split_cycle (nframes_t offset);

	int connect (const std::string& source, const std::string& destination);
	int disconnect (const std::string& source, const std::string& destination);
	int disconnect (Port &);

	const char ** get_ports (const std::string& port_name_pattern, const std::string& type_name_pattern, uint32_t flags);

	bool can_request_hardware_monitoring ();

	uint32_t n_physical_outputs (DataType type) const;
	uint32_t n_physical_inputs (DataType type) const;

	void get_physical_outputs (DataType type, std::vector<std::string>&);
	void get_physical_inputs (DataType type, std::vector<std::string>&);

	std::string get_nth_physical_output (DataType type, uint32_t n) {
		return get_nth_physical (type, n, JackPortIsInput);
	}

	std::string get_nth_physical_input (DataType type, uint32_t n) {
		return get_nth_physical (type, n, JackPortIsOutput);
	}

	void update_total_latencies ();
	void update_total_latency (const Port&);

	Port *get_port_by_name (const std::string &);
	Port *get_port_by_name_locked (const std::string &);

	enum TransportState {
		TransportStopped = JackTransportStopped,
		TransportRolling = JackTransportRolling,
		TransportLooping = JackTransportLooping,
		TransportStarting = JackTransportStarting
	};

	void transport_start ();
	void transport_stop ();
	void transport_locate (nframes_t);
	TransportState transport_state ();

	int  reset_timebase ();

	/* start/stop freewheeling */

	int freewheel (bool onoff);
	bool freewheeling() const { return _freewheeling; }

	/* this signal is sent for every process() cycle while freewheeling.
_	   the regular process() call to session->process() is not made.
	*/

	PBD::Signal1<int,nframes_t> Freewheel;

	PBD::Signal0<void> Xrun;

	/* this signal is if JACK notifies us of a graph order event */

	PBD::Signal0<void> GraphReordered;

#ifdef HAVE_JACK_SESSION
	PBD::Signal1<void,jack_session_event_t *> JackSessionEvent;
#endif


	/* this signal is emitted if the sample rate changes */

	PBD::Signal1<void,nframes_t> SampleRateChanged;

	/* this signal is sent if JACK ever disconnects us */

	PBD::Signal1<void,const char*> Halted;

	/* these two are emitted when the engine itself is
	   started and stopped
	*/

	PBD::Signal0<void> Running;
	PBD::Signal0<void> Stopped;

	/** Emitted if a JACK port is registered or unregistered */
	PBD::Signal0<void> PortRegisteredOrUnregistered;

	/** Emitted if a JACK port is connected or disconnected */
	PBD::Signal0<void> PortConnectedOrDisconnected;

	std::string make_port_name_relative (std::string);
	std::string make_port_name_non_relative (std::string);

	static AudioEngine* instance() { return _instance; }
	void died ();

        pthread_t create_process_thread (boost::function<void()>, size_t stacksize);

  private:
	static AudioEngine*       _instance;

	jack_client_t* volatile   _jack; /* could be reset to null by SIGPIPE or another thread */
	std::string                jack_client_name;
	Glib::Mutex               _process_lock;
	Glib::Cond                 session_removed;
	bool                       session_remove_pending;
	bool                      _running;
	bool                      _has_run;
	mutable nframes_t         _buffer_size;
	std::map<DataType,size_t> _raw_buffer_sizes;
	mutable nframes_t         _frame_rate;
	/// number of frames between each check for changes in monitor input
	nframes_t                  monitor_check_interval;
	/// time of the last monitor check in frames
	nframes_t                  last_monitor_check;
	/// the number of frames processed since start() was called
	nframes_t                 _processed_frames;
	bool                      _freewheeling;
	bool                      _freewheel_pending;
	boost::function<int(nframes_t)>  freewheel_action;
	bool                       reconnect_on_halt;
	int                       _usecs_per_cycle;

	SerializedRCUManager<Ports> ports;

	Port *register_port (DataType type, const std::string& portname, bool input);

	int    process_callback (nframes_t nframes);
	void*  process_thread ();
        void   finish_process_cycle (int status);
	void   remove_all_ports ();

	std::string get_nth_physical (DataType type, uint32_t n, int flags);

	void port_registration_failure (const std::string& portname);

	static int  _xrun_callback (void *arg);
#ifdef HAVE_JACK_SESSION
	static void _session_callback (jack_session_event_t *event, void *arg);
#endif
	static int  _graph_order_callback (void *arg);
	static int  _process_callback (nframes_t nframes, void *arg);
	static void* _process_thread (void *arg);
	static int  _sample_rate_callback (nframes_t nframes, void *arg);
	static int  _bufsize_callback (nframes_t nframes, void *arg);
	static void _jack_timebase_callback (jack_transport_state_t, nframes_t, jack_position_t*, int, void*);
	static int  _jack_sync_callback (jack_transport_state_t, jack_position_t*, void *arg);
	static void _freewheel_callback (int , void *arg);
	static void _registration_callback (jack_port_id_t, int, void *);
	static void _connect_callback (jack_port_id_t, jack_port_id_t, int, void *);

	void jack_timebase_callback (jack_transport_state_t, nframes_t, jack_position_t*, int);
	int  jack_sync_callback (jack_transport_state_t, jack_position_t*);
	int  jack_bufsize_callback (nframes_t);
	int  jack_sample_rate_callback (nframes_t);

	int connect_to_jack (std::string client_name, std::string session_uuid);

	static void halted (void *);
	static void halted_info (jack_status_t,const char*,void *);

	void meter_thread ();
	void start_metering_thread ();
	void stop_metering_thread ();

	Glib::Thread*    m_meter_thread;
	static gint      m_meter_exit;

        ProcessThread* _main_thread;

        struct ThreadData {
            AudioEngine* engine;
            boost::function<void()> f;
            size_t stacksize;
            
            ThreadData (AudioEngine* ae, boost::function<void()> fp, size_t stacksz) 
            : engine (ae) , f (fp) , stacksize (stacksz) {}
        };
        
        static void* _start_process_thread (void*);
};

} // namespace ARDOUR

#endif /* __ardour_audioengine_h__ */

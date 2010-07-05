/*
    Copyright (C) 2006 Paul Davis 
	Written by Dave Robillard
 
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
 
    $Id: jack.h 4 2005-05-13 20:47:18Z taybin $
*/

#ifndef __jack_midiport_h__
#define __jack_midiport_h__

#include <vector>
#include <string>

#include <fcntl.h>
#include <unistd.h>

#include <glibmm/thread.h>

#include <jack/weakjack.h>
#include <jack/jack.h>
#include <jack/midiport.h>

#include "pbd/ringbuffer.h"
#include "pbd/signals.h"
#include "pbd/crossthread.h"
#include "evoral/EventRingBuffer.hpp"

#include "midi++/port.h"
#include "midi++/event.h"

namespace MIDI
{

class JACK_MidiPort : public Port
{
public:
	JACK_MidiPort (const XMLNode& node, jack_client_t* jack_client);
	virtual ~JACK_MidiPort ();

	int write(byte *msg, size_t msglen, timestamp_t timestamp);
	int read(byte *buf, size_t max);

	int selectable() const { return xthread.selectable(); }
	bool must_drain_selectable() const { return true; }
	
	void cycle_start(nframes_t nframes);
	void cycle_end();

	static std::string typestring;

	XMLNode& get_state () const;
	void set_state (const XMLNode&);

	static void set_process_thread (pthread_t);
	static pthread_t get_process_thread () { return _process_thread; }
	static bool is_process_thread();
	
	nframes_t nframes_this_cycle() const {	return _nframes_this_cycle; }

	void reestablish (void *);
	void reconnect ();

	static PBD::Signal0<void> MakeConnections;
	static PBD::Signal0<void> JackHalted;

  protected:
	std::string get_typestring () const {
		return typestring;
	}

private:
	int create_ports(const XMLNode&);
	int create_ports ();

	jack_client_t* _jack_client;
	std::string    _jack_input_port_name; /// input port name, or empty if there isn't one
	jack_port_t*   _jack_input_port;
	std::string    _jack_output_port_name; /// output port name, or empty if there isn't one
	jack_port_t*   _jack_output_port;
	nframes_t      _last_read_index;
	timestamp_t    _last_write_timestamp;
	CrossThreadChannel xthread;
	std::string    _inbound_connections;
	std::string    _outbound_connections;
	PBD::ScopedConnection connect_connection;
	PBD::ScopedConnection halt_connection;
	void flush (void* jack_port_buffer);
	void jack_halted ();
	void make_connections();

	static pthread_t _process_thread;

	RingBuffer< Evoral::Event<double> > output_fifo;
	Evoral::EventRingBuffer<timestamp_t> input_fifo;

	Glib::Mutex output_fifo_lock;
};


} /* namespace MIDI */

#endif // __jack_midiport_h__

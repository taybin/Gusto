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

#ifndef __ardour_location_h__
#define __ardour_location_h__

#include <string>
#include <list>
#include <iostream>
#include <map>

#include <sys/types.h>

#include <glibmm/thread.h>

#include "pbd/undo.h"
#include "pbd/stateful.h"
#include "pbd/statefuldestructible.h"

#include "ardour/ardour.h"

namespace ARDOUR {

class Location : public PBD::StatefulDestructible
{
  public:
	enum Flags {
		IsMark = 0x1,
		IsAutoPunch = 0x2,
		IsAutoLoop = 0x4,
		IsHidden = 0x8,
		IsCDMarker = 0x10,
		IsRangeMarker = 0x20,
		IsSessionRange = 0x40
	};

	Location (nframes64_t sample_start,
			nframes64_t sample_end,
			const std::string &name,
			Flags bits = Flags(0))

		: _name (name),
		_start (sample_start),
		_end (sample_end),
		_flags (bits),
		_locked (false) { }

	Location () {
		_start = 0;
		_end = 0;
		_flags = Flags (0);
		_locked = false;
	}

	Location (const Location& other);
	Location (const XMLNode&);
	Location* operator= (const Location& other);

	bool locked() const { return _locked; }
	void lock() { _locked = true; changed (this); }
	void unlock() { _locked = false; changed (this); }

	nframes64_t start() const  { return _start; }
	nframes64_t end() const { return _end; }
	nframes64_t length() const { return _end - _start; }

	int set_start (nframes64_t s);
	int set_end (nframes64_t e);
	int set (nframes64_t start, nframes64_t end);

	int move_to (nframes64_t pos);

	const std::string& name() const { return _name; }
	void set_name (const std::string &str) { _name = str; name_changed(this); }

	void set_auto_punch (bool yn, void *src);
	void set_auto_loop (bool yn, void *src);
	void set_hidden (bool yn, void *src);
	void set_cd (bool yn, void *src);
	void set_is_range_marker (bool yn, void* src);

	bool is_auto_punch () const { return _flags & IsAutoPunch; }
	bool is_auto_loop () const { return _flags & IsAutoLoop; }
	bool is_mark () const { return _flags & IsMark; }
	bool is_hidden () const { return _flags & IsHidden; }
	bool is_cd_marker () const { return _flags & IsCDMarker; }
	bool is_session_range () const { return _flags & IsSessionRange; }
	bool is_range_marker() const { return _flags & IsRangeMarker; }
	bool matches (Flags f) const { return _flags & f; }

	PBD::Signal1<void,Location*> name_changed;
	PBD::Signal1<void,Location*> end_changed;
	PBD::Signal1<void,Location*> start_changed;

	PBD::Signal2<void,Location*,void*> FlagsChanged;

	/* this is sent only when both start&end change at the same time */

	PBD::Signal1<void,Location*> changed;

	/* CD Track / CD-Text info */

	std::map<std::string, std::string> cd_info;
	XMLNode& cd_info_node (const std::string &, const std::string &);

	XMLNode& get_state (void);
	int set_state (const XMLNode&, int version);

  private:
	std::string   _name;
	nframes64_t   _start;
	nframes64_t   _end;
	Flags         _flags;
	bool          _locked;

	void set_mark (bool yn);
	bool set_flag_internal (bool yn, Flags flag);
};

class Locations : public PBD::StatefulDestructible
{
  public:
	typedef std::list<Location *> LocationList;

	Locations ();
	~Locations ();

	const LocationList& list() { return locations; }

	void add (Location *, bool make_current = false);
	void remove (Location *);
	void clear ();
	void clear_markers ();
	void clear_ranges ();

	XMLNode& get_state (void);
	int set_state (const XMLNode&, int version);
        Location *get_location_by_id(PBD::ID);

	Location* auto_loop_location () const;
	Location* auto_punch_location () const;
	Location* session_range_location() const;

	int next_available_name(std::string& result,std::string base);
	uint32_t num_range_markers() const;

	int set_current (Location *, bool want_lock = true);
	Location *current () const { return current_location; }

	Location* first_location_before (nframes64_t, bool include_special_ranges = false);
	Location* first_location_after (nframes64_t, bool include_special_ranges = false);

	void marks_either_side (nframes64_t const, nframes64_t &, nframes64_t &) const;

	void find_all_between (nframes64_t start, nframes64_t, LocationList&, Location::Flags);

	enum Change {
		ADDITION, ///< a location was added, but nothing else changed
		REMOVAL, ///< a location was removed, but nothing else changed
		OTHER ///< something more complicated happened
	};

	PBD::Signal1<void,Location*> current_changed;
	/** something changed about the location list; the parameter gives some idea as to what */
	PBD::Signal1<void,Change>    changed;
	/** a location has been added to the end of the list */
	PBD::Signal1<void,Location*> added;
	PBD::Signal1<void,Location*> removed;
	PBD::Signal1<void,const PBD::PropertyChange&>    StateChanged;

	template<class T> void apply (T& obj, void (T::*method)(LocationList&)) {
		Glib::Mutex::Lock lm (lock);
		(obj.*method)(locations);
	}

	template<class T1, class T2> void apply (T1& obj, void (T1::*method)(LocationList&, T2& arg), T2& arg) {
		Glib::Mutex::Lock lm (lock);
		(obj.*method)(locations, arg);
	}

  private:

	LocationList         locations;
	Location            *current_location;
	mutable Glib::Mutex  lock;

	int set_current_unlocked (Location *);
	void location_changed (Location*);
};

} // namespace ARDOUR

#endif /* __ardour_location_h__ */

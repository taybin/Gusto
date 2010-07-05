/*
    Copyright (C) 2006 Paul Davis

    This program is free software; you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by the Free
    Software Foundation; either version 2 of the License, or (at your option)
    any later version.

    This program is distributed in the hope that it will be useful, but WITHOUT
    ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
    FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
    for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    675 Mass Ave, Cambridge, MA 02139, USA.
*/

#ifndef __ardour_port_set_h__
#define __ardour_port_set_h__

#include <vector>
#include "ardour/chan_count.h"
#include <boost/utility.hpp>

namespace ARDOUR {

class Port;
class AudioPort;
class MidiPort;

/** An ordered list of Ports, possibly of various types.
 *
 * This allows access to all the ports as a list, ignoring type, or accessing
 * the nth port of a given type.  Note that port(n) and nth_audio_port(n) may
 * NOT return the same port.
 */
class PortSet : public boost::noncopyable {
public:
	PortSet();

	size_t num_ports() const;
	size_t num_ports(DataType type) const { return _ports[type].size(); }

	void add(Port* port);
	bool remove(Port* port);

	/** nth port */
	Port* port(size_t index) const;

	/** nth port of type @a t, or nth port if t = NIL */
	Port* port(DataType t, size_t index) const;

	AudioPort* nth_audio_port(size_t n) const;

	MidiPort* nth_midi_port(size_t n) const;

	bool contains(const Port* port) const;

	/** Remove all ports from the PortSet.  Ports are not deregistered with
	 * the engine, it's the caller's responsibility to not leak here!
	 */
	void clear() { _ports.clear(); }

	const ChanCount& count() const { return _count; }

	bool empty() const { return (_count.n_total() == 0); }

	template<typename PS, typename P>
	class iterator_base {
	public:
		P& operator*()  { return *_set.port(_type, _index); }
		P* operator->() { return _set.port(_type, _index); }
		iterator_base<PS,P>& operator++() { ++_index; return *this; } // yes, prefix only
		bool operator==(const iterator_base<PS,P>& other) { return (_index == other._index); }
		bool operator!=(const iterator_base<PS,P>& other) { return (_index != other._index); }

	private:
		friend class PortSet;

		iterator_base<PS,P>(PS& list, DataType type, size_t index)
			: _set(list), _type(type), _index(index) {}

		PS&      _set;
		DataType _type; ///< Ignored if NIL (to iterator over entire set)
		size_t   _index;
	};

	typedef iterator_base<PortSet, Port>             iterator;
	typedef iterator_base<const PortSet, const Port> const_iterator;

	iterator begin(DataType type = DataType::NIL) {
		return iterator(*this, type, 0);
	}

	iterator end(DataType type = DataType::NIL) {
		return iterator(*this, type,
			(type == DataType::NIL) ? _count.n_total() : _count.get(type));
	}

	const_iterator begin(DataType type = DataType::NIL) const {
		return const_iterator(*this, type, 0);
	}

	const_iterator end(DataType type = DataType::NIL) const {
		return const_iterator(*this, type,
			(type == DataType::NIL) ? _count.n_total() : _count.get(type));
	}

	class audio_iterator {
	public:
		AudioPort& operator*()  { return *_set.nth_audio_port(_index); }
		AudioPort* operator->() { return _set.nth_audio_port(_index); }
		audio_iterator& operator++() { ++_index; return *this; } // yes, prefix only
		bool operator==(const audio_iterator& other) { return (_index == other._index); }
		bool operator!=(const audio_iterator& other) { return (_index != other._index); }

	private:
		friend class PortSet;

		audio_iterator(PortSet& list, size_t index) : _set(list), _index(index) {}

		PortSet& _set;
		size_t   _index;
	};

	audio_iterator audio_begin() { return audio_iterator(*this, 0); }
	audio_iterator audio_end()   { return audio_iterator(*this, _count.n_audio()); }

private:
	typedef std::vector<Port*> PortVec;

	// Vector of vectors, indexed by DataType::to_index()
	std::vector<PortVec> _ports;

	ChanCount _count;
};


} // namespace ARDOUR

#endif // __ardour_port_set_h__
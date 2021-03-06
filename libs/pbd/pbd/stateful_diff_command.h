/*
    Copyright (C) 2010 Paul Davis 

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

#ifndef __pbd_stateful_diff_command_h__
#define __pbd_stateful_diff_command_h__

#include <boost/shared_ptr.hpp>
#include <boost/weak_ptr.hpp>
#include "pbd/command.h"

namespace PBD
{

class StatefulDestructible;	
class PropertyList;

/** A Command which stores its action as the differences between the before and after
 *  state of a Stateful object.
 */
class StatefulDiffCommand : public Command
{
public:
	StatefulDiffCommand (boost::shared_ptr<StatefulDestructible>);
	StatefulDiffCommand (boost::shared_ptr<StatefulDestructible>, XMLNode const &);
	~StatefulDiffCommand ();

	void operator() ();
	void undo ();
        
	XMLNode& get_state ();

private:
	boost::weak_ptr<Stateful> _object; ///< the object in question
        PBD::PropertyList* _undo; ///< its (partial) state before the command, to allow undo
        PBD::PropertyList* _redo;  ///< its (partial) state after the operation, to allow redo
};

};

#endif /* __pbd_stateful_diff_command_h__ */

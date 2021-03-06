/*
    Copyright (C) 2008 Paul Davis
    Author: Sakari Bergen

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

#ifndef __ardour_export_filename_h__
#define __ardour_export_filename_h__

#include <boost/shared_ptr.hpp>
#include <glibmm/ustring.h>
#include "pbd/statefuldestructible.h"

namespace ARDOUR
{

class Session;
class ExportTimespan;
class ExportChannelConfiguration;
class ExportFormatSpecification;

class ExportFilename {
  private:

	typedef boost::shared_ptr<ExportTimespan> TimespanPtr;
	typedef boost::shared_ptr<ExportChannelConfiguration> ChannelConfigPtr;
	typedef boost::shared_ptr<ExportFormatSpecification const> FormatPtr;

  public:

	enum DateFormat {
		D_None,
		D_ISO,       // ISO 8601 full date
		D_ISOShortY, // Like ISO 8601, but short year representation
		D_BE,        // big endian (no deliminator)
		D_BEShortY   // big endian short year representation
	};

	enum TimeFormat {
		T_None,
		T_NoDelim,
		T_Delim
	};

  private:
	friend class ExportElementFactory;
	ExportFilename (Session & session);

  public:
	/* Serialization */

	XMLNode & get_state ();
	int set_state (const XMLNode &);

	/* data access */

	Glib::ustring get_path (FormatPtr format) const;
	Glib::ustring get_folder () const { return folder; }

	TimeFormat get_time_format () const { return time_format; }
	DateFormat get_date_format () const { return date_format; }
	Glib::ustring get_time_format_str (TimeFormat format) const;
	Glib::ustring get_date_format_str (DateFormat format) const;

	Glib::ustring get_label () const { return label; }
	uint32_t get_revision () const { return revision; }

	/* data modification */

	void set_time_format (TimeFormat format);
	void set_date_format (DateFormat format);
	void set_label (Glib::ustring value);
	void set_revision (uint32_t value) { revision = value; }
	void set_channel (uint32_t value) { channel = value; }
	bool set_folder (Glib::ustring path);

	void set_timespan (TimespanPtr ts) { timespan = ts; }
	void set_channel_config (ChannelConfigPtr cc) { channel_config = cc; }

	/* public members */

	bool include_label;
	bool include_session;
	bool include_revision;
	bool include_channel_config;
	bool include_channel;
	bool include_timespan;
	bool include_time;
	bool include_date;

  private:

	Session & session;

	Glib::ustring   label;
	uint32_t  revision;
	uint32_t  channel;

	Glib::ustring   folder;

	DateFormat date_format;
	TimeFormat time_format;

	Glib::ustring get_formatted_time (Glib::ustring const & format) const;
	// Due to the static allocation used in strftime(), no destructor or copy-ctor is needed for this
	struct tm * time_struct;

	TimespanPtr timespan;
	ChannelConfigPtr channel_config;

	/* Serialization helpers */

	typedef std::pair<bool, Glib::ustring> FieldPair;

	void add_field (XMLNode * node, Glib::ustring const & name, bool enabled, Glib::ustring const & value = "");
	FieldPair get_field (XMLNode const & node, Glib::ustring const & name);
	FieldPair analyse_folder ();
};


} // namespace ARDOUR

#endif /* __ardour_export_filename_h__ */

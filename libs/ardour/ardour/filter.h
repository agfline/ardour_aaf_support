/*
 * Copyright (C) 2007-2011 David Robillard <d@drobilla.net>
 * Copyright (C) 2007-2015 Paul Davis <paul@linuxaudiosystems.com>
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

#pragma once

#include <vector>

#include "ardour/libardour_visibility.h"
#include "ardour/types.h"

namespace PBD {
	class Progress;
}

namespace ARDOUR {

class Region;
class Session;

class LIBARDOUR_API Filter {

  public:
	virtual ~Filter() {}

	virtual int run (std::shared_ptr<ARDOUR::Region>, PBD::Progress* progress = 0) = 0;
	std::vector<std::shared_ptr<ARDOUR::Region> > results;

  protected:
	Filter (ARDOUR::Session& s) : session(s) {}

	int make_new_sources (std::shared_ptr<ARDOUR::Region>, ARDOUR::SourceList&, std::string suffix = "", bool use_session_sample_rate = true);
	int finish (std::shared_ptr<ARDOUR::Region>, ARDOUR::SourceList&, std::string region_name = "");

	ARDOUR::Session& session;
};

} /* namespace */


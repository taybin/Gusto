#ifndef __ardour_readable_h__
#define __ardour_readable_h__

#include "ardour/types.h"

namespace ARDOUR {

class Readable {
  public:
	Readable () {}
	virtual ~Readable() {}

	virtual framecnt_t read (Sample*, framepos_t pos, framecnt_t cnt, int channel) const = 0;
	virtual framecnt_t readable_length() const = 0;
	virtual uint32_t  n_channels () const = 0;
};

}

#endif /* __ardour_readable_h__ */

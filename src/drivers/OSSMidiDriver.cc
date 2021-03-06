/*
 *  OSSMidiDriver.cc
 *
 *  Copyright (c) 2001-2012 Nick Dowell
 *
 *  This file is part of amsynth.
 *
 *  amsynth is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  amsynth is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with amsynth.  If not, see <http://www.gnu.org/licenses/>.
 */

#if HAVE_CONFIG_H
#include "../../config.h"
#endif

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>

#include "OSSMidiDriver.h"

class OSSMidiDriver : public MidiDriver
{
public:
	OSSMidiDriver();
  	virtual ~OSSMidiDriver();
	
	int open( Config & config );
	int close();
	
	int read(unsigned char *bytes, unsigned maxBytes);
	int write_cc(unsigned int channel, unsigned int param, unsigned int value);
	
private:
		int _fd;
};


OSSMidiDriver::OSSMidiDriver()
:	_fd(-1)
{
}

OSSMidiDriver::~OSSMidiDriver()
{
    close();
}

int OSSMidiDriver::open( Config & config )
{
	if (_fd == -1) 
	{
		const char *dev = config.oss_midi_device.c_str();
		_fd = ::open(dev, O_RDONLY | O_NONBLOCK, 0);
	}
	return (_fd > -1) ? 0 : -1;
}

int OSSMidiDriver::close()
{
	if (_fd > -1)
	{
		::close(_fd);
		_fd = -1;
	}
	return 0;
}

int OSSMidiDriver::read(unsigned char *bytes, unsigned maxBytes)
{
    return ::read(_fd, bytes, maxBytes);
}


int OSSMidiDriver::write_cc(unsigned int /*channel*/, unsigned int /*param*/, unsigned int /*value*/)
{
    return -1;
}

MidiDriver* CreateOSSMidiDriver() { return new OSSMidiDriver; }

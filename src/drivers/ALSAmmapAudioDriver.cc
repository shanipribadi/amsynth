/*
 *  ALSAmmapAudioDriver.cc
 *
 *  Copyright (c) 2001-2012 Nick Dowell, Karsten Wiese
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

#include <iostream>

#include "ALSAmmapAudioDriver.h"

int
ALSAmmapAudioDriver::xrun_recovery()
{
#ifdef WITH_ALSA
        if (err == -EPIPE) {    /* under-run */
				periods = 0;
                err = snd_pcm_prepare(playback_handle);
                if (err < 0){
                        cerr << "Can't recovery from underrun, prepare failed: " << snd_strerror(err) << "\n";
return -1;
				}
                return 0;
        } else if (err == -ESTRPIPE) {
                while ((err = snd_pcm_resume(playback_handle)) == -EAGAIN)
                        sleep(1);       /* wait until the suspend flag is released */
                if (err < 0) {
						periods = 0;
                        err = snd_pcm_prepare(playback_handle);
                        if (err < 0){
                                cerr << "Can't recovery from suspend, prepare failed: " << snd_strerror(err) << "\n";
return -1;
						}
                }
                return 0;
        }
	return 0;
#else
	return -1;
#endif //with_alsa
}


int
ALSAmmapAudioDriver::write(float *buffer, int frames)
{
#ifdef WITH_ALSA
	int i,p;
	snd_pcm_sframes_t avail;
	snd_pcm_uframes_t offset, lframes = frames / 2;
	const snd_pcm_channel_area_t* areas;
	short int*		audiobuf;

	while( 1 )
	{
		avail = snd_pcm_avail_update( playback_handle);
		if (avail < 0)
		{
//			char b[ 80];
//			sprintf( b, "%i", err = avail);
//			cerr << "snd_pcm_avail_update error " << b << "=\"" 
//			<< snd_strerror( err) << "\"\n";
			return xrun_recovery();
		}
		lframes = frames / 2;
		if( (int)avail >= (int)lframes ) break;
		if( 0 > ( err = snd_pcm_wait( playback_handle, -1)))
		{
//			cerr << "snd_pcm_wait error\n";
			config->xruns++;
			return xrun_recovery();
		}
	}

	if( 0 > ( err = snd_pcm_mmap_begin( playback_handle, &areas, &offset, &lframes)))
	{
		cerr << "snd_pcm_mmap_begin error\n";
		xrun_recovery();
		// Return an error code so we can quickly test during initialisation.
		// Won't stop playback at runtime because AudioOutput checks for a return code of -1
		return 0xfeedface;
	}

	audiobuf = (short int*)areas[ 0].addr + 2*offset;
	p=0;
	for( i=0; i<(frames); i++ )
		*(audiobuf++) = (int)(32767*( 1.0 + buffer[i])) - 32767;

	if( 0 > ( err = snd_pcm_mmap_commit(  playback_handle, offset, lframes)))
	{
		cerr << "snd_pcm_mmap_commit error\n";
		return xrun_recovery();
	}

	if( periods < 2)
		if( 2 == ++periods )
//				cerr << "snd_pcm_start soon\n";
			if( 0 > ( err = snd_pcm_start( playback_handle ) ) )
			{
				cerr << "snd_pcm_start error\n";
				return -1;
			}
	/*else{
		if( ++periods == 0)
			periods = 2;
		if( ! ( periods % 0x100))
				cerr << "( periods % 0x100)\n";
	}*/
	return 0;
#else //with_alsa
	UNUSED_PARAM(buffer);
	UNUSED_PARAM(frames);
	return -1;
#endif
}

int
ALSAmmapAudioDriver::open( Config & config )
{
#ifdef WITH_ALSA
	if (playback_handle != NULL) return 0;
	
	_channels = config.channels;
	_rate = config.sample_rate;

	if(snd_pcm_open(&playback_handle, config.alsa_audio_device.c_str(), SND_PCM_STREAM_PLAYBACK, 0)<0){
		cerr << "ALSA: cannot open audio device " << config.alsa_audio_device << endl;
		return -1;
	}
    snd_pcm_hw_params_alloca( &hw_params );
    snd_pcm_hw_params_any( playback_handle, hw_params );
    snd_pcm_hw_params_set_access( playback_handle, hw_params, SND_PCM_ACCESS_MMAP_INTERLEAVED/*SND_PCM_ACCESS_RW_INTERLEAVED*/ );
    snd_pcm_hw_params_set_format( playback_handle, hw_params, SND_PCM_FORMAT_S16_LE );
    snd_pcm_hw_params_set_rate_near( playback_handle, hw_params, _rate, 0 );
    snd_pcm_hw_params_set_channels( playback_handle, hw_params, _channels );
	snd_pcm_hw_params_set_periods( playback_handle, hw_params, 16, 0 );
	snd_pcm_hw_params_set_period_size( playback_handle, hw_params, config.buffer_size, 0 );
    snd_pcm_hw_params( playback_handle, hw_params );
	
	config.sample_rate = snd_pcm_hw_params_get_rate( hw_params, 0 );
	config.current_audio_driver = "ALSA-MMAP";
#ifdef ENABLE_REALTIME
	config.current_audio_driver_wants_realtime = 1;
#endif
	
	this->config = &config;

	periods = 0;
	return 0;
#else
	UNUSED_PARAM(config);
	return -1;
#endif
}

void ALSAmmapAudioDriver::close()
{
#ifdef WITH_ALSA
	if (playback_handle != NULL) snd_pcm_close (playback_handle);
	playback_handle = NULL;
#endif
}

int ALSAmmapAudioDriver::setChannels(int channels)
{
  // WRITE ME!
  _channels = channels;
  return 0;
}

int ALSAmmapAudioDriver::setRate(int rate)
{
  // WRITE ME!
  _rate = rate;
  return 0;
}

int ALSAmmapAudioDriver::setRealtime()
{
  // WRITE ME!
  return 0;
}

ALSAmmapAudioDriver::ALSAmmapAudioDriver()
{
#ifdef WITH_ALSA
	playback_handle = NULL;
#endif
}

ALSAmmapAudioDriver::~ALSAmmapAudioDriver()
{
  close();
}

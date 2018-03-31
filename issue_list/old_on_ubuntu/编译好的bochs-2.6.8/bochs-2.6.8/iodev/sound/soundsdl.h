/////////////////////////////////////////////////////////////////////////
// $Id: soundsdl.h 12774 2015-06-04 08:58:02Z vruppert $
/////////////////////////////////////////////////////////////////////////
//
//  Copyright (C) 2012-2015  The Bochs Project
//
//  This library is free software; you can redistribute it and/or
//  modify it under the terms of the GNU Lesser General Public
//  License as published by the Free Software Foundation; either
//  version 2 of the License, or (at your option) any later version.
//
//  This library is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
//  Lesser General Public License for more details.
//
//  You should have received a copy of the GNU Lesser General Public
//  License along with this library; if not, write to the Free Software
//  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA

// Lowlevel sound output support for SDL written by Volker Ruppert


#if BX_HAVE_SOUND_SDL

#include "bochs.h"
#include <SDL_audio.h>

// the waveout class

class bx_soundlow_waveout_sdl_c : public bx_soundlow_waveout_c {
public:
  bx_soundlow_waveout_sdl_c();
  virtual ~bx_soundlow_waveout_sdl_c();

  virtual int openwaveoutput(const char *wavedev);
  virtual int set_pcm_params(bx_pcm_param_t *param);
  virtual int convert_pcm_data(void);

  virtual void unregister_wave_callback(int callback_id);
  virtual bx_bool mixer_common(Bit8u *buffer, int len);
private:
  bx_bool WaveOpen;
  SDL_AudioSpec fmt;
};

class bx_sound_sdl_c : public bx_sound_lowlevel_c {
public:
  bx_sound_sdl_c();
  virtual ~bx_sound_sdl_c() {}

  virtual bx_soundlow_waveout_c* get_waveout();
};

#endif  // BX_HAVE_SOUND_SDL

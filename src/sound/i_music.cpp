/*
** i_music.cpp
** Plays music
**
**---------------------------------------------------------------------------
** Copyright 1998-2010 Randy Heit
** All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions
** are met:
**
** 1. Redistributions of source code must retain the above copyright
**    notice, this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
** 3. The name of the author may not be used to endorse or promote products
**    derived from this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
** IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
** OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
** IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
** INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
** NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
** THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**---------------------------------------------------------------------------
**
*/

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#else
#include <SDL.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <wordexp.h>
#include <stdio.h>
#include "mus2midi.h"
#define FALSE 0
#define TRUE 1
extern void ChildSigHandler (int signum);
#endif

#include <ctype.h>
#include <assert.h>
#include <stdio.h>

#include "i_musicinterns.h"
#include "doomtype.h"
#include "m_argv.h"
#include "i_music.h"
#include "w_wad.h"
#include "c_console.h"
#include "c_dispatch.h"
#include "i_system.h"
#include "i_sound.h"
#include "s_sound.h"
#include "m_swap.h"
#include "tempfiles.h"
#include "templates.h"
#include "stats.h"
#include <zmusic.h>

#define GZIP_ID1		31
#define GZIP_ID2		139
#define GZIP_CM			8
#define GZIP_ID			MAKE_ID(GZIP_ID1,GZIP_ID2,GZIP_CM,0)

#define GZIP_FTEXT		1
#define GZIP_FHCRC		2
#define GZIP_FEXTRA		4
#define GZIP_FNAME		8
#define GZIP_FCOMMENT	16

extern int MUSHeaderSearch(const BYTE *head, int len);

EXTERN_CVAR (Int, snd_samplerate)
EXTERN_CVAR (Int, snd_mididevice)


static bool MusicDown = true;

static BYTE *ungzip(BYTE *data, int *size);

int		nomusic = 0;
float	relative_volume = 1.f;
float	saved_relative_volume = 1.0f;	// this could be used to implement an ACS FadeMusic function

//==========================================================================
//
// CVAR snd_musicvolume
//
// Maximum volume of MOD/stream music.
//==========================================================================

CUSTOM_CVAR (Float, snd_musicvolume, 0.25f, CVAR_ARCHIVE|CVAR_GLOBALCONFIG)
{
	if (self < 0.f)
		self = 0.f;
	else if (self > 1.f)
		self = 1.f;
	else
	{
		// Set general music volume.
		ChangeMusicSetting(zmusic_snd_musicvolume, nullptr, self);
		if (GSnd != NULL)
		{
			GSnd->SetMusicVolume(clamp<float>(self * relative_volume, 0, 1));
		}
		// For music not implemented through the digital sound system,
		// let them know about the change.
		if (S_GetMusPlaying()->handle != NULL)
		{
			ZMusic_VolumeChanged(S_GetMusPlaying()->handle);
		}
		else
		{ // If the music was stopped because volume was 0, start it now.
			S_RestartMusic();
		}
	}
}

//==========================================================================
//
//
//
//==========================================================================

void I_InitMusic (void)
{
	static bool setatterm = false;

	snd_musicvolume.Callback ();

	nomusic = !!Args->CheckParm("-nomusic") || !!Args->CheckParm("-nosound") || !!Args->CheckParm("-host");

#ifdef _WIN32
	I_InitMusicWin32 ();
#endif // _WIN32
	
	if (!setatterm)
	{
		setatterm = true;
		atterm (I_ShutdownMusic);
	
#ifndef _WIN32
		signal (SIGCHLD, ChildSigHandler);
#endif
	}
	MusicDown = false;
}


//==========================================================================
//
//
//
//==========================================================================

void I_ShutdownMusic(void)
{
	if (MusicDown)
		return;
	MusicDown = true;
	S_StopMusic (true);
#ifdef _WIN32
	I_ShutdownMusicWin32();
#endif // _WIN32
}


//==========================================================================
//
// identify MIDI file type
//
//==========================================================================

static EMIDIType IdentifyMIDIType(DWORD *id, int size)
{
	// Check for MUS format
	// Tolerate sloppy wads by searching up to 32 bytes for the header
	if (MUSHeaderSearch((BYTE*)id, size) >= 0)
	{
		return MIDI_MUS;
	}
	// Check for HMI format
	else 
	if (id[0] == MAKE_ID('H','M','I','-') &&
		id[1] == MAKE_ID('M','I','D','I') &&
		id[2] == MAKE_ID('S','O','N','G'))
	{
		return MIDI_HMI;
	}
	// Check for HMP format
	else
	if (id[0] == MAKE_ID('H','M','I','M') &&
		id[1] == MAKE_ID('I','D','I','P'))
	{
		return MIDI_HMI;
	}
	// Check for XMI format
	else
	if ((id[0] == MAKE_ID('F','O','R','M') &&
		 id[2] == MAKE_ID('X','D','I','R')) ||
		((id[0] == MAKE_ID('C','A','T',' ') || id[0] == MAKE_ID('F','O','R','M')) &&
		 id[2] == MAKE_ID('X','M','I','D')))
	{
		return MIDI_XMI;
	}
	// Check for MIDI format
	else if (id[0] == MAKE_ID('M','T','h','d'))
	{
		return MIDI_MIDI;
	}
	else
	{
		return MIDI_NOTMIDI;
	}
}

//==========================================================================
//
// ungzip
//
// VGZ files are compressed with gzip, so we need to uncompress them before
// handing them to GME.
//
//==========================================================================

BYTE *ungzip(BYTE *data, int *complen)
{
	const BYTE *max = data + *complen - 8;
	const BYTE *compstart = data + 10;
	BYTE flags = data[3];
	unsigned isize;
	BYTE *newdata;
	z_stream stream;
	int err;

	// Find start of compressed data stream
	if (flags & GZIP_FEXTRA)
	{
		compstart += 2 + LittleShort(*(WORD *)(data + 10));
	}
	if (flags & GZIP_FNAME)
	{
		while (compstart < max && *compstart != 0)
		{
			compstart++;
		}
	}
	if (flags & GZIP_FCOMMENT)
	{
		while (compstart < max && *compstart != 0)
		{
			compstart++;
		}
	}
	if (flags & GZIP_FHCRC)
	{
		compstart += 2;
	}
	if (compstart >= max - 1)
	{
		return NULL;
	}

	// Decompress
	isize = LittleLong(*(DWORD *)(data + *complen - 4));
	newdata = new BYTE[isize];

	stream.next_in = (Bytef *)compstart;
	stream.avail_in = (uInt)(max - compstart);
	stream.next_out = newdata;
	stream.avail_out = isize;
	stream.zalloc = (alloc_func)0;
	stream.zfree = (free_func)0;

	err = inflateInit2(&stream, -MAX_WBITS);
	if (err != Z_OK)
	{
		delete[] newdata;
		return NULL;
	}

	err = inflate(&stream, Z_FINISH);
	if (err != Z_STREAM_END)
	{
		inflateEnd(&stream);
		delete[] newdata;
		return NULL;
	}
	err = inflateEnd(&stream);
	if (err != Z_OK)
	{
		delete[] newdata;
		return NULL;
	}
	*complen = isize;
	return newdata;
}

//==========================================================================
//
// 
//
//==========================================================================

static SoundStream *musicStream;
static TArray<int16_t> convert;
static bool FillStream(SoundStream* stream, void* buff, int len, void* userdata)
{
	bool written;
	if (S_GetMusPlaying()->isfloat)
	{
		written = ZMusic_FillStream(S_GetMusPlaying()->handle, buff, len);
		if (S_GetMusPlaying()->replayGainFactor != 1.f)
		{
			float* fbuf = (float*)buff;
			for (int i = 0; i < len / 4; i++)
			{
				fbuf[i] *= S_GetMusPlaying()->replayGainFactor;
			}
		}
	}
	else
	{
		// To apply replay gain we need floating point streaming data, so 16 bit input needs to be converted here.
		convert.Resize(len / 2);
		written = ZMusic_FillStream(S_GetMusPlaying()->handle, &convert[0], len/2);
		float* fbuf = (float*)buff;
		for (int i = 0; i < len / 4; i++)
		{
			fbuf[i] = convert[i] * S_GetMusPlaying()->replayGainFactor * (1.f/32768.f);
		}
	}

	if (!written)
	{
		memset((char*)buff, 0, len);
		return false;
	}
	return true;
}


void S_CreateStream()
{
	if (!S_GetMusPlaying()->handle) return;
	SoundStreamInfo fmt;
	ZMusic_GetStreamInfo(S_GetMusPlaying()->handle, &fmt);
	// always create a floating point streaming buffer so we can apply replay gain without risk of integer overflows.
	S_GetMusPlaying()->isfloat = fmt.mNumChannels > 0;
	if (!S_GetMusPlaying()->isfloat) fmt.mBufferSize *= 2;
	if (fmt.mBufferSize > 0) // if buffer size is 0 the library will play the song itself (e.g. Windows system synth.)
	{
		int flags = SoundStream::Float;
		if (abs(fmt.mNumChannels) < 2) flags |= SoundStream::Mono;

		musicStream = GSnd->CreateStream(FillStream, fmt.mBufferSize, flags, fmt.mSampleRate, nullptr);
		if (musicStream) musicStream->Play(true, 1);
	}
}

void S_PauseStream(bool paused)
{
	if (musicStream) musicStream->SetPaused(paused);
}

void S_StopStream()
{
	if (musicStream)
	{
		musicStream->Stop();
		musicStream = nullptr;
	}
}

//==========================================================================
//
// 
//
//==========================================================================

void I_SetRelativeVolume(float vol)
{
	relative_volume = (float)vol;
	ChangeMusicSetting(zmusic_relative_volume, nullptr, (float)vol);
	snd_musicvolume.Callback();
}

//==========================================================================
//
// Sets relative music volume. Takes $musicvolume in SNDINFO into consideration
//
//==========================================================================

void I_SetMusicVolume (float factor)
{
	factor = clamp<float>(factor, 0, 2.0f);
	I_SetRelativeVolume((float)factor);
}

//==========================================================================
//
// test a relative music volume
//
//==========================================================================

CCMD(testmusicvol)
{
	if (argv.argc() > 1) 
	{
		I_SetRelativeVolume((float)strtod(argv[1], nullptr));
	}
	else
		Printf("Current relative volume is %1.2f\n", relative_volume);
}

//==========================================================================
//
// STAT music
//
//==========================================================================

ADD_STAT(music)
{
	if (S_GetMusPlaying()->handle != NULL)
	{
		return ZMusic_GetStats(S_GetMusPlaying()->handle);
	}
	return "No song playing";
}

//==========================================================================
//
// Common loader for the dumpers.
//
//==========================================================================

static ZMusic_MidiSource GetMIDISource(const char *fn)
{
	FString src = fn;
	if (src.Compare("*") == 0) src = S_GetMusPlaying()->name;

	auto lump = Wads.CheckNumForName(src, ns_music);
	if (lump < 0) lump = Wads.CheckNumForFullName(src);
	if (lump < 0)
	{
		Printf("Cannot find MIDI lump %s.\n", src.GetChars());
		return nullptr;
	}

	auto wlump = Wads.ReadLump(lump);
	MemoryReader reader((char*)wlump.GetMem(), wlump.GetSize());

	uint32_t id[32 / 4];

	if (reader.Read(id, 32) != 32 || reader.Seek(-32, SEEK_CUR) != 0)
	{
		Printf("Unable to read lump %s\n", src.GetChars());
		return nullptr;
	}
	auto type = ZMusic_IdentifyMIDIType(id, 32);
	if (type == MIDI_NOTMIDI)
	{
		Printf("%s is not MIDI-based.\n", src.GetChars());
		return nullptr;
	}

	auto source = ZMusic_CreateMIDISource((uint8_t*)wlump.GetMem(), wlump.GetSize(), type);

	if (source == nullptr)
	{
		Printf("Unable to open %s: %s\n", src.GetChars(), ZMusic_GetLastError());
		return nullptr;
	}
	return source;
}

//==========================================================================
//
// CCMD writewave
//
// If the current song can be represented as a waveform, dump it to
// the specified file on disk. The sample rate parameter is merely a
// suggestion, and the dumper is free to ignore it.
//
//==========================================================================

CCMD (writewave)
{
	if (argv.argc() >= 3 && argv.argc() <= 7)
	{
		auto source = GetMIDISource(argv[1]);
		if (source == nullptr) return;

		EMidiDevice dev = MDEV_DEFAULT;
#ifndef ZMUSIC_LITE
		if (argv.argc() >= 6)
		{
			if (!stricmp(argv[5], "WildMidi")) dev = MDEV_WILDMIDI;
			else if (!stricmp(argv[5], "GUS")) dev = MDEV_GUS;
			else if (!stricmp(argv[5], "Timidity") || !stricmp(argv[5], "Timidity++")) dev = MDEV_TIMIDITY;
			else if (!stricmp(argv[5], "FluidSynth")) dev = MDEV_FLUIDSYNTH;
			else if (!stricmp(argv[5], "OPL")) dev = MDEV_OPL;
			else if (!stricmp(argv[5], "OPN")) dev = MDEV_OPN;
			else if (!stricmp(argv[5], "ADL")) dev = MDEV_ADL;
			else
			{
				Printf("%s: Unknown MIDI device\n", argv[5]);
				return;
			}
		}
#endif
		// We must stop the currently playing music to avoid interference between two synths. 
		auto savedsong = S_GetMusPlaying();
		S_StopMusic(true);
		if (dev == MDEV_DEFAULT && snd_mididevice >= 0) dev = MDEV_FLUIDSYNTH;	// The Windows system synth cannot dump a wave.
		if (!ZMusic_MIDIDumpWave(source, dev, argv.argc() < 6 ? nullptr : argv[6], argv[2], argv.argc() < 4 ? 0 : (int)strtol(argv[3], nullptr, 10), argv.argc() < 5 ? 0 : (int)strtol(argv[4], nullptr, 10)))
		{
			Printf("MIDI dump of %s failed: %s\n",argv[1], ZMusic_GetLastError());
		}

		S_ChangeMusic(savedsong->name, savedsong->baseorder, savedsong->loop, true);
	}
	else
	{
		Printf ("Usage: writewave <midi> <filename> [subsong] [sample rate] [synth] [soundfont]\n"
		" - use '*' as song name to dump the currently playing song\n"
		" - use 0 for subsong and sample rate to play the default\n");
	}
}

//==========================================================================
//
// CCMD writemidi
//
// If the currently playing song is a MIDI variant, write it to disk.
// If successful, the current song will restart, since MIDI file generation
// involves a simulated playthrough of the song.
//
//==========================================================================

CCMD (writemidi)
{
	if (argv.argc() != 3)
	{
		Printf("Usage: writemidi <midisong> <filename> - use '*' as song name to dump the currently playing song\n");
		return;
	}
	auto source = GetMIDISource(argv[1]);
	if (source == nullptr)
	{
		Printf("Unable to open %s: %s\n", argv[1], ZMusic_GetLastError());
		return;
	}
	if (!ZMusic_WriteSMF(source, argv[1], 1))
	{
		Printf("Unable to write %s\n", argv[1]);
	}
}

//==========================================================================
//
// ADL Midi device
//
//==========================================================================

#define FORWARD_CVAR(key) \
	decltype(*self) newval; \
	auto ret = ChangeMusicSetting(zmusic_##key, mus_playing.handle, *self, &newval); \
	self = (decltype(*self))newval; \
	if (ret) S_MIDIDeviceChanged(-1);

#define FORWARD_BOOL_CVAR(key) \
	int newval; \
	auto ret = ChangeMusicSetting(zmusic_##key, mus_playing.handle,*self, &newval); \
	self = !!newval; \
	if (ret) S_MIDIDeviceChanged(-1);

#define FORWARD_STRING_CVAR(key) \
	auto ret = ChangeMusicSetting(zmusic_##key, mus_playing.handle,*self); \
	if (ret) S_MIDIDeviceChanged(-1); 

#ifndef ZMUSIC_LITE
CUSTOM_CVAR(Int, adl_chips_count, 6, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	ChangeMusicSetting(zmusic_adl_chips_count, S_GetMusPlaying()->handle, self);
}

CUSTOM_CVAR(Int, adl_emulator_id, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	ChangeMusicSetting(zmusic_adl_emulator_id, S_GetMusPlaying()->handle, self);
}

CUSTOM_CVAR(Bool, adl_run_at_pcm_rate, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	ChangeMusicSetting(zmusic_adl_run_at_pcm_rate, S_GetMusPlaying()->handle, self);
}

CUSTOM_CVAR(Bool, adl_fullpan, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	ChangeMusicSetting(zmusic_adl_fullpan, S_GetMusPlaying()->handle, self);
}

CUSTOM_CVAR(Int, adl_bank, 14, CVAR_ARCHIVE)
{
	ChangeMusicSetting(zmusic_adl_bank, S_GetMusPlaying()->handle, self);
}

CUSTOM_CVAR(Bool, adl_use_custom_bank, false, CVAR_ARCHIVE)
{
	ChangeMusicSetting(zmusic_adl_use_custom_bank, S_GetMusPlaying()->handle, self);
}

CUSTOM_CVAR(String, adl_custom_bank, "", CVAR_ARCHIVE)
{
	ChangeMusicSetting(zmusic_adl_custom_bank, S_GetMusPlaying()->handle, self);
}

CUSTOM_CVAR(Int, adl_volume_model, 0 /*ADLMIDI_VolumeModel_AUTO*/, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	ChangeMusicSetting(zmusic_adl_volume_model, S_GetMusPlaying()->handle, self);
}
#endif

//==========================================================================
//
// Fluidsynth MIDI device
//
//==========================================================================

CUSTOM_CVAR(String, fluid_lib, "", CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	ChangeMusicSetting(zmusic_fluid_lib, S_GetMusPlaying()->handle, self);
}

CUSTOM_CVAR(String, fluid_patchset, "gm.sf2", CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	ChangeMusicSetting(zmusic_fluid_patchset, S_GetMusPlaying()->handle, self);
}

CUSTOM_CVAR(Float, fluid_gain, 0.75, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0)
		self = 0;
	else if (self > 10)
		self = 10;

	ChangeMusicSetting(zmusic_fluid_gain, S_GetMusPlaying()->handle, self);
}

CUSTOM_CVAR(Bool, fluid_reverb, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	ChangeMusicSetting(zmusic_fluid_reverb, S_GetMusPlaying()->handle, self);
}

CUSTOM_CVAR(Bool, fluid_chorus, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	ChangeMusicSetting(zmusic_fluid_chorus, S_GetMusPlaying()->handle, self);
}

CUSTOM_CVAR(Int, fluid_voices, 128, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	ChangeMusicSetting(zmusic_fluid_voices, S_GetMusPlaying()->handle, self);
}

CUSTOM_CVAR(Int, fluid_interp, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	ChangeMusicSetting(zmusic_fluid_interp, S_GetMusPlaying()->handle, self);
}

CUSTOM_CVAR(Int, fluid_samplerate, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	ChangeMusicSetting(zmusic_fluid_samplerate, S_GetMusPlaying()->handle, self);
}

CUSTOM_CVAR(Int, fluid_threads, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	ChangeMusicSetting(zmusic_fluid_threads, S_GetMusPlaying()->handle, self);
}

CUSTOM_CVAR(Float, fluid_reverb_roomsize, 0.61f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	ChangeMusicSetting(zmusic_fluid_reverb_roomsize, S_GetMusPlaying()->handle, self);
}

CUSTOM_CVAR(Float, fluid_reverb_damping, 0.23f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	ChangeMusicSetting(zmusic_fluid_reverb_damping, S_GetMusPlaying()->handle, self);
}

CUSTOM_CVAR(Float, fluid_reverb_width, 0.76f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	ChangeMusicSetting(zmusic_fluid_reverb_width, S_GetMusPlaying()->handle, self);
}

CUSTOM_CVAR(Float, fluid_reverb_level, 0.57f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	ChangeMusicSetting(zmusic_fluid_reverb_level, S_GetMusPlaying()->handle, self);
}

CUSTOM_CVAR(Int, fluid_chorus_voices, 3, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	ChangeMusicSetting(zmusic_fluid_chorus_voices, S_GetMusPlaying()->handle, self);
}

CUSTOM_CVAR(Float, fluid_chorus_level, 1.2f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	ChangeMusicSetting(zmusic_fluid_chorus_level, S_GetMusPlaying()->handle, self);
}

CUSTOM_CVAR(Float, fluid_chorus_speed, 0.3f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	ChangeMusicSetting(zmusic_fluid_chorus_speed, S_GetMusPlaying()->handle, self);
}

// depth is in ms and actual maximum depends on the sample rate
CUSTOM_CVAR(Float, fluid_chorus_depth, 8.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	ChangeMusicSetting(zmusic_fluid_chorus_depth, S_GetMusPlaying()->handle, self);
}

CUSTOM_CVAR(Int, fluid_chorus_type, 0/*FLUID_CHORUS_DEFAULT_TYPE*/, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	ChangeMusicSetting(zmusic_fluid_chorus_type, S_GetMusPlaying()->handle, self);
}

//==========================================================================
//
// OPL MIDI device
//
//==========================================================================

CUSTOM_CVAR(Int, opl_numchips, 2, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	ChangeMusicSetting(zmusic_opl_numchips, S_GetMusPlaying()->handle, self);
}

CUSTOM_CVAR(Int, opl_core, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	ChangeMusicSetting(zmusic_opl_core, S_GetMusPlaying()->handle, self);
}

CUSTOM_CVAR(Bool, opl_fullpan, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	ChangeMusicSetting(zmusic_opl_fullpan, S_GetMusPlaying()->handle, self);
}

//==========================================================================
//
// OPN MIDI device
//
//==========================================================================

#ifndef ZMUSIC_LITE
CUSTOM_CVAR(Int, opn_chips_count, 8, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	ChangeMusicSetting(zmusic_opn_chips_count, S_GetMusPlaying()->handle, self);
}

CUSTOM_CVAR(Int, opn_emulator_id, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	ChangeMusicSetting(zmusic_opn_emulator_id, S_GetMusPlaying()->handle, self);
}

CUSTOM_CVAR(Bool, opn_run_at_pcm_rate, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	ChangeMusicSetting(zmusic_opn_run_at_pcm_rate, S_GetMusPlaying()->handle, self);
}

CUSTOM_CVAR(Bool, opn_fullpan, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	ChangeMusicSetting(zmusic_opn_fullpan, S_GetMusPlaying()->handle, self);
}

CUSTOM_CVAR(Bool, opn_use_custom_bank, false, CVAR_ARCHIVE)
{
	ChangeMusicSetting(zmusic_opn_use_custom_bank, S_GetMusPlaying()->handle, self);
}

CUSTOM_CVAR(String, opn_custom_bank, "", CVAR_ARCHIVE)
{
	ChangeMusicSetting(zmusic_opn_custom_bank, S_GetMusPlaying()->handle, self);
}

//==========================================================================
//
// GUS MIDI device
//
//==========================================================================


CUSTOM_CVAR(String, midi_config, "", CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	ChangeMusicSetting(zmusic_gus_config, S_GetMusPlaying()->handle, self);
}

CUSTOM_CVAR(Bool, midi_dmxgus, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)	// This was 'true' but since it requires special setup that's not such a good idea.
{
	ChangeMusicSetting(zmusic_gus_dmxgus, S_GetMusPlaying()->handle, self);
}

CUSTOM_CVAR(String, gus_patchdir, "", CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	ChangeMusicSetting(zmusic_gus_patchdir, S_GetMusPlaying()->handle, self);
}

CUSTOM_CVAR(Int, midi_voices, 32, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	ChangeMusicSetting(zmusic_gus_midi_voices, S_GetMusPlaying()->handle, self);
}

CUSTOM_CVAR(Int, gus_memsize, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	ChangeMusicSetting(zmusic_gus_memsize, S_GetMusPlaying()->handle, self);
}

//==========================================================================
//
// Timidity++ device
//
//==========================================================================

CUSTOM_CVAR(Bool, timidity_modulation_wheel, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	ChangeMusicSetting(zmusic_timidity_modulation_wheel, S_GetMusPlaying()->handle, self);
}

CUSTOM_CVAR(Bool, timidity_portamento, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	ChangeMusicSetting(zmusic_timidity_portamento, S_GetMusPlaying()->handle, self);
}

CUSTOM_CVAR(Int, timidity_reverb, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	ChangeMusicSetting(zmusic_timidity_reverb, S_GetMusPlaying()->handle, self);
}

CUSTOM_CVAR(Int, timidity_reverb_level, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	ChangeMusicSetting(zmusic_timidity_reverb_level, S_GetMusPlaying()->handle, self);
}

CUSTOM_CVAR(Int, timidity_chorus, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	ChangeMusicSetting(zmusic_timidity_chorus, S_GetMusPlaying()->handle, self);
}

CUSTOM_CVAR(Bool, timidity_surround_chorus, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	ChangeMusicSetting(zmusic_timidity_surround_chorus, S_GetMusPlaying()->handle, self);
}

CUSTOM_CVAR(Bool, timidity_channel_pressure, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	ChangeMusicSetting(zmusic_timidity_channel_pressure, S_GetMusPlaying()->handle, self);
}

CUSTOM_CVAR(Int, timidity_lpf_def, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	ChangeMusicSetting(zmusic_timidity_lpf_def, S_GetMusPlaying()->handle, self);
}

CUSTOM_CVAR(Bool, timidity_temper_control, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	ChangeMusicSetting(zmusic_timidity_temper_control, S_GetMusPlaying()->handle, self);
}

CUSTOM_CVAR(Bool, timidity_modulation_envelope, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	ChangeMusicSetting(zmusic_timidity_modulation_envelope, S_GetMusPlaying()->handle, self);
}

CUSTOM_CVAR(Bool, timidity_overlap_voice_allow, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	ChangeMusicSetting(zmusic_timidity_overlap_voice_allow, S_GetMusPlaying()->handle, self);
}

CUSTOM_CVAR(Bool, timidity_drum_effect, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	ChangeMusicSetting(zmusic_timidity_drum_effect, S_GetMusPlaying()->handle, self);
}

CUSTOM_CVAR(Bool, timidity_pan_delay, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	ChangeMusicSetting(zmusic_timidity_pan_delay, S_GetMusPlaying()->handle, self);
}

CUSTOM_CVAR(Float, timidity_drum_power, 1.0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG) /* coef. of drum amplitude */
{
	ChangeMusicSetting(zmusic_timidity_drum_power, S_GetMusPlaying()->handle, self);
}

CUSTOM_CVAR(Int, timidity_key_adjust, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	ChangeMusicSetting(zmusic_timidity_key_adjust, S_GetMusPlaying()->handle, self);
}

CUSTOM_CVAR(Float, timidity_tempo_adjust, 1.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	ChangeMusicSetting(zmusic_timidity_tempo_adjust, S_GetMusPlaying()->handle, self);
}

CUSTOM_CVAR(Float, timidity_min_sustain_time, 5000.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	ChangeMusicSetting(zmusic_timidity_min_sustain_time, S_GetMusPlaying()->handle, self);
}
#endif

CUSTOM_CVAR(String, timidity_config, "zandronum", CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	ChangeMusicSetting(zmusic_timidity_config, S_GetMusPlaying()->handle, self);
}

//==========================================================================
//
// WildMidi
//
//==========================================================================

#ifndef ZMUSIC_LITE
CUSTOM_CVAR(String, wildmidi_config, "", CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	ChangeMusicSetting(zmusic_wildmidi_config, S_GetMusPlaying()->handle, self);
}

CUSTOM_CVAR(Bool, wildmidi_reverb, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	ChangeMusicSetting(zmusic_wildmidi_reverb, S_GetMusPlaying()->handle, self);
}

CUSTOM_CVAR(Bool, wildmidi_enhanced_resampling, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	ChangeMusicSetting(zmusic_wildmidi_enhanced_resampling, S_GetMusPlaying()->handle, self);
}
#endif
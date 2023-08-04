
#include "i_musicinterns.h"
#include "c_dispatch.h"
#include "i_music.h"
#include "i_system.h"

#include "templates.h"
#include "v_text.h"
#include "menu/menu.h"

static DWORD	nummididevices;
static bool		nummididevicesset;

#ifdef HAVE_FLUIDSYNTH
#define NUM_DEF_DEVICES 5
#else
#define NUM_DEF_DEVICES 4
#endif

#ifdef HAVE_FLUIDSYNTH
#define DEF_MIDIDEVICE -5
#else
#define DEF_MIDIDEVICE -1
#endif

void I_BuildMIDIMenuList(FOptionValues* opt)
{
	int amount;
	auto list = ZMusic_GetMidiDevices(&amount);

	for (int i = 0; i < amount; i++)
	{
		if (list[i].ID == 0) // [geNia] Device 0 crashes for some reason
			continue;
		FOptionValues::Pair* pair = &opt->mValues[opt->mValues.Reserve(1)];
		pair->Text = list[i].Name;
		pair->Value = (float)list[i].ID;
	}
}

static void PrintMidiDevice(int id, const char* name, WORD tech, DWORD support);

CCMD(snd_listmididevices)
{
	int amount;
	auto list = ZMusic_GetMidiDevices(&amount);

	for (int i = 0; i < amount; i++)
	{
		if (list[i].ID == 0) // [geNia] Device 0 crashes for some reason
			continue;
		PrintMidiDevice(list[i].ID, list[i].Name, list[i].Technology, 0);
	}
}

CUSTOM_CVAR(Int, snd_mididevice, DEF_MIDIDEVICE, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	int amount;
	auto list = ZMusic_GetMidiDevices(&amount);

	bool found = false;
	// The list is not necessarily contiguous so we need to check each entry.
	for (int i = 0; i < amount; i++)
	{
		if (self == list[i].ID)
		{
			found = true;
			break;
		}
	}
	if (!found)
	{
		// Don't do repeated message spam if there is no valid device.
		if (self != 0 && self != -1)
		{
			Printf("ID out of range. Using default device.\n");
		}
		if (self != DEF_MIDIDEVICE) self = DEF_MIDIDEVICE;
		return;
	}
	bool change = ChangeMusicSetting(zmusic_snd_mididevice, nullptr, self);
	if (change) S_MIDIDeviceChanged();
}

static void PrintMidiDevice(int id, const char* name, WORD tech, DWORD support)
{
	if (id == snd_mididevice)
	{
		Printf(TEXTCOLOR_BOLD);
	}
	Printf("% 2d. %s : ", id, name);
	switch (tech)
	{
	case MIDIDEV_MIDIPORT:		Printf("MIDIPORT");		break;
	case MIDIDEV_SYNTH:			Printf("SYNTH");			break;
	case MIDIDEV_SQSYNTH:		Printf("SQSYNTH");			break;
	case MIDIDEV_FMSYNTH:		Printf("FMSYNTH");			break;
	case MIDIDEV_MAPPER:		Printf("MAPPER");			break;
	case MIDIDEV_WAVETABLE:		Printf("WAVETABLE");		break;
	case MIDIDEV_SWSYNTH:		Printf("SWSYNTH");			break;
	}
	Printf(TEXTCOLOR_NORMAL "\n");
}

#ifdef _WIN32
void I_InitMusicWin32 ()
{
	nummididevices = midiOutGetNumDevs ();
	nummididevicesset = true;
	snd_mididevice.Callback ();
}

void I_ShutdownMusicWin32 ()
{
	// Ancient bug a saw on NT 4.0 and an old version of FMOD 3: If waveout
	// is used for sound and a MIDI is also played, then when I quit, the OS
	// tells me a free block was modified after being freed. This is
	// apparently a synchronization issue between two threads, because if I
	// put this Sleep here after stopping the music but before shutting down
	// the entire sound system, the error does not happen. Observed with a
	// Vortex 2 (may Aureal rest in peace) and an Audigy (damn you, Creative!).
	// I no longer have a system with NT4 drivers, so I don't know if this
	// workaround is still needed or not.
	if (OSPlatform == os_WinNT4)
	{
		Sleep(50);
	}
}

#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define USE_WINDOWS_DWORD
#if defined(_WIN32_WINNT) && _WIN32_WINNT < 0x0400
#undef _WIN32_WINNT
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0400
#endif
#ifndef USE_WINDOWS_DWORD
#define USE_WINDOWS_DWORD
#endif
#include <windows.h>
#include <mmsystem.h>
#else
#define FALSE 0
#define TRUE 1
#endif
#ifdef __APPLE__
#include <AudioToolbox/AudioToolbox.h>
#endif // __APPLE__
#include "tempfiles.h"
#include "c_cvars.h"
#include "mus2midi.h"
#include "i_sound.h"
#include "i_music.h"
#include "s_sound.h"
#include <zmusic.h>

void I_InitMusicWin32 ();
void I_ShutdownMusicWin32 ();

extern float relative_volume;

EXTERN_CVAR (Float, timidity_mastervolume)


// A device that provides a WinMM-like MIDI streaming interface -------------

#ifndef _WIN32
struct MIDIHDR
{
	BYTE *lpData;
	DWORD dwBufferLength;
	DWORD dwBytesRecorded;
	MIDIHDR *lpNext;
};

enum
{
	MOD_MIDIPORT = 1,
	MOD_SYNTH,
	MOD_SQSYNTH,
	MOD_FMSYNTH,
	MOD_MAPPER,
	MOD_WAVETABLE,
	MOD_SWSYNTH
};

typedef BYTE *LPSTR;

#define MEVT_TEMPO			((BYTE)1)
#define MEVT_NOP			((BYTE)2)
#define MEVT_LONGMSG		((BYTE)128)

#define MEVT_EVENTTYPE(x)	((BYTE)((x) >> 24))
#define MEVT_EVENTPARM(x)   ((x) & 0xffffff)

#define MOM_DONE			969
#else
// w32api does not define these
#ifndef MOD_WAVETABLE
#define MOD_WAVETABLE   6
#define MOD_SWSYNTH     7
#endif
#endif

class MIDIStreamer;

class MIDIDevice
{
public:
	MIDIDevice();
	virtual ~MIDIDevice();

	virtual int Open(void (*callback)(unsigned int, void *, DWORD, DWORD), void *userdata) = 0;
	virtual void Close() = 0;
	virtual bool IsOpen() const = 0;
	virtual int GetTechnology() const = 0;
	virtual int SetTempo(int tempo) = 0;
	virtual int SetTimeDiv(int timediv) = 0;
	virtual int StreamOut(MIDIHDR *data) = 0;
	virtual int StreamOutSync(MIDIHDR *data) = 0;
	virtual int Resume() = 0;
	virtual void Stop() = 0;
	virtual int PrepareHeader(MIDIHDR *data);
	virtual int UnprepareHeader(MIDIHDR *data);
	virtual bool FakeVolume();
	virtual bool Pause(bool paused) = 0;
	virtual bool NeedThreadedCallback();
	virtual void PrecacheInstruments(const WORD *instruments, int count);
	virtual void TimidityVolumeChanged();
	virtual void FluidSettingInt(const char *setting, int value);
	virtual void FluidSettingNum(const char *setting, double value);
	virtual void FluidSettingStr(const char *setting, const char *value);
	virtual bool Preprocess(MIDIStreamer *song, bool looping);
	virtual FString GetStats();
};

// WinMM implementation of a MIDI output device -----------------------------

#ifdef _WIN32
class WinMIDIDevice : public MIDIDevice
{
public:
	WinMIDIDevice(int dev_id);
	~WinMIDIDevice();
	int Open(void (*callback)(unsigned int, void *, DWORD, DWORD), void *userdata);
	void Close();
	bool IsOpen() const;
	int GetTechnology() const;
	int SetTempo(int tempo);
	int SetTimeDiv(int timediv);
	int StreamOut(MIDIHDR *data);
	int StreamOutSync(MIDIHDR *data);
	int Resume();
	void Stop();
	int PrepareHeader(MIDIHDR *data);
	int UnprepareHeader(MIDIHDR *data);
	bool FakeVolume();
	bool NeedThreadedCallback();
	bool Pause(bool paused);
	void PrecacheInstruments(const WORD *instruments, int count);

protected:
	static void CALLBACK CallbackFunc(HMIDIOUT, UINT, DWORD_PTR, DWORD, DWORD);

	HMIDISTRM MidiOut;
	UINT DeviceID;
	DWORD SavedVolume;
	bool VolumeWorks;

	void (*Callback)(unsigned int, void *, DWORD, DWORD);
	void *CallbackData;
};
#endif

// AudioToolbox implementation of a MIDI output device ----------------------

#ifdef __APPLE__

class AudioToolboxMIDIDevice : public MIDIDevice
{
public:
	virtual int Open(void (*callback)(unsigned int, void *, DWORD, DWORD), void *userData) override;
	virtual void Close() override;
	virtual bool IsOpen() const override;
	virtual int GetTechnology() const override;
	virtual int SetTempo(int tempo) override;
	virtual int SetTimeDiv(int timediv) override;
	virtual int StreamOut(MIDIHDR *data) override;
	virtual int StreamOutSync(MIDIHDR *data) override;
	virtual int Resume() override;
	virtual void Stop() override;
	virtual int PrepareHeader(MIDIHDR* data) override;
	virtual bool FakeVolume() override { return true; }
	virtual bool Pause(bool paused) override;
	virtual bool Preprocess(MIDIStreamer *song, bool looping) override;

private:
	MusicPlayer m_player = nullptr;
	MusicSequence m_sequence = nullptr;
	AudioUnit m_audioUnit = nullptr;
	CFRunLoopTimerRef m_timer = nullptr;
	MusicTimeStamp m_length = 0;

	typedef void (*Callback)(unsigned int, void *, DWORD, DWORD);
	Callback m_callback = nullptr;
	void* m_userData = nullptr;

	static void TimerCallback(CFRunLoopTimerRef timer, void* info);
};

#endif // __APPLE__

// Base class for pseudo-MIDI devices ---------------------------------------

class PseudoMIDIDevice : public MIDIDevice
{
public:
	PseudoMIDIDevice();
	~PseudoMIDIDevice();

	void Close();
	bool IsOpen() const;
	int GetTechnology() const;
	bool Pause(bool paused);
	int Resume();
	void Stop();
	int StreamOut(MIDIHDR *data);
	int StreamOutSync(MIDIHDR *data);
	int SetTempo(int tempo);
	int SetTimeDiv(int timediv);
	FString GetStats();

protected:
	SoundStream *Stream;
	bool Started;
	bool bLooping;
};

// Sound System pseudo-MIDI device ------------------------------------------

class SndSysMIDIDevice : public PseudoMIDIDevice
{
public:
	int Open(void (*callback)(unsigned int, void *, DWORD, DWORD), void *userdata);
	bool Preprocess(MIDIStreamer *song, bool looping);
};


// Base class for streaming MUS and MIDI files ------------------------------

// HMI file played with a MIDI stream ---------------------------------------

struct AutoNoteOff
{
	DWORD Delay;
	BYTE Channel, Key;
};
// Sorry, std::priority_queue, but I want to be able to modify the contents of the heap.
class NoteOffQueue : public TArray<AutoNoteOff>
{
public:
	void AddNoteOff(DWORD delay, BYTE channel, BYTE key);
	void AdvanceTime(DWORD time);
	bool Pop(AutoNoteOff &item);

protected:
	void Heapify();

	unsigned int Parent(unsigned int i) { return (i + 1u) / 2u - 1u; }
	unsigned int Left(unsigned int i) { return (i + 1u) * 2u - 1u; }
	unsigned int Right(unsigned int i) { return (i + 1u) * 2u; }
};

#if defined(_WIN32) || !defined(NO_SOUND)
#endif

// --------------------------------------------------------------------------

EXTERN_CVAR (Float, snd_musicvolume)

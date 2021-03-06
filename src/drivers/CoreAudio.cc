/*
 *  CoreAudio.cc
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

#include "CoreAudio.h"

#if (__APPLE__)

#pragma mark - CoreAudio

#include <vector>
#include <CoreAudio/CoreAudio.h>

#include "../VoiceAllocationUnit.h"

OSStatus audioDeviceIOProc (   AudioDeviceID           inDevice,
							   const AudioTimeStamp*   inNow,
							   const AudioBufferList*  inInputData,
							   const AudioTimeStamp*   inInputTime,
							   AudioBufferList*        outOutputData,
							   const AudioTimeStamp*   inOutputTime,
							   void*                   inClientData)
{
	float* outL;
	float* outR;
	unsigned numSampleFrames = 0;
	unsigned stride = 1;
	
	if (1 < outOutputData->mBuffers[0].mNumberChannels)
	{
		numSampleFrames = outOutputData->mBuffers[0].mDataByteSize / (outOutputData->mBuffers[0].mNumberChannels * sizeof(float));
		stride = outOutputData->mBuffers[0].mNumberChannels;
		outL = (float*)outOutputData->mBuffers[0].mData;
		outR = outL + 1;
	}
	else if (1 < outOutputData->mNumberBuffers)
	{
		numSampleFrames = outOutputData->mBuffers[0].mDataByteSize / (outOutputData->mBuffers[0].mNumberChannels * sizeof(float));
		outL = (float*)outOutputData->mBuffers[0].mData;
		outR = (float*)outOutputData->mBuffers[1].mData;
	}
	else
	{
		return kAudioDeviceUnsupportedFormatError;
	}
	
	if (inClientData != NULL) {
		(*(AudioCallback)inClientData)(outL, outR, numSampleFrames, stride);
	}
	
	return noErr;
}


class CoreAudioOutput : public GenericOutput
{
public:
	CoreAudioOutput() : m_DeviceID(0)
	{
		UInt32 size; Boolean writable; OSStatus err;
		
		err = AudioHardwareGetPropertyInfo (kAudioHardwarePropertyDevices, &size, &writable);
		if (kAudioHardwareNoError != err) return;
		
		int m_DeviceListSize = size / sizeof (AudioDeviceID);
		m_DeviceList = new AudioDeviceID[m_DeviceListSize];
		
		err = AudioHardwareGetProperty (kAudioHardwarePropertyDevices, &size, m_DeviceList);
		if (kAudioHardwareNoError != err)
		{
			delete[] m_DeviceList;
			m_DeviceList = 0;
			m_DeviceListSize = 0;
		}
	}
	
	~CoreAudioOutput()
	{
		Stop();
		delete[] m_DeviceList;
		m_DeviceListSize = 0;
	}
	
	virtual int init(Config & config)
	{
		if (m_DeviceList && 0 < m_DeviceListSize)
		{
			config.current_audio_driver = "CoreAudio";
			return 0;
		}
		return -1;
	}
	
	virtual bool Start()
	{
		if (!m_DeviceID)
		{
			OSStatus status = noErr;
			AudioDeviceID defaultDeviceID = 0;
			UInt32 propertySize = sizeof(defaultDeviceID);
			status = AudioHardwareGetProperty(kAudioHardwarePropertyDefaultOutputDevice, &propertySize, &defaultDeviceID);
			if (status != kAudioHardwareNoError) return false;
			m_DeviceID = defaultDeviceID;

			AudioDeviceAddIOProc(m_DeviceID, audioDeviceIOProc, (void *)mAudioCallback);
			AudioDeviceStart(m_DeviceID, audioDeviceIOProc);
		}
		return 0;
	}
	
	virtual void Stop()
	{
		if (m_DeviceID) {
			AudioDeviceStop(m_DeviceID, audioDeviceIOProc);
			m_DeviceID = 0;
		}
	}
	
private: ;
	AudioDeviceID* m_DeviceList;
	unsigned long  m_DeviceListSize;
	AudioDeviceID  m_DeviceID;
};

GenericOutput* CreateCoreAudioOutput() { return new CoreAudioOutput; }


#pragma mark - CoreMIDI

#include <CoreMIDI/MIDIServices.h>

class CoreMidiInterface : public MidiInterface
{
public:
	
	CoreMidiInterface();
	~CoreMidiInterface();
	
	virtual int open(Config&);
	virtual void close();
	
protected:

	virtual void ThreadAction() {};
	
	static void midiNotifyProc (const MIDINotification*, void*);
	static void midiReadProc (const MIDIPacketList*, void*, void*);
	
	void HandleMidiPacketList(const MIDIPacketList*);
	
private:
	
	MIDIClientRef   m_clientRef;
	MIDIPortRef     m_inPort;
	MIDIEndpointRef m_currInput;
};

MidiInterface* CreateCoreMidiInterface() { return new CoreMidiInterface; }


CoreMidiInterface::CoreMidiInterface()
{
	MIDIClientCreate(CFSTR("amSynth"), midiNotifyProc, this, &m_clientRef);
	MIDIInputPortCreate(m_clientRef, CFSTR("Input port"), midiReadProc, this, &m_inPort);
}

CoreMidiInterface::~CoreMidiInterface()
{
	MIDIClientDispose(m_clientRef);
}

int
CoreMidiInterface::open(Config & config)
{
	m_currInput = MIDIGetSource(0);
	if (m_currInput)
	{
		MIDIPortConnectSource(m_inPort, m_currInput, NULL);
		
		CFStringRef name = NULL;
		MIDIObjectGetStringProperty(m_currInput, kMIDIPropertyName, &name);
		if (name)
		{
			char cstring[64] = "";
			CFStringGetCString(name, cstring, sizeof(cstring), kCFStringEncodingUTF8);
			config.current_midi_driver = std::string(cstring);
		}
	}
	return (m_currInput) ? 0 : -1;
}

void
CoreMidiInterface::close()
{
	if (m_currInput) 
	{
		MIDIPortDisconnectSource(m_inPort, m_currInput);
		m_currInput = NULL;
	}
}

void
CoreMidiInterface::HandleMidiPacketList(const MIDIPacketList* pktlist)
{
	if (!_handler) return;
	
	for (int i=0; i<pktlist->numPackets; i++)
	{
		unsigned char* data = (unsigned char *)(pktlist->packet+i)->data;
		const UInt16 length = (pktlist->packet+i)->length;
		_handler->HandleMidiData(data, length);
	}
}

void
CoreMidiInterface::midiNotifyProc (const MIDINotification *message, void *refCon)
{
}

void
CoreMidiInterface::midiReadProc (const MIDIPacketList *pktlist, void *readProcRefCon, void *srcConnRefCon)
{
	CoreMidiInterface* self = (CoreMidiInterface *)readProcRefCon;
	self->HandleMidiPacketList(pktlist);
}

#pragma mark - CoreAudio



#else
GenericOutput* CreateCoreAudioOutput() { return NULL; }
MidiInterface* CreateCoreMidiInterface() { return NULL; }
#endif

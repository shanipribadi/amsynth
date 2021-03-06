/*
 *  dssi.cpp
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

#include "controls.h"
#include "midi.h"
#include "MidiController.h"
#include "PresetController.h"
#include "VoiceAllocationUnit.h"

#include <assert.h>
#include <dssi.h>
#include <ladspa.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#ifdef DEBUG
#define TRACE( msg ) fprintf (stderr, "[amsynth-dssi] %s(): " msg "\n", __func__)
#define TRACE_ARGS( fmt, ... ) fprintf (stderr, "[amsynth-dssi] %s(): " fmt "\n", __func__, __VA_ARGS__)
#else
#define TRACE( msg ) (void)0
#define TRACE_ARGS( ... ) (void)0
#endif

static LADSPA_Descriptor *	s_ladspaDescriptor = NULL;
static DSSI_Descriptor *	s_dssiDescriptor   = NULL;


typedef struct _amsynth_wrapper {
	VoiceAllocationUnit * vau;
	PresetController *    bank;
	MidiController *      mc;
	LADSPA_Data *         out_l;
	LADSPA_Data *         out_r;
	LADSPA_Data **        params;
} amsynth_wrapper;




/*	DLL Entry point
 *
 *- Each shared object file containing DSSI plugins must include a
 *   function dssi_descriptor(), with the following function prototype
 *   and C-style linkage.  Hosts may enumerate the plugin types
 *   available in the shared object file by repeatedly calling
 *   this function with successive Index values (beginning from 0),
 *   until a return value of NULL indicates no more plugin types are
 *   available.  Each non-NULL return is the DSSI_Descriptor
 *   of a distinct plugin type.
 */
const DSSI_Descriptor *dssi_descriptor (unsigned long index)
{
    switch (index)
    {
    case 0: return s_dssiDescriptor;
    default: return NULL;
    }
}




//////////////////// Constructor & Destructor (equivalents) ////////////////////

static LADSPA_Handle instantiate (const LADSPA_Descriptor * descriptor, unsigned long s_rate)
{
	TRACE();
    static Config config;
    config.Defaults();
    config.load();
    Preset amsynth_preset;
    amsynth_wrapper * a = new amsynth_wrapper;
    a->vau = new VoiceAllocationUnit;
    a->vau->SetSampleRate (s_rate);
    a->vau->setPitchBendRangeSemitones (config.pitch_bend_range);
    a->bank = new PresetController;
    a->bank->loadPresets(config.current_bank_file.c_str());
    a->bank->selectPreset(0);
    a->bank->getCurrentPreset().AddListenerToAll (a->vau);
    a->mc = new MidiController(config);
    a->mc->SetMidiEventHandler(a->vau);
    a->mc->setPresetController(*a->bank);
	a->mc->set_midi_channel(0);
    a->params = (LADSPA_Data **) calloc (kAmsynthParameterCount, sizeof (LADSPA_Data *));
    return (LADSPA_Handle) a;
}

static void cleanup (LADSPA_Handle instance)
{
	TRACE();
    amsynth_wrapper * a = (amsynth_wrapper *) instance;
    delete a->vau;
    delete a->bank;
    delete a->mc;
    free (a->params);
    delete a;
}

//////////////////// Program handling //////////////////////////////////////////

static const DSSI_Program_Descriptor *get_program(LADSPA_Handle Instance, unsigned long Index)
{
	amsynth_wrapper * a = (amsynth_wrapper *) Instance;

	static DSSI_Program_Descriptor descriptor;
	memset(&descriptor, 0, sizeof(descriptor));

	if (Index < PresetController::kNumPresets) {
		Preset &preset = a->bank->getPreset(Index);
		descriptor.Bank = 0;
		descriptor.Program = Index;
		descriptor.Name = preset.getName().c_str();
		TRACE_ARGS("%d %d %s", descriptor.Bank, descriptor.Program, descriptor.Name);
		return &descriptor;
	}
	
	return NULL;
}

static void select_program(LADSPA_Handle Instance, unsigned long Bank, unsigned long Index)
{
	amsynth_wrapper * a = (amsynth_wrapper *) Instance;

	TRACE_ARGS("Bank = %d Index = %d", Bank, Index);

	if (Bank == 0 && Index < PresetController::kNumPresets) {
		a->bank->selectPreset(Index);
		// now update DSSI host's view of the parameters
		for (unsigned int i=0; i<kAmsynthParameterCount; i++) {
			float value = a->bank->getCurrentPreset().getParameter(i).getValue();
			if (*(a->params[i]) != value) {
				*(a->params[i]) = value;
			}
		}
	}
}

//////////////////// LADSPA port setup /////////////////////////////////////////

static void connect_port (LADSPA_Handle instance, unsigned long port, LADSPA_Data * data)
{
    amsynth_wrapper * a = (amsynth_wrapper *) instance;
    switch (port)
    {
    case 0: a->out_l = data; break;
    case 1: a->out_r = data; break;
    default:
		if ((port - 2) < kAmsynthParameterCount) { a->params[port-2] = data; }
		break;
    }
}

//////////////////// Audio callback ////////////////////////////////////////////

const float kMidiScaler = (1. / 127.);

static void run_synth (LADSPA_Handle instance, unsigned long sample_count, snd_seq_event_t *events, unsigned long event_count)
{
    amsynth_wrapper * a = (amsynth_wrapper *) instance;

    Preset &preset = a->bank->getCurrentPreset();

	for (snd_seq_event_t *e = events; e < events + event_count; e++) {
		unsigned char data[3] = {0};
		switch (e->type) {
		case SND_SEQ_EVENT_NOTEON:
			data[0] = MIDI_STATUS_NOTE_ON;
			data[1] = e->data.note.note;
			data[2] = e->data.note.velocity;
			a->mc->HandleMidiData(data, 3);
			break;
		case SND_SEQ_EVENT_NOTEOFF:
			data[0] = MIDI_STATUS_NOTE_OFF;
			data[1] = e->data.note.note;
			data[2] = e->data.note.off_velocity;
			a->mc->HandleMidiData(data, 3);
			break;
		case SND_SEQ_EVENT_KEYPRESS:
			data[0] = MIDI_STATUS_NOTE_PRESSURE;
			data[1] = e->data.note.note;
			data[2] = e->data.note.velocity;
			a->mc->HandleMidiData(data, 3);
			break;
		case SND_SEQ_EVENT_CONTROLLER:
			data[0] = MIDI_STATUS_CONTROLLER;
			data[1] = e->data.control.param;
			data[2] = e->data.control.value;
			a->mc->HandleMidiData(data, 3);
			for (unsigned int i=0; i<kAmsynthParameterCount; i++) {
				float value = preset.getParameter(i).getValue();
				if (*(a->params[i]) != value) {
					*(a->params[i]) = value;
				}
			}
			break;
		case SND_SEQ_EVENT_PGMCHANGE:
			data[0] = MIDI_STATUS_PROGRAM_CHANGE;
			data[1] = e->data.control.value;
			a->mc->HandleMidiData(data, 2);
			for (unsigned int i=0; i<kAmsynthParameterCount; i++) {
				float value = preset.getParameter(i).getValue();
				if (*(a->params[i]) != value) {
					*(a->params[i]) = value;
				}
			}
			break;
		case SND_SEQ_EVENT_CHANPRESS:
			break;
		case SND_SEQ_EVENT_PITCHBEND:
			data[0] = MIDI_STATUS_PITCH_WHEEL;
			data[1] = (((unsigned int)(e->data.control.value + 0x2000)) >> 0) & 0x7F;
			data[2] = (((unsigned int)(e->data.control.value + 0x2000)) >> 7) & 0x7F;
			a->mc->HandleMidiData(data, 3);
			break;
		default:
			break;
		}
	}

    // push through changes to parameters
    for (unsigned i=0; i<kAmsynthParameterCount; i++)
    {
		const LADSPA_Data host_value = *(a->params[i]);
	    if (preset.getParameter(i).getValue() != host_value)
	    {
			TRACE_ARGS("parameter %32s = %f", preset.getParameter(i).getName().c_str(), host_value);
		    preset.getParameter(i).setValue(host_value);
	    }
    }

    a->vau->Process ((float *) a->out_l, (float *) a->out_r, sample_count);
}

// renoise ignores DSSI plugins that don't implement run

static void run (LADSPA_Handle instance, unsigned long sample_count)
{
    run_synth (instance, sample_count, NULL, 0);
}

////////////////////////////////////////////////////////////////////////////////

/*
 * Magic routines called by the system upon opening and closing libraries..
 * http://www.tldp.org/HOWTO/Program-Library-HOWTO/miscellaneous.html#INIT-AND-CLEANUP
 */

void __attribute__ ((constructor)) my_init ()
{
    const char **port_names;
    LADSPA_PortDescriptor *port_descriptors;
    LADSPA_PortRangeHint *port_range_hints;

	/* LADSPA descriptor */
    s_ladspaDescriptor = (LADSPA_Descriptor *) calloc (1, sizeof (LADSPA_Descriptor));
    if (s_ladspaDescriptor)
	{
		s_ladspaDescriptor->UniqueID = 23;
		s_ladspaDescriptor->Label = "amsynth";
		s_ladspaDescriptor->Properties = LADSPA_PROPERTY_REALTIME | LADSPA_PROPERTY_HARD_RT_CAPABLE;
		s_ladspaDescriptor->Name = "amsynth DSSI plugin";
		s_ladspaDescriptor->Maker = "Nick Dowell <nick@nickdowell.com>";
		s_ladspaDescriptor->Copyright = "(c) 2005";

		//
		// set up ladspa 'Ports' - used to perform audio and parameter communication...
		//
		
		port_descriptors = (LADSPA_PortDescriptor *) calloc (kAmsynthParameterCount+2, sizeof (LADSPA_PortDescriptor));
		port_range_hints = (LADSPA_PortRangeHint *) calloc (kAmsynthParameterCount+2, sizeof (LADSPA_PortRangeHint));
		port_names = (const char **) calloc (kAmsynthParameterCount+2, sizeof (char *));
		
		// we need ports to transmit the audio data...
		port_descriptors[0] = LADSPA_PORT_OUTPUT | LADSPA_PORT_AUDIO;
		port_range_hints[0].HintDescriptor = 0;
		port_names[0] = "OutL";

		port_descriptors[1] = LADSPA_PORT_OUTPUT | LADSPA_PORT_AUDIO;
		port_range_hints[1].HintDescriptor = 0;
		port_names[1] = "OutR";

		Preset amsynth_preset;
		for (unsigned i=0; i<kAmsynthParameterCount; i++)
		{
			const Parameter &parameter = amsynth_preset.getParameter(i);
			const int numSteps = parameter.getSteps();
			port_descriptors[i+2] = LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL;
			port_range_hints[i+2].LowerBound = parameter.getMin();
			port_range_hints[i+2].UpperBound = parameter.getMax();
			port_range_hints[i+2].HintDescriptor = LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE
				| ((numSteps == 2) ? LADSPA_HINT_TOGGLED : 0)
				| ((numSteps  > 2) ? LADSPA_HINT_INTEGER : 0);
				
			// Try to map onto LADSPA's insane take on 'default' values
			const float def = parameter.getValue();
			const float min = parameter.getMin();
			const float max = parameter.getMax();
			const float med = (parameter.getMin() + parameter.getMax()) / 2.0;
			const float rng = parameter.getMax() - parameter.getMin();
			//
			if (def == 0)			port_range_hints[i+2].HintDescriptor |= LADSPA_HINT_DEFAULT_0;
			else if (def == 1)		port_range_hints[i+2].HintDescriptor |= LADSPA_HINT_DEFAULT_1;
			else if (def == 100)	port_range_hints[i+2].HintDescriptor |= LADSPA_HINT_DEFAULT_100;
			else if (def == 440)	port_range_hints[i+2].HintDescriptor |= LADSPA_HINT_DEFAULT_440;
			else if (def == min)	port_range_hints[i+2].HintDescriptor |= LADSPA_HINT_DEFAULT_MINIMUM;
			else if (def == max)	port_range_hints[i+2].HintDescriptor |= LADSPA_HINT_DEFAULT_MAXIMUM;
			else if (def  < med)	port_range_hints[i+2].HintDescriptor |= LADSPA_HINT_DEFAULT_LOW;
			else if (def == med)	port_range_hints[i+2].HintDescriptor |= LADSPA_HINT_DEFAULT_MIDDLE;
			else if (def  > med)	port_range_hints[i+2].HintDescriptor |= LADSPA_HINT_DEFAULT_HIGH;
			
			port_names[i+2] = parameter_name_from_index(i); // returns a pointer to a static string
			TRACE_ARGS("port hints for %32s : %x", parameter_name_from_index(i), port_range_hints[i+2]);
		}
	
		s_ladspaDescriptor->PortDescriptors = port_descriptors;
		s_ladspaDescriptor->PortRangeHints  = port_range_hints;
		s_ladspaDescriptor->PortNames       = port_names;
		s_ladspaDescriptor->PortCount       = kAmsynthParameterCount+2;
	
	
		s_ladspaDescriptor->instantiate = instantiate;
		s_ladspaDescriptor->cleanup = cleanup;

		s_ladspaDescriptor->activate = NULL;
		s_ladspaDescriptor->deactivate = NULL;

		s_ladspaDescriptor->connect_port = connect_port;
		s_ladspaDescriptor->run = run;
		s_ladspaDescriptor->run_adding = NULL;
		s_ladspaDescriptor->set_run_adding_gain = NULL;
    }

	/* DSSI descriptor */
    s_dssiDescriptor = (DSSI_Descriptor *) malloc (sizeof (DSSI_Descriptor));
    if (s_dssiDescriptor)
	{
		s_dssiDescriptor->DSSI_API_Version				= 1;
		s_dssiDescriptor->LADSPA_Plugin				= s_ladspaDescriptor;
		s_dssiDescriptor->configure					= NULL;
		s_dssiDescriptor->get_program 					= get_program;
		s_dssiDescriptor->get_midi_controller_for_port	= NULL;
		s_dssiDescriptor->select_program 				= select_program;
		s_dssiDescriptor->run_synth 					= run_synth;
		s_dssiDescriptor->run_synth_adding 			= NULL;
		s_dssiDescriptor->run_multiple_synths 			= NULL;
		s_dssiDescriptor->run_multiple_synths_adding	= NULL;
    }
}

void __attribute__ ((destructor)) my_fini ()
{
    if (s_ladspaDescriptor)
	{
		free ((LADSPA_PortDescriptor *) s_ladspaDescriptor->PortDescriptors);
		free ((char **) s_ladspaDescriptor->PortNames);
		free ((LADSPA_PortRangeHint *) s_ladspaDescriptor->PortRangeHints);
		free (s_ladspaDescriptor);
    }
    if (s_dssiDescriptor)
	{
		free (s_dssiDescriptor);
    }
}

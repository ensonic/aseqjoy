/**
 * aseqjoy - Tiny Jostick -> MIDI Controller Tool
 * Copyright 2003-2016 by Alexander Koenig - alex@lisas.de
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
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 * Note: that these sources contain a few lines of Vojtech Pavlik's jstest.c 
 * example, which is GPL'd, too and available from:
 * http://atrey.karlin.mff.cuni.cz/~vojtech/joystick/
 */

#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <limits.h>

#include <linux/joystick.h>
#include <alsa/asoundlib.h>

#define NAME_LENGTH 128

#define TOOL_NAME "aseqjoy"

#define MAX_JS_AXIS 10

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

int joystick_no=0;

typedef struct _ctrl {
	int last_val_i;
	// input values from joystick
	long i_min,i_max;
	double i_rng;
	// output values to midi
	long o_min;
	double o_rng;
	snd_seq_event_t ev;
	// input values (only track in verbose mode)
	int v_min, v_max;
} ctrl_t;
ctrl_t ctrls[MAX_JS_AXIS];

snd_seq_t *seq_handle;
int verbose=0;

static int open_alsa_seq()
{
	char client_name[32];
	char port_name[48];
	snd_seq_addr_t src;
	int i;
	
	/* Create the sequencer port. */
	
	sprintf(client_name, "Joystick%i", joystick_no);
	sprintf(port_name , "%s Output", client_name);

	if (snd_seq_open(&seq_handle, "default", SND_SEQ_OPEN_OUTPUT, 0) < 0) {
		puts("Error: Failed to access the ALSA sequencer.");
		exit(-1);
	}

	snd_seq_set_client_name(seq_handle, client_name);
	src.client = snd_seq_client_id(seq_handle);
	src.port = snd_seq_create_simple_port(seq_handle, "Joystick Output",
		SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ, SND_SEQ_PORT_TYPE_APPLICATION);

	/* Init the event structure */
	for (i=0; i<MAX_JS_AXIS; i++) {
	    snd_seq_ev_set_source(&ctrls[i].ev, src.port);
    }
	return 0;
}

static int open_joystick()
{
    int joy_fd;
    int axes, buttons;
	char device[256];
	char name[NAME_LENGTH] = "Unknown";	
	
	sprintf(device, "/dev/js%i", joystick_no);

	if ((joy_fd = open(device, O_RDONLY)) < 0) {
		fprintf(stderr, "%s: ", TOOL_NAME); perror(device);
		sprintf(device, "/dev/input/js%i", joystick_no);
		
		if ((joy_fd = open(device, O_RDONLY)) < 0) {	
			fprintf(stderr, "%s: ", TOOL_NAME); perror(device);
			exit(-3);
		}
	}

	ioctl(joy_fd, JSIOCGAXES, &axes);
	ioctl(joy_fd, JSIOCGBUTTONS, &buttons);
	ioctl(joy_fd, JSIOCGNAME(NAME_LENGTH), name);

	printf("Using joystick (%s) through device %s with %i axes and %i buttons.\n", name, device, axes, buttons);

	return joy_fd;
}

static void loop(int joy_fd)
{
	struct js_event js;
	snd_seq_event_t *ev;
	int current_channel=1;
		
	puts("Ready, entering loop - use Ctrl-C to exit.");	

	while (1) {
		if (read(joy_fd, &js, sizeof(struct js_event)) != sizeof(struct js_event)) {
			perror(TOOL_NAME ": error reading from joystick device");
			exit (-5);
		}

		switch(js.type & ~JS_EVENT_INIT) {		
			case JS_EVENT_BUTTON:
				if (js.value) {			
					current_channel=js.number+1;
				
					if (verbose) {
						printf("Switched to MIDI channel %i.\n", current_channel);
					}
				}
			break;
			
			case JS_EVENT_AXIS:
			    if (js.number < MAX_JS_AXIS) {		
			        ctrl_t *ctrl=&ctrls[js.number];
                    double val_d;
                    int val_i;

				    // js.value signed 16 bit int
				    val_d=((double) js.value - ctrl->i_min) / ctrl->i_rng;
				    val_d=(ctrl->o_rng * val_d) + ctrl->o_min;
				    val_i=(int) val_d;
			    
				    if (ctrl->last_val_i!=val_i) {
                        ev=&ctrl->ev;
					    ev->data.control.channel=current_channel;
					    ev->data.control.value=val_i;
					    
					    // snd_seq_ev_set_controller(&ev, current_channel, ctrls[js.number].controller, val_i);
					    snd_seq_event_output_direct(seq_handle, ev);
					    
					    if (verbose) {
					        if (js.value < ctrl->v_min) {
					          ctrl->v_min = js.value;
					        } else if (js.value > ctrl->v_max) {
					          ctrl->v_max = js.value;
					        }
					        // TODO: replace 'controller %i' with actual name
						    printf("Sent controller %i with value: %5i (range: %5i..%5i).\n", 
						      ev->data.control.param, val_i, ctrl->v_min , ctrl->v_max);
					    }
				    }
				}
			break;
		}
	}
}

static int parse_axis_spec(int axis, char* optarg)
{
    ctrl_t *ctrl=&ctrls[axis];
    snd_seq_event_t *ev = &ctrl->ev;
    char *delim = strchr(optarg, ':');
    
    if (!delim) {
        // legacy format: <cc-number>
        ev->data.control.param=atoi(optarg);
    } else {
        /* new format: <min>:<max>:<ev>
         * - min:max is used to calibrate the joy-stick and to flip direction
         *   defaults are: -32768:32767
         * - ev: can be a letter or a cc number
         */
         char *mi=optarg, *ma, *evt;
         *delim='\0';delim++;
         ma=delim;
         delim = strchr(delim, ':');
         if (!delim) {
             fprintf(stderr, "Missing 2nd delimiter. Needs to be <cc> or <min>:<max>:<ev>\n");
             exit(-1);
         }
         *delim='\0';delim++;
         evt=delim;
         if (!*mi || !*ma || !*evt) {
             fprintf(stderr, "One of the fields is empty. Needs to be <min>:<max>:<ev>\n");
             exit(-1);
         }
       
         ctrl->i_min=atoi(mi);
         ctrl->i_max=atoi(ma);
         switch(evt[0]) {
           case 'p':
               ev->type = SND_SEQ_EVENT_PITCHBEND;
               ev->data.control.param=0;
               ctrl->o_min = -8192;
               ctrl->o_rng = 16383.0;
               break;
           default:
               ev->data.control.param=atoi(evt);
               break;
         }
    } 
}

int main (int argc, char **argv)
{
	snd_seq_event_t *ev;
	int i;
    int cc14=0;
	int joy_fd;
	
    fprintf(stderr, 
      "%s version %s - Copyright (C) 2003-2016 by Alexander Koenig\n"
      "                Copyright          2021 by Stefan Sauer\n", TOOL_NAME, VERSION);
    fprintf(stderr, "%s comes with ABSOLUTELY NO WARRANTY - for details read the license.\n", TOOL_NAME);

	for (i=0; i<MAX_JS_AXIS; i++) {
		ctrls[i].last_val_i = INT_MAX;
		ctrls[i].i_min = SHRT_MIN;
		ctrls[i].i_max = SHRT_MAX;
	    ctrls[i].o_min = 0;
	    ctrls[i].o_rng = 127.0;
	    ctrls[i].v_min = SHRT_MAX;
	    ctrls[i].v_max = SHRT_MIN + 1;
		ev = &ctrls[i].ev;
		snd_seq_ev_clear(ev);
    	snd_seq_ev_set_subs(ev);
    	snd_seq_ev_set_direct(ev);
    	snd_seq_ev_set_fixed(ev);
    	ev->type = SND_SEQ_EVENT_CONTROLLER;
    	ev->data.control.param=10+i;
	}
	
	while (1) {
		int i=getopt(argc, argv, "vhrd:0:1:2:3:4:5:6:7:");
		if (i==-1) break;
		
		switch (i) {
			case '?':
			case 'h':
				printf("usage: %s [-d joystick_no] [-v] [-0 ctrl0] [-1 ctrl1] ... [-9 ctrl9]\n\n", TOOL_NAME);
				puts("\t-d select the joystick to use: 0..3");
				puts("\t-0 select the controller for axis 0 (1-127)");
				puts("\t-1 select the controller for axis 1 (1-127) etc");
				puts("\t-r use fine control change events (14 bit resolution)");
				puts("\t-v verbose mode.");
				exit(-2);
			break;
			
			case '0':
			case '1':
			case '2':
			case '3':
			case '4':
			case '5':
			case '6':
			case '7':
			case '8':
			case '9':
			    parse_axis_spec(i - '0', optarg);
			break;
			
			case 'v':
				verbose=1;
			break;

			case 'r':
				cc14=1;
			break;
			
			case 'd':
				joystick_no=atoi(optarg);
			break;
		}
	}

	joy_fd = open_joystick();
	open_alsa_seq();
	
	for (i=0; i<MAX_JS_AXIS; i++) {
    	ev = &ctrls[i].ev;
	    if (cc14 && (ev->type == SND_SEQ_EVENT_CONTROLLER)) {
		    ev->type = SND_SEQ_EVENT_CONTROL14;
		    ctrls[i].o_rng = 16383.0;
	    }
	    ctrls[i].i_rng = (double) (ctrls[i].i_max - ctrls[i].i_min);
	}
	
	loop(joy_fd);

	return 0;
}

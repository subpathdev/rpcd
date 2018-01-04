#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>

#include "x11.h"
#include "control.h"
#include "command.h"

static int init_done = 0;
static size_t ndisplays = 0;
static display_t* displays = NULL;

size_t x11_count(){
	return ndisplays;
}

display_t* x11_get(size_t index){
	if(index < ndisplays){
		return displays + index;
	}
	return NULL;
}

size_t x11_find_id(char* name){
	size_t u;

	for(u = 0; u < ndisplays; u++){
		if(!strcmp(displays[u].name, name)){
			return u;
		}
	}

	fprintf(stderr, "Failed to find display %s, replacing with default display 0\n", name);
	return 0;
}

//See ratpoison:src/communications.c for the original implementation of the ratpoison
//command protocol
static int x11_fetch_response(display_t* display, Window w, char** response){
	int format, rv = -1;
	unsigned long items, bytes;
	unsigned char* result = NULL;
	Atom type;

	if(XGetWindowProperty(display->display_handle, w, display->rp_command_result,
				0, 0, False, XA_STRING,
				&type, &format, &items, &bytes, &result) != Success
			|| !result){
		fprintf(stderr, "Failed to fetch ratpoison command result status\n");
		goto bail;
	}

	XFree(result);

	if(XGetWindowProperty(display->display_handle, w, display->rp_command_result,
				0, (bytes / 4) + ((bytes % 4) ? 1 : 0), True, XA_STRING,
				&type, &format, &items, &bytes, &result) != Success
			|| !result){
		fprintf(stderr, "Failed to fetch ratpoison command result\n");
		goto bail;
	}

	if(*result){
		//command failed, look for a reason
		if(*result == '0'){
			fprintf(stderr, "ratpoison command failed: %s\n", result + 1);
			goto bail;
		}

		//command ok
		if(*result == '1'){
			*response = strdup((char*) (result + 1));
			if(!*response){
				fprintf(stderr, "Failed to allocate memory\n");
			}
		}
	}

	rv = 0;
bail:
	if(result){
		XFree(result);
	}
	return rv;
}

static int x11_run_command(display_t* display, char* command, char** response){
	int rv = 1;
	XEvent ev;

	if(!display){
		fprintf(stderr, "Invalid display passed to x11_run_command\n");
		return 1;
	}

	Window root = DefaultRootWindow(display->display_handle);
	Window w = XCreateSimpleWindow(display->display_handle, root, 0, 0, 1, 1, 0, 0, 0);
	char* command_string = calloc(strlen(command) + 2, sizeof(char));

	if(!command_string){
		fprintf(stderr, "Failed to allocate memory\n");
		goto bail;
	}

	if(!display->rp_command
			|| !display->rp_command_request
			|| !display->rp_command_result){
		fprintf(stderr, "Window manager interaction disabled on %s, would have run: %s\n", display->name, command);
		rv = 0;
		goto bail;
	}

	memcpy(command_string + 1, command, strlen(command));

	XSelectInput(display->display_handle, w, PropertyChangeMask);
	XChangeProperty(display->display_handle, w, display->rp_command, XA_STRING, 8, PropModeReplace, (unsigned char*) command_string, strlen(command) + 2);
	XChangeProperty(display->display_handle, root, display->rp_command_request, XA_WINDOW, 8, PropModeAppend, (unsigned char*) &w, sizeof(Window));

	for(;;){
		XMaskEvent(display->display_handle, PropertyChangeMask, &ev);
		if(ev.xproperty.atom == display->rp_command_result && ev.xproperty.state == PropertyNewValue){
			rv = 0;
			if(response){
				rv = x11_fetch_response(display, w, response);
			}
			break;
		}
	}

bail:
	free(command_string);
	XDestroyWindow(display->display_handle, w);
  	return rv;
}

static int x11_repatriate(size_t display_id){
	int rv = 1;
	size_t frame_id, window;
	char* layout = NULL, *frame = NULL;
	if(x11_fetch_layout(display_id, &layout) || !layout){
		fprintf(stderr, "Failed to repatriate windows, could not read layout\n");
		return 1;
	}

	for(frame = strtok(layout, ","); frame; frame = strtok(NULL, ",")){
		if(!strstr(frame, ":number") || !strstr(frame, ":window")){
			fprintf(stderr, "Skipping frame, missing either ID or window\n");
			continue;
		}

		frame_id = strtoul(strstr(frame, ":number") + 7, NULL, 10);
		window = strtoul(strstr(frame, ":window") + 7, NULL, 10);

		if(control_repatriate(display_id, frame_id, window)){
			fprintf(stderr, "Failed to repatriate frame %zu\n", frame_id);
			goto bail;
		}
	}

	rv = 0;
bail:
	free(layout);
	return rv;
}

static void x11_display_free(display_t* display){
	free(display->name);
	display->name = NULL;

	free(display->identifier);
	display->identifier = NULL;

	free(display->default_layout_name);
	display->default_layout_name = NULL;

	if(display->display_handle){
		XCloseDisplay(display->display_handle);
	}
}

static int x11_display_init(display_t* display, char* name){
	display_t empty = {
		0
	};

	*display = empty;
	display->name = strdup(name);
	return display->name ? 0 : 1;
}

int x11_fetch_layout(size_t display_id, char** layout){
	return x11_run_command(x11_get(display_id), "sfdump", layout);
}

int x11_activate_layout(layout_t* layout){
	size_t left = 0, frame = 0, off = 10;
	display_t* display = x11_get(layout->display_id);
	char* layout_string = strdup("sfrestore ");
	ssize_t required = 0;
	int rv;

	if(!layout_string || !display){
		fprintf(stderr, "Failed to allocate memory\n");
		free(layout_string);
		return 1;
	}

	for(frame = 0; frame < layout->nframes; frame++){
		required = snprintf(layout_string + off, left, "%s(frame :number %zu :x %zu :y %zu :width %zu :height %zu :screenw %zu :screenh %zu :window %zu) %zu",
				frame ? "," : "", layout->frames[frame].id,
				layout->frames[frame].bbox[0], layout->frames[frame].bbox[1],
				layout->frames[frame].bbox[2], layout->frames[frame].bbox[3],
				layout->frames[frame].screen[0], layout->frames[frame].screen[1],
				control_get_window(layout->display_id, layout->frames[frame].id),
				layout->frames[frame].screen[2]);

		if(required < 0){
			fprintf(stderr, "Failed to design layout string for %s on %s\n", layout->name, display->name);
			rv = 1;
			goto bail;
		}

		if(required > left){
			layout_string = realloc(layout_string, (strlen(layout_string) + 1 + DATA_CHUNK) * sizeof(char));
			if(!layout_string){
				fprintf(stderr, "Failed to allocate memory\n");
				rv = 1;
				goto bail;
			}

			left = DATA_CHUNK;
			frame--;
			continue;
		}

		off += required;
		left -= required;
	}

	rv = x11_run_command(display, layout_string, NULL);
	display->current_layout = layout;
	//stop commands from undoing the layout change
	command_discard_restores(layout->display_id);
bail:
	free(layout_string);
	return rv;
}

int x11_fullscreen(size_t display_id){
	return x11_run_command(x11_get(display_id), "only", NULL);
}

int x11_rollback(size_t display_id){
	return x11_run_command(x11_get(display_id), "undo", NULL);
}

int x11_select_frame(size_t display_id, size_t frame_id){
	char command_buffer[DATA_CHUNK];
	snprintf(command_buffer, sizeof(command_buffer), "fselect %zu", frame_id);
	return x11_run_command(x11_get(display_id), command_buffer, NULL);
}

layout_t* x11_current_layout(size_t display_id){
	display_t* display = x11_get(display_id);

	if(!display){
		fprintf(stderr, "Invalid display ID passed to x11_current_layout\n");
		return NULL;
	}

	return display->current_layout ? display->current_layout : display->default_layout;
}

int x11_loop(fd_set* in, fd_set* out, int* max_fd){
	size_t u;

	if(!init_done){
		for(u = 0; u < ndisplays; u++){
			if(displays[u].default_layout_name){
				displays[u].default_layout = layout_find(u, displays[u].default_layout_name);
				if(!displays[u].default_layout){
					fprintf(stderr, "Failed to find default layout %s for %s\n", displays[u].default_layout_name, displays[u].name);
					return 1;
				}
			}
		}
		init_done = 1;
	}
	return 0;
}

int x11_new(char* name){
	size_t u;
	int rv = 0;

	if(!name || strlen(name) < 1){
		fprintf(stderr, "Invalid display name passed\n");
		return 1;
	}

	for(u = 0; u < ndisplays; u++){
		if(!strcmp(displays[u].name, name)){
			fprintf(stderr, "Display name %s already defined\n", name);
			return 1;
		}
	}

	displays = realloc(displays, (ndisplays + 1) * sizeof(display_t));
	if(!displays){
		ndisplays = 0;
		fprintf(stderr, "Failed to allocate memory\n");
		return 1;
	}

	rv = x11_display_init(displays + ndisplays, name);
	ndisplays++;
	return rv;
}

int x11_config(char* option, char* value){
	display_t* last = displays + (ndisplays - 1);
	if(!strcmp(option, "display")){
		if(last->identifier){
			fprintf(stderr, "Multiple display connections specified for display %s\n", last->name);
			return 1;
		}

		last->display_handle = XOpenDisplay(value);
		if(!last->display_handle){
			fprintf(stderr, "Failed to open display %s\n", value);
			return 1;
		}
		last->rp_command = XInternAtom(last->display_handle, "RP_COMMAND", True);
		last->rp_command_request = XInternAtom(last->display_handle, "RP_COMMAND_REQUEST", True);
		last->rp_command_result = XInternAtom(last->display_handle, "RP_COMMAND_RESULT", True);
		last->identifier = strdup(value);
		if(!last->identifier){
			fprintf(stderr, "Failed to allocate memory\n");
			return 1;
		}
		return 0;
	}
	else if(!strcmp(option, "deflayout")){
		last->default_layout_name = strdup(value);
		if(!last->default_layout_name){
			fprintf(stderr, "Failed to allocate memory\n");
			return 1;
		}
		return 0;
	}
	else if(!strcmp(option, "repatriate")){
		if(!strcmp(value, "yes")){
			last->repatriate = 1;
		}
		return 0;
	}

	fprintf(stderr, "Option %s not recognized for type display\n", option);
	return 1;
}

int x11_ok(){
	if(!displays){
		fprintf(stderr, "No display defined\n");
		return 1;
	}

	display_t* last = displays + (ndisplays - 1);

	if(!last->display_handle || !last->identifier){
		fprintf(stderr, "No display connected for %s\n", last->name);
		return 1;
	}

	if(!last->rp_command || !last->rp_command_request || !last->rp_command_result){
		fprintf(stderr, "Failed to query ratpoison-specific Atoms on %s, window manager interaction disabled\n", last->name);
	}

	if(last->repatriate){
		x11_repatriate(ndisplays - 1);
		last->repatriate = 0;
	}
	return 0;
}

void x11_cleanup(){
	size_t u;

	for(u = 0; u < ndisplays; u++){
		x11_display_free(displays + u);
	}
	free(displays);
	ndisplays = 0;
}

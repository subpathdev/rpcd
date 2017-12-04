#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "layout.h"

int layout_ok(layout_t* layout){
	if(!layout->name || !layout->frames){
		fprintf(stderr, "Layout defines no frames or has no name\n");
		return 1;
	}

	if(layout->width == 0 || layout->height == 0){
		fprintf(stderr, "Invalid layout dimensions\n");
		return 1;
	}
	return 0;
}

int layout_parse(char* file, layout_t* layout){
	//TODO read and parse an sfdump file
	return 1;
}

void layout_init(layout_t* layout){
	layout_t empty = {
		.name = NULL,
		.width = 0,
		.height = 0,
		.nframes = 0,
		.frames = NULL
	};
	*layout = empty;
}

void layout_free(layout_t* layout){
	free(layout->name);
	free(layout->frames);
	layout_init(layout);
}
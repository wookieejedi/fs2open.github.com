/*
 * Copyright (C) Volition, Inc. 1999.  All rights reserved.
 *
 * All source code herein is the property of Volition, Inc. You may not sell 
 * or otherwise commercially exploit the source or things you created based on the 
 * source.
 *
*/



#ifndef FREESPACE_MEDAL_HEADER_FILE
#define FREESPACE_MEDAL_HEADER_FILE

#include "globalincs/globals.h"
#include "globalincs/pstypes.h"

class scoring_struct;
class player;

extern int Rank_medal_index;

// NUM_MEDALS stored in scoring.h since needed for player scoring structure

class medal_stuff
{
public:
	char	name[NAME_LENGTH];
	SCP_string alt_name;
	char	bitmap[MAX_FILENAME_LEN];
	char	debrief_bitmap[MAX_FILENAME_LEN];
	int	num_versions;
	bool version_starts_at_1;
	bool available_from_start;
	int	kills_needed;
	int mask_index;

	//If this is a badge (kills_needed > 0)
	char voice_base[MAX_FILENAME_LEN];
	SCP_map<int, SCP_string> promotion_text;

	medal_stuff();

	const char* get_display_name() const;
};

extern SCP_vector<medal_stuff> Medals;

extern void medals_init();

// modes for this screen
#define MM_NORMAL				0		// normal - run through the state code
#define MM_POPUP				1		// called from within some other tight loop (don't use gameseq_ functions)

// main medals screen
void medal_main_init(player *pl,int mode = MM_NORMAL);

// return 0 if the screen should close (used for MM_POPUP mode)
int medal_main_do();
void medal_main_close();

int medals_info_lookup(const char *name);

#endif

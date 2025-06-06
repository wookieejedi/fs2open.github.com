/*
 * Copyright (C) Volition, Inc. 1999.  All rights reserved.
 *
 * All source code herein is the property of Volition, Inc. You may not sell 
 * or otherwise commercially exploit the source or things you created based on the 
 * source.
 *
*/ 




#include "ai/ai.h"
#include "fireball/fireballs.h"
#include "gamesnd/gamesnd.h"
#include "globalincs/alphacolors.h"
#include "globalincs/linklist.h"
#include "hud/hudartillery.h"
#include "hud/hudmessage.h"
#include "io/timer.h"
#include "math/vecmat.h"
#include "network/multi.h"
#include "object/object.h"
#include "parse/parselo.h"
#include "ship/shipfx.h"
#include "sound/sound.h"
#include "utils/Random.h"
#include "weapon/beam.h"
#include "weapon/weapon.h"

// -----------------------------------------------------------------------------------------------------------------------
// ARTILLERY DEFINES/VARS
//
// Goober5000 - moved to hudartillery.h

// -----------------------------------------------------------------------------------------------------------------------
// ARTILLERY FUNCTIONS
//

// test code for subspace missile strike -------------------------------------------

// ssm_info, like ship_info etc.
SCP_vector<ssm_info> Ssm_info;

// list of active strikes
SCP_list<ssm_strike> Ssm_strikes;

// Goober5000
int ssm_info_lookup(const char *name)
{
	if(name == NULL)
		return -1;

	for (auto it = Ssm_info.cbegin(); it != Ssm_info.cend(); ++it)
		if (!stricmp(name, it->name))
			return (int)std::distance(Ssm_info.cbegin(), it);

	return -1;
}

void parse_ssm(const char *filename)
{
	char ssm_name[NAME_LENGTH];
	char weapon_name[NAME_LENGTH];

	try
	{
		read_file_text(filename, CF_TYPE_TABLES);
		reset_parse();

		// parse the table
		while(required_string_either("#end", "$SSM:"))
		{
			bool no_create = false;
			int string_index;
			ssm_info *s, new_ssm;

			s = &new_ssm;

			// name
			required_string("$SSM:");
			stuff_string(ssm_name, F_NAME, NAME_LENGTH);
			if (*ssm_name == 0)
			{
				sprintf(ssm_name, "SSM " SIZE_T_ARG, Ssm_info.size());
				mprintf(("Found an SSM entry without a name.  Assigning \"%s\".\n", ssm_name));
			}

			if (optional_string("+nocreate"))
			{
				no_create = true;

				int i = ssm_info_lookup(ssm_name);
				if (i >= 0)
					s = &Ssm_info[i];
			}
			else
			{
				strcpy_s(s->name, ssm_name);
			}

			// stuff data
			if (optional_string("+Weapon:"))
			{
				stuff_string(weapon_name, F_NAME, NAME_LENGTH);

				// see if we have a valid weapon
				s->weapon_info_index = weapon_info_lookup(weapon_name);
				if (s->weapon_info_index < 0)
					error_display(0, "Unknown weapon [%s] for SSM strike [%s]; this SSM strike will be discarded.", weapon_name, s->name);
			}

			string_index = optional_string_either("+Count:", "+Min Count:");
			if (string_index == 0)
			{
				stuff_int(&s->count);
				s->max_count = -1;
			}
			else if (string_index == 1)
			{
				stuff_int(&s->count);
				required_string("+Max Count:");
				stuff_int(&s->max_count);
			}

			if (optional_string("+WarpEffect:"))
			{
				char unique_id[NAME_LENGTH];
				stuff_string(unique_id, F_NAME, NAME_LENGTH);

				if (can_construe_as_integer(unique_id))
				{
					int temp = atoi(unique_id);

					if (!SCP_vector_inbounds(Fireball_info, temp))
						error_display(0, "Fireball index [%d] out of range (should be 0-%d) for SSM strike [%s]", temp, static_cast<int>(Fireball_info.size()) - 1, s->name);
					else
						s->fireball_type = temp;
				}
				else
				{
					// We have a string to parse instead.
					int fireball_type = fireball_info_lookup(unique_id);
					if (fireball_type < 0)
						error_display(0, "Unknown fireball [%s] to use as warp effect for SSM strike [%s]", unique_id, s->name);
					else
						s->fireball_type = fireball_type;
				}
			}

			if (optional_string("+WarpRadius:"))
			{
				stuff_float(&s->warp_radius);
			}

			if (optional_string("+WarpTime:"))
			{
				stuff_float(&s->warp_time);

				// According to fireballs.cpp, "Warp lifetime must be at least 4 seconds!"
				if (s->warp_time < 4.0f)
				{
					// So let's warn them before they try to use it, shall we?
					error_display(0, "Expected a '+WarpTime:' value equal or greater than 4.0, found '%f' in SSM strike [%s].\nSetting to 4.0, please check and set to a number 4.0 or greater!", s->warp_time, s->name);
					// And then make the Assert obsolete -- Zacam
					s->warp_time = 4.0f;
				}
			}

			string_index = optional_string_either("+Radius:", "+Min Radius:");
			if (string_index == 0)
			{
				stuff_float(&s->radius);
				s->max_radius = -1.0f;
			}
			else if (string_index == 1)
			{
				stuff_float(&s->radius);
				required_string("+Max Radius:");
				stuff_float(&s->max_radius);
			}

			string_index = optional_string_either("+Offset:", "+Min Offset:");
			if (string_index == 0)
			{
				stuff_float(&s->offset);
				s->max_offset = -1.0f;
			}
			else if (string_index == 1)
			{
				stuff_float(&s->offset);
				required_string("+Max Offset:");
				stuff_float(&s->max_offset);
			}
			
			if (optional_string("+Shape:"))
			{
				switch (required_string_one_of(3, "Point", "Circle", "Sphere"))
				{
					case 0:
						required_string("Point");
						s->shape = SSM_SHAPE_POINT;
						break;
					case 1:
						required_string("Circle");
						FALLTHROUGH;
					case -1:	// If we're ignoring parse errors and can't identify the shape, go with a circle.
						s->shape = SSM_SHAPE_CIRCLE;
						break;
					case 2:
						required_string("Sphere");
						s->shape = SSM_SHAPE_SPHERE;
						break;
					default:
						UNREACHABLE("Impossible return value from required_string_one_of(); get a coder!\n");
				}
			}

			if (optional_string("+HUD Message:"))
				stuff_boolean(&s->send_message);

			if (optional_string("+Custom Message:"))
			{
				stuff_string(s->custom_message, F_NAME, NAME_LENGTH);
				s->use_custom_message = true;
			}

			parse_game_sound("+Alarm Sound:", &s->sound_index);

			// don't add new entry if this is just a modified one
			if (!no_create)
				Ssm_info.push_back(new_ssm);
		}
	}
	catch (const parse::ParseException& e)
	{
		mprintf(("TABLES: Unable to parse '%s'!  Error message = %s.\n", filename, e.what()));
		return;
	}
}

// game init
void ssm_init()
{
	if (cf_exists_full("ssm.tbl", CF_TYPE_TABLES)) {
		mprintf(("TABLES => Starting parse of 'ssm.tbl'...\n"));
		parse_ssm("ssm.tbl");
	}
	parse_modular_table(NOX("*-ssm.tbm"), parse_ssm);

	// We already warned the modder that an SSM strike is invalid without a valid weapon, so now remove any that are in the list
	Ssm_info.erase(
		std::remove_if(Ssm_info.begin(), Ssm_info.end(), [](const ssm_info &ssm)
		{
			return ssm.weapon_info_index < 0;
		}),
		Ssm_info.end()
	);

	// Now that we've populated Ssm_info, let's validate weapon $SSM: entries.
	validate_SSM_entries();
}

void ssm_get_random_start_pos(vec3d *out, const vec3d *start, const matrix *orient, size_t ssm_index)
{
	vec3d temp;
	ssm_info *s = &Ssm_info[ssm_index];
	float radius, offset;

	if (s->max_radius == -1.0f)
		radius = s->radius;
	else
		radius = frand_range(s->radius, s->max_radius);

	if (s->max_offset == -1.0f)
		offset = s->offset;
	else
		offset = frand_range(s->offset, s->max_offset);

	switch (s->shape) {
	case SSM_SHAPE_SPHERE:
		// get a random vector in a sphere around the target
		vm_vec_random_in_sphere(&temp, start, radius, true);
		break;
	case SSM_SHAPE_CIRCLE:
		// get a random vector in the circle of the firing plane
		vm_vec_random_in_circle(&temp, start, orient, radius, true);
		break;
	case SSM_SHAPE_POINT:
		// boooring
		vm_vec_scale_add(&temp, start, &orient->vec.fvec, radius);
		break;
	default:
		UNREACHABLE("Unknown shape '%d' in SSM type #" SIZE_T_ARG " ('%s'). This should not be possible; get a coder!\n", s->shape, ssm_index, s->name);
		break;
	}

	// offset it a bit
	vm_vec_scale_add(out, &temp, &orient->vec.fvec, offset);
}

// level init
void ssm_level_init()
{
}

// start a subspace missile effect
// (it might be possible to make `target` const, but that would set off another const-cascade)
void ssm_create(object *target, const vec3d *start, size_t ssm_index, const ssm_firing_info *override, int team)
{	
	ssm_strike ssm;
	matrix dir;
	int idx, count;

	// sanity
	Assert(target != NULL);
	if(target == NULL){
		return;
	}
	Assert(start != NULL);
	if(start == NULL){
		return;
	}
	if (ssm_index >= Ssm_info.size()) {
		return;
	}

	// Init the ssm data

	count = Ssm_info[ssm_index].count;
	if (Ssm_info[ssm_index].max_count != -1 && (Ssm_info[ssm_index].max_count - count) > 0) {
		count = Random::next(count, Ssm_info[ssm_index].max_count);
	}

	// override in multiplayer
	if(override != NULL){
		ssm.sinfo = *override;
	}
	// single player or the server
	else {
		// forward orientation
		vec3d temp;

		vm_vec_normalized_dir(&temp, &target->pos, start);
		vm_vector_2_matrix_norm(&dir, &temp, nullptr, nullptr);

		// stuff info
		ssm.sinfo.count = count;
		ssm.sinfo.ssm_index = ssm_index;
		ssm.sinfo.target = target;
		ssm.sinfo.ssm_team = team;

		// Instead of pushing them on one at a time, let's just grab all the memory we'll need at once
		// (as a side effect, don't need to change the logic from the old array-based code)
		ssm.sinfo.delay_stamp.resize(count);
		ssm.sinfo.start_pos.resize(count);

		for (idx = 0; idx < count; idx++) {
			ssm.sinfo.delay_stamp[idx] = _timestamp(200 + Random::next(-199, 1000));
			ssm_get_random_start_pos(&ssm.sinfo.start_pos[idx], start, &dir, ssm_index);
		}

		ssm_info *si = &Ssm_info[ssm_index];
		weapon_info *wip = &Weapon_info[si->weapon_info_index];
		if (wip->wi_flags[Weapon::Info_Flags::Beam]) {
			ssm.sinfo.duration = ((si->warp_time - ((wip->b_info.beam_warmup / 1000.0f) + wip->b_info.beam_life + (wip->b_info.beam_warmdown / 1000.0f))) / 2.0f) / si->warp_time;
		} else {
			ssm.sinfo.duration = 0.5f;
		}

		// if we're the server, send a packet
		if(MULTIPLAYER_MASTER){
			//
		}
	}

	ssm.done_flags.clear();
	ssm.done_flags.resize(count);
	ssm.fireballs.clear();
	ssm.fireballs.resize(count, -1);
	
	if(Ssm_info[ssm_index].send_message) {
		if (!Ssm_info[ssm_index].use_custom_message)
			HUD_printf("%s", XSTR("Firing artillery", 1570));
		else
			HUD_printf("%s", Ssm_info[ssm_index].custom_message);
	}
	if (Ssm_info[ssm_index].sound_index.isValid()) {
		snd_play(gamesnd_get_game_sound(Ssm_info[ssm_index].sound_index));
	}

	Ssm_strikes.push_back(ssm);
}

// delete a finished ssm effect
void ssm_delete(SCP_list<ssm_strike>::iterator ssm)
{
	Ssm_strikes.erase(ssm);
}

// process subspace missile stuff
void ssm_process()
{
	int idx, finished;
	SCP_list<ssm_strike>::iterator moveup, eraser;
	ssm_info *si;
	int weapon_objnum;

	// process all strikes
	moveup = Ssm_strikes.begin();
	while ( moveup != Ssm_strikes.end() ) {
		// get the type
		Assertion(moveup->sinfo.ssm_index < Ssm_info.size(), "Invalid SSM index detected!");
		si = &Ssm_info[moveup->sinfo.ssm_index];

		// check all the individual missiles
		finished = 1;
		for(idx=0; idx<moveup->sinfo.count; idx++){
			// if this guy is not marked as done
			if(!moveup->done_flags[idx]){
				finished = 0;				

				// if he already has the fireball effect
				if(moveup->fireballs[idx] >= 0){
					if ((1.0f - fireball_lifeleft_percent(&Objects[moveup->fireballs[idx]])) >= moveup->sinfo.duration) {
						weapon_info *wip = &Weapon_info[si->weapon_info_index];

						// get an orientation
						vec3d temp;
						matrix orient;

						vm_vec_normalized_dir(&temp, &moveup->sinfo.target->pos, &moveup->sinfo.start_pos[idx]);
						vm_vector_2_matrix_norm(&orient, &temp, nullptr, nullptr);

						// are we a beam? -MageKing17
						if (wip->wi_flags[Weapon::Info_Flags::Beam]) {
							int num_beams = wip->b_info.beam_type == BeamType::OMNI && (int)wip->b_info.t5info.burst_rot_pattern.size() > 1 ?
								(int)wip->b_info.t5info.burst_rot_pattern.size() : 1;
							for (int i = 0; i < num_beams; i++) {
								beam_fire_info fire_info;
								memset(&fire_info, 0, sizeof(beam_fire_info));

								fire_info.accuracy = 0.000001f;		// this will guarantee a hit
								fire_info.shooter = nullptr;
								fire_info.turret = nullptr;
								fire_info.target = moveup->sinfo.target;
								fire_info.target_subsys = nullptr;
								fire_info.bfi_flags |= BFIF_FLOATING_BEAM;
								fire_info.starting_pos = moveup->sinfo.start_pos[idx];
								fire_info.beam_info_index = si->weapon_info_index;
								fire_info.team = static_cast<char>(moveup->sinfo.ssm_team);
								fire_info.fire_method = BFM_SUBSPACE_STRIKE;
								fire_info.burst_index = i;

								// Fill target_pos but DONT turn on BFIF_TARGETING_COORDS
								// It would mess up normal beams but type 5s can still make use of this
								fire_info.target_pos1 = orient.vec.fvec + fire_info.starting_pos;

								// fire the beam
								beam_fire(&fire_info);

								moveup->done_flags[idx] = true;
							}
						} else {
							// fire the missile and flash the screen
							weapon_objnum = weapon_create(&moveup->sinfo.start_pos[idx], &orient, si->weapon_info_index, -1, -1, true);

							if (weapon_objnum >= 0) {
								Weapons[Objects[weapon_objnum].instance].team = moveup->sinfo.ssm_team;
								Weapons[Objects[weapon_objnum].instance].homing_object = moveup->sinfo.target;
								Weapons[Objects[weapon_objnum].instance].target_sig = moveup->sinfo.target->signature;
							}

							// this makes this particular missile done
							moveup->done_flags[idx] = true;
						}
					}
				}
				// maybe create his warpin effect
				else if((moveup->sinfo.delay_stamp[idx].isValid()) && timestamp_elapsed(moveup->sinfo.delay_stamp[idx])){
					// get an orientation
					vec3d temp;
					matrix orient;

                    vm_vec_normalized_dir(&temp, &moveup->sinfo.target->pos, &moveup->sinfo.start_pos[idx]);
					vm_vector_2_matrix_norm(&orient, &temp, nullptr, nullptr);
					moveup->fireballs[idx] = fireball_create(&moveup->sinfo.start_pos[idx], si->fireball_type, FIREBALL_WARP_EFFECT, -1, si->warp_radius, false, &vmd_zero_vector, si->warp_time, 0, &orient);
				}
			}
		}
		if(finished){
			eraser = moveup;
			++moveup;
			ssm_delete(eraser);
			continue;
		}
		
		++moveup;
	}
}


// test code for subspace missile strike -------------------------------------------

// level init
void hud_init_artillery()
{
}

// update all hud artillery related stuff
void hud_artillery_update()
{
}

// render all hud artillery related stuff
void hud_artillery_render()
{
	// render how long the player has been painting his target	
	if((Player_ai != NULL) && (Player_ai->artillery_objnum >= 0)){
		gr_set_color_fast(&Color_bright_blue);
		gr_printf_no_resize(gr_screen.center_offset_x + 10, gr_screen.center_offset_y + 50, "%f", Player_ai->artillery_lock_time);
	}
}



#include "ai/aibig.h"
#include "ai/aiinternal.h"
#include "asteroid/asteroid.h"
#include "debugconsole/console.h"
#include "freespace.h"
#include "gamesnd/gamesnd.h"
#include "gamesequence/gamesequence.h"
#include "globalincs/linklist.h"
#include "globalincs/systemvars.h"
#include "iff_defs/iff_defs.h"
#include "io/timer.h"
#include "math/staticrand.h"
#include "network/multi.h"
#include "network/multimsgs.h"
#include "object/objectdock.h"
#include "scripting/global_hooks.h"
#include "scripting/scripting.h"
#include "render/3d.h"
#include "ship/ship.h"
#include "ship/shipfx.h"
#include "utils/Random.h"
#include "weapon/beam.h"
#include "weapon/flak.h"
#include "weapon/muzzleflash.h"
#include "weapon/swarm.h"
#include "weapon/weapon.h"
#include "utils/modular_curves.h"

#include <climits>


// How close a turret has to be point at its target before it
// can fire.  If the dot of the gun normal and the vector from gun
// to target is greater than this, the turret fires.  The smaller
// the sloppier the shooting.
#define AICODE_TURRET_DUMBFIRE_ANGLE		(0.8f)	
#define AICODE_TURRET_HEATSEEK_ANGLE		(0.7f)	
#define AICODE_TURRET_MAX_TIME_IN_RANGE	(5.0f)
#define BEAM_NEBULA_RANGE_REDUCE_FACTOR		(0.8f)

float Lethality_range_const = 2.0f;
DCF(lethality_range, "N for modifying range: 1 / (1+N) at 100")
{
	dc_stuff_float(&Lethality_range_const);
}

float Player_lethality_bump[NUM_SKILL_LEVELS] = {
	// 0.0f, 5.0f, 10.0f, 25.0f, 40.0f
	0.0f, 0.0f, 0.0f, 0.0f, 0.0f
};

const char *Turret_target_order_names[NUM_TURRET_ORDER_TYPES] = {
	"Bombs",
	"Ships",
	"Asteroids",
};

#define EEOF_BIG_ONLY		(1<<0)	// turret fires only at big and huge ships
#define EEOF_SMALL_ONLY		(1<<1)	// turret fires only at small ships
#define EEOF_TAGGED_ONLY	(1<<2)	// turret fires only at tagged ships

#define EEOF_BEAM			(1<<3)	// turret is a beam
#define EEOF_FLAK			(1<<4)	// turret is flak
#define EEOF_LASER			(1<<5)	// turret is a laser
#define EEOF_MISSILE		(1<<6)	// turret is a missile

const char* Turret_valid_types[NUM_TURRET_TYPES] = {
	"Beam",
	"Flak",
	"Laser",
	"Missile",
};

typedef struct eval_enemy_obj_struct {
	int			turret_parent_objnum = -1;			// parent of turret
	float		weapon_travel_dist = 0.0f;			// max targeting range of turret weapon
	int			enemy_team_mask = 0;
	bool		weapon_system_ok = false;			// is the weapon subsystem of turret ship ok
	int			eeo_flags = 0;

	const vec3d	*tpos = nullptr;
	const vec3d	*tvec = nullptr;
	const ship_subsys *turret_subsys = nullptr;
	int			current_enemy = -1;


	float		nearest_attacker_dist = 99999.0f;		// nearest ship
	int			nearest_attacker_objnum = -1;

	float		nearest_homing_bomb_dist = 99999.0f;	// nearest homing bomb
	int			nearest_homing_bomb_objnum = -1;

	float		nearest_bomb_dist = 99999.0f;			// nearest non-homing bomb
	int			nearest_bomb_objnum = -1;

	float		nearest_dist = 99999.0f;				// nearest ship attacking this turret
	int			nearest_objnum = -1;
}	eval_enemy_obj_struct;

// the current world orientation of the turret matrix, corresponding to its fvec and uvec defined in the model
// is NOT affected by the turret's current aiming
void turret_instance_find_world_orient(matrix* out_mat, int model_instance_num, int submodel_num, const matrix* objorient)
{
	auto pmi = model_get_instance(model_instance_num);
	auto pm = model_get(pmi->model_num);
	vec3d fvec, uvec;
	model_instance_local_to_global_dir(&fvec, &pm->submodel[submodel_num].frame_of_reference.vec.fvec, pm, pmi, pm->submodel[submodel_num].parent, objorient);
	model_instance_local_to_global_dir(&uvec, &pm->submodel[submodel_num].frame_of_reference.vec.uvec, pm, pmi, pm->submodel[submodel_num].parent, objorient);
	vm_vector_2_matrix_norm(out_mat, &fvec, &uvec);
}

/**
 * Is object in turret field of view?
 *
 * @param objp  Pointer to object to test
 * @param ss    Ship turret subsystem to test from
 * @param tvec  Turret initial vector
 * @param tpos  Turret initial position
 * @param dist  Distance from turret to center point of object
 *
 * @return true if objp is in fov of the specified turret.  Otherwise return false.
 */
bool object_in_turret_fov(const object *objp, const ship_subsys *ss, const vec3d *tvec, const vec3d *tpos, float dist)
{
	vec3d	v2e;
	float size_mod;
	bool  in_fov = false;

	if (ss->flags[Ship::Subsystem_Flags::FOV_edge_check]) {
		int model_num;
		switch (objp->type) {
			case OBJ_SHIP:
				model_num = Ship_info[Ships[objp->instance].ship_info_index].model_num;
				break;
			case OBJ_ASTEROID:
				model_num = Asteroid_info[Asteroids[objp->instance].asteroid_type].subtypes[Asteroids[objp->instance].asteroid_subtype].model_number;
				break;
			default:
				vm_vec_normalized_dir(&v2e, &objp->pos, tpos);
				size_mod = objp->radius / (dist + objp->radius);

				in_fov = turret_fov_test(ss, tvec, &v2e, size_mod);

				return in_fov;
		}

		auto pm = model_get(model_num);
		for (int i = 0; i < 8; i++) {
			vec3d bbox_point;
			vm_vec_unrotate(&bbox_point, &pm->bounding_box[i], &objp->orient);
			bbox_point += objp->pos;

			vm_vec_normalized_dir(&v2e, &bbox_point, tpos);
			in_fov = turret_fov_test(ss, tvec, &v2e, -0.2f);

			if (in_fov)
				return true;
		}
	} 
	// if the bbox points method didn't work (or fov_edge_checks isn't on)
	// try the normal method

	vm_vec_normalized_dir(&v2e, &objp->pos, tpos);
	size_mod = objp->radius / (dist + objp->radius);

	in_fov = turret_fov_test(ss, tvec, &v2e, size_mod);


	return in_fov;
}

/**
 * Is bomb headed towards given ship?
 *
 * @param bomb_objp Bomb specified
 * @param ship_objp Ship specified
 *
 * @return true if bomb_objp is headed towards ship_objp
 */
bool bomb_headed_towards_ship(const object *bomb_objp, const object *ship_objp)
{
	float		dot;
	vec3d	bomb_to_ship_vector;

	vm_vec_normalized_dir(&bomb_to_ship_vector, &ship_objp->pos, &bomb_objp->pos);
	dot = vm_vec_dot(&bomb_objp->orient.vec.fvec, &bomb_to_ship_vector);

	return ( dot > 0 );
}

// Set active weapon for turret
//This really isn't needed...but whatever -WMC

/**
 * Returns the best weapon on turret for target
 *
 * @note All non-negative return values are expressed in what I like to call "widx"s.
 * @return -1 if unable to find a weapon for the target at all.
 */
int turret_select_best_weapon(const ship_subsys *turret, const object * /*target*/)
{
	//TODO: Fill this out with extraodinary gun-picking algorithms
	if(turret->weapons.num_primary_banks > 0)
		return 0;
	else if(turret->weapons.num_secondary_banks > 0)
		return MAX_SHIP_PRIMARY_BANKS;
	else
		return -1;
}

/**
 * Returns true if all weapons in swp have the specified flag
 */
bool all_turret_weapons_have_flags(const ship_weapon *swp, flagset<Weapon::Info_Flags> flags)
{
	int i;
	for(i = 0; i < swp->num_primary_banks; i++)
	{
		if(!(Weapon_info[swp->primary_bank_weapons[i]].wi_flags & flags).any_set())
			return false;
	}
	for(i = 0; i < swp->num_secondary_banks; i++)
	{
		if(!(Weapon_info[swp->secondary_bank_weapons[i]].wi_flags & flags).any_set())
			return false;
	}

	return true;
}

bool all_turret_weapons_have_flags(const ship_weapon *swp, Weapon::Info_Flags flags)
{
    int i;
    for (i = 0; i < swp->num_primary_banks; i++)
    {
        if (!(Weapon_info[swp->primary_bank_weapons[i]].wi_flags[flags]))
            return false;
    }
    for (i = 0; i < swp->num_secondary_banks; i++)
    {
        if (!(Weapon_info[swp->secondary_bank_weapons[i]].wi_flags[flags]))
            return false;
    }

    return true;
}

/**
 * Returns true if any of the weapons in swp have flags
 *
 */
bool turret_weapon_has_flags(const ship_weapon *swp, Weapon::Info_Flags flags)
{
	Assert(swp != NULL);
    
	int i = 0;
	for(i = 0; i < swp->num_primary_banks; i++)
	{
		if(swp->primary_bank_weapons[i] >=0) {
			if(Weapon_info[swp->primary_bank_weapons[i]].wi_flags[flags])
				return true;
		}
	}
	for(i = 0; i < swp->num_secondary_banks; i++)
	{
		if(swp->secondary_bank_weapons[i] >=0) {
			if(Weapon_info[swp->secondary_bank_weapons[i]].wi_flags[flags])
				return true;
		}
	}

	return false;
}

/**
 * Just gloms all the flags from all the weapons into one variable.  More efficient if all you need to do is test for the existence of a flag.
 */
flagset<Weapon::Info_Flags> turret_weapon_aggregate_flags(const ship_weapon *swp)
{
	Assert(swp != NULL);

    int i = 0;
    flagset<Weapon::Info_Flags> flags;
	for (i = 0; i < swp->num_primary_banks; i++)
	{
		if (swp->primary_bank_weapons[i] >= 0) {
			flags |= Weapon_info[swp->primary_bank_weapons[i]].wi_flags;
		}
	}
	for (i = 0; i < swp->num_secondary_banks; i++)
	{
		if (swp->secondary_bank_weapons[i] >= 0) {
			flags |= Weapon_info[swp->secondary_bank_weapons[i]].wi_flags;
		}
	}

	return flags;
}

/**
 * Returns true if any of the weapons in swp have the subtype specified
 *
 * @note It might be a little faster to optimize based on WP_LASER should only appear in primaries
 * and WP_MISSILE in secondaries. but in the interest of future coding I leave it like this.
 */
bool turret_weapon_has_subtype(const ship_weapon *swp, int subtype)
{
	Assert(swp != NULL);
    
	int i = 0;
	for(i = 0; i < swp->num_primary_banks; i++)
	{
		if(swp->primary_bank_weapons[i] >=0) {
			if(Weapon_info[swp->primary_bank_weapons[i]].subtype == subtype)
				return true;
		}
	}
	for(i = 0; i < swp->num_secondary_banks; i++)
	{
		if(swp->secondary_bank_weapons[i] >=0) {
			if(Weapon_info[swp->secondary_bank_weapons[i]].subtype == subtype)
				return true;
		}
	}

	return false;
}

/**
 * Use for getting a Weapon_info pointer, given a turret and a turret weapon indice
 *
 * @return a pointer to the Weapon_info for weapon_num
 */
const weapon_info *get_turret_weapon_wip(const ship_weapon *swp, int weapon_num)
{
	Assert(weapon_num < MAX_SHIP_WEAPONS);
	Assert(weapon_num >= 0);

	int wi_index;
	if(weapon_num >= MAX_SHIP_PRIMARY_BANKS)
		wi_index = swp->secondary_bank_weapons[weapon_num - MAX_SHIP_PRIMARY_BANKS];
	else
		wi_index = swp->primary_bank_weapons[weapon_num];

	if (wi_index < 0)
		return nullptr;
	else
		return &Weapon_info[wi_index];
}

int get_turret_weapon_next_fire_stamp(const ship_weapon *swp, int weapon_num)
{
	Assert(weapon_num < MAX_SHIP_WEAPONS);
	Assert(weapon_num >= 0);

	if(weapon_num >= MAX_SHIP_PRIMARY_BANKS)
		return swp->next_secondary_fire_stamp[weapon_num - MAX_SHIP_PRIMARY_BANKS];
	else
		return swp->next_primary_fire_stamp[weapon_num];
}

void set_turret_weapon_next_fire_stamp(ship_weapon *swp, int weapon_num, int delta_ms)
{
	Assert(weapon_num < MAX_SHIP_WEAPONS);
	Assert(weapon_num >= 0);

	if (weapon_num >= MAX_SHIP_PRIMARY_BANKS)
		swp->next_secondary_fire_stamp[weapon_num - MAX_SHIP_PRIMARY_BANKS] = timestamp(delta_ms);
	else
		swp->next_primary_fire_stamp[weapon_num] = timestamp(delta_ms);
}

/**
 * Returns the longest-ranged weapon on a turret
 *
 * @todo This function is kinda slow
 */
float longest_turret_weapon_range(const ship_weapon *swp)
{
	float longest_range_so_far = 0.0f;
	float weapon_range;
	weapon_info *wip;

	int i = 0;
	for(i = 0; i < swp->num_primary_banks; i++)
	{
		wip = &Weapon_info[swp->primary_bank_weapons[i]];
		if (wip->wi_flags[Weapon::Info_Flags::Local_ssm])
			weapon_range = wip->lssm_lock_range;
		else
			weapon_range = MIN(wip->lifetime * wip->max_speed, wip->weapon_range);

		if(weapon_range > longest_range_so_far)
			longest_range_so_far = weapon_range;
	}
	for(i = 0; i < swp->num_secondary_banks; i++)
	{
		wip = &Weapon_info[swp->secondary_bank_weapons[i]];
		if (wip->wi_flags[Weapon::Info_Flags::Local_ssm])
			weapon_range = wip->lssm_lock_range;
		else
			weapon_range = MIN(wip->lifetime * wip->max_speed, wip->weapon_range);

		if(weapon_range > longest_range_so_far)
			longest_range_so_far = weapon_range;
	}

	return longest_range_so_far;
}

/**
 * Is valid turret target? 
 *
 * @param objp          Object that turret is considering as an enemy
 * @param turret_parent	Object of ship that turret sits on
 *
 * @return !0 if objp can be considered for a turret target, 0 otherwise
 */
int valid_turret_enemy(object *objp, object *turret_parent)
{
	if ( objp == turret_parent ) {
		return 0;
	}

	if ( objp->type == OBJ_ASTEROID ) {
		return 1;
	}

	if ( objp->type == OBJ_SHIP ) {
		Assert( objp->instance >= 0 );
		ship *shipp;
		ship_info *sip;
		shipp = &Ships[objp->instance];
		sip = &Ship_info[shipp->ship_info_index];

		// don't fire at ships with protected bit set!!!
		if ( objp->flags[Object::Object_Flags::Protected] ) {
			return 0;
		}

		// don't shoot at ships without collision check
		if (!(objp->flags[Object::Object_Flags::Collides]) && !(objp->flags[Object::Object_Flags::Attackable_if_no_collide])) {
			return 0;
		}

		// don't shoot at arriving ships
		if (shipp->is_arriving()) {
			return 0;
		}

		// Goober5000 - don't fire at cargo containers (now specified in ship_types)
		if ( (sip->class_type >= 0) && !(Ship_types[sip->class_type].flags[Ship::Type_Info_Flags::AI_turrets_attack]) ) {
			return 0;
		}

		return 1;
	}

	if ( objp->type == OBJ_WEAPON ) {
		Assert( objp->instance >= 0 );
		weapon *wp = &Weapons[objp->instance];
		weapon_info *wip = &Weapon_info[wp->weapon_info_index];

		if (wip->subtype == WP_LASER && !(wip->wi_flags[Weapon::Info_Flags::Turret_Interceptable])) {	// If the thing can't be shot down, don't try. -MageKing17
			return 0;
		}

		if ( (!((wip->wi_flags[Weapon::Info_Flags::Bomb]) || (wip->wi_flags[Weapon::Info_Flags::Turret_Interceptable])) && !(Ai_info[Ships[turret_parent->instance].ai_index].ai_profile_flags[AI::Profile_Flags::Allow_turrets_target_weapons_freely]) ) ) {
			return 0;
		}

		if ( (wip->wi_flags[Weapon::Info_Flags::Local_ssm]) && (wp->lssm_stage == 3) ) {
			return 0;
		}

		if ( !iff_x_attacks_y(obj_team(turret_parent), wp->team) ) {
			return 0;
		}

		return 1;
	}

	return 0;
}

extern int Player_attacking_enabled;
void evaluate_obj_as_target(object *objp, eval_enemy_obj_struct *eeo)
{
	object	*turret_parent_obj = &Objects[eeo->turret_parent_objnum];
	ship *shipp;
	auto ss = eeo->turret_subsys;
	float dist, dist_comp;
	bool turret_has_no_target = false;

	// Don't look for bombs when weapon system is not ok
	if (objp->type == OBJ_WEAPON && !eeo->weapon_system_ok) {
		return;
	}

	if ( !valid_turret_enemy(objp, turret_parent_obj) ) {
		return;
	}

#ifndef NDEBUG
	if (!Player_attacking_enabled && (objp == Player_obj)) {
		return;
	}
#endif

	if ( objp->type == OBJ_SHIP ) {
		shipp = &Ships[objp->instance];
		ship_info* sip = &Ship_info[shipp->ship_info_index];

		// check on enemy team
		if ( !iff_matches_mask(shipp->team, eeo->enemy_team_mask) ) {
			return;
		}

		// check if protected
		if (objp->flags[Object::Object_Flags::Protected]) {
			return;
		}

		// check if beam protected
		if (eeo->eeo_flags & EEOF_BEAM) {
			if (objp->flags[Object::Object_Flags::Beam_protected]) {
				return;
			}
		}

		// check if flak protected
		if (eeo->eeo_flags & EEOF_FLAK) {
			if (objp->flags[Object::Object_Flags::Flak_protected]) {
				return;
			}
		}

		// check if laser protected
		if (eeo->eeo_flags & EEOF_LASER) {
			if (objp->flags[Object::Object_Flags::Laser_protected]) {
				return;
			}
		}

		// check if missile protected
		if (eeo->eeo_flags & EEOF_MISSILE) {
			if (objp->flags[Object::Object_Flags::Missile_protected]) {
				return;
			}
		}

		// don't shoot at big ships with huge weapons unless they have the flag
		if (eeo->eeo_flags & EEOF_BIG_ONLY) {
			if (sip->class_type == -1 || !(Ship_types[sip->class_type].flags[Ship::Type_Info_Flags::Targeted_by_huge_Ignored_by_small_only])) {
				return;
			}
		}
		//  ^ Note the difference between these checks
		//  V   "small only" EXCLUDES only big ships, "huge" INCLUDES only big ships

		// don't shoot at ships ignored by small weapons if this is a small weapon
		if (eeo->eeo_flags & EEOF_SMALL_ONLY) {
			if (sip->class_type >= 0 && (Ship_types[sip->class_type].flags[Ship::Type_Info_Flags::Targeted_by_huge_Ignored_by_small_only])) {
				return;
			}
		}

		// check if turret flagged to only target tagged ships
		// Note: retail behaviour was turrets with tagged-only could fire at bombs
		// and could fire their spawn weapons
		// this check is almost redundant; see the almost identical check in ai_fire_from_turret
		// however if this is removed turrets still track targets but don't fire at them (which looks silly)
		if (eeo->eeo_flags & EEOF_TAGGED_ONLY) {
			if (!ship_is_tagged(objp) &&
					( (The_mission.ai_profile->flags[AI::Profile_Flags::Strict_turret_tagged_only_targeting]) ||
					( !(objp->type == OBJ_WEAPON) && !(turret_weapon_has_flags(&eeo->turret_subsys->weapons, Weapon::Info_Flags::Spawn))) )) {
				return;
			}
		}

		// check if valid target in nebula
		if ( !object_is_targetable(objp, &Ships[Objects[eeo->turret_parent_objnum].instance]) ) {
			// BYPASS ocassionally for stealth
			int try_anyway = FALSE;
			if ( is_object_stealth_ship(objp) ) {
				float turret_stealth_find_chance = 0.5f;
				float speed_mod = -0.1f + vm_vec_mag_quick(&objp->phys_info.vel) / 70.0f;
				if (frand() > (turret_stealth_find_chance + speed_mod)) {
					try_anyway = TRUE;
				}
			}

			if (!try_anyway) {
				return;
			}
		}

	} else {
		shipp = NULL;
	}

	// modify dist for BIG|HUGE, getting closest point on bbox, if not inside
	vec3d vec_to_target;
	vm_vec_sub(&vec_to_target, &objp->pos, eeo->tpos);
	dist = vm_vec_mag_quick(&vec_to_target) - objp->radius;
	
	if (dist < 0.0f) {
		dist = 0.0f;
	}
	
	dist_comp = dist;
	// if weapon has optimum range set then use it
	float optimum_range = ss->optimum_range;
	if (optimum_range > 0.0f) {
		if (dist < optimum_range) {
			dist_comp = (2*optimum_range) - dist;
		}
	}

	// if turret has been told to prefer targets from the current direction then do so
	float favor_one_side = ss->favor_current_facing;
	if (favor_one_side >= 1.0f) {
		vm_vec_normalize(&vec_to_target);
		float dot_to_target = vm_vec_dot(&ss->turret_last_fire_direction, &vec_to_target);
		dot_to_target = 1.0f - (dot_to_target / favor_one_side);
		dist_comp *= dot_to_target;
	}

	// check if object is a bomb attacking the turret parent
	// check if bomb is homing on the turret parent ship
	bool check_weapon = true;

	if ((Ai_info[Ships[turret_parent_obj->instance].ai_index].ai_profile_flags[AI::Profile_Flags::Prevent_targeting_bombs_beyond_range]) && (dist > eeo->weapon_travel_dist)) {
		check_weapon = false;
	}

	if ((objp->type == OBJ_WEAPON) && check_weapon) {
		//Maybe restrict the number of turrets attacking this bomb
		if (ss->turret_max_bomb_ownage != -1) {	
			int num_att_turrets = num_turrets_attacking(turret_parent_obj, OBJ_INDEX(objp));
			if (num_att_turrets > ss->system_info->turret_max_bomb_ownage) {
				return;
			}
		}

		if ( Weapons[objp->instance].homing_object == &Objects[eeo->turret_parent_objnum] ) {
			if ( dist_comp < eeo->nearest_homing_bomb_dist ) {
				if (!(ss->flags[Ship::Subsystem_Flags::FOV_Required]) && (eeo->current_enemy == -1)) {
					turret_has_no_target = true;
				}
				if ( (turret_has_no_target) || object_in_turret_fov(objp, ss, eeo->tvec, eeo->tpos, dist + objp->radius) ) {
					eeo->nearest_homing_bomb_dist = dist_comp;
					eeo->nearest_homing_bomb_objnum = OBJ_INDEX(objp);
				}
			}
		// if not homing, check if bomb is flying towards ship
		} else if ( bomb_headed_towards_ship(objp, &Objects[eeo->turret_parent_objnum]) ) {
			if ( dist_comp < eeo->nearest_bomb_dist ) {
				if (!(ss->flags[Ship::Subsystem_Flags::FOV_Required]) && (eeo->current_enemy == -1)) {
					turret_has_no_target = true;
				}
				if ( (turret_has_no_target) || object_in_turret_fov(objp, ss, eeo->tvec, eeo->tpos, dist + objp->radius) ) {
					eeo->nearest_bomb_dist = dist_comp;
					eeo->nearest_bomb_objnum = OBJ_INDEX(objp);
				}
			}
		}
	} // end weapon section

	// maybe recalculate dist for big or huge ship
//	if (shipp && (Ship_info[shipp->ship_info_index].is_big_or_huge())) {
//		fvi_ray_boundingbox(min, max, start, direction, hit);
//		dist = vm_vec_dist_quick(hit, tvec);
//	}

	// check for nearest attcker
	if ( (shipp) && (dist < eeo->weapon_travel_dist) ) {
		ai_info *aip = &Ai_info[shipp->ai_index];

		// modify distance based on number of turrets from my ship attacking enemy (add 10% per turret)
		// dist *= (num_enemies_attacking(OBJ_INDEX(objp))+2)/2;	//	prevents lots of ships from attacking same target
		int num_att_turrets = num_turrets_attacking(turret_parent_obj, OBJ_INDEX(objp));
		dist_comp *= (1.0f + 0.1f*num_att_turrets);

		// return if we're over the cap
//		int max_turrets = 3 + Game_skill_level * Game_skill_level;
		int max_turrets = The_mission.ai_profile->max_turret_ownage_target[Game_skill_level];
		if (objp->flags[Object::Object_Flags::Player_ship]) {
			max_turrets = The_mission.ai_profile->max_turret_ownage_player[Game_skill_level];
		}
		// Apply the per-turret limit for small targets, if there is one and this is a small target
		if (ss->turret_max_target_ownage != -1 && (Ship_info[shipp->ship_info_index].is_small_ship())) {
			max_turrets = MIN(max_turrets, ss->system_info->turret_max_target_ownage);
		}
		if (num_att_turrets > max_turrets) {
			return;
		}

		// modify distance based on lethality of objp to my ship
		float active_lethality = aip->lethality;
		if (objp->flags[Object::Object_Flags::Player_ship]) {
			active_lethality += Player_lethality_bump[Game_skill_level];
		}

		dist_comp /= (1.0f + 0.01f*Lethality_range_const*active_lethality);

		// Make level 2 tagged ships more likely to be targeted
		if (shipp->level2_tag_left > 0.0f) {
			dist_comp *= 0.3f;
		}

		// check if objp is targeting the turret's ship, or if objp has just hit the turret's ship
		if ( aip->target_objnum == eeo->turret_parent_objnum || aip->last_objsig_hit == Objects[eeo->turret_parent_objnum].signature ) {
			// A turret will always target a ship that is attacking itself... self-preservation!
			if ( aip->targeted_subsys == eeo->turret_subsys ) {
				dist_comp *= 0.5f;	// highest priority
			}
		}

		// maybe update nearest attacker
		if ( dist_comp < eeo->nearest_attacker_dist ) {
			if (!(ss->flags[Ship::Subsystem_Flags::FOV_Required]) && (eeo->current_enemy == -1)) {
				turret_has_no_target = true;
			}
			if ( (turret_has_no_target) || object_in_turret_fov(objp, ss, eeo->tvec, eeo->tpos, dist + objp->radius) ) {
				// nprintf(("AI", "Nearest enemy = %s, dist = %7.3f, dot = %6.3f, fov = %6.3f\n", Ships[objp->instance].ship_name, dist, vm_vec_dot(&v2e, tvec), tp->turret_fov));
				eeo->nearest_attacker_dist = dist_comp;
				eeo->nearest_attacker_objnum = OBJ_INDEX(objp);
			}
		}
	} // end ship section

	// check if object is an asteroid attacking the turret parent - taylor
	if (objp->type == OBJ_ASTEROID) {
		if ( eeo->turret_parent_objnum == asteroid_collide_objnum(objp) ) {
			// give priority to the closest asteroid *impact* (ms intervals)
			dist_comp *= 0.9f + (0.01f * asteroid_time_to_impact(objp));

			if (dist_comp < eeo->nearest_dist ) {
				if (!(ss->flags[Ship::Subsystem_Flags::FOV_Required]) && (eeo->current_enemy == -1)) {
					turret_has_no_target = true;
				}
				if ( (turret_has_no_target) || object_in_turret_fov(objp, ss, eeo->tvec, eeo->tpos, dist + objp->radius) ) {
					eeo->nearest_dist = dist_comp;
					eeo->nearest_objnum = OBJ_INDEX(objp);
				}
			}
		}
	} // end asteroid selection
}

/**
 * Given an object and an enemy team, return the index of the nearest enemy object.
 *
 * @param turret_parent_objnum	Parent objnum for the turret
 * @param turret_subsys			Pointer to system_info for the turret subsystem
 * @param enemy_team_mask		OR'ed TEAM_ flags for the enemy of the turret parent ship
 * @param tpos                  Position of turret (world coords)
 * @param tvec					Forward vector of turret (world coords)
 * @param current_enemy			Objnum of current turret target
 * @param big_only_flag
 * @param small_only_flag
 * @param tagged_only_flag
 * @param beam_flag
 * @param flak_flag
 * @param laser_flag
 * @param missile_flag
 */
int get_nearest_turret_objnum(int turret_parent_objnum, const ship_subsys *turret_subsys, int enemy_team_mask, const vec3d *tpos, const vec3d *tvec, int current_enemy, bool big_only_flag, bool small_only_flag, bool tagged_only_flag, bool beam_flag, bool flak_flag, bool laser_flag, bool missile_flag)
{
	eval_enemy_obj_struct eeo;
	auto swp = &turret_subsys->weapons;

	// list of stuff to go thru
	ship_obj		*so;
	missile_obj *mo;

	//wip=&Weapon_info[tp->turret_weapon_type];
	//weapon_travel_dist = MIN(wip->lifetime * wip->max_speed, wip->weapon_range);

	//if (wip->wi_flags[Weapon::Info_Flags::Local_ssm])
	//	weapon_travel_dist=wip->lssm_lock_range;

	// Set flag based on strength of weapons subsystem.  If weapons subsystem is destroyed, don't let turrets fire at bombs
	bool weapon_system_ok = !ship_subsystems_blown(&Ships[Objects[turret_parent_objnum].instance], SUBSYSTEM_WEAPONS);

	// Initialize eeo struct.
	eeo.turret_parent_objnum = turret_parent_objnum;
	eeo.weapon_system_ok = weapon_system_ok;
	eeo.weapon_travel_dist = longest_turret_weapon_range(swp);

	// set flags
	eeo.eeo_flags = 0;
	if (big_only_flag)
		eeo.eeo_flags |= EEOF_BIG_ONLY;
	if (small_only_flag)
		eeo.eeo_flags |= EEOF_SMALL_ONLY;
	if (tagged_only_flag)
		eeo.eeo_flags |= EEOF_TAGGED_ONLY;

	// flags for weapon types
	if (beam_flag)
		eeo.eeo_flags |= EEOF_BEAM;
	if (flak_flag)
		eeo.eeo_flags |= EEOF_FLAK;
	if (laser_flag)
		eeo.eeo_flags |= EEOF_LASER;
	if (missile_flag)
		eeo.eeo_flags |= EEOF_MISSILE;

	eeo.enemy_team_mask = enemy_team_mask;
	eeo.current_enemy = current_enemy;
	eeo.tpos = tpos;
	eeo.tvec = tvec;
	eeo.turret_subsys = turret_subsys;

	// here goes the new targeting priority setting
	int n_tgt_priorities;
	int priority_weapon_idx = -1;

	// check for turret itself first
	n_tgt_priorities = turret_subsys->num_target_priorities;

	// turret had no priorities set for it.. try weapons
	if (n_tgt_priorities <= 0) {
		if (swp->num_primary_banks > 0)
			// first try highest primary slot...
			priority_weapon_idx = swp->primary_bank_weapons[0];
		else
			// ...and then secondary slot
			priority_weapon_idx = swp->secondary_bank_weapons[0];

		if (priority_weapon_idx > -1)
			n_tgt_priorities = Weapon_info[priority_weapon_idx].num_targeting_priorities;
	}

	if (n_tgt_priorities > 0) 
    {
		for(int i = 0; i < n_tgt_priorities; i++) {
			// courtesy of WMC...
			ai_target_priority *tt;
			if (priority_weapon_idx == -1)
				tt = &Ai_tp_list[turret_subsys->target_priority[i]];
			else
				tt = &Ai_tp_list[Weapon_info[priority_weapon_idx].targeting_priorities[i]];

			int n_types = (int)tt->ship_type.size();
			int n_s_classes = (int)tt->ship_class.size();
			int n_w_classes = (int)tt->weapon_class.size();
			
			bool found_something;
			for (auto ptr: list_range(&obj_used_list)) {
				if (ptr->flags[Object::Object_Flags::Should_be_dead])
					continue;

				found_something = false;

				if(tt->obj_type > -1 && (ptr->type == tt->obj_type)) {
					found_something = true;
				}

				if( ( n_types > 0 ) && ( ptr->type == OBJ_SHIP ) ) {
					for (int j = 0; j < n_types; j++) {
						if ( Ship_info[Ships[ptr->instance].ship_info_index].class_type == tt->ship_type[j] ) {
							found_something = true;
						}
					}
				}

				if( ( n_s_classes > 0 ) && ( ptr->type == OBJ_SHIP ) ) {
					for (int j = 0; j < n_s_classes; j++) {
						if ( Ships[ptr->instance].ship_info_index == tt->ship_class[j] ) {
							found_something = true;
						}
					}
				}

				if( ( n_w_classes > 0 ) && ( ptr->type == OBJ_WEAPON ) ) {
					for (int j = 0; j < n_w_classes; j++) {
						if ( Weapons[ptr->instance].weapon_info_index == tt->weapon_class[j] ) {
							found_something = true;
						}
					}
				}

				if( (tt->wif_flags.any_set()) && (ptr->type == OBJ_WEAPON) ) {
					if( ( (Weapon_info[Weapons[ptr->instance].weapon_info_index].wi_flags & tt->wif_flags ) == tt->wif_flags) ) {
							found_something = true;
					}
				}

				if( ( tt->sif_flags.any_set() && (ptr->type == OBJ_SHIP) ) ) {
					if( (Ship_info[Ships[ptr->instance].ship_info_index].flags & tt->sif_flags) == tt->sif_flags)
	                {
    					found_something = true;
					}
				}

                if ((tt->obj_flags.any_set()) && !((ptr->flags & tt->obj_flags) == tt->obj_flags)) {
					found_something = true;
				}

				if(!(found_something)) {
					//we didnt find this object within this priority group
					//skip to next without evaluating the object as target
					continue;
				}

				evaluate_obj_as_target(ptr, &eeo);
			}

			//homing weapon entry...
			/*
			if ( eeo.nearest_homing_bomb_objnum != -1 ) {               // highest priority is an incoming homing bomb
				return eeo.nearest_homing_bomb_objnum;
				//weapon entry...
			} else if ( eeo.nearest_bomb_objnum != -1 ) {               // next highest priority is an incoming dumbfire bomb
				return eeo.nearest_bomb_objnum;
				//ship entry...
			} else if ( eeo.nearest_attacker_objnum != -1 ) {        // next highest priority is an attacking ship
				return eeo.nearest_attacker_objnum;
				//something else entry...
			} else if ( eeo.nearest_objnum != -1 ) {
				return eeo.nearest_objnum;
			}
			*/
			// if we got something...
			if ( ( eeo.nearest_homing_bomb_objnum != -1 ) || 
				( eeo.nearest_bomb_objnum != -1 ) || 
				( eeo.nearest_attacker_objnum != -1 ) ||
				( eeo.nearest_objnum != -1 ) )
			{
				// ...start with homing bombs...
				int return_objnum =	eeo.nearest_homing_bomb_objnum;
				float return_distance = eeo.nearest_homing_bomb_dist;

				// ...next test non-homing bombs...
				if ( eeo.nearest_bomb_dist < return_distance ) {
					return_objnum =  eeo.nearest_bomb_objnum;
					return_distance = eeo.nearest_bomb_dist;
				}

				// ...then attackers...
				if ( eeo.nearest_attacker_dist < return_distance ) {
					return_objnum =  eeo.nearest_attacker_objnum;
					return_distance = eeo.nearest_attacker_dist;
				}

				// ...and finally the rest of the lot...
				if ( eeo.nearest_dist < return_distance ) {
					return_objnum =  eeo.nearest_objnum;
				}

				// ...and return the objnum to the closest target regardless
				return return_objnum;
			}
		}
	} else 
    {
        flagset<Weapon::Info_Flags> tmp_flagset;
        const Weapon::Info_Flags weapon_flags[] = { Weapon::Info_Flags::Huge, Weapon::Info_Flags::Flak, Weapon::Info_Flags::Homing_aspect, Weapon::Info_Flags::Homing_heat, Weapon::Info_Flags::Homing_javelin, Weapon::Info_Flags::Spawn };
        tmp_flagset.set_multiple(std::begin(weapon_flags), std::end(weapon_flags));

		for(int i = 0; i < NUM_TURRET_ORDER_TYPES; i++)
		{
			ai_info *aip = &Ai_info[Ships[Objects[eeo.turret_parent_objnum].instance].ai_index];
			switch(turret_subsys->turret_targeting_order[i])
			{
				case -1:
						//Empty priority slot
					break;

				case 0:
					//Return if a bomb is found
					//don't fire anti capital ship turrets at bombs.
					if ( !((aip->ai_profile_flags[AI::Profile_Flags::Huge_turret_weapons_ignore_bombs]) && big_only_flag) )
					{
						// Missile_obj_list
						for( mo = GET_FIRST(&Missile_obj_list); mo != END_OF_LIST(&Missile_obj_list); mo = GET_NEXT(mo) ) {
							auto objp = &Objects[mo->objnum];
							if (objp->flags[Object::Object_Flags::Should_be_dead])
								continue;

							Assert(objp->type == OBJ_WEAPON);
							if ((Weapon_info[Weapons[objp->instance].weapon_info_index].wi_flags[Weapon::Info_Flags::Bomb]) || (Weapon_info[Weapons[objp->instance].weapon_info_index].wi_flags[Weapon::Info_Flags::Turret_Interceptable]))
							{
								evaluate_obj_as_target(objp, &eeo);
							}
						}
						// highest priority
						if ( eeo.nearest_homing_bomb_objnum != -1 ) {					// highest priority is an incoming homing bomb
							return eeo.nearest_homing_bomb_objnum;
						} else if ( eeo.nearest_bomb_objnum != -1 ) {					// next highest priority is an incoming dumbfire bomb
							return eeo.nearest_bomb_objnum;
						}
					}
					break;

				case 1:
					//Return if a ship is found
					// Ship_used_list
					for ( so = GET_FIRST(&Ship_obj_list); so != END_OF_LIST(&Ship_obj_list); so = GET_NEXT(so) ) {
						auto objp = &Objects[so->objnum];
						if (objp->flags[Object::Object_Flags::Should_be_dead])
							continue;
						evaluate_obj_as_target(objp, &eeo);
					}

					// next highest priority is attacking ship
					if ( eeo.nearest_attacker_objnum != -1 ) {			// next highest priority is an attacking ship
						return eeo.nearest_attacker_objnum;
					}
					break;

				case 2:
					//Return if an asteroid is found
					// asteroid check - taylor
					asteroid_obj *ao;

					// don't use turrets that are better for other things:
					// - no cap ship beams
					// - no flak
					// - no heat or aspect missiles
					// - no spawn type missiles/bombs
					// do use for sure:
					// - lasers
					// - dumbfire type missiles
					// - AAA beams
                    
					if ( !all_turret_weapons_have_flags(swp, tmp_flagset) ) {
						// Asteroid_obj_list
						for ( ao = GET_FIRST(&Asteroid_obj_list); ao != END_OF_LIST(&Asteroid_obj_list); ao = GET_NEXT(ao) ) {
							auto objp = &Objects[ao->objnum];
							if (objp->flags[Object::Object_Flags::Should_be_dead])
								continue;
							evaluate_obj_as_target(objp, &eeo);
						}

						if (eeo.nearest_objnum != -1) {
							return eeo.nearest_objnum;
						}
					}
					break;

				default:
					UNREACHABLE("Invalid target type of %d sent to get_nearest_turret_objnum.  Please report to the SCP!", turret_subsys->turret_targeting_order[i]);
			}
		}
	}

	return -1;
}

int Use_parent_target = 0;
DCF_BOOL(use_parent_target, Use_parent_target)

/**
 * Return objnum if enemy found, else return -1;
 *		
 * @param turret_subsys     Pointer to turret subsystem
 * @param objnum			Parent objnum for the turret
 * @param tpos				Position of turret (world coords)
 * @param tvec				Forward vector of turret (world coords)
 * @param current_enemy     Objnum of current turret target
 */
int find_turret_enemy(const ship_subsys *turret_subsys, int objnum, const vec3d *tpos, const vec3d *tvec, int current_enemy)
{
	int					enemy_team_mask, enemy_objnum;
	ship_info			*sip;

	enemy_team_mask = iff_get_attackee_mask(obj_team(&Objects[objnum]));

	bool big_only_flag = all_turret_weapons_have_flags(&turret_subsys->weapons, Weapon::Info_Flags::Huge);
	bool small_only_flag = all_turret_weapons_have_flags(&turret_subsys->weapons, Weapon::Info_Flags::Small_only);
    bool tagged_only_flag = all_turret_weapons_have_flags(&turret_subsys->weapons, Weapon::Info_Flags::Tagged_only) || (turret_subsys->weapons.flags[Ship::Weapon_Flags::Tagged_Only]);

	bool beam_flag = turret_weapon_has_flags(&turret_subsys->weapons, Weapon::Info_Flags::Beam);
	bool flak_flag = turret_weapon_has_flags(&turret_subsys->weapons, Weapon::Info_Flags::Flak);
	bool laser_flag = turret_weapon_has_subtype(&turret_subsys->weapons, WP_LASER);
	bool missile_flag = turret_weapon_has_subtype(&turret_subsys->weapons, WP_MISSILE);

	//	If a small ship and target_objnum != -1, use that as goal.
	ai_info	*aip = &Ai_info[Ships[Objects[objnum].instance].ai_index];
	sip = &Ship_info[Ships[Objects[objnum].instance].ship_info_index];

	if ((sip->class_type >= 0) && (Ship_types[sip->class_type].flags[Ship::Type_Info_Flags::Turret_tgt_ship_tgt]) && (aip->target_objnum != -1)) {
		int target_objnum = aip->target_objnum;

		if (Objects[target_objnum].signature == aip->target_signature) {
			if (iff_matches_mask(Ships[Objects[target_objnum].instance].team, enemy_team_mask)) {
				if ( !(Objects[target_objnum].flags[Object::Object_Flags::Protected]) ) {		// check this flag as well
					// nprintf(("AI", "Frame %i: Object %i resuming goal of object %i\n", AI_FrameCount, objnum, target_objnum));
					return target_objnum;
				}
			}
		} else {
			aip->target_objnum = -1;
			aip->target_signature = -1;
		}
	// Not small or small with target objnum
	} else {
		// maybe use aip->target_objnum as next target
		if ((frand() < 0.8f) && (aip->target_objnum != -1) && Use_parent_target) {

			//check if aip->target_objnum is valid target
			auto target_flags = Objects[aip->target_objnum].flags;
			if ( target_flags[Object::Object_Flags::Protected] ) {
				// AL 2-27-98: why is a protected ship being targeted?
				set_target_objnum(aip, -1);
				return -1;
			}

			// maybe use ship target_objnum if valid for turret
			// check for beam weapon and beam protected, etc.
			bool skip = false;
			     if ( target_flags[Object::Object_Flags::Beam_protected] && beam_flag ) skip = true;
			else if ( target_flags[Object::Object_Flags::Flak_protected] && flak_flag ) skip = true;
			else if ( target_flags[Object::Object_Flags::Laser_protected] && laser_flag ) skip = true;
            else if ( target_flags[Object::Object_Flags::Missile_protected] && missile_flag) skip = true;

			if (!skip) {
				if ( Objects[aip->target_objnum].type == OBJ_SHIP ) {
					ship_info* esip = &Ship_info[Ships[Objects[aip->target_objnum].instance].ship_info_index];
					// check for huge weapon and huge ship
					if ( !big_only_flag || (esip->class_type >= 0 && Ship_types[esip->class_type].flags[Ship::Type_Info_Flags::Targeted_by_huge_Ignored_by_small_only]) ) {
						// check for tagged only and tagged ship
						if ( tagged_only_flag && ship_is_tagged(&Objects[aip->target_objnum]) ) {
							// select new target if aip->target_objnum is out of field of view
							vec3d v2e;
							bool in_fov;
							vm_vec_normalized_dir(&v2e, &Objects[aip->target_objnum].pos, tpos);

							in_fov = turret_fov_test(turret_subsys, tvec, &v2e);

							// MODIFY FOR ATTACKING BIG SHIP
							// dot += (0.5f * Objects[aip->target_objnum].radius / dist);
							if (in_fov) {
								return aip->target_objnum;
							}
						}
					}
				}
			}
		}
	}

	enemy_objnum = get_nearest_turret_objnum(objnum, turret_subsys, enemy_team_mask, tpos, tvec, current_enemy, big_only_flag, small_only_flag, tagged_only_flag, beam_flag, flak_flag, laser_flag, missile_flag);
	if ( enemy_objnum >= 0 ) {
		Assert( !((Objects[enemy_objnum].flags[Object::Object_Flags::Beam_protected]) && beam_flag) );
		Assert( !((Objects[enemy_objnum].flags[Object::Object_Flags::Flak_protected]) && flak_flag) );
		Assert( !((Objects[enemy_objnum].flags[Object::Object_Flags::Laser_protected]) && laser_flag) );
		Assert( !((Objects[enemy_objnum].flags[Object::Object_Flags::Missile_protected]) && missile_flag) );
		Assertion(!Objects[enemy_objnum].flags[Object::Object_Flags::Protected], "find_turret_enemy selected an object of type %d %s that is protected, please report to the SCP!", Objects[enemy_objnum].type, Objects[enemy_objnum].type == OBJ_SHIP ? Ships[Objects[enemy_objnum].instance].ship_name : "");

		if ( Objects[enemy_objnum].flags[Object::Object_Flags::Protected] ) {
			enemy_objnum = aip->target_objnum;
		}
	}

	return enemy_objnum;
}


/**
 * Given an object and a turret on that object, return the global position and forward vector
 * of the turret.
 *
 * @param objp  Pointer to object
 * @param tp    Turret model system on that object
 * @param gpos  [Output] Global absolute position of gun firing point
 * @param gvec  [Output] Global vector
 *
 * @note The gun normal is the unrotated gun normal, (the center of the FOV cone), not
 * the actual gun normal given using the current turret heading.  But it _is_ rotated into the model's orientation
 * in global space.
 * @note2 Because of this, both single-part and multi-part turrets are treated the same way; no need to find the multi-part's gun submodel.
 */
void ship_get_global_turret_info(const object *objp, const model_subsystem *tp, vec3d *gpos, vec3d *gvec)
{
	auto model_instance_num = Ships[objp->instance].model_instance_num;
	if (gpos)
		model_instance_local_to_global_point(gpos, &vmd_zero_vector, model_instance_num, tp->subobj_num, &objp->orient, &objp->pos);
	if (gvec)
		model_instance_local_to_global_dir(gvec, &tp->turret_norm, model_instance_num, tp->subobj_num, &objp->orient, true);
}

/**
 * Given an object and a turret on that object, return the actual firing point of the gun and its normal.
 *
 * @note This uses the current turret angles.  We are keeping track of which
 * gun to fire next in the ship specific info for this turret subobject.  Use this info
 * to determine which position to fire from next.
 *
 * @param objp          Pointer to object
 * @param ssp           Pointer to turret subsystem
 * @param gpos          Absolute position of gun firing point
 * @param avg_origin	Use virtual gun pos corresponding to the average of the firing points
 * @param gvec          Vector from *gpos to *targetp
 * @param use_angles    Use current angles
 * @param targetp       Absolute position of target object
 */
void ship_get_global_turret_gun_info(const object *objp, const ship_subsys *ssp, vec3d *gpos, bool avg_origin, vec3d *gvec, bool use_angles, const vec3d *targetp)
{
	vec3d *gun_pos;
	model_subsystem *tp = ssp->system_info;
	polymodel_instance *pmi = model_get_instance(Ships[objp->instance].model_instance_num);
	polymodel *pm = model_get(pmi->model_num);

	vec3d avg_gun_pos;
	if (avg_origin) {
		vm_vec_avg_n(&avg_gun_pos, tp->turret_num_firing_points, tp->turret_firing_point);
		gun_pos = &avg_gun_pos;
	} else {
		gun_pos = &tp->turret_firing_point[ssp->turret_next_fire_pos % tp->turret_num_firing_points];
	}

	model_instance_local_to_global_point(gpos, gun_pos, pm, pmi, tp->turret_gun_sobj, &objp->orient, &objp->pos);
	

	// we might not need to calculate this
	if (!gvec)
		return;

	if (use_angles) {
		model_instance_local_to_global_dir(gvec, &tp->turret_norm, pm, pmi, tp->turret_gun_sobj, &objp->orient);
		vm_vec_normalize(gvec);
	} else {
		Assertion(targetp != nullptr, "The targetp parameter must not be null here!");
		vm_vec_normalized_dir(gvec, targetp, gpos);
	}
}

/**
 * Update turret aiming data based on max turret aim update delay
 */
void turret_ai_update_aim(const ai_info *aip, const object *En_Objp, ship_subsys *ss)
{
	if (Missiontime >= ss->next_aim_pos_time)
	{
		ss->last_aim_enemy_pos = En_Objp->pos;
		ss->last_aim_enemy_vel = En_Objp->phys_info.vel;
		ss->next_aim_pos_time = Missiontime + fl2f(frand_range(0.0f, aip->ai_turret_max_aim_update_delay));
	}
	else
	{
		//Update the position based on the velocity (assume no velocity vector change)
		vm_vec_scale_add2(&ss->last_aim_enemy_pos, &ss->last_aim_enemy_vel, flFrametime);
	}
}

/**
 *	Sets predicted enemy position.  Previously done at the beginning of aifft_rotate_turret.
 *	If the turret (*ss) has a subsystem targeted, the subsystem is used as the predicted point.
 */
void aifft_update_predicted_enemy_pos(const object *objp, const ship *shipp, ship_subsys *ss, const vec3d *global_gun_pos, const vec3d *global_gun_vec, const object *lep, vec3d *predicted_enemy_pos)
{
	if (ss->turret_enemy_objnum != -1) {
		model_subsystem *tp = ss->system_info;
		vec3d	target_vel;
		float		weapon_system_strength;
		//HACK HACK HACK -WMC
		//This should use the best weapon variable
		//It doesn't, because I haven't implemented code to set it yet -WMC
		int best_weapon_tidx = turret_select_best_weapon(ss, lep);

		//This turret doesn't have any good weapons
		if (best_weapon_tidx < 0)
			return;

		auto wip = get_turret_weapon_wip(&ss->weapons, best_weapon_tidx);

		//	weapon_system_strength scales time enemy in range in 0..1.  So, the lower this is, the worse the aiming will be.
		weapon_system_strength = ship_get_subsystem_strength(shipp, SUBSYSTEM_WEAPONS);

		//Update "known" position and velocity of target. Only matters if max_aim_update_delay is set.
		turret_ai_update_aim(&Ai_info[shipp->ai_index], &Objects[ss->turret_enemy_objnum], ss);

		//Figure out what point on the ship we want to point the gun at, and store the global location
		//in enemy_point.
		vec3d	enemy_point;
		if ((ss->targeted_subsys != NULL) && !(ss->flags[Ship::Subsystem_Flags::No_SS_targeting])) {
			if (ss->turret_enemy_objnum != -1) {
				vm_vec_unrotate(&enemy_point, &ss->targeted_subsys->system_info->pnt, &Objects[ss->turret_enemy_objnum].orient);
				vm_vec_add2(&enemy_point, &ss->last_aim_enemy_pos);
			}
		} else {
			if ((lep->type == OBJ_SHIP) && (Ship_info[Ships[lep->instance].ship_info_index].is_big_or_huge())) {
				ai_big_pick_attack_point_turret(lep, ss, global_gun_pos, global_gun_vec, &enemy_point, tp->turret_fov, MIN(wip->max_speed * wip->lifetime, wip->weapon_range));
			} else {
				enemy_point = ss->last_aim_enemy_pos;
			}
		}

		target_vel = ss->last_aim_enemy_vel;

		//Try to guess where the enemy will be, and store that spot in predicted_enemy_pos
		if (The_mission.ai_profile->flags[AI::Profile_Flags::Use_additive_weapon_velocity]) {
			vm_vec_scale_sub2(&target_vel, &objp->phys_info.vel, wip->vel_inherit_amount);
		}

		if (IS_VEC_NULL(&The_mission.gravity) || wip->gravity_const == 0.0f)
			set_predicted_enemy_pos_turret(predicted_enemy_pos, global_gun_pos, objp, &enemy_point, &target_vel, wip->max_speed, ss->turret_time_enemy_in_range * (weapon_system_strength + 1.0f)/2.0f);
		else {
			vec3d shoot_vec;
			vec3d gravity_vec = The_mission.gravity * wip->gravity_const;
			if (physics_lead_ballistic_trajectory(global_gun_pos, &enemy_point, &target_vel, wip->max_speed, &gravity_vec, &shoot_vec)) {
				*predicted_enemy_pos = *global_gun_pos + shoot_vec * 1000.0f;
			}
		}

		//Mess with the turret's accuracy if the weapon system is damaged.
		if (weapon_system_strength < Weapon_SS_Threshold_Turret_Inaccuracy) {
			vec3d	rand_vec;

			static_randvec(Missiontime >> 18, &rand_vec);	//	Return same random number for two seconds.
			//	Add to predicted_enemy_pos value in .45 to 1.5x radius of enemy ship, so will often miss, but not by a huge amount.
			vm_vec_scale_add2(predicted_enemy_pos, &rand_vec, (1.0f - weapon_system_strength)*1.5f * lep->radius);
		}
	}
}

/**
 * Rotate a turret towards an enemy.
 *
 *	Some obscure model thing only John Slagel knows about.
 *
 * @return TRUE if caller should use angles in subsequent rotations.
 */
bool aifft_rotate_turret(const object *objp, const ship *shipp, ship_subsys *ss, const vec3d *global_gun_pos, const vec3d *global_gun_vec, const vec3d *predicted_enemy_pos)
{
	bool ret_val __UNUSED = false; // to be used in future, see comment @ end of function
	auto pmi = model_get_instance(shipp->model_instance_num);
	auto pm = model_get(pmi->model_num);

	bool in_lab = (gameseq_get_state() == GS_STATE_LAB);

	if (!in_lab && ss->turret_enemy_objnum != -1) {
		model_subsystem* tp = ss->system_info;

		// Get the normalized dir between the turret and the predicted enemy position.
		// If the dot product is smaller than or equal to the turret's FOV, try and point the gun at it.
		vec3d v2e;
		vm_vec_normalized_dir(&v2e, predicted_enemy_pos, global_gun_pos);

		bool in_fov;
		in_fov = turret_fov_test(ss, global_gun_vec, &v2e);

		if (in_fov) {
			ret_val = model_rotate_gun(objp, pm, pmi, ss, predicted_enemy_pos);
		} else if ((tp->flags[Model::Subsystem_Flags::Turret_reset_idle]) &&(timestamp_elapsed(ss->rotation_timestamp))) {
			ret_val = model_rotate_gun(objp, pm, pmi, ss, nullptr);
		}
	} else if (!in_lab && (ss->system_info->flags[Model::Subsystem_Flags::Turret_reset_idle]) && (timestamp_elapsed(ss->rotation_timestamp))) {
		ret_val = model_rotate_gun(objp, pm, pmi, ss, nullptr);
	} else if (in_lab) {
		// in the lab, if predicted_enemy_pos is zero vec, then go to initial, else try to aim at the target
		if (predicted_enemy_pos != nullptr && *predicted_enemy_pos == vmd_zero_vector) {
			ret_val = model_rotate_gun(objp, pm, pmi, ss, nullptr);
		} else {
			ret_val = model_rotate_gun(objp, pm, pmi, ss, predicted_enemy_pos);
		}
	}

	// by default "ret_val" should be set to true for multi-part turrets, and false for single-part turrets
	// but we need to keep retail behavior by default, which means always returning false unless a special
	// flag is used.  the "ret_val" stuff is here is needed/wanted at a later date however.
	//return ret_val;

	// return false by default (to preserve retail behavior) but allow for a per-subsystem option
	// for using the turret normals for firing
	if (ss->system_info->flags[Model::Subsystem_Flags::Fire_on_normal])
		return true;

	return false;
}

/**
 * Determine if subsystem *enemy_subsysp is hittable from objp.
 *
 *	@return Dot product of vector from point abs_gunposp to *enemy_subsysp, if hittable
 */
float	aifft_compute_turret_dot(const object *objp, const object *enemy_objp, const vec3d *abs_gunposp, const ship_subsys *turret_subsysp, const ship_subsys *enemy_subsysp)
{
	float	dot_out;
	vec3d	subobj_pos, vector_out;

	vm_vec_unrotate(&subobj_pos, &enemy_subsysp->system_info->pnt, &enemy_objp->orient);
	vm_vec_add2(&subobj_pos, &enemy_objp->pos);

	if (ship_subsystem_in_sight(enemy_objp, enemy_subsysp, abs_gunposp, &subobj_pos, true, &dot_out, &vector_out)) {
		vec3d	turret_norm;

		model_instance_local_to_global_dir(&turret_norm, &turret_subsysp->system_info->turret_norm, Ships[objp->instance].model_instance_num, turret_subsysp->system_info->subobj_num, &objp->orient, true);
		float dot_return = vm_vec_dot(&turret_norm, &vector_out);

		if (Ai_info[Ships[objp->instance].ai_index].ai_profile_flags[AI::Profile_Flags::Smart_subsystem_targeting_for_turrets]) {
			if (dot_return > turret_subsysp->system_info->turret_fov) {
				// target is in sight and in fov
				return dot_return;
			} else {
				// target is in sight but is not in turret's fov
				return -1.0f;
			}
		} else {
			// target is in sight and we don't care if its in turret's fov or not
			return dot_return;
		}
	} else
		return -1.0f;

}

// NOTE:  Do not change this value unless you understand exactly what it means and what it does.
//        It refers to how many (non-destroyed) subsystems (and turrets) will be scanned for possible
//        targeting, per turret, per frame.  A higher value will process more systems at once,
//        but it will be much slower to scan though them.  It is not necessary to scan all
//        non-destroyed subsystem each frame for each turret.  Also, "aifft_max_checks" is balanced
//        against the original value, be sure to account for this discrepancy with any changes.
#define MAX_AIFFT_TURRETS			60

ship_subsys *aifft_list[MAX_AIFFT_TURRETS];
float aifft_rank[MAX_AIFFT_TURRETS];
int aifft_list_size = 0;
int aifft_max_checks = 5;
DCF(mf, "Adjusts the maximum number of tries an AI may do when trying to pick a subsystem to attack (Default is 5)")
{
	dc_stuff_int(&aifft_max_checks);

	if (aifft_max_checks <= 0) {
		dc_printf("Value must be a non-negative, non-zero integer\n");
		dc_printf("aifft_max_checks set to default value of 5\n");

		aifft_max_checks = 5;
	}
}


/**
 * Pick a subsystem to attack on enemy_objp.
 * Only pick one if enemy_objp is a big ship or a capital ship.
 *
 * @return Dot product from turret to subsystem in *dot_out
 */
ship_subsys *aifft_find_turret_subsys(const object *objp, const ship_subsys *ssp, const vec3d *global_gun_pos, const object *enemy_objp, float *dot_out)
{
	ship	*eshipp, *shipp;
	ship_info	*esip;
	ship_subsys	*best_subsysp = NULL;
	float dot;

	Assert(enemy_objp->type == OBJ_SHIP);

	eshipp = &Ships[enemy_objp->instance];
	esip = &Ship_info[eshipp->ship_info_index];

	shipp = &Ships[objp->instance];

	float	best_dot = 0.0f;
	*dot_out = best_dot;

	//	Only pick a turret to attack on large ships.
	if (!esip->is_big_or_huge())
		return best_subsysp;

	// Make sure big or huge ship *actually* has subsystems  (ie, knossos)
	if (esip->n_subsystems == 0) {
		return best_subsysp;
	}

	// first build up a list subsystems to traverse
	ship_subsys	*pss;
	aifft_list_size = 0;
	for ( pss = GET_FIRST(&eshipp->subsys_list); pss !=END_OF_LIST(&eshipp->subsys_list); pss = GET_NEXT(pss) ) {
		model_subsystem *psub = pss->system_info;

		// if we've reached max turrets bail
		if(aifft_list_size >= MAX_AIFFT_TURRETS){
			break;
		}

		// Don't process destroyed objects
		if ( pss->current_hits <= 0.0f ){
			continue;
		}
		
		switch (psub->type) {
		case SUBSYSTEM_WEAPONS:
			aifft_list[aifft_list_size] = pss;
			aifft_rank[aifft_list_size++] = 1.4f;
			break;

		case SUBSYSTEM_TURRET:
			aifft_list[aifft_list_size] = pss;
			aifft_rank[aifft_list_size++] = 1.2f;
			break;

		case SUBSYSTEM_SENSORS:
		case SUBSYSTEM_ENGINE:
			aifft_list[aifft_list_size] = pss;
			aifft_rank[aifft_list_size++] = 1.0f;
			break;
		}
	}

	// DKA:  6/28/99 all subsystems can be destroyed.
	//Assert(aifft_list_size > 0);
	if (aifft_list_size == 0) {
		return best_subsysp;
	}

	// determine a stride value so we're not checking too many turrets
	int stride = aifft_list_size > aifft_max_checks ? aifft_list_size / aifft_max_checks : 1;
	if(stride <= 0){
		stride = 1;
	}
	int offset = Random::next((aifft_list_size % stride) + 1);	// returns an offset in [0, size % stride] inclusive
	int idx;
	float dot_fov_modifier = 0.0f;

	if (Ai_info[shipp->ai_index].ai_profile_flags[AI::Profile_Flags::Smart_subsystem_targeting_for_turrets]) {
		if (ssp->system_info->turret_fov < 0)
			dot_fov_modifier = ssp->system_info->turret_fov;
	}

	for(idx=offset; idx<aifft_list_size; idx+=stride){
		dot = aifft_compute_turret_dot(objp, enemy_objp, global_gun_pos, ssp, aifft_list[idx]);

		if ((dot - dot_fov_modifier)* aifft_rank[idx] > best_dot) {
			best_dot = (dot - dot_fov_modifier)*aifft_rank[idx];
			best_subsysp = aifft_list[idx];
		}
	}

	Assert(best_subsysp != &eshipp->subsys_list);

	*dot_out = best_dot;
	return best_subsysp;
}

/**
 * @return true if the specified target should scan for a new target, otherwise return false
 */
bool turret_should_pick_new_target(ship_subsys *turret)
{
	return timestamp_elapsed(turret->turret_next_enemy_check_stamp);
}

/**
 * Set the next fire timestamp for a turret, based on weapon type and ai class
 */
void turret_set_next_fire_timestamp(int weapon_num, const weapon_info *wip, ship_subsys *turret, const ai_info *aip, const WeaponLaunchCurveData& launch_curve_data)
{
	Assert(weapon_num < MAX_SHIP_WEAPONS);
	float wait = 1000.0f;
	// we have to add 1 to wip->burst_shots so that the multiplier behaves as expected, then remove 1 from burst_shots to match the zero-indexed burst_counter
	int base_burst_shots = (wip->burst_flags[Weapon::Burst_Flags::Num_firepoints_burst_shots] ? turret->system_info->turret_num_firing_points : wip->burst_shots + 1);
	float burst_shots_mult = wip->weapon_launch_curves.get_output(weapon_info::WeaponLaunchCurveOutputs::BURST_SHOTS_MULT, launch_curve_data);
	int burst_shots = MAX(fl2i(i2fl(base_burst_shots) * burst_shots_mult) - 1, 0);

	bool burst = burst_shots > turret->weapons.burst_counter[weapon_num];

	if (burst) {
		wait *= wip->burst_delay;
		wait *= wip->weapon_launch_curves.get_output(weapon_info::WeaponLaunchCurveOutputs::BURST_DELAY_MULT, launch_curve_data);
		turret->weapons.burst_counter[weapon_num]++;
	} else {
		// Random fire delay (DahBlount) used in ship_fire_primary(), added here by wookieejedi to correct oversight
		if (wip->max_delay != 0.0f && wip->min_delay != 0.0f) {
			wait *= frand_range(wip->min_delay, wip->max_delay);
			wait *= wip->weapon_launch_curves.get_output(weapon_info::WeaponLaunchCurveOutputs::FIRE_WAIT_MULT, launch_curve_data);
		} else {
			wait *= wip->fire_wait;
			wait *= wip->weapon_launch_curves.get_output(weapon_info::WeaponLaunchCurveOutputs::FIRE_WAIT_MULT, launch_curve_data);
		}
		if ((burst_shots > 0) && (wip->burst_flags[Weapon::Burst_Flags::Random_length])) {
			turret->weapons.burst_counter[weapon_num] = Random::next(burst_shots);
			turret->weapons.burst_seed[weapon_num] = Random::next();
		} else {
			turret->weapons.burst_counter[weapon_num] = 0;
			turret->weapons.burst_seed[weapon_num] = Random::next();
		}
	}

	int *fs_dest;
	if(weapon_num < MAX_SHIP_PRIMARY_BANKS)
		fs_dest = &turret->weapons.next_primary_fire_stamp[weapon_num];
	else
		fs_dest = &turret->weapons.next_secondary_fire_stamp[weapon_num - MAX_SHIP_PRIMARY_BANKS];

	//Check for the new cooldown flag
	if(!((wip->wi_flags[Weapon::Info_Flags::Same_turret_cooldown]) || ((burst_shots > 0) && (wip->burst_flags[Weapon::Burst_Flags::Fast_firing]))))
	{

		// make side even for team vs. team
		if (MULTI_TEAM) {
			// flak guns need to fire more rapidly
			if (wip->wi_flags[Weapon::Info_Flags::Flak]) {
				wait *= aip->ai_ship_fire_delay_scale_friendly * 0.5f;
				if (aip->ai_class_autoscale)
				{
					if (The_mission.ai_profile->flags[AI::Profile_Flags::Adjusted_AI_class_autoscale])
						wait += (ai_get_autoscale_index(Num_ai_classes) - ai_get_autoscale_index(aip->ai_class) - 1) * 40.0f;
					else
						wait += (Num_ai_classes - aip->ai_class - 1) * 40.0f;
				}
			} else {
				wait *= aip->ai_ship_fire_delay_scale_friendly;
				if (aip->ai_class_autoscale)
				{
					if (The_mission.ai_profile->flags[AI::Profile_Flags::Adjusted_AI_class_autoscale])
						wait += (ai_get_autoscale_index(Num_ai_classes) - ai_get_autoscale_index(aip->ai_class) - 1) * 100.0f;
					else
						wait += (Num_ai_classes - aip->ai_class - 1) * 100.0f;
				}
			}
		} else {
			// flak guns need to fire more rapidly
			if (wip->wi_flags[Weapon::Info_Flags::Flak]) {
				if (Player_ship != nullptr && Ships[aip->shipnum].team == Player_ship->team) {
					wait *= aip->ai_ship_fire_delay_scale_friendly * 0.5f;
				} else {
					wait *= aip->ai_ship_fire_delay_scale_hostile * 0.5f;
				}	
				if (aip->ai_class_autoscale)
				{
					if (The_mission.ai_profile->flags[AI::Profile_Flags::Adjusted_AI_class_autoscale])
						wait += (ai_get_autoscale_index(Num_ai_classes) - ai_get_autoscale_index(aip->ai_class) - 1) * 40.0f;
					else
						wait += (Num_ai_classes - aip->ai_class - 1) * 40.0f;
				}

			} else if (wip->wi_flags[Weapon::Info_Flags::Huge]) {
				// make huge weapons fire independently of team
				wait *= aip->ai_ship_fire_delay_scale_friendly;
				if (aip->ai_class_autoscale)
				{
					if (The_mission.ai_profile->flags[AI::Profile_Flags::Adjusted_AI_class_autoscale])
						wait += (ai_get_autoscale_index(Num_ai_classes) - ai_get_autoscale_index(aip->ai_class) - 1) * 100.0f;
					else
						wait += (Num_ai_classes - aip->ai_class - 1) * 100.0f;
				}
			} else {
				// give team friendly an advantage
				if (Player_ship != nullptr && Ships[aip->shipnum].team == Player_ship->team) {
					wait *= aip->ai_ship_fire_delay_scale_friendly;
				} else {
					wait *= aip->ai_ship_fire_delay_scale_hostile;
				}	
				if (aip->ai_class_autoscale)
				{
					if (The_mission.ai_profile->flags[AI::Profile_Flags::Adjusted_AI_class_autoscale])
						wait += (ai_get_autoscale_index(Num_ai_classes) - ai_get_autoscale_index(aip->ai_class) - 1) * 100.0f;
					else
						wait += (Num_ai_classes - aip->ai_class - 1) * 100.0f;
				}
			}
		}
		// vary wait time +/- 10%
		wait *= frand_range(0.9f, 1.1f);
	}

	if(turret->rof_scaler != 1.0f && !(burst && turret->system_info->flags[Model::Subsystem_Flags::Burst_ignores_RoF_Mult]))
		wait /= get_adjusted_turret_rof(turret);

	(*fs_dest) = timestamp((int)wait);
}

/**
 * Decide if a turret should launch an aspect seeking missile
 */
bool turret_should_fire_aspect(const ship_subsys *turret, const weapon_info *wip, bool in_sight)
{
	if ( in_sight && (turret->turret_time_enemy_in_range >= MIN(wip->min_lock_time,AICODE_TURRET_MAX_TIME_IN_RANGE)) ) {
		return true;
	}

	return false;
}

/**
 * Update how long current target has been in this turrets range
 */
void turret_update_enemy_in_range(ship_subsys *turret, float seconds)
{
	turret->turret_time_enemy_in_range += seconds;

	if ( turret->turret_time_enemy_in_range < 0.0f ) {
		turret->turret_time_enemy_in_range = 0.0f;
	}

	if ( turret->turret_time_enemy_in_range > AICODE_TURRET_MAX_TIME_IN_RANGE ) {
		turret->turret_time_enemy_in_range = AICODE_TURRET_MAX_TIME_IN_RANGE;
	}
}

/**
 * Fire a weapon from a turret
 */
bool turret_fire_weapon(int weapon_num,
	ship_subsys *turret,
	int parent_objnum,
	const WeaponLaunchCurveData& launch_curve_data,
	const vec3d *orig_firing_pos,
	const vec3d *orig_firing_vec,
	const vec3d *predicted_pos = nullptr,
	float flak_range_override = 100.0f,
	bool play_sound = true)
{
	matrix	firing_orient;
	int weapon_objnum;
	ai_info	*parent_aip;
	ship		*parent_ship;
	float flak_range = 0.0f;
	weapon *wp;
	object *objp;

	// make sure we actually have a valid weapon
	auto wip = get_turret_weapon_wip(&turret->weapons, weapon_num);
	if (!wip)
		return false;

	bool in_lab = (gameseq_get_state() == GS_STATE_LAB);

	if (in_lab) {
		predicted_pos = nullptr;
	}

	bool last_shot_in_salvo = true;
	if (turret->system_info->flags[Model::Subsystem_Flags::Turret_salvo])
	{
		if ((turret->turret_next_fire_pos + 1) == (turret->system_info->turret_num_firing_points))
		{
			last_shot_in_salvo = true;
		}
		else
		{
			last_shot_in_salvo = false;
		}
	}

	//WMC - Limit firing to firestamp
	if((!timestamp_elapsed(get_turret_weapon_next_fire_stamp(&turret->weapons, weapon_num))) && last_shot_in_salvo)
		return false;

	parent_aip = &Ai_info[Ships[Objects[parent_objnum].instance].ai_index];
	parent_ship = &Ships[Objects[parent_objnum].instance];
	int turret_weapon_class = weapon_info_get_index(wip);

#ifndef NDEBUG
	// moved here from check_ok_to_fire
	if (turret->turret_enemy_objnum >= 0) {
		object	*tobjp = &Objects[turret->turret_enemy_objnum];

		// should not get this far. check if ship is protected from beam and weapon is type beam
		if ( (wip->wi_flags[Weapon::Info_Flags::Beam]) && (tobjp->flags[Object::Object_Flags::Beam_protected]) ) {
            nprintf(("Warning","Ship %s is trying to fire beam turret at beam protected ship\n", parent_ship->ship_name));
            return false;
		}
		// should not get this far. check if ship is protected from flak and weapon is type flak
		else if ( (wip->wi_flags[Weapon::Info_Flags::Flak]) && (tobjp->flags[Object::Object_Flags::Flak_protected]) ) {
            nprintf(("Warning","Ship %s is trying to fire flak turret at flak protected ship\n", parent_ship->ship_name));
            return false;
		}
		// should not get this far. check if ship is protected from laser and weapon is type laser
		else if ( (wip->subtype == WP_LASER) && (tobjp->flags[Object::Object_Flags::Laser_protected]) ) {
            nprintf(("Warning","Ship %s is trying to fire laser turret at laser protected ship\n", parent_ship->ship_name));
            return false;
		}
		// should not get this far. check if ship is protected from missile and weapon is type missile
		else if ( (wip->subtype == WP_MISSILE) && (tobjp->flags[Object::Object_Flags::Missile_protected]) ) {
            nprintf(("Warning","Ship %s is trying to fire missile turret at missile protected ship\n", parent_ship->ship_name));
            return false;
		}
	}
#endif

	if (check_ok_to_fire(parent_objnum, turret->turret_enemy_objnum, wip, -1, orig_firing_pos)) {
		auto turret_enemy_objp = (turret->turret_enemy_objnum >= 0) ? &Objects[turret->turret_enemy_objnum] : nullptr;

		ship_weapon* swp = &turret->weapons;
		turret->turret_last_fire_direction = *orig_firing_vec;

		const vec3d *firing_vec = orig_firing_vec;
		vec3d firing_vec_buf;

		if (turret->turret_inaccuracy > 0.0f) {
			vm_vec_random_cone(&firing_vec_buf, firing_vec, turret->turret_inaccuracy);
			firing_vec = &firing_vec_buf;
		}

		vm_vector_2_matrix_norm(&firing_orient, firing_vec, nullptr, nullptr);

		// grab and set some burst data before turret_set_next_fire_timestamp wipes it
		int old_burst_seed = swp->burst_seed[weapon_num];
		int old_burst_counter = swp->burst_counter[weapon_num];
		// only used by type 5 beams
		if (turret->weapons.burst_counter[weapon_num] == 0) {
			swp->per_burst_rot += wip->b_info.t5info.per_burst_rot;
			if (swp->per_burst_rot > PI2)
				swp->per_burst_rot -= PI2;
			else if (swp->per_burst_rot < -PI2)
				swp->per_burst_rot += PI2;
		}

		// set next fire timestamp for the turret
		if (last_shot_in_salvo)
			turret_set_next_fire_timestamp(weapon_num, wip, turret, parent_aip, launch_curve_data);

		// if this weapon is a beam weapon, handle it specially
		if (wip->wi_flags[Weapon::Info_Flags::Beam]) {
			// if this beam isn't free to fire
			if (!in_lab && !(turret->weapons.flags[Ship::Weapon_Flags::Beam_Free])) {
				return false;
			}

			for (int i = 0; i < wip->shots; i++) {
				beam_fire_info fire_info;

				// stuff beam firing info
				memset(&fire_info, 0, sizeof(beam_fire_info));
				fire_info.accuracy = 1.0f;
				fire_info.beam_info_index = turret_weapon_class;
				fire_info.beam_info_override = nullptr;
				fire_info.shooter = &Objects[parent_objnum];
				fire_info.target = turret_enemy_objp;
				if (wip->wi_flags[Weapon::Info_Flags::Antisubsysbeam])
					fire_info.target_subsys = turret->targeted_subsys;
				else
					fire_info.target_subsys = nullptr;
				fire_info.turret = turret;
				fire_info.burst_seed = old_burst_seed;
				fire_info.fire_method = BFM_TURRET_FIRED;
				fire_info.per_burst_rotation = swp->per_burst_rot;
				fire_info.burst_index = (old_burst_counter * wip->shots) + i;

				// If we're in the lab then force fire
				if (in_lab) {
					fire_info.fire_method = BFM_TURRET_FORCE_FIRED;
					fire_info.bfi_flags |= BFIF_TARGETING_COORDS;
					fire_info.target_pos1 = turret->last_aim_enemy_pos;
				}


				// fire a beam weapon
				weapon_objnum = beam_fire(&fire_info);

				if (weapon_objnum != -1) {
					objp = &Objects[weapon_objnum];

					parent_ship->last_fired_turret = turret;
					turret->last_fired_weapon_info_index = turret_weapon_class;
					turret->turret_last_fired = _timestamp();

					if (scripting::hooks::OnTurretFired->isActive()) {
						scripting::hooks::OnTurretFired->run(scripting::hooks::WeaponUsedConditions{ parent_ship , turret_enemy_objp, SCP_vector<int>{ turret_weapon_class }, true },
							scripting::hook_param_list(
								scripting::hook_param("Ship", 'o', &Objects[parent_objnum]),
								scripting::hook_param("Beam", 'o', objp),
								scripting::hook_param("Target", 'o', turret_enemy_objp)
							));
					}
				}
			}
			turret->flags.set(Ship::Subsystem_Flags::Has_fired); //set fired flag for scripting -nike
			return true;
		}
		// don't fire swam, but set up swarm info instead
		else if ((wip->wi_flags[Weapon::Info_Flags::Swarm]) || (wip->wi_flags[Weapon::Info_Flags::Corkscrew])) {
			if (swp->current_secondary_bank < 0) {
				swp->current_secondary_bank = 0;
			}
			int bank_to_fire = swp->current_secondary_bank;
			if (!in_lab && (turret->system_info->flags[Model::Subsystem_Flags::Turret_use_ammo]) && !ship_secondary_has_ammo(swp, bank_to_fire)) {
				if (!(turret->system_info->flags[Model::Subsystem_Flags::Use_multiple_guns])) {
					swp->current_secondary_bank++;
					if (swp->current_secondary_bank >= swp->num_secondary_banks) {
						swp->current_secondary_bank = 0;
					}
					return false;
				} else {
					return false;
				}
			} else {
				turret_swarm_set_up_info(parent_objnum, turret, wip, turret->turret_next_fire_pos, in_lab);

				turret->flags.set(Ship::Subsystem_Flags::Has_fired);	//set fired flag for scripting -nike
				return true;
			}
		}
		// now do anything else
		else {
			float shots_mult = wip->weapon_launch_curves.get_output(weapon_info::WeaponLaunchCurveOutputs::SHOTS_MULT, launch_curve_data);
			int shots = fl2i(i2fl(wip->shots) * shots_mult);
			for (int i = 0; i < shots; i++) {
				if (!in_lab && turret->system_info->flags[Model::Subsystem_Flags::Turret_use_ammo]) {
					int bank_to_fire, num_slots = turret->system_info->turret_num_firing_points;
					if (wip->subtype == WP_LASER) {
						int points;
						if (swp->num_primary_banks <= 0) {
							return false;
						}

						if (swp->current_primary_bank < 0){
							swp->current_primary_bank = 0;
							return false;
						}

						int	num_primary_banks = swp->num_primary_banks;

						Assert(num_primary_banks > 0);
						if (num_primary_banks < 1){
							return false;
						}

						bank_to_fire = swp->current_primary_bank;

						if (turret->system_info->flags[Model::Subsystem_Flags::Turret_salvo]) {
							points = num_slots;
						} else {
							points = 1;
						}

						if (swp->primary_bank_ammo[bank_to_fire] >= points) {
							swp->primary_bank_ammo[bank_to_fire] -= points;
						} else if ((swp->primary_bank_ammo[bank_to_fire] >= 0) && !(The_mission.ai_profile->flags[AI::Profile_Flags::Prevent_negative_turret_ammo])) {
							// default behavior allowed ammo to be negative
							swp->primary_bank_ammo[bank_to_fire] -= points;
						} else if (!(turret->system_info->flags[Model::Subsystem_Flags::Use_multiple_guns]) && (swp->primary_bank_ammo[bank_to_fire] < 0)) {
							swp->current_primary_bank++;
							if (swp->current_primary_bank >= swp->num_primary_banks) {
								swp->current_primary_bank = 0;
							}
							return false;
						} else {
							return false;
						}
					}
					else if (wip->subtype == WP_MISSILE) {
						if (swp->num_secondary_banks <= 0) {
							return false;
						}

						if (swp->current_secondary_bank < 0){
							swp->current_secondary_bank = 0;
							return false;
						}

						bank_to_fire = swp->current_secondary_bank;

						int start_slot, end_slot;

						start_slot = swp->secondary_next_slot[bank_to_fire];
						end_slot = start_slot;

						for (int j = start_slot; j <= end_slot; j++) {
							swp->secondary_next_slot[bank_to_fire]++;

							if (Weapon_info[swp->secondary_bank_weapons[bank_to_fire]].wi_flags[Weapon::Info_Flags::SecondaryNoAmmo])
								continue;

							if (swp->secondary_next_slot[bank_to_fire] > (num_slots - 1)){
								swp->secondary_next_slot[bank_to_fire] = 0;
							}

							if (swp->secondary_bank_ammo[bank_to_fire] > 0) {
								swp->secondary_bank_ammo[bank_to_fire]--;
							} else if ((swp->secondary_bank_ammo[bank_to_fire] == 0) && !(The_mission.ai_profile->flags[AI::Profile_Flags::Prevent_negative_turret_ammo])) {
								// default behavior allowed ammo to be negative
								swp->secondary_bank_ammo[bank_to_fire]--;
							} else if (!(turret->system_info->flags[Model::Subsystem_Flags::Use_multiple_guns]) && (swp->secondary_bank_ammo[bank_to_fire] < 0)) {
								swp->current_secondary_bank++;
								if (swp->current_secondary_bank >= swp->num_secondary_banks) {
									swp->current_secondary_bank = 0;
								}
								return false;
							} else {
								return false;
							}
						}
					}
				}

				vec3d firing_pos_buf;
				const vec3d *firing_pos = &firing_pos_buf;

				// zookeeper - Firepoints should cycle normally between shots, 
				// so we need to get the position info separately for each shot
				ship_get_global_turret_gun_info(&Objects[parent_objnum], turret, &firing_pos_buf, false, nullptr, true, nullptr);

				weapon_objnum = weapon_create(firing_pos, &firing_orient, turret_weapon_class, parent_objnum, -1, true, false, 0.0f, turret, launch_curve_data);
				weapon_set_tracking_info(weapon_objnum, parent_objnum, turret->turret_enemy_objnum, 1, turret->targeted_subsys);		
			
				//nprintf(("AI", "Turret_time_enemy_in_range = %7.3f\n", ss->turret_time_enemy_in_range));		
				if (weapon_objnum != -1) {
					objp = &Objects[weapon_objnum];
					wp = &Weapons[objp->instance];
					wip = &Weapon_info[wp->weapon_info_index];

					parent_ship->last_fired_turret = turret;
					turret->last_fired_weapon_info_index = wp->weapon_info_index;
					turret->turret_last_fired = _timestamp();

					wp->target_num = turret->turret_enemy_objnum;
					// AL 1-6-97: Store pointer to turret subsystem
					wp->turret_subsys = turret;	

					if (scripting::hooks::OnTurretFired->isActive()) {
						scripting::hooks::OnTurretFired->run(scripting::hooks::WeaponUsedConditions{ parent_ship , turret_enemy_objp, SCP_vector<int>{ turret_weapon_class }, wip->subtype == WP_LASER },
							scripting::hook_param_list(
								scripting::hook_param("Ship", 'o', &Objects[parent_objnum]),
								scripting::hook_param("Weapon", 'o', objp),
								scripting::hook_param("Target", 'o', turret_enemy_objp)
							));
					}

					// if the gun is a flak gun
					if (wip->wi_flags[Weapon::Info_Flags::Flak]) {

						if(predicted_pos != nullptr)
						{
							// pick a firing range so that it detonates properly			
							flak_pick_range(objp, firing_pos, predicted_pos, ship_get_subsystem_strength(parent_ship, SUBSYSTEM_WEAPONS));

							// determine what that range was
							flak_range = flak_get_range(objp);
						}
						else
						{
							flak_set_range(objp, flak_range_override);
						}
					}

					// do mflash if the weapon has it
					if (wip->muzzle_effect.isValid()) {
						float radius_mult = 1.f;
						if (wip->render_type == WRT_LASER) {
							radius_mult = wip->weapon_curves.get_output(weapon_info::WeaponCurveOutputs::LASER_RADIUS_MULT, *wp, &wp->modular_curves_instance);
						}
						//spawn particle effect
						auto particleSource = particle::ParticleManager::get()->createSource(wip->muzzle_effect);
						particleSource->setHost(make_unique<EffectHostTurret>(&Objects[parent_ship->objnum], turret->system_info->turret_gun_sobj, turret->turret_next_fire_pos));
						particleSource->setTriggerRadius(objp->radius * radius_mult);
						particleSource->setTriggerVelocity(vm_vec_mag_quick(&objp->phys_info.vel));
						particleSource->finishCreation();
					}

					// in multiplayer (and the master), then send a turret fired packet.
					if ( MULTIPLAYER_MASTER && (weapon_objnum != -1) ) {
						int subsys_index;

						subsys_index = ship_get_subsys_index(turret);
						Assert( subsys_index != -1 );
						if(wip->wi_flags[Weapon::Info_Flags::Flak]){			
							send_flak_fired_packet( parent_objnum, subsys_index, weapon_objnum, flak_range, launch_curve_data.distance_to_target, launch_curve_data.target_radius );
						} else {
							send_turret_fired_packet( parent_objnum, subsys_index, weapon_objnum, launch_curve_data.distance_to_target, launch_curve_data.target_radius );
						}
					}

					if ( play_sound && wip->launch_snd.isValid() ) {
						// Don't play turret firing sound if turret sits on player ship... it gets annoying.
						if ( parent_objnum != OBJ_INDEX(Player_obj) || (turret->flags[Ship::Subsystem_Flags::Play_sound_for_player]) ) {						
							snd_play_3d( gamesnd_get_game_sound(wip->launch_snd), firing_pos, &View_position );
						}
					}
				}
				turret->turret_next_fire_pos++;
			}
			turret->turret_next_fire_pos--;
			// reset any animations if we need to
			// (I'm not sure how accurate this timestamp would be in practice - taylor)
			if (turret->turret_animation_position == MA_POS_READY)
				turret->turret_animation_done_time = timestamp(100);
		}
	}
	//Not useful -WMC
	else if (!(parent_aip->ai_profile_flags[AI::Profile_Flags::Dont_insert_random_turret_fire_delay]) && last_shot_in_salvo)
	{
		float wait = 1000.0f * frand_range(0.9f, 1.1f);
		turret->turret_next_fire_stamp = timestamp((int) wait);
	}

	turret->flags.set(Ship::Subsystem_Flags::Has_fired); //set has fired flag for scriptng - nuke

	//Fire animation stuff
	Ship_info[parent_ship->ship_info_index].animations.get(model_get_instance(parent_ship->model_instance_num), animation::ModelAnimationTriggerType::TurretFired, animation::anim_name_from_subsys(turret->system_info))
		.start(animation::ModelAnimationDirection::FWD, true);
	return true;
}

//void turret_swarm_fire_from_turret(ship_subsys *turret, int parent_objnum, int target_objnum, ship_subsys *target_subsys)
void turret_swarm_fire_from_turret(turret_swarm_info *tsi)
{
	int weapon_objnum;
	matrix firing_orient;
	vec3d turret_pos, turret_fvec;

	// parent not alive, quick out.
	if (Objects[tsi->parent_objnum].type != OBJ_SHIP) {
		return;
	}
	
	//	if fixed fp make sure to use constant fp
	if (tsi->turret->system_info->flags[Model::Subsystem_Flags::Turret_fixed_fp])
		tsi->turret->turret_next_fire_pos = tsi->weapon_num;

	//	change firing point
	ship_get_global_turret_gun_info(&Objects[tsi->parent_objnum], tsi->turret, &turret_pos, false, &turret_fvec, true, nullptr);
	tsi->turret->turret_next_fire_pos++;

	//check if this really is a swarm. If not, how the hell did it get here?
	Assert((Weapon_info[tsi->weapon_class].wi_flags[Weapon::Info_Flags::Swarm]) || (Weapon_info[tsi->weapon_class].wi_flags[Weapon::Info_Flags::Corkscrew]));


    // *If it's a non-homer, then use the last fire direction instead of turret orientation to fix inaccuracy
    //  problems with non-homing swarm weapons -Et1
	if ( (Weapon_info[tsi->weapon_class].subtype == WP_LASER) || ((The_mission.ai_profile->flags[AI::Profile_Flags::Hack_improve_non_homing_swarm_turret_fire_accuracy]) 
																	&& !(Weapon_info[tsi->weapon_class].is_homing())) )
	{
		turret_fvec = tsi->turret->turret_last_fire_direction;
	}

	// make firing_orient from turret_fvec -- turret->turret_last_fire_direction
	vm_vector_2_matrix_norm(&firing_orient, &turret_fvec, nullptr, nullptr);

	auto launch_curve_data = WeaponLaunchCurveData {
		tsi->turret->system_info->turret_num_firing_points,
		0.f,
		0.f,
	};

	// create weapon and homing info
	weapon_objnum = weapon_create(&turret_pos, &firing_orient, tsi->weapon_class, tsi->parent_objnum, -1, true, false, 0.0f, tsi->turret, launch_curve_data);
	weapon_set_tracking_info(weapon_objnum, tsi->parent_objnum, tsi->target_objnum, 1, tsi->target_subsys);

	// do other cool stuff if weapon is created.
	if (weapon_objnum > -1) {
		Weapons[Objects[weapon_objnum].instance].turret_subsys = tsi->turret;
		Weapons[Objects[weapon_objnum].instance].target_num = tsi->turret->turret_enemy_objnum;

		Ships[Objects[tsi->parent_objnum].instance].last_fired_turret = tsi->turret;
		tsi->turret->last_fired_weapon_info_index = tsi->weapon_class;
		tsi->turret->turret_last_fired = _timestamp();

		if (scripting::hooks::OnTurretFired->isActive()) {
			scripting::hooks::OnTurretFired->run(scripting::hooks::WeaponUsedConditions{
				&Ships[Objects[tsi->parent_objnum].instance],
				&Objects[tsi->turret->turret_enemy_objnum], 
				SCP_vector<int>{ tsi->weapon_class }, Weapon_info[tsi->weapon_class].subtype == WP_LASER
				},
				scripting::hook_param_list(
					scripting::hook_param("Ship", 'o', &Objects[tsi->parent_objnum]),
					scripting::hook_param("Weapon", 'o', &Objects[weapon_objnum]),
					scripting::hook_param("Target", 'o', &Objects[tsi->turret->turret_enemy_objnum])
				));
		}

		// muzzle flash?
		if (Weapon_info[tsi->weapon_class].muzzle_effect.isValid()) {
			//spawn particle effect
			auto particleSource = particle::ParticleManager::get()->createSource(Weapon_info[tsi->weapon_class].muzzle_effect);
			particleSource->setHost(make_unique<EffectHostTurret>(&Objects[tsi->parent_objnum], tsi->turret->system_info->turret_gun_sobj, tsi->turret->turret_next_fire_pos - 1));
			particleSource->setTriggerRadius(Objects[weapon_objnum].radius);
			particleSource->finishCreation();
		}

		// maybe sound
		if ( Weapon_info[tsi->weapon_class].launch_snd.isValid() ) {
			// Don't play turret firing sound if turret sits on player ship... it gets annoying.
			if ( tsi->parent_objnum != OBJ_INDEX(Player_obj) || (tsi->turret->flags[Ship::Subsystem_Flags::Play_sound_for_player]) ) {
				snd_play_3d( gamesnd_get_game_sound(Weapon_info[tsi->weapon_class].launch_snd), &turret_pos, &View_position );
			}
		}

		// in multiplayer (and the master), then send a turret fired packet.
		if ( MULTIPLAYER_MASTER && (weapon_objnum != -1) ) {
			int subsys_index;

			subsys_index = ship_get_subsys_index(tsi->turret);
			Assert( subsys_index != -1 );
			send_turret_fired_packet( tsi->parent_objnum, subsys_index, weapon_objnum, 0.f, 0.f);
		}
	}
}

int Num_ai_firing = 0;
int Num_find_turret_enemy = 0;
int Num_turrets_fired = 0;

/**
 * Previously called ai_fire_from_turret()
 * Given a ship and a turret subsystem, handle all its behavior, mostly targeting and shooting but also 
 * all turret movement and resetting when idle
 */
extern int Nebula_sec_range;
void ai_turret_execute_behavior(const ship *shipp, ship_subsys *ss)
{
	float		weapon_firing_range;
    float		weapon_min_range;			// *Weapon minimum firing range -Et1
	vec3d	v2e;
	object	*lep;		//	Last enemy pointer
	model_subsystem	*tp = ss->system_info;
	ship_weapon *swp = &ss->weapons;
	vec3d	predicted_enemy_pos = vmd_zero_vector;
	object	*objp;

	bool in_lab = (gameseq_get_state() == GS_STATE_LAB);

	if (in_lab) {
		// Adjust behavior for Lab
		ss->turret_enemy_objnum = -1;
		lep = nullptr;
	}

	// Reset the points to target value
	ss->points_to_target = -1.0f;
	ss->base_rotation_rate_pct = 0.0f;
	ss->gun_rotation_rate_pct = 0.0f;

	if (!in_lab && !Ai_firing_enabled) {
		return;
	}

	if (ss->current_hits <= 0.0f) {
		return;
	}

	if ( ship_subsys_disrupted(ss) ){		// AL 1/19/98: Make sure turret isn't suffering disruption effects
		return;
	}

	// Check turret free
	if (!in_lab && ss->weapons.flags[Ship::Weapon_Flags::Turret_Lock]) {
		return;
	}

	// check if there is any available weapon to fire
	int weap_check;
	bool valid_weap = false;
	for (weap_check = 0; (weap_check < swp->num_primary_banks) && !valid_weap; weap_check++) {
		if (swp->primary_bank_weapons[weap_check] >= 0)
			valid_weap = true;
	}
	if (!valid_weap) {
		for (weap_check = 0; (weap_check < ss->weapons.num_secondary_banks) && !valid_weap; weap_check++) {
			if (ss->weapons.secondary_bank_weapons[weap_check] >= 0)
				valid_weap = true;
		}
		if (!valid_weap)
			return;
	}

	// Monitor number of calls to ai_fire_from_turret
	Num_ai_firing++;

	// Handle turret animation
	if (ss->turret_animation_position == MA_POS_SET) {
		if ( timestamp_elapsed(ss->turret_animation_done_time) ) {
			ss->turret_animation_position = MA_POS_READY;
			// setup a reversal (closing) timestamp at 1 second
			// (NOTE: this will probably get changed by other parts of the code (swarming, beams, etc) to be
			// a more accurate time, but we need to give it a long enough time for the other parts of the code
			// to change the timestamp before it gets acted upon - taylor)
			ss->turret_animation_done_time = timestamp(200);
		} else {
			return;
		}
	}

	// AL 09/14/97: ensure ss->turret_enemy_objnum != -1 before setting lep
	if ( (ss->turret_enemy_objnum >= 0 && ss->turret_enemy_objnum < MAX_OBJECTS) && (ss->turret_enemy_sig == Objects[ss->turret_enemy_objnum].signature) )
	{
		lep = &Objects[ss->turret_enemy_objnum];
	}
	else
	{
		ss->turret_enemy_objnum = -1;
		lep = nullptr;
		ss->flags.remove(Ship::Subsystem_Flags::Forced_target);
		ss->flags.remove(Ship::Subsystem_Flags::Forced_subsys_target);
	}

	Assert((shipp->objnum >= 0) && (shipp->objnum < MAX_OBJECTS));
	int parent_objnum = shipp->objnum;
	objp = &Objects[shipp->objnum];
	Assert(objp->type == OBJ_SHIP);

	vec3d	 global_gun_pos, global_gun_vec;
	if (tp->flags[Model::Subsystem_Flags::Turret_distant_firepoint] || Always_use_distant_firepoints) {
		//The firing point of this turret is so far away from the its center that we should consider this for firing calculations
		//This will do the enemy position prediction based on their relative position and speed to the firing point, not the turret center.
		ship_get_global_turret_gun_info(objp, ss, &global_gun_pos, false, &global_gun_vec, true, nullptr);
	} else {
		// Use the turret info for all guns, not one gun in particular.
		ship_get_global_turret_info(objp, tp, &global_gun_pos, &global_gun_vec);
	}

	if (!in_lab) {
		// Update predicted enemy position.  This used to be done in aifft_rotate_turret
		aifft_update_predicted_enemy_pos(objp, shipp, ss, &global_gun_pos, &global_gun_vec, lep, &predicted_enemy_pos);
	} else {
		// Lab hijacks this value to use as the target position
		predicted_enemy_pos = ss->last_aim_enemy_pos;
	}

	// Rotate the turret even if time hasn't elapsed, since it needs to turn to face its target.
	bool use_angles = aifft_rotate_turret(objp, shipp, ss, &global_gun_pos, &global_gun_vec, &predicted_enemy_pos);

	// Multiplayer clients are now able to try to track their targets and also reset to their idle positions.
	// But everything after this point is turret firing, so we need to bail here.
	if (MULTIPLAYER_CLIENT) {
		return;
	}

	if ((tp->flags[Model::Subsystem_Flags::Fire_on_target]) && (ss->points_to_target >= 0.0f))
	{
		// value probably needs tweaking... could perhaps be made into table option?
		if (ss->points_to_target > 0.010f)
			return;
	}

	if ( !timestamp_elapsed(ss->turret_next_fire_stamp) ) {
		return;
	}

	// Don't try to fire beyond weapon_limit_range
	// WMC moved the range check to within the loop, but we can still calculate the enemy distance here
	float base_dist_to_enemy = 0.0f;
	float dist_to_enemy = 0.0f;
	if (lep) {
		base_dist_to_enemy = vm_vec_normalized_dir(&v2e, &predicted_enemy_pos, &global_gun_pos);
		dist_to_enemy = base_dist_to_enemy;
		if (!The_mission.ai_profile->flags[AI::Profile_Flags::Turrets_ignore_target_radius]) {
			dist_to_enemy -= lep->radius;
		}
		base_dist_to_enemy = MAX(0.0f, base_dist_to_enemy);
		dist_to_enemy = MAX(0.0f, dist_to_enemy);
	} else if (in_lab) {
		base_dist_to_enemy = 500.0f;
		dist_to_enemy = base_dist_to_enemy;
		vm_vec_normalized_dir(&v2e, &predicted_enemy_pos, &global_gun_pos);
	}

	// count the number of enemies, in case we have a spawning weapon
	int num_ships_nearby = num_nearby_fighters(iff_get_attackee_mask(obj_team(objp)), &global_gun_pos, 1500.0f);

	// some flags considering there may be different weapon types on this turret
	bool we_did_non_spawning_logic = false;
	bool tentative_return = false;

	float turret_barrel_length = -1.0f;

	float target_radius = 0.f;

	if (lep != nullptr) {
		target_radius = lep->radius;
	}

	// grab the data for the launch curve inputs
	auto launch_curve_data = WeaponLaunchCurveData {
			ss->system_info->turret_num_firing_points,
			base_dist_to_enemy,
			target_radius,
	};

	//WMC - go through all valid weapons. Fire spawns if there are any.
	int valid_weapons[MAX_SHIP_WEAPONS];
	int num_valid = 0;
	for (int i = 0; i < MAX_SHIP_WEAPONS; ++i)
	{
		//WMC - Only fire more than once if we have multiple guns flag set.
		if (num_valid > 0 && !(tp->flags[Model::Subsystem_Flags::Use_multiple_guns]))
			break;

		// figure out which weapon we are firing, if we even have a valid one
		weapon_info *wip = nullptr;
		if (i < MAX_SHIP_PRIMARY_BANKS)
		{
			if (i < swp->num_primary_banks && timestamp_elapsed(swp->next_primary_fire_stamp[i]))
				wip = &Weapon_info[swp->primary_bank_weapons[i]];
		}
		else
		{
			int j = i - MAX_SHIP_PRIMARY_BANKS;
			if (j < swp->num_secondary_banks && timestamp_elapsed(swp->next_secondary_fire_stamp[j]))
				wip = &Weapon_info[swp->secondary_bank_weapons[j]];
		}
		if (!wip)
			continue;

		// Turret spawn weapons are a special case.  They fire if there are enough enemies in the 
		// immediate area (not necessarily in the turret fov).
		// However, if this is a 'smart spawn', just use it like a normal weapon
		if (!in_lab && (wip->wi_flags[Weapon::Info_Flags::Spawn]) && !(wip->wi_flags[Weapon::Info_Flags::Smart_spawn]) )
		{
			if (( num_ships_nearby >= 3 ) || ((num_ships_nearby >= 2) && (frand() < 0.1f))) {
				turret_fire_weapon(i, ss, parent_objnum, launch_curve_data, &global_gun_pos, &ss->turret_last_fire_direction);
			} else {
				//	Regardless of firing rate, don't check whether should fire for awhile.
				if (tp->flags[Model::Subsystem_Flags::Use_multiple_guns])
					set_turret_weapon_next_fire_stamp(swp, i, 1000);
				else
					ss->turret_next_fire_stamp = timestamp(1000);
			}

			//we're done with this weapon mount
			continue;
		}
		we_did_non_spawning_logic = true;	// for the post-loop check

		// If beam weapon, check beam free
		if (!in_lab && wip->wi_flags[Weapon::Info_Flags::Beam] && !(ss->weapons.flags[Ship::Weapon_Flags::Beam_Free])) {
			tentative_return = true;
			continue;
		}

		// the remaining checks in this loop all require a valid lep
		if (!in_lab && !lep)
			continue;

		// Get weapon range
		if (wip->wi_flags[Weapon::Info_Flags::Local_ssm])
		{
			weapon_firing_range = wip->lssm_lock_range;
			weapon_min_range = 0.0f;
		}
		else
		{
			weapon_firing_range = MIN(wip->lifetime * wip->max_speed, wip->weapon_range);
			weapon_min_range = wip->weapon_min_range;
		}

		// if beam weapon in nebula and target not tagged, decrease firing range
		if (wip->wi_flags[Weapon::Info_Flags::Beam]) {
			if ( !((shipp->tag_left > 0) || (shipp->level2_tag_left > 0)) ) {
				if (Nebula_sec_range) {
					weapon_firing_range *= BEAM_NEBULA_RANGE_REDUCE_FACTOR;
				}
			}
		}

		// If the barrel of this turret is long, we shouldn't try to fire if the enemy is under the gun.
		// Note that this isn't the same thing as min_range, which is unrelated to barrel length.
		if (tp->flags[Model::Subsystem_Flags::Turret_distant_firepoint] || Always_use_distant_firepoints) {
			if (turret_barrel_length < 0.0f) {
				if (tp->turret_gun_sobj >= 0) {
					auto pm = model_get(tp->model_num);
					auto submodel = &pm->submodel[tp->turret_gun_sobj];
					turret_barrel_length = 2 * submodel->rad;
				} else {
					turret_barrel_length = 0.0f;
				}
			}
			if (dist_to_enemy < turret_barrel_length) continue;
		}

		// Don't try to fire beyond weapon_limit_range (or within min range)
		// In lab we ignore range issues
		if (!in_lab && (dist_to_enemy < weapon_min_range || dist_to_enemy > weapon_firing_range)) {
			// it's possible another weapon is in range, but if not,
			// we will end up selecting a new target
			continue;
		}

		if (!in_lab) {

			ship_info* esip = lep->type != OBJ_SHIP ? nullptr : &Ship_info[Ships[lep->instance].ship_info_index];

			//	If targeted a small ship and have a huge weapon, don't fire.  But this shouldn't happen, as a small ship should not get selected.
			if ( wip->wi_flags[Weapon::Info_Flags::Huge] ) {
				if ( lep->type != OBJ_SHIP ) {
						tentative_return = true;
						continue;
					}
				if ( esip->class_type >= 0 && !(Ship_types[esip->class_type].flags[Ship::Type_Info_Flags::Targeted_by_huge_Ignored_by_small_only])) {
						tentative_return = true;
						continue;
					}
				}

				// Similar check for small weapons
			if ( wip->wi_flags[Weapon::Info_Flags::Small_only] ) {
				if ( (lep->type == OBJ_SHIP) && esip->class_type >= 0 && 
					(Ship_types[esip->class_type].flags[Ship::Type_Info_Flags::Targeted_by_huge_Ignored_by_small_only])) {
						tentative_return = true;
						continue;
					}
				}

			bool tagged_only = ((wip->wi_flags[Weapon::Info_Flags::Tagged_only]) || (ss->weapons.flags[Ship::Weapon_Flags::Tagged_Only]));

			// If targeting protected or beam protected ship, don't fire.  Reset enemy objnum
			// (note: due to the loop, we need to check all weapons on this turret.  if none of them are valid, that's when the enemy objnum is reset)
			if (lep->type == OBJ_SHIP) {
				// Check if we're targeting a protected ship
				if (lep->flags[Object::Object_Flags::Protected]) {
					// *none* of the weapons will work on this ship, so just quit the loop
					tentative_return = true;
					break;
				}

				// Check if we're targeting a beam protected ship with a beam weapon
				if ( (lep->flags[Object::Object_Flags::Beam_protected]) && (wip->wi_flags[Weapon::Info_Flags::Beam]) ) {
					tentative_return = true;
					continue;
				}
				// Check if we're targeting a flak protected ship with a flak weapon
				else if ( (lep->flags[Object::Object_Flags::Flak_protected]) && (wip->wi_flags[Weapon::Info_Flags::Flak]) ) {
					tentative_return = true;
					continue;
				}
				// Check if we're targeting a laser protected ship with a laser weapon
				else if ( (lep->flags[Object::Object_Flags::Laser_protected]) && (wip->subtype == WP_LASER) ) {
					tentative_return = true;
					continue;
				}
				// Check if we're targeting a missile protected ship with a missile weapon
				else if ( (lep->flags[Object::Object_Flags::Missile_protected]) && (wip->subtype == WP_MISSILE) ) {
					tentative_return = true;
					continue;
				}
				// Check if weapon or turret is set to tagged-only
				// must check here in case turret has multiple weapons and not all are tagged-only
				else if (tagged_only && !ship_is_tagged(lep)) {
					tentative_return = true;
					continue;
				}

				if (!weapon_target_satisfies_lock_restrictions(wip, lep)) {
					tentative_return = true;
					continue;
				}
			}
			else {
				// check tagged-only for bombs only if the flag is set; see Mantis #3114
				if (tagged_only && ((lep->type != OBJ_WEAPON) || The_mission.ai_profile->flags[AI::Profile_Flags::Strict_turret_tagged_only_targeting])) {
					tentative_return = true;
					continue;
				}
			}
		}

		// If we made it this far, we have a valid weapon to fire
		valid_weapons[num_valid++] = i;
	}

	// in the original code, spawning weapons immediately bailed after firing,
	// so bail unless we have other weapons that need the rest of the function
	if (!we_did_non_spawning_logic)
		return;

	//none of our guns can hit the enemy, so find a new enemy
	if (num_valid == 0) {
		if (!ss->flags[Ship::Subsystem_Flags::Forced_target]) {
			ss->turret_enemy_objnum = -1;
			ss->turret_time_enemy_in_range = 0.0f;
		}

		// in the original code, we returned where the "tentative return" variable was set,
		// so return for real now, since we don't have a reason to remain in the function
		if (tentative_return)
			return;
	}

	// if we've got Forced_subsys_target targeted_subsys *really* shouldnt be null but just in case...
	if (ss->flags[Ship::Subsystem_Flags::Forced_subsys_target] && ss->targeted_subsys != nullptr) {
		if (ss->targeted_subsys->current_hits <= 0.0f) {
			ss->flags.remove(Ship::Subsystem_Flags::Forced_subsys_target);
			ss->flags.remove(Ship::Subsystem_Flags::Forced_target);
		}
	}

	//	Maybe pick a new enemy, unless targeting has been taken over by scripting
	if ( turret_should_pick_new_target(ss) && !ss->scripting_target_override && !(ss->flags[Ship::Subsystem_Flags::Forced_target])) {
		Num_find_turret_enemy++;
		int objnum = find_turret_enemy(ss, parent_objnum, &global_gun_pos, &global_gun_vec, ss->turret_enemy_objnum);

		if (objnum >= 0) {
			if (ss->turret_enemy_objnum == -1) {
				ss->turret_enemy_objnum = objnum;
				ss->targeted_subsys = NULL;		// Turret has retargeted; reset subsystem - Valathil for Mantis #2652
				ss->turret_enemy_sig = Objects[objnum].signature;
				// why return?
				return;
			} else {
				ss->turret_enemy_objnum = objnum;
				ss->targeted_subsys = NULL;		// Turret has retargeted; reset subsystem - Valathil for Mantis #2652
				ss->turret_enemy_sig = Objects[objnum].signature;
			}
		} else {
			ss->turret_enemy_objnum = -1;
		}

		if (ss->turret_enemy_objnum != -1) {
			float	dot = 1.0f;
			lep = &Objects[ss->turret_enemy_objnum];
			if (( lep->type == OBJ_SHIP ) && !(ss->flags[Ship::Subsystem_Flags::No_SS_targeting])) {
				ss->targeted_subsys = aifft_find_turret_subsys(objp, ss, &global_gun_pos, lep, &dot);
			}
			// recheck in 2-3 seconds
			ss->turret_next_enemy_check_stamp = timestamp((int)((MAX(dot, 0.5f) * The_mission.ai_profile->turret_target_recheck_time) + (The_mission.ai_profile->turret_target_recheck_time / 2.0f)));
		} else {
			ss->turret_next_enemy_check_stamp = timestamp((int)(The_mission.ai_profile->turret_target_recheck_time * frand_range(0.9f, 1.1f)));	//	Check every two seconds
		}
	}

	//	If still don't have an enemy, return.  Or, if enemy is protected, return.
	if (ss->turret_enemy_objnum != -1) {
		// Don't shoot at ship we're docked with.
		if (dock_check_find_docked_object(objp, &Objects[ss->turret_enemy_objnum]))
		{
			ss->turret_enemy_objnum = -1;
			return;
		}

		// Goober5000 - Also, don't shoot at a ship we're docking with.  Volition
		// had this in the code originally but messed it up so that it didn't work.
		ai_info *aip = &Ai_info[shipp->ai_index];
		if ((aip->mode == AIM_DOCK) && (aip->goal_objnum == ss->turret_enemy_objnum))
		{
			ss->turret_enemy_objnum = -1;
			return;
		}

		if (Objects[ss->turret_enemy_objnum].flags[Object::Object_Flags::Protected]) {
			//	This can happen if the enemy was selected before it became protected.
			ss->turret_enemy_objnum = -1;
			return;
		}

		lep = &Objects[ss->turret_enemy_objnum];
	} else {
		if (!in_lab) {
			if (timestamp_until(ss->turret_next_fire_stamp) < 500) {
				ss->turret_next_fire_stamp = timestamp(500);
			}
			return;
		}
	}

	//This can't happen. See above code -- Except in the Lab as of 4/26/2025
	//if ( lep == nullptr ){
	//	return;
	//}

	//This can't happen. See above code  -- Except in the Lab as of 4/26/2025
	//Assert(ss->turret_enemy_objnum != -1);

	bool in_fov = turret_fov_test(ss, &global_gun_vec, &v2e);
	bool something_was_ok_to_fire = false;

	// the only reason predicted_enemy_pos might be zero, while everything else is valid is if
	// its a ballistic trajectory with no valid path
	if (in_lab || (in_fov && num_valid && predicted_enemy_pos != vmd_zero_vector)) {

		// Do salvo thing separately - to prevent messing up things
		int number_of_firings;
		if (tp->flags[Model::Subsystem_Flags::Turret_salvo])
		{
			number_of_firings = tp->turret_num_firing_points;
			ss->turret_next_fire_pos = 0;
		}
		else
		{
			number_of_firings = num_valid;
		}

		auto sound_played = gamesnd_id();

		for (int i = 0; i < number_of_firings; ++i)
		{
			int valid_index;
			if (tp->flags[Model::Subsystem_Flags::Turret_salvo])
				valid_index = 0;
			else
				valid_index = i;
			auto wip = get_turret_weapon_wip(&ss->weapons, valid_weapons[valid_index]);

			bool play_sound = false;

			if (sound_played != wip->launch_snd) {
				sound_played = wip->launch_snd;
				play_sound = true;
			}

			// Forces the firing turret to retain the firingpoints of their weapons:
			// if the turret has multiple banks, the first bank will only fire from the first firepoint,
			// the second bank only from the second firepoint, and so on.
			if (tp->flags[Model::Subsystem_Flags::Turret_fixed_fp])
			{
				int ffp_bank = valid_weapons[valid_index];

				if (ffp_bank < MAX_SHIP_PRIMARY_BANKS)
					ss->turret_next_fire_pos = ffp_bank;
				else
					ss->turret_next_fire_pos = ffp_bank - MAX_SHIP_PRIMARY_BANKS + ss->weapons.num_primary_banks;
			}

			// Ok, the turret is lined up... now line up a particular gun.
			bool ok_to_fire = false;

			// We're ready to fire... now get down to specifics, like where is the
			// actual gun point and normal, not just the one for whole turret.
			ship_get_global_turret_gun_info(objp, ss, &global_gun_pos, false, &global_gun_vec, use_angles, &predicted_enemy_pos);

			// Fire in the direction the turret is facing, not right at the target regardless of turret dir.
			// [Yet this retail comment precedes the calculation of vector-to-enemy...]
			dist_to_enemy = vm_vec_normalized_dir(&v2e, &predicted_enemy_pos, &global_gun_pos);
			float dot = vm_vec_dot(&v2e, &global_gun_vec);

			// (flak jitter moved to after we obtain shoot_vector below)

			// Fire if:
			//		dumbfire and nearly pointing at target.
			//		heat seeking and target in a fairly wide cone.
			//		aspect seeking and target is locked.
			bool in_sight = false;
			
			if (The_mission.ai_profile->flags[AI::Profile_Flags::Use_only_single_fov_for_turrets]) {
				// we have already passed the FOV test of the turret so...
				in_sight = true;
			} else {
				if (wip->wi_flags[Weapon::Info_Flags::Homing_heat]) {
					if (dot > AICODE_TURRET_HEATSEEK_ANGLE) {
						in_sight = true;
					}
				} else {
					if (dot > AICODE_TURRET_DUMBFIRE_ANGLE) {
						in_sight = true;
					}
				}
			}

			// if dumbfire (lasers and non-homing missiles)
			if ( !(wip->is_homing()) )
			{
				if ((dist_to_enemy < 75.0f) || in_sight)
				{
					turret_update_enemy_in_range(ss, 2*wip->fire_wait);
					ok_to_fire = true;
				}
			}
			// if heat seekers
			else if ( wip->wi_flags[Weapon::Info_Flags::Homing_heat] )
			{
				if ((dist_to_enemy < 50.0f) || in_sight)
				{
					turret_update_enemy_in_range(ss, 2*wip->fire_wait);
					ok_to_fire = true;
				}
			}
			// if aspect seeker
			else if ( wip->wi_flags[Weapon::Info_Flags::Homing_aspect] )
			{
				if ((dist_to_enemy < 50.0f) || in_sight)
				{
					turret_update_enemy_in_range(ss, 2*wip->fire_wait);
				}
				if ( turret_should_fire_aspect(ss, wip, in_sight) )
				{
					ok_to_fire = true;
				}
			}
			// if javelin heat seeker
			else if ( wip->wi_flags[Weapon::Info_Flags::Homing_javelin] )
			{
				if ((dist_to_enemy < 50.0f) || in_sight)
				{
					turret_update_enemy_in_range(ss, 2*wip->fire_wait);
				}
				// Check if turret should fire and enemy's engines are
				// in line of sight
				if (turret_should_fire_aspect(ss, wip, in_sight) &&
					ship_get_closest_subsys_in_sight(&Ships[lep->signature], SUBSYSTEM_ENGINE, &global_gun_pos))
				{
					ok_to_fire = true;
				}
			}

			// maybe check that the gun vector doesn't intersect with the hull
			if ( ok_to_fire && (tp->flags[Model::Subsystem_Flags::Turret_hull_check]) ) {
				int model_num = Ship_info[shipp->ship_info_index].model_num;
				vec3d end;
				vm_vec_scale_add(&end, &global_gun_pos, &global_gun_vec, model_get_radius(model_num));

				mc_info hull_check;
				hull_check.model_instance_num = shipp->model_instance_num;
				hull_check.model_num = model_num;
				hull_check.orient = &objp->orient;
				hull_check.pos = &objp->pos;
				hull_check.p0 = &global_gun_pos;
				hull_check.p1 = &end;
				hull_check.flags = MC_CHECK_MODEL | MC_CHECK_RAY;

				if ( model_collide(&hull_check) ) {
					ok_to_fire = false;
				}
			}

			if ( ok_to_fire )
			{
				// starting animation checks
				if (ss->turret_animation_position == MA_POS_NOT_SET) {
					//For legacy animations using subtype for turret number
					auto animList = Ship_info[shipp->ship_info_index].animations.getAll(model_get_instance(shipp->model_instance_num), animation::ModelAnimationTriggerType::TurretFiring, ss->system_info->subobj_num, true);
					//For modern animations using proper triggered-by-subsys name
					animList += Ship_info[shipp->ship_info_index].animations.get(model_get_instance(shipp->model_instance_num), animation::ModelAnimationTriggerType::TurretFiring, animation::anim_name_from_subsys(ss->system_info));
					
					if (animList.start(animation::ModelAnimationDirection::FWD)) {
						ss->turret_animation_done_time = timestamp(animList.getTime());
						ss->turret_animation_position = MA_POS_SET;
					}
				}

				//Wait until the animation is done to actually fire
				if (tp->flags[Model::Subsystem_Flags::Turret_anim_wait] && (ss->turret_animation_position != MA_POS_READY))
				{
					ok_to_fire = false;
				}
			}

			if ( ok_to_fire )
			{
				// finally!
				something_was_ok_to_fire = true;
				Num_turrets_fired++;

				// (Despite the retail comment above, retail actually fired toward the enemy; v2e = vector-to-enemy)
				vec3d shoot_vector = v2e;
				if (in_lab || tp->flags[Model::Subsystem_Flags::Fire_on_normal])
					shoot_vector = global_gun_vec;

				// In the lab the order of operations is a little fuzzy since we're faking targets so always fire along the turret's current vector no matter what if we're a multipart turret
				if (in_lab && (ss->system_info->turret_gun_sobj >= 0 && ss->system_info->subobj_num != ss->system_info->turret_gun_sobj)) {
					vec3d new_pos;
					vec3d new_vec;
					ship_get_global_turret_gun_info(objp, ss, &new_pos, false, &new_vec, true, nullptr);
					shoot_vector = new_vec;
					vm_vec_normalize(&shoot_vector);
					global_gun_pos = new_pos;
					predicted_enemy_pos = new_pos + new_vec * 500.0f;
				}

				// if the weapon is a flak gun, add some jitter to its aim so it fires in a "cone" 
				// to make a cool visual effect and make them less lethal
				if (wip->wi_flags[Weapon::Info_Flags::Flak]) {
					flak_jitter_aim(&shoot_vector, dist_to_enemy, ship_get_subsystem_strength(shipp, SUBSYSTEM_WEAPONS), wip);
				}

				turret_fire_weapon(valid_weapons[valid_index], ss, parent_objnum, launch_curve_data, &global_gun_pos, &shoot_vector, &predicted_enemy_pos, 100.0f, play_sound);
			}
			else
			{
				turret_update_enemy_in_range(ss, -4*wip->fire_wait);
				if (tp->flags[Model::Subsystem_Flags::Use_multiple_guns])
					set_turret_weapon_next_fire_stamp(swp, valid_weapons[valid_index], 500);
				else
					ss->turret_next_fire_stamp = timestamp(500);

				// make sure salvo fire mode does not turn into autofire
				if ((tp->flags[Model::Subsystem_Flags::Turret_salvo]) && ((i + 1) == number_of_firings)) {
					ai_info *parent_aip = &Ai_info[shipp->ai_index];
					turret_set_next_fire_timestamp(valid_weapons[valid_index], wip, ss, parent_aip, launch_curve_data);
				}
			}

			// we are done firing, so fire the next weapon from the next firing point
			// (This was originally done under the "We're ready to fire..." comment)
			ss->turret_next_fire_pos++;
		}

		if (!something_was_ok_to_fire)
		{
			// If nothing is OK to fire (lost track of the target?) 
			// reset the target (so we don't continue to track what we can't hit)
			if (tp->flags[Model::Subsystem_Flags::Turret_only_target_if_can_fire] && !(ss->flags[Ship::Subsystem_Flags::Forced_target]))
			{
				ss->turret_enemy_objnum = -1;		//	Reset enemy objnum, find a new one next frame.
				ss->turret_time_enemy_in_range = 0.0f;
			}
		}
		else
		{
			//something did fire, get the lowest valid timestamp
			// don't do this if we already set a turret timestamp previously in the function
			if (timestamp_since(ss->turret_next_fire_stamp) >= 0)
			{
				int minimum_stamp = -1;

				for (int i = 0; i < MAX_SHIP_WEAPONS; ++i)
			{
					int stamp = (i < MAX_SHIP_PRIMARY_BANKS) ? swp->next_primary_fire_stamp[i] : swp->next_secondary_fire_stamp[i - MAX_SHIP_PRIMARY_BANKS];

					// valid timestamps start at 2; stamp must be in the future
					if (stamp < 2 || timestamp_since(stamp) >= 0)
						continue;

					// find minimum
					if (minimum_stamp < 0 || timestamp_until(stamp) < timestamp_until(minimum_stamp))
						minimum_stamp = stamp;
				}

				// set turret timestamp, or 100ms for sanity's sake
				if (minimum_stamp >= 0)
					ss->turret_next_fire_stamp = minimum_stamp;
				else
					ss->turret_next_fire_stamp = timestamp(100);
			}
		}
	}
	else if (!(ss->flags[Ship::Subsystem_Flags::Forced_target]))
	{
		// Lost him!
		ss->turret_enemy_objnum = -1;		//	Reset enemy objnum, find a new one next frame.
		ss->turret_time_enemy_in_range = 0.0f;
	}
}

bool turret_std_fov_test(const ship_subsys *ss, const vec3d *gvec, const vec3d *v2e, float size_mod)
{
	model_subsystem *tp = ss->system_info;
	float dot = vm_vec_dot(v2e, gvec);
	if (((dot + size_mod) >= tp->turret_fov) && ((dot - size_mod) <= tp->turret_max_fov))
		return true;
	
	return false;
}

bool turret_adv_fov_test(const ship_subsys *ss, const vec3d *gvec, const vec3d *v2e, float size_mod)
{
	model_subsystem *tp = ss->system_info;
	float dot = vm_vec_dot(v2e, gvec);
	if (((dot + size_mod) >= tp->turret_fov) && ((dot - size_mod) <= tp->turret_max_fov)) {
		object* objp = &Objects[ss->parent_objnum];
		matrix turret_matrix;
		turret_instance_find_world_orient(&turret_matrix, Ships[objp->instance].model_instance_num, tp->subobj_num, &objp->orient);

		vec3d of_dst;
		vm_vec_rotate(&of_dst, v2e, &turret_matrix);
		if ((of_dst.xyz.x == 0) && (of_dst.xyz.z == 0)) 
			return true; 
		else {
			of_dst.xyz.y = 0;
			if (!IS_VEC_NULL_SQ_SAFE(&of_dst)) {
				vm_vec_normalize(&of_dst);
				// now we have 2d vector with lenght of 1 that points at the targets direction after being rotated to turrets FOR
				if ((of_dst.xyz.z + size_mod) >= tp->turret_base_fov)
					return true;
			}
		}
	}
	return false;
}

bool turret_fov_test(const ship_subsys *ss, const vec3d *gvec, const vec3d *v2e, float size_mod)
{
	bool in_fov = false;
	if (ss->system_info->turret_base_fov > -1.0f)
		in_fov = turret_adv_fov_test(ss, gvec, v2e, size_mod);
	else
		in_fov = turret_std_fov_test(ss, gvec, v2e, size_mod);

	return in_fov;
}

float get_adjusted_turret_rof(ship_subsys *ss)
{
	float tempf = ss->rof_scaler;

	// optional reset switch (negative value)
	if (tempf < 0) {
		ss->rof_scaler = 1.0f;
		return 1.0f;
	}

	if (tempf == 0) {
		// special case returning the number of firingpoints
		ss->rof_scaler = (float) ss->system_info->turret_num_firing_points;
		tempf = ss->rof_scaler;

		// safety check to avoid div/0 issues
		if (tempf == 0) {
			ss->rof_scaler = 1.0f;
			return 1.0f;
		}
	}

	return tempf;
}

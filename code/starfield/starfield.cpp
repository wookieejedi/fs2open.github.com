/*
 * Copyright (C) Volition, Inc. 1999.  All rights reserved.
 *
 * All source code herein is the property of Volition, Inc. You may not sell 
 * or otherwise commercially exploit the source or things you created based on the 
 * source.
 *
*/



#include "globalincs/pstypes.h"
#include <climits>
#include <string>

#include "freespace.h"
#include "cmdline/cmdline.h"
#include "debugconsole/console.h"
#include "graphics/matrix.h"
#include "graphics/paths/PathRenderer.h"
#include "hud/hud.h"
#include "hud/hudtarget.h"
#include "io/timer.h"
#include "lighting/lighting.h"
#include "math/vecmat.h"
#include "mission/missionparse.h"
#include "model/modelrender.h"
#include "nebula/neb.h"
#include "options/Option.h"
#include "osapi/dialogs.h"
#include "parse/parselo.h"
#include "render/3d.h"
#include "render/batching.h"
#include "starfield/nebula.h"
#include "starfield/starfield.h"
#include "starfield/supernova.h"
#include "tracing/tracing.h"
#include "utils/Random.h"

#define DEBRIS_ROT_MIN				10000
#define DEBRIS_ROT_RANGE			8
#define DEBRIS_ROT_RANGE_SCALER		10000
#define RND_MAX_MASK				0x3fff
#define HALF_RND_MAX				0x2000
#define MAX_MOTION_DEBRIS			300

typedef struct {
	vec3d pos;
	int vclip;
	float size;
} motion_debris_instance;

const float MAX_DIST_RANGE = 80.0f;
const float MIN_DIST_RANGE = 14.0f;
const float BASE_SIZE = 0.04f;
const float BASE_SIZE_NEB = 0.15f;

static int Subspace_model_inner = -1;
static int Subspace_model_outer = -1;
static int Rendering_to_env = 0;

int Num_stars = 500;

// A timestamp for animated skyboxes -MageKing17
TIMESTAMP Skybox_timestamp;

#define MAX_FLARE_COUNT 10
#define MAX_FLARE_BMP 6

typedef struct flare_info {
	float pos;
	float scale;
	int tex_num;
} flare_info;

typedef struct flare_bitmap {
	char filename[MAX_FILENAME_LEN];
	int bitmap_id;
} flare_bitmap;


// global info (not individual instances)
typedef struct starfield_bitmap {
	char filename[MAX_FILENAME_LEN];				// bitmap filename
	char glow_filename[MAX_FILENAME_LEN];			// only for suns
	int bitmap_id;									// bitmap handle
	int n_frames;
	int fps;
	int glow_bitmap;								// only for suns
	int glow_n_frames;
	int glow_fps;
	int xparent;
	float r, g, b, i;		// only for suns
	int glare;										// only for suns
	int flare;										// Is there a lens-flare for this sun?
	flare_info flare_infos[MAX_FLARE_COUNT];		// each flare can use a texture in flare_bmp, with different scale
	flare_bitmap flare_bitmaps[MAX_FLARE_BMP];		// bitmaps for different lens flares (can be re-used)
	int n_flares;									// number of flares actually used
	int n_flare_bitmaps;							// number of flare bitmaps available
	int used_this_level;
	int preload;
} starfield_bitmap;

// starfield bitmap instance
typedef struct starfield_bitmap_instance {
	float scale_x, scale_y;							// x and y scale
	int div_x, div_y;								// # of x and y divisions
	angles ang;										// angles from FRED
	int star_bitmap_index;							// index into starfield_bitmap array
	int n_verts;
	vertex *verts;

	starfield_bitmap_instance() : scale_x(1.0f), scale_y(1.0f), div_x(1), div_y(1), star_bitmap_index(0), n_verts(0), verts(NULL) {
		ang.p = 0.0f;
		ang.b = 0.0f;
		ang.h = 0.0f;
    }
} starfield_bitmap_instance;

// for drawing cool stuff on the background - comes from a table
static SCP_vector<starfield_bitmap> Starfield_bitmaps;
static SCP_vector<starfield_bitmap_instance> Starfield_bitmap_instances;

// sun bitmaps and sun glow bitmaps
static SCP_vector<starfield_bitmap> Sun_bitmaps;
static SCP_vector<starfield_bitmap_instance> Suns;

// Goober5000
int Cur_background = -1;
SCP_vector<background_t> Backgrounds;

SCP_vector<int> Preload_background_indexes;
void stars_preload_background(int background_idx);

int last_stars_filled = 0;
color star_colors[8];
color star_aacolors[8];

typedef struct vDist {
	int x;
	int y;
} vDist;

std::unique_ptr<star[]> Stars = make_unique<star[]>(MAX_STARS);

motion_debris_instance Motion_debris[MAX_MOTION_DEBRIS];

SCP_vector<motion_debris_types> Motion_debris_info;
motion_debris_bitmaps* Motion_debris_ptr = nullptr;

// background data
int Stars_background_inited = 0;			// if we're inited
int Nmodel_num = -1;							// model num
int Nmodel_instance_num = -1;					// model instance num
matrix Nmodel_orient = IDENTITY_MATRIX;			// model orientation
uint64_t Nmodel_flags = DEFAULT_NMODEL_FLAGS;		// model flags
int Nmodel_bitmap = -1;						// model texture
float Nmodel_alpha = 1.0f;					// model transparency

bool Dynamic_environment = false;

bool Motion_debris_override = false;
bool Motion_debris_enabled = true;

static void parse_motion_debris_func()
{
	bool enabled;
	stuff_boolean(&enabled);
	Motion_debris_enabled = enabled;
}

auto MotionDebrisOption = options::OptionBuilder<bool>("Graphics.MotionDebris",
                     std::pair<const char*, int>{"Motion Debris", 1713},
                     std::pair<const char*, int>{"Enable or disable visible motion debris", 1714})
                     .category(std::make_pair("Graphics", 1825))
                     .bind_to(&Motion_debris_enabled)
                     .default_func([]() { return Motion_debris_enabled;})
                     .level(options::ExpertLevel::Advanced)
                     .importance(67)
                     .parser(parse_motion_debris_func)
                     .finish();

static int Default_env_map = -1;
static int Mission_env_map = -1;
static bool Env_cubemap_drawn = false;
static bool Irr_cubemap_drawn = false;

int get_motion_debris_by_name(const SCP_string &name)
{
	int count = static_cast<int>(Motion_debris_info.size());
	for (int i = 0; i < count; i++) {
		if (lcase_equal(Motion_debris_info[i].name, name)) {
			return i;
		}
	}

	return -1;
}

void stars_release_motion_debris(motion_debris_bitmaps* vclips)
{

	if (vclips == nullptr)
		return;

	for (int i = 0; i < MAX_MOTION_DEBRIS_BITMAPS; i++) {
		if ( (vclips[i].bm >= 0) && bm_release(vclips[i].bm) ) {
			vclips[i].bm = -1;
			vclips[i].nframes = -1;
		}
	}
}

void stars_load_motion_debris(motion_debris_bitmaps* vclips)
{

	Assertion(vclips != nullptr, "Motion debris not loaded!");

	for (int i = 0; i < MAX_MOTION_DEBRIS_BITMAPS; i++) {
		vclips[i].bm = bm_load_animation( vclips[i].name, &vclips[i].nframes, nullptr, nullptr, nullptr, true );

		if ( vclips[i].bm < 0 ) {
			// try loading it as a single bitmap
			vclips[i].bm = bm_load(Motion_debris_ptr[i].name);
			vclips[i].nframes = 1;

			if (vclips[i].bm <= 0) {
				Error( LOCATION, "Couldn't load animation/bitmap '%s'\n", vclips[i].name );
			}
		}
	}
}

void stars_load_debris(int fullneb, const SCP_string &custom_name)
{
	if (!Motion_debris_enabled || Motion_debris_info.empty()) {
		return;
	}

	SCP_string debris_name = "";
	
	if (!custom_name.empty()) {
		debris_name = custom_name;
	}

	// if not using custom the load the default type for the mission
	if (fullneb && debris_name.empty()) { // if we're in nebula mode
		debris_name = "nebula";
	} else if (debris_name.empty()) {
		debris_name = "default";
	}

	int debris_index = get_motion_debris_by_name(debris_name);

	//If we can't find the motion debris that's been called for then warn and
	//set debris override to prevent downstream errors. Then abort.
	if (debris_index < 0) {
		Warning(LOCATION, "Motion debris '%s' not found in stars.tbl!\n", debris_name.c_str());
		Motion_debris_override = true;
		Motion_debris_ptr = nullptr;
		return;
	}

	stars_release_motion_debris(Motion_debris_ptr);
	stars_load_motion_debris(Motion_debris_info[debris_index].bitmaps);
	Motion_debris_ptr = Motion_debris_info[debris_index].bitmaps;
}

const int MAX_PERSPECTIVE_DIVISIONS = 5;
const float p_phi = 10.0f, p_theta = 10.0f;

extern void stars_project_2d_onto_sphere( vec3d *pnt, float rho, float phi, float theta );

static void starfield_create_bitmap_buffer(const size_t si_idx)
{
	vec3d s_points[MAX_PERSPECTIVE_DIVISIONS+1][MAX_PERSPECTIVE_DIVISIONS+1];

	vertex v[4];
	matrix m;
	int idx, s_idx;
	float ui, vi;

	starfield_bitmap_instance *sbi = &Starfield_bitmap_instances[si_idx];

	angles *a = &sbi->ang;
	float scale_x = sbi->scale_x;
	float scale_y = sbi->scale_y;
	int div_x = sbi->div_x;
	int div_y = sbi->div_y;

	// cap division values
//	div_x = div_x > MAX_PERSPECTIVE_DIVISIONS ? MAX_PERSPECTIVE_DIVISIONS : div_x;
	div_x = 1;
	div_y = div_y > MAX_PERSPECTIVE_DIVISIONS ? MAX_PERSPECTIVE_DIVISIONS : div_y;

	if (sbi->verts != NULL) {
		delete [] sbi->verts;
	}

	sbi->verts = new(std::nothrow) vertex[div_x * div_y * 6];

	if (sbi->verts == NULL) {
		sbi->star_bitmap_index = -1;
		return;
	}

	sbi->n_verts = div_x * div_y * 6;

	// texture increment values
	ui = 1.0f / (float)div_x;
	vi = 1.0f / (float)div_y;	

	// adjust for aspect ratio
	//scale_x *= (gr_screen.clip_aspect + 0.55f); // fudge factor
	//scale_x *= (gr_screen.clip_aspect + (0.7333333f/gr_screen.clip_aspect)); // fudge factor
	scale_x *= 1.883333f; //fudge factor

	float s_phi = 0.5f + (((p_phi * scale_x) / 360.0f) / 2.0f);
	float s_theta = (((p_theta * scale_y) / 360.0f) / 2.0f);
	float d_phi = -(((p_phi * scale_x) / 360.0f) / (float)(div_x));
	float d_theta = -(((p_theta * scale_y) / 360.0f) / (float)(div_y));

	// bank matrix
	vm_angles_2_matrix(&m, a);

	// generate the bitmap points
	for(idx=0; idx<=div_x; idx++) {
		for(s_idx=0; s_idx<=div_y; s_idx++) {
			// get world spherical coords
			stars_project_2d_onto_sphere(&s_points[idx][s_idx], 1000.0f, s_phi + ((float)idx*d_phi), s_theta + ((float)s_idx*d_theta));

			// (un)rotate on the sphere
			vm_vec_unrotate(&s_points[idx][s_idx], &s_points[idx][s_idx], &m);
		}
	}

	memset(v, 0, sizeof(vertex) * 4);

	int j = 0;

	vertex *verts = sbi->verts;

	for (idx = 0; idx < div_x; idx++) {
		for (s_idx = 0; s_idx < div_y; s_idx++) {
			// stuff texture coords
			v[0].texture_position.u = ui * float(idx);
			v[0].texture_position.v = vi * float(s_idx);

			v[1].texture_position.u = ui * float(idx+1);
			v[1].texture_position.v = vi * float(s_idx);

			v[2].texture_position.u = ui * float(idx+1);
			v[2].texture_position.v = vi * float(s_idx+1);

			v[3].texture_position.u = ui * float(idx);
			v[3].texture_position.v = vi * float(s_idx+1);

			g3_transfer_vertex(&v[0], &s_points[idx][s_idx]);
			g3_transfer_vertex(&v[1], &s_points[idx+1][s_idx]);
			g3_transfer_vertex(&v[2], &s_points[idx+1][s_idx+1]);
			g3_transfer_vertex(&v[3], &s_points[idx][s_idx+1]);

			// poly 1
			verts[j++] = v[0];
			verts[j++] = v[1];
			verts[j++] = v[2];
			// poly 2
			verts[j++] = v[0];
			verts[j++] = v[2];
			verts[j++] = v[3];
		}
	}

	Assert( j == sbi->n_verts );
}

// take the Starfield_bitmap_instances[] and make all the vertex buffers that you'll need to draw it 
static void starfield_generate_bitmap_buffers()
{
	auto sb_instances = Starfield_bitmap_instances.size();
	for (size_t idx = 0; idx < sb_instances; idx++)
	{
		if (Starfield_bitmap_instances[idx].star_bitmap_index < 0)
			continue;

		starfield_create_bitmap_buffer(idx);
	}
}

static void starfield_bitmap_entry_init(starfield_bitmap *sbm)
{
	int i;

	Assert( sbm != NULL );

	memset( sbm, 0, sizeof(starfield_bitmap) );

	sbm->bitmap_id = -1;
	sbm->glow_bitmap = -1;
	sbm->glow_n_frames = 1;

	for (i = 0; i < MAX_FLARE_BMP; i++) {
		sbm->flare_bitmaps[i].bitmap_id = -1;
	}

	for (i = 0; i < MAX_FLARE_COUNT; i++) {
		sbm->flare_infos[i].tex_num = -1;
	}
}

#define CHECK_END() {	\
	if (in_check) {	\
		required_string("#end");	\
		in_check = false;	\
		run_count = 0;	\
	}	\
}

void parse_startbl(const char *filename)
{
	char name[MAX_FILENAME_LEN], tempf[16];
	starfield_bitmap sbm;
	int idx;
	bool in_check = false;
	int rc = -1;
	int run_count = 0;

	try
	{
		read_file_text(filename, CF_TYPE_TABLES);
		reset_parse();

		// freaky! ;)
		while (!check_for_eof()) {
			
			optional_string("#Background Bitmaps");

			while ((rc = optional_string_either("$Bitmap:", "$BitmapX:")) != -1) {
				in_check = true;

				starfield_bitmap_entry_init(&sbm);

				stuff_string(sbm.filename, F_NAME, MAX_FILENAME_LEN);
				sbm.xparent = rc;  // 0 == intensity alpha bitmap,  1 == green xparency bitmap

				if ((idx = stars_find_bitmap(sbm.filename)) >= 0) {
					if (sbm.xparent == Starfield_bitmaps[idx].xparent) {
						if (!Parsing_modular_table)
							Warning(LOCATION, "Starfield bitmap '%s' listed more than once!!  Only using the first entry!", sbm.filename);
					}
					else {
						Warning(LOCATION, "Starfield bitmap '%s' already listed as a %s bitmap!!  Only using the xparent version!",
							sbm.filename, (rc) ? "xparent" : "non-xparent");
					}
				}
				else {
					Starfield_bitmaps.push_back(sbm);
				}
			}

			CHECK_END();

			optional_string("#Stars");

			while (optional_string("$Sun:")) {
				in_check = true;

				starfield_bitmap_entry_init(&sbm);

				stuff_string(sbm.filename, F_NAME, MAX_FILENAME_LEN);

				// associated glow
				required_string("$Sunglow:");
				stuff_string(sbm.glow_filename, F_NAME, MAX_FILENAME_LEN);

				// associated lighting values
				required_string("$SunRGBI:");
				stuff_float(&sbm.r);
				stuff_float(&sbm.g);
				stuff_float(&sbm.b);
				stuff_float(&sbm.i);

				if (optional_string("$SunSpecularRGB:")) {
					SCP_string warning;
					sprintf(warning, "Sun %s tried to set SunSpecularRGB. This feature has been deprecated and will be ignored.", sbm.filename);

					float spec_r, spec_g, spec_b;
					stuff_float(&spec_r);
					stuff_float(&spec_g);
					stuff_float(&spec_b);

					if (fl_equal(sbm.r, spec_r) && fl_equal(sbm.g, spec_g) && fl_equal(sbm.b, spec_b))
						mprintf(("%s\n", warning.c_str()));				// default case is not significant
					else
						Warning(LOCATION, "%s", warning.c_str());		// customized case is significant
				}

				// lens flare stuff
				if (optional_string("$Flare:")) {
					sbm.flare = 1;

					required_string("+FlareCount:");
					stuff_int(&sbm.n_flares);

					// if there's a flare, it has to have at least one texture
					required_string("$FlareTexture1:");
					stuff_string(sbm.flare_bitmaps[0].filename, F_NAME, MAX_FILENAME_LEN);

					sbm.n_flare_bitmaps = 1;

					for (idx = 1; idx < MAX_FLARE_BMP; idx++) {
						// allow 9999 textures (theoretically speaking, that is)
						sprintf(tempf, "$FlareTexture%d:", idx + 1);

						if (optional_string(tempf)) {
							sbm.n_flare_bitmaps++;
							stuff_string(sbm.flare_bitmaps[idx].filename, F_NAME, MAX_FILENAME_LEN);
						}
						//	else break; //don't allow flaretexture1 and then 3, etc.
					}

					required_string("$FlareGlow1:");

					required_string("+FlareTexture:");
					stuff_int(&sbm.flare_infos[0].tex_num);

					required_string("+FlarePos:");
					stuff_float(&sbm.flare_infos[0].pos);

					required_string("+FlareScale:");
					stuff_float(&sbm.flare_infos[0].scale);

					sbm.n_flares = 1;

					for (idx = 1; idx < MAX_FLARE_COUNT; idx++) {
						// allow a lot of glows
						sprintf(tempf, "$FlareGlow%d:", idx + 1);

						if (optional_string(tempf)) {
							sbm.n_flares++;

							required_string("+FlareTexture:");
							stuff_int(&sbm.flare_infos[idx].tex_num);

							required_string("+FlarePos:");
							stuff_float(&sbm.flare_infos[idx].pos);

							required_string("+FlareScale:");
							stuff_float(&sbm.flare_infos[idx].scale);
						}
						//	else break; //don't allow "flare 1" and then "flare 3"
					}
				}

				sbm.glare = !optional_string("$NoGlare:");

				sbm.xparent = 1;

				if ((idx = stars_find_sun(sbm.filename)) >= 0) {
					if (Parsing_modular_table)
						Sun_bitmaps[idx] = sbm;
					else
						Warning(LOCATION, "Sun bitmap '%s' listed more than once!!  Only using the first entry!", sbm.filename);
				}
				else {
					Sun_bitmaps.push_back(sbm);
				}
			}

			CHECK_END();

			optional_string("#Motion Debris");

			// normal debris pieces
			// leaving this for retail - Mjn
			if (check_for_string("$Debris:")) {

				mprintf(("Using deprecated motion debris parsing for Default motion debris!\n"));
				motion_debris_types this_debris;
				this_debris.name = "Default";

				int count = 0;

				while (optional_string("$Debris:")) {
				in_check = true;

					stuff_string(name, F_NAME, MAX_FILENAME_LEN);

					if (count < MAX_MOTION_DEBRIS_BITMAPS) {
						strcpy_s(this_debris.bitmaps[count++].name, name);
					} else {
						Warning(LOCATION, "Could not load normal motion debris '%s'; maximum of %d exceeded.", name, MAX_MOTION_DEBRIS_BITMAPS);
					}
				}
				if (count == MAX_MOTION_DEBRIS_BITMAPS) {
					Motion_debris_info.push_back(this_debris);
				} else {
					error_display(0, "Not enough bitmaps defined for motion debris '%s'. Skipping!\n", this_debris.name.c_str());
				}
			}

			CHECK_END();

			// nebula debris pieces
			// leaving this for retail - Mjn
			if (check_for_string("$DebrisNeb:")) {

				mprintf(("Using deprecated motion debris parsing for Nebula motion debris!\n"));
				motion_debris_types this_debris;
				this_debris.name = "Nebula";

				int count = 0;

				while (optional_string("$DebrisNeb:")) {
				in_check = true;

					stuff_string(name, F_NAME, MAX_FILENAME_LEN);

					if (count < MAX_MOTION_DEBRIS_BITMAPS) {
						strcpy_s(this_debris.bitmaps[count++].name, name);
					} else {
						Warning(LOCATION, "Could not load nebula motion debris '%s'; maximum of %d exceeded.", name, MAX_MOTION_DEBRIS_BITMAPS);
					}
				}
				if (count == MAX_MOTION_DEBRIS_BITMAPS) {
					Motion_debris_info.push_back(this_debris);
				} else {
					error_display(0, "Not enough bitmaps defined for motion debris '%s'. Skipping!\n", this_debris.name.c_str());
				}
			}

			CHECK_END();

			// custom debris pieces
			while (optional_string("$Motion Debris Name:")) {
				in_check = true;

				stuff_string(name, F_NAME, MAX_NAME_LEN);

				motion_debris_types this_debris;
				this_debris.name = name;

				// check if we will replace an existing entry
				int check = get_motion_debris_by_name(name);

				motion_debris_types* debris_ptr;

				//If we're going to create a new motion debris then set it up
				if (check == -1) {

					// initialize all the names as empty strings for later checking
					for (int i = 0; i < MAX_MOTION_DEBRIS_BITMAPS; i++) {
						this_debris.bitmaps[i].name[0] = '\0';
					}

					Motion_debris_info.push_back(this_debris);
					check = static_cast<int>(Motion_debris_info.size()) - 1;
				}

				debris_ptr = &Motion_debris_info[check];

				int count = 0;

				while (count < MAX_MOTION_DEBRIS_BITMAPS){
					
					required_string("+Bitmap:");
					stuff_string(name, F_NAME, MAX_FILENAME_LEN);

					strcpy_s(debris_ptr->bitmaps[count++].name, name);

				}

				for (int i = 0; i < MAX_MOTION_DEBRIS_BITMAPS; i++) {
					if(debris_ptr->bitmaps[i].name[0] == '\0'){
						error_display(0, "Not enough bitmaps defined for motion debris '%s'. Removing!\n", this_debris.name.c_str());
						Motion_debris_info.erase(Motion_debris_info.begin() + check);
						break;
					}
				}

			}

			CHECK_END();

			// since it's possible for some idiot to have a tbl screwed up enough
			// that this ends up in an endless loop, give an opportunity to advance
			// through the file no matter what, because even the retail tbl has an
			// extra "#end" line in it.
			if (optional_string("#end") || (run_count++ > 5)) {
				run_count = 0;
				advance_to_eoln(NULL);
			}
		}
	}
	catch (const parse::ParseException& e)
	{
		mprintf(("TABLES: Unable to parse '%s'!  Error message = %s.\n", filename, e.what()));
		return;
	}
}

void stars_load_all_bitmaps()
{
	static int Star_bitmaps_loaded = 0;

	if (Star_bitmaps_loaded)
		return;

	// pre-load all starfield bitmaps.  ONLY SHOULD DO THIS FOR FRED!!
	// this can get nasty when a lot of bitmaps are in use so spare it for
	// the normal game and only do this in FRED
	int mprintf_count = 0;
	for (auto &sb : Starfield_bitmaps) {
		if (sb.bitmap_id < 0) {
			sb.bitmap_id = bm_load(sb.filename);

			// maybe didn't load a static image so try for an animated one
			if (sb.bitmap_id < 0) {
				sb.bitmap_id = bm_load_animation(sb.filename, &sb.n_frames, &sb.fps, nullptr, nullptr, true);

				if (sb.bitmap_id < 0) {
					mprintf(("Unable to load starfield bitmap: '%s'!\n", sb.filename));
					mprintf_count++;
				}
			}
		}
	}
	if (mprintf_count > 0) {
		Warning(LOCATION, "Unable to load %d starfield bitmap(s)!\n", mprintf_count);
	}

	for (auto &sb : Sun_bitmaps) {
		// normal bitmap
		if (sb.bitmap_id < 0) {
			sb.bitmap_id = bm_load(sb.filename);

			// maybe didn't load a static image so try for an animated one
			if (sb.bitmap_id < 0) {
				sb.bitmap_id = bm_load_animation(sb.filename, &sb.n_frames, &sb.fps, nullptr, nullptr, true);

				if (sb.bitmap_id < 0) {
					Warning(LOCATION, "Unable to load sun bitmap: '%s'!\n", sb.filename);
				}
			}
		}

		// glow bitmap
		if (sb.glow_bitmap < 0) {
			sb.glow_bitmap = bm_load(sb.glow_filename);

			// maybe didn't load a static image so try for an animated one
			if (sb.glow_bitmap < 0) {
				sb.glow_bitmap = bm_load_animation(sb.glow_filename, &sb.glow_n_frames, &sb.glow_fps, nullptr, nullptr, true);

				if (sb.glow_bitmap < 0) {
					Warning(LOCATION, "Unable to load sun glow bitmap: '%s'!\n", sb.glow_filename);
				}
			}
		}

		if (sb.flare) {
			for (int i = 0; i < MAX_FLARE_BMP; i++) {
				if ( !strlen(sb.flare_bitmaps[i].filename) )
					continue;

				if (sb.flare_bitmaps[i].bitmap_id < 0) {
					sb.flare_bitmaps[i].bitmap_id = bm_load(sb.flare_bitmaps[i].filename);

					if (sb.flare_bitmaps[i].bitmap_id < 0) {
						Warning(LOCATION, "Unable to load sun flare bitmap: '%s'!\n", sb.flare_bitmaps[i].filename);
						continue;
					}
				}
			}
		}
	}

	Star_bitmaps_loaded = 1;
}

void stars_clear_instances()
{
	for (auto &sbi : Starfield_bitmap_instances) {
		delete [] sbi.verts;
		sbi.verts = nullptr;
	}

	Starfield_bitmap_instances.clear();
	Suns.clear();
}

// call on game startup
void stars_init()
{
	// parse stars.tbl
	parse_startbl("stars.tbl");

	parse_modular_table("*-str.tbm", parse_startbl);

	// Warn if we can't find the two retail motion debris types.
	if (get_motion_debris_by_name("Default") < 0)
		Warning(LOCATION, "Motion debris 'Default' not found in stars.tbl. Motion debris will be disabled!\n");

	if (get_motion_debris_by_name("Nebula") < 0)
		Warning(LOCATION, "Motion debris 'Nebula' not found in stars.tbl. Motion debris will be disabled!\n");

	if (Cmdline_env) {
		ENVMAP = Default_env_map = bm_load("cubemap");
	}
}

// call only from game_shutdown()!!
void stars_close()
{
	stars_clear_instances();

	// any other code goes here
}

// called before mission parse so we can clear out all of the old stuff
void stars_pre_level_init(bool clear_backgrounds)
{
	Num_stars = 500;

	// we used to clear all the array entries, but now we can just wipe the vector
	if (clear_backgrounds)
		Backgrounds.clear();

	stars_clear_instances();

	stars_set_background_model(nullptr, nullptr);
	stars_set_background_orientation();

	// mark all starfield and sun bitmaps as unused for this mission and release any current bitmaps
	// NOTE: that because of how we have to load the bitmaps it's important to release all of
	//       them first thing rather than after we have marked and loaded only what's needed
	// NOTE2: there is a reason that we don't check for release before setting the handle to -1 so
	//        be aware that this is NOT a bug. also, bmpman should NEVER return 0 as a valid handle!
	if ( !Fred_running ) {
		for (auto &sb : Starfield_bitmaps) {
			if (sb.bitmap_id > 0) {
				bm_release(sb.bitmap_id);
				sb.bitmap_id = -1;
			}

			sb.used_this_level = 0;
			sb.preload = 0;
		}

		for (auto &sb : Sun_bitmaps) {
			if (sb.bitmap_id > 0) {
				bm_release(sb.bitmap_id);
				sb.bitmap_id = -1;
			}

			if (sb.glow_bitmap > 0) {
				bm_release(sb.glow_bitmap);
				sb.glow_bitmap = -1;
			}

			for (int i = 0; i < MAX_FLARE_BMP; i++) {
				if (sb.flare_bitmaps[i].bitmap_id > 0) {
					bm_release(sb.flare_bitmaps[i].bitmap_id);
					sb.flare_bitmaps[i].bitmap_id = -1;
				}
			}

			sb.used_this_level = 0;
			sb.preload = 0;
		}
	}

	// also clear the preload indexes
	Preload_background_indexes.clear();

	Dynamic_environment = false;
	Motion_debris_override = false;

	Env_cubemap_drawn = false;
	Irr_cubemap_drawn = false;

	// reset the skybox timestamp, used for animated textures
	Skybox_timestamp = _timestamp();
}

// setup the render target ready for this mission's environment map
static void environment_map_gen()
{
	const int size = 512;
	int gen_flags = (BMP_FLAG_RENDER_TARGET_STATIC | BMP_FLAG_CUBEMAP | BMP_FLAG_RENDER_TARGET_MIPMAP);

	if ( !Cmdline_env ) {
		return;
	}

	if (gr_screen.envmap_render_target >= 0) {
		if ( !bm_release(gr_screen.envmap_render_target, 1) ) {
			Warning(LOCATION, "Unable to release environment map render target.");
		}

		gr_screen.envmap_render_target = -1;
	}

	if ( Dynamic_environment || (The_mission.flags[Mission::Mission_Flags::Subspace]) ) {
		Dynamic_environment = true;
		gen_flags &= ~BMP_FLAG_RENDER_TARGET_STATIC;
		gen_flags |= BMP_FLAG_RENDER_TARGET_DYNAMIC;
	}
		// bail if we are going to be static, and have an envmap specified already
	else if ( strlen(The_mission.envmap_name) ) {
		// Load the mission map so we can use it later
		Mission_env_map = bm_load(The_mission.envmap_name);
		return;
	}

	gr_screen.envmap_render_target = bm_make_render_target(size, size, gen_flags);
}

// setup the render target ready for this mission's environment map
static void irradiance_map_gen()
{
	const int irr_size = 16;
	int gen_flags = (BMP_FLAG_RENDER_TARGET_STATIC | BMP_FLAG_CUBEMAP | BMP_FLAG_RENDER_TARGET_MIPMAP);

	if (!Cmdline_env) {
		return;
	}

	if (gr_screen.irrmap_render_target >= 0) {
		if (!bm_release(gr_screen.irrmap_render_target, 1)) {
			Warning(LOCATION, "Unable to release environment map render target.");
		}

		gr_screen.irrmap_render_target = -1;
		IRRMAP = -1;
	}

	if (Dynamic_environment || (The_mission.flags[Mission::Mission_Flags::Subspace])) {
		Dynamic_environment = true;
		gen_flags &= ~BMP_FLAG_RENDER_TARGET_STATIC;
		gen_flags |= BMP_FLAG_RENDER_TARGET_DYNAMIC;
	}

	gr_screen.irrmap_render_target = bm_make_render_target(irr_size, irr_size, gen_flags);
	IRRMAP = gr_screen.irrmap_render_target;
}

// call this in game_post_level_init() so we know whether we're running in full nebula mode or not
void stars_post_level_init()
{
	int i;
	vec3d v;
	float dist, dist_max;
	ubyte red,green,blue,alpha;

	if (gr_screen.mode == GR_STUB) {
		return;
	}

	// in FRED, make sure we always have at least one background
	if (Fred_running)
	{
		if (Backgrounds.empty())
			stars_add_blank_background(true);
	}
	// in FSO, see if we have any backgrounds to preload
	// (since the backgrounds aren't parsed at the time we parse the sexps)
	else
	{
		for (int idx : Preload_background_indexes)
			stars_preload_background(idx);
	}

	stars_set_background_model(The_mission.skybox_model, NULL, The_mission.skybox_flags);
	stars_set_background_orientation(&The_mission.skybox_orientation);

	stars_load_debris( ((The_mission.flags[Mission::Mission_Flags::Fullneb]) || Nebula_sexp_used) );

// following code randomly distributes star points within a sphere volume, which
// avoids there being denser areas along the edges and in corners that we had in the
// old rectangular distribution scheme.
	dist_max = (float) (HALF_RND_MAX * HALF_RND_MAX);
	for (i=0; i<MAX_STARS; i++) {
		dist = dist_max;
		while (dist >= dist_max) {
			v.xyz.x = (float) ((Random::next() & RND_MAX_MASK) - HALF_RND_MAX);
			v.xyz.y = (float) ((Random::next() & RND_MAX_MASK) - HALF_RND_MAX);
			v.xyz.z = (float) ((Random::next() & RND_MAX_MASK) - HALF_RND_MAX);

			dist = v.xyz.x * v.xyz.x + v.xyz.y * v.xyz.y + v.xyz.z * v.xyz.z;
		}
		vm_vec_copy_normalize(&Stars[i].pos, &v);

		{
			red = (ubyte)Random::next(192, 255);
			green = (ubyte)Random::next(192, 255);
			blue = (ubyte)Random::next(192, 255);
			alpha = (ubyte)Random::next(24, 216);

			gr_init_alphacolor(&Stars[i].col, red, green, blue, alpha, AC_TYPE_BLEND);
		}

	}

	memset( &Motion_debris, 0, sizeof(motion_debris_instance) * MAX_MOTION_DEBRIS );

	
	for (i=0; i<8; i++ ) {
		ubyte intensity = (ubyte)((i + 1) * 24);
		gr_init_alphacolor(&star_aacolors[i], 255, 255, 255, intensity, AC_TYPE_BLEND );
		gr_init_color(&star_colors[i], intensity, intensity, intensity );
	}

	last_stars_filled = 0;

	// if we have no sun instances, create one
	if ( Suns.empty() ) {
		if ( !strlen(Sun_bitmaps[0].filename) ) {
			mprintf(("Trying to add default sun but no default exists!!\n"));
		} else {
			mprintf(("Adding default sun.\n"));

			starfield_bitmap_instance def_sun;

			// stuff some values
			def_sun.ang.h = fl_radians(-60.0f);

			Suns.push_back(def_sun);
		}
	}

	// FRED doesn't do normal page_in stuff so we need to load up the bitmaps here instead
	if (Fred_running) {
		stars_load_all_bitmaps();

		// see whether we are missing any suns or bitmaps
		int count = static_cast<int>(Backgrounds.size());
		for (i = 0; i < count; ++i) {
			SCP_string failed_suns;
			for (auto &sun : Backgrounds[i].suns) {
				if (stars_find_sun(sun.filename) < 0) {
					failed_suns += sun.filename;
					failed_suns += "\n";
				}
			}

			SCP_string failed_bitmaps;
			for (auto &bitmap : Backgrounds[i].bitmaps) {
				if (stars_find_bitmap(bitmap.filename) < 0) {
					failed_bitmaps += bitmap.filename;
					failed_bitmaps += "\n";
				}
			}

			if (!failed_suns.empty()) {
				Warning(LOCATION, "In Background %d, failed to load the following suns:\n%s", (i + 1), failed_suns.c_str());
			}
			if (!failed_bitmaps.empty()) {
				Warning(LOCATION, "In Background %d, failed to load the following bitmaps:\n%s", (i + 1), failed_bitmaps.c_str());
			}
		}
	}

	starfield_generate_bitmap_buffers();

	environment_map_gen();
}

void stars_level_close() {
	if (gr_screen.envmap_render_target >= 0) {
		if ( bm_release(gr_screen.envmap_render_target, 1) ) {
			gr_screen.envmap_render_target = -1;
		}
	}

	if (Mission_env_map >= 0) {
		bm_release(Mission_env_map);
		Mission_env_map = -1;
	}

	ENVMAP = Default_env_map;
}


extern object * Player_obj;

#define STAR_AMOUNT_DEFAULT 0.75f
#define STAR_DIM_DEFAULT 7800.0f
#define STAR_CAP_DEFAULT 75.0f
#define STAR_MAX_LENGTH_DEFAULT 0.04f		// 312

float Star_amount = STAR_AMOUNT_DEFAULT;
float Star_dim = STAR_DIM_DEFAULT;
float Star_cap = STAR_CAP_DEFAULT;
float Star_max_length = STAR_MAX_LENGTH_DEFAULT;

#define STAR_FLAG_TAIL			(1<<0)	// Draw a tail when moving
#define STAR_FLAG_DIM			(1<<1)	// Dim as you move
#define STAR_FLAG_ANTIALIAS		(1<<2)	// Draw the star using antialiased lines
#define STAR_FLAG_DEFAULT		(STAR_FLAG_TAIL | STAR_FLAG_DIM)

uint Star_flags = STAR_FLAG_DEFAULT;

//XSTR:OFF
DCF(stars,"Set parameters for starfield")
{
	SCP_string arg;
	float val_f;
	int val_i;

	if (dc_optional_string_either("help", "--help")) {
		dc_printf( "Usage: stars keyword\nWhere keyword can be in the following forms:\n" );
		dc_printf( "stars default   Resets stars to all default values\n" );
		dc_printf( "stars num X     Sets number of stars to X.  Between 0 and %d.\n", MAX_STARS );
		dc_printf( "stars tail X    Where X is the percent of 'tail' between 0 and 1.0\n" );
		dc_printf( "stars dim X     Where X is the amount stars dim between 0 and 255.0\n" );
		dc_printf( "stars cap X     Where X is the cap of dimming between 0 and 255.\n" );
		dc_printf( "stars len X     Where X is the cap of length.\n" );
		dc_printf( "stars m0        Macro0. Old 'pixel type' crappy stars. flags=none\n" );
		dc_printf( "stars m1        Macro1. (default) tail=.75, dim=20.0, cap=75.0, flags=dim,tail\n" );
		dc_printf( "stars m2        Macro2. tail=.75, dim=20.0, cap=75.0, flags=dim,tail,aa\n" );
		dc_printf( "stars flag X    Toggles flag X, where X is tail or dim or aa (aa=antialias)\n" );
		dc_printf( "\nHINT: set cap to 0 to get dim rate and tail down, then use\n" );
		dc_printf( "cap to keep the lines from going away when moving too fast.\n" );
		dc_printf( "\nUse '? stars' to see current values.\n" );
		return;	// don't print status if help is printed.  Too messy.
	}

	if (dc_optional_string_either("status", "--status") || dc_optional_string_either("?", "--?")) {
		dc_printf( "Num_stars: %d\n", Num_stars );
		dc_printf( "Tail: %.2f\n", Star_amount );
		dc_printf( "Dim : %.2f\n", Star_dim );
		dc_printf( "Cap : %.2f\n", Star_cap );
		dc_printf( "Max length: %.2f\n", Star_max_length );
		dc_printf( "Flags:\n" );
		dc_printf( "  Tail : %s\n", (Star_flags&STAR_FLAG_TAIL?"On":"Off") );
		dc_printf( "  Dim  : %s\n", (Star_flags&STAR_FLAG_DIM?"On":"Off") );
		dc_printf( "  Antialias: %s\n", (Star_flags&STAR_FLAG_ANTIALIAS?"On":"Off") );
		dc_printf( "\nTHESE AREN'T SAVED TO DISK, SO IF YOU TWEAK\n" );
		dc_printf( "THESE AND LIKE THEM, WRITE THEM DOWN!!\n" );
		return;
	}

	dc_stuff_string_white(arg);
	// "stars default" is handled by "stars m1"
	if (arg == "num") {
		dc_stuff_int(&val_i);

		CLAMP(val_i, 0, MAX_STARS);
		Num_stars = val_i;

		dc_printf("Num_stars set to %i\n", Num_stars);
	
	} else if (arg == "tail") {
		dc_stuff_float(&val_f);
		CLAMP(val_f, 0.0f, 1.0f);
		Star_amount = val_f;
		
		dc_printf("Star_amount set to %f\n", Star_amount);

	} else if (arg == "dim") {
		dc_stuff_float(&val_f);

		if (val_f > 0.0f ) {
			Star_dim = val_f;
			dc_printf("Star_dim set to %f\n", Star_dim);
		
		} else {
			dc_printf("Error: Star_dim value must be non-negative\n");
		}
	
	} else if (arg == "cap") {
		dc_stuff_float(&val_f);
		CLAMP(val_f, 0.0f, 255.0f);
		Star_cap = val_f;
		
		dc_printf("Star_cap set to %f\n", Star_cap);
	
	} else if (arg == "len") {
		dc_stuff_float(&Star_max_length);

		dc_printf("Star_max_length set to %f\n", Star_max_length);

	} else if (arg == "m0") {
		Star_amount = 0.0f;
		Star_dim = 0.0f;
		Star_cap = 0.0f;
		Star_flags = 0;
		Star_max_length = STAR_MAX_LENGTH_DEFAULT;

		dc_printf("Starfield set: Old 'pixel type' crappy stars. flags=none\n");
	
	} else if ((arg == "m1") || (arg == "default")) {
		Star_amount = STAR_AMOUNT_DEFAULT;
		Star_dim = STAR_DIM_DEFAULT;
		Star_cap = STAR_CAP_DEFAULT;
		Star_flags = STAR_FLAG_DEFAULT;
		Star_max_length = STAR_MAX_LENGTH_DEFAULT;

		dc_printf("Starfield set: (default) tail=.75, dim=20.0, cap=75.0, flags=dim,tail\n");

	} else if (arg == "m2") {
		Star_amount = 0.75f;
		Star_dim = 20.0f;
		Star_cap = 75.0f;
		Star_flags = STAR_FLAG_TAIL|STAR_FLAG_DIM|STAR_FLAG_ANTIALIAS;
		Star_max_length = STAR_MAX_LENGTH_DEFAULT;

		dc_printf("Starfield set: tail=.75, dim=20.0, cap=75.0, flags=dim,tail,aa\n");

	} else if (arg == "flag") {
		dc_stuff_string_white(arg);
		if (arg == "tail") {
			Star_flags ^= STAR_FLAG_TAIL;
		} else if (arg == "dim" ) {
			Star_flags ^= STAR_FLAG_DIM;
		} else if (arg == "aa" ) {
			Star_flags ^= STAR_FLAG_ANTIALIAS;
		} else {
			dc_printf("Error: unknown flag argument '%s'\n", arg.c_str());
		}

	} else {
		dc_printf("Error: Unknown argument '%s'", arg.c_str());
	}
}
//XSTR:ON

bool refresh_motion_debris = true; // If set to true, then regenerate the positions of motion debris
// Call this if camera "cuts" or moves long distances
// so blur effect doesn't draw lines all over the screen.
void stars_camera_cut()
{
	last_stars_filled = 0;
	refresh_motion_debris = true;
}

//#define TIME_STAR_CODE		// enable to time star code

extern int Sun_drew;

// get the world coords of the sun pos on the unit sphere.
void stars_get_sun_pos(int sun_n, vec3d *pos)
{
	vec3d temp;
	matrix rot;

	// sanity
	Assert(SCP_vector_inbounds(Suns, sun_n));
	if (!SCP_vector_inbounds(Suns, sun_n)) {
		return;
	}

	// rotate the sun properly
	temp = vmd_zero_vector;
	temp.xyz.z = 1.0f;
	
	// rotation matrix
	vm_angles_2_matrix(&rot, &Suns[sun_n].ang);
	vm_vec_unrotate(pos, &temp, &rot);
}

// draw sun
void stars_draw_sun(int show_sun)
{	
	GR_DEBUG_SCOPE("Draw Suns");
	TRACE_SCOPE(tracing::DrawSuns);

	int idx;
	vec3d sun_pos;
	vec3d sun_dir;
	vertex sun_vex;
	starfield_bitmap *bm;
	float local_scale;

	if (gr_screen.mode == GR_STUB) {
		return;
	}

	// should we even be here?
	if (!show_sun)
		return;

	// no suns drew yet
	Sun_drew = 0;

	// draw all suns
	int num_suns = static_cast<int>(Suns.size());
	for (idx = 0; idx < num_suns; idx++) {
		// get the instance
		if (Suns[idx].star_bitmap_index < 0)
			return;

		bm = &Sun_bitmaps[Suns[idx].star_bitmap_index];

		// if no bitmap then bail...
		if (bm->bitmap_id < 0)
			continue;

		memset( &sun_vex, 0, sizeof(vertex) );

		// get sun pos
		sun_pos = vmd_zero_vector;
		sun_pos.xyz.y = 1.0f;
		stars_get_sun_pos(idx, &sun_pos);
		
		// get the direction
		sun_dir = sun_pos;
		vm_vec_normalize(&sun_dir);

		// add the light source corresponding to the sun, except when rendering to an envmap
		if ( !Rendering_to_env )
			light_add_directional(&sun_dir, idx, !bm->glare, bm->i, bm->r, bm->g, bm->b);

		// if supernova
		if ( supernova_active() && (idx == 0) )
			local_scale = 1.0f + (SUPERNOVA_SUN_SCALE * supernova_pct_complete());
		else
			local_scale = 1.0f;

		// draw the sun itself, keep track of how many we drew
		int bitmap_id = -1;
		if (bm->fps) {
			bitmap_id = bm->bitmap_id + ((timestamp() * bm->fps / MILLISECONDS_PER_SECOND) % bm->n_frames);
		} else {
			bitmap_id = bm->bitmap_id;
		}

		g3_rotate_faraway_vertex(&sun_vex, &sun_pos);

		if ( sun_vex.codes & (CC_BEHIND|CC_OFF_USER) ) {
			continue;
		}

		if ( !(sun_vex.flags & PF_PROJECTED) ) {
			g3_project_vertex(&sun_vex);
		}

		if ( sun_vex.flags & PF_OVERFLOW ) {
			continue;
		}

		material mat_params;
		material_set_unlit(&mat_params, bitmap_id, 0.999f, true, false);
		g3_render_rect_screen_aligned_2d(&mat_params, &sun_vex, 0, 0.05f * Suns[idx].scale_x * local_scale, true);
		Sun_drew++;

// 		if ( !g3_draw_bitmap(&sun_vex, 0, 0.05f * Suns[idx].scale_x * local_scale, TMAP_FLAG_TEXTURED) )
// 			Sun_drew++;
	}
}

// draw a star's lens-flare
void stars_draw_lens_flare(vertex *sun_vex, int sun_n)
{
	starfield_bitmap *bm;
	int i,j;
	float dx,dy;
	vertex flare_vex = *sun_vex; //copy over to flare_vex to get all sorts of properties

	if (gr_screen.mode == GR_STUB) {
		return;
	}

	Assert(SCP_vector_inbounds(Suns, sun_n));
	if (!SCP_vector_inbounds(Suns, sun_n)) {
		return;
	}

	// get the instance
	if (Suns[sun_n].star_bitmap_index < 0) {
		return;
	} else {
		bm = &Sun_bitmaps[Suns[sun_n].star_bitmap_index];
	}

	if (!bm->flare)
		return;
	
	/* (dx,dy) is a 2d vector equal to two times the vector from the sun's
	position to the center fo the screen meaning it is the vector to the 
	opposite position on the screen. */
	dx = 2.0f*(i2fl(gr_screen.clip_right-gr_screen.clip_left)*0.5f - sun_vex->screen.xyw.x);
	dy = 2.0f*(i2fl(gr_screen.clip_bottom-gr_screen.clip_top)*0.5f - sun_vex->screen.xyw.y);

	for (j = 0; j < bm->n_flare_bitmaps; j++)
	{
		// if no bitmap then bail...
		if (bm->flare_bitmaps[j].bitmap_id < 0)
			continue;

		//gr_set_bitmap(bm->flare_bitmaps[j].bitmap_id, GR_ALPHABLEND_FILTER, GR_BITBLT_MODE_NORMAL, 0.999f);

		for (i = 0; i < bm->n_flares; i++) {
			// draw sorted by texture, to minimize texture changes. not the most efficient way, but better than non-sorted
			if (bm->flare_infos[i].tex_num == j) {
				flare_vex.screen.xyw.x = sun_vex->screen.xyw.x + dx * bm->flare_infos[i].pos;
				flare_vex.screen.xyw.y = sun_vex->screen.xyw.y + dy * bm->flare_infos[i].pos;
				//g3_draw_bitmap(&flare_vex, 0, 0.05f * bm->flare_infos[i].scale, TMAP_FLAG_TEXTURED);
				material mat_params;
				material_set_unlit(&mat_params, bm->flare_bitmaps[j].bitmap_id, 0.999f, true, false);
				g3_render_rect_screen_aligned_2d(&mat_params, &flare_vex, 0, 0.05f * bm->flare_infos[i].scale);
			}
		}
	}
}

// draw the corresponding glow for sun_n
void stars_draw_sun_glow(int sun_n)
{
	starfield_bitmap *bm;
	vec3d sun_pos, sun_dir;
	vertex sun_vex;	
	float local_scale;

	if (gr_screen.mode == GR_STUB) {
		return;
	}

	// sanity
	Assert(SCP_vector_inbounds(Suns, sun_n));
	if (!SCP_vector_inbounds(Suns, sun_n)) {
		return;
	}

	// get the instance
	if (Suns[sun_n].star_bitmap_index < 0)
		return;

	bm = &Sun_bitmaps[Suns[sun_n].star_bitmap_index];

	// if no bitmap then bail...
	if (bm->glow_bitmap < 0)
		return;

	memset( &sun_vex, 0, sizeof(vertex) );

	// get sun pos
	sun_pos = vmd_zero_vector;
	sun_pos.xyz.y = 1.0f;
	stars_get_sun_pos(sun_n, &sun_pos);	

	// get the direction
	sun_dir = sun_pos;
	vm_vec_normalize(&sun_dir);

	// if supernova
	if ( supernova_active() && (sun_n == 0) )
		local_scale = 1.0f + (SUPERNOVA_SUN_SCALE * supernova_pct_complete());
	else
		local_scale = 1.0f;

	// draw the sun itself, keep track of how many we drew
	int bitmap_id = -1;
	if (bm->glow_fps) {
		bitmap_id = bm->glow_bitmap + ((timestamp() * bm->glow_fps / MILLISECONDS_PER_SECOND) % bm->glow_n_frames);
	} else {
		bitmap_id = bm->glow_bitmap;
	}

	g3_rotate_faraway_vertex(&sun_vex, &sun_pos);
	//int zbuff = gr_zbuffer_set(GR_ZBUFF_NONE);
	//g3_draw_bitmap(&sun_vex, 0, 0.10f * Suns[sun_n].scale_x * local_scale, TMAP_FLAG_TEXTURED);
	material mat_params;
	material_set_unlit(&mat_params, bitmap_id, 0.5f, true, false);
	g3_render_rect_screen_aligned_2d(&mat_params, &sun_vex, 0, 0.10f * Suns[sun_n].scale_x * local_scale, true);

	if (bm->flare) {
		vec3d light_dir;
		vec3d local_light_dir;
		light_get_global_dir(&light_dir, sun_n);
		vm_vec_rotate(&local_light_dir, &light_dir, &Eye_matrix);
		float dot=vm_vec_dot( &light_dir, &Eye_matrix.vec.fvec );
		if (dot > 0.7f) // Only render the flares if the sun is reasonably near the center of the screen
			stars_draw_lens_flare(&sun_vex, sun_n);
	}

	//gr_zbuffer_set(zbuff);
}

void stars_draw_bitmaps(int show_bitmaps)
{
	GR_DEBUG_SCOPE("Draw Bitmaps");
	TRACE_SCOPE(tracing::DrawBitmaps);

	int idx;
	int star_index;

	if (gr_screen.mode == GR_STUB) {
		return;
	}

	// should we even be here?
	if ( !show_bitmaps )
		return;

	// if we're in the nebula, don't render any backgrounds
	if (The_mission.flags[Mission::Mission_Flags::Fullneb] && !The_mission.flags[Mission::Mission_Flags::Fullneb_background_bitmaps])
		return;

	// detail settings
	if ( !Detail.planets_suns )
		return;

	gr_start_instance_matrix(&Eye_position, &vmd_identity_matrix);

	int sb_instances = static_cast<int>(Starfield_bitmap_instances.size());
	for (idx = 0; idx < sb_instances; idx++) {
		// lookup the info index
		star_index = Starfield_bitmap_instances[idx].star_bitmap_index;

		if (star_index < 0) {
			continue;
		}

		// if no bitmap then bail...
		if (Starfield_bitmaps[star_index].bitmap_id < 0) {
			continue;
		}

		int bitmap_id;
		bool blending = false;
		float alpha = 1.0f;

		if (Starfield_bitmaps[star_index].xparent) {
			if (Starfield_bitmaps[star_index].fps) {
				bitmap_id = Starfield_bitmaps[star_index].bitmap_id + (((timestamp() * Starfield_bitmaps[star_index].fps) / MILLISECONDS_PER_SECOND) % Starfield_bitmaps[star_index].n_frames);
			} else {
				bitmap_id = Starfield_bitmaps[star_index].bitmap_id;
			}
		} else {
			if (Starfield_bitmaps[star_index].fps) {
				bitmap_id = Starfield_bitmaps[star_index].bitmap_id + (((timestamp() * Starfield_bitmaps[star_index].fps) / MILLISECONDS_PER_SECOND) % Starfield_bitmaps[star_index].n_frames);
				blending = true;
				alpha = 0.9999f;
			} else {
				bitmap_id = Starfield_bitmaps[star_index].bitmap_id;	
				blending = true;
				alpha = 0.9999f;
			}
		}

		material material_params;
		material_set_unlit(&material_params, bitmap_id, alpha, blending, false);
		g3_render_primitives_textured(&material_params, Starfield_bitmap_instances[idx].verts, Starfield_bitmap_instances[idx].n_verts, PRIM_TYPE_TRIS, false);
	}
	
	gr_end_instance_matrix();
}

extern int Interp_subspace;
extern float Interp_subspace_offset_u;
extern float Interp_subspace_offset_u;
extern float Interp_subspace_offset_v;

float subspace_offset_u = 0.0f;
float subspace_offset_u_inner = 0.0f;
float subspace_offset_v = 0.0f;

float subspace_u_speed = 0.07f;			// how fast u changes
float subspace_v_speed = 0.05f;			// how fast v changes

int Subspace_glow_bitmap = -1;

float Subspace_glow_frame = 0.0f;
float Subspace_glow_rate = 1.0f;


//XSTR:OFF
DCF(subspace_set,"Set parameters for subspace effect")
{
	SCP_string arg;
	float value;

	if (dc_optional_string_either("help", "--help")) {
		dc_printf( "Usage: subspace [--status] <axis> <speed>\n");
		dc_printf("[--status] -- Displays the current speeds for both axes\n");
		dc_printf("<axis>  -- May be either 'u' or 'v', and corresponds to the texture axis\n");
		dc_printf("<speed> -- is the speed along the axis that the texture is moved\n");
		return;
	}

	if (dc_optional_string_either("status", "--status") || dc_optional_string_either("?", "--?")) {
		dc_printf( "u: %.2f\n", subspace_u_speed );
		dc_printf( "v: %.2f\n", subspace_v_speed );
		return;
	}

	dc_stuff_string_white(arg);
	if (arg == "u") {
		dc_stuff_float(&value);

		if ( value < 0.0f ) {
			dc_printf("Error: speed must be non-negative");
			return;
		}
		subspace_u_speed = value;

	} else if (arg == "v") {
		dc_stuff_float(&value);

		if (value < 0.0f) {
			dc_printf("Error: speed must be non-negative");
			return;
		}
		subspace_v_speed = value;

	} else {
		dc_printf("Error: Unknown axis '%s'", arg.c_str());
	}
}
//XSTR:ON

void subspace_render()
{
	int framenum = 0;

	if (gr_screen.mode == GR_STUB) {
		return;
	}

	if ( Subspace_model_inner < 0 )	{
		Subspace_model_inner = model_load( "subspace_small.pof" );
		Assert(Subspace_model_inner >= 0);
	}

	if ( Subspace_model_outer < 0 )	{
		Subspace_model_outer = model_load( "subspace_big.pof" );
		Assert(Subspace_model_outer >= 0);
	}

	if ( Subspace_glow_bitmap < 0 )	{
		Subspace_glow_bitmap = bm_load( NOX("SunGlow01"));
		Assert(Subspace_glow_bitmap >= 0);
	}

	if ( !Rendering_to_env ) {
		Subspace_glow_frame += flFrametime * 1.0f;

		float total_time = i2fl(NOISE_NUM_FRAMES) / 15.0f;

		// Sanity checks
		if ( Subspace_glow_frame < 0.0f )
			Subspace_glow_frame = 0.0f;
		if ( Subspace_glow_frame > 100.0f )
			Subspace_glow_frame = 0.0f;

		while ( Subspace_glow_frame > total_time ) {
			Subspace_glow_frame -= total_time;
		}

		framenum = fl2i( (Subspace_glow_frame*NOISE_NUM_FRAMES) / total_time );

		if ( framenum < 0 )
			framenum = 0;
		if ( framenum >= NOISE_NUM_FRAMES )
			framenum = NOISE_NUM_FRAMES-1;

		subspace_offset_u += flFrametime*subspace_u_speed;
		if (subspace_offset_u > 1.0f ) {
			subspace_offset_u -= 1.0f;
		}

		subspace_offset_u_inner += flFrametime*subspace_u_speed*3.0f;
		if (subspace_offset_u > 1.0f ) {
			subspace_offset_u -= 1.0f;
		}

		subspace_offset_v += flFrametime*subspace_v_speed;
		if (subspace_offset_v > 1.0f ) {
			subspace_offset_v -= 1.0f;
		}
	}


	matrix tmp;
	angles angs = vmd_zero_angles;

	angs.b = subspace_offset_v * PI2;

	vm_angles_2_matrix(&tmp,&angs);

	int saved_gr_zbuffering = gr_zbuffer_get();

	gr_zbuffer_set(GR_ZBUFF_NONE);

	uint64_t render_flags = MR_NO_LIGHTING | MR_ALL_XPARENT;

	Interp_subspace = 1;
	Interp_subspace_offset_u = 1.0f - subspace_offset_u;
	Interp_subspace_offset_v = 0.0f;

	model_render_params render_info;
	render_info.set_alpha(1.0f);
	render_info.set_flags(render_flags);

	gr_set_texture_panning(Interp_subspace_offset_v, Interp_subspace_offset_u, true);

	model_render_immediate( &render_info, Subspace_model_outer, &tmp, &Eye_position);	//MR_NO_CORRECT|MR_SHOW_OUTLINE

	gr_set_texture_panning(0, 0, false);

	Interp_subspace = 1;
	Interp_subspace_offset_u = 1.0f - subspace_offset_u_inner;
	Interp_subspace_offset_v = 0.0f;

	angs.b = -subspace_offset_v * PI2;

	vm_angles_2_matrix(&tmp,&angs);

	render_info.set_color(255, 255, 255);
	render_info.set_alpha(1.0f);
	render_info.set_flags(render_flags);

	gr_set_texture_panning(Interp_subspace_offset_v, Interp_subspace_offset_u, true);

	model_render_immediate( &render_info, Subspace_model_inner, &tmp, &Eye_position );	//MR_NO_CORRECT|MR_SHOW_OUTLINE

	gr_set_texture_panning(0, 0, false);

	//Render subspace glows here and not as thrusters - Valathil 
	vec3d glow_pos;
	vertex glow_vex;	

	glow_pos.xyz.x = 0.0f;
	glow_pos.xyz.y = 0.0f;
	glow_pos.xyz.z = 1.0f;

	//gr_set_bitmap(Subspace_glow_bitmap, GR_ALPHABLEND_FILTER, GR_BITBLT_MODE_NORMAL, 1.0f);
	material mat_params;
	material_set_unlit(&mat_params, Subspace_glow_bitmap, 1.0f, true, false);

	g3_rotate_faraway_vertex(&glow_vex, &glow_pos);
	//g3_draw_bitmap(&glow_vex, 0, 17.0f + 0.5f * Noise[framenum], TMAP_FLAG_TEXTURED);
	g3_render_rect_screen_aligned_2d(&mat_params, &glow_vex, 0, (17.0f + 0.5f * Noise[framenum]) * 0.01f, true);

	glow_pos.xyz.z = -1.0f;

	g3_rotate_faraway_vertex(&glow_vex, &glow_pos);
	//g3_draw_bitmap(&glow_vex, 0, 17.0f + 0.5f * Noise[framenum], TMAP_FLAG_TEXTURED);
	g3_render_rect_screen_aligned_2d(&mat_params, &glow_vex, 0, (17.0f + 0.5f * Noise[framenum]) * 0.01f, true);

	Interp_subspace = 0;
	gr_zbuffer_set(saved_gr_zbuffering);
}

void stars_draw_stars()
{
	GR_DEBUG_SCOPE("Draw Starfield");
	TRACE_SCOPE(tracing::DrawStarfield);

	int i;
	star *sp;
	float dist = 0.0f;
	float ratio;
	vDist vDst;
	vertex p1, p2;
	int can_draw = 1;

	if (gr_screen.mode == GR_STUB) {
		return;
	}

	if ( !last_stars_filled ) {
		for (i = 0; i < Num_stars; i++) {
			g3_rotate_faraway_vertex(&p2, &Stars[i].pos);
			Stars[i].last_star_pos = p2.world;
		}
	}

	int tmp_num_stars = 0;

	tmp_num_stars = (Detail.num_stars * Num_stars) / MAX_DETAIL_VALUE;
	CLAMP(tmp_num_stars, 0, Num_stars);

	auto path = graphics::paths::PathRenderer::instance();

	path->saveState();
	path->resetState();

	path->beginFrame();

	for (i = 0; i < tmp_num_stars; i++) {
		sp = &Stars[i];

		can_draw = 1;
		memset(&p1, 0, sizeof(vertex));
		memset(&p2, 0, sizeof(vertex));

		// This makes a star look "proper" by not translating the
		// point around the viewer's eye before rotation.  In other
		// words, when the ship translates, the stars do not change.

		g3_rotate_faraway_vertex(&p2, &sp->pos);
		if ( p2.codes )	{
			can_draw = 0;
		} else {
			g3_project_vertex(&p2);
			if ( p2.flags & PF_OVERFLOW ) {
				can_draw = 0;
			}
		}

		if ( can_draw && (Star_flags & (STAR_FLAG_TAIL|STAR_FLAG_DIM)) ) {
			dist = vm_vec_dist_quick( &sp->last_star_pos, &p2.world );

			if ( dist > Star_max_length ) {
 				ratio = Star_max_length / dist;
				dist = Star_max_length;
			} else {
				ratio = 1.0f;
			}
			
			ratio *= Star_amount;

			vm_vec_sub(&p1.world, &sp->last_star_pos, &p2.world);
			vm_vec_scale(&p1.world, ratio);
			vm_vec_add2(&p1.world, &p2.world);

			p1.flags = 0;	// not projected
			g3_code_vertex( &p1 );

			if ( p1.codes )	{
				can_draw = 0;
			} else {
				g3_project_vertex(&p1);
				if ( p1.flags & PF_OVERFLOW ) {
					can_draw = 0;
				}
			}
		}

		sp->last_star_pos = p2.world;

		if ( !can_draw )
			continue;

		vDst.x = fl2i(p1.screen.xyw.x) - fl2i(p2.screen.xyw.x);
		vDst.y = fl2i(p1.screen.xyw.y) - fl2i(p2.screen.xyw.y);

		float len = sqrtf((float)((vDst.x * vDst.x) + (vDst.y * vDst.y)));

		color col = sp->col;
		if (len <= 2.0f ) {
			p1.screen.xyw.x = p2.screen.xyw.x + 1.0f;
			p1.screen.xyw.y = p2.screen.xyw.y;
		} else {
			// gamma correction
			col.red = (ubyte)((float)col.red / powf(len, 1.0f / 2.2f));
			col.green = (ubyte)((float)col.green / powf(len, 1.0f / 2.2f));
			col.blue = (ubyte)((float)col.blue / powf(len, 1.0f / 2.2f));
			col.alpha = (ubyte)((float)col.alpha  / powf(len, 1.0f / 2.2f));
		}
		path->beginPath();

		path->moveTo(p1.screen.xyw.x, p1.screen.xyw.y);
		path->lineTo(p2.screen.xyw.x, p2.screen.xyw.y);

		path->setStrokeColor(&col);
		path->stroke();
	}
	path->endFrame();
	
	path->restoreState();
}

void stars_draw_motion_debris()
{
	GR_DEBUG_SCOPE("Draw motion debris");
	TRACE_SCOPE(tracing::DrawMotionDebris);

	if (gr_screen.mode == GR_STUB) {
		return;
	}

	if (Motion_debris_override)
		return;

	if (!Motion_debris_enabled || Motion_debris_info.empty()) {
		return;
	}

	for (motion_debris_instance &mdebris : Motion_debris) {
		float vdist = vm_vec_dist(&mdebris.pos, &Eye_position);

		if ((vdist < MIN_DIST_RANGE) || (vdist > MAX_DIST_RANGE)) {
			// if we just had a camera "cut" and should refresh the debris then generate in the sphere, else just on its surface
			vm_vec_random_in_sphere(&mdebris.pos, &Eye_position, MAX_DIST_RANGE, !refresh_motion_debris);
			vdist = vm_vec_dist(&mdebris.pos, &Eye_position);

			mdebris.vclip = Random::next(MAX_MOTION_DEBRIS_BITMAPS); // rand()

			// if we're in full neb mode
			const float size_multiplier = i2fl(Random::next(4));
			if((The_mission.flags[Mission::Mission_Flags::Fullneb]) && (Neb2_render_mode != NEB2_RENDER_NONE)) {
				mdebris.size = size_multiplier * BASE_SIZE_NEB;
			} else {
				mdebris.size = size_multiplier * BASE_SIZE;
			}
		}

		vertex pnt;
		g3_rotate_vertex(&pnt, &mdebris.pos);

		if (pnt.codes == 0) {
			int frame = Missiontime / (DEBRIS_ROT_MIN + (1 % DEBRIS_ROT_RANGE) * DEBRIS_ROT_RANGE_SCALER);
			frame %= Motion_debris_ptr[mdebris.vclip].nframes;

			float alpha;

			if ( (The_mission.flags[Mission::Mission_Flags::Fullneb]) && (Neb2_render_mode != NEB2_RENDER_NONE) ) {
				alpha = 0.3f;
			} else {
				alpha = 1.0f;
			}

			// scale alpha from 0 at max range to full at 60% range
			alpha *= (vdist - MAX_DIST_RANGE) / -(MAX_DIST_RANGE * 0.6f);

			g3_transfer_vertex(&pnt, &mdebris.pos);

			batching_add_bitmap(Motion_debris_ptr[mdebris.vclip].bm + frame, &pnt, 0, mdebris.size, alpha);
		}
	}

	if (refresh_motion_debris)
		refresh_motion_debris = false;
}

void stars_draw(int show_stars, int show_suns, int  /*show_nebulas*/, int show_subspace, int env, bool in_mission)
{
	GR_DEBUG_SCOPE("Draw Stars");
	TRACE_SCOPE(tracing::DrawStars);

	if (gr_screen.mode == GR_STUB) {
		return;
	}

	int gr_zbuffering_save = gr_zbuffer_get();
	gr_zbuffer_set(GR_ZBUFF_NONE);

	Rendering_to_env = env;

	if (show_subspace)
		subspace_render();

	if (Num_stars >= MAX_STARS)
		Num_stars = MAX_STARS;

#ifdef TIME_STAR_CODE
	fix xt1, xt2;
	xt1 = timer_get_fixed_seconds();
#endif
	
	// draw background stuff
	if ( show_stars ) {
		// semi-hack, do we don't fog the background
		int neb_save = Neb2_render_mode;
		Neb2_render_mode = NEB2_RENDER_NONE;
		stars_draw_background();
		Neb2_render_mode = neb_save;
	}
	else if ( !show_subspace ) // don't render the background pof when rendering subspace
	{
		stars_draw_background();
	}

	if ( !env && show_stars && (Nmodel_num < 0) && (Game_detail_flags & DETAIL_FLAG_STARS) && !(The_mission.flags[Mission::Mission_Flags::Fullneb]) && (supernova_stage() < SUPERNOVA_STAGE::TOOLTIME) ) {
		stars_draw_stars();
	}

	last_stars_filled = 1;

#ifdef TIME_STAR_CODE
	xt2 = timer_get_fixed_seconds();
	mprintf(( "Stars: %d\n", xt2-xt1 ));
#endif

	if ( !Rendering_to_env && (Game_detail_flags & DETAIL_FLAG_MOTION) && (!Fred_running) && (supernova_stage() < SUPERNOVA_STAGE::TOOLTIME) && in_mission)	{
		stars_draw_motion_debris();
	}

	//if we're not drawing them, quit here
	if (show_suns) {
		stars_draw_sun( show_suns );
		stars_draw_bitmaps( show_suns );
	}

	gr_zbuffer_set( gr_zbuffering_save );
	Rendering_to_env = 0;
}

void stars_preload_background(const char *token)
{
	if (!token)
		return;

	// we can only preload raw numbers, but that's probably sufficient
	if (!can_construe_as_integer(token))
		return;

	int background_idx = atoi(token);

	// human/computer offset
	background_idx--;

	// store it for now
	Preload_background_indexes.push_back(background_idx);
}

void stars_preload_background(int background_idx)
{
	// range check
	if (!SCP_vector_inbounds(Backgrounds, background_idx))
		return;

	// preload all the stuff for this background
	for (auto &bgsun : Backgrounds[background_idx].suns)
		stars_preload_sun_bitmap(bgsun.filename);
	for (auto &bitmap : Backgrounds[background_idx].bitmaps)
		stars_preload_background_bitmap(bitmap.filename);
}

void stars_preload_sun_bitmap(const char *fname)
{
	int idx;

	if (fname == NULL)
		return;

	idx = stars_find_sun(fname);

	if (idx == -1) {
		return;
	}

	Sun_bitmaps[idx].preload = 1;
}

void stars_preload_background_bitmap(const char *fname)
{
	int idx;

	if (fname == NULL)
		return;

	idx = stars_find_bitmap(fname);

	if (idx == -1) {
		return;
	}

	Starfield_bitmaps[idx].preload = 1;
}

void stars_page_in()
{
	int idx, i;

	if (gr_screen.mode == GR_STUB) {
		return;
	}

	// Initialize the subspace stuff

	if (Game_subspace_effect || (The_mission.flags[Mission::Mission_Flags::Preload_subspace])) {
		Subspace_model_inner = model_load("subspace_small.pof");
		Assert(Subspace_model_inner >= 0);

		Subspace_model_outer = model_load("subspace_big.pof");
		Assert(Subspace_model_outer >= 0);

		polymodel *pm;
		
		pm = model_get(Subspace_model_inner);
		
		nprintf(( "Paging", "Paging in textures for inner subspace effect.\n" ));

		for (idx = 0; idx < pm->n_textures; idx++) {
			pm->maps[idx].PageIn();
		}

		pm = model_get(Subspace_model_outer);
		
		nprintf(( "Paging", "Paging in textures for outer subspace effect.\n" ));

		for (idx = 0; idx < pm->n_textures; idx++) {
			pm->maps[idx].PageIn();
		}

		if (Subspace_glow_bitmap < 0) {
			Subspace_glow_bitmap = bm_load( NOX("SunGlow01"));
		}

		bm_page_in_xparent_texture(Subspace_glow_bitmap);
	} else {
		Subspace_model_inner = -1;
		Subspace_model_outer = -1;

		if (Subspace_glow_bitmap > 0) {
			bm_release(Subspace_glow_bitmap);
			Subspace_glow_bitmap = -1;
		}
	}

	// extra SEXP related checks to preload anything that might get used from there
	for (auto &sb : Starfield_bitmaps) {
		if (sb.used_this_level)
			continue;

		if (sb.preload) {
			if (sb.bitmap_id < 0) {
				sb.bitmap_id = bm_load(sb.filename);

				// maybe didn't load a static image so try for an animated one
				if (sb.bitmap_id < 0) {
					sb.bitmap_id = bm_load_animation(sb.filename, &sb.n_frames, &sb.fps, nullptr, nullptr, true);

					if (sb.bitmap_id < 0) {
						Warning(LOCATION, "Unable to load starfield bitmap: '%s'!\n", sb.filename);
					}
				}
			}

			// this happens whether it loaded properly or not, no harm should come from it
			if (sb.xparent) {
				bm_page_in_xparent_texture(sb.bitmap_id);
			} else {
				bm_page_in_texture(sb.bitmap_id);
			}

			sb.used_this_level++;
		}
	}

	for (auto &sb : Sun_bitmaps) {
		if (sb.used_this_level)
			continue;

		if (sb.preload) {
			// normal bitmap
			if (sb.bitmap_id < 0) {
				sb.bitmap_id = bm_load(sb.filename);

				// maybe didn't load a static image so try for an animated one
				if (sb.bitmap_id < 0) {
					sb.bitmap_id = bm_load_animation(sb.filename, &sb.n_frames, &sb.fps, nullptr, nullptr, true);

					if (sb.bitmap_id < 0) {
						Warning(LOCATION, "Unable to load sun bitmap: '%s'!\n", sb.filename);
					}
				}
			}

			// glow bitmap
			if (sb.glow_bitmap < 0) {
				sb.glow_bitmap = bm_load(sb.glow_filename);

				// maybe didn't load a static image so try for an animated one
				if (sb.glow_bitmap < 0) {
					sb.glow_bitmap = bm_load_animation(sb.glow_filename, &sb.glow_n_frames, &sb.glow_fps, nullptr, nullptr, true);

					if (sb.glow_bitmap < 0) {
						Warning(LOCATION, "Unable to load sun glow bitmap: '%s'!\n", sb.glow_filename);
					}
				}
			}

			if (sb.flare) {
				for (i = 0; i < MAX_FLARE_BMP; i++) {
					if ( !strlen(sb.flare_bitmaps[i].filename) )
						continue;

					if (sb.flare_bitmaps[i].bitmap_id < 0) {
						sb.flare_bitmaps[i].bitmap_id = bm_load(sb.flare_bitmaps[i].filename);

						if (sb.flare_bitmaps[i].bitmap_id < 0) {
							Warning(LOCATION, "Unable to load sun flare bitmap: '%s'!\n", sb.flare_bitmaps[i].filename);
							continue;
						}
					}

					bm_page_in_texture(sb.flare_bitmaps[i].bitmap_id);
				}
			}

			bm_page_in_texture(sb.bitmap_id);
			bm_page_in_texture(sb.glow_bitmap);

			sb.used_this_level++;
		}
	}

	// load and page in needed starfield bitmaps
	for (auto &sbi : Starfield_bitmap_instances) {
		if (sbi.star_bitmap_index < 0)
			continue;

		auto &sb = Starfield_bitmaps[sbi.star_bitmap_index];

		if (sb.used_this_level)
			continue;

		if (sb.bitmap_id < 0 ) {
			sb.bitmap_id = bm_load(sb.filename);

			// maybe didn't load a static image so try for an animated one
			if (sb.bitmap_id < 0) {
				sb.bitmap_id = bm_load_animation(sb.filename, &sb.n_frames, &sb.fps, nullptr, nullptr, true);

				if (sb.bitmap_id < 0) {
					Warning(LOCATION, "Unable to load starfield bitmap: '%s'!\n", sb.filename);
				}
			}
		}

		// this happens whether it loaded properly or not, no harm should come from it
		if (sb.xparent) {
			bm_page_in_xparent_texture(sb.bitmap_id);
		} else {
			bm_page_in_texture(sb.bitmap_id);
		}

		sb.used_this_level++;
	}

	// now for sun bitmaps and glows
	for (auto &sbi : Suns) {
		if (sbi.star_bitmap_index < 0)
			continue;

		auto &sb = Sun_bitmaps[sbi.star_bitmap_index];

		if (sb.used_this_level)
			continue;

		// normal bitmap
		if (sb.bitmap_id < 0) {
			sb.bitmap_id = bm_load(sb.filename);

			// maybe didn't load a static image so try for an animated one
			if (sb.bitmap_id < 0) {
				sb.bitmap_id = bm_load_animation(sb.filename, &sb.n_frames, &sb.fps, nullptr, nullptr, true);

				if (sb.bitmap_id < 0) {
					Warning(LOCATION, "Unable to load sun bitmap: '%s'!\n", sb.filename);
				}
			}
		}

		// glow bitmap
		if (sb.glow_bitmap < 0) {
			sb.glow_bitmap = bm_load(sb.glow_filename);

			// maybe didn't load a static image so try for an animated one
			if (sb.glow_bitmap < 0) {
				sb.glow_bitmap = bm_load_animation(sb.glow_filename, &sb.glow_n_frames, &sb.glow_fps, nullptr, nullptr, true);

				if (sb.glow_bitmap < 0) {
					Warning(LOCATION, "Unable to load sun glow bitmap: '%s'!\n", sb.glow_filename);
				}
			}
		}

		if (sb.flare) {
			for (i = 0; i < MAX_FLARE_BMP; i++) {
				if ( !strlen(sb.flare_bitmaps[i].filename) )
					continue;

				if (sb.flare_bitmaps[i].bitmap_id < 0) {
					sb.flare_bitmaps[i].bitmap_id = bm_load(sb.flare_bitmaps[i].filename);

					if (sb.flare_bitmaps[i].bitmap_id < 0) {
						Warning(LOCATION, "Unable to load sun flare bitmap: '%s'!\n", sb.flare_bitmaps[i].filename);
						continue;
					}
				}

				bm_page_in_texture(sb.flare_bitmaps[i].bitmap_id);
			}
		}

		bm_page_in_texture(sb.bitmap_id);
		bm_page_in_texture(sb.glow_bitmap);

		sb.used_this_level++;
	}


	if (!Motion_debris_enabled || Motion_debris_override)
		return;

	for (idx = 0; idx < MAX_MOTION_DEBRIS_BITMAPS; idx++) {
		bm_page_in_xparent_texture(Motion_debris_ptr[idx].bm, Motion_debris_ptr[idx].nframes);
	}	
}

// background nebula models and planets
void stars_draw_background()
{	
	GR_DEBUG_SCOPE("Draw Background");
	TRACE_SCOPE(tracing::DrawBackground);

	if (gr_screen.mode == GR_STUB) {
		return;
	}

	if (Nmodel_num < 0)
		return;

	model_render_params render_info;

	if (Nmodel_bitmap >= 0) {
		render_info.set_forced_bitmap(Nmodel_bitmap);
	}

	// draw the model at the player's eye with no z-buffering
	if (Nmodel_alpha < 1.0f)
		render_info.set_alpha_mult(Nmodel_alpha);
	render_info.set_flags(Nmodel_flags | MR_SKYBOX);

	if (Nmodel_instance_num >= 0)
		render_info.set_replacement_textures(model_get_instance(Nmodel_instance_num)->texture_replace);

	model_render_immediate(&render_info, Nmodel_num, Nmodel_instance_num, &Nmodel_orient, &Eye_position, MODEL_RENDER_ALL, false);
}

void stars_set_background_model(int new_model, int new_bitmap, uint64_t flags, float alpha)
{
	if (gr_screen.mode == GR_STUB) {
		return;
	}

	CLAMP(alpha, 0.0f, 1.0f);

	// see if we are actually changing anything
	if (Nmodel_num == new_model && Nmodel_bitmap == new_bitmap && Nmodel_flags == flags && Nmodel_alpha == alpha) {
		return;
	}

	if (Nmodel_bitmap >= 0) {
		bm_unload(Nmodel_bitmap);
		Nmodel_bitmap = -1;
	}

	if (Nmodel_num >= 0) {
		model_unload(Nmodel_num);
		Nmodel_num = -1;
	}

	if (Nmodel_instance_num >= 0) {
		model_delete_instance(Nmodel_instance_num);
		Nmodel_instance_num = -1;
	}

	Nmodel_flags = flags;
	Nmodel_num = new_model;
	Nmodel_bitmap = new_bitmap;
	Nmodel_alpha = alpha;

	if (Nmodel_num >= 0) {
		model_page_in_textures(Nmodel_num);

		Nmodel_instance_num = model_create_instance(model_objnum_special::OBJNUM_NONE, Nmodel_num);
		The_mission.skybox_model_animations.initializeMoveables(model_get_instance(Nmodel_instance_num));
	}

	// Since we have a new skybox we need to rerender the environment map
	stars_invalidate_environment_map();
}

// call this to set a specific model as the background model
void stars_set_background_model(const char* model_name, const char* texture_name, uint64_t flags, float alpha)
{
	int new_model = -1;
	int new_bitmap = -1;

	if (gr_screen.mode == GR_STUB) {
		return;
	}

	if (model_name != nullptr && *model_name != '\0' && stricmp(model_name, "none") != 0) {
		new_model = model_load(model_name, nullptr, ErrorType::NONE);

		if (texture_name != nullptr && *texture_name != '\0') {
			new_bitmap = bm_load(texture_name);
		}
	}

	stars_set_background_model(new_model, new_bitmap, flags, alpha);
}

// call this to set a specific orientation for the background
void stars_set_background_orientation(const matrix *orient)
{
	if (orient == NULL) {
		vm_set_identity(&Nmodel_orient);
	} else {
		Nmodel_orient = *orient;
	}
}

void stars_set_background_alpha(float alpha)
{
	CLAMP(alpha, 0.0f, 1.0f);
	Nmodel_alpha = alpha;
}

// lookup a starfield bitmap, return index or -1 on fail
int stars_find_bitmap(const char *name)
{
	if (name == nullptr)
		return -1;

	// lookup
	for (size_t idx = 0; idx < Starfield_bitmaps.size(); ++idx) {
		if ( !stricmp(name, Starfield_bitmaps[idx].filename) ) {
			return static_cast<int>(idx);
		}
	}

	// not found 
	return -1;
}

// lookup a sun by bitmap filename, return index or -1 on fail
int stars_find_sun(const char *name)
{
	if (name == nullptr)
		return -1;

	// lookup
	for (size_t idx = 0; idx < Sun_bitmaps.size(); ++idx) {
		if ( !stricmp(name, Sun_bitmaps[idx].filename) ) {
			return static_cast<int>(idx);
		}
	}

	// not found 
	return -1;
}

void stars_get_data(bool is_sun, int idx, starfield_list_entry& sle)
{
	const auto& collection = is_sun ? Suns : Starfield_bitmap_instances;

	if (!SCP_vector_inbounds(collection, idx))
		return;

	const auto& item = collection[idx];

	sle.filename[0] = '\0';
	sle.ang = item.ang;
	sle.div_x = item.div_x;
	sle.div_y = item.div_y;
	sle.scale_x = item.scale_x;
	sle.scale_y = item.scale_y;
}

void stars_set_data(bool is_sun, int idx, starfield_list_entry& sle)
{
	auto& collection = is_sun ? Suns : Starfield_bitmap_instances;

	if (!SCP_vector_inbounds(collection, idx))
		return;

	auto& item = collection[idx];

	item.ang = sle.ang;
	item.div_x = sle.div_x;
	item.div_y = sle.div_y;
	item.scale_x = sle.scale_x;
	item.scale_y = sle.scale_y;

	// this is necessary when modifying bitmaps, but not when modifying suns
	if (!is_sun)
		starfield_create_bitmap_buffer(static_cast<size_t>(idx));
}

// add an instance for a sun (something actually used in a mission)
// NOTE that we assume a duplicate is ok here
int stars_add_sun_entry(starfield_list_entry *sun_ptr)
{
	starfield_bitmap_instance sbi;

	Assert(sun_ptr != NULL);

	// copy information
	sbi.ang.p = sun_ptr->ang.p;
	sbi.ang.b = sun_ptr->ang.b;
	sbi.ang.h = sun_ptr->ang.h;
	sbi.scale_x = sun_ptr->scale_x;
	sbi.scale_y = sun_ptr->scale_y;
	sbi.div_x = sun_ptr->div_x;
	sbi.div_y = sun_ptr->div_y;

	int idx = stars_find_sun(sun_ptr->filename);

	if (idx == -1) {
		if (!Fred_running) {
			Warning(LOCATION, "Trying to add a sun '%s' that does not exist in stars.tbl!", sun_ptr->filename);
		}
		return -1;
	}

	sbi.star_bitmap_index = idx;

	// make sure any needed bitmaps are loaded
	if (Sun_bitmaps[idx].bitmap_id < 0) {
		// normal bitmap
		Sun_bitmaps[idx].bitmap_id = bm_load(Sun_bitmaps[idx].filename);

			// maybe didn't load a static image so try for an animated one
		if (Sun_bitmaps[idx].bitmap_id < 0) {
			Sun_bitmaps[idx].bitmap_id = bm_load_animation(Sun_bitmaps[idx].filename, &Sun_bitmaps[idx].n_frames, &Sun_bitmaps[idx].fps, nullptr, nullptr, true);

			if (Sun_bitmaps[idx].bitmap_id < 0) {
				// failed
				return -1;
			}
		}

		// glow bitmap
		if (Sun_bitmaps[idx].glow_bitmap < 0) {
			Sun_bitmaps[idx].glow_bitmap = bm_load(Sun_bitmaps[idx].glow_filename);

			// maybe didn't load a static image so try for an animated one
			if (Sun_bitmaps[idx].glow_bitmap < 0) {
				Sun_bitmaps[idx].glow_bitmap = bm_load_animation(Sun_bitmaps[idx].glow_filename, &Sun_bitmaps[idx].glow_n_frames, &Sun_bitmaps[idx].glow_fps, nullptr, nullptr, true);

				if (Sun_bitmaps[idx].glow_bitmap < 0) {
					Warning(LOCATION, "Unable to load sun glow bitmap: '%s'!\n", Sun_bitmaps[idx].glow_filename);
				}
			}
		}

		if (Sun_bitmaps[idx].flare) {
			for (int i = 0; i < MAX_FLARE_BMP; i++) {
				flare_bitmap* fbp = &Sun_bitmaps[idx].flare_bitmaps[i];
				if ( !strlen(fbp->filename) )
					continue;

				if (fbp->bitmap_id < 0) {
					fbp->bitmap_id = bm_load(fbp->filename);

					if (fbp->bitmap_id < 0) {
						Warning(LOCATION, "Unable to load sun flare bitmap: '%s'!\n", Sun_bitmaps[idx].flare_bitmaps[i].filename);
						continue;
					}
				}
			}
		}
	}

	// The background changed so we need to invalidate the environment map
	stars_invalidate_environment_map();

	// now check if we can make use of a previously discarded instance entry
	// this should never happen with FRED
	if ( !Fred_running ) {
		for (size_t i = 0; i < Suns.size(); ++i) {
			if ( Suns[i].star_bitmap_index < 0 ) {
				Suns[i] = sbi;
				return static_cast<int>(i);
			}
		}
	}

	// ... or add a new one 
	Suns.push_back(sbi);

	return static_cast<int>(Suns.size() - 1);
}

// add an instance for a starfield bitmap (something actually used in a mission)
// NOTE that we assume a duplicate is ok here
int stars_add_bitmap_entry(starfield_list_entry *sle)
{
	int idx;
	starfield_bitmap_instance sbi;

	Assert(sle != NULL);

	// copy information
	sbi.ang.p = sle->ang.p;
	sbi.ang.b = sle->ang.b;
	sbi.ang.h = sle->ang.h;
	sbi.scale_x = sle->scale_x;
	sbi.scale_y = sle->scale_y;
	sbi.div_x = sle->div_x;
	sbi.div_y = sle->div_y;

	idx = stars_find_bitmap(sle->filename);

	if (idx == -1) {
		if (!Fred_running) {
			Warning(LOCATION, "Trying to add a bitmap '%s' that does not exist in stars.tbl!", sle->filename);
		}
		return -1;
	}

	sbi.star_bitmap_index = idx;

	// make sure any needed bitmaps are loaded
	if (Starfield_bitmaps[idx].bitmap_id < 0) {
		Starfield_bitmaps[idx].bitmap_id = bm_load(Starfield_bitmaps[idx].filename);

		// maybe didn't load a static image so try for an animated one
		if (Starfield_bitmaps[idx].bitmap_id < 0) {
			Starfield_bitmaps[idx].bitmap_id = bm_load_animation(Starfield_bitmaps[idx].filename, &Starfield_bitmaps[idx].n_frames, &Starfield_bitmaps[idx].fps, nullptr, nullptr, true);

			if (Starfield_bitmaps[idx].bitmap_id < 0) {
				// failed
				return -1;
			}
		}
	}

	// The background changed so we need to invalidate the environment map
	stars_invalidate_environment_map();

	// now check if we can make use of a previously discarded instance entry
	for (size_t i = 0; i < Starfield_bitmap_instances.size(); ++i) {
		if ( Starfield_bitmap_instances[i].star_bitmap_index < 0 ) {
			Starfield_bitmap_instances[i] = sbi;
			starfield_create_bitmap_buffer(i);
			return static_cast<int>(i);
		}
	}

	// ... or add a new one
	Starfield_bitmap_instances.push_back(sbi);
	starfield_create_bitmap_buffer(Starfield_bitmap_instances.size() - 1);

	return static_cast<int>(Starfield_bitmap_instances.size() - 1);
}

void stars_correct_background_sun_angles(angles* angs_to_correct)
{
	matrix mat;
	vm_angles_2_matrix(&mat, angs_to_correct);
	vm_transpose(&mat);
	vm_extract_angles_matrix(angs_to_correct, &mat);
	angs_to_correct->p = fmod(angs_to_correct->p + PI2, PI2);
	angs_to_correct->b = fmod(angs_to_correct->b + PI2, PI2);
	angs_to_correct->h = fmod(angs_to_correct->h + PI2, PI2);
}

void stars_uncorrect_background_sun_angles(angles* angs_to_uncorrect)
{
	// the actual operation is an inversion so 'correcting' and 'uncorrecting' are the same
	stars_correct_background_sun_angles(angs_to_uncorrect);
}

void stars_correct_background_bitmap_angles(angles *angs_to_correct)
{
	matrix mat1, mat2;
	angles ang1 = vm_angles_new(0.0, angs_to_correct->b, 0.0);
	angles ang2 = vm_angles_new(angs_to_correct->p, 0.0, angs_to_correct->h);
	vm_angles_2_matrix(&mat1, &ang1);
	vm_angles_2_matrix(&mat2, &ang2);
	matrix mat3 = mat2 * mat1;
	vm_transpose(&mat3);
	vm_extract_angles_matrix(angs_to_correct, &mat3);
	angs_to_correct->p = fmod(angs_to_correct->p + PI2, PI2);
	angs_to_correct->b = fmod(angs_to_correct->b + PI2, PI2);
	angs_to_correct->h = fmod(angs_to_correct->h + PI2, PI2);
}

void stars_uncorrect_background_bitmap_angles(angles *angs_to_uncorrect)
{
	matrix mat;
	vm_angles_2_matrix(&mat, angs_to_uncorrect);
	// transpose and {2,3,1} permute rows and cols
	matrix mat_permuted = vm_matrix_new(
		mat.vec.uvec.xyz.y, mat.vec.fvec.xyz.y, mat.vec.rvec.xyz.y,
		mat.vec.uvec.xyz.z, mat.vec.fvec.xyz.z, mat.vec.rvec.xyz.z,
		mat.vec.uvec.xyz.x, mat.vec.fvec.xyz.x, mat.vec.rvec.xyz.x
	);
	angles angs_permuted;
	vm_extract_angles_matrix(&angs_permuted, &mat_permuted);
	// {2,3,1} unpermute p,b,h
	angs_to_uncorrect->p = angs_permuted.b;
	angs_to_uncorrect->h = angs_permuted.p;
	angs_to_uncorrect->b = angs_permuted.h;
}

// get the number of entries that each vector contains
// "is_a_sun" will get sun instance counts, otherwise it gets normal starfield bitmap instance counts
// "bitmap_count" will get number of starfield_bitmap entries rather than starfield_bitmap_instance entries
int stars_get_num_entries(bool is_a_sun, bool bitmap_count)
{
	// try for instance counts first
	if (!bitmap_count) {
		if (is_a_sun) {
			return static_cast<int>(Suns.size());
		} else {
			return static_cast<int>(Starfield_bitmap_instances.size());
		}
	}
	// looks like we want bitmap counts (probably only FRED uses this)
	else {
		if (is_a_sun) {
			return static_cast<int>(Sun_bitmaps.size());
		} else {
			return static_cast<int>(Starfield_bitmaps.size());
		}
	}
}


// get a starfield_bitmap entry providing only an instance
starfield_bitmap *stars_get_bitmap_entry(int index, bool is_a_sun)
{
	int max_index = static_cast<int>(is_a_sun ? Suns.size() : Starfield_bitmap_instances.size());

	//WMC - Commented out because it keeps happening, and I don't know what this means.
	//Assert( (index >= 0) && (index < max_index) );

	if ( (index < 0) || (index >= max_index) )
		return NULL;

	if (is_a_sun && (Suns[index].star_bitmap_index >= 0)) {
		return &Sun_bitmaps[Suns[index].star_bitmap_index];
	} else if (!is_a_sun && (Starfield_bitmap_instances[index].star_bitmap_index >= 0)) {
		return &Starfield_bitmaps[Starfield_bitmap_instances[index].star_bitmap_index];
	}

	return NULL;
}

// set an instace to not render
void stars_mark_instance_unused(int index, bool is_a_sun)
{
	int max_index = static_cast<int>(is_a_sun ? Suns.size() : Starfield_bitmap_instances.size());

	Assert( (index >= 0) && (index < max_index) );

	if ( (index < 0) || (index >= max_index) )
		return;

	if (is_a_sun) {
		Suns[index].star_bitmap_index =  -1;
	} else {
		Starfield_bitmap_instances[index].star_bitmap_index = -1;
	}

	if ( !is_a_sun ) {
		delete [] Starfield_bitmap_instances[index].verts;
		Starfield_bitmap_instances[index].verts = NULL;
	}

	// The background changed so we need to invalidate the environment map
	stars_invalidate_environment_map();
}

// retrieves the name from starfield_bitmap for the instance index
// NOTE: it's unsafe to return NULL here so use <none> for invalid entries
const char *stars_get_name_from_instance(int index, bool is_a_sun)
{
	int max_index = static_cast<int>(is_a_sun ? Suns.size() : Starfield_bitmap_instances.size());

	Assert( (index >= 0) && (index < max_index) );

	if ( (index < 0) || (index >= max_index) )
		return NOX("<none>");

	if (is_a_sun && (Suns[index].star_bitmap_index >= 0)) {
		return Sun_bitmaps[Suns[index].star_bitmap_index].filename;
	} else if (!is_a_sun && (Starfield_bitmap_instances[index].star_bitmap_index >= 0)) {
		return Starfield_bitmaps[Starfield_bitmap_instances[index].star_bitmap_index].filename;
	}

	return NOX("<none>");
}

// WMC/Goober5000
void stars_set_nebula(bool activate, float range)
{
    The_mission.flags.set(Mission::Mission_Flags::Fullneb, activate);
	
    if (activate)
	{
		Toggle_text_alpha = TOGGLE_TEXT_NEBULA_ALPHA;
		HUD_high_contrast = true;

		Neb2_render_mode = NEB2_RENDER_HTL;
		Neb2_awacs = range;
		if (Neb2_awacs < 0.1f)
			Neb2_awacs = 3000.0f;	// this is also the default in the background editor

		// this function is not currently called in FRED, but this would be needed for FRED support
		if (Fred_running)
		{
			Neb2_render_mode = NEB2_RENDER_POF;
			stars_set_background_model(BACKGROUND_MODEL_FILENAME, Neb2_texture_name);
			stars_set_background_orientation();
		}
	}
	else
	{
		Toggle_text_alpha = TOGGLE_TEXT_NORMAL_ALPHA;
		HUD_high_contrast = false;

		Neb2_render_mode = NEB2_RENDER_NONE;
		Neb2_awacs = -1.0f;
	}

	// (DahBlount)
	// This needs to be done regardless of whether we're in nebula or not
	// Should ensure that we're actually loading the needed anims into BMPMan
	stars_load_debris(static_cast<int>(activate));

	// We need to reload the environment map now
	stars_invalidate_environment_map();
}

// retrieves the name from starfield_bitmap, really only used by FRED2
// NOTE: it is unsafe to return NULL here, but because that's bad anyway it really shouldn't happen, so we do return NULL.
const char *stars_get_name_FRED(int index, bool is_a_sun)
{
	if (!Fred_running)
		return NULL;

	int max_index = static_cast<int>(is_a_sun ? Sun_bitmaps.size() : Starfield_bitmaps.size());

	Assert( (index >= 0) && (index < max_index) );

	if ( (index < 0) || (index >= max_index) )
		return NULL;

	if (is_a_sun) {
		return Sun_bitmaps[index].filename;
	} else {
		return Starfield_bitmaps[index].filename;
	}
}

// modify an existing starfield bitmap instance, or add a new one if needed
void stars_modify_entry_FRED(int index, const char *name, starfield_list_entry *sbi_new, bool is_a_sun)
{
	if (!Fred_running)
		return;

	starfield_bitmap_instance sbi;
	int idx;
	int add_new = index > static_cast<int>(is_a_sun ? Sun_bitmaps.size() : Starfield_bitmaps.size());

	Assert( index >= 0 );
	Assert( sbi_new != NULL );

    // copy information
    sbi.ang.p = sbi_new->ang.p;
	sbi.ang.b = sbi_new->ang.b;
	sbi.ang.h = sbi_new->ang.h;
	sbi.scale_x = sbi_new->scale_x;
	sbi.scale_y = sbi_new->scale_y;
	sbi.div_x = sbi_new->div_x;
	sbi.div_y = sbi_new->div_y;

	if (is_a_sun) {
		idx = stars_find_sun((char*)name);
	} else {
		idx = stars_find_bitmap((char*)name);
	}

	// this shouldn't ever happen from FRED since you select the name from a list of those available
	if (idx == -1)
		return;

	sbi.star_bitmap_index = idx;

	if (add_new) {
		if (is_a_sun) {
			Suns.push_back( sbi );
		} else {
			Starfield_bitmap_instances.push_back( sbi );
		}
	} else {
		if (is_a_sun) {
			Suns[index] = sbi;
		} else {
			Starfield_bitmap_instances[index] = sbi;
		}
	}

	if ( !is_a_sun ) {
		starfield_create_bitmap_buffer(static_cast<size_t>(index));
	}
}

// erase an instance, note that this is very slow so it should only be done in FRED
void stars_delete_entry_FRED(int index, bool is_a_sun)
{
	if (!Fred_running)
		return;

	int max_index = static_cast<int>(is_a_sun ? Suns.size() : Starfield_bitmap_instances.size());

	Assert( (index >= 0) && (index < max_index) );

	if ( (index < 0) || (index >= max_index) )
		return;

	if (is_a_sun) {
		Suns.erase( Suns.begin() + index );
	} else {
		Starfield_bitmap_instances.erase( Starfield_bitmap_instances.begin() + index );
	}
}

// Goober5000
void stars_add_blank_background(bool creating_in_fred)
{
	Backgrounds.emplace_back();

	// any background that is created will have correct angles, so be sure to set the flag
	if (creating_in_fred)
		Backgrounds.back().flags.set(Starfield::Background_Flags::Corrected_angles_in_mission_file);
}

// Goober5000
void stars_load_first_valid_background()
{
	int background_idx = stars_get_first_valid_background();
	stars_load_background(background_idx);
}

// Goober5000
int stars_get_first_valid_background()
{
	size_t i, j;

	if (Backgrounds.empty())
		return -1;

	// scan every background except the last and return the first one that has all its suns and bitmaps present
	for (i = 0; i < Backgrounds.size() - 1; i++)
	{
		bool valid = true;
		background_t *background = &Backgrounds[i];

		for (j = 0; j < background->suns.size(); j++)
		{
			if (stars_find_sun(background->suns[j].filename) < 0)
			{
				mprintf(("Failed to load sun %s for background " SIZE_T_ARG ", falling back to background " SIZE_T_ARG "\n",
					background->suns[j].filename, i + 1, i + 2));
				valid = false;
				break;
			}
		}

		if (valid)
		{
			for (j = 0; j < background->bitmaps.size(); j++)
			{
				if (stars_find_bitmap(background->bitmaps[j].filename) < 0)
				{
					mprintf(("Failed to load bitmap %s for background " SIZE_T_ARG ", falling back to background " SIZE_T_ARG "\n",
						background->bitmaps[j].filename, i + 1, i + 2));
					valid = false;
					break;
				}
			}
		}

		if (valid)
			return static_cast<int>(i);
	}

	// didn't find a valid background yet, so return the last one
	return static_cast<int>(Backgrounds.size() - 1);
}

// Goober5000
void stars_load_background(int background_idx)
{
	size_t j;

	stars_clear_instances();
	Cur_background = background_idx;

	if (Cur_background >= 0)
	{
		background_t *background = &Backgrounds[Cur_background];

		int failed_suns = 0;
		for (j = 0; j < background->suns.size(); j++)
		{
			if ((stars_add_sun_entry(&background->suns[j]) < 0) && !Fred_running)
			{
				nprintf(("General", "Failed to add sun '%s' to the mission!", background->suns[j].filename));
				failed_suns++;
			}
		}
		if (failed_suns > 0)
			Warning(LOCATION, "Failed to add %d sun bitmaps to the mission!", failed_suns);

		int failed_stars = 0;
		for (j = 0; j < background->bitmaps.size(); j++)
		{
			if ((stars_add_bitmap_entry(&background->bitmaps[j]) < 0) && !Fred_running)
			{
				nprintf(("General", "Failed to add starfield bitmap '%s' to the mission!", background->bitmaps[j].filename));
				failed_stars++;
			}
		}
		if (failed_stars > 0)
			Warning(LOCATION, "Failed to add %d starfield bitmaps to the mission!", failed_stars);
	}
}

// Goober5000
void stars_copy_background(background_t *dest, background_t *src)
{
	dest->flags = src->flags;
	dest->suns.assign(src->suns.begin(), src->suns.end());
	dest->bitmaps.assign(src->bitmaps.begin(), src->bitmaps.end());
}

// Goober5000
void stars_swap_backgrounds(int idx1, int idx2)
{
	background_t temp;
	stars_copy_background(&temp, &Backgrounds[idx1]);
	stars_copy_background(&Backgrounds[idx1], &Backgrounds[idx2]);
	stars_copy_background(&Backgrounds[idx2], &temp);
}

// Goober5000
bool stars_background_empty(const background_t &bg)
{
	return (bg.suns.empty() && bg.bitmaps.empty());
}

// Goober5000
void stars_pack_backgrounds()
{
	size_t remove_count = 0;

	// remove all empty backgrounds, with a caveat:
	// in FRED, make sure we always have at least one background
	// (note: older code removed all blank backgrounds and re-added one if necessary; but that method caused flag changes to be lost)
	Backgrounds.erase(
		std::remove_if(Backgrounds.begin(), Backgrounds.end(), [&](const background_t& bg) {
			if (stars_background_empty(bg)) {
				if (Fred_running && ++remove_count == Backgrounds.size())
					return false;	// cancel the last removal if FRED is running and if the last removal would result in all backgrounds being removed (i.e. all were blank)
				return true;
			}
			return false;
		}), Backgrounds.end());
}

static void render_environment(int i, vec3d *eye_pos, matrix *new_orient, fov_t new_zoom)
{
	bm_set_render_target(gr_screen.envmap_render_target, i);

	gr_clear();

	g3_set_view_matrix( eye_pos, new_orient, new_zoom );

	gr_set_proj_matrix( new_zoom * PI_2, 1.0f, Min_draw_distance, Max_draw_distance);
	gr_set_view_matrix( &Eye_position, &Eye_matrix );

	if ( Game_subspace_effect ) {
		stars_draw(0, 0, 0, 1, 1);
	} else {
		stars_draw(0, 1, 1, 0, 1);
	}

	gr_end_view_matrix();
	gr_end_proj_matrix();
}

void stars_setup_environment_mapping(camid cid) {
	matrix new_orient = IDENTITY_MATRIX;

	extern fov_t View_zoom;
	fov_t old_zoom = View_zoom, new_zoom = 1.0f;//0.925f;

	if (gr_screen.mode == GR_STUB) {
		return;
	}

	if(!cid.isValid())
		return;

	vec3d cam_pos;
	matrix cam_orient;
	cid.getCamera()->get_info(&cam_pos, &cam_orient);

	bool renderEnv = true;

	// prefer the mission specified envmap over the static-generated envmap, but
	// the dynamic envmap should always get preference if in a subspace mission
	if (!Dynamic_environment && Mission_env_map >= 0) {
		ENVMAP = Mission_env_map;
		renderEnv = false;
	} else if (gr_screen.envmap_render_target < 0) {
		if (ENVMAP >= 0) {
			renderEnv = false;
		} else if (Mission_env_map >= 0) {
			ENVMAP = Mission_env_map;
		} else {
			ENVMAP = Default_env_map;
		}
		renderEnv = false;
	} else if (Env_cubemap_drawn) {
		// Nothing to do here anymore
		renderEnv = false;
	}

			/*
	 * Envmap matrix setup -- left-handed
	 * -------------------------------------------------
	 * Face --	Forward		Up		Right
	 * px		+X			+Y		-Z
	 * nx		-X			+Y		+Z
	 * py		+Y			-Z		+X
	 * ny		-Y			+Z		+X
	 * pz		+Z 			+Y		+X
	 * nz		-Z			+Y		-X
	 */
	// NOTE: OpenGL needs up/down reversed

	// Save the previous render target so we can reset it once we are done here
	auto previous_target = gr_screen.rendering_to_texture;
	// Encode above table into directions and values.
	// 0 = X, 1 = Y, 2 = Z
	int f_dir[6] = {0, 0, 1, 1, 2, 2};
	int u_dir[6] = {1, 1, 2, 2, 1, 1};
	int r_dir[6] = {2, 2, 0, 0, 0, 0};
	float f_val[6] = {1.0f, -1.0f, 1.0f, -1.0f, 1.0f, -1.0f};
	float u_val[6] = {1.0f, 1.0f, -1.0f, 1.0f, 1.0f, 1.0f};
	float r_val[6] = {-1.0f, 1.0f, 1.0f, 1.0f, 1.0f, -1.0f};

	if (renderEnv) {

		GR_DEBUG_SCOPE("Environment Mapping");
		TRACE_SCOPE(tracing::EnvironmentMapping);

		ENVMAP = gr_screen.envmap_render_target;

		for (int i = 0; i < 6; i++) {
			memset(&new_orient, 0, sizeof(matrix));
			new_orient.vec.fvec.a1d[f_dir[i]] = f_val[i];
			new_orient.vec.uvec.a1d[u_dir[i]] = u_val[i];
			new_orient.vec.rvec.a1d[r_dir[i]] = r_val[i];
			render_environment(i, &cam_pos, &new_orient, new_zoom);
		}

		// we're done, so now reset
		bm_set_render_target(previous_target);
		g3_set_view_matrix(&cam_pos, &cam_orient, old_zoom);
	}
	// Draw irr map if we've just updated the envmap 
	// or otherwise invalidated the irradiance map (i.e. custom/default envmap)
	if ((!Irr_cubemap_drawn || renderEnv) && (ENVMAP >= 0)) {
		// Generate irradiance map.
		if (gr_screen.irrmap_render_target < 0) {
			irradiance_map_gen();
			IRRMAP = gr_screen.irrmap_render_target;
		}
		gr_screen.gf_calculate_irrmap();
	}

	if ( !Dynamic_environment ) {
		Env_cubemap_drawn = true;
		Irr_cubemap_drawn = true;
	}
}
void stars_set_dynamic_environment(bool dynamic) {
	Dynamic_environment = dynamic;
	stars_invalidate_environment_map();
}
void stars_invalidate_environment_map() {
	// This will cause a redraw in the next frame
	Env_cubemap_drawn = false;
	Irr_cubemap_drawn = false;
}

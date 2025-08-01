/*
 * Copyright (C) Volition, Inc. 1999.  All rights reserved.
 *
 * All source code herein is the property of Volition, Inc. You may not sell
 * or otherwise commercially exploit the source or things you created based on the
 * source.
 *
 */

#include <freespace.h>

#include <ai/aigoals.h>
#include <ai/ailua.h>
#include <asteroid/asteroid.h>
#include <cfile/cfile.h>
#include <hud/hudsquadmsg.h>
#include <gamesnd/eventmusic.h>
#include <globalincs/alphacolors.h>
#include <globalincs/linklist.h>
#include <globalincs/version.h>
#include <iff_defs/iff_defs.h>
#include <jumpnode/jumpnode.h>
#include <lighting/lighting_profiles.h>
#include <localization/fhash.h>
#include <localization/localize.h>
#include <math/bitarray.h>
#include <mission/missionbriefcommon.h>
#include <mission/missioncampaign.h>
#include <mission/missiongoals.h>
#include <mission/missionmessage.h>
#include <mission/missionparse.h>
#include <missionui/fictionviewer.h>
#include <missionui/missioncmdbrief.h>
#include <nebula/neb.h>
#include <object/objectdock.h>
#include <object/objectshield.h>
#include <parse/sexp_container.h>
#include <sound/ds.h>
#include <starfield/nebula.h>
#include <starfield/starfield.h>
#include <weapon/weapon.h>

#include "missionsave.h"
#include "util.h"

namespace fso {
namespace fred {

#define FRED_ENSURE_PROPERTY_VERSION(property, comments, formatstr, ...) do {\
	if (optional_string_fred(property)) \
		parse_comments(comments); \
	else \
		fout_version("\n" property); \
	fout(formatstr, __VA_ARGS__); \
} while(false)
#define FRED_ENSURE_PROPERTY_VERSION_WITH_DEFAULT(property, comments, expected_version, default, formatstr, value) do {\
	if (value != default) { \
		FRED_ENSURE_PROPERTY_VERSION(property, comments, formatstr, value); \
	} else \
		bypass_comment(expected_version " " property); \
} while(false)
#define FRED_ENSURE_PROPERTY_VERSION_WITH_DEFAULT_F(property, comments, expected_version, default, formatstr, value) do {\
	if (!fl_equal(value, default)) { \
		FRED_ENSURE_PROPERTY_VERSION(property, comments, formatstr, value); \
	} else \
		bypass_comment(expected_version " " property); \
} while(false)
#define FRED_ENSURE_PROPERTY_VERSION_IF(property, comments, expected_version, ifcase, formatstr, ...) do {\
	if (ifcase) { \
		FRED_ENSURE_PROPERTY_VERSION(property, comments, formatstr, __VA_ARGS__); \
	} else \
		bypass_comment(expected_version " " property); \
} while(false)

int CFred_mission_save::autosave_mission_file(char* pathname)
{
	char backup_name[256], name2[256];
	int i;

	auto len = strlen(pathname);
	strcpy_s(backup_name, pathname);
	strcpy_s(name2, pathname);
	sprintf(backup_name + len, ".%.3d", MISSION_BACKUP_DEPTH);
	cf_delete(backup_name, CF_TYPE_MISSIONS);
	for (i = MISSION_BACKUP_DEPTH; i > 1; i--) {
		sprintf(backup_name + len, ".%.3d", i - 1);
		sprintf(name2 + len, ".%.3d", i);
		cf_rename(backup_name, name2, CF_TYPE_MISSIONS);
	}

	strcpy(backup_name + len, ".001");

	save_mission_internal(backup_name);

	return err;
}

void CFred_mission_save::bypass_comment(const char* comment, const char* end)
{
	char* ch = strstr(raw_ptr, comment);
	if (ch != NULL) {
		if (end != NULL) {
			char* ep = strstr(raw_ptr, end);
			if (ep != NULL && ep < ch) {
				return;
			}
		}
		char* writep = ch;
		char* readp = strchr(writep, '\n');

		// copy all characters past it
		while (*readp != '\0') {
			*writep = *readp;

			writep++;
			readp++;
		}

		*writep = '\0';
	}
}

void CFred_mission_save::convert_special_tags_to_retail(char* text, int max_len)
{
	replace_all(text, "$quote", "''", max_len);
	replace_all(text, "$semicolon", ",", max_len);
}

void CFred_mission_save::convert_special_tags_to_retail(SCP_string& text)
{
	replace_all(text, "$quote", "''");
	replace_all(text, "$semicolon", ",");
}

void CFred_mission_save::convert_special_tags_to_retail()
{
	int i, team, stage;

	if (save_format != MissionFormat::RETAIL) {
		return;
	}

	for (team = 0; team < Num_teams; team++) {
		// command briefing
		for (stage = 0; stage < Cmd_briefs[team].num_stages; stage++) {
			convert_special_tags_to_retail(Cmd_briefs[team].stage[stage].text);
		}

		// briefing
		for (stage = 0; stage < Briefings[team].num_stages; stage++) {
			convert_special_tags_to_retail(Briefings[team].stages[stage].text);
		}

		// debriefing
		for (stage = 0; stage < Debriefings[team].num_stages; stage++) {
			convert_special_tags_to_retail(Debriefings[team].stages[stage].text);
		}
	}

	for (i = Num_builtin_messages; i < Num_messages; i++) {
		convert_special_tags_to_retail(Messages[i].message, MESSAGE_LENGTH - 1);
	}
}

int CFred_mission_save::fout(const char* format, ...)
{
	// don't output anything if we're saving in retail and have version-specific comments active
	if (save_format == MissionFormat::RETAIL && !fso_ver_comment.empty()) {
		return 0;
	}

	SCP_string str;
	va_list args;

	if (err) {
		return err;
	}

	va_start(args, format);
	vsprintf(str, format, args);
	va_end(args);

	cfputs(str.c_str(), fp);
	return 0;
}

int CFred_mission_save::fout_ext(const char* pre_str, const char* format, ...)
{
	// don't output anything if we're saving in retail and have version-specific comments active
	if (save_format == MissionFormat::RETAIL && !fso_ver_comment.empty()) {
		return 0;
	}

	SCP_string str_scp;
	SCP_string str_out_scp;
	va_list args;
	int str_id;

	if (err) {
		return err;
	}

	va_start(args, format);
	vsprintf(str_scp, format, args);
	va_end(args);

	if (pre_str) {
		str_out_scp = pre_str;
	}

	// lookup the string in the hash table
	str_id = fhash_string_exists(str_scp.c_str());

	// doesn't exist, so assign it an ID of -1 and stick it in the table
	if (str_id <= -2) {
		str_out_scp += " XSTR(\"";
		str_out_scp += str_scp;
		str_out_scp += "\", -1)";

		// add the string to the table		
		fhash_add_str(str_scp.c_str(), -1);
	}
		// _does_ exist, so just write it out as it is
	else {
		char buf[10];
		sprintf_safe(buf, "%d", str_id);

		str_out_scp += " XSTR(\"";
		str_out_scp += str_scp;
		str_out_scp += "\", ";
		str_out_scp += buf;
		str_out_scp += ")";
	}

	char* str_out_c = vm_strdup(str_out_scp.c_str());

	// this could be a multi-line string, so we've got to handle it all properly
	if (!fso_ver_comment.empty()) {
		bool first_line = true;
		char* str_p = str_out_c;

		char* ch = strchr(str_out_c, '\n');

		// if we have something, and it's not just at the end, then process it specially
		if ((ch != NULL) && (*(ch + 1) != '\0')) {
			do {
				if (*(ch + 1) != '\0') {
					*ch = '\0';

					if (first_line) {
						first_line = false;
					} else {
						cfputs(fso_ver_comment.back().c_str(), fp);
						cfputs(" ", fp);
					}

					cfputs(str_p, fp);
					cfputc('\n', fp);

					str_p = ch + 1;
				} else {
					if (first_line) {
						first_line = false;
					} else {
						cfputs(fso_ver_comment.back().c_str(), fp);
						cfputs(" ", fp);
					}

					cfputs(str_p, fp);

					str_p = ch + 1;

					break;
				}
			} while ((ch = strchr(str_p, '\n')) != NULL);

			// be sure to account for any ending elements too
			if (strlen(str_p)) {
				cfputs(fso_ver_comment.back().c_str(), fp);
				cfputs(" ", fp);
				cfputs(str_p, fp);
			}

			vm_free(str_out_c);
			return 0;
		}
	}

	cfputs(str_out_c, fp);

	vm_free(str_out_c);
	return 0;
}

int CFred_mission_save::fout_version(const char* format, ...)
{
	SCP_string str_scp;
	char* ch = NULL;
	va_list args;

	if (err) {
		return err;
	}

	// don't output anything if we're saving in retail and have version-specific comments active
	if (save_format == MissionFormat::RETAIL && !fso_ver_comment.empty()) {
		return 0;
	}

	// output the version first thing, but skip the special case where we use
	// fout_version() for multiline value strings (typically indicated by an initial space)
	if ((save_format == MissionFormat::COMPATIBILITY_MODE) && (*format != ' ') && !fso_ver_comment.empty()) {
		while (*format == '\n') {
			str_scp.append(1, *format);
			format++;
		}

		str_scp.append(fso_ver_comment.back().c_str());
		str_scp.append(" ");

		cfputs(str_scp.c_str(), fp);

		str_scp = "";
	}

	va_start(args, format);
	vsprintf(str_scp, format, args);
	va_end(args);

	char* str_c = vm_strdup(str_scp.c_str());

	// this could be a multi-line string, so we've got to handle it all properly
	if ((save_format == MissionFormat::COMPATIBILITY_MODE) && !fso_ver_comment.empty()) {
		bool first_line = true;
		char* str_p = str_c;

		ch = strchr(str_c, '\n');

		// if we have something, and it's not just at the end, then process it specially
		if ((ch != NULL) && (*(ch + 1) != '\0')) {
			do {
				if (*(ch + 1) != '\0') {
					*ch = '\0';

					if (first_line) {
						first_line = false;
					} else {
						cfputs(fso_ver_comment.back().c_str(), fp);
						cfputs(" ", fp);
					}

					cfputs(str_p, fp);
					cfputc('\n', fp);

					str_p = ch + 1;
				} else {
					if (first_line) {
						first_line = false;
					} else {
						cfputs(fso_ver_comment.back().c_str(), fp);
						cfputs(" ", fp);
					}

					cfputs(str_p, fp);

					str_p = ch + 1;

					break;
				}
			} while ((ch = strchr(str_p, '\n')) != NULL);

			// be sure to account for any ending elements too
			if (strlen(str_p)) {
				cfputs(fso_ver_comment.back().c_str(), fp);
				cfputs(" ", fp);
				cfputs(str_p, fp);
			}

			vm_free(str_c);
			return 0;
		}
	}

	cfputs(str_c, fp);

	vm_free(str_c);
	return 0;
}

void CFred_mission_save::fout_raw_comment(const char *comment_start)
{
	Assertion(comment_start <= raw_ptr, "This function assumes the beginning of the comment precedes the current raw pointer!");

	// the current character is \n, so either set it to 0, or set the preceding \r (if there is one) to 0
	if (*(raw_ptr - 1) == '\r') {
		*(raw_ptr - 1) = '\0';
	} else {
		*raw_ptr = '\0';
	}

	// save the comment, which will write all characters up to the 0 we just set
	fout("%s\n", comment_start);

	// restore the overwritten character
	if (*(raw_ptr - 1) == '\0') {
		*(raw_ptr - 1) = '\r';
	} else {
		*raw_ptr = '\n';
	}
}

void CFred_mission_save::parse_comments(int newlines)
{
	char* comment_start = NULL;
	int state = 0, same_line = 0, first_comment = 1, tab = 0, flag = 0;
	bool version_added = false;

	if (newlines < 0) {
		newlines = -newlines;
		tab = 1;
	}

	if (newlines) {
		same_line = 1;
	}

	if (fred_parse_flag || !Token_found_flag || !token_found || (token_found && (*Parse_text_raw == '\0'))) {
		while (newlines-- > 0) {
			fout("\n");
		}

		if (tab && token_found) {
			fout_version("\t%s", token_found);
		} else if (token_found) {
			fout_version("%s", token_found);
		} else if (tab) {
			fout("\t");
		}

		return;
	}

	while (*raw_ptr != '\0') {
		// state values (as far as I could figure out):
		// 0 - raw_ptr not inside comment
		// 1 - raw_ptr inside /**/ comment
		// 2 - raw_ptr inside ; (newline-delimited) comment
		// 3,4 - raw_ptr inside ;; (FSO version) comment
		if (!state) {
			if (token_found && (*raw_ptr == *token_found)) {
				if (!strnicmp(raw_ptr, token_found, strlen(token_found))) {
					same_line = newlines - 1 + same_line;
					while (same_line-- > 0) {
						fout("\n");
					}

					if (tab) {
						fout_version("\t");
						fout("%s", token_found);
					} else {
						fout_version("%s", token_found);
					}

					// If you have a bunch of lines that all start with the same token (like, say, "+Subsystem:"),
					// this makes it so it won't just repeatedly match the first one. -MageKing17
					raw_ptr++;

					if (version_added) {
						fso_comment_pop();
					}

					return;
				}
			}

			if ((*raw_ptr == '/') && (raw_ptr[1] == '*')) {
				comment_start = raw_ptr;
				state = 1;
			}

			if ((*raw_ptr == ';') && (raw_ptr[1] != '!')) {
				comment_start = raw_ptr;
				state = 2;

				// check for a FSO version comment, but if we can't understand it then
				// just handle it as a regular comment
				if ((raw_ptr[1] == ';') && (raw_ptr[2] == 'F') && (raw_ptr[3] == 'S') && (raw_ptr[4] == 'O')) {
					int major, minor, build, revis;
					int s_num = scan_fso_version_string(raw_ptr, &major, &minor, &build, &revis);

					// hack for releases
					if (FS_VERSION_REVIS < 1000) {
						s_num = 3;
					}

					if ((s_num == 3) && ((major < FS_VERSION_MAJOR) || ((major == FS_VERSION_MAJOR)
						&& ((minor < FS_VERSION_MINOR)
							|| ((minor == FS_VERSION_MINOR) && (build <= FS_VERSION_BUILD)))))) {
						state = 3;
					} else if ((s_num == 4) && ((major < FS_VERSION_MAJOR) || ((major == FS_VERSION_MAJOR)
						&& ((minor < FS_VERSION_MINOR) || ((minor == FS_VERSION_MINOR) && ((build < FS_VERSION_BUILD)
							|| ((build == FS_VERSION_BUILD) && (revis <= FS_VERSION_REVIS)))))))) {
						state = 3;
					} else {
						state = 4;
					}
				}
			}

			if (*raw_ptr == '\n') {
				flag = 1;
			}

			if (flag && state && !(state == 3)) {
				fout("\n");
			}

		} else {
			if (*raw_ptr == '\n') {
				if (state == 2) {
					if (first_comment && !flag) {
						fout("\t\t");
					}
					fout_raw_comment(comment_start);

					state = first_comment = same_line = flag = 0;
				} else if (state == 4) {
					same_line = newlines - 2 + same_line;
					while (same_line-- > 0) {
						fout("\n");
					}
					fout_raw_comment(comment_start);

					state = first_comment = same_line = flag = 0;
				}
			}

			if ((*raw_ptr == '*') && (raw_ptr[1] == '/') && (state == 1)) {
				if (first_comment && !flag) {
					fout("\t\t");
				}

				const char tmp = raw_ptr[2];
				raw_ptr[2] = 0;
				fout("%s", comment_start);
				raw_ptr[2] = tmp;
				state = first_comment = flag = 0;
			}

			if ((*raw_ptr == ';') && (raw_ptr[1] == ';') && (state == 3)) {
				const char tmp = raw_ptr[2];
				raw_ptr[2] = 0;
				if (version_added) {
					fso_comment_pop();
				} else {
					version_added = true;
				}
				fso_comment_push(comment_start);
				raw_ptr[2] = tmp;
				state = first_comment = flag = 0;
				raw_ptr++;
			}
		}

		raw_ptr++;
	}

	if (version_added) {
		fso_comment_pop();
	}

	return;
}

void CFred_mission_save::save_ai_goals(ai_goal* goalp, int ship)
{
	const char* str = NULL;
	char buf[80];
	int i, valid, flag = 1;

	for (i = 0; i < MAX_AI_GOALS; i++) {
		if (goalp[i].ai_mode == AI_GOAL_NONE) {
			continue;
		}

		if (flag) {
			if (optional_string_fred("$AI Goals:", "$Name:")) {
				parse_comments();
			} else {
				fout("\n$AI Goals:");
			}

			fout(" ( goals ");
			flag = 0;
		}

		if (goalp[i].ai_mode == AI_GOAL_CHASE_ANY) {
			fout("( ai-chase-any %d ) ", goalp[i].priority);

		} else if (goalp[i].ai_mode == AI_GOAL_UNDOCK) {
			fout("( ai-undock %d ) ", goalp[i].priority);

		} else if (goalp[i].ai_mode == AI_GOAL_KEEP_SAFE_DISTANCE) {
			fout("( ai-keep-safe-distance %d ) ", goalp[i].priority);

		} else if (goalp[i].ai_mode == AI_GOAL_PLAY_DEAD) {
			fout("( ai-play-dead %d ) ", goalp[i].priority);

		} else if (goalp[i].ai_mode == AI_GOAL_PLAY_DEAD_PERSISTENT) {
			fout("( ai-play-dead-persistent %d ) ", goalp[i].priority);

		} else if (goalp[i].ai_mode == AI_GOAL_WARP) {
			fout("( ai-warp-out %d ) ", goalp[i].priority);

		} else {
			valid = 1;
			if (!goalp[i].target_name) {
				Warning(LOCATION, "Ai goal has no target where one is required");

			} else {
				sprintf(buf, "\"%s\"", goalp[i].target_name);
				switch (goalp[i].ai_mode) {
				case AI_GOAL_WAYPOINTS:
					str = "ai-waypoints";
					break;

				case AI_GOAL_WAYPOINTS_ONCE:
					str = "ai-waypoints-once";
					break;

				case AI_GOAL_DESTROY_SUBSYSTEM:
					if (goalp[i].docker.index == -1 || !goalp[i].docker.index) {
						valid = 0;
						Warning(LOCATION, "AI destroy subsystem goal invalid subsystem name\n");

					} else {
						sprintf(buf, "\"%s\" \"%s\"", goalp[i].target_name, goalp[i].docker.name);
						str = "ai-destroy-subsystem";
					}

					break;

				case AI_GOAL_DOCK:
					if (ship < 0) {
						valid = 0;
						Warning(LOCATION, "Wings aren't allowed to have a docking goal\n");

					} else if (goalp[i].docker.index == -1 || !goalp[i].docker.index) {
						valid = 0;
						Warning(LOCATION, "AI dock goal for \"%s\" has invalid docker point "
										  "(docking with \"%s\")\n", Ships[ship].ship_name, goalp[i].target_name);

					} else if (goalp[i].dockee.index == -1 || !goalp[i].dockee.index) {
						valid = 0;
						Warning(LOCATION, "AI dock goal for \"%s\" has invalid dockee point "
										  "(docking with \"%s\")\n", Ships[ship].ship_name, goalp[i].target_name);

					} else {
						sprintf(buf,
								"\"%s\" \"%s\" \"%s\"",
								goalp[i].target_name,
								goalp[i].docker.name,
								goalp[i].dockee.name);

						str = "ai-dock";
					}
					break;

				case AI_GOAL_CHASE:
					str = "ai-chase";
					break;

				case AI_GOAL_CHASE_WING:
					str = "ai-chase-wing";
					break;

				case AI_GOAL_CHASE_SHIP_CLASS:
					str = "ai-chase-ship-class";
					break;

				case AI_GOAL_GUARD:
					str = "ai-guard";
					break;

				case AI_GOAL_GUARD_WING:
					str = "ai-guard-wing";
					break;

				case AI_GOAL_DISABLE_SHIP:
					str = "ai-disable-ship";
					break;

				case AI_GOAL_DISABLE_SHIP_TACTICAL:
					str = "ai-disable-ship-tactical";
					break;

				case AI_GOAL_DISARM_SHIP:
					str = "ai-disarm-ship";
					break;

				case AI_GOAL_DISARM_SHIP_TACTICAL:
					str = "ai-disarm-ship-tactical";
					break;

				case AI_GOAL_IGNORE:
					str = "ai-ignore";
					break;

				case AI_GOAL_IGNORE_NEW:
					str = "ai-ignore-new";
					break;

				case AI_GOAL_EVADE_SHIP:
					str = "ai-evade-ship";
					break;

				case AI_GOAL_STAY_NEAR_SHIP:
					str = "ai-stay-near-ship";
					break;

				case AI_GOAL_STAY_STILL:
					str = "ai-stay-still";
					break;

				case AI_GOAL_REARM_REPAIR:
					str = "ai-rearm-repair";
					break;

				case AI_GOAL_FLY_TO_SHIP:
					str = "ai-fly-to-ship";
					break;

				default:
					UNREACHABLE("Goal %d not handled!", goalp[i].ai_mode);
				}

				if (valid) {
					fout("( %s %s %d ) ", str, buf, goalp[i].priority);
				}
			}
		}

		fso_comment_pop();
	}

	if (!flag) {
		fout(")");
	}

	fso_comment_pop(true);
}

int CFred_mission_save::save_asteroid_fields()
{
	int i;

	fred_parse_flag = 0;
	required_string_fred("#Asteroid Fields");
	parse_comments(2);

	for (i = 0; i < 1 /*MAX_ASTEROID_FIELDS*/; i++) {
		if (!Asteroid_field.num_initial_asteroids) {
			continue;
		}

		required_string_fred("$Density:");
		parse_comments(2);
		fout(" %d", Asteroid_field.num_initial_asteroids);

		// field type
		if (optional_string_fred("+Field Type:")) {
			parse_comments();
		} else {
			fout("\n+Field Type:");
		}
		fout(" %d", Asteroid_field.field_type);

		// debris type
		if (optional_string_fred("+Debris Genre:")) {
			parse_comments();
		} else {
			fout("\n+Debris Genre:");
		}
		fout(" %d", Asteroid_field.debris_genre);

		// field_debris_type (only if debris genre)
		if (Asteroid_field.debris_genre == DG_DEBRIS) {
			for (size_t idx = 0; idx < Asteroid_field.field_debris_type.size(); idx++) {
				if (Asteroid_field.field_debris_type[idx] != -1) {

					if (save_format == MissionFormat::RETAIL) {
						if (idx < MAX_RETAIL_DEBRIS_TYPES) { // Retail can only have 3!
							if (optional_string_fred("+Field Debris Type:")) {
								parse_comments();
							} else {
								fout("\n+Field Debris Type:");
							}
							fout(" %d", Asteroid_field.field_debris_type[idx]);
						}
					} else {
						if (optional_string_fred("+Field Debris Type Name:")) {
							parse_comments();
						} else {
							fout("\n+Field Debris Type Name:");
						}
						fout(" %s", Asteroid_info[Asteroid_field.field_debris_type[idx]].name);
					}
				}
			}
		} else {
			for (size_t idx = 0; idx < Asteroid_field.field_asteroid_type.size(); idx++) {

				if (save_format == MissionFormat::RETAIL) {
					if (idx < MAX_RETAIL_DEBRIS_TYPES) { // Retail can only have 3!
						if (optional_string_fred("+Field Debris Type:")) {
							parse_comments();
						} else {
							fout("\n+Field Debris Type:");
						}
						fout(" %d", idx);
					}
				} else {
					if (optional_string_fred("+Field Debris Type Name:")) {
						parse_comments();
					} else {
						fout("\n+Field Debris Type Name:");
					}
					fout(" %s", Asteroid_field.field_asteroid_type[idx].c_str());
				}
			}
		}


		required_string_fred("$Average Speed:");
		parse_comments();
		fout(" %f", vm_vec_mag(&Asteroid_field.vel));

		required_string_fred("$Minimum:");
		parse_comments();
		save_vector(Asteroid_field.min_bound);

		required_string_fred("$Maximum:");
		parse_comments();
		save_vector(Asteroid_field.max_bound);

		if (Asteroid_field.has_inner_bound) {
			if (optional_string_fred("+Inner Bound:")) {
				parse_comments();
			} else {
				fout("\n+Inner Bound:");
			}

			required_string_fred("$Minimum:");
			parse_comments();
			save_vector(Asteroid_field.inner_min_bound);

			required_string_fred("$Maximum:");
			parse_comments();
			save_vector(Asteroid_field.inner_max_bound);
		}

		if (Asteroid_field.enhanced_visibility_checks) {
			if (save_format != MissionFormat::RETAIL) {
				if (optional_string_fred("+Use Enhanced Checks")) {
					parse_comments();
				} else {
					fout("\n+Use Enhanced Checks");
				}
			}
		}

		if (!Asteroid_field.target_names.empty()) {
			fso_comment_push(";;FSO 22.0.0;;");
			if (optional_string_fred("$Asteroid Targets:")) {
				parse_comments();
				fout(" (");
			} else {
				fout_version("\n$Asteroid Targets: (");
			}

			for (SCP_string& name : Asteroid_field.target_names) {
				fout(" \"%s\"", name.c_str());
			}

			fout(" )");
			fso_comment_pop();
		}

		fso_comment_pop();
	}

	fso_comment_pop(true);

	return err;
}

int CFred_mission_save::save_bitmaps()
{
	fred_parse_flag = 0;
	required_string_fred("#Background bitmaps");
	parse_comments(2);
	fout("\t\t;! %d total\n", stars_get_num_bitmaps());

	required_string_fred("$Num stars:");
	parse_comments();
	fout(" %d", Num_stars);

	required_string_fred("$Ambient light level:");
	parse_comments();
	fout(" %d", The_mission.ambient_light_level);

	// neb2 stuff
	if (The_mission.flags[Mission::Mission_Flags::Fullneb]) {
		fout("\n");
		required_string_fred("+Neb2:");
		parse_comments();
		fout(" %s", Neb2_texture_name);

		if (save_format != MissionFormat::RETAIL && The_mission.flags[Mission::Mission_Flags::Neb2_fog_color_override]) {
			if (optional_string_fred("+Neb2Color:")) {
				parse_comments();
			} else {
				fout("\n+Neb2Color:");
			}
			fout(" (");
			for (auto c : Neb2_fog_color) {
				fout(" %d", c);
			}
			fout(" )");
		}

		if (save_format == MissionFormat::RETAIL) {
			if (optional_string_fred("+Neb2Flags:")) {
				parse_comments();
			} else {
				fout("\n+Neb2Flags:");
			}
			int flags = bit_array_as_int(Neb2_poof_flags.get(), Poof_info.size());
			fout(" %d", flags);
		} else {
			if (optional_string_fred("+Neb2 Poofs List:")) {
				parse_comments();
			} else {
				fout("\n+Neb2 Poofs List:");
			}
			fout(" (");
			for (size_t i = 0; i < Poof_info.size(); ++i) {
				if (get_bit(Neb2_poof_flags.get(), i)) {
					fout(" \"%s\" ", Poof_info[i].name);
				}
			}
			fout(") ");
		}
	}
	// neb 1 stuff
	else {
		if (Nebula_index >= 0) {
			if (optional_string_fred("+Nebula:")) {
				parse_comments();
			} else {
				fout("\n+Nebula:");
			}
			fout(" %s", Nebula_filenames[Nebula_index]);

			required_string_fred("+Color:");
			parse_comments();
			fout(" %s", Nebula_colors[Mission_palette]);

			required_string_fred("+Pitch:");
			parse_comments();
			fout(" %d", Nebula_pitch);

			required_string_fred("+Bank:");
			parse_comments();
			fout(" %d", Nebula_bank);

			required_string_fred("+Heading:");
			parse_comments();
			fout(" %d", Nebula_heading);
		}
	}

	fso_comment_pop();

	// Goober5000 - save all but the lowest priority using the special comment tag
	for (size_t i = 0; i < Backgrounds.size(); i++) {
		bool tag = (i < Backgrounds.size() - 1);
		background_t* background = &Backgrounds[i];

		// each background should be preceded by this line so that the suns/bitmaps are partitioned correctly
		fso_comment_push(";;FSO 3.6.9;;");
		if (optional_string_fred("$Bitmap List:")) {
			parse_comments(2);
		} else {
			fout_version("\n\n$Bitmap List:");
		}

		if (!tag) {
			fso_comment_pop(true);
		}

		// save our flags
		if (save_format == MissionFormat::STANDARD && background->flags.any_set()) {
			if (optional_string_fred("+Flags:")) {
				parse_comments();
			} else {
				fout_version("\n+Flags:");
			}
			fout(" (");
			if (background->flags[Starfield::Background_Flags::Corrected_angles_in_mission_file]) {
				fout(" \"corrected angles\"");
			}
			fout(" )");
		}

		// save suns by filename
		for (size_t j = 0; j < background->suns.size(); j++) {
			starfield_list_entry* sle = &background->suns[j];

			// filename
			required_string_fred("$Sun:");
			parse_comments();
			fout(" %s", sle->filename);

			// angles
			required_string_fred("+Angles:");
			parse_comments();
			angles ang = sle->ang;
			if (save_format != MissionFormat::STANDARD || !background->flags[Starfield::Background_Flags::Corrected_angles_in_mission_file])
				stars_uncorrect_background_sun_angles(&ang);
			fout(" %f %f %f", ang.p, ang.b, ang.h);

			// scale
			required_string_fred("+Scale:");
			parse_comments();
			fout(" %f", sle->scale_x);
		}

		// save background bitmaps by filename
		for (size_t j = 0; j < background->bitmaps.size(); j++) {
			starfield_list_entry* sle = &background->bitmaps[j];

			// filename
			required_string_fred("$Starbitmap:");
			parse_comments();
			fout(" %s", sle->filename);

			// angles
			required_string_fred("+Angles:");
			parse_comments();
			angles ang = sle->ang;
			if (save_format != MissionFormat::STANDARD || !background->flags[Starfield::Background_Flags::Corrected_angles_in_mission_file])
				stars_uncorrect_background_bitmap_angles(&ang);
			fout(" %f %f %f", ang.p, ang.b, ang.h);

			// scale
			required_string_fred("+ScaleX:");
			parse_comments();
			fout(" %f", sle->scale_x);
			required_string_fred("+ScaleY:");
			parse_comments();
			fout(" %f", sle->scale_y);

			// div
			required_string_fred("+DivX:");
			parse_comments();
			fout(" %d", sle->div_x);
			required_string_fred("+DivY:");
			parse_comments();
			fout(" %d", sle->div_y);
		}

		fso_comment_pop();
	}

	// taylor's environment map thingy
	if (strlen(The_mission.envmap_name) > 0) { //-V805
		fso_comment_push(";;FSO 3.6.9;;");
		if (optional_string_fred("$Environment Map:")) {
			parse_comments(2);
			fout(" %s", The_mission.envmap_name);
		} else {
			fout_version("\n\n$Environment Map: %s", The_mission.envmap_name);
		}
		fso_comment_pop();
	} else {
		bypass_comment(";;FSO 3.6.9;; $Environment Map:");
	}

	fso_comment_pop(true);

	return err;
}

int CFred_mission_save::save_briefing()
{
	int i, j, k, nb;
	SCP_string sexp_out;
	brief_stage* bs;
	brief_icon* bi;

	for (nb = 0; nb < Num_teams; nb++) {

		required_string_fred("#Briefing");
		parse_comments(2);

		required_string_fred("$start_briefing");
		parse_comments();

		save_custom_bitmap("$briefing_background_640:",
						   "$briefing_background_1024:",
						   Briefings[nb].background[GR_640],
						   Briefings[nb].background[GR_1024]);
		save_custom_bitmap("$ship_select_background_640:",
						   "$ship_select_background_1024:",
						   Briefings[nb].ship_select_background[GR_640],
						   Briefings[nb].ship_select_background[GR_1024]);
		save_custom_bitmap("$weapon_select_background_640:",
						   "$weapon_select_background_1024:",
						   Briefings[nb].weapon_select_background[GR_640],
						   Briefings[nb].weapon_select_background[GR_1024]);

		Assert(Briefings[nb].num_stages <= MAX_BRIEF_STAGES);
		required_string_fred("$num_stages:");
		parse_comments();
		fout(" %d", Briefings[nb].num_stages);

		for (i = 0; i < Briefings[nb].num_stages; i++) {
			bs = &Briefings[nb].stages[i];

			required_string_fred("$start_stage");
			parse_comments();

			required_string_fred("$multi_text");
			parse_comments();

			// XSTR
			fout_ext("\n", "%s", bs->text.c_str());

			required_string_fred("$end_multi_text", "$start_stage");
			parse_comments();

			if (!drop_white_space(bs->voice)[0]) {
				strcpy_s(bs->voice, "None");
			}

			required_string_fred("$voice:");
			parse_comments();
			fout(" %s", bs->voice);

			required_string_fred("$camera_pos:");
			parse_comments();
			save_vector(bs->camera_pos);

			required_string_fred("$camera_orient:");
			parse_comments();
			save_matrix(bs->camera_orient);

			required_string_fred("$camera_time:");
			parse_comments();
			fout(" %d", bs->camera_time);

			if (!bs->draw_grid) {
				if (save_format != MissionFormat::RETAIL) {
					fout("\n$no_grid");
				}
			}

			if (!gr_compare_color_values(bs->grid_color, Color_briefing_grid)) {
				if (save_format != MissionFormat::RETAIL) {
					fout("\n$grid_color:");
					fout("(%d, %d, %d, %d)", bs->grid_color.red, bs->grid_color.green, bs->grid_color.blue, bs->grid_color.alpha);
				}
			}

			required_string_fred("$num_lines:");
			parse_comments();
			fout(" %d", bs->num_lines);

			for (k = 0; k < bs->num_lines; k++) {
				required_string_fred("$line_start:");
				parse_comments();
				fout(" %d", bs->lines[k].start_icon);

				required_string_fred("$line_end:");
				parse_comments();
				fout(" %d", bs->lines[k].end_icon);

				fso_comment_pop();
			}

			required_string_fred("$num_icons:");
			parse_comments();
			Assert(bs->num_icons <= MAX_STAGE_ICONS);
			fout(" %d", bs->num_icons);

			required_string_fred("$Flags:");
			parse_comments();
			fout(" %d", bs->flags);

			required_string_fred("$Formula:");
			parse_comments();
			convert_sexp_to_string(sexp_out, bs->formula, SEXP_SAVE_MODE);
			fout(" %s", sexp_out.c_str());

			for (j = 0; j < bs->num_icons; j++) {
				bi = &bs->icons[j];

				required_string_fred("$start_icon");
				parse_comments();

				required_string_fred("$type:");
				parse_comments();
				fout(" %d", bi->type);

				required_string_fred("$team:");
				parse_comments();
				fout(" %s", Iff_info[bi->team].iff_name);

				required_string_fred("$class:");
				parse_comments();
				if (bi->ship_class < 0) {
					bi->ship_class = 0;
				}

				fout(" %s", Ship_info[bi->ship_class].name);

				required_string_fred("$pos:");
				parse_comments();
				save_vector(bi->pos);

				if (drop_white_space(bi->label)[0]) {
					if (optional_string_fred("$label:")) {
						parse_comments();
					} else {
						fout("\n$label:");
					}

					fout_ext(" ", "%s", bi->label);
				}
				if (save_format != MissionFormat::RETAIL) {
					if (drop_white_space(bi->closeup_label)[0]) {
						if (optional_string_fred("$closeup label:")) {
							parse_comments();
						}
						else {
							fout("\n$closeup label:");
						}

						fout_ext(" ", "%s", bi->closeup_label);
					}
				}

				if (save_format != MissionFormat::RETAIL && bi->scale_factor != 1.0f) {
					if (optional_string_fred("$icon scale:"))
						parse_comments();
					else
						fout("\n$icon scale:");

					fout(" %d", static_cast<int>(bi->scale_factor * 100.0f));
				}

				if (optional_string_fred("+id:")) {
					parse_comments();
				} else {
					fout("\n+id:");
				}

				fout(" %d", bi->id);

				required_string_fred("$hlight:");
				parse_comments();
				fout(" %d", (bi->flags & BI_HIGHLIGHT) ? 1 : 0);

				if (save_format != MissionFormat::RETAIL) {
					if (optional_string_fred("$mirror:"))
						parse_comments();
					else
						fout("\n$mirror:");

					fout(" %d", (bi->flags & BI_MIRROR_ICON) ? 1 : 0);
				}

				if ((save_format != MissionFormat::RETAIL) && (bi->flags & BI_USE_WING_ICON)) {
					if (optional_string_fred("$use wing icon:"))
						parse_comments();
					else
						fout("\n$use wing icon:");

					fout(" %d", (bi->flags & BI_USE_WING_ICON) ? 1 : 0);
				}

				if ((save_format != MissionFormat::RETAIL) && (bi->flags & BI_USE_CARGO_ICON)) {
					if (optional_string_fred("$use cargo icon:"))
						parse_comments();
					else
						fout("\n$use cargo icon:");

					fout(" %d", (bi->flags & BI_USE_CARGO_ICON) ? 1 : 0);
				}

				required_string_fred("$multi_text");
				parse_comments();

				//				sprintf(out,"\n%s", bi->text);
				//				fout(out);

				required_string_fred("$end_multi_text");
				parse_comments();

				required_string_fred("$end_icon");
				parse_comments();

				fso_comment_pop();
			}

			required_string_fred("$end_stage");
			parse_comments();

			fso_comment_pop();
		}
		required_string_fred("$end_briefing");
		parse_comments();

		fso_comment_pop();
	}

	fso_comment_pop(true);

	return err;
}

int CFred_mission_save::save_cmd_brief()
{
	int stage;

	stage = 0;
	required_string_fred("#Command Briefing");
	parse_comments(2);

	save_custom_bitmap("$Background 640:",
					   "$Background 1024:",
					   Cur_cmd_brief->background[GR_640],
					   Cur_cmd_brief->background[GR_1024],
					   1);

	for (stage = 0; stage < Cur_cmd_brief->num_stages; stage++) {
		required_string_fred("$Stage Text:");
		parse_comments(2);

		// XSTR
		fout_ext("\n", "%s", Cur_cmd_brief->stage[stage].text.c_str());

		required_string_fred("$end_multi_text", "$Stage Text:");
		parse_comments();

		if (!drop_white_space(Cur_cmd_brief->stage[stage].ani_filename)[0]) {
			strcpy_s(Cur_cmd_brief->stage[stage].ani_filename, "<default>");
		}

		required_string_fred("$Ani Filename:");
		parse_comments();
		fout(" %s", Cur_cmd_brief->stage[stage].ani_filename);

		if (!drop_white_space(Cur_cmd_brief->stage[stage].wave_filename)[0]) {
			strcpy_s(Cur_cmd_brief->stage[stage].wave_filename, "None");
		}

		required_string_fred("+Wave Filename:", "$Ani Filename:");
		parse_comments();
		fout(" %s", Cur_cmd_brief->stage[stage].wave_filename);

		fso_comment_pop();
	}

	fso_comment_pop(true);

	return err;
}

int CFred_mission_save::save_cmd_briefs()
{
	int i;

	for (i = 0; i < Num_teams; i++) {
		Cur_cmd_brief = &Cmd_briefs[i];
		save_cmd_brief();
	}

	return err;
}

void CFred_mission_save::fso_comment_push(const char* ver)
{
	if (fso_ver_comment.empty()) {
		fso_ver_comment.push_back(SCP_string(ver));
		return;
	}

	SCP_string before = fso_ver_comment.back();

	int major, minor, build, revis;
	int in_major, in_minor, in_build, in_revis;
	int elem1, elem2;

	elem1 = scan_fso_version_string(fso_ver_comment.back().c_str(), &major, &minor, &build, &revis);
	elem2 = scan_fso_version_string(ver, &in_major, &in_minor, &in_build, &in_revis);

	// check consistency
	if ((elem1 == 3 && elem2 == 4) || (elem1 == 4 && elem2 == 3)) {
		elem1 = elem2 = 3;
	} else if ((elem1 >= 3 && elem2 >= 3) && (revis < 1000 || in_revis < 1000)) {
		elem1 = elem2 = 3;
	}

	if ((elem1 == 3) && ((major > in_major)
		|| ((major == in_major) && ((minor > in_minor) || ((minor == in_minor) && (build > in_build)))))) {
		// the push'd version is older than our current version, so just push a copy of the previous version
		fso_ver_comment.push_back(before);
	} else if ((elem1 == 4) && ((major > in_major) || ((major == in_major) && ((minor > in_minor)
		|| ((minor == in_minor) && ((build > in_build) || ((build == in_build) || (revis > in_revis)))))))) {
		// the push'd version is older than our current version, so just push a copy of the previous version
		fso_ver_comment.push_back(before);
	} else {
		fso_ver_comment.push_back(SCP_string(ver));
	}
}

void CFred_mission_save::fso_comment_pop(bool pop_all)
{
	if (fso_ver_comment.empty()) {
		return;
	}

	if (pop_all) {
		fso_ver_comment.clear();
		return;
	}

	fso_ver_comment.pop_back();
}

int CFred_mission_save::save_common_object_data(object* objp, ship* shipp)
{
	int j, z;
	ship_subsys* ptr = NULL;
	ship_info* sip = NULL;
	ship_weapon* wp = NULL;

	sip = &Ship_info[shipp->ship_info_index];

	if ((int) objp->phys_info.speed) {
		if (optional_string_fred("+Initial Velocity:", "$Name:", "+Subsystem:")) {
			parse_comments();
		} else {
			fout("\n+Initial Velocity:");
		}

		fout(" %d", (int) objp->phys_info.speed);
	}

	if (fl2i(objp->hull_strength) != 100) {
		if (optional_string_fred("+Initial Hull:", "$Name:", "+Subsystem:")) {
			parse_comments();
		} else {
			fout("\n+Initial Hull:");
		}

		fout(" %d", fl2i(objp->hull_strength));
	}

	if (fl2i(objp->shield_quadrant[0]) != 100) {
		if (optional_string_fred("+Initial Shields:", "$Name:", "+Subsystem:")) {
			parse_comments();
		} else {
			fout("\n+Initial Shields:");
		}

		fout(" %d", fl2i(objp->shield_quadrant[0]));
	}

	// save normal ship weapons info
	required_string_fred("+Subsystem:", "$Name:");
	parse_comments();
	fout(" Pilot");

	wp = &shipp->weapons;
	z = 0;
	j = wp->num_primary_banks;
	while (j-- && (j >= 0)) {
		if (wp->primary_bank_weapons[j] != sip->primary_bank_weapons[j]) {
			z = 1;
		}
	}

	if (z) {
		if (optional_string_fred("+Primary Banks:", "$Name:", "+Subsystem:")) {
			parse_comments();
		} else {
			fout("\n+Primary Banks:");
		}

		fout(" ( ");
		for (j = 0; j < wp->num_primary_banks; j++) {
			if (wp->primary_bank_weapons[j] != -1) { // Just in case someone has set a weapon bank to empty
				fout("\"%s\" ", Weapon_info[wp->primary_bank_weapons[j]].name);
			} else {
				fout("\"\" ");
			}
		}

		fout(")");
	}

	z = 0;
	j = wp->num_secondary_banks;
	while (j-- && (j >= 0)) {
		if (wp->secondary_bank_weapons[j] != sip->secondary_bank_weapons[j]) {
			z = 1;
		}
	}

	if (z) {
		if (optional_string_fred("+Secondary Banks:", "$Name:", "+Subsystem:")) {
			parse_comments();
		} else {
			fout("\n+Secondary Banks:");
		}

		fout(" ( ");
		for (j = 0; j < wp->num_secondary_banks; j++) {
			if (wp->secondary_bank_weapons[j] != -1) {
				fout("\"%s\" ", Weapon_info[wp->secondary_bank_weapons[j]].name);
			} else {
				fout("\"\" ");
			}
		}

		fout(")");
	}

	z = 0;
	j = wp->num_secondary_banks;
	while (j-- && (j >= 0)) {
		if (wp->secondary_bank_ammo[j] != 100) {
			z = 1;
		}
	}

	if (z) {
		if (optional_string_fred("+Sbank Ammo:", "$Name:", "+Subsystem:")) {
			parse_comments();
		} else {
			fout("\n+Sbank Ammo:");
		}

		fout(" ( ");
		for (j = 0; j < wp->num_secondary_banks; j++) {
			fout("%d ", wp->secondary_bank_ammo[j]);
		}

		fout(")");
	}

	ptr = GET_FIRST(&shipp->subsys_list);
	Assert(ptr);

	while (ptr != END_OF_LIST(&shipp->subsys_list) && ptr) {
		// Crashing here!
		if ((ptr->current_hits) || (ptr->system_info && ptr->system_info->type == SUBSYSTEM_TURRET)
			|| (ptr->subsys_cargo_name > 0)) {
			if (optional_string_fred("+Subsystem:", "$Name:")) {
				parse_comments();
			} else {
				fout("\n+Subsystem:");
			}

			fout(" %s", ptr->system_info->subobj_name);
		}

		if (ptr->current_hits) {
			if (optional_string_fred("$Damage:", "$Name:", "+Subsystem:")) {
				parse_comments();
			} else {
				fout("\n$Damage:");
			}

			fout(" %d", (int) ptr->current_hits);
		}

		if (ptr->subsys_cargo_name > 0) {
			if (optional_string_fred("+Cargo Name:", "$Name:", "+Subsystem:")) {
				parse_comments();
			} else {
				fout("\n+Cargo Name:");
			}

			fout_ext(NULL, "%s", Cargo_names[ptr->subsys_cargo_name]);
		}

		if (save_format != MissionFormat::RETAIL) {
			if (ptr->subsys_cargo_title[0] != '\0') {
				if (optional_string_fred("+Cargo Title:", "$Name:", "+Subsystem:")) {
					parse_comments();
				} else {
					fout("\n+Cargo Title:");
				}
				fout_ext(nullptr, "%s", ptr->subsys_cargo_title);
			}
		}

		if (ptr->system_info->type == SUBSYSTEM_TURRET) {
			save_turret_info(ptr, SHIP_INDEX(shipp));
		}

		ptr = GET_NEXT(ptr);

		fso_comment_pop();
	}

	fso_comment_pop(true);

	return err;
}

void CFred_mission_save::save_custom_bitmap(const char* expected_string_640,
											const char* expected_string_1024,
											const char* string_field_640,
											const char* string_field_1024,
											int blank_lines)
{
	if (save_format != MissionFormat::RETAIL) {
		if ((*string_field_640 != '\0') || (*string_field_1024 != '\0')) {
			while (blank_lines-- > 0) {
				fout("\n");
			}
		}

		if (*string_field_640 != '\0') {
			fout("\n%s %s", expected_string_640, string_field_640);
		}

		if (*string_field_1024 != '\0') {
			fout("\n%s %s", expected_string_1024, string_field_1024);
		}
	}
}

int CFred_mission_save::save_cutscenes()
{
	char type[NAME_LENGTH];
	SCP_string sexp_out;

	// Let's just assume it has them for now - 
	if (!(The_mission.cutscenes.empty())) {
		if (save_format != MissionFormat::RETAIL) {
			if (optional_string_fred("#Cutscenes")) {
				parse_comments(2);
			} else {
				fout_version("\n\n#Cutscenes");
			}
			fout("\n");

			for (uint i = 0; i < The_mission.cutscenes.size(); i++) {
				if (strlen(The_mission.cutscenes[i].filename)) {
					// determine the name of this cutscene type
					switch (The_mission.cutscenes[i].type) {
					case MOVIE_PRE_FICTION:
						strcpy_s(type, "$Fiction Viewer Cutscene:");
						break;
					case MOVIE_PRE_CMD_BRIEF:
						strcpy_s(type, "$Command Brief Cutscene:");
						break;
					case MOVIE_PRE_BRIEF:
						strcpy_s(type, "$Briefing Cutscene:");
						break;
					case MOVIE_PRE_GAME:
						strcpy_s(type, "$Pre-game Cutscene:");
						break;
					case MOVIE_PRE_DEBRIEF:
						strcpy_s(type, "$Debriefing Cutscene:");
						break;
					case MOVIE_POST_DEBRIEF:
						strcpy_s(type, "$Post-debriefing Cutscene:");
						break;
					case MOVIE_END_CAMPAIGN:
						strcpy_s(type, "$Campaign End Cutscene:");
						break;
					default:
						Int3();
						continue;
					}

					if (optional_string_fred(type)) {
						parse_comments();
						fout(" %s", The_mission.cutscenes[i].filename);
					} else {
						fout_version("\n%s %s", type, The_mission.cutscenes[i].filename);
					}

					required_string_fred("+formula:");
					parse_comments();
					convert_sexp_to_string(sexp_out, The_mission.cutscenes[i].formula, SEXP_SAVE_MODE);
					fout(" %s\n", sexp_out.c_str());
				}
			}
			required_string_fred("#end");
			parse_comments();
		} else {
			_viewport->dialogProvider->showButtonDialog(DialogType::Warning,
														"Incompatibility with retail mission format",
														"Warning: This mission contains cutscene data, but you are saving in the retail mission format. This information will be lost.",
														{ DialogButton::Ok });
		}
	}

	fso_comment_pop(true);
	return err;
}

int CFred_mission_save::save_debriefing()
{
	int j, i;
	SCP_string sexp_out;

	for (j = 0; j < Num_teams; j++) {

		Debriefing = &Debriefings[j];

		required_string_fred("#Debriefing_info");
		parse_comments(2);

		save_custom_bitmap("$Background 640:",
						   "$Background 1024:",
						   Debriefing->background[GR_640],
						   Debriefing->background[GR_1024],
						   1);

		required_string_fred("$Num stages:");
		parse_comments(2);
		fout(" %d", Debriefing->num_stages);

		for (i = 0; i < Debriefing->num_stages; i++) {
			required_string_fred("$Formula:");
			parse_comments(2);
			convert_sexp_to_string(sexp_out, Debriefing->stages[i].formula, SEXP_SAVE_MODE);
			fout(" %s", sexp_out.c_str());

			// XSTR
			required_string_fred("$Multi text");
			parse_comments();
			fout_ext("\n   ", "%s", Debriefing->stages[i].text.c_str());

			required_string_fred("$end_multi_text");
			parse_comments();

			if (!drop_white_space(Debriefing->stages[i].voice)[0]) {
				strcpy_s(Debriefing->stages[i].voice, "None");
			}

			required_string_fred("$Voice:");
			parse_comments();
			fout(" %s", Debriefing->stages[i].voice);

			// XSTR
			required_string_fred("$Recommendation text:");
			parse_comments();
			fout_ext("\n   ", "%s", Debriefing->stages[i].recommendation_text.c_str());

			required_string_fred("$end_multi_text");
			parse_comments();

			fso_comment_pop();
		}
	}

	fso_comment_pop(true);

	return err;
}

int CFred_mission_save::save_events()
{
	SCP_string sexp_out;
	int i, j, add_flag;

	fred_parse_flag = 0;
	required_string_fred("#Events");
	parse_comments(2);
	fout("\t\t;! " SIZE_T_ARG " total\n", Mission_events.size());

	for (i = 0; i < (int)Mission_events.size(); i++) {
		required_string_either_fred("$Formula:", "#Goals");
		required_string_fred("$Formula:");
		parse_comments(i ? 2 : 1);
		convert_sexp_to_string(sexp_out, Mission_events[i].formula, SEXP_SAVE_MODE);
		fout(" %s", sexp_out.c_str());

		if (!Mission_events[i].name.empty()) {
			if (optional_string_fred("+Name:", "$Formula:")) {
				parse_comments();
			} else {
				fout("\n+Name:");
			}

			fout(" %s", Mission_events[i].name.c_str());
		}

		if (optional_string_fred("+Repeat Count:", "$Formula:")) {
			parse_comments();
		} else {
			fout("\n+Repeat Count:");
		}

		// if we have a trigger count but no repeat count, we want the event to loop until it has triggered enough times
		if (Mission_events[i].repeat_count == 1 && Mission_events[i].trigger_count != 1) {
			fout(" -1");
		} else {
			fout(" %d", Mission_events[i].repeat_count);
		}

		if (save_format != MissionFormat::RETAIL && Mission_events[i].trigger_count != 1) {
			fso_comment_push(";;FSO 3.6.11;;");
			if (optional_string_fred("+Trigger Count:", "$Formula:")) {
				parse_comments();
			} else {
				fout_version("\n+Trigger Count:");
			}
			fso_comment_pop();

			fout(" %d", Mission_events[i].trigger_count);
		}

		if (optional_string_fred("+Interval:", "$Formula:")) {
			parse_comments();
		} else {
			fout("\n+Interval:");
		}

		fout(" %d", Mission_events[i].interval);

		if (Mission_events[i].score != 0) {
			if (optional_string_fred("+Score:", "$Formula:")) {
				parse_comments();
			} else {
				fout("\n+Score:");
			}
			fout(" %d", Mission_events[i].score);
		}

		if (Mission_events[i].chain_delay >= 0) {
			if (optional_string_fred("+Chained:", "$Formula:")) {
				parse_comments();
			} else {
				fout("\n+Chained:");
			}

			fout(" %d", Mission_events[i].chain_delay);
		}

		//XSTR
		if (!Mission_events[i].objective_text.empty()) {
			if (optional_string_fred("+Objective:", "$Formula:")) {
				parse_comments();
			} else {
				fout("\n+Objective:");
			}

			fout_ext(" ", "%s", Mission_events[i].objective_text.c_str());
		}

		//XSTR
		if (!Mission_events[i].objective_key_text.empty()) {
			if (optional_string_fred("+Objective key:", "$Formula:")) {
				parse_comments();
			} else {
				fout("\n+Objective key:");
			}

			fout_ext(" ", "%s", Mission_events[i].objective_key_text.c_str());
		}

		// save team
		if (Mission_events[i].team >= 0) {
			if (optional_string_fred("+Team:")) {
				parse_comments();
			} else {
				fout("\n+Team:");
			}
			fout(" %d", Mission_events[i].team);
		}

		// save flags, if any
		if (save_format != MissionFormat::RETAIL) {
			// we need to lazily-write the tag because we should only write it if there are also flags to write
			// (some of the flags are transient, internal flags)
			bool wrote_tag = false;

			for (j = 0; j < Num_mission_event_flags; ++j) {
				if (Mission_events[i].flags & Mission_event_flags[j].def) {
					if (!wrote_tag) {
						wrote_tag = true;

						fso_comment_push(";;FSO 20.0.0;;");
						if (optional_string_fred("+Event Flags: (", "$Formula:")) {
							parse_comments();
						}
						else {
							fout_version("\n+Event Flags: (");
						}
						fso_comment_pop();
					}

					fout(" \"%s\"", Mission_event_flags[j].name);
				}
			}

			if (wrote_tag)
				fout(" )");
		}

		if (save_format != MissionFormat::RETAIL && Mission_events[i].mission_log_flags != 0) {
			fso_comment_push(";;FSO 3.6.11;;");
			if (optional_string_fred("+Event Log Flags: (", "$Formula:")) {
				parse_comments();
			} else {
				fout_version("\n+Event Log Flags: (");
			}
			fso_comment_pop();

			for (j = 0; j < MAX_MISSION_EVENT_LOG_FLAGS; j++) {
				add_flag = 1 << j;
				if (Mission_events[i].mission_log_flags & add_flag) {
					fout(" \"%s\"", Mission_event_log_flags[j]);
				}
			}
			fout(" )");
		}

		// save event annotations
		if (save_format != MissionFormat::RETAIL && !Event_annotations.empty())
		{
			bool at_least_one = false;
			fso_comment_push(";;FSO 21.0.0;;");
			event_annotation default_ea;

			// see if there is an annotation for this event
			for (const auto &ea : Event_annotations)
			{
				if (ea.path.empty() || ea.path.front() != i)
					continue;

				if (!at_least_one)
				{
					if (optional_string_fred("$Annotations Start", "$Formula:"))
						parse_comments();
					else
						fout_version("\n$Annotations Start");
					at_least_one = true;
				}

				if (ea.comment != default_ea.comment)
				{
					if (optional_string_fred("+Comment:", "$Formula:"))
						parse_comments();
					else
						fout_version("\n+Comment:");

					auto copy = ea.comment;
					lcl_fred_replace_stuff(copy);
					fout(" %s", copy.c_str());

					if (optional_string_fred("$end_multi_text", "$Formula:"))
						parse_comments();
					else
						fout_version("\n$end_multi_text");
				}

				if (ea.r != default_ea.r || ea.g != default_ea.g || ea.b != default_ea.b)
				{
					if (optional_string_fred("+Background Color:", "$Formula:"))
						parse_comments();
					else
						fout_version("\n+Background Color:");

					fout(" %d, %d, %d", ea.r, ea.g, ea.b);
				}

				if (ea.path.size() > 1)
				{
					if (optional_string_fred("+Path:", "$Formula:"))
						parse_comments();
					else
						fout_version("\n+Path:");

					bool comma = false;
					auto it = ea.path.begin();
					for (++it; it != ea.path.end(); ++it)
					{
						if (comma)
							fout(",");
						comma = true;
						fout(" %d", *it);
					}
				}
			}

			if (at_least_one)
			{
				if (optional_string_fred("$Annotations End", "$Formula:"))
					parse_comments();
				else
					fout_version("\n$Annotations End");
			}

			fso_comment_pop();
		}
	}

	fso_comment_pop(true);

	return err;
}

int CFred_mission_save::save_fiction()
{
	if (mission_has_fiction()) {
		if (save_format != MissionFormat::RETAIL) {
			if (optional_string_fred("#Fiction Viewer")) {
				parse_comments(2);
			} else {
				fout("\n\n#Fiction Viewer");
			}

			// we have multiple stages now, so save them all
			for (SCP_vector<fiction_viewer_stage>::iterator stage = Fiction_viewer_stages.begin();
				 stage != Fiction_viewer_stages.end(); ++stage) {
				fout("\n");

				// save file
				required_string_fred("$File:");
				parse_comments();
				fout(" %s", stage->story_filename);

				// save font
				if (strlen(stage->font_filename) > 0) //-V805
				{
					if (optional_string_fred("$Font:")) {
						parse_comments();
					} else {
						fout("\n$Font:");
					}
					fout(" %s", stage->font_filename);
				} else {
					optional_string_fred("$Font:");
				}

				// save voice
				if (strlen(stage->voice_filename) > 0) //-V805
				{
					if (optional_string_fred("$Voice:")) {
						parse_comments();
					} else {
						fout("\n$Voice:");
					}
					fout(" %s", stage->voice_filename);
				} else {
					optional_string_fred("$Voice:");
				}

				// save UI
				if (strlen(stage->ui_name) > 0) {
					if (optional_string_fred("$UI:")) {
						parse_comments();
					} else {
						fout("\n$UI:");
					}
					fout(" %s", stage->ui_name);
				} else {
					optional_string_fred("$UI:");
				}

				// save background
				save_custom_bitmap("$Background 640:",
								   "$Background 1024:",
								   stage->background[GR_640],
								   stage->background[GR_1024]);

				// save sexp formula if we have one
				if (stage->formula >= 0 && stage->formula != Locked_sexp_true) {
					SCP_string sexp_out;
					convert_sexp_to_string(sexp_out, stage->formula, SEXP_SAVE_MODE);

					if (optional_string_fred("$Formula:")) {
						parse_comments();
					} else {
						fout("\n$Formula:");
					}
					fout(" %s", sexp_out.c_str());
				} else {
					optional_string_fred("$Formula:");
				}
			}
		} else {
			_viewport->dialogProvider->showButtonDialog(DialogType::Warning,
														"Incompatibility with retail mission format",
														"Warning: This mission contains fiction viewer data, but you are saving in the retail mission format.",
														{ DialogButton::Ok });
		}
	}

	fso_comment_pop(true);

	return err;
}

int CFred_mission_save::save_goals()
{
	SCP_string sexp_out;
	int i;

	fred_parse_flag = 0;
	required_string_fred("#Goals");
	parse_comments(2);
	fout("\t\t;! " SIZE_T_ARG " total\n", Mission_goals.size());

	for (i = 0; i < (int)Mission_goals.size(); i++) {
		int type;

		required_string_either_fred("$Type:", "#Waypoints");
		required_string_fred("$Type:");
		parse_comments(i ? 2 : 1);

		type = Mission_goals[i].type & GOAL_TYPE_MASK;
		fout(" %s", Goal_type_names[type]);

		if (!Mission_goals[i].name.empty()) {
			if (optional_string_fred("+Name:", "$Type:")) {
				parse_comments();
			} else {
				fout("\n+Name:");
			}

			fout(" %s", Mission_goals[i].name.c_str());
		}

		// XSTR
		required_string_fred("$MessageNew:");
		parse_comments();
		fout_ext(" ", "%s", Mission_goals[i].message.c_str());
		fout("\n");
		required_string_fred("$end_multi_text");
		parse_comments(0);

		required_string_fred("$Formula:");
		parse_comments();
		convert_sexp_to_string(sexp_out, Mission_goals[i].formula, SEXP_SAVE_MODE);
		fout(" %s", sexp_out.c_str());

		if (Mission_goals[i].type & INVALID_GOAL) {
			if (optional_string_fred("+Invalid", "$Type:")) {
				parse_comments();
			} else {
				fout("\n+Invalid");
			}
		}

		if (Mission_goals[i].flags & MGF_NO_MUSIC) {
			if (optional_string_fred("+No music", "$Type:")) {
				parse_comments();
			} else {
				fout("\n+No music");
			}
		}

		if (Mission_goals[i].score != 0) {
			if (optional_string_fred("+Score:", "$Type:")) {
				parse_comments();
			} else {
				fout("\n+Score:");
			}
			fout(" %d", Mission_goals[i].score);
		}

		if (The_mission.game_type & MISSION_TYPE_MULTI_TEAMS) {
			if (optional_string_fred("+Team:", "$Type:")) {
				parse_comments();
			} else {
				fout("\n+Team:");
			}
			fout(" %d", Mission_goals[i].team);
		}

		fso_comment_pop();
	}

	fso_comment_pop(true);

	return err;
}

int CFred_mission_save::save_matrix(matrix& m)
{
	fout("\n\t%f, %f, %f,\n", m.vec.rvec.xyz.x, m.vec.rvec.xyz.y, m.vec.rvec.xyz.z);
	fout("\t%f, %f, %f,\n", m.vec.uvec.xyz.x, m.vec.uvec.xyz.y, m.vec.uvec.xyz.z);
	fout("\t%f, %f, %f", m.vec.fvec.xyz.x, m.vec.fvec.xyz.y, m.vec.fvec.xyz.z);
	return 0;
}

int CFred_mission_save::save_messages()
{
	int i;

	fred_parse_flag = 0;
	required_string_fred("#Messages");
	parse_comments(2);
	fout("\t\t;! %d total\n", Num_messages - Num_builtin_messages);

	// Goober5000 - special Command info
	if (save_format != MissionFormat::RETAIL) {
		if (stricmp(The_mission.command_sender, DEFAULT_COMMAND) != 0) {
			fout("\n$Command Sender: %s", The_mission.command_sender);
		}

		if (The_mission.command_persona != Default_command_persona) {
			fout("\n$Command Persona: %s", Personas[The_mission.command_persona].name);
		}
	}

	for (i = Num_builtin_messages; i < Num_messages; i++) {
		required_string_either_fred("$Name:", "#Reinforcements");
		required_string_fred("$Name:");
		parse_comments(2);
		fout(" %s", Messages[i].name);

		// team
		required_string_fred("$Team:");
		parse_comments(1);
		if ((Messages[i].multi_team < 0) || (Messages[i].multi_team >= 2)) {
			fout(" %d", -1);
		} else {
			fout(" %d", Messages[i].multi_team);
		}

		// XSTR
		required_string_fred("$MessageNew:");
		parse_comments();
		fout_ext(" ", "%s", Messages[i].message);
		fout("\n");
		required_string_fred("$end_multi_text");
		parse_comments(0);

		if (Messages[i].persona_index != -1) {
			if (optional_string_fred("+Persona:", "$Name:")) {
				parse_comments();
			} else {
				fout("\n+Persona:");
			}

			fout(" %s", Personas[Messages[i].persona_index].name);
		}

		if (Messages[i].avi_info.name) {
			if (optional_string_fred("+AVI Name:", "$Name:")) {
				parse_comments();
			} else {
				fout("\n+AVI Name:");
			}

			fout(" %s", Messages[i].avi_info.name);
		}

		if (Messages[i].wave_info.name) {
			if (optional_string_fred("+Wave Name:", "$Name:")) {
				parse_comments();
			} else {
				fout("\n+Wave Name:");
			}

			fout(" %s", Messages[i].wave_info.name);
		}

		if (Messages[i].note != "") {
			if (optional_string_fred("+Note:", "$Name:"))
				parse_comments();
			else
				fout("\n+Note:");

			auto copy = Messages[i].note;
			lcl_fred_replace_stuff(copy);
			fout(" %s", copy.c_str());

			if (optional_string_fred("$end_multi_text", "$Name:"))
				parse_comments();
			else
				fout_version("\n$end_multi_text");
		}

		fso_comment_pop();
	}

	fso_comment_pop(true);

	return err;
}

int CFred_mission_save::save_campaign_file(const char *pathname)
{
	reset_parse();
	raw_ptr = Parse_text_raw;
	fred_parse_flag = 0;

	pathname = cf_add_ext(pathname, FS_CAMPAIGN_FILE_EXT);
	fp = cfopen(pathname, "wt", CF_TYPE_MISSIONS);
	if (!fp) {
		nprintf(("Error", "Can't open campaign file to save.\n"));
		return -1;
	}

	required_string_fred("$Name:");
	parse_comments(0);
	fout(" %s", Campaign.name);

	Assert((Campaign.type >= 0) && (Campaign.type < MAX_CAMPAIGN_TYPES));
	required_string_fred("$Type:");
	parse_comments();
	fout(" %s", campaign_types[Campaign.type]);

	// XSTR
	if (Campaign.desc) {
		required_string_fred("+Description:");
		parse_comments();
		fout_ext("\n", "%s", Campaign.desc);
		fout("\n$end_multi_text");
	}

	if (Campaign.type != CAMPAIGN_TYPE_SINGLE) {
		required_string_fred("+Num Players:");
		parse_comments();
		fout(" %d", Campaign.num_players);
	}

	// campaign flags - Goober5000
	if (save_format != MissionFormat::RETAIL) {
		optional_string_fred("$Flags:");
		parse_comments();
		fout(" %d\n", Campaign.flags);
	}

	// write out the ships and weapons which the player can start the campaign with
	optional_string_fred("+Starting Ships: (");
	parse_comments(2);
	for (int i = 0; i < ship_info_size(); i++) {
		if (Campaign.ships_allowed[i]) {
			fout(" \"%s\" ", Ship_info[i].name);
		}
	}
	fout(")\n");

	optional_string_fred("+Starting Weapons: (");
	parse_comments();
	for (int i = 0; i < weapon_info_size(); i++) {
		if (Campaign.weapons_allowed[i]) {
			fout(" \"%s\" ", Weapon_info[i].name);
		}
	}
	fout(")\n");

	fred_parse_flag = 0;
	for (int i = 0; i < Campaign.num_missions; i++) {
		// Expect to get Campaign.missions ordered from FRED
		cmission &cm = Campaign.missions[i];
		required_string_either_fred("$Mission:", "#End");
		required_string_fred("$Mission:");
		parse_comments(2);
		fout(" %s", cm.name);

		if (strlen(cm.briefing_cutscene)) {
			if (optional_string_fred("+Briefing Cutscene:", "$Mission")) {
				parse_comments();
			} else {
				fout("\n+Briefing Cutscene:");
			}

			fout(" %s", cm.briefing_cutscene);
		}

		required_string_fred("+Flags:", "$Mission:");
		parse_comments();

		// don't save any internal flags
		auto flags_to_save = cm.flags & CMISSION_EXTERNAL_FLAG_MASK;

		// Goober5000
		if (save_format != MissionFormat::RETAIL) {
			// don't save Bastion flag
			fout(" %d", flags_to_save & ~CMISSION_FLAG_BASTION);

			// new main hall stuff
			if (optional_string_fred("+Main Hall:", "$Mission:")) {
				parse_comments();
			} else {
				fout("\n+Main Hall:");
			}

			fout(" %s", cm.main_hall.c_str());
		} else {
			// save Bastion flag properly
			fout(" %d", flags_to_save | ((! cm.main_hall.empty()) ? CMISSION_FLAG_BASTION : 0));
		}

		if (!cm.substitute_main_hall.empty()) {
			fso_comment_push(";;FSO 3.7.2;;");
			if (optional_string_fred("+Substitute Main Hall:")) {
				parse_comments(1);
				fout(" %s", cm.substitute_main_hall.c_str());
			} else {
				fout_version("\n+Substitute Main Hall: %s", cm.substitute_main_hall.c_str());
			}
			fso_comment_pop();
		} else {
			bypass_comment(";;FSO 3.7.2;; +Substitute Main Hall:");
		}

		if (cm.debrief_persona_index > 0) {
			fso_comment_push(";;FSO 3.6.8;;");
			if (optional_string_fred("+Debriefing Persona Index:")) {
				parse_comments(1);
				fout(" %d", cm.debrief_persona_index);
			} else {
				fout_version("\n+Debriefing Persona Index: %d", cm.debrief_persona_index);
			}
			fso_comment_pop();
		} else {
			bypass_comment(";;FSO 3.6.8;; +Debriefing Persona Index:");
		}

		//new save cmission sexps
		if (optional_string_fred("+Formula:", "$Mission:")) {
			parse_comments();
		} else {
			fout("\n+Formula:");
		}

		{
			SCP_string sexp_out{};
			convert_sexp_to_string(sexp_out, cm.formula, SEXP_SAVE_MODE);
			fout(" %s", sexp_out.c_str());
		}

		bool mission_loop = cm.flags & CMISSION_FLAG_HAS_LOOP;

		Assertion(cm.flags ^ CMISSION_FLAG_HAS_FORK, "scpFork campaigns cannot be saved, use axemFork.\n Should be detected on load.");

		if (mission_loop) {
			required_string_fred("\n+Mission Loop:");
			parse_comments();

			if (cm.mission_branch_desc) {
				required_string_fred("+Mission Loop Text:");
				parse_comments();
				fout_ext("\n", "%s", cm.mission_branch_desc);
				fout("\n$end_multi_text");
			}

			if (cm.mission_branch_brief_anim) {
				required_string_fred("+Mission Loop Brief Anim:");
				parse_comments();
				fout_ext("\n", "%s", cm.mission_branch_brief_anim);
				fout("\n$end_multi_text");
			}

			if (cm.mission_branch_brief_sound) {
				required_string_fred("+Mission Loop Brief Sound:");
				parse_comments();
				fout_ext("\n", "%s", cm.mission_branch_brief_sound);
				fout("\n$end_multi_text");
			}

			// write out mission loop formula
			fout("\n+Formula:");
			{
				SCP_string sexp_out{};
				convert_sexp_to_string(sexp_out, cm.mission_loop_formula, SEXP_SAVE_MODE);
				fout(" %s", sexp_out.c_str());
			}
		}

		if (optional_string_fred("+Level:", "$Mission:")) {
			parse_comments();
		} else {
			fout("\n\n+Level:");
		}

		fout(" %d", cm.level);

		if (optional_string_fred("+Position:", "$Mission:")) {
			parse_comments();
		} else {
			fout("\n+Position:");
		}

		fout(" %d", cm.pos);

		fso_comment_pop();
	}

	required_string_fred("#End");
	parse_comments(2);
	token_found = nullptr;
	parse_comments();
	fout("\n");

	cfclose(fp);

	fso_comment_pop(true);

	Assertion(! err, "Nothing in here should have a side effect to raise the mission error saving flag.");
	return 0;
}

int CFred_mission_save::save_mission_file(const char* pathname_in)
{
	char backup_name[256], savepath[MAX_PATH_LEN], pathname[MAX_PATH_LEN];

	strcpy_s(pathname, pathname_in);

	strcpy_s(savepath, "");
	auto p = strrchr(pathname, DIR_SEPARATOR_CHAR);
	if (p) {
		*p = '\0';
		strcpy_s(savepath, pathname);
		*p = DIR_SEPARATOR_CHAR;
		strcat_s(savepath, DIR_SEPARATOR_STR);
	}
	strcat_s(savepath, "saving.xxx");

	// only display this warning once, and only when the user explicitly saves, as opposed to autosave
	static bool Displayed_retail_background_warning = false;
	if (save_format != MissionFormat::STANDARD && !Displayed_retail_background_warning) {
		for (const auto &bg : Backgrounds) {
			if (bg.flags[Starfield::Background_Flags::Corrected_angles_in_mission_file]) {
				_viewport->dialogProvider->showButtonDialog(DialogType::Warning,
					"Incompatibility with retail mission format",
					"Warning: Background flags (including the fixed-angles-in-mission-file flag) are not supported in retail.  The sun and bitmap angles will be loaded differently by previous versions.",
					{ DialogButton::Ok });
				Displayed_retail_background_warning = true;
				break;
			}
		}
	}

	save_mission_internal(savepath);

	if (!err) {
		strcpy_s(backup_name, pathname);
		if (backup_name[strlen(backup_name) - 4] == '.') {
			backup_name[strlen(backup_name) - 4] = 0;
		}

		strcat_s(backup_name, ".bak");
		cf_delete(backup_name, CF_TYPE_MISSIONS);
		cf_rename(pathname, backup_name, CF_TYPE_MISSIONS);
		cf_rename(savepath, pathname, CF_TYPE_MISSIONS);
	}

	return err;
}

int CFred_mission_save::save_mission_info()
{
	required_string_fred("#Mission Info");
	parse_comments(0);

	required_string_fred("$Version:");
	parse_comments(2);
	if (save_format == MissionFormat::RETAIL) {
		// All retail missions, both FS1 and FS2, have the same version
		fout(" %d.%d", LEGACY_MISSION_VERSION.major, LEGACY_MISSION_VERSION.minor);
	} else {
		// Since previous versions of FreeSpace interpret this as a float, this can only have one decimal point
		fout(" %d.%d", The_mission.required_fso_version.major, The_mission.required_fso_version.minor);
	}

	// XSTR
	required_string_fred("$Name:");
	parse_comments();
	fout_ext(" ", "%s", The_mission.name);

	required_string_fred("$Author:");
	parse_comments();
	fout(" %s", The_mission.author.c_str());

	required_string_fred("$Created:");
	parse_comments();
	fout(" %s", The_mission.created);

	required_string_fred("$Modified:");
	parse_comments();
	fout(" %s", The_mission.modified);

	required_string_fred("$Notes:");
	parse_comments();
	fout("\n%s", The_mission.notes);

	required_string_fred("$End Notes:");
	parse_comments(0);

	// XSTR
	required_string_fred("$Mission Desc:");
	parse_comments(2);
	fout_ext("\n", "%s", The_mission.mission_desc);
	fout("\n");

	required_string_fred("$end_multi_text");
	parse_comments(0);

#if 0
    if (optional_string_fred("+Game Type:"))
        parse_comments(2);
    else
        fout("\n\n+Game Type:");
    fout("\n%s", Game_types[The_mission.game_type]);
#endif

	if (optional_string_fred("+Game Type Flags:")) {
		parse_comments(1);
	} else {
		fout("\n+Game Type Flags:");
	}

	fout(" %d", The_mission.game_type);

	if (optional_string_fred("+Flags:")) {
		parse_comments(1);
	} else {
		fout("\n+Flags:");
	}

	fout(" %" PRIu64, The_mission.flags.to_u64());

	// maybe write out Nebula intensity
	if (The_mission.flags[Mission::Mission_Flags::Fullneb]) {
		Assert(Neb2_awacs > 0.0f);
		fout("\n+NebAwacs: %f\n", Neb2_awacs);

		// storm name
		fout("\n+Storm: %s\n", Mission_parse_storm_name);
	}

	// Goober5000
	if (save_format != MissionFormat::RETAIL) {
		// write out the nebula clipping multipliers
		fout("\n+Fog Near Mult: %f\n", Neb2_fog_near_mult);
		fout("\n+Fog Far Mult: %f\n", Neb2_fog_far_mult);

		if (The_mission.contrail_threshold != CONTRAIL_THRESHOLD_DEFAULT) {
			fout("\n$Contrail Speed Threshold: %d\n", The_mission.contrail_threshold);
		}
	}

	{
		bool hasVolumetricNoise = false;

		if (The_mission.volumetrics) {
			fso_comment_push(";;FSO 23.1.0;;");

			if (optional_string_fred("+Volumetric Nebula:"))
				parse_comments(2);
			else
				fout_version("\n+Volumetric Nebula:");
			fout(" %s", The_mission.volumetrics->hullPof.c_str());

			FRED_ENSURE_PROPERTY_VERSION("+Position:", 1, " %f, %f, %f",
				The_mission.volumetrics->pos.xyz.x,
				The_mission.volumetrics->pos.xyz.y,
				The_mission.volumetrics->pos.xyz.z);
			FRED_ENSURE_PROPERTY_VERSION("+Color:", 1, " (%d, %d, %d)",
				static_cast<ubyte>(std::get<0>(The_mission.volumetrics->nebulaColor) * 255.0f),
				static_cast<ubyte>(std::get<1>(The_mission.volumetrics->nebulaColor) * 255.0f),
				static_cast<ubyte>(std::get<2>(The_mission.volumetrics->nebulaColor) * 255.0f));
			FRED_ENSURE_PROPERTY_VERSION("+Visibility Opacity:", 1, " %f", The_mission.volumetrics->alphaLim);
			FRED_ENSURE_PROPERTY_VERSION("+Visibility Distance:", 1, " %f", The_mission.volumetrics->opacityDistance);
			FRED_ENSURE_PROPERTY_VERSION_WITH_DEFAULT("+Steps:", 1, ";;FSO 23.1.0;;", 15, " %d", The_mission.volumetrics->steps);
			FRED_ENSURE_PROPERTY_VERSION_WITH_DEFAULT("+Resolution:", 1, ";;FSO 23.1.0;;", 6, " %d", The_mission.volumetrics->resolution);
			FRED_ENSURE_PROPERTY_VERSION_WITH_DEFAULT("+Oversampling:", 1, ";;FSO 23.1.0;;", 2, " %d", The_mission.volumetrics->oversampling);
			FRED_ENSURE_PROPERTY_VERSION_WITH_DEFAULT("+Smoothing:", 1, ";;FSO 25.0.0;;", 0.f, " %f", The_mission.volumetrics->smoothing);
			FRED_ENSURE_PROPERTY_VERSION_WITH_DEFAULT_F("+Heyney Greenstein Coefficient:", 1, ";;FSO 23.1.0;;", 0.2f, " %f", The_mission.volumetrics->henyeyGreensteinCoeff);
			FRED_ENSURE_PROPERTY_VERSION_WITH_DEFAULT_F("+Sun Falloff Factor:", 1, ";;FSO 23.1.0;;", 1.0f, " %f", The_mission.volumetrics->globalLightDistanceFactor);
			FRED_ENSURE_PROPERTY_VERSION_WITH_DEFAULT("+Sun Steps:", 1, ";;FSO 23.1.0;;", 6, " %d", The_mission.volumetrics->globalLightSteps);
			FRED_ENSURE_PROPERTY_VERSION_WITH_DEFAULT_F("+Emissive Light Spread:", 1, ";;FSO 23.1.0;;", 0.7f, " %f", The_mission.volumetrics->emissiveSpread);
			FRED_ENSURE_PROPERTY_VERSION_WITH_DEFAULT_F("+Emissive Light Intensity:", 1, ";;FSO 23.1.0;;", 1.1f, " %f", The_mission.volumetrics->emissiveIntensity);
			FRED_ENSURE_PROPERTY_VERSION_WITH_DEFAULT_F("+Emissive Light Falloff:", 1, ";;FSO 23.1.0;;", 1.5f, " %f", The_mission.volumetrics->emissiveFalloff);

			if (The_mission.volumetrics->noiseActive) {
				hasVolumetricNoise = true;

				if (optional_string_fred("+Noise:"))
					parse_comments(1);
				else
					fout_version("\n+Noise:");

				FRED_ENSURE_PROPERTY_VERSION("+Scale:", 1, " (%f, %f)", std::get<0>(The_mission.volumetrics->noiseScale), std::get<1>(The_mission.volumetrics->noiseScale));
				FRED_ENSURE_PROPERTY_VERSION("+Color:", 1, " (%d, %d, %d)",
					static_cast<ubyte>(std::get<0>(The_mission.volumetrics->noiseColor) * 255.0f),
					static_cast<ubyte>(std::get<1>(The_mission.volumetrics->noiseColor) * 255.0f),
					static_cast<ubyte>(std::get<2>(The_mission.volumetrics->noiseColor) * 255.0f));
				FRED_ENSURE_PROPERTY_VERSION_WITH_DEFAULT_F("+Intensity:", 1, ";;FSO 23.1.0;;", 1.0f, " %f", The_mission.volumetrics->noiseColorIntensity);
				FRED_ENSURE_PROPERTY_VERSION_IF("+Function Base:", 1, ";;FSO 23.1.0;;", The_mission.volumetrics->noiseColorFunc1, " %s", The_mission.volumetrics->noiseColorFunc1->c_str());
				FRED_ENSURE_PROPERTY_VERSION_IF("+Function Sub:", 1, ";;FSO 23.1.0;;", The_mission.volumetrics->noiseColorFunc2, " %s", The_mission.volumetrics->noiseColorFunc2->c_str());
				FRED_ENSURE_PROPERTY_VERSION_WITH_DEFAULT("+Resolution:", 1, ";;FSO 23.1.0;;", 5, " %d", The_mission.volumetrics->noiseResolution);
			}

			fso_comment_pop();
		}
		else {
			bypass_comment(";;FSO 23.1.0;; +Volumetric Nebula:");
			bypass_comment(";;FSO 23.1.0;; +Position:");
			bypass_comment(";;FSO 23.1.0;; +Color:");
			bypass_comment(";;FSO 23.1.0;; +Visibility Opacity:");
			bypass_comment(";;FSO 23.1.0;; +Visibility Distance:");
			bypass_comment(";;FSO 23.1.0;; +Steps:");
			bypass_comment(";;FSO 23.1.0;; +Resolution:");
			bypass_comment(";;FSO 23.1.0;; +Oversampling:");
			bypass_comment(";;FSO 25.0.0;; +Smoothing:");
			bypass_comment(";;FSO 23.1.0;; +Heyney Greenstein Coefficient:");
			bypass_comment(";;FSO 23.1.0;; +Sun Falloff Factor:");
			bypass_comment(";;FSO 23.1.0;; +Sun Steps:");
			bypass_comment(";;FSO 23.1.0;; +Emissive Light Spread:");
			bypass_comment(";;FSO 23.1.0;; +Emissive Light Intensity:");
			bypass_comment(";;FSO 23.1.0;; +Emissive Light Falloff:");
		}

		if (!hasVolumetricNoise) {
			bypass_comment(";;FSO 23.1.0;; +Noise:");
			bypass_comment(";;FSO 23.1.0;; +Scale:");
			bypass_comment(";;FSO 23.1.0;; +Color:");
			bypass_comment(";;FSO 23.1.0;; +Intensity:");
			bypass_comment(";;FSO 23.1.0;; +Function Base:");
			bypass_comment(";;FSO 23.1.0;; +Function Sub:");
			bypass_comment(";;FSO 23.1.0;; +Resolution:");
		}
	}

	// For multiplayer missions -- write out the number of player starts and number of respawns
	if (The_mission.game_type & MISSION_TYPE_MULTI) {
		if (optional_string_fred("+Num Players:")) {
			parse_comments(2);
		} else {
			fout("\n+Num Players:");
		}

		fout(" %d", Player_starts);

		if (optional_string_fred("+Num Respawns:")) {
			parse_comments(2);
		} else {
			fout("\n+Num Respawns:");
		}

		fout(" %d", The_mission.num_respawns);

		if (save_format != MissionFormat::RETAIL) {
			fso_comment_push(";;FSO 3.6.11;;");
			if (optional_string_fred("+Max Respawn Time:")) {
				parse_comments(2);
			} else {
				fout_version("\n+Max Respawn Time:");
			}
			fso_comment_pop();

			fout(" %d", The_mission.max_respawn_delay);
		} else {
			bypass_comment(";;FSO 3.6.11;; +Max Respawn Time:");
		}
	}

	if (save_format == MissionFormat::RETAIL) {
		if (optional_string_fred("+Red Alert:")) {
			parse_comments(2);
		} else {
			fout("\n+Red Alert:");
		}

		fout(" %d", (The_mission.flags[Mission::Mission_Flags::Red_alert]) ? 1 : 0);
	}

	if (save_format == MissionFormat::RETAIL) //-V581
	{
		if (optional_string_fred("+Scramble:")) {
			parse_comments(2);
		} else {
			fout("\n+Scramble:");
		}

		fout(" %d", (The_mission.flags[Mission::Mission_Flags::Scramble]) ? 1 : 0);
	}

	if (optional_string_fred("+Disallow Support:")) {
		parse_comments(2);
	} else {
		fout("\n+Disallow Support:");
	}
	// this is compatible with non-SCP variants - Goober5000
	fout(" %d", (The_mission.support_ships.max_support_ships == 0) ? 1 : 0);

	// here be WMCoolmon's hull and subsys repair stuff
	if (save_format != MissionFormat::RETAIL) {
		if (optional_string_fred("+Hull Repair Ceiling:")) {
			parse_comments(1);
		} else {
			fout("\n+Hull Repair Ceiling:");
		}
		fout(" %f", The_mission.support_ships.max_hull_repair_val);

		if (optional_string_fred("+Subsystem Repair Ceiling:")) {
			parse_comments(1);
		} else {
			fout("\n+Subsystem Repair Ceiling:");
		}
		fout(" %f", The_mission.support_ships.max_subsys_repair_val);
	}

	if (Mission_all_attack) {
		if (optional_string_fred("+All Teams Attack")) {
			parse_comments();
		} else {
			fout("\n+All Teams Attack");
		}
	}

	if (Entry_delay_time) {
		if (optional_string_fred("+Player Entry Delay:")) {
			parse_comments(2);
		} else {
			fout("\n\n+Player Entry Delay:");
		}

		fout("\n%f", f2fl(Entry_delay_time));
	}

	if (optional_string_fred("+Viewer pos:")) {
		parse_comments(2);
	} else {
		fout("\n\n+Viewer pos:");
	}

	save_vector(_viewport->view_pos);

	if (optional_string_fred("+Viewer orient:")) {
		parse_comments();
	} else {
		fout("\n+Viewer orient:");
	}

	save_matrix(_viewport->view_orient);

	// squadron info
	if (!(The_mission.game_type & MISSION_TYPE_MULTI) && (strlen(The_mission.squad_name) > 0)) { //-V805
		// squad name
		fout("\n+SquadReassignName: %s", The_mission.squad_name);

		// maybe squad logo
		if (strlen(The_mission.squad_filename) > 0) { //-V805
			fout("\n+SquadReassignLogo: %s", The_mission.squad_filename);
		}
	}

	// Goober5000 - special wing info
	if (save_format != MissionFormat::RETAIL) {
		int i;
		fout("\n");

		// starting wings
		if (strcmp(Starting_wing_names[0], "Alpha") != 0 || strcmp(Starting_wing_names[1], "Beta") != 0
			|| strcmp(Starting_wing_names[2], "Gamma") != 0) {
			fout("\n$Starting wing names: ( ");

			for (i = 0; i < MAX_STARTING_WINGS; i++) {
				fout("\"%s\" ", Starting_wing_names[i]);
			}

			fout(")");
		}

		// squadron wings
		if (strcmp(Squadron_wing_names[0], "Alpha") != 0 || strcmp(Squadron_wing_names[1], "Beta") != 0
			|| strcmp(Squadron_wing_names[2], "Gamma") != 0 || strcmp(Squadron_wing_names[3], "Delta") != 0
			|| strcmp(Squadron_wing_names[4], "Epsilon") != 0) {
			fout("\n$Squadron wing names: ( ");

			for (i = 0; i < MAX_SQUADRON_WINGS; i++) {
				fout("\"%s\" ", Squadron_wing_names[i]);
			}

			fout(")");
		}

		// tvt wings
		if (strcmp(TVT_wing_names[0], "Alpha") != 0 || strcmp(TVT_wing_names[1], "Zeta") != 0) {
			fout("\n$Team-versus-team wing names: ( ");

			for (i = 0; i < MAX_TVT_WINGS; i++) {
				fout("\"%s\" ", TVT_wing_names[i]);
			}

			fout(")");
		}
	}

	save_custom_bitmap("$Load Screen 640:",
					   "$Load Screen 1024:",
					   The_mission.loading_screen[GR_640],
					   The_mission.loading_screen[GR_1024],
					   1);

	// Phreak's skybox stuff
	if (strlen(The_mission.skybox_model) > 0) //-V805
	{
		char out_str[NAME_LENGTH];
		char* period;

		// kill off any extension, we will add one here
		strcpy_s(out_str, The_mission.skybox_model);
		period = strrchr(out_str, '.');
		if (period != NULL) {
			*period = 0;
		}

		fso_comment_push(";;FSO 3.6.0;;");
		if (optional_string_fred("$Skybox Model:")) {
			parse_comments(2);
			fout(" %s.pof", out_str);
		} else {
			fout_version("\n\n$Skybox Model: %s.pof", out_str);
		}
		fso_comment_pop();

		const auto& anim_triggers = The_mission.skybox_model_animations.getRegisteredAnimNames();
		const auto& moveable_triggers = The_mission.skybox_model_animations.getRegisteredMoveables();

		if (!anim_triggers.empty() || !moveable_triggers.empty()) {
			fso_comment_push(";;FSO 23.0.0;;");
			if (!anim_triggers.empty()) {
				if (optional_string_fred("$Skybox Model Animations:")) {
					parse_comments(1);
					fout("( ");
					for (const auto& animation : anim_triggers) {
						fout("\"%s\" ", animation.c_str());
					}
					fout(")");
				}
				else {
					fout_version("\n$Skybox Model Animations:");
					fout("( ");
					for (const auto& animation : anim_triggers) {
						fout("\"%s\" ", animation.c_str());
					}
					fout(")");
				}
			}
			if (!moveable_triggers.empty()) {
				if (optional_string_fred("$Skybox Model Moveables:")) {
					parse_comments(1);
					fout("( ");
					for (const auto& moveable : moveable_triggers) {
						fout("\"%s\" ", moveable.c_str());
					}
					fout(")");
				}
				else {
					fout_version("\n$Skybox Model Moveables:");
					fout("( ");
					for (const auto& moveable : moveable_triggers) {
						fout("\"%s\" ", moveable.c_str());
					}
					fout(")");
				}
			}
			fso_comment_pop();
		}
	} else {
		bypass_comment(";;FSO 3.6.0;; $Skybox Model:");
	}

	// orientation?
	if ((strlen(The_mission.skybox_model) > 0)
		&& !vm_matrix_same(&vmd_identity_matrix, &The_mission.skybox_orientation)) {
		fso_comment_push(";;FSO 3.6.14;;");
		if (optional_string_fred("+Skybox Orientation:")) {
			parse_comments(1);
			save_matrix(The_mission.skybox_orientation);
		} else {
			fout_version("\n+Skybox Orientation:");
			save_matrix(The_mission.skybox_orientation);
		}
		fso_comment_pop();
	} else {
		bypass_comment(";;FSO 3.6.14;; +Skybox Orientation:");
	}

	// are skybox flags in use?
	if (The_mission.skybox_flags != DEFAULT_NMODEL_FLAGS) {
		//char out_str[4096];
		fso_comment_push(";;FSO 3.6.11;;");
		if (optional_string_fred("+Skybox Flags:")) {
			parse_comments(1);
			fout(" %d", The_mission.skybox_flags);
		} else {
			fout_version("\n+Skybox Flags: %d", The_mission.skybox_flags);
		}
		fso_comment_pop();
	} else {
		bypass_comment(";;FSO 3.6.11;; +Skybox Flags:");
	}

	// Goober5000's AI profile stuff
	int profile_index = AI_PROFILES_INDEX(The_mission.ai_profile);
	Assert(profile_index >= 0 && profile_index < MAX_AI_PROFILES);

	fso_comment_push(";;FSO 3.6.9;;");
	if (optional_string_fred("$AI Profile:")) {
		parse_comments(2);
		fout(" %s", The_mission.ai_profile->profile_name);
	} else {
		fout_version("\n\n$AI Profile: %s", The_mission.ai_profile->profile_name);
	}
	fso_comment_pop();

	// EatThePath's lighting profiles
	if(The_mission.lighting_profile_name!=lighting_profiles::default_name()){
		fso_comment_push(";;FSO 23.1.0;;");
		if (optional_string_fred("$Lighting Profile:")) {
			parse_comments(1);
			fout(" %s", The_mission.lighting_profile_name.c_str());
		} else {
			fout_version("\n\n$Lighting Profile: %s", The_mission.lighting_profile_name.c_str());
		}
		fso_comment_pop();
	} else {
		bypass_comment(";;FSO 23.1.0;; $Lighting Profile:");
	}

	// sound environment (EFX/EAX) - taylor
	sound_env* m_env = &The_mission.sound_environment;
	if ((m_env->id >= 0) && (m_env->id < (int) EFX_presets.size())) {
		EFXREVERBPROPERTIES* prop = &EFX_presets[m_env->id];

		fso_comment_push(";;FSO 3.6.12;;");

		fout_version("\n\n$Sound Environment: %s", prop->name.c_str());

		if (m_env->volume != prop->flGain) {
			fout_version("\n+Volume: %f", m_env->volume);
		}

		if (m_env->damping != prop->flDecayHFRatio) {
			fout_version("\n+Damping: %f", m_env->damping);
		}

		if (m_env->decay != prop->flDecayTime) {
			fout_version("\n+Decay Time: %f", m_env->decay);
		}

		fso_comment_pop();
	}

	return err;
}

void CFred_mission_save::save_mission_internal(const char* pathname)
{
	time_t currentTime;
	time(&currentTime);
	auto timeinfo = localtime(&currentTime);

	time_to_mission_info_string(timeinfo, The_mission.modified, DATE_TIME_LENGTH - 1);

	// Migrate the version!
	The_mission.required_fso_version = MISSION_VERSION;

	// Additional incremental version update for some features
	auto version_23_3 = gameversion::version(23, 3);
	auto version_24_1 = gameversion::version(24, 1);
	auto version_24_3 = gameversion::version(24, 3);
	if (MISSION_VERSION >= version_24_3)
	{
		Warning(LOCATION, "Notify an SCP coder: now that the required mission version is at least 24.3, the check_for_24_3_data(), the check_for_24_1_data() and check_for_23_3_data() code can be removed");
	}
	else if (check_for_24_3_data())
	{
		The_mission.required_fso_version = version_24_3;
	}
	else if (MISSION_VERSION >= version_24_1)
	{
		Warning(LOCATION, "Notify an SCP coder: now that the required mission version is at least 24.1, the check_for_24_1_data() and check_for_23_3_data() code can be removed");
	}
	else if (check_for_24_1_data())
	{
		The_mission.required_fso_version = version_24_1;
	}
	else if (MISSION_VERSION >= version_23_3)
	{
		Warning(LOCATION, "Notify an SCP coder: now that the required mission version is at least 23.3, the check_for_23_3_data() code can be removed");
	}
	else if (check_for_23_3_data())
	{
		The_mission.required_fso_version = version_23_3;
	}

	reset_parse();
	raw_ptr = Parse_text_raw;
	fred_parse_flag = 0;

	fp = cfopen(pathname, "wt", CF_TYPE_MISSIONS);
	if (!fp) {
		nprintf(("Error", "Can't open mission file to save.\n"));
		err = -1;
		return;
	}

	// Goober5000
	convert_special_tags_to_retail();

	if (save_mission_info()) {
		err = -2;
	} else if (save_plot_info()) {
		err = -3;
	} else if (save_variables()) {
		err = -3;
	} else if (save_containers()) {
		err = -3;
		//	else if (save_briefing_info())
		//		err = -4;
	} else if (save_cutscenes()) {
		err = -4;
	} else if (save_fiction()) {
		err = -3;
	} else if (save_cmd_briefs()) {
		err = -4;
	} else if (save_briefing()) {
		err = -4;
	} else if (save_debriefing()) {
		err = -5;
	} else if (save_players()) {
		err = -6;
	} else if (save_objects()) {
		err = -7;
	} else if (save_wings()) {
		err = -8;
	} else if (save_events()) {
		err = -9;
	} else if (save_goals()) {
		err = -10;
	} else if (save_waypoints()) {
		err = -11;
	} else if (save_messages()) {
		err = -12;
	} else if (save_reinforcements()) {
		err = -13;
	} else if (save_bitmaps()) {
		err = -14;
	} else if (save_asteroid_fields()) {
		err = -15;
	} else if (save_music()) {
		err = -16;
	} else if (save_custom_data()) {
		err = -17;
	} else {
		required_string_fred("#End");
		parse_comments(2);
		token_found = NULL;
		parse_comments();
		fout("\n");
	}

	cfclose(fp);
	if (err)
		mprintf(("Mission saving error code #%d\n", err));
}

int CFred_mission_save::save_music()
{
	required_string_fred("#Music");
	parse_comments(2);

	required_string_fred("$Event Music:");
	parse_comments(2);
	if (Current_soundtrack_num < 0) {
		fout(" None");
	} else {
		fout(" %s", Soundtracks[Current_soundtrack_num].name);
	}

	// Goober5000 - save using the special comment prefix
	if (stricmp(The_mission.substitute_event_music_name, "None") != 0) {
		fso_comment_push(";;FSO 3.6.9;;");
		if (optional_string_fred("$Substitute Event Music:")) {
			parse_comments(1);
			fout(" %s", The_mission.substitute_event_music_name);
		} else {
			fout_version("\n$Substitute Event Music: %s", The_mission.substitute_event_music_name);
		}
		fso_comment_pop();
	} else {
		bypass_comment(";;FSO 3.6.9;; $Substitute Event Music:");
	}

	required_string_fred("$Briefing Music:");
	parse_comments();
	if (Mission_music[SCORE_BRIEFING] < 0) {
		fout(" None");
	} else {
		fout(" %s", Spooled_music[Mission_music[SCORE_BRIEFING]].name);
	}

	// Goober5000 - save using the special comment prefix
	if (stricmp(The_mission.substitute_briefing_music_name, "None") != 0) {
		fso_comment_push(";;FSO 3.6.9;;");
		if (optional_string_fred("$Substitute Briefing Music:")) {
			parse_comments(1);
			fout(" %s", The_mission.substitute_briefing_music_name);
		} else {
			fout_version("\n$Substitute Briefing Music: %s", The_mission.substitute_briefing_music_name);
		}
		fso_comment_pop();
	} else {
		bypass_comment(";;FSO 3.6.9;; $Substitute Briefing Music:");
	}

	// avoid keeping the old one around
	bypass_comment(";;FSO 3.6.8;; $Substitute Music:");

	// old stuff
	if (Mission_music[SCORE_DEBRIEFING_SUCCESS] != event_music_get_spooled_music_index("Success")) {
		if (optional_string_fred("$Debriefing Success Music:")) {
			parse_comments(1);
		} else {
			fout("\n$Debriefing Success Music:");
		}
		fout(" %s",
			 Mission_music[SCORE_DEBRIEFING_SUCCESS] < 0 ? "None"
													  : Spooled_music[Mission_music[SCORE_DEBRIEFING_SUCCESS]].name);
	}
	if (save_format != MissionFormat::RETAIL && Mission_music[SCORE_DEBRIEFING_AVERAGE] != event_music_get_spooled_music_index("Average")) {
		if (optional_string_fred("$Debriefing Average Music:")) {
			parse_comments(1);
		} else {
			fout("\n$Debriefing Average Music:");
		}
		fout(" %s",
			 Mission_music[SCORE_DEBRIEFING_AVERAGE] < 0 ? "None"
													  : Spooled_music[Mission_music[SCORE_DEBRIEFING_AVERAGE]].name);
	}
	if (Mission_music[SCORE_DEBRIEFING_FAILURE] != event_music_get_spooled_music_index("Failure")) {
		if (optional_string_fred("$Debriefing Fail Music:")) {
			parse_comments(1);
		} else {
			fout("\n$Debriefing Fail Music:");
		}
		fout(" %s",
			 Mission_music[SCORE_DEBRIEFING_FAILURE] < 0 ? "None" : Spooled_music[Mission_music[SCORE_DEBRIEFING_FAILURE]].name);
	}

	// Goober5000 - save using the special comment prefix
	if (mission_has_fiction() && Mission_music[SCORE_FICTION_VIEWER] >= 0) {
		fso_comment_push(";;FSO 3.6.11;;");
		if (optional_string_fred("$Fiction Viewer Music:")) {
			parse_comments(1);
			fout(" %s", Spooled_music[Mission_music[SCORE_FICTION_VIEWER]].name);
		} else {
			fout_version("\n$Fiction Viewer Music: %s", Spooled_music[Mission_music[SCORE_FICTION_VIEWER]].name);
		}
		fso_comment_pop();
	} else {
		bypass_comment(";;FSO 3.6.11;; $Fiction Viewer Music:");
	}

	fso_comment_pop(true);

	return err;
}

int CFred_mission_save::save_custom_data()
{
	if (save_format != MissionFormat::RETAIL && (!The_mission.custom_data.empty() || !The_mission.custom_strings.empty())) {
		if (optional_string_fred("#Custom Data", "#End")) {
			parse_comments(2);
		} else {
			fout("\n\n#Custom Data");
		}

		if (The_mission.custom_data.size() > 0) {
			if (optional_string_fred("$begin_data_map")) {
				parse_comments(2);
			} else {
				fout("\n\n$begin_data_map");
			}

			for (const auto& pair : The_mission.custom_data) {
				fout("\n+Val: %s %s", pair.first.c_str(), pair.second.c_str());
			}

			if (optional_string_fred("$end_data_map")) {
				parse_comments();
			} else {
				fout("\n$end_data_map");
			}
		}

		if (The_mission.custom_strings.size() > 0) {
			required_string_fred("$begin_custom_strings");
			parse_comments(2);

			for (const auto& cs : The_mission.custom_strings) {
				if (optional_string_fred("$Name:")) {
					parse_comments(2);
				} else {
					fout("\n$Note:");
				}

				fout("%s", cs.name.c_str());
				parse_comments(2);
				fout("\n+Value: %s", cs.value.c_str());
				parse_comments(2);

				auto copy = cs.text;
				lcl_fred_replace_stuff(copy);
				fout("+String: %s", copy.c_str());

				if (optional_string_fred("$end_multi_text", "$Name:"))
					parse_comments();
				else
					fout_version("\n$end_multi_text");
			}
			required_string_fred("$end_custom_strings");
		}
	}

	return err;
}

int CFred_mission_save::save_warp_params(WarpDirection direction, ship *shipp)
{
	if (save_format == MissionFormat::RETAIL)
		return err;

	// for writing to file; c.f. parse_warp_params
	const char *prefix = (direction == WarpDirection::WARP_IN) ? "$Warpin" : "$Warpout";

	WarpParams *shipp_params, *sip_params;
	if (direction == WarpDirection::WARP_IN)
	{
		// if exactly the same params used, no need to output anything
		if (shipp->warpin_params_index == Ship_info[shipp->ship_info_index].warpin_params_index)
			return err;

		shipp_params = &Warp_params[shipp->warpin_params_index];
		sip_params = &Warp_params[Ship_info[shipp->ship_info_index].warpin_params_index];
	}
	else
	{
		// if exactly the same params used, no need to output anything
		if (shipp->warpout_params_index == Ship_info[shipp->ship_info_index].warpout_params_index)
			return err;

		shipp_params = &Warp_params[shipp->warpout_params_index];
		sip_params = &Warp_params[Ship_info[shipp->ship_info_index].warpout_params_index];
	}

	if (shipp_params->warp_type != sip_params->warp_type)
	{
		// is it a fireball?
		if (shipp_params->warp_type & WT_DEFAULT_WITH_FIREBALL)
		{
			fout("\n%s type: %s", prefix, Fireball_info[shipp_params->warp_type & WT_FLAG_MASK].unique_id);
		}
		// probably a warp type
		else if (shipp_params->warp_type >= 0 && shipp_params->warp_type < Num_warp_types)
		{
			fout("\n%s type: %s", prefix, Warp_types[shipp_params->warp_type]);
		}
	}

	if (shipp_params->snd_start != sip_params->snd_start)
	{
		if (shipp_params->snd_start.isValid())
			fout("\n%s Start Sound: %s", prefix, gamesnd_get_game_sound(shipp_params->snd_start)->name.c_str());
	}

	if (shipp_params->snd_end != sip_params->snd_end)
	{
		if (shipp_params->snd_end.isValid())
			fout("\n%s End Sound: %s", prefix, gamesnd_get_game_sound(shipp_params->snd_end)->name.c_str());
	}

	if (direction == WarpDirection::WARP_OUT && shipp_params->warpout_engage_time != sip_params->warpout_engage_time)
	{
		if (shipp_params->warpout_engage_time > 0)
			fout("\n%s engage time: %.2f", prefix, i2fl(shipp_params->warpout_engage_time) / 1000.0f);
	}

	if (shipp_params->speed != sip_params->speed)
	{
		if (shipp_params->speed > 0.0f)
			fout("\n%s speed: %.2f", prefix, shipp_params->speed);
	}

	if (shipp_params->time != sip_params->time)
	{
		if (shipp_params->time > 0)
			fout("\n%s time: %.2f", prefix, i2fl(shipp_params->time) / 1000.0f);
	}

	if (shipp_params->accel_exp != sip_params->accel_exp)
	{
		if (shipp_params->accel_exp > 0.0f)
			fout("\n%s %s exp: %.2f", prefix, direction == WarpDirection::WARP_IN ? "decel" : "accel", shipp_params->accel_exp);
	}

	if (shipp_params->radius != sip_params->radius)
	{
		if (shipp_params->radius > 0.0f)
			fout("\n%s radius: %.2f", prefix, shipp_params->radius);
	}

	if (stricmp(shipp_params->anim, sip_params->anim) != 0)
	{
		if (strlen(shipp_params->anim) > 0)
			fout("\n%s animation: %s", prefix, shipp_params->anim);
	}

	if (shipp_params->supercap_warp_physics != sip_params->supercap_warp_physics)
	{
		fout("\n$Supercap warp%s physics: %s", direction == WarpDirection::WARP_IN ? "in" : "out", shipp_params->supercap_warp_physics ? "YES" : "NO");
	}

	if (direction == WarpDirection::WARP_OUT && shipp_params->warpout_player_speed != sip_params->warpout_player_speed)
	{
		if (shipp_params->warpout_player_speed > 0.0f)
			fout("\n$Player warpout speed: %.2f", shipp_params->warpout_player_speed);
	}

	return err;
}

int CFred_mission_save::save_objects()
{
	SCP_string sexp_out;
	int i, z;
	object* objp;
	ship* shipp;
	ship_info* sip;

	required_string_fred("#Objects");
	parse_comments(2);
	fout("\t\t;! %d total\n", ship_get_num_ships());

	for (i = z = 0; i < MAX_SHIPS; i++) {
		if (Ships[i].objnum < 0) {
			continue;
		}

		auto j = Objects[Ships[i].objnum].type;
		if ((j != OBJ_SHIP) && (j != OBJ_START)) {
			continue;
		}

		shipp = &Ships[i];
		objp = &Objects[shipp->objnum];
		sip = &Ship_info[shipp->ship_info_index];
		required_string_either_fred("$Name:", "#Wings");
		required_string_fred("$Name:");
		parse_comments(z ? 2 : 1);
		fout(" %s\t\t;! Object #%d", shipp->ship_name, i);

		// Display name
		// The display name is only written if there was one at the start to avoid introducing inconsistencies
		if (save_format != MissionFormat::RETAIL && (_viewport->Always_save_display_names || shipp->has_display_name())) {
			char truncated_name[NAME_LENGTH];
			strcpy_s(truncated_name, shipp->ship_name);
			end_string_at_first_hash_symbol(truncated_name);

			// Also, the display name is not written if it's just the truncation of the name at the hash
			if (_viewport->Always_save_display_names || strcmp(shipp->get_display_name(), truncated_name) != 0) {
				if (optional_string_fred("$Display name:", "$Class:")) {
					parse_comments();
				} else {
					fout("\n$Display name:");
				}
				fout_ext(" ", "%s", shipp->get_display_name());
			}
		}

		required_string_fred("\n$Class:");
		parse_comments(0);
		fout(" %s", Ship_info[shipp->ship_info_index].name);

		//alt classes stuff
		if (save_format != MissionFormat::RETAIL) {
			for (SCP_vector<alt_class>::iterator ii = shipp->s_alt_classes.begin(); ii != shipp->s_alt_classes.end();
				 ++ii) {
				// is this a variable?
				if (ii->variable_index != -1) {
					fout("\n$Alt Ship Class: @%s", Sexp_variables[ii->variable_index].variable_name);
				} else {
					fout("\n$Alt Ship Class: \"%s\"", Ship_info[ii->ship_class].name);
				}

				// default class?					
				if (ii->default_to_this_class) {
					fout("\n+Default Class:");
				}
			}
		}

		// optional alternate type name
		if (strlen(Fred_alt_names[i])) {
			fout("\n$Alt: %s\n", Fred_alt_names[i]);
		}

		// optional callsign
		if (save_format != MissionFormat::RETAIL && strlen(Fred_callsigns[i])) {
			fout("\n$Callsign: %s\n", Fred_callsigns[i]);
		}

		required_string_fred("$Team:");
		parse_comments();
		fout(" %s", Iff_info[shipp->team].iff_name);

		if (save_format != MissionFormat::RETAIL && Ship_info[shipp->ship_info_index].uses_team_colors) {
			required_string_fred("$Team Color Setting:");
			parse_comments();
			fout(" %s", shipp->team_name.c_str());
		}

		required_string_fred("$Location:");
		parse_comments();
		save_vector(Objects[shipp->objnum].pos);

		required_string_fred("$Orientation:");
		parse_comments();
		save_matrix(Objects[shipp->objnum].orient);

		if (save_format == MissionFormat::RETAIL) {
			required_string_fred("$IFF:");
			parse_comments();
			fout(" %s", "IFF 1");

			required_string_fred("$AI Behavior:");
			parse_comments();
			fout(" %s", Ai_behavior_names[AIM_NONE]);
		}

		if (shipp->weapons.ai_class != Ship_info[shipp->ship_info_index].ai_class) {
			if (optional_string_fred("+AI Class:", "$Name:")) {
				parse_comments();
			} else {
				fout("\n+AI Class:");
			}

			fout(" %s", Ai_class_names[shipp->weapons.ai_class]);
		}

		save_ai_goals(Ai_info[shipp->ai_index].goals, i);

		// XSTR
		required_string_fred("$Cargo 1:");
		parse_comments();
		fout_ext(" ", "%s", Cargo_names[shipp->cargo1]);

		if (save_format != MissionFormat::RETAIL) {
			if (shipp->cargo_title[0] != '\0') {
				if (optional_string_fred("$Cargo Title:", "$Name:")) {
					parse_comments();
				} else {
					fout("\n$Cargo Title:");
				}
				fout_ext(nullptr, "%s", shipp->cargo_title);
			}
		}

		save_common_object_data(&Objects[shipp->objnum], &Ships[i]);

		if (shipp->wingnum >= 0) {
			shipp->arrival_location = ArrivalLocation::AT_LOCATION;
		}

		required_string_fred("$Arrival Location:");
		parse_comments();
		fout(" %s", Arrival_location_names[static_cast<int>(shipp->arrival_location)]);

		if (shipp->arrival_location != ArrivalLocation::AT_LOCATION) {
			if (optional_string_fred("+Arrival Distance:", "$Name:")) {
				parse_comments();
			} else {
				fout("\n+Arrival Distance:");
			}

			fout(" %d", shipp->arrival_distance);
			if (optional_string_fred("$Arrival Anchor:", "$Name:")) {
				parse_comments();
			} else {
				fout("\n$Arrival Anchor:");
			}

			z = shipp->arrival_anchor;
			if (z & SPECIAL_ARRIVAL_ANCHOR_FLAG) {
				// get name
				char tmp[NAME_LENGTH + 15];
				stuff_special_arrival_anchor_name(tmp, z, save_format == MissionFormat::RETAIL);

				// save it
				fout(" %s", tmp);
			} else if (z >= 0) {
				fout(" %s", Ships[z].ship_name);
			} else {
				fout(" <error>");
			}
		}

		// Goober5000
		if (save_format != MissionFormat::RETAIL) {
			if ((shipp->arrival_location == ArrivalLocation::FROM_DOCK_BAY) && (shipp->arrival_path_mask > 0)) {
				int anchor_shipnum;
				polymodel* pm;

				anchor_shipnum = shipp->arrival_anchor;
				Assert(anchor_shipnum >= 0 && anchor_shipnum < MAX_SHIPS);

				fout("\n+Arrival Paths: ( ");

				pm = model_get(Ship_info[Ships[anchor_shipnum].ship_info_index].model_num);
				for (auto n = 0; n < pm->ship_bay->num_paths; n++) {
					if (shipp->arrival_path_mask & (1 << n)) {
						fout("\"%s\" ", pm->paths[pm->ship_bay->path_indexes[n]].name);
					}
				}

				fout(")");
			}
		}

		if (shipp->arrival_delay) {
			if (optional_string_fred("+Arrival Delay:", "$Name:")) {
				parse_comments();
			} else {
				fout("\n+Arrival Delay:");
			}

			fout(" %d", shipp->arrival_delay);
		}

		required_string_fred("$Arrival Cue:");
		parse_comments();
		convert_sexp_to_string(sexp_out, shipp->arrival_cue, SEXP_SAVE_MODE);
		fout(" %s", sexp_out.c_str());

		if (shipp->wingnum >= 0) {
			shipp->departure_location = DepartureLocation::AT_LOCATION;
		}

		required_string_fred("$Departure Location:");
		parse_comments();
		fout(" %s", Departure_location_names[static_cast<int>(shipp->departure_location)]);

		if (shipp->departure_location != DepartureLocation::AT_LOCATION) {
			required_string_fred("$Departure Anchor:");
			parse_comments();

			if (shipp->departure_anchor >= 0) {
				fout(" %s", Ships[shipp->departure_anchor].ship_name);
			} else {
				fout(" <error>");
			}
		}

		// Goober5000
		if (save_format != MissionFormat::RETAIL) {
			if ((shipp->departure_location == DepartureLocation::TO_DOCK_BAY) && (shipp->departure_path_mask > 0)) {
				int anchor_shipnum;
				polymodel* pm;

				anchor_shipnum = shipp->departure_anchor;
				Assert(anchor_shipnum >= 0 && anchor_shipnum < MAX_SHIPS);

				fout("\n+Departure Paths: ( ");

				pm = model_get(Ship_info[Ships[anchor_shipnum].ship_info_index].model_num);
				for (auto n = 0; n < pm->ship_bay->num_paths; n++) {
					if (shipp->departure_path_mask & (1 << n)) {
						fout("\"%s\" ", pm->paths[pm->ship_bay->path_indexes[n]].name);
					}
				}

				fout(")");
			}
		}

		if (shipp->departure_delay) {
			if (optional_string_fred("+Departure delay:", "$Name:")) {
				parse_comments();
			} else {
				fout("\n+Departure delay:");
			}

			fout(" %d", shipp->departure_delay);
		}

		required_string_fred("$Departure Cue:");
		parse_comments();
		convert_sexp_to_string(sexp_out, shipp->departure_cue, SEXP_SAVE_MODE);
		fout(" %s", sexp_out.c_str());

		save_warp_params(WarpDirection::WARP_IN, shipp);
		save_warp_params(WarpDirection::WARP_OUT, shipp);

		required_string_fred("$Determination:");
		parse_comments();
		fout(" 10"); // dummy value for backwards compatibility

		if (optional_string_fred("+Flags:", "$Name:")) {
			parse_comments();
			fout(" (");
		} else {
			fout("\n+Flags: (");
		}

		if (shipp->flags[Ship::Ship_Flags::Cargo_revealed]) {
			fout(" \"cargo-known\"");
		}
		if (shipp->flags[Ship::Ship_Flags::Ignore_count]) {
			fout(" \"ignore-count\"");
		}
		if (objp->flags[Object::Object_Flags::Protected]) {
			fout(" \"protect-ship\"");
		}
		if (shipp->flags[Ship::Ship_Flags::Reinforcement]) {
			fout(" \"reinforcement\"");
		}
		if (objp->flags[Object::Object_Flags::No_shields] && !sip->flags[Ship::Info_Flags::Intrinsic_no_shields]) {	// don't save no-shields for intrinsic-no-shields ships
			fout(" \"no-shields\"");
		}
		if (shipp->flags[Ship::Ship_Flags::Escort]) {
			fout(" \"escort\"");
		}
		if (objp->type == OBJ_START) {
			fout(" \"player-start\"");
		}
		if (shipp->flags[Ship::Ship_Flags::No_arrival_music]) {
			fout(" \"no-arrival-music\"");
		}
		if (shipp->flags[Ship::Ship_Flags::No_arrival_warp]) {
			fout(" \"no-arrival-warp\"");
		}
		if (shipp->flags[Ship::Ship_Flags::No_departure_warp]) {
			fout(" \"no-departure-warp\"");
		}
		if (Objects[shipp->objnum].flags[Object::Object_Flags::Invulnerable]) {
			fout(" \"invulnerable\"");
		}
		if (shipp->flags[Ship::Ship_Flags::Hidden_from_sensors]) {
			fout(" \"hidden-from-sensors\"");
		}
		if (shipp->flags[Ship::Ship_Flags::Scannable]) {
			fout(" \"scannable\"");
		}
		if (Ai_info[shipp->ai_index].ai_flags[AI::AI_Flags::Kamikaze]) {
			fout(" \"kamikaze\"");
		}
		if (Ai_info[shipp->ai_index].ai_flags[AI::AI_Flags::No_dynamic]) {
			fout(" \"no-dynamic\"");
		}
		if (shipp->flags[Ship::Ship_Flags::Red_alert_store_status]) {
			fout(" \"red-alert-carry\"");
		}
		if (objp->flags[Object::Object_Flags::Beam_protected]) {
			fout(" \"beam-protect-ship\"");
		}
		if (objp->flags[Object::Object_Flags::Flak_protected]) {
			fout(" \"flak-protect-ship\"");
		}
		if (objp->flags[Object::Object_Flags::Laser_protected]) {
			fout(" \"laser-protect-ship\"");
		}
		if (objp->flags[Object::Object_Flags::Missile_protected]) {
			fout(" \"missile-protect-ship\"");
		}
		if (shipp->ship_guardian_threshold != 0) {
			fout(" \"guardian\"");
		}
		if (objp->flags[Object::Object_Flags::Special_warpin]) {
			fout(" \"special-warp\"");
		}
		if (shipp->flags[Ship::Ship_Flags::Vaporize]) {
			fout(" \"vaporize\"");
		}
		if (shipp->flags[Ship::Ship_Flags::Stealth]) {
			fout(" \"stealth\"");
		}
		if (shipp->flags[Ship::Ship_Flags::Friendly_stealth_invis]) {
			fout(" \"friendly-stealth-invisible\"");
		}
		if (shipp->flags[Ship::Ship_Flags::Dont_collide_invis]) {
			fout(" \"don't-collide-invisible\"");
		}
		//for compatibility reasons ship locked or weapons locked are saved as both locked in retail mode
		if ((save_format == MissionFormat::RETAIL)
			&& ((shipp->flags[Ship::Ship_Flags::Ship_locked]) || (shipp->flags[Ship::Ship_Flags::Weapons_locked]))) {
				fout(" \"locked\"");
		}
		fout(" )");

		// flags2 added by Goober5000 --------------------------------
		if (save_format != MissionFormat::RETAIL) {
			if (optional_string_fred("+Flags2:", "$Name:")) {
				parse_comments();
				fout(" (");
			} else {
				fout("\n+Flags2: (");
			}

			if (shipp->flags[Ship::Ship_Flags::Primitive_sensors]) {
				fout(" \"primitive-sensors\"");
			}
			if (shipp->flags[Ship::Ship_Flags::No_subspace_drive]) {
				fout(" \"no-subspace-drive\"");
			}
			if (shipp->flags[Ship::Ship_Flags::Navpoint_carry]) {
				fout(" \"nav-carry-status\"");
			}
			if (shipp->flags[Ship::Ship_Flags::Affected_by_gravity]) {
				fout(" \"affected-by-gravity\"");
			}
			if (shipp->flags[Ship::Ship_Flags::Toggle_subsystem_scanning]) {
				fout(" \"toggle-subsystem-scanning\"");
			}
			if (objp->flags[Object::Object_Flags::Targetable_as_bomb]) {
				fout(" \"targetable-as-bomb\"");
			}
			if (shipp->flags[Ship::Ship_Flags::No_builtin_messages]) {
				fout(" \"no-builtin-messages\"");
			}
			if (shipp->flags[Ship::Ship_Flags::Primaries_locked]) {
				fout(" \"primaries-locked\"");
			}
			if (shipp->flags[Ship::Ship_Flags::Secondaries_locked]) {
				fout(" \"secondaries-locked\"");
			}
			if (shipp->flags[Ship::Ship_Flags::No_death_scream]) {
				fout(" \"no-death-scream\"");
			}
			if (shipp->flags[Ship::Ship_Flags::Always_death_scream]) {
				fout(" \"always-death-scream\"");
			}
			if (shipp->flags[Ship::Ship_Flags::Navpoint_needslink]) {
				fout(" \"nav-needslink\"");
			}
			if (shipp->flags[Ship::Ship_Flags::Hide_ship_name]) {
				fout(" \"hide-ship-name\"");
			}
			if (shipp->flags[Ship::Ship_Flags::Set_class_dynamically]) {
				fout(" \"set-class-dynamically\"");
			}
			if (shipp->flags[Ship::Ship_Flags::Lock_all_turrets_initially]) {
				fout(" \"lock-all-turrets\"");
			}
			if (shipp->flags[Ship::Ship_Flags::Afterburner_locked]) {
				fout(" \"afterburners-locked\"");
			}
			if (shipp->flags[Ship::Ship_Flags::Force_shields_on]) {
				fout(" \"force-shields-on\"");
			}
			if (objp->flags[Object::Object_Flags::Dont_change_position]) {
				fout(" \"don't-change-position\"");
			}
			if (objp->flags[Object::Object_Flags::Dont_change_orientation]) {
				fout(" \"don't-change-orientation\"");
			}
			if (shipp->flags[Ship::Ship_Flags::No_ets]) {
				fout(" \"no-ets\"");
			}
			if (shipp->flags[Ship::Ship_Flags::Cloaked]) {
				fout(" \"cloaked\"");
			}
			if (shipp->flags[Ship::Ship_Flags::Ship_locked]) {
				fout(" \"ship-locked\"");
			}
			if (shipp->flags[Ship::Ship_Flags::Weapons_locked]) {
				fout(" \"weapons-locked\"");
			}
			if (shipp->flags[Ship::Ship_Flags::Scramble_messages]) {
				fout(" \"scramble-messages\"");
			}
			if (!(objp->flags[Object::Object_Flags::Collides])) {
				fout(" \"no_collide\"");
			}
			if (shipp->flags[Ship::Ship_Flags::No_disabled_self_destruct]) {
				fout(" \"no-disabled-self-destruct\"");
			}
			if (shipp->flags[Ship::Ship_Flags::Same_arrival_warp_when_docked]) {
				fout(" \"same-arrival-warp-when-docked\"");
			}
			if (shipp->flags[Ship::Ship_Flags::Same_departure_warp_when_docked]) {
				fout(" \"same-departure-warp-when-docked\"");
			}
			if (objp->flags[Object::Object_Flags::Attackable_if_no_collide]) {
				fout(" \"ai-attackable-if-no-collide\"");
			}
			if (shipp->flags[Ship::Ship_Flags::Fail_sound_locked_primary]) {
				fout(" \"fail-sound-locked-primary\"");
			}
			if (shipp->flags[Ship::Ship_Flags::Fail_sound_locked_secondary]) {
				fout(" \"fail-sound-locked-secondary\"");
			}
			if (shipp->flags[Ship::Ship_Flags::Aspect_immune]) {
				fout(" \"aspect-immune\"");
			}
			if (shipp->flags[Ship::Ship_Flags::Cannot_perform_scan]) {
				fout(" \"cannot-perform-scan\"");
			}
			if (shipp->flags[Ship::Ship_Flags::No_targeting_limits]) {
				fout(" \"no-targeting-limits\"");
			}
			fout(" )");
		}
		// -----------------------------------------------------------

		fout("\n+Respawn priority: %d", shipp->respawn_priority);    // HA!  Newline added by Goober5000

		if (shipp->flags[Ship::Ship_Flags::Escort]) {
			if (optional_string_fred("+Escort priority:", "$Name:")) {
				parse_comments();
			} else {
				fout("\n+Escort priority:");
			}

			fout(" %d", shipp->escort_priority);
		}

		// special explosions
		if (save_format != MissionFormat::RETAIL) {
			if (shipp->use_special_explosion) {
				fso_comment_push(";;FSO 3.6.13;;");
				if (optional_string_fred("$Special Explosion:", "$Name:")) {
					parse_comments();

					required_string_fred("+Special Exp Damage:");
					parse_comments();
					fout(" %d", shipp->special_exp_damage);

					required_string_fred("+Special Exp Blast:");
					parse_comments();
					fout(" %d", shipp->special_exp_blast);

					required_string_fred("+Special Exp Inner Radius:");
					parse_comments();
					fout(" %d", shipp->special_exp_inner);

					required_string_fred("+Special Exp Outer Radius:");
					parse_comments();
					fout(" %d", shipp->special_exp_outer);

					if (shipp->use_shockwave && (shipp->special_exp_shockwave_speed > 0)) {
						optional_string_fred("+Special Exp Shockwave Speed:");
						parse_comments();
						fout(" %d", shipp->special_exp_shockwave_speed);
					} else {
						bypass_comment(";;FSO 3.6.13;; +Special Exp Shockwave Speed:", "$Name:");
					}

					if (shipp->special_exp_deathroll_time > 0) {
						optional_string_fred("+Special Exp Death Roll Time:");
						parse_comments();
						fout(" %d", shipp->special_exp_deathroll_time);
					} else {
						bypass_comment(";;FSO 3.6.13;; +Special Exp Death Roll Time:", "$Name:");
					}
				} else {
					fout_version("\n$Special Explosion:");

					fout_version("\n+Special Exp Damage:");
					fout(" %d", shipp->special_exp_damage);

					fout_version("\n+Special Exp Blast:");
					fout(" %d", shipp->special_exp_blast);

					fout_version("\n+Special Exp Inner Radius:");
					fout(" %d", shipp->special_exp_inner);

					fout_version("\n+Special Exp Outer Radius:");
					fout(" %d", shipp->special_exp_outer);

					if (shipp->use_shockwave && (shipp->special_exp_shockwave_speed > 0)) {
						fout_version("\n+Special Exp Shockwave Speed:");
						fout(" %d", shipp->special_exp_shockwave_speed);
					}

					if (shipp->special_exp_deathroll_time > 0) {
						fout_version("\n+Special Exp Death Roll Time:");
						fout(" %d", shipp->special_exp_deathroll_time);
					}

				}
				fso_comment_pop();
			} else {
				bypass_comment(";;FSO 3.6.13;; +Special Exp Shockwave Speed:", "$Name:");
				bypass_comment(";;FSO 3.6.13;; +Special Exp Death Roll Time:", "$Name:");
			}
		}
			// retail format special explosions
		else {
			if (shipp->use_special_explosion) {
				int special_exp_index;

				if (has_special_explosion_block_index(&Ships[i], &special_exp_index)) {
					fout("\n+Special Exp index:");
					fout(" %d", special_exp_index);
				} else {
					SCP_string text = "You are saving in the retail mission format, but ";
					text += "the mission has too many special explosions defined. \"";
					text += shipp->ship_name;
					text += "\" has therefore lost any special explosion data that was defined for it. ";
					text += "\" Either remove special explosions or SEXP variables if you need it to have one ";
					_viewport->dialogProvider->showButtonDialog(DialogType::Warning,
																"Too many variables!",
																text,
																{ DialogButton::Ok });
				}
			}
		}

		// Goober5000 ------------------------------------------------
		if (save_format != MissionFormat::RETAIL) {
			if (shipp->special_hitpoints) {
				fso_comment_push(";;FSO 3.6.13;;");
				if (optional_string_fred("+Special Hitpoints:", "$Name:")) {
					parse_comments();
				} else {
					fout_version("\n+Special Hitpoints:");
				}
				fso_comment_pop();

				fout(" %d", shipp->special_hitpoints);
			} else {
				bypass_comment(";;FSO 3.6.13;; +Special Hitpoints:", "$Name:");
			}

			if (shipp->special_shield >= 0) {
				fso_comment_push(";;FSO 3.6.13;;");
				if (optional_string_fred("+Special Shield Points:", "$Name:")) {
					parse_comments();
				} else {
					fout_version("\n+Special Shield Points:");
				}
				fso_comment_pop();

				fout(" %d", shipp->special_shield);
			} else {
				bypass_comment(";;FSO 3.6.13;; +Special Shield Points:", "$Name:");
			}
		}
		// -----------------------------------------------------------

		if (Ai_info[shipp->ai_index].ai_flags[AI::AI_Flags::Kamikaze]) {
			if (optional_string_fred("+Kamikaze Damage:", "$Name:")) {
				parse_comments();
			} else {
				fout("\n+Kamikaze Damage:");
			}

			fout(" %d", Ai_info[shipp->ai_index].kamikaze_damage);
		}

		if (shipp->hotkey != -1) {
			if (optional_string_fred("+Hotkey:", "$Name:")) {
				parse_comments();
			} else {
				fout("\n+Hotkey:");
			}

			fout(" %d", shipp->hotkey);
		}

		// mwa -- new code to save off information about initially docked ships.
		// Goober5000 - newer code to save off information about initially docked ships. ;)
		if (object_is_docked(&Objects[shipp->objnum])) {
			// possible incompatibility
			if (save_format == MissionFormat::RETAIL && !dock_check_docked_one_on_one(&Objects[shipp->objnum])) {
				static bool warned = false;
				if (!warned) {
					SCP_string text = "You are saving in the retail mission format, but \"";
					text += shipp->ship_name;
					text += "\" is docked to more than one ship.  If you wish to run this mission in retail, ";
					text += "you should remove the additional ships and save the mission again.";
					_viewport->dialogProvider->showButtonDialog(DialogType::Warning,
																"Incompatibility with retail mission format",
																text,
																{ DialogButton::Ok });

					warned = true;    // to avoid zillions of boxes
				}
			}

			// save one-on-one groups as if they were retail
			if (dock_check_docked_one_on_one(&Objects[shipp->objnum])) {
				// retail format only saved information for non-leaders
				if (!(shipp->flags[Ship::Ship_Flags::Dock_leader])) {
					save_single_dock_instance(&Ships[i], Objects[shipp->objnum].dock_list);
				}
			}
				// multiply docked
			else {
				// save all instances for all ships
				for (dock_instance* dock_ptr = Objects[shipp->objnum].dock_list; dock_ptr != NULL;
					 dock_ptr = dock_ptr->next) {
					save_single_dock_instance(&Ships[i], dock_ptr);
				}
			}
		}

		// check the ship flag about killing off the ship before a mission starts.  Write out the appropriate
		// variable if necessary
		if (shipp->flags[Ship::Ship_Flags::Kill_before_mission]) {
			if (optional_string_fred("+Destroy At:", "$Name:")) {
				parse_comments();
			} else {
				fout("\n+Destroy At: ");
			}

			fout(" %d", shipp->final_death_time);
		}

		// possibly write out the orders that this ship will accept.  We'll only do it if the orders
		// are not the default set of orders
		if (shipp->orders_accepted != ship_get_default_orders_accepted(&Ship_info[shipp->ship_info_index])) {
			if (save_format == MissionFormat::RETAIL) {
				if (optional_string_fred("+Orders Accepted:", "$Name:")) {
					parse_comments();
				} else {
					fout("\n+Orders Accepted:");
				}

				int bitfield = 0;
				for (size_t order : shipp->orders_accepted) {
					if (order < 32)
						bitfield |= (1 << (order - 1)); //The first "true" order starts at idx 1, since 0 can be "no order"
				}
				fout(" %d\t\t;! note that this is a bitfield!!!", bitfield);
			} else {
				if (optional_string_fred("+Orders Accepted List:", "$Name:")) {
					parse_comments();
				} else {
					fout("\n+Orders Accepted List:");
				}

				fout(" (");
				for (size_t order_id : shipp->orders_accepted) {
					const auto& order = Player_orders[order_id];
					fout(" \"%s\"", order.parse_name.c_str());
				}
				fout(" )");
			}
		}

		if (shipp->group >= 0) {
			if (optional_string_fred("+Group:", "$Name:")) {
				parse_comments();
			} else {
				fout("\n+Group:");
			}

			fout(" %d", shipp->group);
		}

		// always write out the score to ensure backwards compatibility. If the score is the same as the value 
		// in the table write out a flag to tell the game to simply use whatever is in the table instead
		if (Ship_info[shipp->ship_info_index].score == shipp->score) {
			fso_comment_push(";;FSO 3.6.10;;");
			if (optional_string_fred("+Use Table Score:", "$Name:")) {
				parse_comments();
			} else {
				fout_version("\n+Use Table Score:");
			}
			fso_comment_pop();
		} else {
			bypass_comment(";;FSO 3.6.10;; +Use Table Score:", "$Name:");
		}

		if (optional_string_fred("+Score:", "$Name:")) {
			parse_comments();
		} else {
			fout("\n+Score:");
		}

		fout(" %d", shipp->score);


		if (save_format != MissionFormat::RETAIL && shipp->assist_score_pct != 0) {
			fso_comment_push(";;FSO 3.6.10;;");
			if (optional_string_fred("+Assist Score Percentage:")) {
				parse_comments();
			} else {
				fout_version("\n+Assist Score Percentage:");
			}
			fso_comment_pop();

			fout(" %f", shipp->assist_score_pct);
		} else {
			bypass_comment(";;FSO 3.6.10;; +Assist Score Percentage:", "$Name:");
		}

		// deal with the persona for this ship as well.
		if (shipp->persona_index != -1) {
			if (save_format == MissionFormat::RETAIL) {
				if (optional_string_fred("+Persona Index:", "$Name:"))
					parse_comments();
				else
					fout("\n+Persona Index:");

				fout(" %d", shipp->persona_index);
			} else {
				if (optional_string_fred("+Persona Name:", "$Name:"))
					parse_comments();
				else
					fout("\n+Persona Name:");

				fout(" %s", Personas[shipp->persona_index].name);
			}
		}

		// Goober5000 - deal with texture replacement ----------------
		if (!Fred_texture_replacements.empty()) {
			bool needs_header = true;
			fso_comment_push(";;FSO 3.6.8;;");

			for (SCP_vector<texture_replace>::iterator ii = Fred_texture_replacements.begin();
				 ii != Fred_texture_replacements.end(); ++ii) {
				// Only look at this entry if it's not from the table. Table entries will just be read by FSO.
				if (!stricmp(shipp->ship_name, ii->ship_name) && !(ii->from_table)) {
					if (needs_header) {
						if (optional_string_fred("$Texture Replace:")) {
							parse_comments(1);
						} else {
							fout_version("\n$Texture Replace:");
						}

						needs_header = false;
					}

					// write out this entry
					if (optional_string_fred("+old:")) {
						parse_comments(1);
						fout(" %s", ii->old_texture);
					} else {
						fout_version("\n+old: %s", ii->old_texture);
					}

					if (optional_string_fred("+new:")) {
						parse_comments(1);
						fout(" %s", ii->new_texture);
					} else {
						fout_version("\n+new: %s", ii->new_texture);
					}
				}
			}

			fso_comment_pop();
		} else {
			bypass_comment(";;FSO 3.6.8;; $Texture Replace:", "$Name:");
		}

		// end of texture replacement -------------------------------

		z++;

		fso_comment_pop();
	}

	fso_comment_pop(true);

	return err;
}

int CFred_mission_save::save_players()
{
	bool wrote_fso_data = false;
	int i, j;
	int var_idx;
	int used_pool[MAX_WEAPON_TYPES];

	if (optional_string_fred("#Alternate Types:")) {    // Make sure the parser doesn't get out of sync
		required_string_fred("#end");
	}
	if (optional_string_fred("#Callsigns:")) {
		required_string_fred("#end");
	}

	// write out alternate name list
	if (Mission_alt_type_count > 0) {
		fout("\n\n#Alternate Types:\n");

		// write them all out
		for (i = 0; i < Mission_alt_type_count; i++) {
			fout("$Alt: %s\n", Mission_alt_types[i]);
		}

		// end
		fout("\n#end\n");
	}

	// write out callsign list
	if (save_format != MissionFormat::RETAIL && Mission_callsign_count > 0) {
		fout("\n\n#Callsigns:\n");

		// write them all out
		for (i = 0; i < Mission_callsign_count; i++) {
			fout("$Callsign: %s\n", Mission_callsigns[i]);
		}

		// end
		fout("\n#end\n");
	}

	required_string_fred("#Players");
	parse_comments(2);
	fout("\t\t;! %d total\n", Player_starts);

	SCP_vector<SCP_string> e_list = ai_lua_get_general_orders(true);

	if (save_format != MissionFormat::RETAIL && (e_list.size() > 0)) {
		if (optional_string_fred("+General Orders Enabled:", "#Players"))
			parse_comments();
		else
			fout("\n+General Orders Enabled:");

		fout(" (");

		for (const SCP_string& order : e_list) {
			fout(" \"%s\"", order.c_str());
		}
		fout(" )\n");
	}

	SCP_vector<SCP_string> v_list = ai_lua_get_general_orders(false, true);

	if (save_format != MissionFormat::RETAIL && (v_list.size() > 0)) {
		if (optional_string_fred("+General Orders Valid:", "#Players"))
			parse_comments();
		else
			fout("\n+General Orders Valid:");

		fout(" (");

		for (const SCP_string& order : v_list) {
			fout(" \"%s\"", order.c_str());
		}
		fout(" )\n");
	}

	for (i = 0; i < Num_teams; i++) {
		required_string_fred("$Starting Shipname:");
		parse_comments();
		Assert(Player_start_shipnum >= 0);
		fout(" %s", Ships[Player_start_shipnum].ship_name);

		if (save_format != MissionFormat::RETAIL) {
			if (Team_data[i].do_not_validate) {
				required_string_fred("+Do Not Validate");
				parse_comments();
			}
		}

		required_string_fred("$Ship Choices:");
		parse_comments();
		fout(" (\n");

		int num_dogfight_weapons = 0;
		SCP_vector<SCP_string> dogfight_ships;

		for (j = 0; j < Team_data[i].num_ship_choices; j++) {
			// Check to see if a variable name should be written for the class rather than a number
			if (strlen(Team_data[i].ship_list_variables[j])) {
				var_idx = get_index_sexp_variable_name(Team_data[i].ship_list_variables[j]);
				Assert(var_idx > -1 && var_idx < MAX_SEXP_VARIABLES);
				wrote_fso_data = true;

				fout("\t@%s\t", Sexp_variables[var_idx].variable_name);
			} else {
				fout("\t\"%s\"\t", Ship_info[Team_data[i].ship_list[j]].name);
			}

			// Now check if we should write a variable or a number for the amount of ships available
			if (strlen(Team_data[i].ship_count_variables[j])) {
				var_idx = get_index_sexp_variable_name(Team_data[i].ship_count_variables[j]);
				Assert(var_idx > -1 && var_idx < MAX_SEXP_VARIABLES);
				wrote_fso_data = true;

				fout("@%s\n", Sexp_variables[var_idx].variable_name);
			} else {
				fout("%d\n", Team_data[i].ship_count[j]);
			}

			// Check the weapons pool for at least one dogfight weapon for this ship type
			if (IS_MISSION_MULTI_DOGFIGHT) {
				for (int wepCount = 0; wepCount < Team_data[i].num_weapon_choices; wepCount++) {
					if (Ship_info[Team_data[i].ship_list[j]].allowed_weapons[Team_data[i].weaponry_pool[wepCount]] & DOGFIGHT_WEAPON) {
						num_dogfight_weapons++;
						break;
					} else {
						dogfight_ships.push_back(Ship_info[Team_data[i].ship_list[j]].name);
					}
				}
			}
		}

		fout(")");

		// make sure we have at least one dogfight weapon for each ship type in a dogfight mission
		if (IS_MISSION_MULTI_DOGFIGHT && (num_dogfight_weapons != Team_data[i].num_ship_choices)) {
			for (int numErrors = 0; numErrors < (int)dogfight_ships.size(); numErrors++) {
				mprintf(("Warning: Ship %s has no dogfight weapons allowed\n", dogfight_ships[numErrors].c_str()));
			}
			_viewport->dialogProvider->showButtonDialog(DialogType::Warning,
				"No dogfight weapons",
				"Warning: This mission is a dogfight mission but no dogfight weapons are available for at least one "
				"ship in the loadout! In Debug mode a list of ships will be printed to the log.",
				{DialogButton::Ok});
		}

		if (optional_string_fred("+Weaponry Pool:", "$Starting Shipname:")) {
			parse_comments(2);
		} else {
			fout("\n\n+Weaponry Pool:");
		}

		fout(" (\n");
		generate_weaponry_usage_list_team(i, used_pool);

		for (j = 0; j < Team_data[i].num_weapon_choices; j++) {
			// first output the weapon name or a variable that sets it 
			if (strlen(Team_data[i].weaponry_pool_variable[j])) {
				var_idx = get_index_sexp_variable_name(Team_data[i].weaponry_pool_variable[j]);
				Assert(var_idx > -1 && var_idx < MAX_SEXP_VARIABLES);
				wrote_fso_data = true;

				fout("\t@%s\t", Sexp_variables[var_idx].variable_name);
			} else {
				fout("\t\"%s\"\t", Weapon_info[Team_data[i].weaponry_pool[j]].name);
			}

			// now output the amount of this weapon or a variable that sets it. If this weapon is in the used pool and isn't
			// set by a variable we should add the amount of weapons used by the wings to it and zero the entry so we know 
			// that we have dealt with it
			if (strlen(Team_data[i].weaponry_amount_variable[j])) {
				var_idx = get_index_sexp_variable_name(Team_data[i].weaponry_amount_variable[j]);
				Assert(var_idx > -1 && var_idx < MAX_SEXP_VARIABLES);
				wrote_fso_data = true;

				fout("@%s\n", Sexp_variables[var_idx].variable_name);
			} else {
				if (strlen(Team_data[i].weaponry_pool_variable[j])) {
					fout("%d\n", Team_data[i].weaponry_count[j]);
				} else {
					fout("%d\n", Team_data[i].weaponry_count[j] + used_pool[Team_data[i].weaponry_pool[j]]);
					used_pool[Team_data[i].weaponry_pool[j]] = 0;
				}
			}
		}

		// now we add anything left in the used pool as a static entry
		if (!Team_data[i].do_not_validate) {
			for (j = 0; j < static_cast<int>(Weapon_info.size()); j++) {
				if (used_pool[j] > 0) {
					fout("\t\"%s\"\t%d\n", Weapon_info[j].name, used_pool[j]);
				}
			}
		}

		fout(")");

		// sanity check
		if (save_format == MissionFormat::RETAIL && wrote_fso_data) {
			// this is such an unlikely (and hard-to-fix) case that a warning should be sufficient
			_viewport->dialogProvider->showButtonDialog(DialogType::Warning,
				"Incompatibility with retail mission format",
				"Warning: This mission contains variable-based team loadout information, but you are saving in the retail mission format. Retail FRED and FS2 will not be able to read this information.",
				{ DialogButton::Ok });
		}

		// Goober5000 - mjn.mixael's required weapon feature
		bool uses_required_weapon = false;
		for (j = 0; j < static_cast<int>(Weapon_info.size()); j++) {
			if (Team_data[i].weapon_required[j]) {
				uses_required_weapon = true;
				break;
			}
		}
		if (save_format != MissionFormat::RETAIL && uses_required_weapon) {
			if (optional_string_fred("+Required for mission:", "$Starting Shipname:")) {
				parse_comments(2);
			} else {
				fout("\n+Required for mission:");
			}

			fout(" (");
			for (j = 0; j < static_cast<int>(Weapon_info.size()); j++) {
				if (Team_data[i].weapon_required[j]) {
					fout(" \"%s\"", Weapon_info[j].name);
				}
			}
			fout(" )");
		}

		fso_comment_pop();
	}

	fso_comment_pop(true);

	return err;
}

int CFred_mission_save::save_plot_info()
{
	if (save_format == MissionFormat::RETAIL) {
		if (optional_string_fred("#Plot Info")) {
			parse_comments(2);

			// XSTR
			required_string_fred("$Tour:");
			parse_comments(2);
			fout_ext(" ", "Blah");

			required_string_fred("$Pre-Briefing Cutscene:");
			parse_comments();
			fout(" Blah");

			required_string_fred("$Pre-Mission Cutscene:");
			parse_comments();
			fout(" Blah");

			required_string_fred("$Next Mission Success:");
			parse_comments();
			fout(" Blah");

			required_string_fred("$Next Mission Partial:");
			parse_comments();
			fout(" Blah");

			required_string_fred("$Next Mission Failure:");
			parse_comments();
			fout(" Blah");
		} else {
			fout("\n\n#Plot Info\n\n");

			fout("$Tour: ");
			fout_ext(NULL, "Blah");
			fout("\n");
			fout("$Pre-Briefing Cutscene: Blah\n");
			fout("$Pre-Mission Cutscene: Blah\n");
			fout("$Next Mission Success: Blah\n");
			fout("$Next Mission Partial: Blah\n");
			fout("$Next Mission Failure: Blah\n");

			fout("\n");
		}
	}

	fso_comment_pop(true);

	return err;
}

int CFred_mission_save::save_reinforcements()
{
	int i, j, type;

	fred_parse_flag = 0;
	required_string_fred("#Reinforcements");
	parse_comments(2);
	fout("\t\t;! %d total\n", Num_reinforcements);

	for (i = 0; i < Num_reinforcements; i++) {
		required_string_either_fred("$Name:", "#Background bitmaps");
		required_string_fred("$Name:");
		parse_comments(i ? 2 : 1);
		fout(" %s", Reinforcements[i].name);

		type = TYPE_ATTACK_PROTECT;
		for (j = 0; j < MAX_SHIPS; j++) {
			if ((Ships[j].objnum != -1) && !stricmp(Ships[j].ship_name, Reinforcements[i].name)) {
				if (Ship_info[Ships[j].ship_info_index].flags[Ship::Info_Flags::Support]) {
					type = TYPE_REPAIR_REARM;
				}
				break;
			}
		}

		required_string_fred("$Type:");
		parse_comments();
		fout(" %s", Reinforcement_type_names[type]);

		required_string_fred("$Num times:");
		parse_comments();
		fout(" %d", Reinforcements[i].uses);

		if (optional_string_fred("+Arrival Delay:", "$Name:")) {
			parse_comments();
		} else {
			fout("\n+Arrival Delay:");
		}
		fout(" %d", Reinforcements[i].arrival_delay);

		if (optional_string_fred("+No Messages:", "$Name:")) {
			parse_comments();
		} else {
			fout("\n+No Messages:");
		}
		fout(" (");
		for (j = 0; j < MAX_REINFORCEMENT_MESSAGES; j++) {
			if (strlen(Reinforcements[i].no_messages[j])) {
				fout(" \"%s\"", Reinforcements[i].no_messages[j]);
			}
		}
		fout(" )");

		if (optional_string_fred("+Yes Messages:", "$Name:")) {
			parse_comments();
		} else {
			fout("\n+Yes Messages:");
		}
		fout(" (");
		for (j = 0; j < MAX_REINFORCEMENT_MESSAGES; j++) {
			if (strlen(Reinforcements[i].yes_messages[j])) {
				fout(" \"%s\"", Reinforcements[i].yes_messages[j]);
			}
		}
		fout(" )");

		fso_comment_pop();
	}

	fso_comment_pop(true);

	return err;
}

void CFred_mission_save::save_single_dock_instance(ship* shipp, dock_instance* dock_ptr)
{
	Assert(shipp && dock_ptr);
	Assert(dock_ptr->docked_objp->type == OBJ_SHIP || dock_ptr->docked_objp->type == OBJ_START);

	// get ships and objects
	object* objp = &Objects[shipp->objnum];
	object* other_objp = dock_ptr->docked_objp;
	ship* other_shipp = &Ships[other_objp->instance];

	// write other ship
	if (optional_string_fred("+Docked With:", "$Name:")) {
		parse_comments();
	} else {
		fout("\n+Docked With:");
	}
	fout(" %s", other_shipp->ship_name);


	// Goober5000 - hm, Volition seems to have reversed docker and dockee here

	// write docker (actually dockee) point
	required_string_fred("$Docker Point:", "$Name:");
	parse_comments();
	fout(" %s",
		 model_get_dock_name(Ship_info[other_shipp->ship_info_index].model_num,
							 dock_find_dockpoint_used_by_object(other_objp, objp)));

	// write dockee (actually docker) point
	required_string_fred("$Dockee Point:", "$Name:");
	parse_comments();
	fout(" %s",
		 model_get_dock_name(Ship_info[shipp->ship_info_index].model_num,
							 dock_find_dockpoint_used_by_object(objp, other_objp)));

	fso_comment_pop(true);
}

void CFred_mission_save::save_turret_info(ship_subsys* ptr, int ship)
{
	int i, z;
	ship_weapon* wp = &ptr->weapons;

	if (wp->ai_class != Ship_info[Ships[ship].ship_info_index].ai_class) {
		if (optional_string_fred("+AI Class:", "$Name:", "+Subsystem:")) {
			parse_comments();
		} else {
			fout("\n+AI Class:");
		}

		fout(" %s", Ai_class_names[wp->ai_class]);
	}

	z = 0;
	i = wp->num_primary_banks;
	while (i--) {
		if (wp->primary_bank_weapons[i] != ptr->system_info->primary_banks[i]) {
			z = 1;
		}
	}

	if (z) {
		if (optional_string_fred("+Primary Banks:", "$Name:", "+Subsystem:")) {
			parse_comments();
		} else {
			fout("\n+Primary Banks:");
		}

		fout(" ( ");
		for (i = 0; i < wp->num_primary_banks; i++) {
			if (wp->primary_bank_weapons[i] != -1) { // Just in case someone has set a weapon bank to empty
				fout("\"%s\" ", Weapon_info[wp->primary_bank_weapons[i]].name);
			} else {
				fout("\"\" ");
			}
		}
		fout(")");
	}

	z = 0;
	i = wp->num_secondary_banks;
	while (i--) {
		if (wp->secondary_bank_weapons[i] != ptr->system_info->secondary_banks[i]) {
			z = 1;
		}
	}

	if (z) {
		if (optional_string_fred("+Secondary Banks:", "$Name:", "+Subsystem:")) {
			parse_comments();
		} else {
			fout("\n+Secondary Banks:");
		}

		fout(" ( ");
		for (i = 0; i < wp->num_secondary_banks; i++) {
			if (wp->secondary_bank_weapons[i] != -1) {
				fout("\"%s\" ", Weapon_info[wp->secondary_bank_weapons[i]].name);
			} else {
				fout("\"\" ");
			}
		}
		fout(")");
	}

	z = 0;
	i = wp->num_secondary_banks;
	while (i--) {
		if (wp->secondary_bank_ammo[i] != 100) {
			z = 1;
		}
	}

	if (z) {
		if (optional_string_fred("+Sbank Ammo:", "$Name:", "+Subsystem:")) {
			parse_comments();
		} else {
			fout("\n+Sbank Ammo:");
		}

		fout(" ( ");
		for (i = 0; i < wp->num_secondary_banks; i++) {
			fout("%d ", wp->secondary_bank_ammo[i]);
		}

		fout(")");
	}

	fso_comment_pop(true);
}

int CFred_mission_save::save_variables()
{
	char* type;
	char number[] = "number";
	char string[] = "string";
	char block[] = "block";
	int i;
	int num_block_vars = 0;

	// sort sexp_variables
	sexp_variable_sort();

	// get count
	int num_variables = sexp_variable_count();

	if (save_format == MissionFormat::RETAIL) {
		generate_special_explosion_block_variables();
		num_block_vars = num_block_variables();
	}
	int total_variables = num_variables + num_block_vars;

	if (total_variables > 0) {

		// write 'em out
		required_string_fred("#Sexp_variables");
		parse_comments(2);

		required_string_fred("$Variables:");
		parse_comments(2);

		fout("\n(");
		//		parse_comments();

		for (i = 0; i < num_variables; i++) {
			if (Sexp_variables[i].type & SEXP_VARIABLE_NUMBER) {
				type = number;
			} else {
				type = string;
			}
			// index "var name" "default" "type"
			fout("\n\t\t%d\t\t\"%s\"\t\t\"%s\"\t\t\"%s\"",
				 i,
				 Sexp_variables[i].variable_name,
				 Sexp_variables[i].text,
				 type);

			// persistent and network variables
			if (save_format != MissionFormat::RETAIL) {
				// Network variable - Karajorma
				if (Sexp_variables[i].type & SEXP_VARIABLE_NETWORK) {
					fout("\t\t\"%s\"", "network-variable");
				}

				// player-persistent - Goober5000
				if (Sexp_variables[i].type & SEXP_VARIABLE_SAVE_ON_MISSION_CLOSE) {
					fout("\t\t\"%s\"", "save-on-mission-close");
					// campaign-persistent - Goober5000
				} else if (Sexp_variables[i].type & SEXP_VARIABLE_SAVE_ON_MISSION_PROGRESS) {
					fout("\t\t\"%s\"", "save-on-mission-progress");
				}
			}

			//			parse_comments();
		}

		for (i = MAX_SEXP_VARIABLES - num_block_vars; i < MAX_SEXP_VARIABLES; i++) {
			type = block;
			fout("\n\t\t%d\t\t\"%s\"\t\t\"%s\"\t\t\"%s\"",
				 i,
				 Block_variables[i].variable_name,
				 Block_variables[i].text,
				 type);
		}

		fout("\n)");

		fso_comment_pop();
	}

	fso_comment_pop(true);

	return err;
}

int CFred_mission_save::save_containers()
{
	if (save_format == MissionFormat::RETAIL) {
		return 0;
	}

	const auto &containers = get_all_sexp_containers();

	if (containers.empty()) {
		fso_comment_pop(true);
		return 0;
	}

	required_string_fred("#Sexp_containers");
	parse_comments(2);

	bool list_found = false;
	bool map_found = false;

	// What types of container do we have?
	for (const auto &container : containers) {
		if (container.is_list()) {
			list_found = true;
		} else if (container.is_map()) {
			map_found = true;
		}
		if (list_found && map_found) {
			// no point in continuing to check
			break;
		}
	}

	if (list_found) {
		required_string_fred("$Lists");
		parse_comments(2);

		for (const auto &container : containers) {
			if (container.is_list()) {
				fout("\n$Name: %s", container.container_name.c_str());
				if (any(container.type & ContainerType::STRING_DATA)) {
					fout("\n$Data Type: String");
				} else if (any(container.type & ContainerType::NUMBER_DATA)) {
					fout("\n$Data Type: Number");
				}

				if (any(container.type & ContainerType::STRICTLY_TYPED_DATA)) {
					fout("\n+Strictly Typed Data");
				}

				fout("\n$Data: ( ");
				for (const auto &list_entry : container.list_data) {
					fout("\"%s\" ", list_entry.c_str());
				}

				fout(")\n");

				save_container_options(container);
			}
		}

		required_string_fred("$End Lists");
		parse_comments(1);
	}

	if (map_found) {
		required_string_fred("$Maps");
		parse_comments(2);

		for (const auto &container : containers) {
			if (container.is_map()) {
				fout("\n$Name: %s", container.container_name.c_str());
				if (any(container.type & ContainerType::STRING_DATA)) {
					fout("\n$Data Type: String");
				} else if (any(container.type & ContainerType::NUMBER_DATA)) {
					fout("\n$Data Type: Number");
				}

				if (any(container.type & ContainerType::NUMBER_KEYS)) {
					fout("\n$Key Type: Number");
				} else {
					fout("\n$Key Type: String");
				}

				if (any(container.type & ContainerType::STRICTLY_TYPED_KEYS)) {
					fout("\n+Strictly Typed Keys");
				}

				if (any(container.type & ContainerType::STRICTLY_TYPED_DATA)) {
					fout("\n+Strictly Typed Data");
				}

				SCP_vector<std::pair<SCP_string, SCP_string>> sorted_data(container.map_data.begin(), container.map_data.end());
				std::stable_sort(sorted_data.begin(), sorted_data.end(),
					[](const std::pair<SCP_string, SCP_string> &a, const std::pair<SCP_string, SCP_string> &b)
					{ return a.first < b.first; });

				fout("\n$Data: ( ");
				for (const auto &map_entry : sorted_data) {
					fout("\"%s\" \"%s\" ", map_entry.first.c_str(), map_entry.second.c_str());
				}

				fout(")\n");

				save_container_options(container);
			}
		}

		required_string_fred("$End Maps");
		parse_comments(1);
	}

	return err;
}

void CFred_mission_save::save_container_options(const sexp_container &container)
{
	if (any(container.type & ContainerType::NETWORK)) {
		fout("+Network Container\n");
	}

	if (container.is_eternal()) {
		fout("+Eternal\n");
	}

	if (any(container.type & ContainerType::SAVE_ON_MISSION_CLOSE)) {
		fout("+Save On Mission Close\n");
	} else if (any(container.type & ContainerType::SAVE_ON_MISSION_PROGRESS)) {
		fout("+Save On Mission Progress\n");
	}

	fout("\n");
}

int CFred_mission_save::save_vector(const vec3d& v)
{
	fout(" %f, %f, %f", v.xyz.x, v.xyz.y, v.xyz.z);
	return 0;
}

int CFred_mission_save::save_waypoints()
{
	//object *ptr;

	fred_parse_flag = 0;
	required_string_fred("#Waypoints");
	parse_comments(2);
	fout("\t\t;! %d lists total\n", Waypoint_lists.size());

	SCP_list<CJumpNode>::iterator jnp;
	for (jnp = Jump_nodes.begin(); jnp != Jump_nodes.end(); ++jnp) {
		required_string_fred("$Jump Node:", "$Jump Node Name:");
		parse_comments(2);
		save_vector(jnp->GetSCPObject()->pos);

		required_string_fred("$Jump Node Name:", "$Jump Node:");
		parse_comments();
		fout(" %s", jnp->GetName());

		if (save_format != MissionFormat::RETAIL) {
			
			// The display name is only written if there was one at the start to avoid introducing inconsistencies
			if (_viewport->Always_save_display_names || jnp->HasDisplayName()) {
				char truncated_name[NAME_LENGTH];
				strcpy_s(truncated_name, jnp->GetName());
				end_string_at_first_hash_symbol(truncated_name);

				// Also, the display name is not written if it's just the truncation of the name at the hash
				if (_viewport->Always_save_display_names || strcmp(jnp->GetDisplayName(), truncated_name) != 0) {
					if (optional_string_fred("+Display Name:", "$Jump Node:")) {
						parse_comments();
					} else {
						fout("\n+Display Name:");
					}

					fout_ext("", "%s", jnp->GetDisplayName());
				}
			}
			
			if (jnp->IsSpecialModel()) {
				if (optional_string_fred("+Model File:", "$Jump Node:")) {
					parse_comments();
				} else {
					fout("\n+Model File:");
				}

				int model = jnp->GetModelNumber();
				polymodel* pm = model_get(model);
				fout(" %s", pm->filename);
			}

			if (jnp->IsColored()) {
				if (optional_string_fred("+Alphacolor:", "$Jump Node:")) {
					parse_comments();
				} else {
					fout("\n+Alphacolor:");
				}

				const auto &jn_color = jnp->GetColor();
				fout(" %u %u %u %u", jn_color.red, jn_color.green, jn_color.blue, jn_color.alpha);
			}

			int hidden_is_there = optional_string_fred("+Hidden:", "$Jump Node:");
			if (hidden_is_there) {
				parse_comments();
			}

			if (hidden_is_there || jnp->IsHidden()) {
				if (!hidden_is_there) {
					fout("\n+Hidden:");
				}

				if (jnp->IsHidden()) {
					fout(" %s", "true");
				}
				else {
					fout(" %s", "false");
				}
			}
		}

		fso_comment_pop();
	}

	bool first_wpt_list = true;
	for (const auto &ii: Waypoint_lists) {
		required_string_either_fred("$Name:", "#Messages");
		required_string_fred("$Name:");
		parse_comments(first_wpt_list ? 1 : 2);
		fout(" %s", ii.get_name());

		required_string_fred("$List:");
		parse_comments();
		fout(" (\t\t;! %d points in list\n", ii.get_waypoints().size());

		save_waypoint_list(&ii);
		fout(")");

		fso_comment_pop();
	}

	fso_comment_pop(true);

	return err;
}

int CFred_mission_save::save_waypoint_list(const waypoint_list* wp_list)
{
	Assert(wp_list != NULL);

	for (const auto &ii: wp_list->get_waypoints()) {
		auto pos = ii.get_pos();
		fout("\t( %f, %f, %f )\n", pos->xyz.x, pos->xyz.y, pos->xyz.z);
	}

	return 0;
}

int CFred_mission_save::save_wings()
{
	SCP_string sexp_out;
	int i, j, z, count = 0;

	fred_parse_flag = 0;
	required_string_fred("#Wings");
	parse_comments(2);
	fout("\t\t;! %d total", Num_wings);

	for (i = 0; i < MAX_WINGS; i++) {
		if (!Wings[i].wave_count) {
			continue;
		}

		count++;
		required_string_either_fred("$Name:", "#Events");
		required_string_fred("$Name:");
		parse_comments(2);
		fout(" %s", Wings[i].name);

		// squad logo - Goober5000
		if (save_format != MissionFormat::RETAIL) {
			if (strlen(Wings[i].wing_squad_filename) > 0) //-V805
			{
				if (optional_string_fred("+Squad Logo:", "$Name:")) {
					parse_comments();
				} else {
					fout("\n+Squad Logo:");
				}

				fout(" %s", Wings[i].wing_squad_filename);
			}
		}

		required_string_fred("$Waves:");
		parse_comments();
		fout(" %d", Wings[i].num_waves);

		required_string_fred("$Wave Threshold:");
		parse_comments();
		fout(" %d", Wings[i].threshold);

		required_string_fred("$Special Ship:");
		parse_comments();
		fout(" %d\t\t;! %s", Wings[i].special_ship, Ships[Wings[i].ship_index[Wings[i].special_ship]].ship_name);

		if (save_format != MissionFormat::RETAIL) {
			if (Wings[i].formation >= 0 && Wings[i].formation < (int)Wing_formations.size())
			{
				if (optional_string_fred("+Formation:", "$Name:")) {
					parse_comments();
				}
				else {
					fout("\n+Formation:");
				}

				fout(" %s", Wing_formations[Wings[i].formation].name);
			}
			if (!fl_equal(Wings[i].formation_scale, 1.0f, 0.001f))
			{
				if (optional_string_fred("+Formation Scale:", "$Name:")) {
					parse_comments();
				}
				else {
					fout("\n+Formation Scale:");
				}
				fout(" %f", Wings[i].formation_scale);
			}
		}

		required_string_fred("$Arrival Location:");
		parse_comments();
		fout(" %s", Arrival_location_names[static_cast<int>(Wings[i].arrival_location)]);

		if (Wings[i].arrival_location != ArrivalLocation::AT_LOCATION) {
			if (optional_string_fred("+Arrival Distance:", "$Name:")) {
				parse_comments();
			} else {
				fout("\n+Arrival Distance:");
			}

			fout(" %d", Wings[i].arrival_distance);
			if (optional_string_fred("$Arrival Anchor:", "$Name:")) {
				parse_comments();
			} else {
				fout("\n$Arrival Anchor:");
			}

			z = Wings[i].arrival_anchor;
			if (z & SPECIAL_ARRIVAL_ANCHOR_FLAG) {
				// get name
				char tmp[NAME_LENGTH + 15];
				stuff_special_arrival_anchor_name(tmp, z, save_format == MissionFormat::RETAIL);

				// save it
				fout(" %s", tmp);
			} else if (z >= 0) {
				fout(" %s", Ships[z].ship_name);
			} else {
				fout(" <error>");
			}
		}

		// Goober5000
		if (save_format != MissionFormat::RETAIL) {
			if ((Wings[i].arrival_location == ArrivalLocation::FROM_DOCK_BAY) && (Wings[i].arrival_path_mask > 0)) {
				int anchor_shipnum;
				polymodel* pm;

				anchor_shipnum = Wings[i].arrival_anchor;
				Assert(anchor_shipnum >= 0 && anchor_shipnum < MAX_SHIPS);

				fout("\n+Arrival Paths: ( ");

				pm = model_get(Ship_info[Ships[anchor_shipnum].ship_info_index].model_num);
				for (auto n = 0; n < pm->ship_bay->num_paths; n++) {
					if (Wings[i].arrival_path_mask & (1 << n)) {
						fout("\"%s\" ", pm->paths[pm->ship_bay->path_indexes[n]].name);
					}
				}

				fout(")");
			}
		}

		if (Wings[i].arrival_delay) {
			if (optional_string_fred("+Arrival delay:", "$Name:")) {
				parse_comments();
			} else {
				fout("\n+Arrival delay:");
			}

			fout(" %d", Wings[i].arrival_delay);
		}

		required_string_fred("$Arrival Cue:");
		parse_comments();
		convert_sexp_to_string(sexp_out, Wings[i].arrival_cue, SEXP_SAVE_MODE);
		fout(" %s", sexp_out.c_str());

		required_string_fred("$Departure Location:");
		parse_comments();
		fout(" %s", Departure_location_names[static_cast<int>(Wings[i].departure_location)]);

		if (Wings[i].departure_location != DepartureLocation::AT_LOCATION) {
			required_string_fred("$Departure Anchor:");
			parse_comments();

			if (Wings[i].departure_anchor >= 0) {
				fout(" %s", Ships[Wings[i].departure_anchor].ship_name);
			} else {
				fout(" <error>");
			}
		}

		// Goober5000
		if (save_format != MissionFormat::RETAIL) {
			if ((Wings[i].departure_location == DepartureLocation::TO_DOCK_BAY) && (Wings[i].departure_path_mask > 0)) {
				int anchor_shipnum;
				polymodel* pm;

				anchor_shipnum = Wings[i].departure_anchor;
				Assert(anchor_shipnum >= 0 && anchor_shipnum < MAX_SHIPS);

				fout("\n+Departure Paths: ( ");

				pm = model_get(Ship_info[Ships[anchor_shipnum].ship_info_index].model_num);
				for (auto n = 0; n < pm->ship_bay->num_paths; n++) {
					if (Wings[i].departure_path_mask & (1 << n)) {
						fout("\"%s\" ", pm->paths[pm->ship_bay->path_indexes[n]].name);
					}
				}

				fout(")");
			}
		}

		if (Wings[i].departure_delay) {
			if (optional_string_fred("+Departure delay:", "$Name:")) {
				parse_comments();
			} else {
				fout("\n+Departure delay:");
			}

			fout(" %d", Wings[i].departure_delay);
		}

		required_string_fred("$Departure Cue:");
		parse_comments();
		convert_sexp_to_string(sexp_out, Wings[i].departure_cue, SEXP_SAVE_MODE);
		fout(" %s", sexp_out.c_str());

		required_string_fred("$Ships:");
		parse_comments();
		fout(" (\t\t;! %d total\n", Wings[i].wave_count);

		for (j = 0; j < Wings[i].wave_count; j++) {
			//			if (Objects[Ships[ship].objnum].type == OBJ_START)
			//				fout("\t\"Player 1\"\n");
			//			else
			fout("\t\"%s\"\n", Ships[Wings[i].ship_index[j]].ship_name);
		}

		fout(")");

		save_ai_goals(Wings[i].ai_goals, -1);

		if (Wings[i].hotkey != -1) {
			if (optional_string_fred("+Hotkey:", "$Name:")) {
				parse_comments();
			} else {
				fout("\n+Hotkey:");
			}

			fout(" %d", Wings[i].hotkey);
		}

		if (optional_string_fred("+Flags:", "$Name:")) {
			parse_comments();
			fout(" (");
		} else {
			fout("\n+Flags: (");
		}

		if (Wings[i].flags[Ship::Wing_Flags::Ignore_count]) {
			fout(" \"ignore-count\"");
		}
		if (Wings[i].flags[Ship::Wing_Flags::Reinforcement]) {
			fout(" \"reinforcement\"");
		}
		if (Wings[i].flags[Ship::Wing_Flags::No_arrival_music]) {
			fout(" \"no-arrival-music\"");
		}
		if (Wings[i].flags[Ship::Wing_Flags::No_arrival_message]) {
			fout(" \"no-arrival-message\"");
		}
		if (Wings[i].flags[Ship::Wing_Flags::No_first_wave_message]) {
			fout(" \"no-first-wave-message\"");
		}
		if (Wings[i].flags[Ship::Wing_Flags::No_arrival_warp]) {
			fout(" \"no-arrival-warp\"");
		}
		if (Wings[i].flags[Ship::Wing_Flags::No_departure_warp]) {
			fout(" \"no-departure-warp\"");
		}
		if (Wings[i].flags[Ship::Wing_Flags::No_dynamic]) {
			fout(" \"no-dynamic\"");
		}
		if (save_format != MissionFormat::RETAIL) {
			if (Wings[i].flags[Ship::Wing_Flags::Nav_carry]) {
				fout(" \"nav-carry-status\"");
			}
			if (Wings[i].flags[Ship::Wing_Flags::Same_arrival_warp_when_docked]) {
				fout(" \"same-arrival-warp-when-docked\"");
			}
			if (Wings[i].flags[Ship::Wing_Flags::Same_departure_warp_when_docked]) {
				fout(" \"same-departure-warp-when-docked\"");
			}
		}

		fout(" )");

		if (Wings[i].wave_delay_min) {
			if (optional_string_fred("+Wave Delay Min:", "$Name:")) {
				parse_comments();
			} else {
				fout("\n+Wave Delay Min:");
			}

			fout(" %d", Wings[i].wave_delay_min);
		}

		if (Wings[i].wave_delay_max) {
			if (optional_string_fred("+Wave Delay Max:", "$Name:")) {
				parse_comments();
			} else {
				fout("\n+Wave Delay Max:");
			}

			fout(" %d", Wings[i].wave_delay_max);
		}

		fso_comment_pop();
	}

	fso_comment_pop(true);

	Assert(count == Num_wings);
	return err;
}

}
}

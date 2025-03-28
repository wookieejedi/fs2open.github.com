//
//

#include <parse/parselo.h>
#include <ship/ship.h>
#include <globalincs/linklist.h>
#include <iff_defs/iff_defs.h>
#include "FormWingDialogModel.h"

namespace fso {
namespace fred {
namespace dialogs {


FormWingDialogModel::FormWingDialogModel(QObject* parent, EditorViewport* viewport) :
	AbstractDialogModel(parent, viewport) {
}
bool FormWingDialogModel::apply() {
	drop_white_space(_name);
	if (_name.empty()) {
		_viewport->dialogProvider->showButtonDialog(DialogType::Error,
													"Error",
													"You must give a name before you can continue.",
													{ DialogButton::Ok });
		return false;
	}

	for (auto i = 0; i < MAX_WINGS; i++) {
		if (!stricmp(Wings[i].name, _name.c_str()) && Wings[i].wave_count) {
			SCP_string msg;
			sprintf(msg, "The name \"%s\" is already being used by another wing", _name.c_str());

			_viewport->dialogProvider->showButtonDialog(DialogType::Error,
														"Error",
														msg,
														{ DialogButton::Ok });
			return false;
		}
	}

	auto len = _name.length();
	for (auto ptr : list_range(&obj_used_list)) {
		if ((ptr->type != OBJ_SHIP) && (ptr->type != OBJ_START))
			continue;
		int i = ptr->instance;

		// if this ship is actually going to be in the wing, and if it *can* be in a wing
		// (i.e. it will not be taken out later), then skip the name check
		if (ptr->flags[Object::Object_Flags::Marked]) {
			int ship_type = ship_query_general_type(i);
			if (ship_type >= 0 && Ship_types[ship_type].flags[Ship::Type_Info_Flags::AI_can_form_wing])
				continue;
		}

		// see if this ship name matches what a ship in the wing would be
		if (!strnicmp(_name.c_str(), Ships[i].ship_name, len)) {
			auto namep = Ships[i].ship_name + len;
			if (*namep == ' ') {
				namep++;
				while (*namep) {
					if (!isdigit(*namep))
						break;
					namep++;
				}
			}

			if (!*namep) {
				_viewport->dialogProvider->showButtonDialog(DialogType::Error,
					"Error",
					"This wing name is already being used by a ship",
					{ DialogButton::Ok });
				return false;
			}
		}
	}

	// We don't need to check teams.  "Unknown" is a valid name and also an IFF.

	for (auto i = 0; i < (int) Ai_tp_list.size(); i++) {
		if (!stricmp(_name.c_str(), Ai_tp_list[i].name)) {
			SCP_string msg;
			sprintf(msg, "The name \"%s\" is already being used by a target priority group", _name.c_str());

			_viewport->dialogProvider->showButtonDialog(DialogType::Error,
														"Error",
														msg,
														{ DialogButton::Ok });
			return false;
		}
	}

	if (find_matching_waypoint_list(_name.c_str()) != NULL) {
		_viewport->dialogProvider->showButtonDialog(DialogType::Error,
													"Error",
													"This wing name is already being used by a waypoint path",
													{ DialogButton::Ok });
		return false;
	}

	if (_name[0] == '<') {
		_viewport->dialogProvider->showButtonDialog(DialogType::Error,
													"Error",
													"Wing names not allowed to begin with <",
													{ DialogButton::Ok });
		return false;
	}

	return true;
}
void FormWingDialogModel::reject() {

}
const SCP_string& FormWingDialogModel::getName() const {
	return _name;
}
void FormWingDialogModel::setName(const SCP_string& name) {
	modify(_name, name);
}

}
}
}

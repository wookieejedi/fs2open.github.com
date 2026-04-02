//

#include "AboutDialogModel.h"

#include <project.h>
#include <graphics/2d.h>

namespace fso::fred::dialogs {

AboutDialogModel::AboutDialogModel(QObject* parent, EditorViewport* viewport)
	: AbstractDialogModel(parent, viewport)
{
}

bool AboutDialogModel::apply()
{
	return true;
}

void AboutDialogModel::reject()
{
}

SCP_string AboutDialogModel::getVersionString() const
{
	SCP_string graphicsAPI;
	switch (gr_screen.mode) {
	case GR_OPENGL:
		graphicsAPI = "OpenGL";
		break;
	case GR_VULKAN:
		graphicsAPI = "Vulkan";
		break;
	}

	SCP_string result = "qtFRED - FreeSpace Editor, Version ";
	result += FS_VERSION_FULL;
	if (!graphicsAPI.empty()) {
		result += " ";
		result += graphicsAPI;
	}
	return result;
}

} // namespace fso::fred::dialogs

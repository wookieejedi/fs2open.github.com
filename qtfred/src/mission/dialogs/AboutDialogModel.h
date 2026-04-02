#pragma once

#include "AbstractDialogModel.h"

#include <globalincs/pstypes.h>

namespace fso::fred::dialogs {

class AboutDialogModel : public AbstractDialogModel {
	Q_OBJECT

public:
	AboutDialogModel(QObject* parent, EditorViewport* viewport);

	bool apply() override;
	void reject() override;

	SCP_string getVersionString() const;
};

} // namespace fso::fred::dialogs

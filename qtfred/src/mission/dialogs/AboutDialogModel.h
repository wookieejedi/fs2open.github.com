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
	SCP_string getCopyrightString() const;
	SCP_vector<SCP_string> getQtFREDCredits() const;
	SCP_vector<SCP_string> getGraphicsCredits() const;
	SCP_string getSCPCreditsText() const;
	SCP_string getQuoteString() const;
};

} // namespace fso::fred::dialogs

// lxhelp.h - PsiLinux's Help dialog (lxhelp.cpp)
#ifndef LXHELP_H
#define LXHELP_H
#include <eikdialg.h>

class CLxHelpDialog : public CEikDialog
	{
public:
	CLxHelpDialog(TInt aTopic) : iTopic(aTopic) {}
private:
	void SetSizeAndPositionL(const TSize& aSize);
	void PreLayoutDynInitL();
	void PostLayoutDynInitL();
	void HandleControlStateChangeL(TInt aControlId);
	TBool OkToExitL(TInt aButtonId);
	void ShowTopicL(TInt aTopic);
	TInt iTopic;
	};

#endif

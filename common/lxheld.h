// lxheld.h - (0.47, RN-18) the memory the window's process holds: every
// named chunk of the process ("<process>::$HEAP", "::$DAT"), committed
// bytes (lxshared.h app_held). The runner counts its own Linux chunks,
// which are local and nameless, itself (bk_epoc.cpp RunnerHeld). The
// status line and /proc/psilinux/mem's "in all" show the two together.
#ifndef LXHELD_H
#define LXHELD_H
#include <e32std.h>

inline TUint LxHeldBytes()
	{
	TFullName pattern(RProcess().FullName());
	if (pattern.Length() > pattern.MaxLength() - 3)
		return 0;
	pattern.Append(_L("::*"));
	TFindChunk fc(pattern);
	TFullName name;
	TUint total = 0;
	while (fc.Next(name) == KErrNone)
		{
		RChunk c;
		if (c.Open(fc) == KErrNone)
			{
			total += (TUint)c.Size();
			c.Close();
			}
		}
	return total;
	}

#endif

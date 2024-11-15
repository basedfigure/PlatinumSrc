#ifndef PSRC_VERSION_H
#define PSRC_VERSION_H

#include "platform.h"

#define PSRC_BUILD 2024111500
#define PSRC_COMPATBUILD 2024111500

extern char* titlestr;
extern char verstr[];
extern char platstr[];
#if PLATFORM == PLAT_NXDK
extern char titleidstr[9];
#endif

void makeVerStrs(void);

#endif

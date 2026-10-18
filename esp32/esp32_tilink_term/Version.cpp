// The commit and time of the build, for the Cardputer's build page. On their
// own here because version.h changes at every build: only this file is then
// compiled again, not Display.cpp with all of the screen's library.

// The Makefile writes it in the build folder; a build from elsewhere has none.
#if __has_include("version.h")
#include "version.h"
#else
#define VERSION "unknown"
#define BUILT __DATE__ " " __TIME__
#endif

extern const char BUILD_VERSION[] = VERSION;
extern const char BUILD_TIME[] = BUILT;

// Hello world for the TI Voyage 200, built with TIGCC / GCC4TI.

#define USE_V200
#define MIN_AMS 100
#define SAVE_SCREEN

#include <tigcclib.h>

void _main(void)
{
  ClrScr();
  FontSetSys(F_8x10);
  DrawStr(0, 0, "Hello, world!", A_NORMAL);
  FontSetSys(F_6x8);
  DrawStr(0, 16, "Press any key...", A_NORMAL);
  ngetchx();
}

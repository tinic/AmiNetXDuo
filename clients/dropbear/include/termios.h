/* clients/dropbear/include, see clients/dropbear/build.sh.  newlib has the
 * termios struct but declares none of the functions; the definitions are in
 * clients/dropbear/amiga_dropbear.c.  SPDX-License-Identifier: MIT */
#ifndef AMIGA_TERMIOS_SHIM_H
#define AMIGA_TERMIOS_SHIM_H

#include_next <termios.h>

int tcgetattr(int fd, struct termios *t);
int tcsetattr(int fd, int actions, const struct termios *t);

#ifndef TCSANOW
#define TCSANOW   0
#define TCSADRAIN 1
#define TCSAFLUSH 2
#endif

#endif

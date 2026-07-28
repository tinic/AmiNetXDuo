/* clients/dropbear/include -- see clients/dropbear/build.sh.
 *
 * newlib on this toolchain has <sys/termios.h> with `struct termios` and the
 * flag constants, and declares none of the functions.  AmigaOS has no termios
 * at all: a Shell's input is a DOS handle and raw mode is SetMode() on a
 * console.  So the declarations are here and the definitions are in
 * clients/dropbear/amiga_dropbear.c, where they fail with ENOTTY rather than
 * pretend.  Hence `dbclient -T` (no pty) is the supported shape.
 *
 * SPDX-License-Identifier: MIT */
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

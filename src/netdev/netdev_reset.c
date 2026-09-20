/*
 * anxnet.device: stopping a bus master before the machine reboots.
 *
 * A warm reboot resets the 68k and nothing else.  A chip that masters the
 * bus into RAM -- the GENET -- keeps receiving into the ring it was given,
 * which the next boot is already using for something else, until the driver
 * loads again twenty seconds later: silent corruption of whatever landed in
 * those pages.  So both ways a running machine reboots are hooked, and both
 * hooks stop every such unit's DMA before the reset goes ahead:
 *
 *   - ColdReboot(), which is what a program (this stack's own SyncReboot, the
 *     Emu68 reset guard's ancestor) calls: patched with SetFunction(), the
 *     way a reboot hook has to be on an exec without AddResetCallback().
 *   - the keyboard, Ctrl-Amiga-Amiga: keyboard.device's reset handler chain,
 *     which is the documented one.
 *
 * The patch is installed the first time such a unit goes online and taken
 * out at expunge -- when it can be.  SetFunction() answers with the vector
 * it replaced; if that is not ours somebody patched after us, and the
 * device then stays resident with its hook in place rather than leaving a
 * jump into freed memory in the chain.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netdev_internal.h"

#include <exec/execbase.h>
#include <exec/interrupts.h>
#include <exec/io.h>
#include <devices/keyboard.h>
#include <proto/exec.h>
/* BeginIO(): a macro over the device's own vector, no amiga.lib to link. */
#include <inline/alib.h>

extern struct ExecBase *SysBase;

#define LVO_ColdReboot  (-726)

static NetdevDevice    *rg_dev;         /* the one device this guards      */
static APTR             rg_old_cold;    /* ColdReboot before the patch     */
static struct Interrupt rg_kbd;         /* the keyboard.device handler     */
static struct IOStdReq *rg_kbd_io;      /* its request, kept open          */
static struct MsgPort  *rg_kbd_port;
static UBYTE            rg_installed;

/* Every bus-master unit that is running, stopped.  From any context. */
static VOID rg_stop_all(VOID)
{
    NetdevDevice *d = rg_dev;
    UWORD         i;

    if (d == NULL)
        return;

    for (i = 0; i < d->nd_UnitCount; i++)
    {
        NetdevUnit *u = &d->nd_Units[i];

        if (u->nu_Nic.bus_master && u->nu_Nic.running)
            u->nu_Nic.ops->stop(&u->nu_Nic);
    }
}

/*
 * The ColdReboot() replacement.  Registers are as exec's own: a6 = SysBase,
 * nothing else in, and it does not return.  The stop takes microseconds.
 */
static VOID rg_cold_reboot(VOID)
{
    register APTR _a6 __asm("a6");

    Disable();
    rg_stop_all();
    Enable();

    _a6 = (APTR)SysBase;
    __asm__ __volatile__ ("movea.l %0,a0\n\tjmp a0@"
                          : : "r" (rg_old_cold), "r" (_a6) : "a0", "memory");
}

/*
 * The keyboard reset handler: a1 = is_Data.  It stops the units and then
 * tells keyboard.device it is done, on the request set up at install; the
 * device holds the reset until every handler has said so or ten seconds
 * pass, and a handler that never answers costs the user those ten seconds.
 */
static ULONG rg_kbd_handler(register APTR data __asm("a1"))
{
    (VOID)data;

    rg_stop_all();

    if (rg_kbd_io != NULL)
    {
        rg_kbd_io->io_Command = KBD_RESETHANDLERDONE;
        rg_kbd_io->io_Data    = &rg_kbd;
        rg_kbd_io->io_Length  = sizeof(rg_kbd);
        BeginIO((struct IORequest *)rg_kbd_io);
    }
    return 0;
}

VOID netdev_reset_guard(NetdevDevice *dev)
{
    if (rg_installed)
        return;
    rg_dev = dev;

    rg_old_cold = SetFunction((struct Library *)SysBase, LVO_ColdReboot,
                              (APTR)rg_cold_reboot);

    rg_kbd_port = CreateMsgPort();
    if (rg_kbd_port != NULL)
    {
        rg_kbd_io = (struct IOStdReq *)
            CreateIORequest(rg_kbd_port, sizeof(struct IOStdReq));
        if (rg_kbd_io != NULL &&
            OpenDevice((CONST_STRPTR)"keyboard.device", 0,
                       (struct IORequest *)rg_kbd_io, 0) == 0)
        {
            rg_kbd.is_Node.ln_Type = NT_INTERRUPT;
            rg_kbd.is_Node.ln_Pri  = 32;    /* early: before the disks flush */
            rg_kbd.is_Node.ln_Name = (char *)"anxnet.device";
            rg_kbd.is_Data         = dev;
            rg_kbd.is_Code         = (VOID (*)())rg_kbd_handler;

            rg_kbd_io->io_Command = KBD_ADDRESETHANDLER;
            rg_kbd_io->io_Data    = &rg_kbd;
            rg_kbd_io->io_Length  = sizeof(rg_kbd);
            DoIO((struct IORequest *)rg_kbd_io);
        }
        else
        {
            if (rg_kbd_io != NULL)
                DeleteIORequest((struct IORequest *)rg_kbd_io);
            DeleteMsgPort(rg_kbd_port);
            rg_kbd_io   = NULL;
            rg_kbd_port = NULL;
        }
    }

    rg_installed = 1;
}

/* TRUE when the hooks are out and the device may go. */
BOOL netdev_reset_guard_remove(VOID)
{
    APTR now;

    if (!rg_installed)
        return TRUE;

    now = SetFunction((struct Library *)SysBase, LVO_ColdReboot, rg_old_cold);
    if (now != (APTR)rg_cold_reboot)
    {
        /* Someone patched over us.  Put ours back and stay. */
        (VOID)SetFunction((struct Library *)SysBase, LVO_ColdReboot, now);
        return FALSE;
    }

    if (rg_kbd_io != NULL)
    {
        rg_kbd_io->io_Command = KBD_REMRESETHANDLER;
        rg_kbd_io->io_Data    = &rg_kbd;
        rg_kbd_io->io_Length  = sizeof(rg_kbd);
        DoIO((struct IORequest *)rg_kbd_io);
        CloseDevice((struct IORequest *)rg_kbd_io);
        DeleteIORequest((struct IORequest *)rg_kbd_io);
        DeleteMsgPort(rg_kbd_port);
        rg_kbd_io   = NULL;
        rg_kbd_port = NULL;
    }

    rg_installed = 0;
    rg_dev       = NULL;
    return TRUE;
}

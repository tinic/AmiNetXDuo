/*
 * bootcheck -- start the network the way a boot would, using nothing but
 * what the installer left behind, and see whether it comes up.
 *
 * This is the half of the installer test that matters.  installdrive runs
 * the Installer; this runs on the machine the Installer just wrote to,
 * reads S:User-Startup, finds the block the (startup) statement created --
 *
 *      ;BEGIN AmiNetXDuo
 *      C:AddNetInterface DEVS:NetInterfaces/eth0 QUIET
 *      ;END AmiNetXDuo
 *
 * -- and executes exactly those command lines, exactly as the Shell would on
 * the next boot.  Nothing here knows the interface's name, the device, or
 * the address; if the installer wrote the wrong thing, this fails.
 *
 * The pass criterion is that ShowNetStatus reports a real address for this
 * machine.  That is a stronger claim than it looks: the interface file the
 * installer wrote says CONFIGURE=DHCP, so an address can only be there if
 * the stack opened the driver named in that file, put a DHCP DISCOVER on the
 * wire, answered the ARP for it and took the lease.
 *
 * ping and host are run afterwards for the record, but not used to decide.
 * As things stand they report that they cannot read a stack another program
 * has open, which is a limitation of those two commands rather than
 * anything to do with the installation.
 *
 * Exit status:  0  the User-Startup block ran and the machine has an address
 *               5  it ran but no address arrived
 *              10  no block, or the commands in it failed
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dostags.h>

#include <proto/exec.h>
#include <proto/dos.h>

#define USER_STARTUP    "S:User-Startup"
#define BEGIN_MARK      ";BEGIN AmiNetXDuo"
#define END_MARK        ";END AmiNetXDuo"
#define STATUS_FILE     "DH0:t/netstatus.txt"

/*
 * FS-UAE's SLIRP network is 10.0.2.0/24 with the gateway, the DHCP server
 * and the DNS forwarder all at 10.0.2.2 -- see tools/fsuae-run.sh.
 */
#define GATEWAY         "10.0.2.2"

static BPTR out;

static VOID say(const char *fmt, LONG a)
{
    LONG args[1];

    args[0] = a;
    if (out != 0)
    {
        VFPrintf(out, (STRPTR)fmt, args);
        Flush(out);
    }
}

static BOOL starts_with(const char *line, const char *prefix)
{
    while (*prefix != '\0')
    {
        if (*line++ != *prefix++)
            return FALSE;
    }
    return TRUE;
}

/* Run one command with its output appended to a file handle. */
static LONG run_to(const char *command, BPTR where)
{
    struct TagItem tags[3];
    LONG           rc;

    say("\n--- %s\n", (LONG)command);

    tags[0].ti_Tag  = SYS_Output;
    tags[0].ti_Data = (ULONG)where;     /* borrowed: no SYS_Asynch, not closed */
    tags[1].ti_Tag  = SYS_Input;
    tags[1].ti_Data = 0;
    tags[2].ti_Tag  = TAG_DONE;
    tags[2].ti_Data = 0;

    rc = SystemTagList((STRPTR)command, tags);
    say("--- returned %ld\n", rc);
    return rc;
}

static LONG run(const char *command)
{
    return run_to(command, out);
}

/*
 * Did the machine end up with an address?  ShowNetStatus prints
 *
 *      This machine:   10.0.2.15
 *
 * or "none" when it has not got one.  Reading its output back is the only
 * way to ask: there is no call into the running stack from another process.
 */
static BOOL has_address(void)
{
    BPTR  file;
    char  line[256];
    BOOL  found = FALSE;

    file = Open((STRPTR)STATUS_FILE, MODE_NEWFILE);
    if (file == 0)
    {
        say("bootcheck: cannot write " STATUS_FILE "\n", 0);
        return FALSE;
    }
    run_to("C:ShowNetStatus ALL", file);
    Close(file);

    file = Open((STRPTR)STATUS_FILE, MODE_OLDFILE);
    if (file == 0)
        return FALSE;

    while (FGets(file, (STRPTR)line, sizeof(line)) != NULL)
    {
        char *p = line;

        if (out != 0)
            FPuts(out, (STRPTR)line);

        while (*p == ' ' || *p == '\t')
            p++;

        if (!starts_with(p, "This machine:"))
            continue;

        p += 13;
        while (*p == ' ' || *p == '\t')
            p++;

        /* A real address starts with a digit, and 0.0.0.0 is not one. */
        if (*p >= '1' && *p <= '9')
            found = TRUE;

        say("bootcheck: ShowNetStatus says this machine is %s", (LONG)p);
    }

    Close(file);
    return found;
}

int main(void)
{
    BPTR  file;
    char  line[256];
    BOOL  in_block  = FALSE;
    LONG  commands  = 0;
    LONG  failures  = 0;

    out = Open((STRPTR)"DH0:bootcheck.txt", MODE_NEWFILE);
    if (out == 0)
        return RETURN_FAIL;

    say("bootcheck: reading " USER_STARTUP "\n", 0);

    file = Open((STRPTR)USER_STARTUP, MODE_OLDFILE);
    if (file == 0)
    {
        say("bootcheck: there is no " USER_STARTUP
            " -- the installer wrote no startup line\n", 0);
        Close(out);
        return 10;
    }

    while (FGets(file, (STRPTR)line, sizeof(line)) != NULL)
    {
        char *p = line;
        LONG  i;

        /* strip the newline FGets leaves on */
        for (i = 0; line[i] != '\0'; i++)
        {
            if (line[i] == '\n' || line[i] == '\r')
            {
                line[i] = '\0';
                break;
            }
        }

        while (*p == ' ' || *p == '\t')
            p++;

        if (starts_with(p, BEGIN_MARK))
        {
            in_block = TRUE;
            say("bootcheck: found the AmiNetXDuo block\n", 0);
            continue;
        }
        if (starts_with(p, END_MARK))
        {
            in_block = FALSE;
            continue;
        }
        if (!in_block || *p == '\0' || *p == ';')
            continue;

        commands++;
        if (run(p) != 0)
            failures++;
    }

    Close(file);

    if (commands == 0)
    {
        say("bootcheck: no commands between the markers\n", 0);
        Close(out);
        return 10;
    }
    if (failures != 0)
    {
        say("bootcheck: %ld of the startup commands failed\n", commands);
        Close(out);
        return 10;
    }

    if (!has_address())
    {
        say("bootcheck: the startup line ran but no address arrived\n", 0);
        Close(out);
        return 5;
    }

    /*
     * For the record only.  Both of these currently report that they cannot
     * read a stack that another process holds open, which says nothing
     * about whether the installation is correct.
     */
    run("C:ping " GATEWAY " COUNT 3");
    run("C:host aminet.net");
    run("C:netstat -r");

    say("\nbootcheck: the network came up from what the installer wrote\n", 0);
    Close(out);
    return RETURN_OK;
}

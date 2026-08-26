/* Whether an exit() caught by the argv/stack shim should return to its caller.
 *
 * Normal Amiga CLI startup expects exit() to terminate the Process.  A client
 * hosted by dos.library/RunCommand() must instead return its status so the
 * small hosting entry routine can regain control and release the SegList.
 * Keep the weak default in a separate archive member so a client's strong
 * definition can interpose it on the HUNK linker.
 *
 * SPDX-License-Identifier: MIT
 */

__attribute__((weak)) int amiga_client_exit_returns(void)
{
    return 0;
}

__attribute__((weak)) unsigned long amiga_client_stack_size(void)
{
    return 32UL * 1024UL;
}

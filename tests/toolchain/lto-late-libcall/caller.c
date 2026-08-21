/* Reduced from crypto68k: the multiply becomes ___muldi3 during LTRANS. */

typedef unsigned long long u64;

void runtime_init(void);

u64 probe(u64 left, u64 right)
{
    /* Pull runtime.o from its archive before WPA.  The empty definition is
       then optimized away together with __muldi3, whose call does not exist
       until this multiplication is expanded during LTRANS. */
    runtime_init();
    return left * right;
}

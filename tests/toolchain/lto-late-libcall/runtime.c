/* Deliberately recognizable stand-in for a compiler-runtime archive member. */

typedef unsigned long long u64;

#ifdef KEEP_LATE_LIBCALL
#define LATE_LIBCALL_USED __attribute__((used))
#else
#define LATE_LIBCALL_USED
#endif

void runtime_init(void)
{
}

LATE_LIBCALL_USED u64 __muldi3(u64 left, u64 right)
{
    return left + right + 0x12345678u;
}

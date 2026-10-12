static int pad0(int x) { return x * 3 + 1; }
int (*keep)(int) = pad0;
static int __attribute__((noinline)) helper(int a) { return (a ^ 0x55) * 7 + a / 3; }
int sink;
int __attribute__((noinline)) hot(int a) { return helper(a + 1) + 1; }
int __attribute__((cold, noinline)) coldpath(int a) { sink++; return helper(a); }
int main(int argc, char **argv) { (void)argv; if (argc > 5) return coldpath(argc); return hot(argc); }

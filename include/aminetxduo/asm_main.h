/*
 * AmiNetXDuo, pin C main() to the convention the startup code calls it with.
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * FORCE-INCLUDED BY cmake/toolchain-m68k-amigaos.cmake, only when -mregparm
 * is in force.  Nothing should include it.
 *
 * Nothing in this tree calls main().  The toolchain's crt0.o does, and so does
 * src/tools/tool_startup.S: both push argv, then argc, then `jsr _main`.  Under
 * -mregparm=3 a C main() reads argc out of d0 instead -- whatever the startup
 * left there -- so a command reads argc == 0 and believes Workbench launched
 * it.  87 of the 130 main-bearing images here link crt0.o, which is not ours to
 * edit, so the C side is pinned instead.
 *
 * The macro is FUNCTION-LIKE for one reason: an object-like
 * `#define main __attribute__((__stkparm__)) main` also expands where main is
 * an EXPRESSION, and 16 files here do that --
 * `ami_crash_set_reference((APTR)main, "main")`.  A function-like macro needs a
 * following `(` and so fires only on a parameter list.  A CALL to main(...) is
 * the one construct it does not tolerate; that is a compile error, not a silent
 * miscompile.  String literals and comments are single tokens and never
 * matched.
 *
 * Not spelled -Dmain=... on the command line: CMake writes flags into the make
 * recipe as raw text, so the parentheses reach /bin/sh unquoted and the
 * compiler never runs.  A path has no shell metacharacters.
 *
 * __stdargs is the same attribute -- the compiler predefines it as
 * `__attribute__((__stkparm__))` -- and this uses the expansion because it
 * needs no space.
 */

#if defined(__m68k__) && (defined(__GNUC__) || defined(__clang__))
#  if defined(__has_attribute) && __has_attribute(__stkparm__)
#    define main(...) __attribute__((__stkparm__)) main(__VA_ARGS__)
#  else
#    error "m68k compiler without __attribute__((__stkparm__)): main() cannot be pinned to the startup's convention"
#  endif
#endif

/*
 * AND THE LANGUAGE HAS TO BE C23 OR LATER, which is the other half of the same
 * pin and the reason no warning flag has to guard this convention.
 *
 * A declaration of the form `VOID f();' is a different thing in the two
 * standards, and the difference is exactly the bug above.  In C17 it declares
 * f with NO prototype, so `f(a, b)' is accepted and the arguments travel on the
 * stack -- a call the callee, compiled for -mregparm=3, reads out of registers.
 * In C23 `()' MEANS `(void)': f is declared to take no arguments at all, and
 * `f(a, b)' is "too many arguments to function", a hard error, for a direct
 * call and for a call through a function pointer alike (probed with gcc 14 on
 * `extern void f(); f(1,2);' and `extern void (*p)(); p(3,4);').
 *
 * The m68k compiler defaults to C23 -- m68k-amigaos-gcc 16.2 reports
 * __STDC_VERSION__ 202311L with no -std on the command line -- so an
 * unprototyped call cannot be written in this build in the first place and
 * there is nothing left for a warning to catch.  That is why -Wstrict-prototypes
 * is NOT in cmake/ci-warnings.cmake: it can never fire on this arm, and on the
 * host arm (gcc 14, C17) it fires on the NDK's own callback spelling --
 * `VOID (*putChProc)()' in exec_protos.h, which forces the cast in
 * src/bsdsocket/loghook.c:163 -- where nothing about it is ours to change.
 *
 * The default is the whole guarantee, so it is asserted rather than assumed: a
 * build that pins an older -std, or a future compiler that drops C23, would
 * reopen the hole silently and produce an image that mixes the two conventions.
 */
#if defined(__m68k__) && (defined(__GNUC__) || defined(__clang__))
#  if !defined(__STDC_VERSION__) || (__STDC_VERSION__ < 202311L)
#    error "the m68k build must be C23 or later: `()' has to mean `(void)', or a call through an unprototyped declaration passes arguments on the stack while the definition reads them from registers"
#  endif
#endif

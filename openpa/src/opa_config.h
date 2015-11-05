/* src/config.h.in.  Generated from configure.ac by autoheader.  */

/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil ; -*- */
/*
 *  (C) 2008 by Argonne National Laboratory.
 *      See COPYRIGHT in top-level directory.
 */


// /* define if lock-based emulation was explicitly requested at configure time
//    via --with-atomic-primitives=no */
// #undef EXPLICIT_EMULATION
//
// /* Define to 1 if you have the <atomic.h> header file. */
// #undef HAVE_ATOMIC_H
//
// /* Define to 1 if you have the <dlfcn.h> header file. */
// #undef HAVE_DLFCN_H
//
// /* define to 1 if we have support for gcc ARM atomics */
// #undef HAVE_GCC_AND_ARM_ASM
//
// /* define to 1 if we have support for gcc ia64 primitives */
// #undef HAVE_GCC_AND_IA64_ASM
//
// /* define to 1 if we have support for gcc PowerPC atomics */
// #undef HAVE_GCC_AND_POWERPC_ASM
//
// /* define to 1 if we have support for gcc SiCortex atomics */
// #undef HAVE_GCC_AND_SICORTEX_ASM
//
// /* Define if GNU __attribute__ is supported */
// #undef HAVE_GCC_ATTRIBUTE
//
// /* define to 1 if we have support for gcc atomic intrinsics */
// #undef HAVE_GCC_INTRINSIC_ATOMICS
//
// /* define to 1 if we have support for gcc x86/x86_64 primitives */
// #undef HAVE_GCC_X86_32_64
//
// /* define to 1 if we have support for gcc x86 primitives for pre-Pentium 4 */
// #undef HAVE_GCC_X86_32_64_P3
//
// /* Define to 1 if you have the <intrin.h> header file. */
// #undef HAVE_INTRIN_H
//
// /* Define to 1 if you have the <inttypes.h> header file. */
// #undef HAVE_INTTYPES_H
//
// /* Define to 1 if you have the `pthread' library (-lpthread). */
// #undef HAVE_LIBPTHREAD
//
// /* Define to 1 if you have the <memory.h> header file. */
// #undef HAVE_MEMORY_H
//
// /* define to 1 if we have support for Windows NT atomic intrinsics */
// #undef HAVE_NT_INTRINSICS
//
// /* Define to 1 if you have the <pthread.h> header file. */
// #undef HAVE_PTHREAD_H
//
// /* Define to 1 if you have the `pthread_yield' function. */
// #undef HAVE_PTHREAD_YIELD
//
// /* Define to 1 if you have the `sched_yield' function. */
// #undef HAVE_SCHED_YIELD
//
// /* Define to 1 if you have the <stddef.h> header file. */
// #undef HAVE_STDDEF_H
//
// /* Define to 1 if you have the <stdint.h> header file. */
// #undef HAVE_STDINT_H
//
// /* Define to 1 if you have the <stdlib.h> header file. */
// #undef HAVE_STDLIB_H
//
// /* Define if strict checking of atomic operation fairness is desired */
// #undef HAVE_STRICT_FAIRNESS_CHECKS
//
// /* Define to 1 if you have the <strings.h> header file. */
// #undef HAVE_STRINGS_H
//
// /* Define to 1 if you have the <string.h> header file. */
// #undef HAVE_STRING_H
//
// /* define to 1 if we have support for Sun atomic operations library */
// #undef HAVE_SUN_ATOMIC_OPS
//
// /* Define to 1 if you have the <sys/stat.h> header file. */
// #undef HAVE_SYS_STAT_H
//
// /* Define to 1 if you have the <sys/types.h> header file. */
// #undef HAVE_SYS_TYPES_H
//
// /* Define to 1 if you have the <unistd.h> header file. */
// #undef HAVE_UNISTD_H
//
// /* Define to the sub-directory in which libtool stores uninstalled libraries.
//    */
// #undef LT_OBJDIR
//
// /* define to the maximum number of simultaneous threads */
// #undef MAX_NTHREADS
//
// /* Define to 1 if assertions should be disabled. */
// #undef NDEBUG
//
// /* Name of package */
// #undef PACKAGE
//
// /* Define to the address where bug reports for this package should be sent. */
// #undef PACKAGE_BUGREPORT
//
// /* Define to the full name of this package. */
// #undef PACKAGE_NAME
//
// /* Define to the full name and version of this package. */
// #undef PACKAGE_STRING
//
// /* Define to the one symbol short name of this package. */
// #undef PACKAGE_TARNAME
//
// /* Define to the home page for this package. */
// #undef PACKAGE_URL
//
// /* Define to the version of this package. */
// #undef PACKAGE_VERSION
//
// /* The size of `int', as computed by sizeof. */
// #undef SIZEOF_INT
//
// /* The size of `void *', as computed by sizeof. */
// #undef SIZEOF_VOID_P
//
// /* Define to 1 if you have the ANSI C header files. */
// #undef STDC_HEADERS
//
// /* define to 1 to force using lock-based atomic primitives */
// #undef USE_LOCK_BASED_PRIMITIVES
//
// /* define to 1 if unsafe (non-atomic) primitives should be used */
// #undef USE_UNSAFE_PRIMITIVES
//
// /* Version number of package */
// #undef VERSION
//
// /* Define to empty if `const' does not conform to ANSI C. */
// #undef const
//
// /* Define to the equivalent of the C99 'restrict' keyword, or to
//    nothing if this is not supported.  Do not define if restrict is
//    supported directly.  */
// #undef restrict
// /* Work around a bug in Sun C++: it does not support _Restrict or
//    __restrict__, even though the corresponding Sun C compiler ends up with
//    "#define restrict _Restrict" or "#define restrict __restrict__" in the
//    previous line.  Perhaps some future version of Sun C++ will work with
//    restrict; if so, hopefully it defines __RESTRICT like Sun C does.  */
// #if defined __SUNPRO_CC && !defined __RESTRICT
// # define _Restrict
// # define __restrict__
// #endif

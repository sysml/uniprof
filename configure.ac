
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.69])
AC_INIT([uniprof], [0.7], [florian.schmidt@neclab.eu])
AC_CONFIG_SRCDIR([uniprof.c])
AC_CONFIG_HEADERS([include/config.h])

# Parse options
AC_ARG_WITH([libxc], [AS_HELP_STRING([--with-libxc],[use libxc hypercall interface])], [with_libxc="y"])
AC_ARG_WITH([libxencall], [AS_HELP_STRING([--with-libxencall],[use libxencall hypercall interface])], [with_libxencall="y"])
AS_IF([test "x$with_libxc" == "xy"],
          AS_IF([test "x$with_libxencall" == "xy"],
              [AC_MSG_ERROR([Cannot specify both --with-libxc and --with-libxencall at the same time.])]
          )
)
AC_ARG_WITH([xen], [AS_HELP_STRING([--with-xen=DIR],[build against Xen sources in directory @<:@default: build against installed headers/libraries@:>@])], [xenroot=$withval], [use_syslibs="y"])
AS_IF([test "x$xenroot" != "x"],
     [AC_CHECK_FILE([$xenroot/tools],[],[AC_MSG_WARN([$xenroot does not look like a xen source directory!])])])
AC_SUBST(use_syslibs)
AC_SUBST(xenroot)
AC_ARG_ENABLE([debug], [AS_HELP_STRING([--enable-debug],[print debugging output at runtime])],
              [AC_DEFINE([DEBUG], [1], [enable debugging output])])

AC_CANONICAL_TARGET
AS_CASE([$target_cpu], [arm*|aarch*], [target_arch="arm"], [i345686|x86*], [target_arch="x86"], [AC_MSG_ERROR([Unsupported CPU architecture $target_cpu])])
AC_SUBST(target_arch)

AS_IF([test "x$with_libxc" == "xy"],
     AS_IF([test "$target_arch" == "arm"],
          [AC_MSG_ERROR([libxc support lacks functionality to support stack tracing on ARM. Use libxencall instead.])]
     )
)

# Checks for programs.
AC_PROG_CXX
AC_PROG_CC
AC_PROG_MAKE_SET

# Checks for libraries.
AC_CHECK_LIB([xenctrl], [xc_domain_pause], [xc_available="y"])
AC_CHECK_LIB([xencall], [xencall1],
            AC_CHECK_LIB([xenforeignmemory], [xenforeignmemory_map],
                        [libxencall_available="y"] #note LIBxencall_available, we need to test for the headers too
            )
)


# Checks for header files.
AC_CHECK_HEADERS([inttypes.h stdlib.h string.h xenctrl.h], [], [AC_MSG_ERROR([Missing required header file.])])
AC_CHECK_HEADERS([xencall.h], [headerxencall_available="y"], [headerxencall_available="n"])
AC_CHECK_HEADERS([xenforeignmemory.h], [headerxencall_available="y"], [headerxencall_available="n"])

# libxencall is available if both headers and libs are available
AS_IF([test "x$libxencall_available" == "xy"],
     AS_IF([test "x$headerxencall_available" == "xy"],
          [xencall_available="y"]
     )
)

AS_IF([test "x$with_libxc" == "xy"],
     AS_IF([test "x$xc_available" != "xy"],
          [AC_MSG_ERROR([libxc not available.])],
          [use_libxc="y"]
     )
)

AS_IF([test "x$with_libxencall" == "xy"],
     AS_IF([test "x$xencall_available" != "xy"],
          [AC_MSG_ERROR([libxencall not available.])],
          [use_libxencall="y"]
     )
)

# default case: if neither --with-libxc nor --with-libxencall is specified, prefer libxencall if available, otherwise use libxc
AS_IF([test "x$with_libxc" != "xy"],
     AS_IF([test "x$with_libxencall" != "xy"],
          AS_IF([test "x$xencall_available" == "xy"],
               [use_libxencall="y"],
               AS_IF([test "x$xc_available" != "xy"],
                    [use_libxc="y"],
                    AC_MSG_ERROR([Neither libxc nor libxencall/libxenforeignmemory found.])
               )
          )
     )
)

# sanity check: use_libxc and use_libxencall should never be set at the same time
AS_IF([test "x$use_libxc" == "xy"],
     AS_IF([test "x$use_libxencall" == "xy"],
          AC_MSG_ERROR([Internal error: use_libxc and use_libxencall set at the same time.])
     )
)

# add header defines
AS_IF([test "x$use_libxc" == "xy"], AC_DEFINE([HYPERCALL_LIBXC], [1], [Use libxc as hypercall interface]))
AS_IF([test "x$use_libxc" == "xy"], AC_SUBST([use_libs],[libxc]))
AS_IF([test "x$use_libxencall" == "xy"], AC_DEFINE([HYPERCALL_XENCALL], [1], [Use libxencall as hypercall interface]))
AS_IF([test "x$use_libxencall" == "xy"], AC_SUBST([use_libs],[libxencall]))

# ARM needs libxencall/libxenforeignmemory
AS_IF([test "$target_arch" == "arm"],
     AS_IF([test "x$use_libxencall" != "xy"],
          [AC_MSG_ERROR([need libxencall/libxenforeignmemory support from Xen 4.7 for ARM])],
     )
)

# Checks for typedefs, structures, and compiler characteristics.
AC_CHECK_HEADER_STDBOOL
AC_TYPE_SIZE_T
AC_TYPE_UINT16_T
AC_TYPE_UINT32_T
AC_TYPE_UINT64_T

# Checks for library functions.
AC_FUNC_MALLOC
AC_CHECK_FUNCS([clock_gettime strerror strtol strtoul strtoull])

AC_CONFIG_FILES([Makefile])
AC_OUTPUT

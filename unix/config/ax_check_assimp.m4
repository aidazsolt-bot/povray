# SYNOPSIS
#
#   AX_CHECK_ASSIMP(assimp_root)
#
# DESCRIPTION
#
#   Locate Assimp headers and library. assimp_root may be empty (system /
#   pkg-config) or an Arris-style tree with include/ and build-host/{include,bin,lib}.
#   On success sets ax_check_assimp=ok and appends CPPFLAGS / LDFLAGS / LIBS.
#
#   Callers should also add -DPOV_PARSER_EXPERIMENTAL_ASSIMP_IMPORT=1.

AC_DEFUN([AX_CHECK_ASSIMP],
[
  ax_check_assimp="not found"
  ax_check_assimp_save_cppflags="$CPPFLAGS"
  ax_check_assimp_save_ldflags="$LDFLAGS"
  ax_check_assimp_save_libs="$LIBS"

  ax_assimp_root="$1"
  if test x"$ax_assimp_root" != x"" && test x"$ax_assimp_root" != x"yes"; then
    # Preference order matters: Arris host builds live under build-host/; a
    # sibling build/ tree may lack Gaussian splat symbols. Prepend in reverse
    # so the first entry below wins at link time.
    ax_assimp_inc_flags=""
    for d in \
      "$ax_assimp_root/include" \
      "$ax_assimp_root/build/include" \
      "$ax_assimp_root/build-host/include"
    do
      if test -d "$d"; then
        ax_assimp_inc_flags="-I$d $ax_assimp_inc_flags"
      fi
    done
    CPPFLAGS="$ax_assimp_inc_flags $CPPFLAGS"

    ax_assimp_lib_flags=""
    for d in \
      "$ax_assimp_root/bin" \
      "$ax_assimp_root/lib" \
      "$ax_assimp_root/build/lib" \
      "$ax_assimp_root/build/bin" \
      "$ax_assimp_root/build-host/lib" \
      "$ax_assimp_root/build-host/bin"
    do
      if test -d "$d"; then
        ax_assimp_lib_flags="-L$d -Wl,-rpath,$d $ax_assimp_lib_flags"
      fi
    done
    LDFLAGS="$ax_assimp_lib_flags $LDFLAGS"
  else
    AC_CHECK_PROGS([PKGCONFIG], [pkg-config])
    if test x"$PKGCONFIG" != x"" && pkg-config --exists assimp 2>/dev/null; then
      CPPFLAGS="`pkg-config --cflags assimp` $CPPFLAGS"
      LIBS="`pkg-config --libs assimp` $LIBS"
    fi
  fi

  AC_LANG_PUSH([C++])
  AC_CHECK_HEADER(
    [assimp/Importer.hpp],
    [
      AC_MSG_CHECKING([for Assimp library (aiImportFile)])
      LIBS="-lassimp $LIBS"
      AC_LINK_IFELSE(
        [AC_LANG_PROGRAM(
          [[#include <assimp/cimport.h>]],
          [[aiImportFile("x", 0);]])],
        [
          ax_check_assimp="ok"
          AC_MSG_RESULT([yes])
        ],
        [
          ax_check_assimp="link failed"
          AC_MSG_RESULT([no])
        ]
      )
    ],
    [ax_check_assimp="no headers"]
  )
  AC_LANG_POP([C++])

  if test x"$ax_check_assimp" != x"ok"; then
    CPPFLAGS="$ax_check_assimp_save_cppflags"
    LDFLAGS="$ax_check_assimp_save_ldflags"
    LIBS="$ax_check_assimp_save_libs"
  fi
])

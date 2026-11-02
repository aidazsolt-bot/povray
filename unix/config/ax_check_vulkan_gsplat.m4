# SYNOPSIS
#
#   AX_CHECK_VULKAN_GSPLAT(vulkan_headers_root)
#
# DESCRIPTION
#
#   Locate Vulkan headers for Gaussian-splat compute. Prefer linking
#   libvulkan when available (Linux). On MinGW/Windows (and any host where
#   the loader lib is missing) fall back to in-tree volk + VK_NO_PROTOTYPES
#   and load vulkan-1.dll / libvulkan.so.1 at runtime - matching the MSVC
#   gsplat_deps.props path (no vulkan import lib required at link time).
#   On success sets ax_check_vulkan_gsplat=ok and appends CPPFLAGS / LIBS
#   (including -DPOV_HAVE_VULKAN).

AC_DEFUN([AX_CHECK_VULKAN_GSPLAT],
[
  ax_check_vulkan_gsplat="not found"
  ax_check_vulkan_gsplat_save_cppflags="$CPPFLAGS"
  ax_check_vulkan_gsplat_save_libs="$LIBS"

  ax_vk_headers_root="$1"
  # In-tree volk (header-only loader). configure runs from the repo root.
  ax_volk_inc=`cd "$srcdir" && pwd`/libraries/volk
  if test ! -d "$ax_volk_inc"; then
    ax_volk_inc=`cd "$srcdir/.." && pwd`/libraries/volk
  fi
  if test -d "$ax_volk_inc"; then
    CPPFLAGS="-I$ax_volk_inc $CPPFLAGS"
  else
    AC_MSG_WARN([in-tree libraries/volk not found (srcdir=$srcdir)])
  fi

  if test x"$ax_vk_headers_root" != x"" && test x"$ax_vk_headers_root" != x"yes"; then
    if test -d "$ax_vk_headers_root/include"; then
      CPPFLAGS="-I$ax_vk_headers_root/include $CPPFLAGS"
    else
      CPPFLAGS="-I$ax_vk_headers_root $CPPFLAGS"
    fi
  fi

  AC_CHECK_HEADER(
    [vulkan/vulkan.h],
    [
      AC_CHECK_LIB(
        [vulkan],
        [vkCreateInstance],
        [
          ax_check_vulkan_gsplat="ok"
          CPPFLAGS="-DPOV_HAVE_VULKAN $CPPFLAGS"
        ],
        [
          # Some distros ship only libvulkan.so.1 (no unversioned .so).
          AC_MSG_CHECKING([for libvulkan.so.1 (versioned loader)])
          ax_vk_save_libs="$LIBS"
          LIBS="-l:libvulkan.so.1 $LIBS"
          AC_LINK_IFELSE(
            [AC_LANG_PROGRAM(
              [[extern "C" int vkCreateInstance(void);]],
              [[return vkCreateInstance();]])],
            [
              ax_check_vulkan_gsplat="ok"
              CPPFLAGS="-DPOV_HAVE_VULKAN $CPPFLAGS"
              AC_MSG_RESULT([yes])
            ],
            [
              LIBS="$ax_vk_save_libs"
              AC_MSG_RESULT([no])
              # MinGW / Windows: volk loads vulkan-1.dll at runtime.
              if test -d "$ax_volk_inc"; then
                AC_MSG_CHECKING([for volk runtime loader (no libvulkan at link)])
                ax_check_vulkan_gsplat="ok"
                CPPFLAGS="-DPOV_HAVE_VULKAN -DVK_NO_PROTOTYPES $CPPFLAGS"
                AC_MSG_RESULT([yes])
              else
                ax_check_vulkan_gsplat="libvulkan not found"
              fi
            ]
          )
        ]
      )
    ],
    [ax_check_vulkan_gsplat="no headers"]
  )

  if test x"$ax_check_vulkan_gsplat" != x"ok"; then
    CPPFLAGS="$ax_check_vulkan_gsplat_save_cppflags"
    LIBS="$ax_check_vulkan_gsplat_save_libs"
  fi
])

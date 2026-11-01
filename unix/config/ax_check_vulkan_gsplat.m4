# SYNOPSIS
#
#   AX_CHECK_VULKAN_GSPLAT(vulkan_headers_root)
#
# DESCRIPTION
#
#   Locate Vulkan headers and libvulkan for Gaussian-splat compute.
#   vulkan_headers_root should contain include/vulkan/vulkan.h (Khronos
#   Vulkan-Headers tree or system prefix). Also adds -I for in-tree volk.
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
              ax_check_vulkan_gsplat="libvulkan not found"
              AC_MSG_RESULT([no])
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

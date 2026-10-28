//******************************************************************************
///
/// @file windows/povconfig/syspovconfig_mingw32.h
///
/// MinGW / MinGW-w64 GCC compiler-specific POV-Ray compile-time configuration.
///
/// Fresh header (old pre-3.7 mingw support was removed as unused since 3.6).
/// Targets modern MinGW-w64 with C++11.
///
/// @copyright
/// @parblock
///
/// Persistence of Vision Ray Tracer ('POV-Ray') version 3.8.
/// Copyright 1991-2021 Persistence of Vision Raytracer Pty. Ltd.
///
/// POV-Ray is free software: you can redistribute it and/or modify
/// it under the terms of the GNU Affero General Public License as
/// published by the Free Software Foundation, either version 3 of the
/// License, or (at your option) any later version.
///
/// POV-Ray is distributed in the hope that it will be useful,
/// but WITHOUT ANY WARRANTY; without even the implied warranty of
/// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
/// GNU Affero General Public License for more details.
///
/// You should have received a copy of the GNU Affero General Public License
/// along with this program.  If not, see <http://www.gnu.org/licenses/>.
///
/// ----------------------------------------------------------------------------
///
/// POV-Ray is based on the popular DKB raytracer version 2.12.
/// DKBTrace was originally written by David K. Buck.
/// DKBTrace Ver 2.0-2.12 were written by David K. Buck & Aaron A. Collins.
///
/// @endparblock
///
//******************************************************************************

#ifndef POVRAY_WINDOWS_SYSPOVCONFIG_MINGW32_H
#define POVRAY_WINDOWS_SYSPOVCONFIG_MINGW32_H

#include <cstdint>
#include <x86intrin.h>

// MinGW-w64 declares both __MINGW32__ and (on 64-bit) __MINGW64__.
#if !defined(__MINGW32__)
  #error "syspovconfig_mingw32.h included without MinGW"
#endif

#define POV_COMPILER_VER                  "mingw"
#define METADATA_COMPILER_STRING          "mingw-w64 gcc"
#define COMPILER_NAME                     "MinGW GCC"
#define COMPILER_VERSION                  (__GNUC__ * 10000 + __GNUC_MINOR__ * 100 + __GNUC_PATCHLEVEL__)

#define POV_CPP11_SUPPORTED               1

#ifdef _WIN64
  #if defined(__x86_64__) || defined(__amd64__)
    #define METADATA_PLATFORM_STRING      "x86_64-pc-win"
  #else
    #error "Please update syspovconfig_mingw32.h for this 64-bit MinGW architecture"
  #endif
#elif defined(_WIN32)
  #if defined(__i386__)
    #define METADATA_PLATFORM_STRING      "i686-pc-win"
  #else
    #error "Please update syspovconfig_mingw32.h for this 32-bit MinGW architecture"
  #endif
#endif

#define POV_LONG                            long long
#define POV_ULONG                           unsigned long long
#define FORCEINLINE                         inline __attribute__((always_inline))

#define POV_INT8                            std::int8_t
#define POV_UINT8                           std::uint8_t
#define POV_INT16                           std::int16_t
#define POV_UINT16                          std::uint16_t
#define POV_INT32                           std::int32_t
#define POV_UINT32                          std::uint32_t
#define POV_INT64                           std::int64_t
#define POV_UINT64                          std::uint64_t

// MSVC compatibility aliases used elsewhere in the Windows config headers.
#ifndef __int64
  #define __int64                           long long
#endif

#undef ReturnAddress
#define ReturnAddress()                     __builtin_return_address(0)

#define MACHINE_INTRINSICS_H                <x86intrin.h>

// Same optimized-noise feature gates as recent MSVC builds. Individual TUs are
// compiled with -mavx / -mavx2 -mfma / -mfma4 in windows/mingw/build_console.sh;
// runtime dispatch via CPUID still selects a safe implementation.
#define TRY_OPTIMIZED_NOISE
#define TRY_OPTIMIZED_NOISE_AVX_PORTABLE
#define TRY_OPTIMIZED_NOISE_AVX
#define TRY_OPTIMIZED_NOISE_AVXFMA4
#define TRY_OPTIMIZED_NOISE_AVX2FMA3

#define POV_CPUINFO         CPUInfo::GetFeatures()
#define POV_CPUINFO_DETAILS CPUInfo::GetDetails()
#define POV_CPUINFO_H       "cpuid.h"

// MinGW provides these; keep names used by the Windows config layer.
#ifndef _MAX_PATH
  #define _MAX_PATH                         260
#endif

#endif // POVRAY_WINDOWS_SYSPOVCONFIG_MINGW32_H

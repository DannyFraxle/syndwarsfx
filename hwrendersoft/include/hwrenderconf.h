#ifndef _INCLUDE_HWRENDERCONF_H
#define _INCLUDE_HWRENDERCONF_H 1
 
/* include/hwrenderconf.h. Generated automatically at end of configure. */
/* include/hwrendprivconf.h.  Generated from hwrendprivconf.h.in by configure.  */
/* include/hwrendprivconf.h.in.  Generated from configure.ac by autoheader.  */

/* Define for SDL2 support */
#ifndef HWR_ENABLE_SDL2
#define HWR_ENABLE_SDL2 1
#endif

/* Define to 1 if you have the <inttypes.h> header file. */
#ifndef HWR_HAVE_INTTYPES_H
#define HWR_HAVE_INTTYPES_H 1
#endif

/* Define to 1 if you have the <stdint.h> header file. */
#ifndef HWR_HAVE_STDINT_H
#define HWR_HAVE_STDINT_H 1
#endif

/* Define to 1 if you have the <stdio.h> header file. */
#ifndef HWR_HAVE_STDIO_H
#define HWR_HAVE_STDIO_H 1
#endif

/* Define to 1 if you have the <stdlib.h> header file. */
#ifndef HWR_HAVE_STDLIB_H
#define HWR_HAVE_STDLIB_H 1
#endif

/* Define to 1 if you have the <strings.h> header file. */
#ifndef HWR_HAVE_STRINGS_H
#define HWR_HAVE_STRINGS_H 1
#endif

/* Define to 1 if you have the <string.h> header file. */
#ifndef HWR_HAVE_STRING_H
#define HWR_HAVE_STRING_H 1
#endif

/* Define to 1 if you have the <sys/stat.h> header file. */
#ifndef HWR_HAVE_SYS_STAT_H
#define HWR_HAVE_SYS_STAT_H 1
#endif

/* Define to 1 if you have the <sys/types.h> header file. */
#ifndef HWR_HAVE_SYS_TYPES_H
#define HWR_HAVE_SYS_TYPES_H 1
#endif

/* Define to 1 if you have the <unistd.h> header file. */
#ifndef HWR_HAVE_UNISTD_H
#define HWR_HAVE_UNISTD_H 1
#endif

/* Name of package */
#ifndef HWR_PACKAGE
#define HWR_PACKAGE "hwrenderlib"
#endif

/* Define to the address where bug reports for this package should be sent. */
#ifndef HWR_PACKAGE_BUGREPORT
#define HWR_PACKAGE_BUGREPORT "mefistotelis@gmail.com"
#endif

/* Define to the full name of this package. */
#ifndef HWR_PACKAGE_NAME
#define HWR_PACKAGE_NAME "syndwars-hardware-render-library"
#endif

/* Define to the full name and version of this package. */
#ifndef HWR_PACKAGE_STRING
#define HWR_PACKAGE_STRING "syndwars-hardware-render-library 0.1.0.0"
#endif

/* Define to the one symbol short name of this package. */
#ifndef HWR_PACKAGE_TARNAME
#define HWR_PACKAGE_TARNAME "hwrenderlib"
#endif

/* Define to the home page for this package. */
#ifndef HWR_PACKAGE_URL
#define HWR_PACKAGE_URL ""
#endif

/* Define to the version of this package. */
#ifndef HWR_PACKAGE_VERSION
#define HWR_PACKAGE_VERSION "0.1.0.0"
#endif

/* Define to 1 if all of the C89 standard headers exist (not just the ones
   required in a freestanding environment). This macro is provided for
   backward compatibility; new code need not use it. */
#ifndef HWR_STDC_HEADERS
#define HWR_STDC_HEADERS 1
#endif

/* Version number of package */
#ifndef HWR_VERSION
#define HWR_VERSION "0.1.0.0"
#endif

/* Define for Solaris 2.5.1 so the uint32_t typedef from <sys/synch.h>,
   <pthread.h>, or <semaphore.h> is not used. If the typedef were allowed, the
   #define below would cause a syntax error. */
/* #undef _UINT32_T */

/* Define for Solaris 2.5.1 so the uint8_t typedef from <sys/synch.h>,
   <pthread.h>, or <semaphore.h> is not used. If the typedef were allowed, the
   #define below would cause a syntax error. */
/* #undef _UINT8_T */

/* Define to '__inline__' or '__inline' if that's what the C compiler
   calls it, or to nothing if 'inline' is not supported under any name.  */
#ifndef __cplusplus
/* #undef inline */
#endif

/* Define to the type of a signed integer type of width exactly 16 bits if
   such a type exists and the standard includes do not define it. */
/* #undef int16_t */

/* Define to the type of a signed integer type of width exactly 32 bits if
   such a type exists and the standard includes do not define it. */
/* #undef int32_t */

/* Define to the type of an unsigned integer type of width exactly 16 bits if
   such a type exists and the standard includes do not define it. */
/* #undef uint16_t */

/* Define to the type of an unsigned integer type of width exactly 32 bits if
   such a type exists and the standard includes do not define it. */
/* #undef uint32_t */

/* Define to the type of an unsigned integer type of width exactly 8 bits if
   such a type exists and the standard includes do not define it. */
/* #undef uint8_t */
 
/* once: _INCLUDE_HWRENDERCONF_H */
#endif

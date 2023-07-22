/*
 * Copyright (C) 2017-2023 Adrien Gesta-Fline
 *
 * This file is part of libAAF.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef __utils_h__
#define __utils_h__

#include <stdarg.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ANSI_COLOR_RED      "\033[38;5;124m" //"\x1b[31m"
#define ANSI_COLOR_GREEN    "\x1b[92m"
#define ANSI_COLOR_YELLOW   "\x1b[33m" //"\x1b[93m"
#define ANSI_COLOR_ORANGE   "\033[38;5;130m"
#define ANSI_COLOR_BLUE     "\x1b[34m"
#define ANSI_COLOR_MAGENTA  "\x1b[35m"
#define ANSI_COLOR_CYAN     "\033[38;5;81m" //"\x1b[36m"
#define ANSI_COLOR_DARKGREY "\x1b[38;5;242m"
#define ANSI_COLOR_BOLD     "\x1b[1m"
#define ANSI_COLOR_RESET    "\x1b[0m"


#ifdef _WIN32
  #define DIR_SEP '\\'
  #define DIR_SEP_STR "\\"
  /*
   * swprintf() specific string format identifiers
   * https://learn.microsoft.com/en-us/cpp/c-runtime-library/format-specification-syntax-printf-and-wprintf-functions?view=msvc-170#type
   */
  #define WPRIs  L"S" // char*
  #define WPRIws L"s" // wchar_t*
#else
  #define DIR_SEP '/'
  #define DIR_SEP_STR "/"
  /*
   * swprintf() specific string format identifiers
   * https://learn.microsoft.com/en-us/cpp/c-runtime-library/format-specification-syntax-printf-and-wprintf-functions?view=msvc-170#type
   */
  #define WPRIs  L"s"  // char*
  #define WPRIws L"ls" // wchar_t*
#endif

#define IS_DIR_SEP(c) \
  ( (c) == DIR_SEP || (c) == '/' )


wchar_t * utoa( wchar_t *str );
char * clean_filename( char *filename );
char * build_path( const char *sep, const char *first, ... );
const char * fop_get_file( const char *filepath );

int snprintf_realloc( char **str, int *size, size_t offset, const char *format, ... );
int vsnprintf_realloc( char **str, int *size, int offset, const char *fmt, va_list *args );

char * c99strdup( const char *src );

size_t utf16toa( char *astr, uint16_t alen, uint16_t *wstr, uint16_t wlen );
wchar_t * atowchar( const char *astr, uint16_t alen );


char *remove_file_ext (char* myStr, char extSep, char pathSep);

wchar_t * w16tow32( wchar_t *w32buf, uint16_t *w16buf, size_t w16len );

int dump_hex( const unsigned char *stream, size_t stream_sz, char **buf, int *bufsz, int offset );

char * url_decode( char *dst, char *src );

wchar_t * wurl_decode( wchar_t *dst, wchar_t *src );

#ifdef __cplusplus
}
#endif

#endif // ! __utils_h__

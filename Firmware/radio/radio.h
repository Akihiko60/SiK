// -*- Mode: C; c-basic-offset: 8; -*-
//
// Copyright (c) 2011 Michael Smith, All Rights Reserved
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//
//  o Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//  o Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in
//    the documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
// FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
// COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
// INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
// HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
// OF THE POSSIBILITY OF SUCH DAMAGE.
//

///
/// @file	radio.h
///
/// General definitions for the radio application
///

#ifndef _RADIO_H_
#define _RADIO_H_

/// @page hardware Notes on Hardware Allocation
///
/// @section timers Timer Allocation
/// @li Timer0 is used by rtPhy for its timeouts.
/// @li Timer1 is used by the UART driver.
/// @li Timer3 is used to generate the 10ms timer tick.

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <string.h>

#include "board.h"
#include "serial.h"
#include "rtPhy.h"
#include "board_info.h"
#include "parameters.h"
#include "at.h"
#include "flash.h"

/// System clock frequency
///
/// @todo This is standard for the Si1000 if running off the internal 
///       oscillator, but we should have a way to override it.
#define SYSCLK	24500000UL

#if DEBUG
# define debug(fmt, args...)	printf_small(fmt "\n", ##args)
#else
# define debug(fmt, args...)
#endif

/// Print a message and halt, largely for debug purposes
///
/// @param	fmt		printf-style format string and argments
///				to be printed.
///
extern void	panic(char *fmt, ...);

/// Disable interrupts and save their current state
///
/// @param	_save		A local bool or __bit in which to save
///				the current interrupt-enable state.
///
#define interrupt_disable(_save)	do { _save = EA; EA = 0; } while(0)

/// Restore saved interrupt state
///
/// @param	_save		The local bool or __bit in which 
///				interrupt_disable previously saved state.
///
#define interrupt_restore(_save)	do { EA = _save; } while(0)

/// Alternate vprintf implementation
///
extern void	vprintfl(char *fmt, va_list ap) __reentrant;
#define	vprintf(_fmt, _ap)	vprintfl(_fmt, _ap)		///< avoid fighting with the library vprintf() prototype

/// Alternate printf implementation
///
extern void	printfl(char *fmt, ...) __reentrant;
#define printf(_fmt, args...)	printfl(_fmt, ##args)		///< avoid fighting with the library printf() prototype

// Macro evil for generating strings
#define __stringify(_x)		#_x
#define stringify(_x)		__stringify(_x)


extern __code const char g_version_string[];			///< printable version string
extern __code const char g_banner_string[];			///< printable startup banner string

extern __pdata enum BoardFrequency	g_board_frequency;	///< board RF frequency from the bootloader
extern __pdata uint8_t			g_board_bl_version;	///< bootloader version

#endif // _RADIO_H_

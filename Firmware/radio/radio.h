/* -*- Mode: C; c-basic-offset: 8; -*- */
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

#ifndef RADIO_H_
#define RADIO_H_

/*
 * Notes on hardware allocation:
 *
 * Timer0 is used by rtPhy for its timeouts.
 * Timer1 is used by the UART driver.
 */

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <string.h>

#include "board.h"
#include "uart.h"
#include "rtPhy.h"
#include "board_info.h"
#include "parameters.h"
#include "at.h"

// System clock frequency
#define SYSCLK	245000000

#if DEBUG
# define debug(fmt, args...)	printf_tiny(fmt "\n", ##args)
#else
# define debug(fmt, args...)
#endif

#define panic(fmt, args...)	do { printf_small(fmt, ##args); _panic(); } while(0)
#define printf(fmt, args...)	printf_small(fmt, ##args)

#define interrupt_disable(_save)	do { _save = EA; EA = 0; } while(0)
#define interrupt_restore(_save)	do { EA = _save; } while(0)

#define __stringify(_x)		#_x
#define stringify(_x)		__stringify(_x)

extern __code const char g_version_string[];
extern __code const char g_banner_string[];

extern __pdata enum BoardFrequency	g_board_frequency;
extern __pdata uint8_t			g_board_bl_version;

extern void _panic(void);

#endif /* RADIO_H_ */

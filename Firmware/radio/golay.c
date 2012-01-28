// -*- Mode: C; c-basic-offset: 8; -*-
//
// Copyright (c) 2012 Andrew Tridgell, All Rights Reserved
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
/// @file	golay23.c
///
/// golay 23/12 error correction encoding and decoding
///

#include <stdarg.h>
#include "radio.h"
#include "golay23.h"

// intermediate arrays for encodeing/decoding. Using these
// saves some interal memory that would otherwise be needed
// for pointers
static __xdata uint8_t g3[3], g6[6];

// calculate the golay syndrome value
static uint16_t golay_syndrome(uint32_t codeword)
{
	uint32_t shift = (1UL<<22);

	while (codeword >= (1UL<<11)) {
		while ((shift & codeword) == 0) {
			shift >>= 1;
		}
		codeword ^= (shift>>11) * 0xc75UL;
	}
	return codeword;
}

// encode 3 bytes data into 6 bytes of coded data
// input is in g3[], output in g6[]
static void golay_encode24(void)
{
	uint16_t v;
	uint32_t codeword;

	v = g3[0] | ((uint16_t)g3[1]&0xF)<<8;
	codeword = golay23_encode[v];
	g6[0] = codeword & 0xFF;
	g6[1] = (codeword >> 8) & 0xFF;
	g6[2] = (codeword >> 16) & 0xFF;

	v = g3[2] | ((uint16_t)g3[1]&0xF0)<<4;
	codeword = golay23_encode[v];
	g6[3] = codeword & 0xFF;
	g6[4] = (codeword >> 8) & 0xFF;
	g6[5] = (codeword >> 16) & 0xFF;
}

// encode n bytes of data into 2n coded bytes. n must be a multiple 3
void golay_encode(uint8_t n, __xdata uint8_t * __pdata in, __xdata uint8_t * __pdata out)
{
	while (n >= 3) {
		g3[0] = in[0]; g3[1] = in[1]; g3[2] = in[2];
		golay_encode24();
		out[0] = g6[0]; out[1] = g6[1]; out[2] = g6[2]; 
		out[3] = g6[3]; out[4] = g6[4]; out[5] = g6[5]; 
		in += 3;
		out += 6;
		n -= 3;
	}
}

// decode 6 bytes of coded data into 3 bytes of original data
// input is in g6[], output in g3[]
static void golay_decode24(void)
{
	uint16_t v;
	uint32_t codeword;

	codeword = g6[0] | (((uint16_t)g6[1])<<8) | (((uint32_t)(g6[2]&0x7F))<<16);
	v = golay_syndrome(codeword);
	codeword ^= golay23_decode[v];
	v = codeword >> 11;

	g3[0] = v & 0xFF;
	g3[1] = (v >> 8);

	codeword = g6[3] | (((uint16_t)g6[4])<<8) | (((uint32_t)(g6[5]&0x7F))<<16);
	v = golay_syndrome(codeword);
	codeword ^= golay23_decode[v];
	v = codeword >> 11;

	g3[1] |= ((v >> 4)&0xF0);
	g3[2] = v & 0xFF;
}

// decode n bytes of coded data into n/2 bytes of original data
// n must be a multiple of 6
void golay_decode(uint8_t n, __xdata uint8_t * __pdata in, __xdata uint8_t * __pdata out)
{
	while (n >= 6) {
		g6[0] = in[0]; g6[1] = in[1]; g6[2] = in[2];
		g6[3] = in[3]; g6[4] = in[4]; g6[5] = in[5];
		golay_decode24();
		out[0] = g3[0]; out[1] = g3[1]; out[2] = g3[2];
		in += 6;
		out += 3;
		n -= 6;
	}
}


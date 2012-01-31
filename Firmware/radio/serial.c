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
/// @file	serial.c
///
/// MCS51 Serial port driver with flow control and AT command
/// parser integration.
///

#include "serial.h"
#include "packet.h"

// Serial rx/tx buffers.
//
// Note that the rx buffer is much larger than you might expect
// as we need the receive buffer to be many times larger than the
// largest possible air packet size for efficient TDM. Ideally it
// would be about 16x larger than the largest air packet if we have
// 8 TDM time slots
//
__xdata uint8_t rx_buf[2048] = {0};
__xdata uint8_t tx_buf[512] = {0};
__pdata const uint16_t  rx_mask = sizeof(rx_buf) - 1;
__pdata const uint16_t  tx_mask = sizeof(tx_buf) - 1;

// FIFO insert/remove pointers
static volatile __pdata uint16_t				rx_insert, rx_remove;
static volatile __pdata uint16_t				tx_insert, tx_remove;



// flag indicating the transmitter is idle
static volatile bool			tx_idle;

// FIFO status
#define BUF_FULL(_which)	(((_which##_insert + 1) & _which##_mask) == (_which##_remove))
#define BUF_NOT_FULL(_which)	(((_which##_insert + 1) & _which##_mask) != (_which##_remove))
#define BUF_EMPTY(_which)	(_which##_insert == _which##_remove)
#define BUF_NOT_EMPTY(_which)	(_which##_insert != _which##_remove)
#define BUF_USED(_which)	((_which##_insert - _which##_remove) & _which##_mask)
#define BUF_FREE(_which)	((_which##_remove - _which##_insert - 1) & _which##_mask)

// FIFO insert/remove operations
//
// Note that these are nominally interrupt-safe as only one of each
// buffer's end pointer is adjusted by either of interrupt or regular
// mode code.  This is violated if printing from interrupt context,
// which should generally be avoided when possible.
//
#define BUF_INSERT(_which, _c)	do { _which##_buf[_which##_insert] = (_c); \
		_which##_insert = ((_which##_insert+1) & _which##_mask); } while(0)
#define BUF_REMOVE(_which, _c)	do { (_c) = _which##_buf[_which##_remove]; \
		_which##_remove = ((_which##_remove+1) & _which##_mask); } while(0)
#define BUF_PEEK(_which)	_which##_buf[_which##_remove]
#define BUF_PEEK2(_which)	_which##_buf[(_which##_remove+1) & _which##_mask]

static void			_serial_write(uint8_t c);
static void			serial_restart(void);
static void serial_device_set_speed(uint8_t speed);

// save and restore serial interrupt. We use this rather than
// __critical to ensure we don't disturb the timer interrupt at all.
// minimal tick drift is critical for TDM
#define ES0_SAVE_DISABLE __bit ES_saved = ES0; ES0 = 0
#define ES0_RESTORE ES0 = ES_saved

void
serial_interrupt(void) __interrupt(INTERRUPT_UART0) __using(1)
{
	register uint8_t	c;

	// check for received byte first
	if (RI0) {
		// acknowledge interrupt and fetch the byte immediately
		RI0 = 0;
		c = SBUF0;

		// if AT mode is active, the AT processor owns the byte
		if (at_mode_active) {
			// If an AT command is ready/being processed, we would ignore this byte
			if (!at_cmd_ready) {
				at_input(c);
			}
		} else {
			// run the byte past the +++ detector
			at_plus_detector(c);

			// and queue it for general reception
			if (BUF_NOT_FULL(rx)) {
				BUF_INSERT(rx, c);
			} else if (errors.serial_rx_overflow != 255) {
				errors.serial_rx_overflow++;
			}

			// XXX use BUF_FREE here to determine flow control state
		}
	}

	// check for anything to transmit
	if (TI0) {
		// acknowledge the interrupt
		TI0 = 0;

		// look for another byte we can send
		if (BUF_NOT_EMPTY(tx)) {
			// fetch and send a byte
			BUF_REMOVE(tx, c);
			SBUF0 = c;
		} else {
			// note that the transmitter requires a kick to restart it
			tx_idle = true;
		}
	}
}

void
serial_init(uint8_t speed)
{
	// disable UART interrupts
	ES0 = 0;

	// reset buffer state, discard all data
	rx_insert = 0;
	tx_remove = 0;
	tx_insert = 0;
	tx_remove = 0;
	tx_idle = true;

	// configure timer 1 for bit clock generation
	TR1 	= 0;				// timer off
	TMOD	= (TMOD & ~0xf0) | 0x20;	// 8-bit free-running auto-reload mode
	serial_device_set_speed(speed);		// device-specific clocking setup
	TR1	= 1;				// timer on

	// configure the serial port
	SCON0	= 0x10;				// enable receiver, clear interrupts

	// re-enable UART interrupts
	ES0 = 1;
}

bool
serial_write(uint8_t c)
{
	if (serial_write_space() < 1)
		return false;

	_serial_write(c);
	return true;
}

static void
_serial_write(uint8_t c) __reentrant
{
	ES0_SAVE_DISABLE;

	// if we have space to store the character
	if (BUF_NOT_FULL(tx)) {

		// queue the character
		BUF_INSERT(tx, c);

		// if the transmitter is idle, restart it
		if (tx_idle)
			serial_restart();
	} else if (errors.serial_tx_overflow != 255) {
		errors.serial_tx_overflow++;
	}

	ES0_RESTORE;
}

bool
serial_write_buf(__xdata uint8_t *buf, __pdata uint8_t count)
{
	register uint8_t n = count;
	ES0_SAVE_DISABLE;

	if (serial_write_space() < count) {
		if (errors.serial_tx_overflow != 255) {
			errors.serial_tx_overflow++;
		}
		ES0_RESTORE;
		return false;
	}

	while (n--)
		BUF_INSERT(tx, *buf++);

	// if the transmitter is idle, restart it
	if (tx_idle)
		serial_restart();

	ES0_RESTORE;
	return true;
}

uint16_t
serial_write_space(void)
{
	register uint16_t ret;
	ES0_SAVE_DISABLE;

	// If we are in AT mode, discourage anyone from sending bytes.
	// We don't necessarily want to stall serial_write callers, or
	// to block AT commands while their response bytes trickle out,
	// so we maintain ordering for outbound serial bytes and assume
	// that the receiver will drain the stream while waiting for the
	// OK response on AT mode entry.
	//
	if (at_mode_active) {
		ES0_RESTORE;
		return 0;
	}
	ret = BUF_FREE(tx);
	ES0_RESTORE;
	return ret;
}

static void
serial_restart(void)
{
	// XXX may be idle due to flow control - check signals before starting

	// generate a transmit-done interrupt to force the handler to send another byte
	tx_idle = false;
	TI0 = 1;
}

uint8_t
serial_read(void)
{
	register uint8_t	c;

	ES0_SAVE_DISABLE;

	if (BUF_NOT_EMPTY(rx)) {
		BUF_REMOVE(rx, c);
	} else {
		c = '\0';
	}

	ES0_RESTORE;

	return c;
}

uint8_t
serial_peek(void)
{
	register uint8_t c;

	ES0_SAVE_DISABLE;
	c = BUF_PEEK(rx);
	ES0_RESTORE;

	return c;
}

uint8_t
serial_peek2(void)
{
	register uint8_t c;

	ES0_SAVE_DISABLE;
	c = BUF_PEEK2(rx);
	ES0_RESTORE;

	return c;
}

bool
serial_read_buf(__xdata uint8_t * __pdata buf, __pdata uint8_t count)
{
	register uint8_t n = count;
	ES0_SAVE_DISABLE;
	if (BUF_USED(rx) < count) {
		ES0_RESTORE;
		return false;
	}
	while (n--)
		BUF_REMOVE(rx, *buf++);
	ES0_RESTORE;
	return true;
}

uint16_t
serial_read_available(void)
{
	register uint16_t ret;
	ES0_SAVE_DISABLE;
	ret = BUF_USED(rx);
	ES0_RESTORE;
	return ret;
}

void
putchar(char c) __reentrant
{
	if (c == '\n')
		_serial_write('\r');
	_serial_write(c);
}


///
/// Table of supported serial speed settings.
/// the table is looked up based on the 'one byte'
/// serial rate scheme that APM uses. If an unsupported
/// rate is chosen then 57600 is used
///
static const __code struct {
	uint8_t rate;
	uint8_t th1;
	uint8_t ckcon;
} serial_rates[] = {
	{9,   0x96, 0x00}, // 9600
	{19,  0x60, 0x01}, // 19200
	{38,  0xb0, 0x01}, // 38400
	{57,  0x2b, 0x08}, // 57600 - default
	{115, 0x96, 0x08}, // 115200
	{230, 0xcb, 0x08}, // 230400
};

//
// check if a serial speed is valid
//
bool serial_device_valid_speed(uint8_t speed)
{
	uint8_t i;
	uint8_t num_rates = ARRAY_LENGTH(serial_rates);

	for (i = 0; i < num_rates; i++) {
		if (speed == serial_rates[i].rate) {
			return true;
		}
	}
	return false;
}

static void serial_device_set_speed(uint8_t speed)
{
	uint8_t i;
	uint8_t num_rates = ARRAY_LENGTH(serial_rates);

	for (i = 0; i < num_rates; i++) {
		if (speed == serial_rates[i].rate) {
			break;
		}
	}
	if (i == num_rates) {
		i = 3; // 57600 default
	}

	// set the rates in the UART
	TH1 = serial_rates[i].th1;
	CKCON = (CKCON & ~0x0b) | serial_rates[i].ckcon;

	// tell the packet layer how fast the serial link is. This is
	// needed for packet framing timeouts
	packet_set_serial_speed(speed*125UL);	
}


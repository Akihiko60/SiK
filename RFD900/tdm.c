// -*- Mode: C; c-basic-offset: 8; -*-
//
// Copyright (c) 2012 Andrew Tridgell, All Rights Reserved
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
/// @file	tdm.c
///
/// time division multiplexing code
///

#include <stdarg.h>
#include "radio_old.h"
#include "timer.h"
#include "packet.h"
#include "freq_hopping.h"
#include "at.h"
#include "parameters.h"
#include "serial.h"
#include "pins_user.h"

#define USE_TICK_YIELD 1


/// the state of the tdm system
typedef enum{
	TDM_TRANSMIT=0,
	TDM_SILENCE1=1,
	TDM_RECEIVE=2,
	TDM_SILENCE2=3
}tdm_state_t;

static tdm_state_t tdm_state;

/// a packet buffer for the TDM code
uint8_t	pbuf[MAX_PACKET_LENGTH];

/// how many 16usec ticks are remaining in the current state
static uint16_t tdm_state_remaining = 100;

/// This is enough to hold at least 3 packets and is based
/// on the configured air data rate.
static uint16_t tx_window_width;

/// the maximum data packet size we can fit
static uint8_t max_data_packet_length;

/// the silence period between transmit windows
/// This is calculated as the number of ticks it would take to transmit
/// two zero length packets
static uint16_t silence_period;

/// whether we can transmit in the other radios transmit window
/// due to the other radio yielding to us
static bool bonus_transmit;

/// whether we have yielded our window to the other radio
static bool transmit_yield;

// activity indication
// when the 16 bit timer2_tick() value wraps we check if we have received a
// packet since the last wrap (ie. every second)
// If we have the green radio LED is held on.
// Otherwise it blinks every 1 seconds. The received_packet flag
// is set for any received packet, whether it contains user data or
// not.
static bool blink_state;
static bool received_packet = false;;
static bool link_active = false;

/// the latency in 16usec timer2 ticks for sending a zero length packet
static uint16_t packet_latency;

/// the time in 16usec ticks for sending one byte
static uint16_t ticks_per_byte;

/// number of 16usec ticks to wait for a preamble to turn into a packet
/// This is set when we get a preamble interrupt, and causes us to delay
/// sending for a maximum packet latency. This is used to make it more likely
/// that two radios that happen to be exactly in sync in their sends
/// will eventually get a packet through and get their transmit windows
/// sorted out
uint16_t transmit_wait;

/// the long term duty cycle we are aiming for
uint8_t duty_cycle;

/// the average duty cycle we have been transmitting
static float average_duty_cycle;

/// duty cycle offset due to temperature
uint8_t duty_cycle_offset;

/// set to true if we need to wait for our duty cycle average to drop
static bool duty_cycle_wait;

/// how many ticks we have transmitted for in this TDM round
static uint16_t transmitted_ticks;

/// the LDB (listen before talk) RSSI threshold
uint8_t lbt_rssi;

/// how long we have listened for for LBT
static uint16_t lbt_listen_time;

/// how long we have to listen for before LBT is OK
static uint16_t lbt_min_time;

/// random addition to LBT listen time (see European regs)
static uint16_t lbt_rand;

/// test data to display in the main loop. Updated when the tick
/// counter wraps, zeroed when display has happened
uint8_t test_display;

/// set when we should send a statistics packet on the next round
static bool send_statistics;

/// set when we should send a MAVLink report pkt
extern bool seen_mavlink;

enum RSSI_Hunt_ID {
	RSSI_HUNT_IDLE = 0,
	RSSI_HUNT_UP,
	RSSI_HUNT_DOWN,
  RSSI_HUNT_DISABLE,
};

// Varibles used to hunt for a target RSSI by changing the power levels
uint8_t maxPower, presentPower, target_RSSI, powerHysteresis;
enum RSSI_Hunt_ID Hunt_RSSI;

struct tdm_trailer {
	uint16_t window:13;
	uint16_t command:1;
	uint16_t bonus:1;
	uint16_t resend:1;
};
struct tdm_trailer trailer;

/// buffer to hold a remote AT command before sending
static bool send_at_command;
static char remote_at_cmd[AT_CMD_MAXLEN + 1];

#define PACKET_OVERHEAD (sizeof(trailer)+16)

/// display RSSI output
///
void
tdm_show_rssi(void)
{
	printf("L/R RSSI: %u/%u  L/R noise: %u/%u pkts: %u ",
		(unsigned)statistics.average_rssi,
		(unsigned)remote_statistics.average_rssi,
		(unsigned)statistics.average_noise,
		(unsigned)remote_statistics.average_noise,
		(unsigned)statistics.receive_count);
	printf(" txe=%u rxe=%u stx=%u srx=%u ecc=%u/%u",
		(unsigned)errors.tx_errors,
		(unsigned)errors.rx_errors,
		(unsigned)errors.serial_tx_overflow,
		(unsigned)errors.serial_rx_overflow,
		(unsigned)errors.corrected_errors,
		(unsigned)errors.corrected_packets);
	printf(" temp=%d dco=%u pwr=%u\n",
		(int)radio_temperature(),
		(unsigned)duty_cycle_offset,
		(unsigned)presentPower);
	statistics.receive_count = 0;
}

/// display test output
///
static void display_test_output(void)
{
	if (test_display & AT_TEST_RSSI) {
		tdm_show_rssi();
	}
}


/// estimate the flight time for a packet given the payload size
///
/// @param packet_len		payload length in bytes
///
/// @return			flight time in 16usec ticks
static uint16_t flight_time_estimate(uint8_t packet_len)
{
	return packet_latency + (packet_len * ticks_per_byte);
}


/// synchronise tx windows
///
/// we receive a 16 bit value with each packet which indicates how many
/// more 16usec ticks the sender has in their transmit window. The
/// sender has already adjusted the value for the flight time
///
/// The job of this function is to adjust our own transmit window to
/// match the other radio and thus bring the two radios into sync
///
static void sync_tx_windows(uint8_t packet_length)
{
	tdm_state_t old_state = tdm_state;
	uint16_t old_remaining = tdm_state_remaining;

	if (trailer.bonus) {
		// the other radio is using our transmit window
		// via yielded ticks
		if (old_state == TDM_SILENCE1) {
			// This can be caused by a packet
			// taking longer than expected to arrive.
			// don't change back to transmit state or we
			// will cause an extra frequency change which
			// will get us out of sequence
			tdm_state_remaining = silence_period;
		} else if (old_state == TDM_RECEIVE || old_state == TDM_SILENCE2) {
			// this is quite strange. We received a packet
			// so we must have been on the right
			// frequency. Best bet is to set us at the end
			// of their silence period
			tdm_state = TDM_SILENCE2;
			tdm_state_remaining = 1;
		} else {
			tdm_state = TDM_TRANSMIT;
			tdm_state_remaining = trailer.window;
		}
	} else {
		// we are in the other radios transmit window, our
		// receive window
		tdm_state = TDM_RECEIVE;
		tdm_state_remaining = trailer.window;
	}

	// if the other end has sent a zero length packet and we are
	// in their transmit window then they are yielding some ticks to us.
	bonus_transmit = (tdm_state == TDM_RECEIVE && packet_length==0);

#ifdef DEBUG_PINS_YIELD
  if(bonus_transmit)
  {
    P2 |= 0x40;
  }
#endif // DEBUG_PINS_YIELD
  
	// if we are not in transmit state then we can't be yielded
	if (tdm_state != TDM_TRANSMIT) {
		transmit_yield = 0;
	}

	if (at_testmode & AT_TEST_TDM) {
		int16_t delta;
		delta = old_remaining - tdm_state_remaining;
		if (old_state != tdm_state ||
				delta > (int16_t)packet_latency/2 ||
				delta < -(int16_t)packet_latency/2) {
			printf("TDM: %u/%u len=%u ",
					(unsigned)old_state,
					(unsigned)tdm_state,
					(unsigned)packet_length);
			printf(" delta: %d\n",(int)delta);
		}
	}
}

/// update the TDM state machine
///
static void tdm_state_update(uint16_t tdelta)
{
	// update the amount of time we are waiting for a preamble
	// to turn into a real packet
	if (tdelta > transmit_wait) {
		transmit_wait = 0;
	} else {
		transmit_wait -= tdelta;
	}

	// have we passed the next transition point?
	while (tdelta >= tdm_state_remaining) {
		// advance the tdm state machine
		tdm_state = (tdm_state+1) % 4;

		// work out the time remaining in this state
		tdelta -= tdm_state_remaining;

		if (tdm_state == TDM_TRANSMIT || tdm_state == TDM_RECEIVE) {
			tdm_state_remaining = tx_window_width;
		} else {
			tdm_state_remaining = silence_period;
		}

		// change frequency at the start and end of our transmit window
		// this maximises the chance we will be on the right frequency
		// to match the other radio
		if (tdm_state == TDM_TRANSMIT || tdm_state == TDM_SILENCE1) {
			fhop_window_change();
			radio_receiver_on();

			if (num_fh_channels > 1) {
				// reset the LBT listen time
				lbt_listen_time = 0;
				lbt_rand = 0;
			}
		}

		if (tdm_state == TDM_TRANSMIT && (duty_cycle - duty_cycle_offset) != 100) {
			// update duty cycle averages
			average_duty_cycle = (0.95*average_duty_cycle) + (0.05*(100.0*transmitted_ticks)/(2*(silence_period+tx_window_width)));
			transmitted_ticks = 0;
			duty_cycle_wait = (average_duty_cycle >= (duty_cycle - duty_cycle_offset));
		}

		// we lose the bonus on all state changes
		bonus_transmit = 0;
#ifdef DEBUG_PINS_YIELD
    P2 &= ~0x40;
#endif // DEBUG_PINS_YIELD

		// reset yield flag on all state changes
		transmit_yield = 0;

		// no longer waiting for a packet
		transmit_wait = 0;
	}

	tdm_state_remaining -= tdelta;
}

/// change tdm phase
///
void tdm_change_phase(void)
{
	tdm_state = (tdm_state+2) % 4;
}

/// called to check temperature
///
static void temperature_update(void)
{
	register int16_t diff;
	if (radio_get_transmit_power() <= 20) {
		duty_cycle_offset = 0;
		return;
	}

	diff = radio_temperature() - MAX_PA_TEMPERATURE;
	if (diff <= 0 && duty_cycle_offset > 0) {
		// under temperature
		duty_cycle_offset -= 1;
	} else if (diff > 10) {
		// getting hot!
		duty_cycle_offset += 10;
	} else if (diff > 5) {
		// well over temperature
		duty_cycle_offset += 5;
	} else if (diff > 0) {
		// slightly over temperature
		duty_cycle_offset += 1;				
	}
	// limit to minimum of 20% duty cycle to ensure link stays up OK
	if ((duty_cycle-duty_cycle_offset) < 20) {
		duty_cycle_offset = duty_cycle - 20;
	}
}

#define UNLOCKED_COUNT_BLINK 2
#define UNLOCKED_COUNT_RESCAN 10
/// blink the radio LED if we have not received any packets
///
static void link_update(void)
{
	static uint8_t unlock_count, temperature_count;
	if (link_active) {
		unlock_count = 0;
		link_active = false;
#ifdef TDM_SYNC_LOGIC
		TDM_SYNC_PIN = true;
#endif // TDM_SYNC_LOGIC
	} else {
		unlock_count++;
	}
	
	if (unlock_count < UNLOCKED_COUNT_BLINK) {
		LED_RADIO(LED_ON);
	} else {
#ifdef TDM_SYNC_LOGIC
		TDM_SYNC_PIN = false;
#endif // TDM_SYNC_LOGIC

		LED_RADIO(blink_state);
		blink_state = !blink_state;
	}
	
	if (unlock_count >UNLOCKED_COUNT_RESCAN) {
		// if we have been unlocked for 20 seconds
		// then start frequency scanning again

		unlock_count = UNLOCKED_COUNT_BLINK-1;
		// randomise the next transmit window using some
		// entropy from the radio if we have waited
		// for a full set of hops with this time base
		if (timer_entropy() & 1) {
			register uint16_t old_remaining = tdm_state_remaining;
			if (tdm_state_remaining > silence_period) {
				tdm_state_remaining -= packet_latency;
			} else {
				tdm_state_remaining = 1;
			}
			if (at_testmode & AT_TEST_TDM) {
				printf("TDM: change timing %u/%u\n",
								(unsigned)old_remaining,
								(unsigned)tdm_state_remaining);
			}
		}
		
		if (at_testmode & AT_TEST_TDM) {
			printf("TDM: scanning\n");
		}
		fhop_set_locked(false);
	}

	if (unlock_count != 0) {
		statistics.average_rssi = (radio_last_rssi() + 3*(uint16_t)statistics.average_rssi)/4;

		// reset statistics when unlocked
		statistics.receive_count = 0;
    if (RSSI_HUNT_DISABLE != Hunt_RSSI) {
      radio_set_transmit_power(maxPower);
    }
	}
	
	if (unlock_count > 5) {
		memset(&remote_statistics, 0, sizeof(remote_statistics));
	}

	test_display = at_testmode;
	send_statistics = 1;

	temperature_count++;
	if (temperature_count == 4) {
		// check every 2 seconds
		temperature_update();
		temperature_count = 0;
	}
}

void
disable_rssi_hunt()
{
  Hunt_RSSI = RSSI_HUNT_DISABLE;
}

// Hunt for target RSSI using remote packet data
static void update_rssi_target(void)
{
	switch (Hunt_RSSI) {
		case RSSI_HUNT_IDLE:
			if((target_RSSI > powerHysteresis) &&
				 (remote_statistics.average_rssi < target_RSSI - powerHysteresis) &&
				 (presentPower != maxPower))
			{
				presentPower = radio_change_transmit_power(true, maxPower);
				Hunt_RSSI = RSSI_HUNT_UP;
			}
			else if(((uint16_t)target_RSSI + (uint16_t)powerHysteresis > 255) &&
							(remote_statistics.average_rssi > target_RSSI + powerHysteresis) &&
							(presentPower != 0)) {
				presentPower = radio_change_transmit_power(false, maxPower);
				Hunt_RSSI = RSSI_HUNT_DOWN;
			}
			break;
		
		case RSSI_HUNT_UP:
			if((remote_statistics.average_rssi < target_RSSI) && (presentPower != maxPower))
			{
				presentPower = radio_change_transmit_power(true, maxPower);
			}
			else
			{
				Hunt_RSSI = RSSI_HUNT_IDLE;
			}
			break;
			
		case RSSI_HUNT_DOWN:
			if((remote_statistics.average_rssi > target_RSSI) && (presentPower != 0))
			{
				presentPower = radio_change_transmit_power(false, maxPower);
			}
			else
			{
				Hunt_RSSI = RSSI_HUNT_IDLE;
			}
			break;

    case RSSI_HUNT_DISABLE:
      break;

		default:
			Hunt_RSSI = RSSI_HUNT_IDLE;
			break;
	}
}

// dispatch an AT command to the remote system
void
tdm_remote_at(void)
{
	memcpy(remote_at_cmd, at_cmd, strlen(at_cmd)+1);
	send_at_command = true;
}

// handle an incoming at command from the remote radio
static void
handle_at_command(uint8_t len)
{
	if (len < 2 || len > AT_CMD_MAXLEN || 
			pbuf[0] != (uint8_t)'R' ||
			pbuf[1] != (uint8_t)'T') {
		// assume its an AT command reply
		register uint8_t i;
		for (i=0; i<len; i++) {
			putChar(pbuf[i]);
		}
		return;
	}

	// setup the command in the at_cmd buffer
	memcpy(at_cmd, pbuf, len);
	at_cmd[len] = 0;
	at_cmd[0] = 'A'; // replace 'R'
	at_cmd_len = len;
	at_cmd_ready = true;

	// run the AT command, capturing any output to the packet
	// buffer
	// this reply buffer will be sent at the next opportunity
	printf_start_capture(pbuf, sizeof(pbuf));
	at_command();
	len = printf_end_capture();
	if (len > 0) {
		packet_inject(pbuf, len);
	}
}

// a stack carary to detect a stack overflow
//__at(0xFF) uint8_t __idata _canary;

/// main loop for time division multiplexing transparent serial
///
void tdm_serial_loop(void)
{
	static bool Init = false;
	static uint16_t last_t;
	static uint16_t last_link_update;
	if(!Init)
	{
		last_t = timer2_tick();
		last_link_update = last_t;
		Init = true;
	}

#ifdef RADIO_SPLAT_TESTING_MODE
		for (;;) {
				radio_set_channel(0);
				radio_transmit(MAX_PACKET_LENGTH, pbuf, 0);
				//radio_receiver_on();
		}
#else
		uint8_t i;
	  for (i=0;i<1;i++) {																													// do a once loop so continue statements work
		static uint8_t	len;
		static uint16_t tnow, tdelta;
		static uint8_t max_xmit;


		// give the AT command processor a chance to handle a command
		at_command();

		// display test data if needed
		if (test_display) {
			display_test_output();
			test_display = 0;
		}

		if (seen_mavlink && feature_mavlink_framing && !at_mode_active) {
			seen_mavlink = false;
			MAVLink_report();
		}

		// set right receive channel
		radio_set_channel(fhop_receive_channel());

		// get the time before we check for a packet coming in
		tnow = timer2_tick();

		// see if we have received a packet
		if (radio_receive_packet(&len, pbuf)) {

			// update the activity indication
			link_active = true;
			received_packet = true;
			fhop_set_locked(true);
			
			// update filtered RSSI value and packet stats
			statistics.average_rssi = (radio_last_rssi() + 7*(uint16_t)statistics.average_rssi)/8;
			statistics.receive_count++;
			
			// we're not waiting for a preamble
			// any more
			transmit_wait = 0;
			LED_ACTIVITY_TOGGLE();

			if (len < 2) {
				// not a valid packet. We always send
				// two control bytes at the end of every packet
				continue;
			}

			// extract control bytes from end of packet
			memcpy(&trailer, &pbuf[len-sizeof(trailer)], sizeof(trailer));
			len -= sizeof(trailer);

			if (trailer.window == 0 && len != 0) {
				// its a control packet
				if (len == sizeof(statistics)) {
					memcpy(&remote_statistics, pbuf, len);
				}

				update_rssi_target();
				
				// don't count control packets in the stats
				statistics.receive_count--;
			} else if (trailer.window != 0) {
				// sync our transmit windows based on
				// received header
				sync_tx_windows(len);
				last_t = tnow;

				if (trailer.command == 1) {
					handle_at_command(len);
				} else if (len != 0 && 
							!packet_is_duplicate(len, pbuf, trailer.resend) &&
							!at_mode_active) {
					// its user data - send it out
					// the serial port
					//printf("rcv(%d,[", len);
					LED_ACTIVITY(LED_ON);
					serial_write_buf(pbuf, len);
					LED_ACTIVITY(LED_OFF);
					//printf("]\n");
				}
			}
			continue;
		}

		// see how many 16usec ticks have passed and update
		// the tdm state machine. We re-fetch tnow as a bad
		// packet could have cost us a lot of time.
		tnow = timer2_tick();
		tdelta = tnow - last_t;
		tdm_state_update(tdelta);
		last_t = tnow;

		// update link status every 0.5s
		if (tnow - last_link_update > 32768U) {
			link_update();
			last_link_update = tnow;
		}

		if (lbt_rssi != 0) {
			// implement listen before talk
			if (radio_current_rssi() < lbt_rssi) {
				lbt_listen_time += tdelta;
			} else {
				lbt_listen_time = 0;
				if (lbt_rand == 0) {
					lbt_rand = ((uint16_t)rand()) % lbt_min_time;
				}
			}
			if (lbt_listen_time < lbt_min_time + lbt_rand) {
				// we need to listen some more
				continue;
			}
		}

		// we are allowed to transmit in our transmit window
		// or in the other radios transmit window if we have
		// bonus ticks
#if 0//USE_TICK_YIELD
		if (tdm_state != TDM_TRANSMIT &&
					!(bonus_transmit && tdm_state == TDM_RECEIVE)) {
			// we cannot transmit now
			continue;
		}
#else
		if (tdm_state != TDM_TRANSMIT) {
			continue;
		}		
#endif

		if (transmit_yield != 0) {
			// we've give up our window
			continue;
		}

		if (transmit_wait != 0) {
			// we're waiting for a preamble to turn into a packet
			continue;
		}

		if ((!received_packet &&
					radio_preamble_detected() )||
					radio_receive_in_progress()) {
			// a preamble has been detected. Don't
			// transmit for a while
			transmit_wait = packet_latency;
			continue;
		}
		received_packet = false;

		// sample the background noise when it is out turn to
		// transmit, but we are not transmitting,
		// averaged over around 4 samples
		statistics.average_noise = (radio_current_rssi() + 3*(uint16_t)statistics.average_noise)/4;

		if (duty_cycle_wait) {
			// we're waiting for our duty cycle to drop
			continue;
		}

		// how many bytes could we transmit in the time we
		// have left?
		if (tdm_state_remaining < packet_latency) {
			// none ....
			continue;
		}
		max_xmit = (tdm_state_remaining - packet_latency) / ticks_per_byte;
		if (max_xmit < PACKET_OVERHEAD) {
			// can't fit the trailer in with a byte to spare
			continue;
		}
		max_xmit -= PACKET_OVERHEAD;
		if (max_xmit > max_data_packet_length) {
			max_xmit = max_data_packet_length;
		}

#if PIN_MAX > 0
		// Check to see if any pins have changed state
		pins_user_check();
#endif
		
		// ask the packet system for the next packet to send
		if (send_at_command && 
						max_xmit >= strlen(remote_at_cmd)) {
			// send a remote AT command
			len = strlen(remote_at_cmd);
			memcpy(pbuf, remote_at_cmd, len);
			trailer.command = 1;
			send_at_command = false;
		} else {
			// get a packet from the serial port
			len = packet_get_next(max_xmit, pbuf);
			trailer.command = packet_is_injected();
		}

		if (len > max_data_packet_length) {
			panic("oversized tdm packet");
		}

		trailer.bonus = (tdm_state == TDM_RECEIVE);
		trailer.resend = packet_is_resend();

		if (tdm_state == TDM_TRANSMIT &&
						len == 0 &&
						send_statistics &&
						max_xmit >= sizeof(statistics)) {
			// send a statistics packet
			send_statistics = 0;
			memcpy(pbuf, &statistics, sizeof(statistics));
			len = sizeof(statistics);
		
			// mark a stats packet with a zero window
			trailer.window = 0;
			trailer.resend = 0;
		} else {
			// calculate the control word as the number of
			// 16usec ticks that will be left in this
			// tdm state after this packet is transmitted
			trailer.window = (uint16_t)(tdm_state_remaining - flight_time_estimate(len+sizeof(trailer)));
		}

		// set right transmit channel
		radio_set_channel(fhop_transmit_channel());

		memcpy(&pbuf[len], &trailer, sizeof(trailer));

		if (len != 0 && trailer.window != 0) {
			// show the user that we're sending real data
			LED_ACTIVITY(LED_ON);
		}

		if (len == 0) {
			// sending a zero byte packet gives up
			// our window, but doesn't change the
			// start of the next window
			transmit_yield = 1;
		}

		// after sending a packet leave a bit of time before
		// sending the next one. The receivers don't cope well
		// with back to back packets
		transmit_wait = packet_latency;

		// if we're implementing a duty cycle, add the
		// transmit time to the number of ticks we've been transmitting
		if ((duty_cycle - duty_cycle_offset) != 100) {
			transmitted_ticks += flight_time_estimate(len+sizeof(trailer));
		}

		// start transmitting the packet
		if (!radio_transmit(len + sizeof(trailer), pbuf, tdm_state_remaining + (silence_period/2)) &&
				len != 0 && trailer.window != 0 && trailer.command == 0) {
			packet_force_resend();
		}

		if (lbt_rssi != 0) {
			// reset the LBT listen time
			lbt_listen_time = 0;
			lbt_rand = 0;
		}

		// set right receive channel
		radio_set_channel(fhop_receive_channel());

		// re-enable the receiver
		radio_receiver_on();

		if (len != 0 && trailer.window != 0) {
			LED_ACTIVITY(LED_OFF);
		}
	}
#endif
}

#if 0
/// build the timing table
static void 
tdm_build_timing_table(void)
{
	uint8_t j;
	uint16_t rate;
	bool golay_saved = feature_golay;
	feature_golay = false;

	for (rate=2; rate<256; rate=(rate*3)/2) {
		uint32_t latency_sum=0, per_byte_sum=0;
		uint8_t size = MAX_PACKET_LENGTH;
		radio_configure(rate);
		for (j=0; j<50; j++) {
			uint16_t time_0, time_max, t1, t2;
			radio_set_channel(1);
			radio_receiver_on();
			if (serial_read_available() > 0) {
				feature_golay = golay_saved;
				return;
			}
			t1 = timer2_tick();
			if (!radio_transmit(0, pbuf, 0xFFFF)) {
				break;
			}
			t2 = timer2_tick();
			radio_receiver_on();

			time_0 = t2-t1;

			radio_set_channel(2);
			t1 = timer2_tick();
			if (!radio_transmit(size, pbuf, 0xFFFF)) {
				if (size == 0) {
					break;
				}
				size /= 4;
				j--;
				continue;
			}

			t2 = timer2_tick();
			radio_receiver_on();

			time_max = t2-t1;
			latency_sum += time_0;
			per_byte_sum += ((size/2) + (time_max - time_0))/size;
		}
		if (j > 0) {
			printf("{ %u, %u, %u },\n",
						(unsigned)(radio_air_rate()),
						(unsigned)(latency_sum/j),
						(unsigned)(per_byte_sum/j));
		}
	}
	feature_golay = golay_saved;
}


// test hardware CRC code
static void 
crc_test(void)
{
	uint8_t d[4] = { 0x01, 0x00, 0xbb, 0xcc };
	uint16_t crc;
	uint16_t t1, t2;
	crc = crc16(4, &d[0]);
	printf("CRC: %x %x\n", crc, 0xb166);	
	t1 = timer2_tick();
	crc16(MAX_PACKET_LENGTH/2, pbuf);
	t2 = timer2_tick();
	printf("crc %u bytes took %u 16usec ticks\n",
				(unsigned)MAX_PACKET_LENGTH/2,
				t2-t1);
}

// test golay encoding
static void 
golay_test(void)
{
	uint8_t i;
	uint16_t t1, t2;
	uint8_t	buf[MAX_PACKET_LENGTH];
	for (i=0; i<MAX_PACKET_LENGTH/2; i++) {
		pbuf[i] = i;
	}
	t1 = timer2_tick();
	golay_encode(MAX_PACKET_LENGTH/2, pbuf, buf);
	t2 = timer2_tick();
	printf("encode %u bytes took %u 16usec ticks\n",
				(unsigned)MAX_PACKET_LENGTH/2,
				t2-t1);
	// add an error in the middle
	buf[MAX_PACKET_LENGTH/2] ^= 0x23;
	buf[1] ^= 0x70;
	t1 = timer2_tick();
	golay_decode(MAX_PACKET_LENGTH, buf, pbuf);
	t2 = timer2_tick();
	printf("decode %u bytes took %u 16usec ticks\n",
				(unsigned)MAX_PACKET_LENGTH,
				t2-t1);
	for (i=0; i<MAX_PACKET_LENGTH/2; i++) {
		if (pbuf[i] != i) {
			printf("golay error at %u\n", (unsigned)i);
		}
	}
}
#endif


// initialise the TDM subsystem
void
tdm_init(void)
{
	uint16_t i;
	uint8_t air_rate;
	uint32_t window_width;
	radio_set_diversity(false);
  disable_rssi_hunt();
  tdm_state_remaining = 100;


	// Load parameters from flash or defaults
	// this is done before hardware_init() to get the serial speed
	if (!param_load())
		param_default();







	uint32_t freq_min, freq_max;
	uint32_t channel_spacing;
	uint8_t txpower;


	switch (g_board_frequency) {
	case FREQ_433:
		freq_min = 433050000UL;
		freq_max = 434790000UL;
		txpower = 10;
		num_fh_channels = 10;
		break;
	case FREQ_470:
		freq_min = 470000000UL;
		freq_max = 471000000UL;
		txpower = 10;
		num_fh_channels = 10;
		break;
	case FREQ_868:
		freq_min = 868000000UL;
		freq_max = 869000000UL;
		txpower = 10;
		num_fh_channels = 10;
		break;
	case FREQ_915:
		freq_min = 915000000UL;
		freq_max = 928000000UL;
		txpower = 20;
		num_fh_channels = MAX_FREQ_CHANNELS;
		break;
	default:
		freq_min = 0;
		freq_max = 0;
		txpower = 0;
		//panic("bad board frequency %d", g_board_frequency);
		break;
	}

	if (param_s_get(PARAM_NUM_CHANNELS) != 0) {
		num_fh_channels = param_s_get(PARAM_NUM_CHANNELS);
	}
	if (param_s_get(PARAM_MIN_FREQ) != 0) {
		freq_min        = param_s_get(PARAM_MIN_FREQ) * 1000UL;
	}
	if (param_s_get(PARAM_MAX_FREQ) != 0) {
		freq_max        = param_s_get(PARAM_MAX_FREQ) * 1000UL;
	}
	if (param_s_get(PARAM_TXPOWER) != 0) {
		txpower = param_s_get(PARAM_TXPOWER);
	}

	// constrain power and channels
	txpower = constrain(txpower, BOARD_MINTXPOWER, BOARD_MAXTXPOWER);
	num_fh_channels = constrain(num_fh_channels, 1, MAX_FREQ_CHANNELS);

	// double check ranges the board can do
	switch (g_board_frequency) {
	case FREQ_433:
		freq_min = constrain(freq_min, 414000000UL, 460000000UL);
		freq_max = constrain(freq_max, 414000000UL, 460000000UL);
		break;
	case FREQ_470:
		freq_min = constrain(freq_min, 450000000UL, 490000000UL);
		freq_max = constrain(freq_max, 450000000UL, 490000000UL);
		break;
	case FREQ_868:
		freq_min = constrain(freq_min, 849000000UL, 889000000UL);
		freq_max = constrain(freq_max, 849000000UL, 889000000UL);
		break;
	case FREQ_915:
		freq_min = constrain(freq_min, 868000000UL, 935000000UL);
		freq_max = constrain(freq_max, 868000000UL, 935000000UL);
		break;
	default:
		//panic("bad board frequency %d", g_board_frequency);
		break;
	}

	if (freq_max == freq_min) {
		freq_max = freq_min + 1000000UL;
	}

	// get the duty cycle we will use
	duty_cycle = param_s_get(PARAM_DUTY_CYCLE);
	duty_cycle = constrain(duty_cycle, 0, 100);
	param_s_set(PARAM_DUTY_CYCLE, duty_cycle);

	// get the LBT threshold we will use
	lbt_rssi = param_s_get(PARAM_LBT_RSSI);
	if (lbt_rssi != 0) {
		// limit to the RSSI valid range
		lbt_rssi = constrain(lbt_rssi, 25, 220);
	}
	param_s_set(PARAM_LBT_RSSI, lbt_rssi);

	// sanity checks
	param_s_set(PARAM_MIN_FREQ, freq_min/1000);
	param_s_set(PARAM_MAX_FREQ, freq_max/1000);
	param_s_set(PARAM_NUM_CHANNELS, num_fh_channels);

	channel_spacing = (freq_max - freq_min) / (num_fh_channels+2);

	// add half of the channel spacing, to ensure that we are well
	// away from the edges of the allowed range
	freq_min += channel_spacing/2;

	// add another offset based on network ID. This means that
	// with different network IDs we will have much lower
	// interference
	srand(param_s_get(PARAM_NETID));
	if (num_fh_channels > 5) {
		freq_min += ((unsigned long)(rand()*625)) % channel_spacing;
	}
	debug("freq low=%lu high=%lu spacing=%lu\n",
	       freq_min, freq_min+(num_fh_channels*channel_spacing),
	       channel_spacing);

	// set the frequency and channel spacing
	// change base freq based on netid
	radio_set_frequency(freq_min);

	// set channel spacing
	radio_set_channel_spacing(channel_spacing);

	// start on a channel chosen by network ID
	radio_set_channel(param_s_get(PARAM_NETID) % num_fh_channels);


	// report the real air data rate in parameters
	param_s_set(PARAM_AIR_SPEED, radio_air_rate());

	// setup network ID
	radio_set_network_id(param_s_get(PARAM_NETID));

	// setup transmit power
	radio_set_transmit_power(txpower);

	// report the real transmit power in settings
	param_s_set(PARAM_TXPOWER, radio_get_transmit_power());

	// initialise frequency hopping system
	fhop_init(param_s_get(PARAM_NETID));

















	air_rate = radio_air_rate();

	// setup boolean features
	feature_mavlink_framing = param_s_get(PARAM_MAVLINK);
	feature_opportunistic_resend = param_s_get(PARAM_OPPRESEND)?true:false;
	feature_golay = param_s_get(PARAM_ECC)?true:false;
	//feature_rtscts = param_s_get(PARAM_RTSCTS)?true:false;

#define REGULATORY_MAX_WINDOW (((1000000UL/16)*4)/10)
#define LBT_MIN_TIME_USEC 5000

	// tdm_build_timing_table();

	// calculate how many 16usec ticks it takes to send each byte
	ticks_per_byte = (8+(8000000UL/(air_rate*1000UL)))/16;
	ticks_per_byte++;

	// calculate the minimum packet latency in 16 usec units
	// we initially assume a preamble length of 40 bits, then
	// adjust later based on actual preamble length. This is done
	// so that if one radio has antenna diversity and the other
	// doesn't, then they will both using the same TDM round timings
	packet_latency = (8+(10/2)) * ticks_per_byte + 13;

	if (feature_golay) {
		max_data_packet_length = (MAX_PACKET_LENGTH/2) - (6+sizeof(trailer));

		// golay encoding doubles the cost per byte
		ticks_per_byte *= 2;

		// and adds 4 bytes
		packet_latency += 4*ticks_per_byte;
	} else {
		max_data_packet_length = MAX_PACKET_LENGTH - sizeof(trailer);
	}

	// set the silence period to two times the packet latency
	silence_period = 2*packet_latency;

	// set the transmit window to allow for 3 full sized packets
	window_width = 3*(packet_latency+(max_data_packet_length*(uint32_t)ticks_per_byte));

	// if LBT is enabled, we need at least 3*5ms of window width
	if (lbt_rssi != 0) {
		// min listen time is 5ms
		lbt_min_time = LBT_MIN_TIME_USEC/16;
		window_width = constrain(window_width, 3*lbt_min_time, window_width);
	}

  // user specified window is in milliseconds
  if (window_width > param_s_get(PARAM_MAX_WINDOW)*(1000/16)) {
    window_width = param_s_get(PARAM_MAX_WINDOW)*(1000/16);
  }
  
	// the window width cannot be more than 0.4 seconds to meet US
	// regulations
	if (window_width >= REGULATORY_MAX_WINDOW && num_fh_channels > 1) {
		window_width = REGULATORY_MAX_WINDOW;
	}

	// make sure it fits in the 13 bits of the trailer window
	if (window_width > 0x1fff) {
		window_width = 0x1fff;
	}

	tx_window_width = window_width;

	// now adjust the packet_latency for the actual preamble
	// length, so we get the right flight time estimates, while
	// not changing the round timings
	packet_latency += ((settings.preamble_length-10)/2) * ticks_per_byte;

	// tell the packet subsystem our max packet size, which it
	// needs to know for MAVLink packet boundary detection
	i = (tx_window_width - packet_latency) / ticks_per_byte;
	if (i > max_data_packet_length) {
		i = max_data_packet_length;
	}
	packet_set_max_xmit(i);

#ifdef TDM_SYNC_LOGIC
	TDM_SYNC_PIN = false;
#endif // TDM_SYNC_LOGIC

	// Set the max and target RSSI for hunting mode..
	maxPower = param_s_get(PARAM_TXPOWER);
	presentPower = maxPower;
	target_RSSI = param_r_get(PARAM_R_TARGET_RSSI);
	powerHysteresis = param_r_get(PARAM_R_HYSTERESIS_RSSI);
	Hunt_RSSI = RSSI_HUNT_IDLE;
	
	// crc_test();

	// tdm_test_timing();
	
	// golay_test();
}


/// report tdm timings
///
void 
tdm_report_timing(void)
{
	printf("silence_period: %u\n", (unsigned)silence_period); delay_msec(1);
	printf("tx_window_width: %u\n", (unsigned)tx_window_width); delay_msec(1);
	printf("max_data_packet_length: %u\n", (unsigned)max_data_packet_length); delay_msec(1);
}


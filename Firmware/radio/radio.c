// -*- Mode: C; c-basic-offset: 8; -*-
//
// Copyright (c) 2011 Michael Smith, All Rights Reserved
// Copyright (c) 2011 Andrew Tridgell, All Rights Reserved
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
#include "board.h"
#include "radio.h"


__xdata static uint8_t receive_buffer[64];
__xdata static uint8_t receive_packet_length;
__xdata static uint8_t receive_header;
__xdata static uint8_t last_rssi;

static __bit packet_received;
static __bit preamble_detected;

__xdata static struct radio_statistics statistics;

__xdata static struct {
	uint32_t frequency;
	uint32_t channel_spacing;
} settings;

/*
  internal helper functions
 */
static void register_write(uint8_t reg, uint8_t value);
static uint8_t register_read(uint8_t reg);
static bool software_reset(void);
static void set_frequency_registers(uint32_t frequency);
static uint32_t scale_uint32(uint32_t value, uint32_t scale);
static void clear_status_registers(void);

/*
  return a received packet

  returns true on success, false on no packet available

  on success the header_3 byte from the packet is returned in rxheader
 */
bool radio_receive_packet(uint8_t *length, __xdata uint8_t *buf, uint8_t *rxheader)
__critical {
	if (!packet_received) {
		return false;
	}

	*length = receive_packet_length;
	memcpy(buf, receive_buffer, *length);

	*rxheader = receive_header;
	packet_received = 0;
	
	return true;
}


/*
  write to the radios transmit FIFO
 */
void radio_write_transmit_fifo(uint8_t n, __xdata uint8_t *buffer)
__critical {
	NSS1 = 0;
	SPIF1 = 0;
	SPI1DAT = (0x80 | EZRADIOPRO_FIFO_ACCESS);

	while (n--) {
		while (!TXBMT1) /* noop */;
		SPI1DAT = *buffer++;
	}

	while (!TXBMT1) /* noop */;
	while((SPI1CFG & 0x80) == 0x80);

	SPIF1 = 0;
	NSS1 = 1;
}


/*
  return true if a packet preamble has been detected. This means that
  a packet may be coming in
 */
bool radio_preamble_detected(void)
__critical {
	bool ret = (preamble_detected == 1);
	preamble_detected = 0;
	return ret;
}


/*
  return the RSSI from the last packet
 */
uint8_t radio_last_rssi(void)
{
	return last_rssi;
}

/*
  start transmitting a packet from the transmit FIFO
 */
void radio_transmit_start(uint8_t length, uint8_t txheader, uint8_t timeout_ticks)
{
	uint8_t status;

	__critical {
		register_write(EZRADIOPRO_TRANSMIT_HEADER_3, txheader);
		register_write(EZRADIOPRO_TRANSMIT_PACKET_LENGTH, length);
		
		// enable just the packet sent IRQ
		register_write(EZRADIOPRO_INTERRUPT_ENABLE_1, EZRADIOPRO_ENPKSENT);
		register_write(EZRADIOPRO_INTERRUPT_ENABLE_2, 0x00);
		
		clear_status_registers();
		
		// start TX
		register_write(EZRADIOPRO_OPERATING_AND_FUNCTION_CONTROL_1,EZRADIOPRO_TXON|EZRADIOPRO_XTON);
		
		preamble_detected = 0;
	}

	// wait for the IPKSENT interrupt to be raised
	delay_set_ticks(timeout_ticks);
	while (!delay_expired()) {
		status = register_read(EZRADIOPRO_INTERRUPT_STATUS_2);
		status = register_read(EZRADIOPRO_INTERRUPT_STATUS_1);
		if (status & EZRADIOPRO_IPKSENT) {
			return;
		}
	}   

	// transmit timeout ... clear the FIFO 
	radio_clear_transmit_fifo();
}


/*
  clear the transmit FIFO
 */
void radio_clear_transmit_fifo(void)
__critical {
	uint8_t control;
	control = register_read(EZRADIOPRO_OPERATING_AND_FUNCTION_CONTROL_2);
	register_write(EZRADIOPRO_OPERATING_AND_FUNCTION_CONTROL_2, control | EZRADIOPRO_FFCLRTX);
	register_write(EZRADIOPRO_OPERATING_AND_FUNCTION_CONTROL_2, control & ~EZRADIOPRO_FFCLRTX);
}


/*
  clear the receive FIFO
 */
void radio_clear_receive_fifo(void)
__critical {
	uint8_t control;
	control = register_read(EZRADIOPRO_OPERATING_AND_FUNCTION_CONTROL_2);
	register_write(EZRADIOPRO_OPERATING_AND_FUNCTION_CONTROL_2, control | EZRADIOPRO_FFCLRRX);
	register_write(EZRADIOPRO_OPERATING_AND_FUNCTION_CONTROL_2, control & ~EZRADIOPRO_FFCLRRX);
}


/*
  put the radio in receive mode
 */
bool radio_receiver_on(void)
{
	__critical {
		packet_received = 0;
		preamble_detected = 0;
		
		// enable packet valid, CRC error and preamble detection interrupts
		register_write(EZRADIOPRO_INTERRUPT_ENABLE_1, EZRADIOPRO_ENPKVALID|EZRADIOPRO_ENCRCERROR);
		register_write(EZRADIOPRO_INTERRUPT_ENABLE_2, EZRADIOPRO_ENPREAVAL);
		
		clear_status_registers();
		
		// put the radio in receive mode
		register_write(EZRADIOPRO_OPERATING_AND_FUNCTION_CONTROL_1,(EZRADIOPRO_RXON|EZRADIOPRO_XTON));
	}
	
	EX0 = 1;
	
	return true;
}


/*
  initialise the radio hardware
 */
bool radio_initialise(void)
{
	uint8_t status;

	SDN = 0;

	delay_msec(25);

	// make sure there is a radio on the SPI bus
	status = register_read(EZRADIOPRO_DEVICE_VERSION);
	if(status == 0xFF || status < 5) {
		// no valid radio there?
		return false;
	}

	status = register_read(EZRADIOPRO_INTERRUPT_STATUS_2);

	if ((status & EZRADIOPRO_IPOR) == 0) {
		// it hasn't powered up cleanly, reset it
		return software_reset();
	}

	if (status & EZRADIOPRO_ICHIPRDY) {
		// already ready
		return true;
	}

	// enable chip ready interrupt
	register_write(EZRADIOPRO_INTERRUPT_ENABLE_1, 0);
	register_write(EZRADIOPRO_INTERRUPT_ENABLE_2, EZRADIOPRO_ENCHIPRDY);

	// wait for the chip ready bit for 10ms
	delay_set(10);
	while (!delay_expired()) {
		status = register_read(EZRADIOPRO_INTERRUPT_STATUS_1);
		status = register_read(EZRADIOPRO_INTERRUPT_STATUS_2);
		if (status & EZRADIOPRO_ICHIPRDY) {
			return true;
		}
	} 

	return false;
}


/*
  set the transmit frequency
 */
bool radio_set_frequency(uint32_t value)
{
	if (value < 240000000L || value > 930000000L) {
		return false;
	}
	settings.frequency = value;
	set_frequency_registers(value);
	return true;
}


/*
  set the channel spacing
 */
bool radio_set_channel_spacing(uint32_t value)
{
	if (value > 2550000L)
		return false;
	value = scale_uint32(value, 10000);
	settings.channel_spacing = value;
	register_write(EZRADIOPRO_FREQUENCY_HOPPING_STEP_SIZE, value);
	return true;
}


/*
  This table gives the register settings for the radio core indexed by
  the desired air data rate.

  the data for this table is based on the OpenPilot rfm22b driver

  Note that air rates below 2000 bps won't work with the current TDM scheme
 */
#define NUM_DATA_RATES 13
#define NUM_RADIO_REGISTERS 16
__code static const uint32_t air_data_rates[NUM_DATA_RATES] = {   
	   500,  1000,  2000,  4000,  8000,  9600, 16000, 19200, 24000,  32000,  64000, 128000, 192000
};

__code static const uint8_t reg_table[NUM_RADIO_REGISTERS][1+NUM_DATA_RATES] = {
	// first column is the register number
	// remaining columns are the values for the corresponding air data rate
	{ EZRADIOPRO_IF_FILTER_BANDWIDTH, 
	  0x37,  0x37,  0x37,  0x37,  0x3A,  0x3B,  0x26,  0x28,  0x2E,   0x16,   0x07,   0x83,   0x8A},
	{ EZRADIOPRO_AFC_LOOP_GEARSHIFT_OVERRIDE, 
	  0x44,  0x44,  0x44,  0x44,  0x44,  0x44,  0x44,  0x44,  0x44,   0x44,   0x44,   0x44,   0x44},
	{ EZRADIOPRO_AFC_TIMING_CONTROL, 
	  0x0A,  0x0A,  0x0A,  0x0A,  0x0A,  0x0A,  0x0A,  0x0A,  0x0A,   0x0A,   0x0A,   0x0A,   0x0A},
	{ EZRADIOPRO_CLOCK_RECOVERY_GEARSHIFT_OVERRIDE, 
	  0x03,  0x03,  0x03,  0x03,  0x03,  0x03,  0x03,  0x03,  0x03,   0x03,   0x03,   0x03,   0x03},
	{ EZRADIOPRO_CLOCK_RECOVERY_OVERSAMPLING_RATIO, 
	  0xE8,  0xF4,  0xFA,  0x70,  0x3F,  0x34,  0x3F,  0x34,  0x2A,   0x3F,   0x3F,   0x5E,   0x3F},
	{ EZRADIOPRO_CLOCK_RECOVERY_OFFSET_2, 
	  0x60,  0x20,  0x00,  0x01,  0x02,  0x02,  0x02,  0x02,  0x03,   0x02,   0x02,   0x01,   0x02},
	{ EZRADIOPRO_CLOCK_RECOVERY_OFFSET_1, 
	  0x20,  0x41,  0x83,  0x06,  0x0C,  0x75,  0x0C,  0x75,  0x12,   0x0C,   0x0C,   0x5D,   0x0C},
	{ EZRADIOPRO_CLOCK_RECOVERY_OFFSET_0, 
	  0xC5,  0x89,  0x12,  0x25,  0x4A,  0x25,  0x4A,  0x25,  0x6F,   0x4A,   0x4A,   0x86,   0x4A},
	{ EZRADIOPRO_CLOCK_RECOVERY_TIMING_LOOP_GAIN_1, 
	  0x00,  0x00,  0x00,  0x02,  0x07,  0x07,  0x07,  0x07,  0x07,   0x07,   0x07,   0x05,   0x07},
	{ EZRADIOPRO_CLOCK_RECOVERY_TIMING_LOOP_GAIN_0, 
	  0x0A,  0x23,  0x85,  0x0E,  0xFF,  0xFF,  0xFF,  0xFF,  0xFF,   0xFF,   0xFF,   0x74,   0xFF},
	{ EZRADIOPRO_AFC_LIMITER, 
	  0x0E,  0x0E,  0x0E,  0x0E,  0x0E,  0x0D,  0x0D,  0x0E,  0x12,   0x17,   0x31,   0x50,   0x50},
	{ EZRADIOPRO_TX_DATA_RATE_1, 
	  0x04,  0x08,  0x10,  0x20,  0x41,  0x4E,  0x83,  0x9D,  0xC4,   0x08,   0x10,   0x20,   0x31},
	{ EZRADIOPRO_TX_DATA_RATE_0, 
	  0x19,  0x31,  0x62,  0xC5,  0x89,  0xA5,  0x12,  0x49,  0x9C,   0x31,   0x62,   0xC5,   0x27},
	{ EZRADIOPRO_MODULATION_MODE_CONTROL_1, 
	  0x2D,  0x2D,  0x2D,  0x2D,  0x2D,  0x2D,  0x2D,  0x2D,  0x2D,   0x0D,   0x0D,   0x0D,   0x0D},
	{ EZRADIOPRO_MODULATION_MODE_CONTROL_2, 
	  0x23,  0x23,  0x23,  0x23,  0x23,  0x23,  0x23,  0x23,  0x23,   0x23,   0x23,   0x23,   0x23},
	{ EZRADIOPRO_FREQUENCY_DEVIATION, 
	  0x06,  0x06,  0x06,  0x06,  0x06,  0x08,  0x0D,  0x0F,  0x13,   0x1A,   0x33,   0x66,   0x9A}
};



/*
  configure radio based on the air data rate
 */
bool radio_configure(uint32_t air_rate)
{
	uint8_t i, rate_selection;

	// disable interrupts
	register_write(EZRADIOPRO_INTERRUPT_ENABLE_1, 0x00);
	register_write(EZRADIOPRO_INTERRUPT_ENABLE_2, 0x00);

	clear_status_registers();

#ifdef ENABLE_RF_SWITCH
	//set GPIO0 to GND
	register_write(EZRADIOPRO_GPIO0_CONFIGURATION, 0x14);	// RX data (output)
	//set GPIO1 & GPIO2 to control the TRX switch
	register_write(EZRADIOPRO_GPIO1_CONFIGURATION, 0x12);	// TX state (output)
	register_write(EZRADIOPRO_GPIO2_CONFIGURATION, 0x15);	// RX state (output)
#elif ENABLE_RFM50_SWITCH
	//set GPIO0 & GPIO1 to control the TRX switch
	register_write(EZRADIOPRO_GPIO0_CONFIGURATION, 0x15);	// RX state (output)
	register_write(EZRADIOPRO_GPIO1_CONFIGURATION, 0x12);	// TX state (output)
	//set GPIO2 to GND
	register_write(EZRADIOPRO_GPIO2_CONFIGURATION, 0x14);	// RX data (output)
#else
	//set GPIOx to GND
	register_write(EZRADIOPRO_GPIO0_CONFIGURATION, 0x14);	// RX data (output)
	register_write(EZRADIOPRO_GPIO1_CONFIGURATION, 0x14);	// RX data (output)
	register_write(EZRADIOPRO_GPIO2_CONFIGURATION, 0x14);	// RX data (output)
#endif

	// set capacitance
	register_write(EZRADIOPRO_CRYSTAL_OSCILLATOR_LOAD_CAPACITANCE, EZRADIOPRO_OSC_CAP_VALUE);
	
	// setup frequency and channel spacing
	set_frequency_registers(settings.frequency);
	register_write(EZRADIOPRO_FREQUENCY_HOPPING_STEP_SIZE, settings.channel_spacing);
	
	// enable automatic packet handling and CRC
	register_write(EZRADIOPRO_DATA_ACCESS_CONTROL, 
		       EZRADIOPRO_ENPACTX | 
		       EZRADIOPRO_ENCRC | 
		       EZRADIOPRO_CRC_16 |
		       EZRADIOPRO_ENPACRX);
	
	// set FIFO limits to max (we are not using FIFO
	// overflow/underflow interrupts)
	register_write(EZRADIOPRO_TX_FIFO_CONTROL_1, 0x3F);
	register_write(EZRADIOPRO_TX_FIFO_CONTROL_2, 0x0);
	register_write(EZRADIOPRO_RX_FIFO_CONTROL, 0x3F);
	
	// preamble setup
	register_write(EZRADIOPRO_PREAMBLE_LENGTH, 0x0A); // 40 bits
	register_write(EZRADIOPRO_PREAMBLE_DETECTION_CONTROL, 0x28); //  5 nibbles, 20 chips, 10 bits

	// 2 sync bytes and 3 header bytes
	register_write(EZRADIOPRO_HEADER_CONTROL_2, EZRADIOPRO_HDLEN_3BYTE | EZRADIOPRO_SYNCLEN_2BYTE);
	register_write(EZRADIOPRO_SYNC_WORD_3, 0x2D);
	register_write(EZRADIOPRO_SYNC_WORD_2, 0xD4);
	
	// check 2 bytes of header (the network ID)
	register_write(EZRADIOPRO_HEADER_CONTROL_1, 2);
	
	// setup maximum output power
	register_write(EZRADIOPRO_TX_POWER, 0x7);	

	// work out which register table column we will use
	for (i=0; i<NUM_DATA_RATES-1; i++) {
		if (air_data_rates[i] >= air_rate) break;
	}
	rate_selection = i;
	
	// set the registers from the table
	for (i=0; i<NUM_RADIO_REGISTERS; i++) {
		register_write(reg_table[i][0], 
			 reg_table[i][rate_selection+1]);
	}
   
	return true;
}


/*
  setup a 16 bit network ID
 */
void radio_set_network_id(uint16_t id)
{
	register_write(EZRADIOPRO_TRANSMIT_HEADER_1, id>>8);
	register_write(EZRADIOPRO_TRANSMIT_HEADER_2, id&0xFF);
}


/*
  write to a radio register
 */
static void register_write(uint8_t reg, uint8_t value)
__critical {
	NSS1 = 0;                           // drive NSS low
	SPIF1 = 0;                          // clear SPIF
	SPI1DAT = (reg | 0x80);             // write reg address
	while(!TXBMT1);                     // wait on TXBMT
	SPI1DAT = value;                    // write value
	while(!TXBMT1);                     // wait on TXBMT
	while((SPI1CFG & 0x80) == 0x80);    // wait on SPIBSY
	
	SPIF1 = 0;                          // leave SPIF cleared
	NSS1 = 1;                           // drive NSS high
}


/*
  read from a radio register
 */
static uint8_t register_read(uint8_t reg)
__critical {
	uint8_t value;

	NSS1 = 0;                           // dsrive NSS low
	SPIF1 = 0;                          // cleat SPIF
	SPI1DAT = ( reg );                  // write reg address
	while(!TXBMT1);                     // wait on TXBMT
	SPI1DAT = 0x00;                     // write anything
	while(!TXBMT1);                     // wait on TXBMT
	while((SPI1CFG & 0x80) == 0x80);    // wait on SPIBSY
	value = SPI1DAT;                    // read value
	SPIF1 = 0;                          // leave SPIF cleared
	NSS1 = 1;                           // drive NSS low
	
	return value;
}

/*
  read some bytes from the receive FIFO
 */
static void read_receive_fifo(uint8_t n, __xdata uint8_t *buffer)
__critical {
	NSS1 = 0;                            // drive NSS low
	SPIF1 = 0;                           // clear SPIF
	SPI1DAT = (EZRADIOPRO_FIFO_ACCESS);
	while(!SPIF1);                       // wait on SPIF
	ACC = SPI1DAT;                      // discard first byte

	while(n--) {
		SPIF1 = 0;                        // clear SPIF
		SPI1DAT = 0x00;                  // write anything
		while(!SPIF1);                    // wait on SPIF
		*buffer++ = SPI1DAT;             // copy to buffer
	}

	SPIF1 = 0;                           // leave SPIF cleared
	NSS1 = 1;                            // drive NSS high
}

/*
  clear interrupts by reading the two status registers
 */
static void clear_status_registers(void)
{
	register_read(EZRADIOPRO_INTERRUPT_STATUS_1);
	register_read(EZRADIOPRO_INTERRUPT_STATUS_2);
}

/*
  scale a uint32_t, rounding to nearest multiple
 */
static uint32_t scale_uint32(uint32_t value, uint32_t scale)
{
	return (value + (value>>1))/scale;
}


/*
  reset the radio using a software reset
 */
static bool software_reset(void)
{
	uint8_t status;

	// Clear interrupt enable and interrupt flag bits
	register_write(EZRADIOPRO_INTERRUPT_ENABLE_1, 0);
	register_write(EZRADIOPRO_INTERRUPT_ENABLE_2, 0);

	clear_status_registers();

	// SWReset
	register_write(EZRADIOPRO_OPERATING_AND_FUNCTION_CONTROL_1, (EZRADIOPRO_SWRES|EZRADIOPRO_XTON));

	// wait on any interrupt with a 2 MS timeout
	delay_set(2);
	while (IRQ) {
		if(delay_expired()) {
			return false;
		}
	}

	// enable chip ready interrupt
	register_write(EZRADIOPRO_INTERRUPT_ENABLE_1, 0);
	register_write(EZRADIOPRO_INTERRUPT_ENABLE_2, EZRADIOPRO_ENCHIPRDY);

	delay_set(20);
	while (!delay_expired()) {
		status = register_read(EZRADIOPRO_INTERRUPT_STATUS_1);
		status = register_read(EZRADIOPRO_INTERRUPT_STATUS_2);
		if (status & EZRADIOPRO_ICHIPRDY) {
			return true;
		}
	}
	return false;
}

/*
  set the radio frequency registers
 */
static void set_frequency_registers(uint32_t frequency)
{
	uint8_t band;
	uint16_t carrier;

	if (frequency > 480000000UL) {
		frequency -= 480000000UL;
		band  = frequency / 20000000UL;
		frequency -= (uint32_t)band * 20000000UL;
		frequency  = scale_uint32(frequency, 625);
		frequency <<= 1;
		band |= EZRADIOPRO_HBSEL;
	} else {
		frequency -= 240000000UL;
		band  = frequency / 10000000UL;
		frequency -= (uint32_t)band * 10000000UL;
		frequency  = scale_uint32(frequency, 625);
		frequency <<= 2;
	}

	band |= EZRADIOPRO_SBSEL;
	carrier = (uint16_t)frequency;

	register_write(EZRADIOPRO_FREQUENCY_BAND_SELECT, band);
	register_write(EZRADIOPRO_NOMINAL_CARRIER_FREQUENCY_1, carrier >> 8);
	register_write(EZRADIOPRO_NOMINAL_CARRIER_FREQUENCY_0, carrier & 0xFF);
}


/*
  the receiver interrupt

  We expect to get the following types of interrupt:

   - packet valid, when we have received a good packet
   - CRC error, when a packet fails the CRC check
   - preamble valid, when a packet has started arriving
 */
INTERRUPT(Receiver_ISR, INTERRUPT_INT0)
{
	uint8_t status, status2;

	status2 = register_read(EZRADIOPRO_INTERRUPT_STATUS_2);
	status  = register_read(EZRADIOPRO_INTERRUPT_STATUS_1);
	register_write(EZRADIOPRO_INTERRUPT_ENABLE_1, 0);
	register_write(EZRADIOPRO_INTERRUPT_ENABLE_2, 0);
	
	if (status & EZRADIOPRO_IPKVALID) {
		// we have received a valid packet
		preamble_detected = 0;

		if (packet_received==0) {
			packet_received = 1;
			receive_packet_length = register_read(EZRADIOPRO_RECEIVED_PACKET_LENGTH);
			receive_header        = register_read(EZRADIOPRO_RECEIVED_HEADER_3);
			if (receive_packet_length != 0) {
				read_receive_fifo(receive_packet_length, receive_buffer);
			}
		}

		radio_clear_receive_fifo();
	} else if (status & EZRADIOPRO_ICRCERROR) {
		// we got a crc error on the packet
		statistics.rx_errors++;
	} else if (status2 & EZRADIOPRO_IPREAVAL) {
		// a valid preamble has been detected
		preamble_detected = 1;

		// read the RSSI register for logging
		last_rssi = register_read(EZRADIOPRO_RECEIVED_SIGNAL_STRENGTH_INDICATOR);

		// enable packet valid and CRC error IRQ
		register_write(EZRADIOPRO_INTERRUPT_ENABLE_1, EZRADIOPRO_ENPKVALID|EZRADIOPRO_ENCRCERROR);
		register_write(EZRADIOPRO_INTERRUPT_ENABLE_2, EZRADIOPRO_ENPREAVAL);
		return;
	}
	
	// enable packet valid and CRC error IRQ
	register_write(EZRADIOPRO_INTERRUPT_ENABLE_1, EZRADIOPRO_ENPKVALID|EZRADIOPRO_ENCRCERROR);
	register_write(EZRADIOPRO_INTERRUPT_ENABLE_2, EZRADIOPRO_ENPREAVAL);

	// enable RX again
	register_write(EZRADIOPRO_OPERATING_AND_FUNCTION_CONTROL_1,(EZRADIOPRO_RXON|EZRADIOPRO_XTON));
}


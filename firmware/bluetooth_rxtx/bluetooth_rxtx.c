/*
 * Copyright 2010-2013 Michael Ossmann
 * Copyright 2011-2013 Dominic Spill
 *
 * This file is part of Project Ubertooth.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

#include <string.h>

#include "ubertooth.h"
#include "ubertooth_usb.h"
#include "ubertooth_interface.h"
#include "ubertooth_rssi.h"
#include "ubertooth_cs.h"
#include "ubertooth_dma.h"
#include "ubertooth_clock.h"
#include "bluetooth.h"
#include "bluetooth_le.h"
#include "cc2400_rangetest.h"
#include "ego.h"

#define MIN(x,y)	((x)<(y)?(x):(y))
#define MAX(x,y)	((x)>(y)?(x):(y))

/* build info */
const char compile_info[] =
	"ubertooth " GIT_REVISION " (" COMPILE_BY "@" COMPILE_HOST ") " TIMESTAMP;

/* hopping stuff */
volatile uint8_t  hop_mode = HOP_NONE;
volatile uint8_t  do_hop = 0;                  // set by timer interrupt
volatile uint16_t channel = 2441;
volatile uint16_t hop_direct_channel = 0;      // for hopping directly to a channel
volatile uint16_t hop_timeout = 158;
volatile uint16_t requested_channel = 0;
volatile uint16_t saved_request = 0;

/* bulk USB stuff */
volatile uint8_t  idle_buf_clkn_high = 0;
volatile uint32_t idle_buf_clk100ns = 0;
volatile uint16_t idle_buf_channel = 0;
volatile uint8_t  dma_discard = 0;
volatile uint8_t  status = 0;

/* operation mode */
volatile uint8_t mode = MODE_IDLE;
volatile uint8_t requested_mode = MODE_IDLE;
volatile uint8_t jam_mode = JAM_NONE;
volatile uint8_t ego_mode = EGO_FOLLOW;

volatile uint8_t modulation = MOD_BT_BASIC_RATE;

/* specan stuff */
volatile uint16_t low_freq = 2400;
volatile uint16_t high_freq = 2483;
volatile int8_t rssi_threshold = -30;  // -54dBm - 30 = -84dBm

/* le stuff */
uint8_t slave_mac_address[6] = { 0, };

le_state_t le = {
	.access_address = 0x8e89bed6,           // advertising channel access address
	.synch = 0x6b7d,                        // bit-reversed adv channel AA
	.syncl = 0x9171,
	.crc_init  = 0x555555,                  // advertising channel CRCInit
	.crc_init_reversed = 0xAAAAAA,
	.crc_verify = 0,

	.link_state = LINK_INACTIVE,
	.conn_epoch = 0,
	.target_set = 0,
	.last_packet = 0,
};

typedef struct _le_promisc_active_aa_t {
	u32 aa;
	int count;
} le_promisc_active_aa_t;

typedef struct _le_promisc_state_t {
	// LFU cache of recently seen AA's
	le_promisc_active_aa_t active_aa[32];

	// recovering hop interval
	u32 smallest_hop_interval;
	int consec_intervals;
} le_promisc_state_t;
le_promisc_state_t le_promisc;
#define AA_LIST_SIZE (int)(sizeof(le_promisc.active_aa) / sizeof(le_promisc_active_aa_t))

/* LE jamming */
#define JAM_COUNT_DEFAULT 40
int le_jam_count = 0;

/* set LE access address */
static void le_set_access_address(u32 aa);

typedef int (*data_cb_t)(char *);
data_cb_t data_cb = NULL;

typedef void (*packet_cb_t)(u8 *);
packet_cb_t packet_cb = NULL;

/* Unpacked symbol buffers (two rxbufs) */
char unpacked[DMA_SIZE*8*2];

static int enqueue(uint8_t type, uint8_t* buf)
{
	usb_pkt_rx* f = usb_enqueue();

	/* fail if queue is full */
	if (f == NULL) {
		status |= FIFO_OVERFLOW;
		return 0;
	}

	f->pkt_type = type;
	if(type == SPECAN) {
		f->clkn_high = (clkn >> 20) & 0xff;
		f->clk100ns = CLK100NS;
	} else {
		f->clkn_high = idle_buf_clkn_high;
		f->clk100ns = idle_buf_clk100ns;
		f->channel = (uint8_t)((idle_buf_channel - 2402) & 0xff);
		f->rssi_min = rssi_min;
		f->rssi_max = rssi_max;
		f->rssi_avg = rssi_get_avg(idle_buf_channel);
		f->rssi_count = rssi_count;
	}

	memcpy(f->data, buf, DMA_SIZE);

	f->status = status;
	status = 0;

	return 1;
}

int enqueue_with_ts(uint8_t type, uint8_t* buf, uint32_t ts)
{
	usb_pkt_rx* f = usb_enqueue();

	/* fail if queue is full */
	if (f == NULL) {
		status |= FIFO_OVERFLOW;
		return 0;
	}

	f->clkn_high = 0;
	f->clk100ns = ts;

	f->channel = (uint8_t)((channel - 2402) & 0xff);
	f->rssi_avg = 0;
	f->rssi_count = 0;

	memcpy(f->data, buf, DMA_SIZE);

	f->status = status;
	status = 0;

	return 1;
}

static int vendor_request_handler(uint8_t request, uint16_t* request_params, uint8_t* data, int* data_len)
{
	uint32_t command[5];
	uint32_t result[5];
	uint64_t ac_copy;
	uint32_t clock;
	size_t length; // string length
	usb_pkt_rx* p = NULL;
	uint16_t reg_val;
	uint8_t i;

	switch (request) {

	case UBERTOOTH_PING:
		*data_len = 0;
		break;

	case UBERTOOTH_RX_SYMBOLS:
		requested_mode = MODE_RX_SYMBOLS;
		*data_len = 0;
		break;

	case UBERTOOTH_TX_SYMBOLS:
		hop_mode = HOP_BLUETOOTH;
		requested_mode = MODE_TX_SYMBOLS;
		*data_len = 0;
		break;

	case UBERTOOTH_GET_USRLED:
		data[0] = (USRLED) ? 1 : 0;
		*data_len = 1;
		break;

	case UBERTOOTH_SET_USRLED:
		if (request_params[0])
			USRLED_SET;
		else
			USRLED_CLR;
		break;

	case UBERTOOTH_GET_RXLED:
		data[0] = (RXLED) ? 1 : 0;
		*data_len = 1;
		break;

	case UBERTOOTH_SET_RXLED:
		if (request_params[0])
			RXLED_SET;
		else
			RXLED_CLR;
		break;

	case UBERTOOTH_GET_TXLED:
		data[0] = (TXLED) ? 1 : 0;
		*data_len = 1;
		break;

	case UBERTOOTH_SET_TXLED:
		if (request_params[0])
			TXLED_SET;
		else
			TXLED_CLR;
		break;

	case UBERTOOTH_GET_1V8:
		data[0] = (CC1V8) ? 1 : 0;
		*data_len = 1;
		break;

	case UBERTOOTH_SET_1V8:
		if (request_params[0])
			CC1V8_SET;
		else
			CC1V8_CLR;
		break;

	case UBERTOOTH_GET_PARTNUM:
		get_part_num(data, data_len);
		break;

	case UBERTOOTH_RESET:
		requested_mode = MODE_RESET;
		break;

	case UBERTOOTH_GET_SERIAL:
		get_device_serial(data, data_len);
		break;

#ifdef UBERTOOTH_ONE
	case UBERTOOTH_GET_PAEN:
		data[0] = (PAEN) ? 1 : 0;
		*data_len = 1;
		break;

	case UBERTOOTH_SET_PAEN:
		if (request_params[0])
			PAEN_SET;
		else
			PAEN_CLR;
		break;

	case UBERTOOTH_GET_HGM:
		data[0] = (HGM) ? 1 : 0;
		*data_len = 1;
		break;

	case UBERTOOTH_SET_HGM:
		if (request_params[0])
			HGM_SET;
		else
			HGM_CLR;
		break;
#endif

#ifdef TX_ENABLE
	case UBERTOOTH_TX_TEST:
		requested_mode = MODE_TX_TEST;
		break;

	case UBERTOOTH_GET_PALEVEL:
		data[0] = cc2400_get(FREND) & 0x7;
		*data_len = 1;
		break;

	case UBERTOOTH_SET_PALEVEL:
		if( request_params[0] < 8 ) {
			cc2400_set(FREND, 8 | request_params[0]);
		} else {
			return 0;
		}
		break;

	case UBERTOOTH_RANGE_TEST:
		requested_mode = MODE_RANGE_TEST;
		break;

	case UBERTOOTH_REPEATER:
		requested_mode = MODE_REPEATER;
		break;
#endif

	case UBERTOOTH_RANGE_CHECK:
		data[0] = rr.valid;
		data[1] = rr.request_pa;
		data[2] = rr.request_num;
		data[3] = rr.reply_pa;
		data[4] = rr.reply_num;
		*data_len = 5;
		break;

	case UBERTOOTH_STOP:
		requested_mode = MODE_IDLE;
		break;

	case UBERTOOTH_GET_MOD:
		data[0] = modulation;
		*data_len = 1;
		break;

	case UBERTOOTH_SET_MOD:
		modulation = request_params[0];
		break;

	case UBERTOOTH_GET_CHANNEL:
		data[0] = channel & 0xFF;
		data[1] = (channel >> 8) & 0xFF;
		*data_len = 2;
		break;

	case UBERTOOTH_SET_CHANNEL:
		requested_channel = request_params[0];
		/* bluetooth band sweep mode, start at channel 2402 */
		if (requested_channel > MAX_FREQ) {
			hop_mode = HOP_SWEEP;
			requested_channel = 2402;
		}
		/* fixed channel mode, can be outside bluetooth band */
		else {
			hop_mode = HOP_NONE;
			requested_channel = MAX(requested_channel, MIN_FREQ);
			requested_channel = MIN(requested_channel, MAX_FREQ);
		}

		if (mode != MODE_BT_FOLLOW_LE) {
			channel = requested_channel;
			requested_channel = 0;

			/* CS threshold is mode-dependent. Update it after
			 * possible mode change. TODO - kludgy. */
			cs_threshold_calc_and_set(channel);
		}
		break;

	case UBERTOOTH_SET_ISP:
		set_isp();
		*data_len = 0; /* should never return */
		break;

	case UBERTOOTH_FLASH:
		bootloader_ctrl = DFU_MODE;
		reset();
		break;

	case UBERTOOTH_SPECAN:
		if (request_params[0] < 2049 || request_params[0] > 3072 ||
				request_params[1] < 2049 || request_params[1] > 3072 ||
				request_params[1] < request_params[0])
			return 0;
		low_freq = request_params[0];
		high_freq = request_params[1];
		requested_mode = MODE_SPECAN;
		*data_len = 0;
		break;

	case UBERTOOTH_LED_SPECAN:
		if (request_params[0] > 256)
			return 0;
		rssi_threshold = 54 - request_params[0];
		requested_mode = MODE_LED_SPECAN;
		*data_len = 0;
		break;

	case UBERTOOTH_GET_REV_NUM:
		data[0] = 0x00;
		data[1] = 0x00;

		length = (u8)strlen(GIT_REVISION);
		data[2] = length;

		memcpy(&data[3], GIT_REVISION, length);

		*data_len = 2 + 1 + length;
		break;

	case UBERTOOTH_GET_COMPILE_INFO:
		length = (u8)strlen(compile_info);
		data[0] = length;
		memcpy(&data[1], compile_info, length);
		*data_len = 1 + length;
		break;

	case UBERTOOTH_GET_BOARD_ID:
		data[0] = BOARD_ID;
		*data_len = 1;
		break;

	case UBERTOOTH_SET_SQUELCH:
		cs_threshold_req = (int8_t)request_params[0];
		cs_threshold_calc_and_set(channel);
		break;

	case UBERTOOTH_GET_SQUELCH:
		data[0] = cs_threshold_req;
		*data_len = 1;
		break;

	case UBERTOOTH_SET_BDADDR:
		target.address = 0;
		target.syncword = 0;
		for(int i=0; i < 8; i++) {
			target.address |= (uint64_t)data[i] << 8*i;
		}
		for(int i=0; i < 8; i++) {
			target.syncword |= (uint64_t)data[i+8] << 8*i;
		}
		precalc();
		break;

	case UBERTOOTH_START_HOPPING:
		clkn_offset = 0;
		for(int i=0; i < 4; i++) {
			clkn_offset <<= 8;
			clkn_offset |= data[i];
		}
		hop_mode = HOP_BLUETOOTH;
		dma_discard = 1;
		DIO_SSEL_SET;
		clk100ns_offset = (data[4] << 8) | (data[5] << 0);
		requested_mode = MODE_BT_FOLLOW;
		break;

	case UBERTOOTH_AFH:
		hop_mode = HOP_AFH;
		requested_mode = MODE_AFH;

		for(int i=0; i < 10; i++) {
			afh_map[i] = 0;
		}
		used_channels = 0;
		afh_enabled = 1;
		break;

	case UBERTOOTH_HOP:
		do_hop = 1;
		break;

	case UBERTOOTH_SET_CLOCK:
		clock = data[0] | data[1] << 8 | data[2] << 16 | data[3] << 24;
		clkn = clock;
		cs_threshold_calc_and_set(channel);
		break;

	case UBERTOOTH_SET_AFHMAP:
		for(int i=0; i < 10; i++) {
			afh_map[i] = data[i];
		}
		afh_enabled = 1;
		*data_len = 10;
		break;

	case UBERTOOTH_CLEAR_AFHMAP:
		for(int i=0; i < 10; i++) {
			afh_map[i] = 0;
		}
		afh_enabled = 0;
		*data_len = 10;
		break;

	case UBERTOOTH_GET_CLOCK:
		clock = clkn;
		for(int i=0; i < 4; i++) {
			data[i] = (clock >> (8*i)) & 0xff;
		}
		*data_len = 4;
		break;

	case UBERTOOTH_TRIM_CLOCK:
		clk100ns_offset = (data[0] << 8) | (data[1] << 0);
		break;

	case UBERTOOTH_BTLE_SNIFFING:
		*data_len = 0;

		do_hop = 0;
		hop_mode = HOP_BTLE;
		requested_mode = MODE_BT_FOLLOW_LE;

		queue_init();
		cs_threshold_calc_and_set(channel);
		break;

	case UBERTOOTH_GET_ACCESS_ADDRESS:
		for(int i=0; i < 4; i++) {
			data[i] = (le.access_address >> (8*i)) & 0xff;
		}
		*data_len = 4;
		break;

	case UBERTOOTH_SET_ACCESS_ADDRESS:
		le_set_access_address(data[0] | data[1] << 8 | data[2] << 16 | data[3] << 24);
		le.target_set = 1;
		break;

	case UBERTOOTH_DO_SOMETHING:
		// do something! just don't commit anything here
		break;

	case UBERTOOTH_DO_SOMETHING_REPLY:
		// after you do something, tell me what you did!
		// don't commit here please
		data[0] = 0x13;
		data[1] = 0x37;
		*data_len = 2;
		break;

	case UBERTOOTH_GET_CRC_VERIFY:
		data[0] = le.crc_verify ? 1 : 0;
		*data_len = 1;
		break;

	case UBERTOOTH_SET_CRC_VERIFY:
		le.crc_verify = request_params[0] ? 1 : 0;
		break;

	case UBERTOOTH_POLL:
		p = dequeue();
		if (p != NULL) {
			memcpy(data, (void *)p, sizeof(usb_pkt_rx));
			*data_len = sizeof(usb_pkt_rx);
		} else {
			data[0] = 0;
			*data_len = 1;
		}
		break;

	case UBERTOOTH_BTLE_PROMISC:
		*data_len = 0;

		hop_mode = HOP_NONE;
		requested_mode = MODE_BT_PROMISC_LE;

		queue_init();
		cs_threshold_calc_and_set(channel);
		break;

	case UBERTOOTH_READ_REGISTER:
		reg_val = cc2400_get(request_params[0]);
		data[0] = (reg_val >> 8) & 0xff;
		data[1] = reg_val & 0xff;
		*data_len = 2;
		break;

	case UBERTOOTH_WRITE_REGISTER:
		cc2400_set(request_params[0] & 0xff, request_params[1]);
		break;

	case UBERTOOTH_WRITE_REGISTERS:
		for(i=0; i<request_params[0]; i++) {
			reg_val = (data[(i*3)+1] << 8) | data[(i*3)+2];
			cc2400_set(data[i*3], reg_val);
		}
		break;

	case UBERTOOTH_READ_ALL_REGISTERS:
		#define MAX_READ_REG 0x2d
		for(i=0; i<=MAX_READ_REG; i++) {
			reg_val = cc2400_get(i);
			data[i*3] = i;
			data[(i*3)+1] = (reg_val >> 8) & 0xff;
			data[(i*3)+2] = reg_val & 0xff;
		}
		*data_len = MAX_READ_REG*3;
		break;

	case UBERTOOTH_BTLE_SLAVE:
		memcpy(slave_mac_address, data, 6);
		requested_mode = MODE_BT_SLAVE_LE;
		break;

	case UBERTOOTH_BTLE_SET_TARGET:
		// Addresses appear in packets in reverse-octet order.
		// Store the target address in reverse order so that we can do a simple memcmp later
		le.target[0] = data[5];
		le.target[1] = data[4];
		le.target[2] = data[3];
		le.target[3] = data[2];
		le.target[4] = data[1];
		le.target[5] = data[0];
		le.target_set = 1;
		break;

#ifdef TX_ENABLE
	case UBERTOOTH_JAM_MODE:
		jam_mode = request_params[0];
		break;
#endif

	case UBERTOOTH_EGO:
#ifndef TX_ENABLE
		if (ego_mode == EGO_JAM)
			return 0;
#endif
		requested_mode = MODE_EGO;
		ego_mode = request_params[0];
		break;

	case UBERTOOTH_GET_API_VERSION:
		for (int i = 0; i < 4; ++i)
			data[i] = (UBERTOOTH_API_VERSION >> (8*i)) & 0xff;
		*data_len = 4;
		break;

	default:
		return 0;
	}
	return 1;
}

/* Update CLKN. */
void TIMER0_IRQHandler()
{
	if (T0IR & TIR_MR0_Interrupt) {

		clkn += clkn_offset + 1;
		clkn_offset = 0;

		uint32_t le_clk = (clkn - le.conn_epoch) & 0x03;

		/* Trigger hop based on mode */

		/* NONE or SWEEP -> 25 Hz */
		if (hop_mode == HOP_NONE || hop_mode == HOP_SWEEP) {
			if ((clkn & 0x7f) == 0)
				do_hop = 1;
		}
		/* BLUETOOTH -> 1600 Hz */
		else if (hop_mode == HOP_BLUETOOTH) {
			if ((clkn & 0x1) == 0)
				do_hop = 1;
		}
		/* BLUETOOTH Low Energy -> 7.5ms - 4.0s in multiples of 1.25 ms */
		else if (hop_mode == HOP_BTLE) {
			// Only hop if connected
			if (le.link_state == LINK_CONNECTED && le_clk == 0) {
				--le.interval_timer;
				if (le.interval_timer == 0) {
					do_hop = 1;
					++le.conn_count;
					le.interval_timer = le.conn_interval;
				} else {
					TXLED_CLR; // hack!
				}
			}
		}
		else if (hop_mode == HOP_AFH) {
			if( (last_hop + hop_timeout) == clkn ) {
				do_hop = 1;
			}
		}

		if(clk100ns_offset > 3124)
			clkn += 2;

		T0MR0 = 3124 + clk100ns_offset;
		clk100ns_offset = 0;

		/* Ack interrupt */
		T0IR = TIR_MR0_Interrupt;
	}
}

/* EINT3 handler is also defined in ubertooth.c for TC13BADGE. */
#ifndef TC13BADGE
void EINT3_IRQHandler()
{
	/* TODO - check specific source of shared interrupt */
	IO2IntClr   = PIN_GIO6; // clear interrupt
	DIO_SSEL_CLR;           // enable SPI
	cs_trigger  = 1;        // signal trigger
	if (hop_mode == HOP_BLUETOOTH)
		dma_discard = 0;

}
#endif // TC13BADGE

/* Sleep (busy wait) for 'millis' milliseconds. The 'wait' routines in
 * ubertooth.c are matched to the clock setup at boot time and can not
 * be used while the board is running at 100MHz. */
static void msleep(uint32_t millis)
{
	uint32_t stop_at = clkn + millis * 3125 / 1000;  // millis -> clkn ticks
	do { } while (clkn < stop_at);                   // TODO: handle wrapping
}

void DMA_IRQHandler()
{
	if ( mode == MODE_RX_SYMBOLS
	   || mode == MODE_SPECAN
	   || mode == MODE_BT_FOLLOW_LE
	   || mode == MODE_BT_PROMISC_LE
	   || mode == MODE_BT_SLAVE_LE)
	{
		/* interrupt on channel 0 */
		if (DMACIntStat & (1 << 0)) {
			if (DMACIntTCStat & (1 << 0)) {
				DMACIntTCClear = (1 << 0);

				if (hop_mode == HOP_BLUETOOTH)
					DIO_SSEL_SET;

				idle_buf_clk100ns  = CLK100NS;
				idle_buf_clkn_high = (clkn >> 20) & 0xff;
				idle_buf_channel   = channel;

				/* Keep buffer swapping in sync with DMA. */
				volatile uint8_t* tmp = active_rxbuf;
				active_rxbuf = idle_rxbuf;
				idle_rxbuf = tmp;

				++rx_tc;
			}
			if (DMACIntErrStat & (1 << 0)) {
				DMACIntErrClr = (1 << 0);
				++rx_err;
			}
		}
	}
}

static void cc2400_idle()
{
	cc2400_strobe(SRFOFF);
	while ((cc2400_status() & FS_LOCK)); // need to wait for unlock?

#ifdef UBERTOOTH_ONE
	PAEN_CLR;
	HGM_CLR;
#endif

	RXLED_CLR;
	TXLED_CLR;
	USRLED_CLR;

	clkn_stop();
	dio_ssp_stop();
	cs_reset();
	rssi_reset();

	/* hopping stuff */
	hop_mode = HOP_NONE;
	do_hop = 0;
	channel = 2441;
	hop_direct_channel = 0;
	hop_timeout = 158;
	requested_channel = 0;
	saved_request = 0;


	/* bulk USB stuff */
	idle_buf_clkn_high = 0;
	idle_buf_clk100ns = 0;
	idle_buf_channel = 0;
	dma_discard = 0;
	status = 0;

	/* operation mode */
	mode = MODE_IDLE;
	requested_mode = MODE_IDLE;
	jam_mode = JAM_NONE;
	ego_mode = EGO_FOLLOW;

	modulation = MOD_BT_BASIC_RATE;

	/* specan stuff */
	low_freq = 2400;
	high_freq = 2483;
	rssi_threshold = -30;

	target.address = 0;
	target.syncword = 0;
}

/* start un-buffered rx */
static void cc2400_rx()
{
	u16 mdmctrl;
	if (modulation == MOD_BT_BASIC_RATE) {
		mdmctrl = 0x0029; // 160 kHz frequency deviation
	} else if (modulation == MOD_BT_LOW_ENERGY) {
		mdmctrl = 0x0040; // 250 kHz frequency deviation
	} else {
		/* oops */
		return;
	}

	cc2400_set(MANAND,  0x7fff);
	cc2400_set(LMTST,   0x2b22);
	cc2400_set(MDMTST0, 0x134b); // without PRNG
	cc2400_set(GRMDM,   0x0101); // un-buffered mode, GFSK
	// 0 00 00 0 010 00 0 00 0 1
	//      |  | |   |  +--------> CRC off
	//      |  | |   +-----------> sync word: 8 MSB bits of SYNC_WORD
	//      |  | +---------------> 2 preamble bytes of 01010101
	//      |  +-----------------> not packet mode
	//      +--------------------> un-buffered mode
	cc2400_set(FSDIV,   channel - 1); // 1 MHz IF
	cc2400_set(MDMCTRL, mdmctrl);

	// Set up CS register
	cs_threshold_calc_and_set(channel);

	clkn_start();

	while (!(cc2400_status() & XOSC16M_STABLE));
	cc2400_strobe(SFSON);
	while (!(cc2400_status() & FS_LOCK));
	cc2400_strobe(SRX);
#ifdef UBERTOOTH_ONE
	PAEN_SET;
	HGM_SET;
#endif
}

/* start un-buffered rx */
static void cc2400_rx_sync(u32 sync)
{
	u16 grmdm, mdmctrl;

	if (modulation == MOD_BT_BASIC_RATE) {
		mdmctrl = 0x0029; // 160 kHz frequency deviation
		grmdm = 0x0461; // un-buffered mode, packet w/ sync word detection
		// 0 00 00 1 000 11 0 00 0 1
		//   |  |  | |   |  +--------> CRC off
		//   |  |  | |   +-----------> sync word: 32 MSB bits of SYNC_WORD
		//   |  |  | +---------------> 0 preamble bytes of 01010101
		//   |  |  +-----------------> packet mode
		//   |  +--------------------> un-buffered mode
		//   +-----------------------> sync error bits: 0

	} else if (modulation == MOD_BT_LOW_ENERGY) {
		mdmctrl = 0x0040; // 250 kHz frequency deviation
		grmdm = 0x0561; // un-buffered mode, packet w/ sync word detection
		// 0 00 00 1 010 11 0 00 0 1
		//   |  |  | |   |  +--------> CRC off
		//   |  |  | |   +-----------> sync word: 32 MSB bits of SYNC_WORD
		//   |  |  | +---------------> 2 preamble bytes of 01010101
		//   |  |  +-----------------> packet mode
		//   |  +--------------------> un-buffered mode
		//   +-----------------------> sync error bits: 0

	} else {
		/* oops */
		return;
	}

	cc2400_set(MANAND,  0x7fff);
	cc2400_set(LMTST,   0x2b22);

	cc2400_set(MDMTST0, 0x124b);
	// 1      2      4b
	// 00 0 1 0 0 10 01001011
	//    | | | | |  +---------> AFC_DELTA = ??
	//    | | | | +------------> AFC settling = 4 pairs (8 bit preamble)
	//    | | | +--------------> no AFC adjust on packet
	//    | | +----------------> do not invert data
	//    | +------------------> TX IF freq 1 0Hz
	//    +--------------------> PRNG off
	//
	// ref: CC2400 datasheet page 67
	// AFC settling explained page 41/42

	cc2400_set(GRMDM,   grmdm);

	cc2400_set(SYNCL,   sync & 0xffff);
	cc2400_set(SYNCH,   (sync >> 16) & 0xffff);

	cc2400_set(FSDIV,   channel - 1); // 1 MHz IF
	cc2400_set(MDMCTRL, mdmctrl);

	// Set up CS register
	cs_threshold_calc_and_set(channel);

	clkn_start();

	while (!(cc2400_status() & XOSC16M_STABLE));
	cc2400_strobe(SFSON);
	while (!(cc2400_status() & FS_LOCK));
	cc2400_strobe(SRX);
#ifdef UBERTOOTH_ONE
	PAEN_SET;
	HGM_SET;
#endif
}

/* start buffered tx */
static void cc2400_tx_sync(uint32_t sync)
{
#ifdef TX_ENABLE
	// Bluetooth-like modulation
	cc2400_set(MANAND,  0x7fff);
	cc2400_set(LMTST,   0x2b22);    // LNA and receive mixers test register
	cc2400_set(MDMTST0, 0x134b);    // no PRNG

	cc2400_set(GRMDM,   0x0c01);
	// 0 00 01 1 000 00 0 00 0 1
	//      |  | |   |  +--------> CRC off
	//      |  | |   +-----------> sync word: 8 MSB bits of SYNC_WORD
	//      |  | +---------------> 0 preamble bytes of 01010101
	//      |  +-----------------> packet mode
	//      +--------------------> buffered mode

	cc2400_set(SYNCL,   sync & 0xffff);
	cc2400_set(SYNCH,   (sync >> 16) & 0xffff);

	cc2400_set(FSDIV,   channel);
	cc2400_set(FREND,   0b1011);    // amplifier level (-7 dBm, picked from hat)

	if (modulation == MOD_BT_BASIC_RATE) {
		cc2400_set(MDMCTRL, 0x0029);    // 160 kHz frequency deviation
	} else if (modulation == MOD_BT_LOW_ENERGY) {
		cc2400_set(MDMCTRL, 0x0040);    // 250 kHz frequency deviation
	} else {
		/* oops */
		return;
	}

	clkn_start();

	while (!(cc2400_status() & XOSC16M_STABLE));
	cc2400_strobe(SFSON);
	while (!(cc2400_status() & FS_LOCK));

#ifdef UBERTOOTH_ONE
	PAEN_SET;
#endif

	while ((cc2400_get(FSMSTATE) & 0x1f) != STATE_STROBE_FS_ON);
	cc2400_strobe(STX);

#endif
}

/*
 * Transmit a BTLE packet with the specified access address.
 *
 * All modulation parameters are set within this function. The data
 * should not be pre-whitened, but the CRC should be calculated and
 * included in the data length.
 */
void le_transmit(u32 aa, u8 len, u8 *data)
{
	unsigned i, j;
	int bit;
	u8 txbuf[64];
	u8 tx_len;
	u8 byte;
	u16 gio_save;

	// first four bytes: AA
	for (i = 0; i < 4; ++i) {
		byte = aa & 0xff;
		aa >>= 8;
		txbuf[i] = 0;
		for (j = 0; j < 8; ++j) {
			txbuf[i] |= (byte & 1) << (7 - j);
			byte >>= 1;
		}
	}

	// whiten the data and copy it into the txbuf
	int idx = whitening_index[btle_channel_index(channel-2402)];
	for (i = 0; i < len; ++i) {
		byte = data[i];
		txbuf[i+4] = 0;
		for (j = 0; j < 8; ++j) {
			bit = (byte & 1) ^ whitening[idx];
			idx = (idx + 1) % sizeof(whitening);
			byte >>= 1;
			txbuf[i+4] |= bit << (7 - j);
		}
	}

	len += 4; // include the AA in len

	// Bluetooth-like modulation
	cc2400_set(MANAND,  0x7fff);
	cc2400_set(LMTST,   0x2b22);    // LNA and receive mixers test register
	cc2400_set(MDMTST0, 0x134b);    // no PRNG

	cc2400_set(GRMDM,   0x0c01);
	// 0 00 01 1 000 00 0 00 0 1
	//      |  | |   |  +--------> CRC off
	//      |  | |   +-----------> sync word: 8 MSB bits of SYNC_WORD
	//      |  | +---------------> 0 preamble bytes of 01010101
	//      |  +-----------------> packet mode
	//      +--------------------> buffered mode

	cc2400_set(FSDIV,   channel);
	cc2400_set(FREND,   0b1011);    // amplifier level (-7 dBm, picked from hat)
	cc2400_set(MDMCTRL, 0x0040);    // 250 kHz frequency deviation
	cc2400_set(INT,     0x0014);	// FIFO_THRESHOLD: 20 bytes

	// sync byte depends on the first transmitted bit of the AA
	if (aa & 1)
		cc2400_set(SYNCH,   0xaaaa);
	else
		cc2400_set(SYNCH,   0x5555);

	// set GIO to FIFO_FULL
	gio_save = cc2400_get(IOCFG);
	cc2400_set(IOCFG, (GIO_FIFO_FULL << 9) | (gio_save & 0x1ff));

	while (!(cc2400_status() & XOSC16M_STABLE));
	cc2400_strobe(SFSON);
	while (!(cc2400_status() & FS_LOCK));
	TXLED_SET;
#ifdef UBERTOOTH_ONE
	PAEN_SET;
#endif
	while ((cc2400_get(FSMSTATE) & 0x1f) != STATE_STROBE_FS_ON);
	cc2400_strobe(STX);

	// put the packet into the FIFO
	for (i = 0; i < len; i += 16) {
		while (GIO6) ; // wait for the FIFO to drain (FIFO_FULL false)
		tx_len = len - i;
		if (tx_len > 16)
			tx_len = 16;
		cc2400_spi_buf(FIFOREG, tx_len, txbuf + i);
	}

	while ((cc2400_get(FSMSTATE) & 0x1f) != STATE_STROBE_FS_ON);
	TXLED_CLR;

	cc2400_strobe(SRFOFF);
	while ((cc2400_status() & FS_LOCK));

#ifdef UBERTOOTH_ONE
	PAEN_CLR;
#endif

	// reset GIO
	cc2400_set(IOCFG, gio_save);
}

void le_jam(void) {
#ifdef TX_ENABLE
	cc2400_set(MANAND,  0x7fff);
	cc2400_set(LMTST,   0x2b22);    // LNA and receive mixers test register
	cc2400_set(MDMTST0, 0x234b);    // PRNG, 1 MHz offset

	cc2400_set(GRMDM,   0x0c01);
	// 0 00 01 1 000 00 0 00 0 1
	//      |  | |   |  +--------> CRC off
	//      |  | |   +-----------> sync word: 8 MSB bits of SYNC_WORD
	//      |  | +---------------> 0 preamble bytes of 01010101
	//      |  +-----------------> packet mode
	//      +--------------------> buffered mode

	// cc2400_set(FSDIV,   channel);
	cc2400_set(FREND,   0b1011);    // amplifier level (-7 dBm, picked from hat)
	cc2400_set(MDMCTRL, 0x0040);    // 250 kHz frequency deviation

	while (!(cc2400_status() & XOSC16M_STABLE));
	cc2400_strobe(SFSON);
	while (!(cc2400_status() & FS_LOCK));
	TXLED_SET;
#ifdef UBERTOOTH_ONE
	PAEN_SET;
#endif
	while ((cc2400_get(FSMSTATE) & 0x1f) != STATE_STROBE_FS_ON);
	cc2400_strobe(STX);
#endif
}

/* TODO - return whether hop happened, or should caller have to keep
 * track of this? */
void hop(void)
{
	do_hop = 0;
	last_hop = clkn;

	// No hopping, if channel is set correctly, do nothing
	if (hop_mode == HOP_NONE) {
		if (cc2400_get(FSDIV) == (channel - 1))
			return;
	}

	/* Slow sweep (100 hops/sec)
	 * only hop to currently used channels if AFH is enabled
	 */
	else if (hop_mode == HOP_SWEEP) {
		do {
			channel += 32;
			if (channel > 2480)
				channel -= 79;
		} while ( used_channels != 0 && afh_enabled && !( afh_map[(channel-2402)/8] & 0x1<<((channel-2402)%8) ) );
	}

	/* AFH detection
	 * only hop to currently unused channesl
	 */
	else if (hop_mode == HOP_AFH) {
		do {
			channel += 32;
			if (channel > 2480)
				channel -= 79;
		} while( used_channels != 79 && (afh_map[(channel-2402)/8] & 0x1<<((channel-2402)%8)) );
	}

	else if (hop_mode == HOP_BLUETOOTH) {
		channel = next_hop(clkn);
	}

	else if (hop_mode == HOP_BTLE) {
		channel = btle_next_hop(&le);
	}

	else if (hop_mode == HOP_DIRECT) {
		channel = hop_direct_channel;
	}

	/* IDLE mode, but leave amp on, so don't call cc2400_idle(). */
	cc2400_strobe(SRFOFF);
	while ((cc2400_status() & FS_LOCK)); // need to wait for unlock?

	/* Retune */
	if(mode == MODE_TX_SYMBOLS)
		cc2400_set(FSDIV, channel);
	else
		cc2400_set(FSDIV, channel - 1);

	/* Update CS register if hopping.  */
	if (hop_mode > 0) {
		cs_threshold_calc_and_set(channel);
	}

	/* Wait for lock */
	cc2400_strobe(SFSON);
	while (!(cc2400_status() & FS_LOCK));

	dma_discard = 1;

	if(mode == MODE_TX_SYMBOLS)
		cc2400_strobe(STX);
	else
		cc2400_strobe(SRX);
}

/* Bluetooth packet monitoring */
void bt_stream_rx()
{
	int8_t rssi;
	int8_t rssi_at_trigger;

	RXLED_CLR;

	queue_init();
	dio_ssp_init();
	dma_init();
	dio_ssp_start();

	cc2400_rx();

	cs_trigger_enable();

	while ( requested_mode == MODE_RX_SYMBOLS || requested_mode == MODE_BT_FOLLOW )
	{

		RXLED_CLR;

		/* Wait for DMA transfer. TODO - need more work on
		 * RSSI. Should send RSSI indications to host even
		 * when not transferring data. That would also keep
		 * the USB stream going. This loop runs 50-80 times
		 * while waiting for DMA, but RSSI sampling does not
		 * cover all the symbols in a DMA transfer. Can not do
		 * RSSI sampling in CS interrupt, but could log time
		 * at multiple trigger points there. The MAX() below
		 * helps with statistics in the case that cs_trigger
		 * happened before the loop started. */
		rssi_reset();
		rssi_at_trigger = INT8_MIN;
		while (!rx_tc) {
			rssi = (int8_t)(cc2400_get(RSSI) >> 8);
			if (cs_trigger && (rssi_at_trigger == INT8_MIN)) {
				rssi = MAX(rssi,(cs_threshold_cur+54));
				rssi_at_trigger = rssi;
			}
			rssi_add(rssi);

			handle_usb(clkn);

			/* If timer says time to hop, do it. */
			if (do_hop) {
				hop();
			} else {
				TXLED_CLR;
			}
			/* TODO - set per-channel carrier sense threshold.
			 * Set by firmware or host. */
		}

		RXLED_SET;

		if (rx_err) {
			status |= DMA_ERROR;
		}

		/* Missed a DMA trasfer? */
		if (rx_tc > 1)
			status |= DMA_OVERFLOW;

		if (dma_discard) {
			status |= DISCARD;
			dma_discard = 0;
		}

		rssi_iir_update(channel);

		/* Set squelch hold if there was either a CS trigger, squelch
		 * is disabled, or if the current rssi_max is above the same
		 * threshold. Currently, this is redundant, but allows for
		 * per-channel or other rssi triggers in the future. */
		if (cs_trigger || cs_no_squelch) {
			status |= CS_TRIGGER;
			cs_trigger = 0;
		}

		if (rssi_max >= (cs_threshold_cur + 54)) {
			status |= RSSI_TRIGGER;
		}

		enqueue(BR_PACKET, (uint8_t*)idle_rxbuf);

rx_continue:
		handle_usb(clkn);
		rx_tc = 0;
		rx_err = 0;
	}

	/* This call is a nop so far. Since bt_rx_stream() starts the
	 * stream, it makes sense that it would stop it. TODO - how
	 * should setup/teardown be handled? Should every new mode be
	 * starting from scratch? */
	dio_ssp_stop();
	cs_trigger_disable();
}

static uint8_t reverse8(uint8_t data)
{
	uint8_t reversed = 0;

	for(size_t i=0; i<8; i++)
	{
		reversed |= ((data >> i) & 0x01) << (7-i);
	}

	return reversed;
}

static uint16_t reverse16(uint16_t data)
{
	uint16_t reversed = 0;

	for(size_t i=0; i<16; i++)
	{
		reversed |= ((data >> i) & 0x01) << (15-i);
	}

	return reversed;
}

/*
 * Transmit a BTBR packet with the specified access code.
 *
 * All modulation parameters are set within this function.
 */
void br_transmit()
{
	uint16_t gio_save;

	uint32_t clkn_saved;

	uint16_t preamble = (target.syncword & 1) == 1 ? 0x5555 : 0xaaaa;
	uint8_t trailer = ((target.syncword >> 63) & 1) == 1 ? 0xaa : 0x55;

	uint8_t data[16] = {
		reverse8((target.syncword >> 0) & 0xFF),
		reverse8((target.syncword >> 8) & 0xFF),
		reverse8((target.syncword >> 16) & 0xFF),
		reverse8((target.syncword >> 24) & 0xFF),
		reverse8((target.syncword >> 32) & 0xFF),
		reverse8((target.syncword >> 40) & 0xFF),
		reverse8((target.syncword >> 48) & 0xFF),
		reverse8((target.syncword >> 56) & 0xFF),
		reverse8(trailer),
		reverse8(0x77),
		reverse8(0x66),
		reverse8(0x55),
		reverse8(0x44),
		reverse8(0x33),
		reverse8(0x22),
		reverse8(0x11)
	};

	cc2400_tx_sync(reverse16(preamble));

	cc2400_set(INT,     0x0014);    // FIFO_THRESHOLD: 20 bytes

	// set GIO to FIFO_FULL
	gio_save = cc2400_get(IOCFG);
	cc2400_set(IOCFG, (GIO_FIFO_FULL << 9) | (gio_save & 0x1ff));

	while ( requested_mode == MODE_TX_SYMBOLS )
	{

		while ((clkn >> 1) == (clkn_saved >> 1) || T0TC < 2250) {

			// If timer says time to hop, do it.
			if (do_hop) {
				hop();
			}
		}

		clkn_saved = clkn;

		TXLED_SET;

		cc2400_spi_buf(FIFOREG, 16, data);

		while ((cc2400_get(FSMSTATE) & 0x1f) != STATE_STROBE_FS_ON);
		TXLED_CLR;

		cc2400_strobe(SRFOFF);
		while ((cc2400_status() & FS_LOCK));

		while (!(cc2400_status() & XOSC16M_STABLE));
		cc2400_strobe(SFSON);
		while (!(cc2400_status() & FS_LOCK));

		while ((cc2400_get(FSMSTATE) & 0x1f) != STATE_STROBE_FS_ON);
		cc2400_strobe(STX);

		handle_usb(clkn);
	}

#ifdef UBERTOOTH_ONE
	PAEN_CLR;
#endif

	// reset GIO
	cc2400_set(IOCFG, gio_save);
}

/* set LE access address */
static void le_set_access_address(u32 aa) {
	u32 aa_rev;

	le.access_address = aa;
	aa_rev = rbit(aa);
	le.syncl = aa_rev & 0xffff;
	le.synch = aa_rev >> 16;
}

/* reset le state, called by bt_generic_le and bt_follow_le() */
void reset_le() {
	le_set_access_address(0x8e89bed6);     // advertising channel access address
	le.crc_init  = 0x555555;               // advertising channel CRCInit
	le.crc_init_reversed = 0xAAAAAA;
	le.crc_verify = 0;
	le.last_packet = 0;

	le.link_state = LINK_INACTIVE;

	le.channel_idx = 0;
	le.channel_increment = 0;

	le.conn_epoch = 0;
	le.interval_timer = 0;
	le.conn_interval = 0;
	le.conn_interval = 0;
	le.conn_count = 0;

	le.win_size = 0;
	le.win_offset = 0;

	le.update_pending = 0;
	le.update_instant = 0;
	le.interval_update = 0;
	le.win_size_update = 0;
	le.win_offset_update;

	do_hop = 0;
}

// reset LE Promisc state
void reset_le_promisc(void) {
	memset(&le_promisc, 0, sizeof(le_promisc));
	le_promisc.smallest_hop_interval = 0xffffffff;
}

/* generic le mode */
void bt_generic_le(u8 active_mode)
{
	u8 *tmp = NULL;
	u8 hold;
	int i, j;
	int8_t rssi, rssi_at_trigger;

	modulation = MOD_BT_LOW_ENERGY;
	mode = active_mode;

	reset_le();

	// enable USB interrupts
	ISER0 = ISER0_ISE_USB;

	RXLED_CLR;

	queue_init();
	dio_ssp_init();
	dma_init();
	dio_ssp_start();
	cc2400_rx();

	cs_trigger_enable();

	hold = 0;

	while (requested_mode == active_mode) {
		if (requested_channel != 0) {
			cc2400_strobe(SRFOFF);
			while ((cc2400_status() & FS_LOCK)); // need to wait for unlock?

			/* Retune */
			cc2400_set(FSDIV, channel - 1);

			/* Wait for lock */
			cc2400_strobe(SFSON);
			while (!(cc2400_status() & FS_LOCK));

			/* RX mode */
			cc2400_strobe(SRX);

			requested_channel = 0;
		}

		if (do_hop) {
			hop();
		} else {
			TXLED_CLR;
		}

		RXLED_CLR;

		/* Wait for DMA. Meanwhile keep track of RSSI. */
		rssi_reset();
		rssi_at_trigger = INT8_MIN;
		while ((rx_tc == 0) && (rx_err == 0))
		{
			rssi = (int8_t)(cc2400_get(RSSI) >> 8);
			if (cs_trigger && (rssi_at_trigger == INT8_MIN)) {
				rssi = MAX(rssi,(cs_threshold_cur+54));
				rssi_at_trigger = rssi;
			}
			rssi_add(rssi);
		}

		if (rx_err) {
			status |= DMA_ERROR;
		}

		/* No DMA transfer? */
		if (!rx_tc)
			goto rx_continue;

		/* Missed a DMA trasfer? */
		if (rx_tc > 1)
			status |= DMA_OVERFLOW;

		rssi_iir_update(channel);

		/* Set squelch hold if there was either a CS trigger, squelch
		 * is disabled, or if the current rssi_max is above the same
		 * threshold. Currently, this is redundant, but allows for
		 * per-channel or other rssi triggers in the future. */
		if (cs_trigger || cs_no_squelch) {
			status |= CS_TRIGGER;
			hold = CS_HOLD_TIME;
			cs_trigger = 0;
		}

		if (rssi_max >= (cs_threshold_cur + 54)) {
			status |= RSSI_TRIGGER;
			hold = CS_HOLD_TIME;
		}

		/* Hold expired? Ignore data. */
		if (hold == 0) {
			goto rx_continue;
		}
		hold--;

		// copy the previously unpacked symbols to the front of the buffer
		memcpy(unpacked, unpacked + DMA_SIZE*8, DMA_SIZE*8);

		// unpack the new packet to the end of the buffer
		for (i = 0; i < DMA_SIZE; ++i) {
			/* output one byte for each received symbol (0x00 or 0x01) */
			for (j = 0; j < 8; ++j) {
				unpacked[DMA_SIZE*8 + i * 8 + j] = (idle_rxbuf[i] & 0x80) >> 7;
				idle_rxbuf[i] <<= 1;
			}
		}

		int ret = data_cb(unpacked);
		if (!ret) break;

	rx_continue:
		rx_tc = 0;
		rx_err = 0;
	}

	// disable USB interrupts
	ICER0 = ICER0_ICE_USB;

	// reset the radio completely
	cc2400_idle();
	dio_ssp_stop();
	cs_trigger_disable();
}


void bt_le_sync(u8 active_mode)
{
	int i;
	int8_t rssi;
	static int restart_jamming = 0;

	modulation = MOD_BT_LOW_ENERGY;
	mode = active_mode;

	le.link_state = LINK_LISTENING;

	// enable USB interrupts
	ISER0 = ISER0_ISE_USB;

	RXLED_CLR;

	queue_init();
	dio_ssp_init();
	dma_init_le();
	dio_ssp_start();

	cc2400_rx_sync(rbit(le.access_address)); // bit-reversed access address

	while (requested_mode == active_mode) {
		if (requested_channel != 0) {
			cc2400_strobe(SRFOFF);
			while ((cc2400_status() & FS_LOCK)); // need to wait for unlock?

			/* Retune */
			cc2400_set(FSDIV, channel - 1);

			/* Wait for lock */
			cc2400_strobe(SFSON);
			while (!(cc2400_status() & FS_LOCK));

			/* RX mode */
			cc2400_strobe(SRX);

			saved_request = requested_channel;
			requested_channel = 0;
		}

		RXLED_CLR;

		/* Wait for DMA. Meanwhile keep track of RSSI. */
		rssi_reset();
		while ((rx_tc == 0) && (rx_err == 0) && (do_hop == 0) && requested_mode == active_mode)
			;

		rssi = (int8_t)(cc2400_get(RSSI) >> 8);
		rssi_min = rssi_max = rssi;

		if (requested_mode != active_mode) {
			goto cleanup;
		}

		if (rx_err) {
			status |= DMA_ERROR;
		}

		if (do_hop)
			goto rx_flush;

		/* No DMA transfer? */
		if (!rx_tc)
			continue;

		/////////////////////
		// process the packet

		uint32_t packet[48/4+1];
		u8 *p = (u8 *)packet;
		packet[0] = le.access_address;

		const uint32_t *whit = whitening_word[btle_channel_index(channel-2402)];
		for (i = 0; i < 4; i+= 4) {
			uint32_t v = rxbuf1[i+0] << 24
					   | rxbuf1[i+1] << 16
					   | rxbuf1[i+2] << 8
					   | rxbuf1[i+3] << 0;
			packet[i/4+1] = rbit(v) ^ whit[i/4];
		}

		unsigned len = (p[5] & 0x3f) + 2;
		if (len > 39)
			goto rx_flush;

		// transfer the minimum number of bytes from the CC2400
		// this allows us enough time to resume RX for subsequent packets on the same channel
		unsigned total_transfers = ((len + 3) + 4 - 1) / 4;
		if (total_transfers < 11) {
			while (DMACC0DestAddr < (uint32_t)rxbuf1 + 4 * total_transfers && rx_err == 0)
				;
		} else { // max transfers? just wait till DMA's done
			while (DMACC0Config & DMACCxConfig_E && rx_err == 0)
				;
		}
		DIO_SSP_DMACR &= ~SSPDMACR_RXDMAE;

		// strobe SFSON to allow the resync to occur while we process the packet
		cc2400_strobe(SFSON);

		// unwhiten the rest of the packet
		for (i = 4; i < 44; i += 4) {
			uint32_t v = rxbuf1[i+0] << 24
					   | rxbuf1[i+1] << 16
					   | rxbuf1[i+2] << 8
					   | rxbuf1[i+3] << 0;
			packet[i/4+1] = rbit(v) ^ whit[i/4];
		}

		if (le.crc_verify) {
			u32 calc_crc = btle_crcgen_lut(le.crc_init_reversed, p + 4, len);
			u32 wire_crc = (p[4+len+2] << 16)
						 | (p[4+len+1] << 8)
						 | (p[4+len+0] << 0);
			if (calc_crc != wire_crc) // skip packets with a bad CRC
				goto rx_flush;
		}


		RXLED_SET;
		packet_cb((uint8_t *)packet);
		enqueue(LE_PACKET, (uint8_t *)packet);
		le.last_packet = CLK100NS;

	rx_flush:
		// this might happen twice, but it's safe to do so
		cc2400_strobe(SFSON);

		// flush any excess bytes from the SSP's buffer
		DIO_SSP_DMACR &= ~SSPDMACR_RXDMAE;
		while (SSP1SR & SSPSR_RNE) {
			u8 tmp = (u8)DIO_SSP_DR;
		}

		// timeout - FIXME this is an ugly hack
		u32 now = CLK100NS;
		if (now < le.last_packet)
			now += 3276800000; // handle rollover
		if  ( // timeout
			((le.link_state == LINK_CONNECTED || le.link_state == LINK_CONN_PENDING)
			&& (now - le.last_packet > 50000000))
			// jam finished
			|| (le_jam_count == 1)
			)
		{
			reset_le();
			le_jam_count = 0;
			TXLED_CLR;

			if (jam_mode == JAM_ONCE) {
				jam_mode = JAM_NONE;
				requested_mode = MODE_IDLE;
				goto cleanup;
			}

			// go back to promisc if the connection dies
			if (active_mode == MODE_BT_PROMISC_LE)
				goto cleanup;

			le.link_state = LINK_LISTENING;

			cc2400_strobe(SRFOFF);
			while ((cc2400_status() & FS_LOCK));

			/* Retune */
			channel = saved_request != 0 ? saved_request : 2402;
			restart_jamming = 1;
		}

		cc2400_set(SYNCL, le.syncl);
		cc2400_set(SYNCH, le.synch);

		if (do_hop)
			hop();

		// ♪ you can jam but you keep turning off the light ♪
		if (le_jam_count > 0) {
			le_jam();
			--le_jam_count;
		} else {
			/* RX mode */
			dma_init_le();
			dio_ssp_start();

			if (restart_jamming) {
				cc2400_rx_sync(rbit(le.access_address));
				restart_jamming = 0;
			} else {
				// wait till we're in FSLOCK before strobing RX
				while (!(cc2400_status() & FS_LOCK));
				cc2400_strobe(SRX);
			}
		}

		rx_tc = 0;
		rx_err = 0;
	}

cleanup:

	// disable USB interrupts
	ICER0 = ICER0_ICE_USB;

	// reset the radio completely
	cc2400_idle();
	dio_ssp_stop();
	cs_trigger_disable();
}



/* low energy connection following
 * follows a known AA around */
int cb_follow_le() {
	int i, j, k;
	int idx = whitening_index[btle_channel_index(channel-2402)];

	u32 access_address = 0;
	for (i = 0; i < 31; ++i) {
		access_address >>= 1;
		access_address |= (unpacked[i] << 31);
	}

	for (i = 31; i < DMA_SIZE * 8 + 32; i++) {
		access_address >>= 1;
		access_address |= (unpacked[i] << 31);
		if (access_address == le.access_address) {
			for (j = 0; j < 46; ++j) {
				u8 byte = 0;
				for (k = 0; k < 8; k++) {
					int offset = k + (j * 8) + i - 31;
					if (offset >= DMA_SIZE*8*2) break;
					int bit = unpacked[offset];
					if (j >= 4) { // unwhiten data bytes
						bit ^= whitening[idx];
						idx = (idx + 1) % sizeof(whitening);
					}
					byte |= bit << k;
				}
				idle_rxbuf[j] = byte;
			}

			// verify CRC
			if (le.crc_verify) {
				int len		 = (idle_rxbuf[5] & 0x3f) + 2;
				u32 calc_crc = btle_crcgen_lut(le.crc_init_reversed, (uint8_t*)idle_rxbuf + 4, len);
				u32 wire_crc = (idle_rxbuf[4+len+2] << 16)
							 | (idle_rxbuf[4+len+1] << 8)
							 |  idle_rxbuf[4+len+0];
				if (calc_crc != wire_crc) // skip packets with a bad CRC
					break;
			}

			// send to PC
			enqueue(LE_PACKET, (uint8_t*)idle_rxbuf);
			RXLED_SET;

			packet_cb((uint8_t*)idle_rxbuf);

			break;
		}
	}

	return 1;
}

/**
 * Called when we receive a packet in connection following mode.
 */
void connection_follow_cb(u8 *packet) {
	int i;
	u32 aa = 0;

#define ADV_ADDRESS_IDX 0
#define HEADER_IDX 4
#define DATA_LEN_IDX 5
#define DATA_START_IDX 6

	u8 *adv_addr = &packet[ADV_ADDRESS_IDX];
	u8 header = packet[HEADER_IDX];
	u8 *data_len = &packet[DATA_LEN_IDX];
	u8 *data = &packet[DATA_START_IDX];
	u8 *crc = &packet[DATA_START_IDX + *data_len];

	if (le.link_state == LINK_CONN_PENDING) {
		// We received a packet in the connection pending state, so now the device *should* be connected
		le.link_state = LINK_CONNECTED;
		le.conn_epoch = clkn;
		le.interval_timer = le.conn_interval - 1;
		le.conn_count = 0;
		le.update_pending = 0;

		// hue hue hue
		if (jam_mode != JAM_NONE)
			le_jam_count = JAM_COUNT_DEFAULT;

	} else if (le.link_state == LINK_CONNECTED) {
		u8 llid =  header & 0x03;

		// Apply any connection parameter update if necessary
		if (le.update_pending && le.conn_count == le.update_instant) {
			// This is the first packet received in the connection interval for which the new parameters apply
			le.conn_epoch = clkn;
			le.conn_interval = le.interval_update;
			le.interval_timer = le.interval_update - 1;
			le.win_size = le.win_size_update;
			le.win_offset = le.win_offset_update;
			le.update_pending = 0;
		}

		if (llid == 0x03 && data[0] == 0x00) {
			// This is a CONNECTION_UPDATE_REQ.
			// The host is changing the connection parameters.
			le.win_size_update = packet[7];
			le.win_offset_update = packet[8] + ((u16)packet[9] << 8);
			le.interval_update = packet[10] + ((u16)packet[11] << 8);
			le.update_instant = packet[16] + ((u16)packet[17] << 8);
			if (le.update_instant - le.conn_count < 32767)
				le.update_pending = 1;
		}

	} else if (le.link_state == LINK_LISTENING) {
		u8 pkt_type = packet[4] & 0x0F;
		if (pkt_type == 0x05) {
			// This is a connect packet
			// if we have a target, see if InitA or AdvA matches
			if (le.target_set &&
				memcmp(le.target, &packet[6], 6) &&  // Target address doesn't match Initiator.
				memcmp(le.target, &packet[12], 6)) {  // Target address doesn't match Advertiser.
				return;
			}

			le.link_state = LINK_CONN_PENDING;
			le.crc_verify = 0; // we will drop many packets if we attempt to filter by CRC

			for (i = 0; i < 4; ++i)
				aa |= packet[18+i] << (i*8);
			le_set_access_address(aa);

#define CRC_INIT (2+4+6+6+4)
			le.crc_init = (packet[CRC_INIT+2] << 16)
						| (packet[CRC_INIT+1] << 8)
						|  packet[CRC_INIT+0];
			le.crc_init_reversed = rbit(le.crc_init);

#define WIN_SIZE (2+4+6+6+4+3)
			le.win_size = packet[WIN_SIZE];

#define WIN_OFFSET (2+4+6+6+4+3+1)
			le.win_offset = packet[WIN_OFFSET];

#define CONN_INTERVAL (2+4+6+6+4+3+1+2)
			le.conn_interval = packet[CONN_INTERVAL];

#define CHANNEL_INC (2+4+6+6+4+3+1+2+2+2+2+5)
			le.channel_increment = packet[CHANNEL_INC] & 0x1f;
			le.channel_idx = le.channel_increment;

			// Hop to the initial channel immediately
			do_hop = 1;
		}
	}
}

void bt_follow_le() {
	reset_le();
	packet_cb = connection_follow_cb;
	bt_le_sync(MODE_BT_FOLLOW_LE);

	/* old non-sync mode
	data_cb = cb_follow_le;
	packet_cb = connection_follow_cb;
	bt_generic_le(MODE_BT_FOLLOW_LE);
	*/

	mode = MODE_IDLE;
}

// issue state change message
void le_promisc_state(u8 type, void *data, unsigned len) {
	u8 buf[50] = { 0, };
	if (len > 49)
		len = 49;

	buf[0] = type;
	memcpy(&buf[1], data, len);
	enqueue(LE_PROMISC, (uint8_t*)buf);
}

// divide, rounding to the nearest integer: round up at 0.5.
#define DIVIDE_ROUND(N, D) ((N) + (D)/2) / (D)

void promisc_recover_hop_increment(u8 *packet) {
	static u32 first_ts = 0;
	if (channel == 2404) {
		first_ts = CLK100NS;
		hop_direct_channel = 2406;
		do_hop = 1;
	} else if (channel == 2406) {
		u32 second_ts = CLK100NS;
		if (second_ts < first_ts)
			second_ts += 3276800000; // handle rollover
		// Number of channels hopped between previous and current timestamp.
		u32 channels_hopped = DIVIDE_ROUND(second_ts - first_ts,
										   le.conn_interval * LE_BASECLK);
		if (channels_hopped < 37) {
			// Get the hop increment based on the number of channels hopped.
			le.channel_increment = hop_interval_lut[channels_hopped];
			le.interval_timer = le.conn_interval / 2;
			le.conn_count = 0;
			le.conn_epoch = 0;
			do_hop = 0;
			// Move on to regular connection following.
			le.channel_idx = (1 + le.channel_increment) % 37;
			le.link_state = LINK_CONNECTED;
			le.crc_verify = 0;
			hop_mode = HOP_BTLE;
			packet_cb = connection_follow_cb;
			le_promisc_state(3, &le.channel_increment, 1);

			if (jam_mode != JAM_NONE)
				le_jam_count = JAM_COUNT_DEFAULT;

			return;
		}
		hop_direct_channel = 2404;
		do_hop = 1;
	}
	else {
		hop_direct_channel = 2404;
		do_hop = 1;
	}
}

void promisc_recover_hop_interval(u8 *packet) {
	static u32 prev_clk = 0;

	u32 cur_clk = CLK100NS;
	if (cur_clk < prev_clk)
		cur_clk += 3267800000; // handle rollover
	u32 clk_diff = cur_clk - prev_clk;
	u16 obsv_hop_interval; // observed hop interval

	// probably consecutive data packets on the same channel
	if (clk_diff < 2 * LE_BASECLK)
		return;

	if (clk_diff < le_promisc.smallest_hop_interval)
		le_promisc.smallest_hop_interval = clk_diff;

	obsv_hop_interval = DIVIDE_ROUND(le_promisc.smallest_hop_interval, 37 * LE_BASECLK);

	if (le.conn_interval == obsv_hop_interval) {
		// 5 consecutive hop intervals: consider it legit and move on
		++le_promisc.consec_intervals;
		if (le_promisc.consec_intervals == 5) {
			packet_cb = promisc_recover_hop_increment;
			hop_direct_channel = 2404;
			hop_mode = HOP_DIRECT;
			do_hop = 1;
			le_promisc_state(2, &le.conn_interval, 2);
		}
	} else {
		le.conn_interval = obsv_hop_interval;
		le_promisc.consec_intervals = 0;
	}

	prev_clk = cur_clk;
}

void promisc_follow_cb(u8 *packet) {
	int i;

	// get the CRCInit
	if (!le.crc_verify && packet[4] == 0x01 && packet[5] == 0x00) {
		u32 crc = (packet[8] << 16) | (packet[7] << 8) | packet[6];

		le.crc_init = btle_reverse_crc(crc, packet + 4, 2);
		le.crc_init_reversed = 0;
		for (i = 0; i < 24; ++i)
			le.crc_init_reversed |= ((le.crc_init >> i) & 1) << (23 - i);

		le.crc_verify = 1;
		packet_cb = promisc_recover_hop_interval;
		le_promisc_state(1, &le.crc_init, 3);
	}
}

// called when we see an AA, add it to the list
void see_aa(u32 aa) {
	int i, max = -1, killme = -1;
	for (i = 0; i < AA_LIST_SIZE; ++i)
		if (le_promisc.active_aa[i].aa == aa) {
			++le_promisc.active_aa[i].count;
			return;
		}

	// evict someone
	for (i = 0; i < AA_LIST_SIZE; ++i)
		if (le_promisc.active_aa[i].count < max || max < 0) {
			killme = i;
			max = le_promisc.active_aa[i].count;
		}

	le_promisc.active_aa[killme].aa = aa;
	le_promisc.active_aa[killme].count = 1;
}

/* le promiscuous mode */
int cb_le_promisc(char *unpacked) {
	int i, j, k;
	int idx;

	// empty data PDU: 01 00
	char desired[4][16] = {
		{ 1, 0, 0, 0, 0, 0, 0, 0,
		  0, 0, 0, 0, 0, 0, 0, 0, },
		{ 1, 0, 0, 1, 0, 0, 0, 0,
		  0, 0, 0, 0, 0, 0, 0, 0, },
		{ 1, 0, 1, 0, 0, 0, 0, 0,
		  0, 0, 0, 0, 0, 0, 0, 0, },
		{ 1, 0, 1, 1, 0, 0, 0, 0,
		  0, 0, 0, 0, 0, 0, 0, 0, },
	};

	for (i = 0; i < 4; ++i) {
		idx = whitening_index[btle_channel_index(channel-2402)];

		// whiten the desired data
		for (j = 0; j < (int)sizeof(desired[i]); ++j) {
			desired[i][j] ^= whitening[idx];
			idx = (idx + 1) % sizeof(whitening);
		}
	}

	// then look for that bitsream in our receive buffer
	for (i = 32; i < (DMA_SIZE*8*2 - 32 - 16); i++) {
		int ok[4] = { 1, 1, 1, 1 };
		int matching = -1;

		for (j = 0; j < 4; ++j) {
			for (k = 0; k < (int)sizeof(desired[j]); ++k) {
				if (unpacked[i+k] != desired[j][k]) {
					ok[j] = 0;
					break;
				}
			}
		}

		// see if any match
		for (j = 0; j < 4; ++j) {
			if (ok[j]) {
				matching = j;
				break;
			}
		}

		// skip if no match
		if (matching < 0)
			continue;

		// found a match! unwhiten it and send it home
		idx = whitening_index[btle_channel_index(channel-2402)];
		for (j = 0; j < 4+3+3; ++j) {
			u8 byte = 0;
			for (k = 0; k < 8; k++) {
				int offset = k + (j * 8) + i - 32;
				if (offset >= DMA_SIZE*8*2) break;
				int bit = unpacked[offset];
				if (j >= 4) { // unwhiten data bytes
					bit ^= whitening[idx];
					idx = (idx + 1) % sizeof(whitening);
				}
				byte |= bit << k;
			}
			idle_rxbuf[j] = byte;
		}

		u32 aa = (idle_rxbuf[3] << 24) |
				 (idle_rxbuf[2] << 16) |
				 (idle_rxbuf[1] <<  8) |
				 (idle_rxbuf[0]);
		see_aa(aa);

		enqueue(LE_PACKET, (uint8_t*)idle_rxbuf);

	}

	// once we see an AA 5 times, start following it
	for (i = 0; i < AA_LIST_SIZE; ++i) {
		if (le_promisc.active_aa[i].count > 3) {
			le_set_access_address(le_promisc.active_aa[i].aa);
			data_cb = cb_follow_le;
			packet_cb = promisc_follow_cb;
			le.crc_verify = 0;
			le_promisc_state(0, &le.access_address, 4);
			// quit using the old stuff and switch to sync mode
			return 0;
		}
	}

	return 1;
}

void bt_promisc_le() {
	while (requested_mode == MODE_BT_PROMISC_LE) {
		reset_le_promisc();

		// jump to a random data channel and turn up the squelch
		if ((channel & 1) == 1)
			channel = 2440;

		// if the PC hasn't given us AA, determine by listening
		if (!le.target_set) {
			// cs_threshold_req = -80;
			cs_threshold_calc_and_set(channel);
			data_cb = cb_le_promisc;
			bt_generic_le(MODE_BT_PROMISC_LE);
		}

		// could have got mode change in middle of above
		if (requested_mode != MODE_BT_PROMISC_LE)
			break;

		le_promisc_state(0, &le.access_address, 4);
		packet_cb = promisc_follow_cb;
		le.crc_verify = 0;
		bt_le_sync(MODE_BT_PROMISC_LE);
	}
}

void bt_slave_le() {
	u32 calc_crc;
	int i;

	u8 adv_ind[] = {
		// LL header
		0x00, 0x09,

		// advertising address
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff,

		// advertising data
		0x02, 0x01, 0x05,

		// CRC (calc)
		0xff, 0xff, 0xff,
	};

	u8 adv_ind_len = sizeof(adv_ind) - 3;

	// copy the user-specified mac address
	for (i = 0; i < 6; ++i)
		adv_ind[i+2] = slave_mac_address[5-i];

	calc_crc = btle_calc_crc(le.crc_init_reversed, adv_ind, adv_ind_len);
	adv_ind[adv_ind_len+0] = (calc_crc >>  0) & 0xff;
	adv_ind[adv_ind_len+1] = (calc_crc >>  8) & 0xff;
	adv_ind[adv_ind_len+2] = (calc_crc >> 16) & 0xff;

	// spam advertising packets
	while (requested_mode == MODE_BT_SLAVE_LE) {
		ICER0 = ICER0_ICE_USB;
		ICER0 = ICER0_ICE_DMA;
		le_transmit(0x8e89bed6, adv_ind_len+3, adv_ind);
		ISER0 = ISER0_ISE_USB;
		ISER0 = ISER0_ISE_DMA;
		msleep(100);
	}
}

/* spectrum analysis */
void specan()
{
	u16 f;
	u8 i = 0;
	u8 buf[DMA_SIZE];

	RXLED_SET;

	queue_init();
	clkn_start();

#ifdef UBERTOOTH_ONE
	PAEN_SET;
	//HGM_SET;
#endif
	cc2400_set(LMTST,   0x2b22);
	cc2400_set(MDMTST0, 0x134b); // without PRNG
	cc2400_set(GRMDM,   0x0101); // un-buffered mode, GFSK
	cc2400_set(MDMCTRL, 0x0029); // 160 kHz frequency deviation
	//FIXME maybe set RSSI.RSSI_FILT
	while (!(cc2400_status() & XOSC16M_STABLE));
	while ((cc2400_status() & FS_LOCK));

	while (requested_mode == MODE_SPECAN) {
		for (f = low_freq; f < high_freq + 1; f++) {
			cc2400_set(FSDIV, f - 1);
			cc2400_strobe(SFSON);
			while (!(cc2400_status() & FS_LOCK));
			cc2400_strobe(SRX);

			/* give the CC2400 time to acquire RSSI reading */
			volatile u32 j = 500; while (--j); //FIXME crude delay
			buf[3 * i] = (f >> 8) & 0xFF;
			buf[(3 * i) + 1] = f  & 0xFF;
			buf[(3 * i) + 2] = cc2400_get(RSSI) >> 8;
			i++;
			if (i == 16) {
				enqueue(SPECAN, buf);
				i = 0;

				handle_usb(clkn);
			}

			cc2400_strobe(SRFOFF);
			while ((cc2400_status() & FS_LOCK));
		}
	}
	RXLED_CLR;
}

/* LED based spectrum analysis */
void led_specan()
{
	int8_t lvl;
	u8 i = 0;
	u16 channels[3] = {2412, 2437, 2462};
	//void (*set[3]) = {TXLED_SET, RXLED_SET, USRLED_SET};
	//void (*clr[3]) = {TXLED_CLR, RXLED_CLR, USRLED_CLR};

#ifdef UBERTOOTH_ONE
	PAEN_SET;
	//HGM_SET;
#endif
	cc2400_set(LMTST,   0x2b22);
	cc2400_set(MDMTST0, 0x134b); // without PRNG
	cc2400_set(GRMDM,   0x0101); // un-buffered mode, GFSK
	cc2400_set(MDMCTRL, 0x0029); // 160 kHz frequency deviation
	cc2400_set(RSSI,    0x00F1); // RSSI Sample over 2 symbols

	while (!(cc2400_status() & XOSC16M_STABLE));
	while ((cc2400_status() & FS_LOCK));

	while (requested_mode == MODE_LED_SPECAN) {
		cc2400_set(FSDIV, channels[i] - 1);
		cc2400_strobe(SFSON);
		while (!(cc2400_status() & FS_LOCK));
		cc2400_strobe(SRX);

		/* give the CC2400 time to acquire RSSI reading */
		volatile u32 j = 500; while (--j); //FIXME crude delay
		lvl = (int8_t)((cc2400_get(RSSI) >> 8) & 0xff);
		if (lvl > rssi_threshold) {
			switch (i) {
				case 0:
					TXLED_SET;
					break;
				case 1:
					RXLED_SET;
					break;
				case 2:
					USRLED_SET;
					break;
			}
		}
		else {
			switch (i) {
				case 0:
					TXLED_CLR;
					break;
				case 1:
					RXLED_CLR;
					break;
				case 2:
					USRLED_CLR;
					break;
			}
		}

		i = (i+1) % 3;

		handle_usb(clkn);

		cc2400_strobe(SRFOFF);
		while ((cc2400_status() & FS_LOCK));
	}
}

int main()
{
	ubertooth_init();
	clkn_init();
	ubertooth_usb_init(vendor_request_handler);
	cc2400_idle();

	while (1) {
		handle_usb(clkn);
		if(requested_mode != mode) {
			switch (requested_mode) {
				case MODE_RESET:
					/* Allow time for the USB command to return correctly */
					wait(1);
					reset();
					break;
				case MODE_AFH:
					mode = MODE_AFH;
					bt_stream_rx();
					break;
				case MODE_RX_SYMBOLS:
					mode = MODE_RX_SYMBOLS;
					queue_init();
					bt_stream_rx();
					break;
				case MODE_TX_SYMBOLS:
					mode = MODE_TX_SYMBOLS;
					br_transmit();
					break;
				case MODE_BT_FOLLOW:
					mode = MODE_BT_FOLLOW;
					bt_stream_rx();
					break;
				case MODE_BT_FOLLOW_LE:
					bt_follow_le();
					break;
				case MODE_BT_PROMISC_LE:
					bt_promisc_le();
					break;
				case MODE_BT_SLAVE_LE:
					bt_slave_le();
					break;
				case MODE_TX_TEST:
					mode = MODE_TX_TEST;
					cc2400_txtest(&modulation, &channel);
					break;
				case MODE_RANGE_TEST:
					mode = MODE_RANGE_TEST;
					cc2400_rangetest(&channel);
					requested_mode = MODE_IDLE;
					break;
				case MODE_REPEATER:
					mode = MODE_REPEATER;
					cc2400_repeater(&channel);
					break;
				case MODE_SPECAN:
					specan();
					break;
				case MODE_LED_SPECAN:
					led_specan();
					break;
				case MODE_EGO:
					mode = MODE_EGO;
					ego_main(ego_mode);
					break;
				case MODE_IDLE:
					cc2400_idle();
					break;
				default:
					/* This is really an error state, but what can you do? */
					break;
			}
		}
	}
}

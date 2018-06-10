#include "cart_bus.h"
#include "cart_emu.h"
#include "vram_m7.h"
#include "Mode7.h"

#define CHARS_PER_ROW 48

void emulate_mode7(void) {
	uint16_t addr, data, c;
	__disable_irq();	// Disable interrupts
	cart_d5xx[0xDE] = 'X';
	while (1) {
		// wait for phi2 high
		while (!((c = CONTROL_IN) & PHI2)) ;

		addr = ADDR_IN;
		if (!(c & CCTL)) {
			// CCTL low
			addr &= 0xFF;
			if (c & RW) {
				// read (control)
				SET_DATA_MODE_OUT
				DATA_OUT = ((uint16_t)(cart_d5xx[addr])) << 8;
				// wait for phi2 low
				while (CONTROL_IN & PHI2) ;
				SET_DATA_MODE_IN
			} else {
				data = DATA_IN;
				// read data bus on falling edge of phi2
				while (CONTROL_IN & PHI2)
					data = DATA_IN;
				cart_d5xx[addr] = data >> 8;
				if (addr == 0xDF)	// write to $D5DF
					break;
			}
		}
		else if (!(c & S5)) {
//			if (c & RW) {
				SET_DATA_MODE_OUT
				//addr = ADDR_IN;
				DATA_OUT = ((uint16_t)(ScreenRAM[addr])) << 8;
				// wait for phi2 low
				while (CONTROL_IN & PHI2) ;
				SET_DATA_MODE_IN
/*			} else {
				//addr = ADDR_IN;
				data = DATA_IN;
				// read data bus on falling edge of phi2
				while (CONTROL_IN & PHI2)
					data = DATA_IN;
				ScreenRAM[addr] = data >> 8;
			}
*/		}
		else if (!(c & S4)) {
			if (c & RW) {
				SET_DATA_MODE_OUT
				//addr = ADDR_IN;
				DATA_OUT = ((uint16_t)(VRAM[addr])) << 8;
				// wait for phi2 low
				while (CONTROL_IN & PHI2) ;
				SET_DATA_MODE_IN
			} else {
				//addr = ADDR_IN;
				data = DATA_IN;
				// read data bus on falling edge of phi2
				while (CONTROL_IN & PHI2)
					data = DATA_IN;
				VRAM[addr] = data >> 8;
			}
		}
	}
	__enable_irq();
}

static void uno_Mode7_cmd(void) {
	uint8_t cmd = cart_d5xx[0xDF];
	switch (cmd) {
	// rebuild the screen from VRAM
	case CART_CMD_REFRESH_M7_SCREEN:
		refresh_m7_vram();
		break;
	case CART_CMD_LOAD_M7_VRAM:
		init_m7_vram();
		ScreenRAM[0x1FFD] = 0xFF;
		break;
	case CART_CMD_PROCESS_MOVEMENT:
		refresh_m7_process(cart_d5xx[0xDC], cart_d5xx[0xDD]);
		break;
	}
}

void emulate_Mode7_vram(void) {
	GREEN_LED_ON
	RD5_HIGH
	RD4_HIGH
	while (1) {
		emulate_mode7();
		RED_LED_OFF
		uno_Mode7_cmd();
		RED_LED_ON
	}
}






#ifndef _Z_FTDI_H_
#define _Z_FTDI_H_


#include <linux/module.h>
#include <linux/kernel.h>

#include "device.h"




#define FTDI_DEVICE_OUT_REQTYPE         (0x02 << 5) | 0x00 | 0x00
#define FTDI_DEVICE_IN_REQTYPE          (0x02 << 5) | 0x00 | 0x80



#define FTDI_REQ_RESET                  0x00
#define FTDI_REQ_SET_LATENCY_TIMER      0x09
#define FTDI_REQ_GET_LATENCY_TIMER      0x0A
#define FTDI_REQ_SET_BITMODE            0x0B
#define FTDI_SIO_READ_EEPROM_REQUEST    0x90
#define FTDI_SIO_WRITE_EEPROM_REQUEST   0x91



#define FTDI_BITMODE_RESET              0x00
#define FTDI_BITMODE_SYNC_FF            0x40



int  ftdi_set_bit_mode       (struct z_device_ctx* ctx, uint8_t mode, uint8_t mask);
int  ftdi_set_latency_timer  (struct z_device_ctx* ctx, uint8_t latency);
int  ftdi_get_eeprom (struct z_device_ctx* ctx, uint8_t eeprom_addr, unsigned short* eeprom_val);
int  ftdi_set_eeprom (struct z_device_ctx* ctx, uint8_t eeprom_addr, unsigned short eeprom_val);

#endif


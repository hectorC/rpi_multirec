




#include "ftdi.h"




int ftdi_set_bit_mode(struct z_device_ctx* ctx, uint8_t mode, uint8_t mask)
{
    int     res;

    
    res  = usb_control_msg(
        ctx->usb.dev,
        usb_sndctrlpipe(ctx->usb.dev, 0),
        FTDI_REQ_SET_BITMODE,
        FTDI_DEVICE_OUT_REQTYPE,
        (mode << 8) | mask,
        1,
        NULL, 0,
        1*HZ);

    return res;
}


int ftdi_set_latency_timer(struct z_device_ctx* ctx, uint8_t latency)
{
    int     res;

    
    if(latency < 1) {
        return -1;
    }

    
    res  = usb_control_msg(
        ctx->usb.dev,
        usb_sndctrlpipe(ctx->usb.dev, 0),
        FTDI_REQ_SET_LATENCY_TIMER,
        FTDI_DEVICE_OUT_REQTYPE,
        latency,
        1,
        NULL, 0,
        1*HZ);

    return res;
}

int ftdi_get_eeprom(struct z_device_ctx* ctx, uint8_t eeprom_addr, unsigned short* eeprom_val){
    unsigned char* buf = kzalloc(2, GFP_KERNEL | GFP_DMA);

    int res  = usb_control_msg(
        ctx->usb.dev,
        usb_rcvctrlpipe(ctx->usb.dev, 0),
        FTDI_SIO_READ_EEPROM_REQUEST,
        FTDI_DEVICE_IN_REQTYPE,
        0,
        eeprom_addr,
        (char*) buf, 2,
        1*HZ);

    (*eeprom_val) = buf[0] | (buf[1] << 8);
    kfree(buf);
    return res;
}

int ftdi_set_eeprom(struct z_device_ctx* ctx, uint8_t eeprom_addr, unsigned short eeprom_val){
    int res  = usb_control_msg(
        ctx->usb.dev,
        usb_rcvctrlpipe(ctx->usb.dev, 0),
        FTDI_SIO_WRITE_EEPROM_REQUEST,
        FTDI_DEVICE_OUT_REQTYPE,
        eeprom_val,
        eeprom_addr,
        NULL, 0,
        1*HZ);

    return res;
}

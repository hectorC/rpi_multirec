





#ifndef Z_IOCTL_H
#define Z_IOCTL_H

#include "defs.h"

#include <linux/ioctl.h>




#define Z_IOCTL_NODE_PREFIX             "/dev/zylia-zm-1_"


#define Z_IOCTL_BUFFER_SIZE             256



#define Z_IOCTL_Q_DRIVER_VERSION        0x00000000


#define Z_IOCTL_Q_DEVICE_MODEL          0x00000010


#define Z_IOCTL_Q_DEVICE_SERIAL         0x00000020



#define Z_IOCTL_Q_FPGA_FW_VERSION       0x00000100



#define Z_IOCTL_Q_LED_BRIGHTNESS        0x00000200


#define Z_IOCTL_S_LED_BRIGHTNESS        0x00000201


#define Z_IOCTL_Q_LED_CONTROL_MODE      0x00000202


#define Z_IOCTL_S_LED_CONTROL_MODE      0x00000203


#define Z_IOCTL_S_LED_COLOR             0x00000205


#define Z_IOCTL_Q_FTDI_EEPROM           0x00000206


#define Z_IOCTL_S_FTDI_EEPROM           0x00000207


#define Z_IOCTL_S_SKIP_CORRECTIONS      0x00000208


#define Z_IOCTL_Q_SKIP_CORRECTIONS      0x00000209


#define Z_IOCTL_PRINT                   0x00000300

#define Z_IOCTL_S_GAIN                  _IOW('Z', 20, uint32_t*)
#define Z_IOCTL_Q_GAIN                  _IOR('Z', 21, uint32_t*)

#define Z_IOCTL_S_TESTMODE              _IOW('Z', 22, uint8_t*)
#define Z_IOCTL_Q_TESTMODE              _IOR('Z', 23, uint8_t*)


#endif 








#include "module.h"
#include "version.h"
#include "chardev.h"


MODULE_LICENSE("GPL");                                  
MODULE_AUTHOR("ZYLIA sp. z o.o.");                      
MODULE_DESCRIPTION("ZYLIA ZM-1 driver");                




static int __init z_module_init(void)
{
	int i, res;

    PRINT("Module "Z_DRIVER_NAME" inserted");
    PRINT(Z_DRIVER_NAME" version: '"Z_DRIVER_VERSION_STRING"'");

    
    res = z_chr_register_class();
    if(res != 0) return res;
    
    
    res = z_usb_driver_register();
    if(res != 0) return res;
    
    
#if (Z_DEBUG_INIT == 1)
    PRINT("USB driver registered");
#endif

	return 0;
}


static void __exit z_module_exit(void)
{    
    
    z_usb_driver_unregister();

    
    z_chr_unregister_class();

    
#if (Z_DEBUG_INIT == 1)
    PRINT("USB driver unregistered");
#endif

    PRINT("Module "Z_DRIVER_NAME" removed");
}




module_init(z_module_init);    
module_exit(z_module_exit);    


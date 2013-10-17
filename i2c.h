
#ifndef _I2C_H_
#define _I2C_H_

int i2c_bus_open();
int i2c_bus_close();
int i2c_cmd_read(int addr, unsigned char cmd, unsigned char buffer[], int length);
int i2c_cmd_write(int addr, unsigned char cmd, unsigned char buffer[], int length);

#endif

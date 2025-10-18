/*
 * Simple I2C test utility for QNX 6.6
 * Usage: i2ctest <device> <slave_addr> [register]
 * Example: i2ctest /dev/i2c1 0x24 0x13
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <hw/i2c.h>

///////////////////////////////////////////////////////////////////////////////////
///////////////Part 1: general definitions and functions///////////////////////////
///////////////////////////////////////////////////////////////////////////////////


// Device scan result
typedef struct {
    unsigned char address;
    int present;
    int readable;
    int writable;
    char device_type[32];
} i2c_device_info_t;

// Bus configuration
typedef struct {
    char device_path[64];    // e.g., "/dev/i2c1"
    int fd;                  // File descriptor
    unsigned int bus_speed;      // Bus speed in Hz
    int initialized;        // Initialization status
} i2c_bus_config_t;

int i2c_init_bus(const char* device_path, i2c_bus_config_t* bus_config) {
    // Open I2C bus device
    bus_config->fd = open(device_path, O_RDWR);
    if (bus_config->fd < 0) {
        printf("Failed to open %s: %s\n", device_path, strerror(errno));
        return -1;
    }

    // Set bus speed
    bus_config->bus_speed = 100000;  // 100kHz
    int status = devctl(bus_config->fd, DCMD_I2C_SET_BUS_SPEED,
                       &bus_config->bus_speed, sizeof(unsigned int), NULL);
    if (status != EOK) {
        printf("Failed to set bus speed: %d\n", status);
        close(bus_config->fd);
        return -1;
    }

    // Verify driver info
    i2c_driver_info_t driver_info;
    status = devctl(bus_config->fd, DCMD_I2C_DRIVER_INFO,
                   &driver_info, sizeof(driver_info), NULL);
    if (status != EOK) {
        printf("Failed to get driver info: %d\n", status);
        close(bus_config->fd);
        return -1;
    }

    printf("I2C bus %s initialized: speed=%dHz, addr_mode=0x%02x\n",
           device_path, bus_config->bus_speed, driver_info.addr_mode);

    bus_config->initialized = 1;
    strncpy(bus_config->device_path, device_path, sizeof(bus_config->device_path)-1);

    return 0;
}
int i2c_probe_device(i2c_bus_config_t* bus_config, unsigned char slave_addr) {

	// Validate address range
    if (slave_addr < 0x08 || slave_addr > 0x77) {
        return 0;  // Invalid or reserved address
    }

    struct {
        i2c_recv_t header;
        unsigned char data[1];
    } recv_packet;

    // Setup receive packet
    recv_packet.header.slave.addr = slave_addr;
    recv_packet.header.slave.fmt = I2C_ADDRFMT_7BIT;
    recv_packet.header.len = 1;
    recv_packet.header.stop = 1;

    // Attempt to read 1 byte (device probe)
    int status = devctl(bus_config->fd, DCMD_I2C_RECV,
                       &recv_packet, sizeof(recv_packet), NULL);

    // Device present if no error or specific acceptable errors
    return (status == EOK || status == ETIMEDOUT);
}
int i2c_read_register(i2c_bus_config_t* bus_config, unsigned char slave_addr,
                     unsigned char reg_addr, unsigned char* data, int len) {
    // First send register address
    struct {
        i2c_send_t header;
        unsigned char reg_addr;
    } send_packet;

    send_packet.header.slave.addr = slave_addr;
    send_packet.header.slave.fmt = I2C_ADDRFMT_7BIT;
    send_packet.header.len = 1;
    send_packet.header.stop = 0;  // No stop bit for repeated start
    send_packet.reg_addr = reg_addr;

    int status = devctl(bus_config->fd, DCMD_I2C_SEND,
                       &send_packet, sizeof(send_packet), NULL);
    if (status != EOK) {
        return status;
    }

    // Then read data
    struct {
        i2c_recv_t header;
        unsigned char data[256];
    } recv_packet;

    recv_packet.header.slave.addr = slave_addr;
    recv_packet.header.slave.fmt = I2C_ADDRFMT_7BIT;
    recv_packet.header.len = len;
    recv_packet.header.stop = 1;

    status = devctl(bus_config->fd, DCMD_I2C_RECV,
                   &recv_packet, sizeof(recv_packet), NULL);
    if (status == EOK) {
        memcpy(data, recv_packet.data, len);
    }

    return status;
}

int i2c_write_register(i2c_bus_config_t* bus_config, unsigned char slave_addr,
                      unsigned char reg_addr, unsigned char* data, int len) {
    struct {
        i2c_send_t header;
        unsigned char payload[257];  // 1 byte reg + up to 256 bytes data
    } send_packet;

    send_packet.header.slave.addr = slave_addr;
    send_packet.header.slave.fmt = I2C_ADDRFMT_7BIT;
    send_packet.header.len = len + 1;
    send_packet.header.stop = 1;

    send_packet.payload[0] = reg_addr;
    memcpy(&send_packet.payload[1], data, len);

    return devctl(bus_config->fd, DCMD_I2C_SEND,
                 &send_packet, sizeof(send_packet.header) + len + 1, NULL);
}

// Test device read capability
int i2c_test_read(i2c_bus_config_t* bus_config, unsigned char slave_addr) {
    unsigned char test_data;

    // Try to read from common readable registers
    unsigned char test_registers[] = {0x00, 0x01, 0x12, 0x13};  // Common GPIO/status registers
    int i = 0;
    for (i = 0; i < sizeof(test_registers); i++) {
        int status = i2c_read_register(bus_config, slave_addr, test_registers[i], &test_data, 1);
        if (status == EOK) {
            return 1;  // Successfully read from at least one register
        }
    }

    // Try simple device read without register address (some devices support this)
    struct {
        i2c_recv_t header;
        unsigned char data[1];
    } recv_packet;

    recv_packet.header.slave.addr = slave_addr;
    recv_packet.header.slave.fmt = I2C_ADDRFMT_7BIT;
    recv_packet.header.len = 1;
    recv_packet.header.stop = 1;

    int status = devctl(bus_config->fd, DCMD_I2C_RECV,
                       &recv_packet, sizeof(recv_packet), NULL);

    return (status == EOK);
}

// Test device write capability (safe write test)
int i2c_test_write(i2c_bus_config_t* bus_config, unsigned char slave_addr) {
    // For safety, we'll only test write capability on known safe registers
    // and avoid writing to configuration registers that might change device behavior

    unsigned char original_data, test_data;
    unsigned char safe_registers[] = {0x12, 0x13};  // Output registers that are typically safe to write

    int i = 0;
    for ( i = 0; i < sizeof(safe_registers); i++) {
        unsigned char reg = safe_registers[i];

        // First, try to read the original value
        int read_status = i2c_read_register(bus_config, slave_addr, reg, &original_data, 1);
        if (read_status != EOK) {
            continue;  // Can't read, skip this register
        }

        // Test write with same value (safest test)
        int write_status = i2c_write_register(bus_config, slave_addr, reg, &original_data, 1);
        if (write_status == EOK) {
            // Verify the write by reading back
            int verify_status = i2c_read_register(bus_config, slave_addr, reg, &test_data, 1);
            if (verify_status == EOK && test_data == original_data) {
                return 1;  // Write capability confirmed
            }
        }
    }

    return 0;  // No writable registers found or write test failed
}
int i2c_scan_bus(i2c_bus_config_t* bus_config, i2c_device_info_t* devices, int max_devices) {
    int device_count = 0;

    printf("Scanning I2C bus %s...\n", bus_config->device_path);
    printf("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n");

    // Scan usable address range, avoiding reserved addresses
    unsigned char addr = 0;
    for ( addr = 0x08; addr <= 0x77; addr++) {
        if (addr % 16 == 0) {
            printf("%02x: ", addr & 0xF0);  // Display row header
        }

        // Skip reserved addresses in display but still show position
        if (addr < 0x08) {
            printf("XX ");  // Reserved address marker
            if ((addr + 1) % 16 == 0) printf("\n");
            continue;
        }

        // Probe device by attempting a read
        int present = i2c_probe_device(bus_config, addr);

        if (present > 0) {
            printf("%02x ", addr);

            if (device_count < max_devices) {
                devices[device_count].address = addr;
                devices[device_count].present = 1;

                // Test read/write capabilities
                devices[device_count].readable = i2c_test_read(bus_config, addr);
                devices[device_count].writable = i2c_test_write(bus_config, addr);

                // Attempt device identification
                i2c_identify_device(bus_config, addr, devices[device_count].device_type);

                device_count++;
            }
        } else {
            printf("-- ");
        }

        if ((addr + 1) % 16 == 0) {
            printf("\n");
        }
    }

    printf("\nFound %d devices\n", device_count);
    return device_count;
}



void i2c_debug_device(i2c_bus_config_t* bus_config, unsigned char slave_addr) {
    printf("=== I2C Device Debug: 0x%02x ===\n", slave_addr);

    // Test basic connectivity
    printf("Device probe: ");
    if (i2c_probe_device(bus_config, slave_addr)) {
        printf("PRESENT\n");
    } else {
        printf("NOT FOUND\n");
        return;
    }

    // Test register reads
    printf("Register scan:\n");
    int reg = 0;
    for ( reg = 0; reg < 16; reg++) {
        unsigned char data;
        int status = i2c_read_register(bus_config, slave_addr, reg, &data, 1);
        printf("  Reg 0x%02x: ", reg);
        if (status == EOK) {
            printf("0x%02x\n", data);
        } else {
            printf("ERROR %d\n", status);
        }
    }

    // Test write capability (safe register)
    printf("Write test: ");
    unsigned char test_val = 0x55;
    int status = i2c_write_register(bus_config, slave_addr, 0x00, &test_val, 1);
    if (status == EOK) {
        printf("SUCCESS\n");
    } else {
        printf("FAILED (%d)\n", status);
    }
}
int i2c_health_check(const char* device_path) {
    i2c_bus_config_t bus_config = {0};

    printf("I2C Bus Health Check: %s\n", device_path);

    // Initialize bus
    if (i2c_init_bus(device_path, &bus_config) != 0) {
        printf("Bus initialization failed\n");
        return -1;
    }
    printf("Bus initialization OK\n");

    // Check driver info
    i2c_driver_info_t info;
    int status = devctl(bus_config.fd, DCMD_I2C_DRIVER_INFO, &info, sizeof(info), NULL);
    if (status == EOK) {
        printf("Driver info: addr_mode=0x%02x, speed_mode=%d\n",
               info.addr_mode, info.speed_mode);
    } else {
        printf("Driver info failed: %d\n", status);
    }

    // Quick device scan
    int device_count = 0;
    int addr = 0;
    for (addr = 0x08; addr <= 0x77; addr++) {
        if (i2c_probe_device(&bus_config, addr)) {
            device_count++;
        }
    }
    printf("Devices found: %d\n", device_count);

    close(bus_config.fd);
    return 0;
}
// Parallel probe method for faster scanning
void i2c_fast_scan(i2c_bus_config_t* bus_config, unsigned char* found_devices, int* count) {
    *count = 0;

    // Use bulk probe with address list
    unsigned char probe_list[] = {0x20, 0x21, 0x24, 0x26, 0x30, 0x31, 0x32, 0x33,
                           0x34, 0x35, 0x36, 0x37, 0x48, 0x49, 0x70, 0x77};

    int i = 0;
    for ( i = 0; i < sizeof(probe_list); i++) {
        if (i2c_probe_device(bus_config, probe_list[i])) {
            found_devices[(*count)++] = probe_list[i];
        }
    }
}
// Always close file descriptors
void cleanup_i2c_bus(i2c_bus_config_t* bus_config) {
    if (bus_config->fd >= 0) {
        close(bus_config->fd);
        bus_config->fd = -1;
    }
    bus_config->initialized = 0;
}
// Implement retry logic for transient errors
int i2c_reliable_read(i2c_bus_config_t* bus_config, unsigned char slave_addr,
                     unsigned char reg_addr, unsigned char* data, int len) {
    const int MAX_RETRIES = 3;

    int retry = 0;
    for (retry = 0; retry < MAX_RETRIES; retry++) {
        int status = i2c_read_register(bus_config, slave_addr, reg_addr, data, len);

        if (status == EOK) {
            return 0;  // Success
        }

        if (status == ENODEV) {
            return status;  // Don't retry for missing device
        }

        // Brief delay before retry
        usleep(1000);  // 1ms
    }

    return -1;  // All retries failed
}
// Set appropriate bus speed for device compatibility
int i2c_set_bus_speed(i2c_bus_config_t* bus_config, unsigned int speed_hz) {
    const unsigned int valid_speeds[] = {100000, 400000, 1000000};  // Standard speeds

    // Validate speed
    int valid = 0, i = 0;
    for ( i = 0; i < sizeof(valid_speeds)/sizeof(valid_speeds[0]); i++) {
        if (speed_hz == valid_speeds[i]) {
            valid = 1;
            break;
        }
    }

    if (!valid) {
        printf("Warning: Non-standard I2C speed %d Hz\n", speed_hz);
    }

    return devctl(bus_config->fd, DCMD_I2C_SET_BUS_SPEED, &speed_hz, sizeof(speed_hz), NULL);
}
// Attempt to identify common I2C devices
void i2c_identify_device(i2c_bus_config_t* bus_config, unsigned char addr, char* device_type) {
    unsigned char reg_data[4];

    // Try common identification registers
    if (i2c_read_register(bus_config, addr, 0x00, reg_data, 2) == EOK) {
        // Check for MCP23017 pattern
        if ((addr & 0xF8) == 0x20) {  // MCP23017 base address
            strcpy(device_type, "MCP23017 GPIO");
            return;
        }

        // Check for DS1808 pattern
        if ((addr & 0xF8) == 0x28) {  // DS1808 base address
            strcpy(device_type, "DS1808 Digital Pot");
            return;
        }
    }

    strcpy(device_type, "Unknown");
}

///////////////////////////////////////////////////////////////////////////////////
///////////////Part 2: actual codes for "i2cdetect" tool///////////////////////////
///////////////////////////////////////////////////////////////////////////////////

int scan_i2c_bus(const char* device) {
    int fd = open(device, O_RDWR);
    int row, col, addr;
    if (fd < 0) {
        printf("Failed to open %s: %s\n", device, strerror(errno));
        return -1;
    }

    printf("Scanning I2C bus %s:\n", device);
    printf("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n");
    
    for (row = 0; row < 8; row++) {
        printf("%02x: ", row * 16);
        for (col = 0; col < 16; col++) {
            addr = row * 16 + col;
            if (addr < 0x08 || addr > 0x77) {
                printf("   ");
                continue;
            }
            
            // Try to read from device - use 1-byte read for better detection
            struct {
                i2c_recv_t header;
                unsigned char data;
            } recv_packet;
            
            recv_packet.header.slave.addr = addr;
            recv_packet.header.slave.fmt = I2C_ADDRFMT_7BIT;
            recv_packet.header.len = 1;  // Try to read 1 byte
            recv_packet.header.stop = 1;
            
            int status = devctl(fd, DCMD_I2C_RECV, &recv_packet, sizeof(recv_packet), NULL);
            if (status == 0) {
                printf("%02x ", addr);
            } else {
                printf("-- ");
            }
        }
        printf("\n");
    }
    
    close(fd);
    return 0;
}

int debug_scan_i2c_bus(const char* device) {
    int fd = open(device, O_RDWR);
    int row, col, addr;
    if (fd < 0) {
        printf("Failed to open %s: %s\n", device, strerror(errno));
        return -1;
    }

    printf("Debug scanning I2C bus %s (showing status codes):\n", device);
    printf("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n");
    
    for (row = 0; row < 8; row++) {
        printf("%02x: ", row * 16);
        for (col = 0; col < 16; col++) {
            addr = row * 16 + col;
            if (addr < 0x08 || addr > 0x77) {
                printf("   ");
                continue;
            }
            
            // Try zero-length operation for more reliable detection
            i2c_send_t send_zero;
            send_zero.slave.addr = addr;
            send_zero.slave.fmt = I2C_ADDRFMT_7BIT;
            send_zero.len = 0;
            send_zero.stop = 1;
            
            int status = devctl(fd, DCMD_I2C_SEND, &send_zero, sizeof(send_zero), NULL);
            if (status == 0) {
                printf("%02x ", addr);
            } else {
                printf("-%d ", status);
            }
        }
        printf("\n");
    }
    
    close(fd);
    return 0;
}

int read_i2c_register(const char* device, int slave_addr, int reg_addr) {
    int fd = open(device, O_RDWR);
    if (fd < 0) {
        printf("Failed to open %s: %s\n", device, strerror(errno));
        return -1;
    }

    // First send register address
    struct {
        i2c_send_t header;
        unsigned char reg;
    } send_packet;
    
    send_packet.header.slave.addr = slave_addr;
    send_packet.header.slave.fmt = I2C_ADDRFMT_7BIT;
    send_packet.header.len = 1;
    send_packet.header.stop = 0;  // No stop, we'll read next
    send_packet.reg = reg_addr;
    
    int status = devctl(fd, DCMD_I2C_SEND, &send_packet, sizeof(send_packet), NULL);
    if (status != 0) {
        printf("Failed to send register address 0x%02x to slave 0x%02x: status %d\n", 
               reg_addr, slave_addr, status);
        close(fd);
        return -1;
    }
    
    // Now read the data
    struct {
        i2c_recv_t header;
        unsigned char data;
    } recv_packet;
    
    recv_packet.header.slave.addr = slave_addr;
    recv_packet.header.slave.fmt = I2C_ADDRFMT_7BIT;
    recv_packet.header.len = 1;
    recv_packet.header.stop = 1;
    
    status = devctl(fd, DCMD_I2C_RECV, &recv_packet, sizeof(recv_packet), NULL);
    if (status != 0) {
        printf("Failed to read from slave 0x%02x: status %d\n", slave_addr, status);
        close(fd);
        return -1;
    }
    
    printf("Read from slave 0x%02x register 0x%02x: 0x%02x\n", 
           slave_addr, reg_addr, recv_packet.data);
    
    close(fd);
    return recv_packet.data;
}

int write_i2c_register(const char* device, int slave_addr, int reg_addr, int value) {
    int fd = open(device, O_RDWR);
    if (fd < 0) {
        printf("Failed to open %s: %s\n", device, strerror(errno));
        return -1;
    }

    // Send register address and data in one operation
    struct {
        i2c_send_t header;
        unsigned char data[2];  // [register_address, value]
    } send_packet;

    send_packet.header.slave.addr = slave_addr;
    send_packet.header.slave.fmt = I2C_ADDRFMT_7BIT;
    send_packet.header.len = 2;  // Register address + data
    send_packet.header.stop = 1; // Complete transaction
    send_packet.data[0] = reg_addr;
    send_packet.data[1] = value;

    int status = devctl(fd, DCMD_I2C_SEND, &send_packet, sizeof(send_packet), NULL);
    if (status != 0) {
        printf("Failed to write 0x%02x to register 0x%02x of slave 0x%02x: status %d\n",
               value, reg_addr, slave_addr, status);
        close(fd);
        return -1;
    }

    printf("Wrote 0x%02x to slave 0x%02x register 0x%02x\n",
           value, slave_addr, reg_addr);

    close(fd);
    return 0;
}

// TCA9546 specific constants, for the case TCA9546 control the other i2c chips (working for the same bus only):
// Microcontroller (Master)
//     |
//     | (Main I2C Bus: SDA/SCL)
//     |
// TCA9546 (I2C Multiplexer) - Address: 0x70
//     |
//     |---- Channel 0  MCP23017 #1 (Address: 0x20)
//     |---- Channel 1  MCP23017 #2 (Address: 0x20)
//     |---- Channel 2  MCP23017 #3 (Address: 0x20)
//     |---- Channel 3  MCP23017 #4 (Address: 0x20)
#define TCA9546_ADDR 0x70

int tca9546_set_channel(const char* device, int channel_mask) {
    int fd = open(device, O_RDWR);
    if (fd < 0) {
        printf("Failed to open %s: %s\n", device, strerror(errno));
        return -1;
    }

    // Send channel selection to TCA9546
    struct {
        i2c_send_t header;
        unsigned char channel;
    } send_packet;

    send_packet.header.slave.addr = TCA9546_ADDR;
    send_packet.header.slave.fmt = I2C_ADDRFMT_7BIT;
    send_packet.header.len = 1;
    send_packet.header.stop = 1;
    send_packet.channel = channel_mask & 0x0F;  // Only bits 0-3 are valid

    int status = devctl(fd, DCMD_I2C_SEND, &send_packet, sizeof(send_packet), NULL);
    if (status != 0) {
        printf("Failed to set TCA9546 channel 0x%02x: status %d\n", channel_mask, status);
        close(fd);
        return -1;
    }

    printf("TCA9546 channels set to: 0x%02x ", channel_mask);
    printf("(");
    if (channel_mask == 0) {
        printf("all disabled");
    } else {
        int first = 1;
        int i=0;
        for ( i = 0; i < 4; i++) {
            if (channel_mask & (1 << i)) {
                if (!first) printf(", ");
                printf("CH%d", i);
                first = 0;
            }
        }
    }
    printf(")\n");

    close(fd);
    return 0;
}

int tca9546_get_channel(const char* device) {
    int fd = open(device, O_RDWR);
    if (fd < 0) {
        printf("Failed to open %s: %s\n", device, strerror(errno));
        return -1;
    }

    // Read current channel selection from TCA9546
    struct {
        i2c_recv_t header;
        unsigned char channel;
    } recv_packet;

    recv_packet.header.slave.addr = TCA9546_ADDR;
    recv_packet.header.slave.fmt = I2C_ADDRFMT_7BIT;
    recv_packet.header.len = 1;
    recv_packet.header.stop = 1;

    int status = devctl(fd, DCMD_I2C_RECV, &recv_packet, sizeof(recv_packet), NULL);
    if (status != 0) {
        printf("Failed to read TCA9546 channel status: status %d\n", status);
        close(fd);
        return -1;
    }

    printf("TCA9546 current channels: 0x%02x ", recv_packet.channel);
    printf("(");
    if (recv_packet.channel == 0) {
        printf("all disabled");
    } else {
        int first = 1;
        int i=0;
        for (i = 0; i < 4; i++) {
            if (recv_packet.channel & (1 << i)) {
                if (!first) printf(", ");
                printf("CH%d", i);
                first = 0;
            }
        }
    }
    printf(")\n");

    close(fd);
    return recv_packet.channel;
}

int tca9546_scan_all_channels(const char* device) {
    printf("Scanning all TCA9546 channels on %s:\n", device);
    printf("=========================================\n");

    int ch = 0;
    for ( ch = 0; ch < 4; ch++) {
        printf("\n--- Channel %d ---\n", ch);
        
        // Enable only this channel - use SAME device for TCA9546 control
        if (tca9546_set_channel(device, 1 << ch) != 0) {
            printf("Failed to enable channel %d\n", ch);
            continue;
        }
        
        // Scan the same bus where TCA9546 is located
        scan_i2c_bus(device);
    }
    
    // Disable all channels when done
    printf("\n--- Disabling all channels ---\n");
    tca9546_set_channel(device, 0x00);
    
    return 0;
}

//add to support 16-bit read/write 
int read_i2c_register16(const char* device, int slave_addr, int reg_addr) {
    int fd = open(device, O_RDWR);
    if (fd < 0) {
        printf("Failed to open %s: %s\n", device, strerror(errno));
        return -1;
    }

    // First, prepare 16-bit register address properly
    unsigned char reg_msb = (reg_addr >> 8) & 0xFF;
    unsigned char reg_lsb = reg_addr & 0xFF;
    
    // Create the register address buffer
    struct {
        i2c_send_t header;
        unsigned char reg[2];
    } send_packet;
    
    send_packet.header.slave.addr = slave_addr;
    send_packet.header.slave.fmt = I2C_ADDRFMT_7BIT;
    send_packet.header.len = 2;
    send_packet.header.stop = 1;  // Complete this transaction with STOP
    send_packet.reg[0] = reg_msb;
    send_packet.reg[1] = reg_lsb;
    
    // Send the register address first with a STOP
    int status = devctl(fd, DCMD_I2C_SEND, &send_packet, sizeof(send_packet), NULL);
    if (status != 0) {
        printf("Failed to send 16-bit register address 0x%04x to slave 0x%02x: status %d\n", 
               reg_addr, slave_addr, status);
        close(fd);
        return -1;
    }
    
    // Small delay to ensure device processes the address
    usleep(1000);  // 1ms delay
    
    // Now perform a simple receive operation
    struct {
        i2c_recv_t header;
        unsigned char data;
    } recv_packet;
    
    recv_packet.header.slave.addr = slave_addr;
    recv_packet.header.slave.fmt = I2C_ADDRFMT_7BIT;
    recv_packet.header.len = 1;
    recv_packet.header.stop = 1;
    
    status = devctl(fd, DCMD_I2C_RECV, &recv_packet, sizeof(recv_packet), NULL);
    if (status != 0) {
        printf("Failed to read from slave 0x%02x: status %d\n", slave_addr, status);
        close(fd);
        return -1;
    }
    
    printf("Read from slave 0x%02x 16-bit register 0x%04x: 0x%02x\n", 
           slave_addr, reg_addr, recv_packet.data);
    
    close(fd);
    return recv_packet.data;
}

int write_i2c_register16(const char* device, int slave_addr, int reg_addr, int value) {
    int fd = open(device, O_RDWR);
    if (fd < 0) {
        printf("Failed to open %s: %s\n", device, strerror(errno));
        return -1;
    }

    // First, send just the 16-bit register address
    struct {
        i2c_send_t header;
        unsigned char reg[2];
    } send_reg_packet;
    
    send_reg_packet.header.slave.addr = slave_addr;
    send_reg_packet.header.slave.fmt = I2C_ADDRFMT_7BIT;
    send_reg_packet.header.len = 2;
    send_reg_packet.header.stop = 1;  // Complete with STOP
    send_reg_packet.reg[0] = (reg_addr >> 8) & 0xFF;
    send_reg_packet.reg[1] = reg_addr & 0xFF;
    
    int status = devctl(fd, DCMD_I2C_SEND, &send_reg_packet, sizeof(send_reg_packet), NULL);
    if (status != 0) {
        printf("Failed to send 16-bit register address 0x%04x to slave 0x%02x: status %d\n",
               reg_addr, slave_addr, status);
        close(fd);
        return -1;
    }
    
    // Small delay
    usleep(1000);  // 1ms delay
    
    // Now send the data value
    struct {
        i2c_send_t header;
        unsigned char data;
    } send_data_packet;
    
    send_data_packet.header.slave.addr = slave_addr;
    send_data_packet.header.slave.fmt = I2C_ADDRFMT_7BIT;
    send_data_packet.header.len = 1;
    send_data_packet.header.stop = 1;
    send_data_packet.data = value;
    
    status = devctl(fd, DCMD_I2C_SEND, &send_data_packet, sizeof(send_data_packet), NULL);
    if (status != 0) {
        printf("Failed to write 0x%02x to 16-bit register 0x%04x of slave 0x%02x: status %d\n",
               value, reg_addr, slave_addr, status);
        close(fd);
        return -1;
    }

    printf("Wrote 0x%02x to slave 0x%02x 16-bit register 0x%04x\n",
           value, slave_addr, reg_addr);

    close(fd);
    return 0;
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        printf("Usage: %s <device> <command> [args...]\n", argv[0]);
        printf("Commands:\n");
        printf("  scan                       - Scan I2C bus for devices\n");
        printf("  debug                      - Debug scan showing status codes\n");
        printf("  read <addr> <reg>          - Read register from device\n");
        //printf("  write <addr> <reg> <value> - Write value to register\n");
        printf("  %s /dev/i2c3 read16 0x24 0x100d\n", argv[0]);
        printf("  %s /dev/i2c3 write 0x24 0x00 0xFF\n", argv[0]);
        printf("  %s /dev/i2c3 write16 0x24 0x100d 0x55\n", argv[0]);
        printf("  tca-set <channel_mask>     - Set TCA9546 channels (0x70)\n");
        printf("  tca-get                    - Get current TCA9546 channel status\n");
        printf("  tca-scan                   - Scan all TCA9546 channels\n");
        printf("Examples:\n");
        printf("  %s /dev/i2c3 scan          # Scan bus where TCA9546 is located\n", argv[0]);
        printf("  %s /dev/i2c3 read 0x24 0x13\n", argv[0]);
        printf("  %s /dev/i2c3 write 0x24 0x00 0xFF\n", argv[0]);
        printf("  %s /dev/i2c3 tca-set 0x01    # Enable channel 0 only\n", argv[0]);
        printf("  %s /dev/i2c3 tca-set 0x05    # Enable channels 0 and 2\n", argv[0]);
        printf("  %s /dev/i2c3 tca-set 0x00    # Disable all channels\n", argv[0]);
        printf("  %s /dev/i2c3 tca-get         # Show current channel status\n", argv[0]);
        printf("  %s /dev/i2c3 tca-scan        # Scan devices on all channels\n", argv[0]);        
        return 1;
    }
    
    const char* device = argv[1];
    const char* command = argv[2];
    
    if (strcmp(command, "scan") == 0) {
        return scan_i2c_bus(device);
    } 
    else if (strcmp(command, "debug") == 0) {
        return debug_scan_i2c_bus(device);
    }
    else if (strcmp(command, "read") == 0 && argc >= 5) {
        int slave_addr = strtol(argv[3], NULL, 0);
        int reg_addr = strtol(argv[4], NULL, 0);
        return read_i2c_register(device, slave_addr, reg_addr);
    }
    else if (strcmp(command, "read16") == 0 && argc >= 5) {
        int slave_addr = strtol(argv[3], NULL, 0);
        int reg_addr = strtol(argv[4], NULL, 0);
        return read_i2c_register16(device, slave_addr, reg_addr);
    }
    else if (strcmp(command, "write") == 0 && argc >= 6) {
        int slave_addr = strtol(argv[3], NULL, 0);
        int reg_addr = strtol(argv[4], NULL, 0);
        int value = strtol(argv[5], NULL, 0);
        return write_i2c_register(device, slave_addr, reg_addr, value);
    }
    else if (strcmp(command, "write16") == 0 && argc >= 6) {
        int slave_addr = strtol(argv[3], NULL, 0);
        int reg_addr = strtol(argv[4], NULL, 0);
        int value = strtol(argv[5], NULL, 0);
        return write_i2c_register16(device, slave_addr, reg_addr, value);
    }
    else if (strcmp(command, "tca-set") == 0 && argc >= 4) {
        int channel_mask = strtol(argv[3], NULL, 0);
        return tca9546_set_channel(device, channel_mask);
    }
    else if (strcmp(command, "tca-get") == 0) {
        return tca9546_get_channel(device);
    }
    else if (strcmp(command, "tca-scan") == 0) {
        return tca9546_scan_all_channels(device);
    }
    else {
        printf("Invalid command or missing arguments\n");
        return 1;
    }
}

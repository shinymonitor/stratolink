//================================================================
// E32
//================================================================

#ifndef _E32_H
#define _E32_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>

#include <stdint.h>
#include <stdbool.h>

#include <gpiod.h>

typedef struct {
    int fd;
    struct gpiod_chip* chip;
    struct gpiod_line* m0_line;
    struct gpiod_line* m1_line;
    struct gpiod_line* aux_line;
} E32_Device;

bool E32_init(const char* portname, uint8_t m0_pin, uint8_t m1_pin, uint8_t aux_pin, E32_Device* device) {
    device->fd = open(portname, O_RDWR | O_NOCTTY | O_SYNC);
    if (device->fd < 0) {perror("open serial"); return false;}
    struct termios tty;
    if (tcgetattr(device->fd, &tty) != 0) {perror("tcgetattr"); close(device->fd); return false;}
    cfsetospeed(&tty, B9600);
    cfsetispeed(&tty, B9600);
    cfmakeraw(&tty);
    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 10;
    if (tcsetattr(device->fd, TCSANOW, &tty) != 0) {perror("tcsetattr"); close(device->fd); return false;}
    tcflush(device->fd, TCIOFLUSH);
    device->chip = gpiod_chip_open("/dev/gpiochip0");
    if (!device->chip) {perror("gpiod_chip_open"); close(device->fd); return false;}
    device->m0_line = gpiod_chip_get_line(device->chip, m0_pin);
    device->m1_line = gpiod_chip_get_line(device->chip, m1_pin);
    device->aux_line = gpiod_chip_get_line(device->chip, aux_pin);
    if (!device->m0_line || !device->m1_line || !device->aux_line) {perror("gpiod_chip_get_line"); gpiod_chip_close(device->chip); close(device->fd); return false;}
    if (gpiod_line_request_input(device->aux_line, "e32_aux") < 0) {perror("gpiod_line_request_input"); gpiod_chip_close(device->chip); close(device->fd); return false;}
    if (gpiod_line_request_output(device->m0_line, "e32_m0", 0) < 0 || gpiod_line_request_output(device->m1_line, "e32_m1", 0) < 0) {perror("gpiod_line_request_output"); gpiod_chip_close(device->chip); close(device->fd); return false;}
    return true;
}

void E32_wait_for_aux(E32_Device* device){while (!gpiod_line_get_value(device->aux_line));}

bool E32_write_bytes(E32_Device* device, uint8_t* data, size_t len) {
    ssize_t written = write(device->fd, data, len);
    if (written == -1) {perror("write"); return false;}
    if ((size_t)written != len) {fprintf(stderr, "Partial write: %zd/%zu bytes\n", written, len); return false;}
    E32_wait_for_aux(device);
    return true;
}

bool E32_write_byte(E32_Device* device, uint8_t* data) {
    if (write(device->fd, data, 1) != 1) {
        perror("write");
        return false;
    } 
    return true;
}

bool E32_read_until_crlf(E32_Device* device, uint8_t* buffer, size_t buffer_size, size_t* len) {
    size_t total = 0;
    uint8_t c;
    bool got_cr = false;
    while (total < buffer_size - 1) {
        if (read(device->fd, &c, 1) != 1) {if (total > 0) break; return false;}
        buffer[total++] = c;
        if (got_cr && c == '\n') {
            buffer[total - 2] = '\0';
            *len = total - 2;
            return true;
        }
        got_cr = (c == '\r');
    }
    buffer[total] = '\0';
    *len = total;
    return total > 0;
}

bool E32_read_n_bytes(E32_Device* device, uint8_t* buffer, size_t buffer_size, size_t n) {
    size_t read_bytes_count = 0;
    uint8_t c;
    if (n > buffer_size) return false;
    while (read_bytes_count < n) {
        if (read(device->fd, &c, 1) != 1) return false;
        buffer[read_bytes_count++] = c;
    }
    return true;
}

bool E32_read_bytes(E32_Device* device, uint8_t* buffer, size_t buffer_size, size_t* len) {
    size_t n = read(device->fd, buffer, buffer_size);
    if (n <= 0) return false;
    *len = n;
    return true;
}

bool E32_set_mode(E32_Device* device, int m0_value, int m1_value) {
    if (gpiod_line_set_value(device->m0_line, m0_value) < 0 || gpiod_line_set_value(device->m1_line, m1_value) < 0) {perror("gpiod_line_set_value"); return false;}
    E32_wait_for_aux(device);
    return true;
}

bool E32_reset(E32_Device* device) {
    if (!E32_set_mode(device, 1, 1)) return false;
    uint8_t cmd[3] = {0xC4, 0xC4, 0xC4};
    if (!E32_write_bytes(device, cmd, 3)) {E32_set_mode(device, 0, 0); return false;}
    E32_wait_for_aux(device);
    E32_set_mode(device, 0, 0);
    return true;
}

typedef struct __attribute__((packed)) {uint8_t addh, addl, speed, channel, option;} E32_Config;

bool E32_read_config(E32_Device* device, E32_Config* config) {
    if (!E32_set_mode(device, 1, 1)) return false;
    uint8_t cmd[3] = {0xC1, 0xC1, 0xC1};
    if (!E32_write_bytes(device, cmd, 3)) {E32_set_mode(device, 0, 0); return false;}
    uint8_t response[6];
    E32_wait_for_aux(device);
    int len = read(device->fd, response, 6);
    if (len != 6) {E32_set_mode(device, 0, 0); return false;}
    memcpy(config, &response[1], 5);
    E32_set_mode(device, 0, 0);
    return true;
}

bool E32_write_config(E32_Device* device, E32_Config* config) {
    if (!E32_set_mode(device, 1, 1)) return false;
    uint8_t cmd[6] = {0xC0};
    memcpy(&cmd[1], config, 5);
    if (!E32_write_bytes(device, cmd, 6)) {E32_set_mode(device, 0, 0); return false;}
    E32_set_mode(device, 0, 0);
    return true;
}

void E32_close(E32_Device* device) {
    if (device->m0_line) gpiod_line_release(device->m0_line);
    if (device->m1_line) gpiod_line_release(device->m1_line);
    if (device->chip) gpiod_chip_close(device->chip);
    if (device->fd >= 0) close(device->fd);
}

#endif // _E32_H
// - Commands must be terminated with \r\n
// - response for everything except send command is in ascii (read until \r\n)
// - for send command, first 4 bytes are the number of bytes incoming (big endian) (if length is zero, an error has occured), then that many number of bytes (dont stop at \r\n)

#include "e32.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <signal.h>

#include <stdint.h>
#include <stdbool.h>

// E32 config
#define E32_PORTNAME "/dev/serial0"
#define E32_M0_PIN 23
#define E32_M1_PIN 24
#define E32_AUX_PIN 25

// buffer sizes for: maximum number of args in the command, storing the received command, storing the status response string
#define MAX_ARGS 10
#define COMMAND_BUFFER_SIZE 2048
#define STATUS_RESPONSE_BUFFER_SIZE 1024
#define IMAGE_READ_CHUNK_SIZE 256
#define LS_READ_CHUNK_SIZE 256
#define MAIN_LOOP_SLEEP_TIME 10000

// (taken from stratolink repo) takes a photo with fswebcam
static inline bool take_photo(){return !system("fswebcam -r 320x240 --jpeg 60 --no-banner photo.jpg");}

// command -> argc, argv
static inline size_t chop_command(char* command, size_t len, char** argv) {
    bool parsing_arg = false;
    size_t arg_count=0;
    for (size_t i = 0; i < len; ++i) {
        if (parsing_arg == false && command[i] != ' ') {argv[arg_count++] = command + i; parsing_arg = true;}
        else if (parsing_arg == true && command[i] == ' ' && (i == 0 || command[i-1] != '\\')) {command[i] = '\0'; parsing_arg = false;}
    }
    if (parsing_arg && len > 0) command[len] = '\0';
    return arg_count;
}

// util functions: send a string, send 4 bytes of 0
static inline void send_string(E32_Device* device, char* str){
    E32_write_bytes(device, (uint8_t*)str, strlen(str));
}
static inline void send_four_zero(E32_Device* device) {
    uint8_t zeros[4] = {0, 0, 0, 0};
    E32_write_bytes(device, zeros, 4);
}

// 4 bytes of length followed by the bytes
static inline bool send_photo(E32_Device* device, char* path){
    FILE* file_handle=fopen(path, "rb");
    if (!file_handle) return false;

    if(fseek(file_handle, 0, SEEK_END)!=0) {fclose(file_handle); return false;}
    size_t bytes_count = ftell(file_handle);
    if (bytes_count == (size_t)-1L) {fclose(file_handle); return false;}
    if(fseek(file_handle, 0, SEEK_SET)!=0) {fclose(file_handle); return false;}

    uint8_t size_bytes[4];
    size_bytes[0] = (uint8_t)(bytes_count >> 24 & 0xFF);
    size_bytes[1] = (uint8_t)(bytes_count >> 16 & 0xFF);
    size_bytes[2] = (uint8_t)(bytes_count >> 8 & 0xFF);
    size_bytes[3] = (uint8_t)(bytes_count & 0xFF);
    E32_write_bytes(device, size_bytes, 4);

    uint8_t image_chunk_buffer[IMAGE_READ_CHUNK_SIZE];
    size_t bytes_read;
    while ((bytes_read = fread(image_chunk_buffer, 1, IMAGE_READ_CHUNK_SIZE, file_handle)) > 0) {E32_write_bytes(device, image_chunk_buffer, bytes_read);}

    fclose(file_handle);

    E32_wait_for_aux(device);

    return true;
}

// command logic, easy to add commands (with arg rules) in future
static inline void handle_command(E32_Device* device, char* command, size_t command_len){
    char* argv[MAX_ARGS]={0};
    size_t argc=chop_command(command, command_len, argv);

    if (argc==0) {send_string(device, "No command given\r\n"); return;}

    else if (strcmp(argv[0], "list")==0){
        if (argc!=1) {send_string(device, "Incorrect number of arguments\r\n"); return;}

        FILE* ls_command_handle=popen("ls -l", "r");
        if (!ls_command_handle) {send_string(device, "Couldnt run ls\r\n"); return;}

        char ls_read_chunk_buffer[256];
        size_t bytes_read;
        while ((bytes_read = fread(ls_read_chunk_buffer, 1, LS_READ_CHUNK_SIZE, ls_command_handle)) > 0) E32_write_bytes(device, (uint8_t*)ls_read_chunk_buffer, bytes_read);

        pclose(ls_command_handle);

        char crlf[2] = {'\r', '\n'};
        E32_write_bytes(device, (uint8_t*)crlf, 2);

        E32_wait_for_aux(device);
    }

    else if (strcmp(argv[0], "send")==0){
        if (argc!=2) {send_four_zero(device); return;}
        if (!send_photo(device, argv[1])) {send_four_zero(device); return;}
    }

    else if (strcmp(argv[0], "photo")==0){
        if (argc!=1) {send_four_zero(device); return;}
        if (!take_photo()) {send_four_zero(device); return;}
        if (!send_photo(device, "photo.jpg")) {send_four_zero(device); return;}
    }

    else if (strcmp(argv[0], "status")==0){
        if (argc!=1) {send_string(device, "Incorrect number of arguments\r\n"); return;}

        char status_response_buffer[STATUS_RESPONSE_BUFFER_SIZE]={0};

        struct statvfs disk_info;
        struct sysinfo sys_info;
        if (statvfs("/", &disk_info)!=0) {send_string(device, "Couldnt run status\r\n"); return;}
        if (sysinfo(&sys_info) != 0) {send_string(device, "Couldnt run status\r\n"); return;}
        uint8_t disk_usage=100-(disk_info.f_bfree*100/disk_info.f_blocks);
        uint8_t ram_usage=100-(sys_info.freeram*100/sys_info.totalram);

        E32_Config config={0};
        E32_read_config(device, &config);
        uint8_t transmission_power = config.option & 3;
        uint8_t transmission_power_dBm=0;
        switch (transmission_power){
            case 0: transmission_power_dBm=30; break;
            case 1: transmission_power_dBm=27; break;
            case 2: transmission_power_dBm=24; break;
            case 3: transmission_power_dBm=21; break;
            default: send_string(device, "Couldnt run status\r\n"); return;
        }

        size_t status_response_buffer_len=sprintf(
            status_response_buffer, "Disk usage: %d%%\nRAM usage: %d%%\nTransmission power: %d dBm\nConfig hexstring: %02x%02x%02x%02x%02x\r\n", 
            disk_usage, ram_usage, transmission_power_dBm, config.addh, config.addl, config.speed, config.channel, config.option
        );
        E32_write_bytes(device, (uint8_t*)status_response_buffer, status_response_buffer_len);
    }

    else if (strcmp(argv[0], "restart")==0){
        if (argc!=1) {send_string(device, "Incorrect number of arguments\r\n"); return;}

        E32_reset(device);
        send_string(device, "Restarting E32\r\n");
    }

    else {send_string(device, "Unknown command\r\n"); return;}
}

// handle signals
volatile bool running = true;
void signal_handler(int sig) {(void)sig; running = false;}

int main() {
    E32_Device device = {0};
    if (!E32_init(E32_PORTNAME, E32_M0_PIN, E32_M1_PIN, E32_AUX_PIN, &device)) {fprintf(stderr, "Failed to initialize E32 module\n"); return 1;}
    if (!E32_set_mode(&device, 0, 0)) {fprintf(stderr, "Failed to set mode\n"); E32_close(&device); return 1;}

    // switch to 21dBm
    E32_Config config={0};
    E32_read_config(&device, &config);
    config.option |= 3;
    E32_write_config(&device, &config);

    uint8_t command[COMMAND_BUFFER_SIZE]={0};
    size_t len;

    // handle signals and enter main loop
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    while(running){
        if (!E32_read_until_crlf(&device, command, sizeof(command), &len)) {usleep(MAIN_LOOP_SLEEP_TIME); continue;}
        handle_command(&device, (char*)command, len);
        memset(command, 0, sizeof(command));
    }

    E32_close(&device);

    return 0;
}

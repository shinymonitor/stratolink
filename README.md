# C Implementation of [StratoLink](https://github.com/AGH-Skylink/StratoLink)

## Commands
- photo = take a photo with fswebcam and send it
- send &lt;file_name&gt; = send a file with a given path
- list = ls -l result
- status = disk & ram usage, transmission power and config hex string
- restart = remote reset E32 module

## Sending and receiving commands
- Commands must be terminated with \r\n
- Commands can have leading and trailing whitespace and variable number of whitespace in between args (Unix-like) (good for users)
- response for everything except send command is in ascii (read until \r\n)
- for send command, first 4 bytes are the number of bytes incoming (big endian) (if length is zero, an error has occured), then that many number of bytes (dont stop at \r\n)
- see `receiver.c` for example code

## Setup
### Config
- /boot/config.txt
```
enable_uart=1
dtoverlay=disable-bt
```
- `raspi-config` -> interface options -> serial -> “No” for login shell, “Yes” for serial port hardware, interface options -> serial -> camera -> enable
- sudo usermod -a -G dialout $USER
- sudo apt-get install fswebcam libgpiod-dev
- reboot
- M0 to 23, M1 to 24, AUX to 25

## Compile

- Compile with `-lgpiod`
- `./lora_daemon > lora_daemon.log 2>&1 &` run and move to background and redirect output to log

/*******************************************************************************
 *
 *  Copyright (C) 2009-2011 Broadcom Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

/*****************************************************************************
**                                                                           
**  Name:          brcm_patchram_plus.c
**
**  Description:   This program downloads a patchram files in the HCD format
**                 to Broadcom Bluetooth based silicon and combo chips and
**				   and other utility functions.
**
**                 It can be invoked from the command line in the form
**						<-d> to print a debug log
**						<--patchram patchram_file>
**						<--baudrate baud_rate>
**						<--bd_addr bd_address>
**						<--enable_lpm>
**						<--enable_hci>
**                                              <--pcm_role master|slave>
**						<--use_baudrate_for_download>
**						uart_device_name
**
**                 For example:
**
**                 brcm_patchram_plus -d --patchram  \
**						BCM2045B2_002.002.011.0348.0349.hcd /dev/ttyHS0
**
**                 It will return 0 for success and a number greater than 0
**                 for any errors.
**
**                 For Android, this program invoked using a 
**                 "system(2)" call from the beginning of the bt_enable
**                 function inside the file 
**                 system/bluetooth/bluedroid/bluetooth.c.
**
**                 If the Android system property "ro.bt.bcm_bdaddr_path" is
**                 set, then the bd_addr will be read from this path.
**                 This is overridden by --bd_addr on the command line.
**  
******************************************************************************/

// TODO: Integrate BCM support into Bluez hciattach

#include <stdio.h>
#include <getopt.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <stdlib.h>

#ifdef ANDROID
#include <termios.h>
#else
#include <sys/termios.h>
#endif

#include <string.h>
#include <signal.h>

#include <cutils/properties.h>

#ifdef ANDROID
#define LOG_TAG "brcm_patchram_plus"
#include <cutils/log.h>
#undef printf
#define printf LOGD
#undef fprintf
#define fprintf(x, ...) \
  { if(x==stderr) LOGE(__VA_ARGS__); else fprintf(x, __VA_ARGS__); }

#endif //ANDROID

#ifndef N_HCI
#define N_HCI	15
#endif

#define HCIUARTSETPROTO		_IOW('U', 200, int)
#define HCIUARTGETPROTO		_IOR('U', 201, int)
#define HCIUARTGETDEVICE	_IOR('U', 202, int)

#define HCI_UART_H4		0
#define HCI_UART_BCSP	1
#define HCI_UART_3WIRE	2
#define HCI_UART_H4DS	3
#define HCI_UART_LL		4


int uart_fd = -1;
int hcdfile_fd = -1;
int termios_baudrate = 0;
int bdaddr_flag = 0;
int enable_lpm = 0;
int enable_hci = 0;
int pcm_slave = 0;
int pcm_master = 0;
int use_baudrate_for_download = 0;
int debug = 0;

struct termios termios;
unsigned char buffer[1024];

unsigned char hci_reset[] = { 0x01, 0x03, 0x0c, 0x00 };

unsigned char hci_download_minidriver[] = { 0x01, 0x2e, 0xfc, 0x00 };

unsigned char hci_update_baud_rate[] = { 0x01, 0x18, 0xfc, 0x06, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00 };

unsigned char hci_write_bd_addr[] = { 0x01, 0x01, 0xfc, 0x06, 
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

unsigned char hci_write_sleep_mode[] = { 0x01, 0x27, 0xfc, 0x0c, 
	0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00,
	0x00, 0x00 };

// HCI_COMMAND_PKT = 0x01
// unpacked ogf/ocf: 0x3F 0x1C
// ... ((ocf & 0x03ff)|(ogf << 10)) => 0xFC1C (0x1C 0xFC)
// packet data len: 0x5
// Data for the Write_SCO_PCM_Int_Param Message:
// 00 (Use PCM)
// 0x (02== 512 KHz bit clock, 03==1024 KHz bit clock, 04==2048 KHz)
// 01 (Long FS)
// 00 (Slave frame sync)
// 00 (Slave bit clock)
unsigned char hci_write_pcm_slave_mode[] =
	{ 0x01, 0x1C, 0xFC, 0x05, 0x00, 0x02, 0x00, 0x00, 0x00 };
unsigned char hci_write_pcm_master_mode[] =
	{ 0x01, 0x1C, 0xFC, 0x05, 0x00, 0x02, 0x00, 0x01, 0x01 };

int
parse_patchram(char *optarg)
{
	char *p;

	if (!(p = strrchr(optarg, '.'))) {
		fprintf(stderr, "file %s not an HCD file\n", optarg);
		exit(3);
	}

	p++;

	if (strcasecmp("hcd", p) != 0) {
		fprintf(stderr, "file %s not an HCD file\n", optarg);
		exit(4);
	}

	if ((hcdfile_fd = open(optarg, O_RDONLY)) == -1) {
		fprintf(stderr, "file %s could not be opened, error %d\n", optarg, errno);
		exit(5);
	}

	return(0);
}

void 
BRCM_encode_baud_rate(uint baud_rate, unsigned char *encoded_baud)
{
	if(baud_rate == 0 || encoded_baud == NULL) {
		fprintf(stderr, "Baudrate not supported!");
		return; 
	}

	encoded_baud[3] = (unsigned char)(baud_rate >> 24);
	encoded_baud[2] = (unsigned char)(baud_rate >> 16);
	encoded_baud[1] = (unsigned char)(baud_rate >> 8);
	encoded_baud[0] = (unsigned char)(baud_rate & 0xFF);
}

typedef struct {
	int baud_rate;
	int termios_value;
} tBaudRates;

tBaudRates baud_rates[] = {
	{ 115200, B115200 },
	{ 230400, B230400 },
	{ 460800, B460800 },
	{ 500000, B500000 },
	{ 576000, B576000 },
	{ 921600, B921600 },
	{ 1000000, B1000000 },
	{ 1152000, B1152000 },
	{ 1500000, B1500000 },
	{ 2000000, B2000000 },
	{ 2500000, B2500000 },
	{ 3000000, B3000000 },
#ifndef __CYGWIN__
	{ 3500000, B3500000 },
	{ 4000000, B4000000 }
#endif
};

int
validate_baudrate(int baud_rate, int *value)
{
	unsigned int i;

	for (i = 0; i < (sizeof(baud_rates) / sizeof(tBaudRates)); i++) {
		if (baud_rates[i].baud_rate == baud_rate) {
			*value = baud_rates[i].termios_value;
			return(1);
		}
	}

	return(0);
}

int
parse_baudrate(char *optarg)
{
	int baudrate = atoi(optarg);

	if (validate_baudrate(baudrate, &termios_baudrate)) {
		BRCM_encode_baud_rate(baudrate, &hci_update_baud_rate[6]);
	}

	return(0);
}

int
parse_bdaddr(char *optarg)
{
	int bd_addr[6];
	int i;

	sscanf(optarg, "%02X:%02X:%02X:%02X:%02X:%02X",
		&bd_addr[5], &bd_addr[4], &bd_addr[3],
		&bd_addr[2], &bd_addr[1], &bd_addr[0]);

	for (i = 0; i < 6; i++) {
		hci_write_bd_addr[4 + i] = bd_addr[i];
	}

	bdaddr_flag = 1;	

	return(0);
}

int
parse_enable_lpm(char *optarg)
{
	enable_lpm = 1;
	return(0);
}

int
parse_use_baudrate_for_download(char *optarg)
{
	use_baudrate_for_download = 1;
	return(0);
}

int
parse_enable_hci(char *optarg)
{
	enable_hci = 1;
	return(0);
}

int
parse_pcm_role(char *optarg) {
	if(!strcmp(optarg, "master")) {
		pcm_master = 1;
	} else if (!strcmp(optarg, "slave")) {
		pcm_slave = 1;
	} else {
		printf("Unknown PCM Role received: %s\n", optarg);
	}
	if (pcm_master && pcm_slave) {
		fprintf(stderr, "Illegal command line- pcm role master && slave\n");
		exit(6);
	}
	return(0);
}

int
parse_cmd_line(int argc, char **argv)
{
	int c;
	int digit_optind = 0;

	typedef int (*PFI)();

	PFI parse_param[] = { parse_patchram, parse_baudrate,
		parse_bdaddr, parse_enable_lpm, parse_enable_hci,
		parse_pcm_role, parse_use_baudrate_for_download};

   	while (1)
    	{
		int this_option_optind = optind ? optind : 1;
		int option_index = 0;

		static struct option long_options[] = {
			{"patchram", 1, 0, 0},
			{"baudrate", 1, 0, 0},
			{"bd_addr", 1, 0, 0},
			{"enable_lpm", 0, 0, 0},
			{"enable_hci", 0, 0, 0},
			{"pcm_role", 1, 0, 0},
			{"use_baudrate_for_download", 0, 0, 0},
			{0, 0, 0, 0}
		};

		c = getopt_long_only (argc, argv, "d", long_options,
				&option_index);

		if (c == -1) {
			break;
		}

		switch (c) {
			case 0:
				if (debug) {
					printf ("option %s",
						long_options[option_index].name);
					if (optarg)
						printf (" with arg %s", optarg);
					printf ("\n");
				}
				(*parse_param[option_index])(optarg);
				break;
			case 'd':
				debug = 1;
				break;

			case '?':
				//nobreak
			default:
				printf("Usage %s:\n", argv[0]);
				printf("\t<-d> to print a debug log\n");
				printf("\t<--patchram patchram_file>\n");
				printf("\t<--baudrate baud_rate>\n");
				printf("\t<--bd_addr bd_address>\n");
				printf("\t<--enable_lpm>\n");
				printf("\t<--enable_hci>\n");
				printf("\t<--pcm_role slave|master>\n");
				printf("\t<--use_baudrate_for_download> - Uses the\
						baudrate for downloading the\
						firmware\n");
				printf("\tuart_device_name\n");
				break;
		}
	}
	if (optind < argc) {
		if (debug)
			printf ("%s \n", argv[optind]);
		if ((uart_fd = open(argv[optind], O_RDWR | O_NOCTTY)) == -1) {
			fprintf(stderr, "port %s could not be opened, error %d\n",
					argv[2], errno);
		}
	}

	return(0);
}

void
init_uart()
{
	tcflush(uart_fd, TCIOFLUSH);
	tcgetattr(uart_fd, &termios);

#ifndef __CYGWIN__
	cfmakeraw(&termios);
#else
	termios.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP
                | INLCR | IGNCR | ICRNL | IXON);
	termios.c_oflag &= ~OPOST;
	termios.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
	termios.c_cflag &= ~(CSIZE | PARENB);
	termios.c_cflag |= CS8;
#endif

	termios.c_cflag |= CRTSCTS;
	tcsetattr(uart_fd, TCSANOW, &termios);
	tcflush(uart_fd, TCIOFLUSH);
	tcsetattr(uart_fd, TCSANOW, &termios);
	tcflush(uart_fd, TCIOFLUSH);
	tcflush(uart_fd, TCIOFLUSH);
	cfsetospeed(&termios, B115200);
	cfsetispeed(&termios, B115200);
	tcsetattr(uart_fd, TCSANOW, &termios);
}

void
dump(unsigned char *out, int len)
{
	int i;

	for (i = 0; i < len; i++) {
		if (i && !(i % 16)) {
			fprintf(stderr, "\n");
		}

		fprintf(stderr, "%02x ", out[i]);
	}

	fprintf(stderr, "\n");
}

void
read_event(int fd, unsigned char *buffer)
{
	int i = 0;
	int len = 3;
	int count;

	while ((count = read(fd, &buffer[i], len)) < len) {
		i += count;
		len -= count;
	}

	i += count;
	len = buffer[2];

	while ((count = read(fd, &buffer[i], len)) < len) {
		i += count;
		len -= count;
	}

	if (debug) {
		count += i;

		fprintf(stderr, "received %d\n", count);
		dump(buffer, count);
	}
}

void
hci_send_cmd(unsigned char *buf, int len)
{
	if (debug) {
		fprintf(stderr, "writing\n");
		dump(buf, len);
	}

	write(uart_fd, buf, len);
}

void
expired(int sig)
{
	hci_send_cmd(hci_reset, sizeof(hci_reset));
	alarm(4);
}

void
proc_reset()
{
	signal(SIGALRM, expired);


	hci_send_cmd(hci_reset, sizeof(hci_reset));

	alarm(4);

	read_event(uart_fd, buffer);

	alarm(0);
}

void
proc_patchram()
{
	int len;

	hci_send_cmd(hci_download_minidriver, sizeof(hci_download_minidriver));

	read_event(uart_fd, buffer);

	read(uart_fd, &buffer[0], 2);

	usleep(50000);

	while (read(hcdfile_fd, &buffer[1], 3)) {
		buffer[0] = 0x01;

		len = buffer[3];

		read(hcdfile_fd, &buffer[4], len);

		hci_send_cmd(buffer, len + 4);

		read_event(uart_fd, buffer);
	}

	proc_reset();
}

void
proc_baudrate()
{
	hci_send_cmd(hci_update_baud_rate, sizeof(hci_update_baud_rate));

	read_event(uart_fd, buffer);

	cfsetospeed(&termios, termios_baudrate);
	cfsetispeed(&termios, termios_baudrate);
	tcsetattr(uart_fd, TCSANOW, &termios);

	if (debug) {
		fprintf(stderr, "Done setting baudrate\n");
	}
}

void
proc_bdaddr()
{
	hci_send_cmd(hci_write_bd_addr, sizeof(hci_write_bd_addr));

	read_event(uart_fd, buffer);
}

void
proc_enable_lpm()
{
	hci_send_cmd(hci_write_sleep_mode, sizeof(hci_write_sleep_mode));

	read_event(uart_fd, buffer);
}

void
proc_pcm_slave()
{
	if (debug)
		printf("Configuring PCM Interface as slave.\n");
	hci_send_cmd(hci_write_pcm_slave_mode, sizeof(hci_write_pcm_slave_mode));
}

void
proc_pcm_master()
{
	if (debug)
		printf("Configuring PCM Interface as master.\n");
	hci_send_cmd(hci_write_pcm_master_mode, sizeof(hci_write_pcm_master_mode));
}

void
proc_enable_hci()
{
	int i = N_HCI;
	int proto = HCI_UART_H4;
	if (ioctl(uart_fd, TIOCSETD, &i) < 0) {
		fprintf(stderr, "Can't set line discipline\n");
		return;
	}

	if (ioctl(uart_fd, HCIUARTSETPROTO, proto) < 0) {
		fprintf(stderr, "Can't set hci protocol\n");
		return;
	}
	fprintf(stderr, "Done setting line discpline\n");
	return;
}

void
read_default_bdaddr()
{
	int sz;
	int fd;
	char path[PROPERTY_VALUE_MAX];
	char bdaddr[18];
	int len = 17;
	memset(bdaddr, 0, (len + 1) * sizeof(char));

	property_get("ro.bt.bdaddr_path", path, "");
	if (path[0] == 0)
		return;

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "open(%s) failed: %s (%d)", path, strerror(errno),
				errno);
		return;
	}

	sz = read(fd, bdaddr, len);
	if (sz < 0) {
		fprintf(stderr, "read(%s) failed: %s (%d)", path, strerror(errno),
				errno);
		close(fd);
		return;
	} else if (sz != len) {
		fprintf(stderr, "read(%s) unexpected size %d", path, sz);
		close(fd);
		return;
	}

	if (debug)
		printf("Read default bdaddr of %s\n", bdaddr);
	parse_bdaddr(bdaddr);
}

int
main (int argc, char **argv)
{
	read_default_bdaddr();

	parse_cmd_line(argc, argv);

	if (uart_fd < 0) {
		exit(1);
	}

	init_uart();

	proc_reset();

	if (use_baudrate_for_download) {
		if (termios_baudrate) {
			proc_baudrate();
		}

		if (hcdfile_fd > 0) {
			proc_patchram();
		}
	} else {
		if (hcdfile_fd > 0) {
			proc_patchram();
		}

		if (termios_baudrate) {
			proc_baudrate();
		}
	}

	if (bdaddr_flag) {
		proc_bdaddr();
	}

	if (enable_lpm) {
		proc_enable_lpm();
	}

	if (pcm_slave) {
		proc_pcm_slave();
	} else if (pcm_master) {
		proc_pcm_master();
	}

	if (enable_hci) {
		proc_enable_hci();
		while (1) {
			sleep(UINT_MAX);
		}
	}

	exit(0);
}

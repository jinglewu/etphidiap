/*
 * Copyright 2021 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <errno.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <linux/i2c-dev.h>
#include <stdint.h>
#include <dirent.h>
#include <linux/types.h>
#include <linux/input.h>
#include <linux/hidraw.h>

#define INITIAL_VALUE -1
#define HID_INTERFACE 1
#define I2C_INTERFACE 2
#define HID_I2C_INTERFACE 3

#define VERSION "1.9"
#define VERSION_SUB "1"
/* Command line options */
static uint16_t vid = 0x04f3;			/* ELAN */
static uint16_t pid = 0x30C5;			/* B50  */
static uint16_t i2caddr = 0x15;			
//static uint16_t module_id = 0x0;	
		
static int hidraw_num = INITIAL_VALUE;	
static int i2c_num = INITIAL_VALUE;		
static uint8_t extended_i2c_exercise;		/* non-zero to exercise */
static char *firmware_binary = "elan_i2c.bin";	/* firmware blob */
#define ETP_I2C_IAP_CTRL_CMD		0x0310

/* Firmware binary blob related */
#define FW_PAGE_SIZE			64
#define MAX_FW_PAGE_COUNT		2048
#define MAX_FW_SIZE			(MAX_FW_PAGE_COUNT*FW_PAGE_SIZE)
#define FW_SIGNATURE_SIZE	6

static uint8_t fw_data[MAX_FW_SIZE];
int fw_page_count;
int fw_size;
int fw_size_all = 0;
static int fw_page_size;	
static int fw_section_size;
static int fw_section_cnt;
static int fw_no_of_sections;
static int iap_version = -1;
static int fw_version = -1;
static int fw_signature_address;

static int eeprom_driver_ic=-1;
static int eeprom_iap_version = -1;
/* Utility functions */
static int le_bytes_to_int(uint8_t *buf)
{
	return buf[0] + (int)(buf[1] << 8);
}

/* Command line parsing related */
static char *progname;
static char *short_opts = ":b:E:v:p:i:h:gdmzwa:CG";
static const struct option long_opts[] = {
	/* name    hasarg *flag val */
	{"bin",      1,   NULL, 'b'},
	{"eebin",      1,   NULL, 'E'},
	{"vid",      1,   NULL, 'v'},
	{"pid",      1,   NULL, 'p'},
	{"hidraw",   1,   NULL, 'h'},
	{"i2cnum",   1,   NULL, 'i'},
	{"i2caddr",   1,   NULL, 'a'},
	{"get_current_version",    0,   NULL, 'g'},
	{"get_module_id",    0,   NULL, 'm'},
	{"get_hardware_id",    0,   NULL, 'w'},
	{"get_eeprom_checksum",    0,   NULL, 'C'},
	{"get_eeprom_version",    0,   NULL, 'G'},
	{"help",     0,   NULL, '?'},
	{"version",    0,   NULL, 'z'},
	{"debug",    0,   NULL, 'd'},
	{NULL,       0,   NULL, 0},
};

static void usage(int errs)
{
	printf("\nUsage: %s [options]\n"
	       "\n"
	       "Firmware updater over HID for elan touchpad\n"
	       "\n"
	       "Options:\n"
	       "\n"
	       "  -b,--bin     STR          	Firmware binary (default %s)\n"
	       "  -E,--eebin   STR          	EEPROM Firmware binary \n"
	       "  -v,--vid     HEXVAL      	Vendor ID (default %04x)\n"
	       "  -p,--pid     HEXVAL      	Product ID (default %04x)\n"
	       "  -h,--hidraw  INT     		/dev/hidraw num\n"
	       "  -i,--i2cnum  INT     		/dev/i2c- num\n"
	       "  -a,--i2caddr HEXVAL     	i2c address (default %02x)\n"
	       "  -g,--get_current_version  	Get Firmware Version\n"
	       "  -m,--get_module_id  		Get Module ID\n"
	       "  -w,--get_hardware_id  	Get Hardward ID\n"
	       "  -C,--get_eeprom_checksum  	Get EEPROM Firmware Checksum\n"
	       "  -G,--get_eeprom_version  	Get EEPROM Firmware Version\n"
	       "  -d,--debug              	Exercise extended read I2C over HID\n"	
	       "  -z,--version              	Version\n"	
	       "  -?,--help               	Show this message\n"
	       "\n", progname, firmware_binary, vid, pid, i2caddr);

	exit(!!errs);
}
#define IAP_STATE  	 	0
#define GET_FWVER_STATE  	1
#define GET_BINVER_STATE 	2
#define GET_MODULEID_STATE 	3
#define GET_HWID_STATE 		4
#define GET_SWVER_STATE		5
#define EEPROM_IAP_STATE	6
#define GET_EEPROM_CHECKSUM	7
#define GET_EEPROM_VERSION	8
static int parse_cmdline(int argc, char *argv[])
{
	char *e = 0;
	int i, errorcnt = 0;
	int state = IAP_STATE;
	progname = strrchr(argv[0], '/');
	if (progname)
		progname++;
	else
		progname = argv[0];

	opterr = 0;				/* quiet, you */
	while ((i = getopt_long(argc, argv, short_opts, long_opts, 0)) != -1) {
		switch (i) {
		case 'b':
			firmware_binary = optarg;
			break;
		case 'E':
			firmware_binary = optarg;
			state = EEPROM_IAP_STATE;
			break;
		case 'p':
			pid = (uint16_t) strtoul(optarg, &e, 16);
			if (!*optarg || (e && *e)) {
				printf("Invalid argument: \"%s\"\n", optarg);
				errorcnt++;
			}
			break;
		case 'v':
			vid = (uint16_t) strtoul(optarg, &e, 16);
			if (!*optarg || (e && *e)) {
				printf("Invalid argument: \"%s\"\n", optarg);
				errorcnt++;
			}
			break;
		case 'h':
			hidraw_num = (int) strtoul(optarg, &e, 10);
			if (!*optarg || (e && *e)) {
				printf("Invalid argument: \"%s\"\n", optarg);
				errorcnt++;
			}
			break;
		case 'i':
			i2c_num  = (int) strtoul(optarg, &e, 10);
			if (!*optarg || (e && *e)) {
				printf("Invalid argument: \"%s\"\n", optarg);
				errorcnt++;
			}
			break;
		case 'a':
			i2caddr  = (int) strtoul(optarg, &e, 16);
			if (!*optarg || (e && *e)) {
				printf("Invalid argument: \"%s\"\n", optarg);
				errorcnt++;
			}
			break;
		case 'g':
			state = GET_FWVER_STATE;
			break;	
		case 'm':
			state = GET_MODULEID_STATE;
			break;
		case 'w':
			state = GET_HWID_STATE;
			break;	
		case 'C':
			state = GET_EEPROM_CHECKSUM;
			break;	
		case 'G':
			state = GET_EEPROM_VERSION;
			break;
		case 'd':
			extended_i2c_exercise = 1;
			break;
		/*case '?':
			usage(errorcnt);
			break;*/
		case 0:				/* auto-handled option */
			break;
		case 'z':			
			state = GET_SWVER_STATE;
			break;
		case '?':
			if (optopt)
				printf("Unrecognized option: -%c\n", optopt);
			else
				printf("Unrecognized option: %s\n",
				       argv[optind - 1]);
			errorcnt++;
			break;
		case ':':
			printf("Missing argument to %s\n", argv[optind - 1]);
			errorcnt++;
			break;
		default:
			printf("Internal error at %s:%d\n", __FILE__, __LINE__);
			exit(1);
		}
	}

	if (errorcnt)
		usage(errorcnt);
	return state;

}

/* HID transfer related */
static uint8_t rx_buf[1024];
static uint8_t tx_buf[1024];

static int do_exit;

static void request_exit(const char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	vfprintf(stderr, format, ap);
	va_end(ap);
	do_exit++;    /* Why need this ? */

	exit(1);
}

#define DIE(msg, r)							\
	request_exit("%s: line %d, %s\n", msg, __LINE__,		\
		     libusb_error_name(r))

#define LINUX_DEV_PATH                  "/dev/"
#define HID_RAW_NAME                    "hidraw"
#define I2C_NAME                    	"i2c-"

static int dev_fd;
static int bus_type = -1;
static int interface_type = -1;
static char raw_name[255];
static int is_new_pattern=0;
static int elan_read_cmd(int reg);
static int scan_i2c()
{
    DIR* FD;
    struct dirent* in_file;

    if (NULL == (FD = opendir ((char*)LINUX_DEV_PATH)))  {
        printf("scan_i2c : Open %s Error \n",(char*)LINUX_DEV_PATH);
        return -1;
    }

    while ((in_file = readdir(FD)))
    {
        if(strstr(in_file->d_name, (char*)I2C_NAME) == NULL) {
            continue;
        }

        char dev_name[262];
        sprintf(dev_name, "%s%s", (char*)LINUX_DEV_PATH,in_file->d_name);
	if(extended_i2c_exercise)
		printf("search i2c device name = %s\n",dev_name);
	if ((dev_fd = open(dev_name, O_RDWR)) < 0) {
            printf("Failed to open the i2c bus (%s).\n", dev_name);
            close(dev_fd);
            continue;
        }

        int addr = i2caddr ;
        if (ioctl(dev_fd, I2C_SLAVE, addr) < 0) {

            if (ioctl(dev_fd, I2C_SLAVE_FORCE, addr) >= 0)
            {
		interface_type = I2C_INTERFACE;
		if(extended_i2c_exercise)
			printf("i2c device: %s \n", dev_name);
                free(FD);	
                return 1;
            }
        }
        else
        {
	    tx_buf[0] = 0x02;
	    tx_buf[1] = 0x01;
	    int res = write(dev_fd, tx_buf, 2);
	    if(res < 0) {
		continue;
	    }
	    if(extended_i2c_exercise)
		printf("i2c device: %s \n", dev_name);
	    interface_type = I2C_INTERFACE;
            free(FD);	
            return 1;
        }
        close(dev_fd);
    }
    free(FD);	
    if(extended_i2c_exercise)
	printf("scan_i2c -1\n");	
    return -1;
}
static int scan_hid(int Vid, int Pid)
{
    DIR* FD;
    struct dirent* in_file;
    int res;
    struct hidraw_devinfo info;
    int tmp_fd;

    if (NULL == (FD = opendir ((char*)LINUX_DEV_PATH)))  {
        printf("scan_hid : Open %s Error \n",(char*)LINUX_DEV_PATH);
        return -1;
    }

    while ((in_file = readdir(FD)))
    {
        if(strstr(in_file->d_name, (char*)HID_RAW_NAME) == NULL) {
            continue;
        }

        char dev_name[262];
        sprintf(dev_name, "%s%s", (char*)LINUX_DEV_PATH,in_file->d_name);


        if ((tmp_fd = open(dev_name, O_RDWR|O_NONBLOCK)) < 0) {
            close(tmp_fd);
        }

        /* Get Raw Name */
	memset(raw_name,0,sizeof(raw_name));
        res = ioctl(tmp_fd, HIDIOCGRAWNAME(256), raw_name);
	if (res >= 0) {
            res = ioctl(tmp_fd, HIDIOCGRAWINFO, &info);
            if (res >= 0) {

                if((info.vendor==Vid)&&(info.product==Pid))
                {
                    bus_type = info.bustype;
                    vid = info.vendor;
                    pid = info.product;
	            if (extended_i2c_exercise) {
                    	printf("HID Raw Info\n");
			printf("Raw name: %s\n", raw_name);
                    	printf("Bus type: %d\n", bus_type);
		    }
		    dev_fd = tmp_fd;
		    interface_type = HID_INTERFACE;

		    if(elan_read_cmd(ETP_I2C_IAP_CTRL_CMD)>=0)
		    {
			free(FD);	
                    	return 1;
                    }
                }
            }

        }
    }
    free(FD);	
    return 0;
}
static int assign_hidraw()
{
        char dev_name[255];
        sprintf(dev_name, "%s%s%d", (char*)LINUX_DEV_PATH, 
			(char*)HID_RAW_NAME, hidraw_num);
	if ((dev_fd = open(dev_name, O_RDWR|O_NONBLOCK)) < 0) {
	    request_exit("Can't open hidraw%d.\n", hidraw_num);
            close(dev_fd);
	    return -1;
        }
	interface_type = HID_INTERFACE;
	return 0;

}
static int assign_i2c()
{
	char dev_name[255];
        sprintf(dev_name, "%s%s%d", (char*)LINUX_DEV_PATH, 
			(char*)I2C_NAME, i2c_num);

	if ((dev_fd = open(dev_name, O_RDWR)) < 0) {
            printf("Failed to open the i2c bus.");
            close(dev_fd);
            return -1;
        }
	int addr = i2caddr ;
        if (ioctl(dev_fd, I2C_SLAVE, addr) < 0) {

            if (ioctl(dev_fd, I2C_SLAVE_FORCE, addr) >= 0)
            {
                interface_type = I2C_INTERFACE;
                return 0;
            }
        }
        else
        {
            interface_type = I2C_INTERFACE;
            return 0;
        }
	
	return -1;
}
static int init_with_hid(void)
{

	if(scan_hid(vid, pid)){
        	return 0;
	}
    	else{		
	       	return -1;
	}
}
static int init_with_i2c(void)
{
	if(scan_i2c())
        	return 0 ;
    	else
	       	return -1;
}
static void init_elan_tp(void)
{
	if(hidraw_num!=INITIAL_VALUE){
		if(assign_hidraw()){
			request_exit("Can't find ELAN TP.\n");
		}
		else
			return;
	}
	if(i2c_num!=INITIAL_VALUE){
		if(assign_i2c()){
			request_exit("Can't find ELAN TP.\n");
		}
		else
			return;
	}
	if(init_with_hid()){
		if(init_with_i2c()){
			request_exit("Can't find ELAN TP.\n");
		}
	}
}

#define MAX_REC_SIZE 950

int hid_read_block(unsigned char *rx, int rx_length)
{
    int res;
    char *buf;
    buf = (char*)malloc(rx_length+1);
    memset(buf, 0x0, rx_length+1);

    buf[0] = 0xC; /* Report Number */
    res = ioctl(dev_fd, HIDIOCGFEATURE(rx_length), buf);
    if (res < 0) {
	free(buf);
	return -1;
    }
    else {

        res+=2;
        rx[0] = (res & 0xFF);
        rx[1] = (res >> 8) & 0xFF;
        memcpy(&rx[2],&buf[0], res );

        if(rx_length==MAX_REC_SIZE)
            rx_length=rx[0] + (int)(rx[1] << 8);
    }
    free(buf);	
    return 0;

}

#define ETP_I2C_INF_LENGTH		2

static int hid_send_cmd(
		unsigned char *tx, int tx_length, 
		unsigned char *rx, int rx_length)
{
    int res;
    char *buf;
    buf = (char*)malloc(rx_length+3);
    memset(buf, 0x0, rx_length+3);
	
    res = ioctl(dev_fd, HIDIOCSFEATURE(tx_length), tx);
    if (res < 0){
	if (extended_i2c_exercise) 
		printf("Error: hid_send_cmd %x %x (SET)", tx[3], tx[4]);
        free(buf);
	return -1;
    }
    if(rx_length<=0)
    {
        free(buf);
        return 0;
    }
    /* Get Feature */

    buf[0] = tx[0]; /* Report Number */
    res = ioctl(dev_fd, HIDIOCGFEATURE(rx_length+3), buf);
    if (res < 0){
       if (extended_i2c_exercise) 
		printf("Error: hid_send_cmd %x %x (GET)", tx[3], tx[4]);
       free(buf);
       return -1;
    }
    else{
        memcpy(&rx[0],&buf[3], rx_length );	
    }
    free(buf);
    return 0;
}

static int i2c_send_cmd(
		unsigned char *tx, int tx_length, 
		unsigned char *rx, int rx_length)
{
    int res;

    res = write(dev_fd, tx, tx_length);
    if (res < 0){
	if (extended_i2c_exercise) 
		printf("Error: i2c_send_cmd %x %x (SET)", tx[0], tx[1]);
	return -1;
    }

    if (rx_length<=0)
        return 0;

    res = read(dev_fd, rx, rx_length);
    if (res < 0){
       if (extended_i2c_exercise) 
		printf("Error: i2c_send_cmd %x %x (GET)", rx[0], rx[1]);
       return -1;
    }
    
    return 0;
}
static int i2c_send_cmd_2(
		unsigned char *tx, int tx_length, 
		unsigned char *rx, int rx_length)
{
    int res;
    char buf[13] = {0x05, 0x00, 0x3D, 0x03, 0x06, 0x0, 0x07,0x00, 0x0D, 0x0, 0x0, 0x0, 0x0};
    char buf2[6] = {0x05, 0x00, 0x3D, 0x02, 0x06, 0x0};
    char buf3[7] = {0};

    if(tx_length==4) {
	    buf[9] = tx[0];
	    buf[10] = tx[1];
	    buf[11] = tx[2];
	    buf[12] = tx[3];
    }
    else if(tx_length==2){

	    buf[9] = 0x05;
	    buf[10] = 0x03;
	    buf[11] = tx[0];
	    buf[12] = tx[1];
    }

    if((tx_length==2)||(tx_length==4)) 
    	res = write(dev_fd, buf, 13);
    else
	res = write(dev_fd, tx, tx_length);

    if (res < 0){
	if (extended_i2c_exercise) 
		printf("Error: i2c_send_cmd_2 %x %x (SET)", tx[0], tx[1]);
	return -1;
    }

    if (rx_length<=0)
        return 0; 
   
    res = write(dev_fd, buf2, 6);
    if (res < 0){
	if (extended_i2c_exercise) 
		printf("Error: i2c_send_cmd_2 %x %x (SET)", tx[0], tx[1]);
	return -2;
    }

    res = read(dev_fd, buf3, rx_length + 5);
    if (res < 0){
       if (extended_i2c_exercise) 
		printf("Error: i2c_send_cmd_2 %x %x (GET)", buf3[5], buf3[6]);
       return -3;
    }
    else{
	if(((buf3[3]&0xFF)==tx[0])&&((buf3[4]&0xFF)==tx[1]))
	{
		rx[0] = buf3[5];
		rx[1] = buf3[6];
	}
	else
	{
       	    	if (extended_i2c_exercise) 
			printf("Error: i2c_send_cmd_2 %x %x %x %x , %x %x(GET)", buf3[3], buf3[4], buf3[5], buf3[6], tx[0], tx[1]);
		return -4;
	}
    }
    return 0;
}

static int hid_read_cmd(
		unsigned char *tx, unsigned char *rx, 
		int rx_length)
{
    char buf[5];

    buf[0] = 0x0D;          /* Report Number */
    buf[1] = 0x05;
    buf[2] = 0x03;
    buf[3] = tx[0];
    buf[4] = tx[1];
    
    return hid_send_cmd((unsigned char*)buf, 5, rx, rx_length);

}
static int hid_write_cmd (unsigned char *tx, unsigned char *rx)
{
    char buf[5];

    buf[0] = 0x0D;          /* Report Number */
    buf[1] = tx[0];
    buf[2] = tx[1];
    buf[3] = tx[2];
    buf[4] = tx[3];

    return hid_send_cmd((unsigned char*)buf, 5, rx, 0);
}



static int elan_write_and_read(
		int reg, uint8_t *buf, int read_length,
		int with_cmd, int cmd)
{
	tx_buf[0] = (reg >> 0) & 0xff;
	tx_buf[1] = (reg >> 8) & 0xff;

	if (with_cmd) {
		tx_buf[2] = (cmd >> 0) & 0xff;
		tx_buf[3] = (cmd >> 8) & 0xff;
		if (interface_type==HID_INTERFACE)
			return hid_write_cmd(tx_buf,  buf);
		else if (interface_type==HID_I2C_INTERFACE)
			return i2c_send_cmd_2(tx_buf, 4, buf, 0);
		else
			return i2c_send_cmd(tx_buf, 4, buf, 0);
	}
	else
	{
		if (interface_type==HID_INTERFACE)
			return hid_read_cmd(tx_buf, buf, read_length);
		else if (interface_type==HID_I2C_INTERFACE)
			return i2c_send_cmd_2(tx_buf, 2, buf, read_length);
		else
			return i2c_send_cmd(tx_buf, 2, buf, read_length);
	}	

}

static int elan_read_cmd(int reg)
{
	return elan_write_and_read(reg, rx_buf, ETP_I2C_INF_LENGTH, 0, 0);
}

static int elan_write_cmd(int reg, int cmd)
{
	return elan_write_and_read(reg, rx_buf, 0, 1, cmd);
}

/* Elan trackpad firmware information related */
#define ETP_I2C_NEW_IAP_VERSION_CMD     0x0110
#define ETP_I2C_IAP_VERSION_CMD		0x0111
#define ETP_I2C_FW_VERSION_CMD		0x0102
#define ETP_I2C_IAP_CHECKSUM_CMD	0x0315
#define ETP_I2C_FW_CHECKSUM_CMD		0x030F
#define ETP_I2C_OSM_VERSION_CMD		0x0103
#define ETP_I2C_IAP_ICBODY_CMD          0x0110
#define ETP_GET_MODULE_ID_CMD           0x0101
#define ETP_GET_HARDWARE_ID_CMD		0x0100
#define ETP_I2C_FLIM_TYPE_ENABLE_CMD	0x0104
#define ETP_BIN_FILM_TYPE_TBL_ADDR	0x0683

#define ETP_FW_FLIM_TYPE_ENABLE_BIT	0x1
#define ETP_FW_EEPROM_ENABLE_BIT	0x2


static int elan_get_flim_type_addr()
{
    return le_bytes_to_int(fw_data + ETP_BIN_FILM_TYPE_TBL_ADDR * 2) * 2;
}

static int elan_get_flim_type_enable()
{
    	elan_read_cmd(ETP_I2C_FLIM_TYPE_ENABLE_CMD);

	if (interface_type==HID_INTERFACE) {
		if((rx_buf[0]==0x1)&&(rx_buf[1]==0x4)) {
			printf("Get flim type enable cmd fail.\n");
			return -1;
		}
	}
	else {
		if((rx_buf[0]==0xFF)&&(rx_buf[1]==0xFF)) {
			printf("Get flim type enable cmd fail.\n");
			return -1;
		}
	}
	if(rx_buf[0] & ETP_FW_FLIM_TYPE_ENABLE_BIT)
		return 1;
	else
		return 0;
}
static int elan_get_eeprom_enable()
{
    	elan_read_cmd(ETP_I2C_FLIM_TYPE_ENABLE_CMD);

	if (interface_type==HID_INTERFACE) {
		if((rx_buf[0]==0x1)&&(rx_buf[1]==0x4)) {
			printf("Get eeprom enable cmd fail.\n");
			return -1;
		}
	}
	else {
		if((rx_buf[0]==0xFF)&&(rx_buf[1]==0xFF)) {
			printf("Get eeprom enable cmd fail.\n");
			return -2;
		}
	}
	if((rx_buf[0] & ETP_FW_FLIM_TYPE_ENABLE_BIT)&&(rx_buf[0] & ETP_FW_EEPROM_ENABLE_BIT)) {
		eeprom_driver_ic = (rx_buf[0]  >> 4) & 0xF;
		return 1;
	}
	else
		return 0;
}
static int elan_get_version(int is_iap)
{
	uint16_t cmd;
	if (is_iap==0)
		cmd = ETP_I2C_FW_VERSION_CMD;
	else if (is_new_pattern == 0)
		cmd = ETP_I2C_IAP_VERSION_CMD;
	else
		cmd = ETP_I2C_NEW_IAP_VERSION_CMD;

	elan_read_cmd(cmd);
	int val = le_bytes_to_int(rx_buf);
	if (is_new_pattern >= 0x01)
		return is_iap ? rx_buf[1] : val;
	else
		return val;
}

static int elan_get_hardware_id()
{
    	elan_read_cmd(ETP_GET_HARDWARE_ID_CMD);
	return (int)rx_buf[0];
}

static int elan_get_checksum(int is_iap)
{
	elan_read_cmd(
		is_iap ? ETP_I2C_IAP_CHECKSUM_CMD : ETP_I2C_FW_CHECKSUM_CMD);
	return le_bytes_to_int(rx_buf);
}
//Version 1.5
#define ETP_GET_HID_ID_CMD                  0x0100
static int elan_get_patten()
{

    	elan_read_cmd(ETP_GET_HID_ID_CMD);
	int tmp = le_bytes_to_int(rx_buf);
	if(tmp==0xFFFF)
		return 0;
        else
		return (tmp& 0xFF00) >> 8; 
}
static uint16_t elan_get_fw_info(void)
{
	//int iap_version = -1;
	//int fw_version = -1;
	uint16_t iap_checksum = 0xffff;
	uint16_t fw_checksum = 0xffff;

	printf("Querying device info...\n");
	is_new_pattern = elan_get_patten();
	//printf("is_new_pattern = %x\n", is_new_pattern);
	fw_checksum = elan_get_checksum(0);
	iap_checksum = elan_get_checksum(1);
	fw_version = elan_get_version(0);
	iap_version = elan_get_version(1);
	printf("IAP  version: %4x, FW  version: %4x\n",
			iap_version, fw_version);
	printf("IAP checksum: %4x, FW checksum: %4x\n",
			iap_checksum, fw_checksum);

	return fw_checksum;
}

static int elan_get_module_id()
{
    	elan_read_cmd(ETP_GET_MODULE_ID_CMD);
	return le_bytes_to_int(rx_buf);
}


/* Update preparation */
#define ETP_I2C_IAP_RESET_CMD		0x0314
#define ETP_I2C_IAP_RESET		0xF0F0

#define ETP_I2C_MAIN_MODE_ON		(1 << 9)
#define ETP_I2C_IAP_CMD			0x0311
#define ETP_I2C_IAP_PASSWORD		0x1EA5
#define ETP_I2C_IAP_0A_PASSWORD		0xE15A

#define ETP_FW_IAP_LAST_FIT		(1 << 9)
#define ETP_FW_IAP_CHECK_PW		(1 << 7)
static uint8_t ic_type = 0;
/*
static int elan_in_main_mode(void)
{
	elan_read_cmd(ETP_I2C_IAP_CTRL_CMD);
	return le_bytes_to_int(rx_buf) & ETP_I2C_MAIN_MODE_ON;
}
*/
static int elan_get_iap_ctrl(void)
{
	elan_read_cmd(ETP_I2C_IAP_CTRL_CMD);
	return le_bytes_to_int(rx_buf);
}
static int elan_get_iap_icbody_interfacetype()
{
    	elan_read_cmd(ETP_I2C_IAP_ICBODY_CMD);
	return le_bytes_to_int(rx_buf);
}
static int elan_get_ic_type(void)
{
	elan_read_cmd(ETP_I2C_OSM_VERSION_CMD);
	int tmp = le_bytes_to_int(rx_buf);
	
	if((tmp==ETP_I2C_OSM_VERSION_CMD)||(tmp==0xFFFF))
		return (elan_get_iap_icbody_interfacetype() & 0xFF);
	return ((tmp >> 8) & 0xFF);
}



static int elan_get_ic_page_count(void)
{
	ic_type = elan_get_ic_type();
	
	switch (ic_type) {
	//case 0x00:
	case 0x06:
	case 0x08:
		return 512;
		break;
	case 0x03:
	case 0x07:
	case 0x09:
	case 0x0A:
	case 0x0B:
	case 0x0C:
		return 768;
		break;
	case 0x0D:
		return 896;
		break;
	case 0x0E:
		return 640;
		break;
	case 0x10:
		return 1024;
		break;
	case 0x11:
		return 1280;
		break;
	case 0x12:
	case 0x13:
		return 2048;
		break;
	case 0x14:
	case 0x15:
		return 1024;
		break;
	default:
		request_exit("The IC type is not supported (%x).\n", ic_type);
	}
	return -1;
}
static void elan_reset_tp(void)
{
	elan_write_cmd(ETP_I2C_IAP_RESET_CMD, ETP_I2C_IAP_RESET);
}


#define ETP_I2C_IAP_TYPE_REG               0x0040
#define ETP_I2C_IAP_TYPE_CMD               0x0304
static int elan_get_iap_type()
{
    	elan_read_cmd(ETP_I2C_IAP_TYPE_CMD);
	return le_bytes_to_int(rx_buf);
}

#define ETP_I2C_DISABLE_REPORT      0x0801
#define ETP_I2C_ENABLE_REPORT       0x0800
static void switch_to_ptpmode()
{
	if(elan_write_cmd(ETP_I2C_IAP_RESET_CMD, ETP_I2C_ENABLE_REPORT))
	{
		usleep(20 * 1000);
		if(elan_write_cmd(ETP_I2C_IAP_RESET_CMD, ETP_I2C_ENABLE_REPORT))
			printf("Can't enable TP report.\n");
	}
	if(elan_write_cmd(0x0306, 0x003))
	{
		usleep(20 * 1000);
		if(elan_write_cmd(0x0306, 0x003))
			printf("Can't switch to TP PTP mode.\n");
	}
}

static void disable_report()
{
	if(elan_write_cmd(ETP_I2C_IAP_RESET_CMD, ETP_I2C_DISABLE_REPORT))
		printf("Can't disable TP report.\n");
	usleep(50 * 1000);

}

static int check_fw_signature()
{

	static const uint8_t signature[] = {0xAA, 0x55, 0xCC, 0x33, 0xFF, 0xFF};
	/* Firmware file must match signature data */
	for(int i=0; i< sizeof(signature); i++)
	{
		if(fw_data[fw_signature_address+i]!=signature[i]) {
			printf("signature mismatch (expected %x, got %x)\n",signature[i], fw_data[fw_signature_address+i]);
			return -1;
		}
	}

	return 0;
}
static void elan_get_iap_fw_page_size(void)
{
	fw_page_size = 64; 
	fw_section_size = 64;
	fw_no_of_sections = 1;
    	if(ic_type>=0x10)
    	{
        	if(iap_version>=1)
        	{

            		if((iap_version>=2)&&((ic_type==0x14)||(ic_type==0x15)))
            		{
                		fw_page_size = 512;
				if(iap_version>=3)
				{
					fw_section_size = elan_get_iap_type() * 2;
					fw_no_of_sections = fw_page_size / fw_section_size;
				}
				else
					fw_section_size = 512;
            		}
            		else
			{
                		fw_page_size = 128; 
				fw_section_size = 128;
			}
			if(fw_section_size == fw_page_size) {
				elan_write_cmd(ETP_I2C_IAP_TYPE_CMD, fw_section_size / 2);
				int iap_type = elan_get_iap_type();
				if((iap_type & 0xFFFF)!= ((fw_section_size / 2)& 0xFFFF))
		    		{
		      
		        		elan_write_cmd(ETP_I2C_IAP_TYPE_CMD, fw_section_size / 2);
					iap_type = elan_get_iap_type();
		        		if((iap_type & 0xFFFF)!= ((fw_section_size / 2)& 0xFFFF))
		        		{			
						elan_reset_tp();
						switch_to_ptpmode();
						request_exit("Read/Wirte IAP Type Command FAIL!!\n");
		        		}
		    		}
			}


        	}
	}

}
static void elan_prepare_for_update(void)
{
	//if((elan_get_module_id()!=module_id) && (ic_type==0x13))
	//	request_exit("Unable to support this module.\n");

	if((ic_type==0x13)||(ic_type==0x12)) {

		if(elan_get_flim_type_enable()==1) {
			if(iap_version<=2) {
				switch_to_ptpmode();
				request_exit("Unable to support this iap version.\n");
			}
			else if(iap_version>=3) {
				int new_fw_size= elan_get_flim_type_addr();
				//printf("addr = %d %x\n", new_fw_size, new_fw_size);
				fw_size_all = fw_size;
				fw_size = new_fw_size - 1;
				fw_signature_address = new_fw_size - FW_SIGNATURE_SIZE;
				if(check_fw_signature()<0)
				{
					switch_to_ptpmode();
					request_exit("Firmware Signatrue FAIL.\n");
				}
			}
				
		}
	}
	int ctrl = elan_get_iap_ctrl();
	if (ctrl < 0) {
		switch_to_ptpmode();
		request_exit("In IAP mode, ReadIAPControl FAIL.\n");
		
    	}

    	if (((ctrl & 0xFFFF) != ETP_FW_IAP_LAST_FIT)) {
        	printf("In IAP mode, reset IC.\n");
        	elan_reset_tp();
	        usleep(30 * 1000);
    	}
	
	elan_get_iap_fw_page_size();
	if((ic_type & 0xFF) == 0x0A)
        	elan_write_cmd(ETP_I2C_IAP_CMD, ETP_I2C_IAP_0A_PASSWORD);
    	else
       	 	elan_write_cmd(ETP_I2C_IAP_CMD, ETP_I2C_IAP_PASSWORD);
	
	usleep(100 * 1000);

	ctrl = elan_get_iap_ctrl();

	if (ctrl < 0) {
		elan_reset_tp();
		switch_to_ptpmode();
		request_exit("In IAP mode, ReadIAPControl FAIL.\n");
	}

	if ((ctrl & ETP_FW_IAP_CHECK_PW) == 0){
		elan_reset_tp();
		switch_to_ptpmode();
		request_exit("Got an unexpected IAP password\n");
	}
}

/* Firmware block update */
#define ETP_IAP_START_ADDR		0x0083

static uint16_t elan_calc_checksum(uint8_t *data, int length)
{
	uint16_t checksum = 0;
	int i;
	for (i = 0; i < length; i += 2)
		checksum += ((uint16_t)(data[i+1]) << 8) | (data[i]);
	return checksum;
}
static uint16_t elan_eeprom_calc_checksum(uint8_t *data, int length)
{
	uint16_t checksum = 0;
	for (int i = 0; i < length; i ++)
		checksum += (data[i]);
	return checksum;
}
static int elan_get_iap_addr(void)
{
	return le_bytes_to_int(fw_data + ETP_IAP_START_ADDR * 2) * 2;
}

#define ETP_I2C_IAP_REG_L		0x01
#define ETP_I2C_IAP_REG_H		0x06

#define ETP_FW_IAP_PAGE_ERR		(1 << 5)
#define ETP_FW_IAP_INTF_ERR		(1 << 4)


static int i2c_write_fw_block(uint8_t *raw_data, uint16_t checksum)
{
    	unsigned char page_store[fw_section_size + 4];
    	int rv;
    	page_store[0] = ETP_I2C_IAP_REG_L;
    	page_store[1] = ETP_I2C_IAP_REG_H;
    	memcpy(page_store + 2, raw_data, fw_section_size);
    	page_store[fw_section_size + 2 + 0] = (checksum >> 0) & 0xff;
    	page_store[fw_section_size + 2 + 1] = (checksum >> 8) & 0xff;
	
	rv = i2c_send_cmd(
			page_store, sizeof(page_store), 0, 0);
	if (rv)
		return rv;

	if((fw_section_size == fw_page_size) || (fw_section_cnt == fw_no_of_sections))
	{
		if(fw_page_size == 512)
			usleep(50 *1000);
	    	else
			usleep(35 * 1000);
		
		elan_read_cmd(ETP_I2C_IAP_CTRL_CMD);
		rv = le_bytes_to_int(rx_buf);
		fw_section_cnt = 0;
		if (rv & (ETP_FW_IAP_PAGE_ERR | ETP_FW_IAP_INTF_ERR)) {
			printf("IAP reports failed write : %x\n", rv);
			fw_section_cnt++;
			return rv;
		}
	}
	fw_section_cnt++;
	return 0;
}

static int elan_enable_long_transmmison_mode()
{
    if(elan_write_cmd(0x0322, 0x4607)) {
	usleep(20 * 1000);
	return elan_write_cmd(0x0322, 0x4607);	
    }
    return 0;
}
static int elan_enable_eeprom_iap_mode()
{
    if(elan_write_cmd(0x0321, 0x0607)) {
	usleep(20 * 1000);
	return elan_write_cmd(0x0321, 0x0607);	
    }
    return 0;
}
static int elan_disable_long_transmmison_mode()
{
    if(elan_write_cmd(0x0322, 0x0000)) {
	usleep(20 * 1000);
	return elan_write_cmd(0x0322, 0x0000);	
    }
    return 0;
}
static int elan_disable_eeprom_iap_mode()
{
    if(elan_write_cmd(0x0321, 0x0606)) {
	usleep(20 * 1000);
	return elan_write_cmd(0x0321, 0x0606);	
    }
    return 0;
}
static int elan_set_eeprom_datatype()
{
    if(elan_write_cmd(0x0321, 0x0702)) {
	usleep(20 * 1000);
	return elan_write_cmd(0x0321, 0x0702);	
    }
    return 0;
}
static int elan_calc_eeprom_checksum()
{
    if(elan_write_cmd(0x0321, 0x060F)) {
	usleep(20 * 1000);
	return elan_write_cmd(0x0321, 0x060F);	
    }
    return 0;
}

static int elan_read_eeprom_checksum()
{
    if(elan_write_cmd(0x0321, 0x070A)) {
	usleep(20 * 1000);
	if(elan_write_cmd(0x0321, 0x070A)) {
		return -1;
	}	
    }
    elan_read_cmd(0x0321);
    return le_bytes_to_int(rx_buf);
}

static int elan_read_eeprom_checksum_process()
{
    int rv=elan_calc_eeprom_checksum();
    int cnt=0;
    if(rv<0)
    {
	printf( "Calc eeprom checksum cmd error..  \n");
        rv=-5;	
        return rv;

    }
wait:
    usleep(100 * 1000);
    rv = elan_set_eeprom_datatype();
    if(rv<0)
    {
	printf( "set eeprom datatype cmd error..  \n");
        rv=-6;
        return rv;
    }

    elan_read_cmd(0x0321);
    rv = le_bytes_to_int(rx_buf);
    if((rv & 0x20)==0x20)
    {	
	cnt++;
	if(cnt>=100)
	{
		printf( "Read eeprom checksum error.. (1) \n");
		rv=-7;
		return rv;
	}
	else
 		goto wait;
    }

    for(int i=0; i<3; i++)
    {
    	rv=elan_read_eeprom_checksum();
     	if(rv>0)
		i=3;
	else
		usleep(100 * 1000);

    }
    if(rv<0)
    {
	printf( "Read eeprom checksum error.. (2)  \n");
	rv=-8;
        return rv;
    }
    return rv;	
}

static int elan_read_eeprom_version()
{
    unsigned short v_s=0;
    unsigned short v_d=0;
    unsigned short v_m=0;
    unsigned short v_y=0;
    char buf[256] = "\0";
    
   if(elan_write_cmd(0x0321, 0x0710)) {
	usleep(20 * 1000);
	if(elan_write_cmd(0x0321, 0x0710))
		return -2;
    }
    elan_read_cmd(0x0321);
    v_d = rx_buf[0];
    v_m = rx_buf[1] & 0xF;
    v_s = (rx_buf[1] & 0xF0) >> 4;

    if(elan_write_cmd(0x0321, 0x0711)) {
	usleep(20 * 1000);
	if(elan_write_cmd(0x0321, 0x0711)) 
		return -3;
    }
    elan_read_cmd(0x0321);
    v_y = rx_buf[0];
    eeprom_iap_version = rx_buf[1];

    if((v_y==0xFF)||(v_m==0xFF)||(v_d==0xFF)||(v_s==0xFF))
	return 0;
    sprintf(buf, "%02d%02d%02d%02d", v_y, v_m, v_d, v_s);
    return atoi(buf);
}

static int elan_restart_driver_ic()
{
    if(elan_write_cmd(0x0321, 0x0601)) {
	usleep(20 * 1000);
	return elan_write_cmd(0x0321, 0x0601);	
    }
    return 0;
}

static int elan_write_info_eeprom_checksum(unsigned short checksum)
{
    if(elan_write_cmd(0x0322, 0x4600)) {
	usleep(20 * 1000);
	if(elan_write_cmd(0x0322, 0x4600)) {
		return -1;
	}
    }
    elan_read_cmd(0x0322);
    int ret = le_bytes_to_int(rx_buf);
    if((ret & 0xFFFF)!=0x4600)
    	return -2;
   
    if(elan_write_cmd(0x0311, 0x1EA5)) {
	usleep(20 * 1000);
	if(elan_write_cmd(0x0311, 0x1EA5)) 
		return -3;
    }
    if(elan_write_cmd(0x048b, checksum)) {
	usleep(20 * 1000);
	if(elan_write_cmd(0x048b, checksum)) 
		return -4;

    }
    if(elan_write_cmd(0x0322, 0x0000)) {
	usleep(20 * 1000);
	if(elan_write_cmd(0x0322, 0x0000)) 
		return -5;

    }
    elan_read_cmd(0x048b);
    ret = le_bytes_to_int(rx_buf);
    if((ret & 0xFFFF)!=checksum)
    	 return -6;
  
    return 1;

}

static int elan_get_eeprom_iap_ctrl(void)
{
    elan_read_cmd(0x0321);
    int ret = le_bytes_to_int(rx_buf);
    if((ret & 0x800)!=0x800)
    {
        printf("Error bit11 fail %x\n", ret);
        return -1;
    }
    if((ret & 0x1000)==0x1000)
    {
        printf("Error bit12 fail Resend %x\n", ret);      
        return 0;
    }
    return 1;

}

static int elan_eeprom_prepare_for_update()
{

    int ret = elan_get_eeprom_enable();
    if(ret <= 0)
    {
	printf("EEPROM is not Enable.(%x) !!\n", ret);
        return -2;
    }
  
    ret = elan_read_eeprom_version();
    if(ret < -1)
    {
	printf("Read EEPROM Version FAIL  (%d) !!\n", ret);
        return -1;
    }
    if((eeprom_driver_ic!=2)||(eeprom_iap_version!=1))
    {
	printf("Can't support this EEPROM IAP (%x,%x) !!\n", eeprom_driver_ic, eeprom_iap_version);
        return -3;
    }

    elan_get_iap_fw_page_size();

    for(int i=0; i<10; i++) {
	    ret = elan_enable_long_transmmison_mode();
	    if(ret < 0)
	    {
		printf("Long Transmmison mode FAIL  (%x) !!\n", ret);
		return -4;
	    }
	    ret = elan_enable_eeprom_iap_mode();
	    if(ret < 0)
	    {
		printf("Enable EEPROM IAP mode FAIL  (%x) !!\n", ret);
		return -5;
	    }

	    elan_read_cmd(0x0321);
	    ret = le_bytes_to_int(rx_buf);
	    if((ret & 0x800)!=0x800)
	    {
                ret=0;
		i=10;
	    }

    }
    if(ret < 0)
    {
	printf("Can't Enter EEPROM IAP mode (%x) !!\n", ret);
	return -6;
    }
    return 1;


}

static int i2c_write_eeprom_fw_block(int index, unsigned char *raw_data, unsigned short checksum, int eeprom_page_size)
{


    unsigned char page_store[fw_page_size*2 + 11];
    memset(page_store, 0 , sizeof(page_store));
    
    int rv;
    page_store[0]  = 0x05;
    page_store[1]  = 0x00;
    page_store[2]  = 0x3b;
    page_store[3]  = 0x03;
    page_store[4]  = 0x06;
    page_store[5]  = 0x00;
    page_store[6]  = fw_page_size*2 + 5;
    page_store[7]  = 0x00;
    page_store[8]  = 0x0B;
    page_store[9]  = eeprom_page_size+5;
    page_store[10] = 0xA2;
    page_store[11] = (index / 256);
    page_store[12] = (index % 256);
    memcpy(page_store + 13, raw_data, eeprom_page_size);
    page_store[eeprom_page_size + 13 + 0] = (checksum >> 8) & 0xff;
    page_store[eeprom_page_size + 13 + 1] = (checksum >> 0) & 0xff;

    rv = i2c_send_cmd(page_store, sizeof(page_store), NULL, 0);

    if (rv)
    	return rv;

    if(fw_page_size == 512)
	usleep(50 *1000);
    else
	usleep(35 * 1000);

    int ret = elan_set_eeprom_datatype();
    if(ret < 0)
    {
	printf("Set EEPROM DataType FAIL  (%x) !!\n", ret);
        return -1;
    }

    ret=elan_get_eeprom_iap_ctrl();
    if(ret==0)
    {
	printf("EEPROM IAP reports failed write %d\n", ret);
        return -1;
    }
    else if(ret==-1)
	return -2;
    
    return 0;
}
static int hid_write_eeprom_fw_block(int index, unsigned char *raw_data, unsigned short checksum, int eeprom_page_size)
{

    unsigned char page_store[fw_page_size*2 + 3];
    memset(page_store, 0 , sizeof(page_store));

    int rv;
    page_store[0] = 0x0B;
    page_store[1] = eeprom_page_size + 5;
    page_store[2] = 0xA2;
    page_store[3] = (index / 256);
    page_store[4] = (index % 256);
    memcpy(page_store + 5, raw_data,(eeprom_page_size));
    page_store[eeprom_page_size + 5 + 0] = (checksum >> 8) & 0xff;
    page_store[eeprom_page_size + 5 + 1] = (checksum >> 0) & 0xff;

    rv = hid_send_cmd(page_store, sizeof(page_store), NULL, 0);

    if (rv)
    	return rv;

    if(fw_page_size == 512)
	usleep(50 *1000);
    else
	usleep(35 * 1000);

    int ret = elan_set_eeprom_datatype();
    if(ret < 0)
    {
        printf("Set EEPROM DataType FAIL  (%x) !!\n", ret);
        return -1;
    }

    ret=elan_get_eeprom_iap_ctrl();
    if(ret==0)
    {
        printf("EEPROM IAP reports failed write %d\n", ret);
        return -1;
    }
    else if(ret==-1)
    	return -2;

    return 0;
}
static int hid_write_fw_block(uint8_t *raw_data, uint16_t checksum)
{
	uint8_t page_store[fw_section_size + 3];
	int rv;
	page_store[0] = 0x0B;   //Report ID
    	memcpy(page_store + 1, raw_data, fw_section_size);
    	page_store[fw_section_size + 1 + 0] = (checksum >> 0) & 0xff;
    	page_store[fw_section_size + 1 + 1] = (checksum >> 8) & 0xff;
	
	rv = hid_send_cmd(
			page_store, sizeof(page_store), 0, 0);
	if (rv)
		return rv;

	if((fw_section_size == fw_page_size) || (fw_section_cnt == fw_no_of_sections))
	{
		if(fw_page_size == 512)
			usleep(50 *1000);
	    	else
			usleep(35 * 1000);

		elan_read_cmd(ETP_I2C_IAP_CTRL_CMD);
		rv = le_bytes_to_int(rx_buf);
		//printf("rv - %d %x\n", rv,rv);	
		fw_section_cnt = 0;
		if (rv & (ETP_FW_IAP_PAGE_ERR | ETP_FW_IAP_INTF_ERR)) {
			printf("IAP reports failed write : %x\n", rv);
			fw_section_cnt++;
			return rv;
		}
		
	}
	fw_section_cnt++;
	return 0;
}

static int _elan_write_fw_block(uint8_t *raw_data, uint16_t checksum)
{
	if (interface_type==HID_INTERFACE)
		return hid_write_fw_block(raw_data, checksum);
	else
		return i2c_write_fw_block(raw_data, checksum);
}

static int elan_write_fw_block(uint8_t *raw_data, uint16_t checksum)
{	
	int rv;
	for(int i=0; i<10 ; i++) {
		rv = _elan_write_fw_block(raw_data, checksum);
		if(rv==0)
			return 0;
		printf("Retry(%d)..\n", i);
		usleep(50);
	}
	return rv;
}

static uint16_t elan_update_firmware(void)
{
	uint16_t checksum = 0, block_checksum;
	int rv, i;

	fw_section_cnt = 1;
	for (i = elan_get_iap_addr(); i < fw_size; i += fw_section_size) {
		block_checksum = elan_calc_checksum(fw_data + i, fw_section_size);
		rv = elan_write_fw_block(fw_data + i, block_checksum);
		checksum += block_checksum;
		printf("\rPage %3d is updated, checksum: %d, section: %d",
			i / fw_page_size, checksum, fw_section_cnt);
		fflush(stdout);
		if (rv)
			request_exit("Failed to update.");
	}

	// For ic_type 0x12 0x13, claculate all checksum.
	if(fw_size_all>0) {
		for(i = fw_size+1; i< fw_size_all; i+= fw_section_size) {
			block_checksum = elan_calc_checksum(fw_data + i, fw_section_size);
			checksum += block_checksum;
		}	
	}
	return checksum;
}
int finish_update_fw()
{
    int ret = elan_disable_long_transmmison_mode();
    if(ret < 0)
    {
	printf("Disable Long Transmmison mode FAIL  (%x) !!\n", ret);
        ret = -30;
    }
    ret = elan_disable_eeprom_iap_mode();
    if(ret < 0)
    {
        printf("Disable EEPROM IAP mode FAIL  (%x) !!\n", ret);
        ret = -31;	
    }
    return ret;
}

int eeprom_write_page(int index,  unsigned short *checksum, int page_size)
{
    unsigned short block_checksum;
    int rv;
    int error_count=0;
    uint8_t *fw_data2 = fw_data;
    uint8_t buffer[page_size];	
    
    if(index<0) {
	index = 0;
	memset(buffer, 0xFF , sizeof(buffer));
	fw_data2 = buffer;	
    }
    block_checksum = elan_eeprom_calc_checksum(fw_data2 + index, page_size);
    do
    {
	    if (interface_type==HID_INTERFACE)
	       	rv = hid_write_eeprom_fw_block(index, fw_data2 + index, block_checksum, page_size);
	    else
		rv = i2c_write_eeprom_fw_block(index, fw_data2 + index, block_checksum, page_size);

	    fflush(stdout);
	    if (rv==-1)
	    {
		error_count++;
	       	if(error_count<10)
		    	printf("\nRetry update page %d,  count = %d\n", index /page_size, error_count);
	       	else
	      		return -1;
	    }
	    else if(rv==-2)
		return -2;
	    else
	    {
		*checksum += block_checksum;
	      	printf("\rPage %3d is updated, block_checksum: %4x checksum: %4x",
		        index / page_size,  block_checksum, *checksum);
		return 0;
	    }
    }while(1);
}


static int elan_eeprom_update_firmware(void)
{
    int rv;
    int ret_prepare=elan_eeprom_prepare_for_update();
    usleep(100 * 1000);
    if(ret_prepare<0)
    {
	printf("-2 .prepare update fw error.. : return %d\n",ret_prepare);
		rv=-2;
        	goto exit;  
    }
    
    unsigned short check_sum=0;
    int eeprom_fw_page_size=32;

    for(int i=0; i<fw_size+1; i+= eeprom_fw_page_size)
    {
	//clear first page
	if(i==0) {
		rv =  eeprom_write_page(-1, &check_sum, eeprom_fw_page_size);
		check_sum=0;	
	}	
	// first page iap
	else if(i>=fw_size)
		rv =  eeprom_write_page(0, &check_sum, eeprom_fw_page_size);
	else
        	rv =  eeprom_write_page(i, &check_sum, eeprom_fw_page_size);
	if (rv<0)
	{
		printf("Failed to update. (%d)\n", rv);
		rv=-3;
        	goto exit;    
	}

    }
    rv=finish_update_fw();
    if(rv<0)
    {
	printf( "-4 .finish_update_fw error (%d)..\n", rv);
        rv=-4;
        goto exit;       	
    }

    usleep(2 * 1000);
    rv = elan_read_eeprom_checksum_process();
    if (rv != check_sum) {
	printf("Update FAIL: checksum diff local=[%04X], remote=[%04X]\n",
		check_sum, rv);
        goto exit;
    }
    for(int i=0; i<3; i++)
    {
    	rv = elan_write_info_eeprom_checksum(check_sum & 0xFFFF);
	if(rv>0)
		i=3;
    }
    if(rv<0)
	printf("Update PASS. (0x%04x) ; Informatin area record fail (%d)\n", check_sum, rv);
    else
	printf("Update PASS. (0x%04x)\n", check_sum);

exit:
    elan_restart_driver_ic();
    elan_reset_tp();
    
    usleep(1500 * 1000);
    return rv;
}
static void pretty_print_buffer(uint8_t *buf, int len)
{
	int i;

	printf("Buffer = 0x");
	for (i = 0; i < len; ++i)
		printf("%02X", buf[i]);
	printf("\n");
}


static int get_current_version()
{
	init_elan_tp();
	fw_version = elan_get_version(0);
	if((fw_version==ETP_I2C_FW_VERSION_CMD)||(fw_version==0xFFFF))
	{	
		printf("-1\n");
		return -1;
	}
	else
	{
		printf("%x\n", fw_version);
		return fw_version;
	}
	
}
static int get_module_id()
{
	int id;
	init_elan_tp();
	id = elan_get_module_id();
	printf("%x\n", id);
	return id;
	
}

static int get_hardware_id()
{
	int id;
	init_elan_tp();
	id = elan_get_hardware_id();
	printf("%x\n", id);
	return id;

}

static int get_eeprom_checksum()
{
    init_elan_tp();
    if (interface_type==I2C_INTERFACE)
	interface_type=HID_I2C_INTERFACE;

    int rv = elan_read_eeprom_checksum_process();
    if(rv<0)
	printf("%d\n", rv);
    else
	printf("%4x\n", rv);

    return rv;

}

static int get_eeprom_version()
{
    init_elan_tp();
    if (interface_type==I2C_INTERFACE)
	interface_type=HID_I2C_INTERFACE;

    int ret = elan_get_eeprom_enable();
    if(ret <= 0)
    {
	//printf("EEPROM is not Enable.(%x) !!\n", ret);
        printf("-2\n");
        return -2;
    }

    int rv=elan_read_eeprom_version();
    if(rv<0)
    {
	    usleep(100 * 1000);
	    rv=elan_read_eeprom_version();
	    if(rv<0) {
    		printf("%d\n", rv);
    		return rv;
	    } 
    }

    if((eeprom_driver_ic!=2)||(eeprom_iap_version!=1))
    {
	//printf("Can't support this EEPROM IAP (%x,%x) !!\n", eeprom_driver_ic, eeprom_iap_version);
	printf("-3\n");
        return -3;
    }

    printf("%x\n", rv);
    return rv;
}

int main(int argc, char *argv[])
{
	uint16_t local_checksum;
	uint16_t remote_checksum;

	int state=parse_cmdline(argc, argv);

	if(state==GET_FWVER_STATE)
	{		
		get_current_version();
		return 0;
	}
	else if(state==GET_MODULEID_STATE)
	{		
		get_module_id();
		return 0;
	}
	else if(state==GET_HWID_STATE)
	{		
		get_hardware_id();
		return 0;
	}	
	else if(state==GET_SWVER_STATE)
	{
		printf("Version: %s.%s\n", VERSION, VERSION_SUB);
		return 0;
	}
	else if(state==GET_EEPROM_CHECKSUM)
	{
		get_eeprom_checksum();
		return 0;
	}
	else if(state==GET_EEPROM_VERSION)
	{
		get_eeprom_version();
		return 0;
	}

	init_elan_tp();

	if (interface_type==HID_INTERFACE)
		printf("HID interface\n");
	else if (interface_type==I2C_INTERFACE)
		printf("I2C interface\n");
	else
		printf("Unknown interface\n");
	/*
	 * Judge IC type  and get page count first.
	 * Then check the FW file.
	 */
	/* Read the FW file */
	FILE *f = fopen(firmware_binary, "rb");

	if (!f) {
		switch_to_ptpmode();
		request_exit("Cannot find binary: %s\n", firmware_binary);
	}
	fseek (f , 0 , SEEK_END);
    	int bin_fw_size= (int)(ftell (f));
	rewind (f);

	if (fread(fw_data, 1, bin_fw_size, f) != (unsigned int)bin_fw_size) {
		switch_to_ptpmode();
		request_exit("binary size mismatch, expect %d\n", bin_fw_size);
	}
	/*
	 * It is possible that you are not able to get firmware info. This
	 * might due to an incomplete update last time
	 */

	disable_report();
	fw_page_count = elan_get_ic_page_count();

	if (state==EEPROM_IAP_STATE) {
		if (interface_type==I2C_INTERFACE)
			interface_type=HID_I2C_INTERFACE;
		fw_size = bin_fw_size;
		elan_eeprom_update_firmware();
		
	}
	else
	{
		//printf("IC page count is %04X\n", fw_page_count);	
		fw_size = fw_page_count * FW_PAGE_SIZE;
		//printf("fw_size %d\n", fw_size);	
		fw_signature_address = (fw_page_count * FW_PAGE_SIZE) - FW_SIGNATURE_SIZE;

		if(check_fw_signature()<0)
		{
			switch_to_ptpmode();
			return -1;
		}
	
		elan_get_fw_info();

		/* Trigger an I2C transaction of expecting reading of 633 bytes. */
		if (extended_i2c_exercise) {
			hid_read_block(rx_buf, 633);
			pretty_print_buffer(rx_buf, 637);
		}

		/* Get the trackpad ready for receiving update */
		elan_prepare_for_update();

		local_checksum = elan_update_firmware();
		/* Wait for a reset */
		usleep(1200 * 1000);
		remote_checksum = elan_get_checksum(1);
		if (remote_checksum != local_checksum)
			printf("checksum diff local=[%04X], remote=[%04X]\n",
					local_checksum, remote_checksum);
		printf("\n");
		/* Print the updated firmware information */
		elan_reset_tp();
	    	usleep(300);
		elan_get_fw_info();
	}
	switch_to_ptpmode();

	close(dev_fd);
	return 0;
}

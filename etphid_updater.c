/*
 * Copyright 2017 The Chromium OS Authors. All rights reserved.
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

/* Command line options */
static uint16_t vid = 0x04f3;			/* ELAN */
static uint16_t pid = 0x30C5;			/* B50  */
//static uint16_t module_id = 0x00bd;		/* B50 Module ID */
		
static int hidraw_num = INITIAL_VALUE;	
static int i2c_num = INITIAL_VALUE;		
static uint8_t extended_i2c_exercise;		/* non-zero to exercise */
static char *firmware_binary = "elan_i2c.bin";	/* firmware blob */

/* Firmware binary blob related */
#define FW_PAGE_SIZE			64
#define MAX_FW_PAGE_COUNT		1024
#define MAX_FW_SIZE			(MAX_FW_PAGE_COUNT*FW_PAGE_SIZE)

static uint8_t fw_data[MAX_FW_SIZE];
int fw_page_count;
int fw_size;

/* Utility functions */
static int le_bytes_to_int(uint8_t *buf)
{
	return buf[0] + (int)(buf[1] << 8);
}

/* Command line parsing related */
static char *progname;
static char *short_opts = ":b:v:p:i:h:gdmw";
static const struct option long_opts[] = {
	/* name    hasarg *flag val */
	{"bin",      1,   NULL, 'b'},
	{"vid",      1,   NULL, 'v'},
	{"pid",      1,   NULL, 'p'},
	{"hidraw",   1,   NULL, 'h'},
	{"i2cnum",   1,   NULL, 'i'},
	{"get_current_version",    0,   NULL, 'g'},
	{"get_module_id",    0,   NULL, 'm'},
	{"get_hardware_id",    0,   NULL, 'w'},
	{"help",     0,   NULL, '?'},
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
	       "  -b,--bin    STR          	Firmware binary (default %s)\n"
	       "  -v,--vid    HEXVAL      	Vendor ID (default %04x)\n"
	       "  -p,--pid    HEXVAL      	Product ID (default %04x)\n"
	       "  -h,--hidraw INT      		/dev/hidraw num\n"
	       "  -i,--i2cnum INT     		/dev/i2c- num\n"
	       "  -g,--get_current_version  	Get Firmware Version\n"
	       "  -m,--get_module_id  		Get Module ID\n"
               "  -w,--get_hardware_id  	Get Hardward ID\n"
	       "  -d,--debug              	Exercise extended read I2C over HID\n"	
	       "  -?,--help               	Show this message\n"
	       "\n", progname, firmware_binary, vid, pid);

	exit(!!errs);
}
#define IAP_STATE  	 	0
#define GET_FWVER_STATE  	1
#define GET_BINVER_STATE 	2
#define GET_MODULEID_STATE 	3
#define GET_HWID_STATE 		4
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
		case 'g':
			state = GET_FWVER_STATE;
			break;	
		case 'm':
			state = GET_MODULEID_STATE;
			break;	
		case 'w':
			state = GET_HWID_STATE;
			break;	
		case 'd':
			extended_i2c_exercise = 1;
			break;
		/*case '?':
			usage(errorcnt);
			break;*/
		case 0:				/* auto-handled option */
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
static int scan_i2c()
{
    DIR* FD;
    struct dirent* in_file;
    //int res;
    //struct hidraw_devinfo info;
    //int tmp_fd;

    if (NULL == (FD = opendir ((char*)LINUX_DEV_PATH)))  {
        printf("scan_i2c : Open %s Error \n",(char*)LINUX_DEV_PATH);
        return -1;
    }

    while ((in_file = readdir(FD)))
    {
        if(strstr(in_file->d_name, (char*)I2C_NAME) == NULL) {
            continue;
        }

        char dev_name[255];
        sprintf(dev_name, "%s%s", (char*)LINUX_DEV_PATH,in_file->d_name);
	if(extended_i2c_exercise)
		printf("search i2c device name = %s\n",dev_name);
	if ((dev_fd = open(dev_name, O_RDWR)) < 0) {

            //perror("Failed to open the i2c bus.");
            printf("Failed to open the i2c bus (%s).\n", dev_name);
            close(dev_fd);
            continue;
        }

        int addr = 0x15 ;
        if (ioctl(dev_fd, I2C_SLAVE, addr) < 0) {

            if (ioctl(dev_fd, I2C_SLAVE_FORCE, addr) >= 0)
            {
                //printf("reach i2c device %s \n", dev_name);
		interface_type = I2C_INTERFACE;
		if(extended_i2c_exercise)
			printf("i2c device: %s \n", dev_name);
                return 1;
            }
        }
        else
        {
            //printf("reach i2c device %s \n", dev_name);

	    tx_buf[0] = 0x02;
	    tx_buf[1] = 0x01;
	    int res = write(dev_fd, tx_buf, 2);
	    if(res < 0) {
		continue;
	    }
	    if(extended_i2c_exercise)
		printf("i2c device: %s \n", dev_name);
	    interface_type = I2C_INTERFACE;
            return 1;
        }
        close(dev_fd);
    }
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

        char dev_name[255];
        sprintf(dev_name, "%s%s", (char*)LINUX_DEV_PATH,in_file->d_name);


        if ((tmp_fd = open(dev_name, O_RDWR|O_NONBLOCK)) < 0) {
            close(tmp_fd);
        }

        /* Get Raw Name */
        res = ioctl(tmp_fd, HIDIOCGRAWNAME(256), raw_name);
        /*if (res < 0)
            //elan_log->WriteLine((char*)"Error: HIDIOCGRAWNAME");
        else*/
	if (res >= 0) {
            res = ioctl(tmp_fd, HIDIOCGRAWINFO, &info);
            /*if (res < 0) {
                //elan_log->WriteLine((char*)"Error: HIDIOCGRAWINFO");
            } else {*/
            if (res >= 0) {

                if((info.vendor==Vid)&&(info.product==Pid))
                {
                    bus_type = info.bustype;
                    vid = info.vendor;
                    pid = info.product;
	            if (extended_i2c_exercise) {
                    	printf("HID Raw Info\n");
			printf("Raw name: %s\n", raw_name);
                    	//printf("Bus type: %d\n", bus_type);
                    	//printf("VID: %x\n", vid);
                    	//printf("PID: %x\n", pid);
		    }
		    dev_fd = tmp_fd;
		    interface_type = HID_INTERFACE;
                    return 1;
                }
            }

        }

        //close(tmp_fd);
    }
    return 0;
}
static int assign_hidraw()
{
        char dev_name[255];
        sprintf(dev_name, "%s%s%d", (char*)LINUX_DEV_PATH, 
			(char*)HID_RAW_NAME, hidraw_num);
	//printf("dev_name = %s\n", dev_name);
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
	int addr = 0x15 ;
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
	//printf("init i2c interface\n");
	//printf("open_device %04x:%04x\n", vid, pid);
	if(scan_i2c()){
		//printf("elan tp i2c interface found\n");
        	return 0 ;
	}
    	else{
		//printf("Can't find i2c interface for elantp.\n");
	       	return -1;
	}
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
        //elan_log->WriteLine((char*)"Error: HIDIOCGFEATURE");
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
 
    return 0;

}
/* Control Elan trackpad I2C over USB */
#define ETP_I2C_INF_LENGTH		2

static int hid_send_cmd(
		unsigned char *tx, int tx_length, 
		unsigned char *rx, int rx_length)
{
    int res;
    char *buf;
    buf = (char*)malloc(rx_length+1);
    memset(buf, 0x0, rx_length+1);
	
    res = ioctl(dev_fd, HIDIOCSFEATURE(tx_length), tx);
    if (res < 0){
	if (extended_i2c_exercise) 
		printf("Error: hid_send_cmd %x %x (SET)", tx[3], tx[4]);
	return -1;
    }
    if(rx_length<=0)
        return 0;

    /* Get Feature */

    buf[0] = tx[0]; /* Report Number */
    res = ioctl(dev_fd, HIDIOCGFEATURE(rx_length+3), buf);
    if (res < 0){
       if (extended_i2c_exercise) 
		printf("Error: hid_send_cmd %x %x (GET)", tx[3], tx[4]);
       return -1;
    }
    else{
        memcpy(&rx[0],&buf[3], rx_length );	
    }
    return 0;
}

static int i2c_send_cmd(
		unsigned char *tx, int tx_length, 
		unsigned char *rx, int rx_length)
{
    int res;
    //char *buf;
    //buf = (char*)malloc(rx_length+1);
    //memset(buf, 0x0, rx_length+1);

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
		printf("Error: hid_send_cmd %x %x (GET)", tx[0], tx[1]);
       return -1;
    }
    else{
        //memcpy(&rx[0],&buf[0], rx_length );	
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
		else
			return i2c_send_cmd(tx_buf, 4, buf, 0);
	}
	else
	{
		if (interface_type==HID_INTERFACE)
			return hid_read_cmd(tx_buf, buf, read_length);
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


static int elan_get_version(int is_iap)
{
	uint16_t iap_ver_cmd = is_new_pattern ? ETP_I2C_NEW_IAP_VERSION_CMD : ETP_I2C_IAP_VERSION_CMD;

	elan_read_cmd(
		is_iap ? iap_ver_cmd : ETP_I2C_FW_VERSION_CMD);

	return le_bytes_to_int(rx_buf);
}

static int elan_get_checksum(int is_iap)
{
	elan_read_cmd(
		is_iap ? ETP_I2C_IAP_CHECKSUM_CMD : ETP_I2C_FW_CHECKSUM_CMD);
	return le_bytes_to_int(rx_buf);
}

static uint16_t elan_get_fw_info(void)
{
	int iap_version = -1;
	int fw_version = -1;
	uint16_t iap_checksum = 0xffff;
	uint16_t fw_checksum = 0xffff;

	printf("Querying device info...\n");
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
static int elan_get_hardware_id()
{
    	elan_read_cmd(ETP_GET_HARDWARE_ID_CMD);
	return (int)rx_buf[0];
}

/* Update preparation */
#define ETP_I2C_IAP_RESET_CMD		0x0314
#define ETP_I2C_IAP_RESET		0xF0F0
#define ETP_I2C_IAP_CTRL_CMD		0x0310
#define ETP_I2C_MAIN_MODE_ON		(1 << 9)
#define ETP_I2C_IAP_CMD			0x0311
#define ETP_I2C_IAP_PASSWORD		0x1EA5

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
	case 0x0a:
	case 0x09:
		return 768;
	case 0x0D:
		return 896;
	case 0x10:
		is_new_pattern=1;
		return 1024;
	default:
		request_exit("The IC type is not supported (%x).\n", ic_type);
	}
	return -1;
}
static void elan_reset_tp(void)
{
	elan_write_cmd(ETP_I2C_IAP_RESET_CMD, ETP_I2C_IAP_RESET);
}
static void elan_prepare_for_update(void)
{
	//if(elan_get_module_id()!=module_id)
	//	request_exit("Can't Support this module.\n");

	int ctrl = elan_get_iap_ctrl();
	if (ctrl < 0) {
		request_exit("In IAP mode, ReadIAPControl FAIL.\n");
		
    	}

    	if (((ctrl & 0xFFFF) != ETP_FW_IAP_LAST_FIT)) {
        	printf("In IAP mode, reset IC.\n");
        	elan_reset_tp();
	        usleep(30 * 1000);
    	}
	
	elan_write_cmd(ETP_I2C_IAP_CMD, ETP_I2C_IAP_PASSWORD);

	usleep(100 * 1000);


	ctrl = elan_get_iap_ctrl();

	if (ctrl < 0) {
		request_exit("In IAP mode, ReadIAPControl FAIL.\n");
	}

	if ((ctrl & ETP_FW_IAP_CHECK_PW) == 0){
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
    	unsigned char page_store[FW_PAGE_SIZE + 4];
    	int rv;
    	page_store[0] = ETP_I2C_IAP_REG_L;
    	page_store[1] = ETP_I2C_IAP_REG_H;
    	memcpy(page_store + 2, raw_data, FW_PAGE_SIZE);
    	page_store[FW_PAGE_SIZE + 2 + 0] = (checksum >> 0) & 0xff;
    	page_store[FW_PAGE_SIZE + 2 + 1] = (checksum >> 8) & 0xff;
	
	rv = i2c_send_cmd(
			page_store, sizeof(page_store), 0, 0);
	if (rv)
		return rv;
	usleep(35 * 1000);
	elan_read_cmd(ETP_I2C_IAP_CTRL_CMD);
	rv = le_bytes_to_int(rx_buf);
	if (rv & (ETP_FW_IAP_PAGE_ERR | ETP_FW_IAP_INTF_ERR)) {
		printf("IAP reports failed write : %x\n", rv);
		return rv;
	}
	return 0;
}
static int hid_write_fw_block(uint8_t *raw_data, uint16_t checksum)
{
	uint8_t page_store[FW_PAGE_SIZE + 3];
	int rv;
	page_store[0] = 0x0B;   //Report ID
    	memcpy(page_store + 1, raw_data, FW_PAGE_SIZE);
    	page_store[FW_PAGE_SIZE + 1 + 0] = (checksum >> 0) & 0xff;
    	page_store[FW_PAGE_SIZE + 1 + 1] = (checksum >> 8) & 0xff;
	
	rv = hid_send_cmd(
			page_store, sizeof(page_store), 0, 0);
	if (rv)
		return rv;
	usleep(35 * 1000);
	elan_read_cmd(ETP_I2C_IAP_CTRL_CMD);
	rv = le_bytes_to_int(rx_buf);
	if (rv & (ETP_FW_IAP_PAGE_ERR | ETP_FW_IAP_INTF_ERR)) {
		printf("IAP reports failed write : %x\n", rv);
		return rv;
	}
	return 0;
}

static int elan_write_fw_block(uint8_t *raw_data, uint16_t checksum)
{
	if (interface_type==HID_INTERFACE)
		return hid_write_fw_block(raw_data, checksum);
	else
		return i2c_write_fw_block(raw_data, checksum);
}

static uint16_t elan_update_firmware(void)
{
	uint16_t checksum = 0, block_checksum;
	int rv, i;

	for (i = elan_get_iap_addr(); i < fw_size; i += FW_PAGE_SIZE) {
		block_checksum = elan_calc_checksum(fw_data + i, FW_PAGE_SIZE);
		rv = elan_write_fw_block(fw_data + i, block_checksum);
		checksum += block_checksum;
		printf("\rPage %3d is updated, checksum: %d",
			i / FW_PAGE_SIZE, checksum);
		fflush(stdout);
		if (rv)
			request_exit("Failed to update.");
	}
	return checksum;
}

static void pretty_print_buffer(uint8_t *buf, int len)
{
	int i;

	printf("Buffer = 0x");
	for (i = 0; i < len; ++i)
		printf("%02X", buf[i]);
	printf("\n");
}

static int16_t fw_version=INITIAL_VALUE;
static int get_current_version()
{
	init_elan_tp();
	fw_version = elan_get_version(0);
	printf("%x\n", fw_version);
	return fw_version;
	/*
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
	*/
	
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

#define ETP_I2C_ENABLE_REPORT       0x0800
static void switch_to_ptpmode()
{
	if(elan_write_cmd(ETP_I2C_IAP_RESET_CMD, ETP_I2C_ENABLE_REPORT))
		printf("Can't enable TP report\n");
	if(elan_write_cmd(0x0306, 0x003))
		printf("Can't switch to TP PTP mode\n");
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
	fw_page_count = elan_get_ic_page_count();
	fw_size = fw_page_count * FW_PAGE_SIZE;
	printf("IC page count is %04X\n", fw_page_count);

	/* Read the FW file */
	FILE *f = fopen(firmware_binary, "rb");
	if (!f)
		request_exit("Cannot find binary: %s\n", firmware_binary);
	if (fread(fw_data, 1, fw_size, f) != (unsigned int)fw_size)
		request_exit("binary size mismatch, expect %d\n", fw_size);
	/*
	 * It is possible that you are not able to get firmware info. This
	 * might due to an incomplete update last time
	 */
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
	switch_to_ptpmode();

	close(dev_fd);
	return 0;
}

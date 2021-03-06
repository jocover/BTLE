// BTLE signal scanner by Xianjun Jiao (putaoshu@gmail.com)

/*
 * Copyright 2012 Jared Boone <jared@sharebrained.com>
 * Copyright 2013-2014 Benjamin Vernoux <titanmkd@gmail.com>
 *
 * This file is part of HackRF and bladeRF.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

#include "common.h"

#ifdef USE_BLADERF
#include <libbladeRF.h>
#else
#include <hackrf.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <math.h>
#include <getopt.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

//----------------------------------some sys stuff----------------------------------
#ifndef bool
typedef int bool;
#define true 1
#define false 0
#endif

#ifdef _WIN32
#include <windows.h>

#ifdef _MSC_VER

#ifdef _WIN64
typedef int64_t ssize_t;
#else
typedef int32_t ssize_t;
#endif

#define strtoull _strtoui64
#define snprintf _snprintf

int gettimeofday(struct timeval *tv, void* ignored)
{
	FILETIME ft;
	unsigned __int64 tmp = 0;
	if (NULL != tv) {
		GetSystemTimeAsFileTime(&ft);
		tmp |= ft.dwHighDateTime;
		tmp <<= 32;
		tmp |= ft.dwLowDateTime;
		tmp /= 10;
		tmp -= 11644473600000000Ui64;
		tv->tv_sec = (long)(tmp / 1000000UL);
		tv->tv_usec = (long)(tmp % 1000000UL);
	}
	return 0;
}

#endif
#endif

#if defined(__GNUC__)
#include <unistd.h>
#include <sys/time.h>
#endif

#include <signal.h>

#if defined _WIN32
	#define sleep(a) Sleep( (a*1000) )
#endif

static inline int
TimevalDiff(const struct timeval *a, const struct timeval *b)
{
   return( (a->tv_sec - b->tv_sec)*1000000 + (a->tv_usec - b->tv_usec) );
}

volatile bool do_exit = false;
#ifdef _MSC_VER
BOOL WINAPI
sighandler(int signum)
{
	if (CTRL_C_EVENT == signum) {
		fprintf(stdout, "Caught signal %d\n", signum);
		do_exit = true;
		return TRUE;
	}
	return FALSE;
}
#else
void sigint_callback_handler(int signum)
{
	fprintf(stdout, "Caught signal %d\n", signum);
	do_exit = true;
}
#endif

//----------------------------------some sys stuff----------------------------------

//----------------------------------print_usage----------------------------------
static void print_usage() {
	printf("Usage:\n");
  printf("    -h --help\n");
  printf("      print this help screen\n");
  printf("    -c --chan\n");
  printf("      channel number. default 37. valid range 0~39\n");
  printf("    -g --gain\n");
  printf("      rx gain in dB. HACKRF rxvga default 10, valid 0~62, lna in max gain. bladeRF default is max rx gain 66dB (valid 0~66)\n");
  printf("\nSee README for detailed information.\n");
}
//----------------------------------print_usage----------------------------------

//----------------------------------some basic signal definition----------------------------------
#define SAMPLE_PER_SYMBOL 4 // 4M sampling rate

volatile int rx_buf_offset; // remember to initialize it!

#define LEN_BUF_IN_SAMPLE (8*4096) //4096 samples = ~1ms for 4Msps; ATTENTION each rx callback get hackrf.c:lib_device->buffer_size samples!!!
#define LEN_BUF (LEN_BUF_IN_SAMPLE*2)
#define LEN_BUF_IN_SYMBOL (LEN_BUF_IN_SAMPLE/SAMPLE_PER_SYMBOL)
//----------------------------------some basic signal definition----------------------------------

//----------------------------------BTLE SPEC related--------------------------------
#include "scramble_table.h"
#define DEFAULT_CHANNEL 37
#define MAX_CHANNEL_NUMBER 39
#define MAX_NUM_INFO_BYTE (43)
#define MAX_NUM_PHY_BYTE (47)
//#define MAX_NUM_PHY_SAMPLE ((MAX_NUM_PHY_BYTE*8*SAMPLE_PER_SYMBOL)+(LEN_GAUSS_FILTER*SAMPLE_PER_SYMBOL))
#define MAX_NUM_PHY_SAMPLE (MAX_NUM_PHY_BYTE*8*SAMPLE_PER_SYMBOL)
#define LEN_BUF_MAX_NUM_PHY_SAMPLE (2*MAX_NUM_PHY_SAMPLE)

#define NUM_PREAMBLE_BYTE (1)
#define NUM_ACCESS_ADDR_BYTE (4)
#define NUM_PREAMBLE_ACCESS_BYTE (NUM_PREAMBLE_BYTE+NUM_ACCESS_ADDR_BYTE)
//----------------------------------BTLE SPEC related--------------------------------

//----------------------------------board specific operation----------------------------------

#ifdef USE_BLADERF //--------------------------------------BladeRF-----------------------
char *board_name = "BladeRF";
#define MAX_GAIN 66
#define DEFAULT_GAIN 66
typedef struct bladerf_devinfo bladerf_devinfo;
typedef struct bladerf bladerf_device;
typedef int16_t IQ_TYPE;
volatile IQ_TYPE rx_buf[LEN_BUF+LEN_BUF_MAX_NUM_PHY_SAMPLE];
static inline const char *backend2str(bladerf_backend b)
{
    switch (b) {
        case BLADERF_BACKEND_LIBUSB:
            return "libusb";
        case BLADERF_BACKEND_LINUX:
            return "Linux kernel driver";
        default:
            return "Unknown";
    }
}

int init_board(bladerf_device *dev, bladerf_devinfo *dev_info) {
  int n_devices = bladerf_get_device_list(&dev_info);

  if (n_devices < 0) {
    if (n_devices == BLADERF_ERR_NODEV) {
        printf("init_board: No bladeRF devices found.\n");
    } else {
        printf("init_board: Failed to probe for bladeRF devices: %s\n", bladerf_strerror(n_devices));
    }
		print_usage();
		return(-1);
  }

  printf("init_board: %d bladeRF devices found! The 1st one will be used:\n", n_devices);
  printf("    Backend:        %s\n", backend2str(dev_info[0].backend));
  printf("    Serial:         %s\n", dev_info[0].serial);
  printf("    USB Bus:        %d\n", dev_info[0].usb_bus);
  printf("    USB Address:    %d\n", dev_info[0].usb_addr);

  int fpga_loaded;
  int status = bladerf_open(&dev, NULL);
  if (status != 0) {
    printf("init_board: Failed to open bladeRF device: %s\n",
            bladerf_strerror(status));
    return(-1);
  }

  fpga_loaded = bladerf_is_fpga_configured(dev);
  if (fpga_loaded < 0) {
      printf("init_board: Failed to check FPGA state: %s\n",
                bladerf_strerror(fpga_loaded));
      status = -1;
      goto initialize_device_out_point;
  } else if (fpga_loaded == 0) {
      printf("init_board: The device's FPGA is not loaded.\n");
      status = -1;
      goto initialize_device_out_point;
  }

  unsigned int actual_sample_rate;
  status = bladerf_set_sample_rate(dev, BLADERF_MODULE_RX, SAMPLE_PER_SYMBOL*1000000ul, &actual_sample_rate);
  if (status != 0) {
      printf("init_board: Failed to set samplerate: %s\n",
              bladerf_strerror(status));
      goto initialize_device_out_point;
  }

  status = bladerf_set_frequency(dev, BLADERF_MODULE_RX, 2402000000ul);
  if (status != 0) {
      printf("init_board: Failed to set frequency: %s\n",
              bladerf_strerror(status));
      goto initialize_device_out_point;
  }

  unsigned int actual_frequency;
  status = bladerf_get_frequency(dev, BLADERF_MODULE_RX, &actual_frequency);
  if (status != 0) {
      printf("init_board: Failed to read back frequency: %s\n",
              bladerf_strerror(status));
      goto initialize_device_out_point;
  }

initialize_device_out_point:
  if (status != 0) {
      bladerf_close(dev);
      dev = NULL;
      return(-1);
  }

  #ifdef _MSC_VER
    SetConsoleCtrlHandler( (PHANDLER_ROUTINE) sighandler, TRUE );
  #else
    signal(SIGINT, &sigint_callback_handler);
    signal(SIGILL, &sigint_callback_handler);
    signal(SIGFPE, &sigint_callback_handler);
    signal(SIGSEGV, &sigint_callback_handler);
    signal(SIGTERM, &sigint_callback_handler);
    signal(SIGABRT, &sigint_callback_handler);
  #endif

  printf("init_board: set bladeRF to %f MHz %u sps BLADERF_LB_NONE.\n", (float)actual_frequency/1000000.0f, actual_sample_rate);
  return(0);
}

inline int open_board(uint64_t freq_hz, int gain, bladerf_device *dev) {
  int status;

  status = bladerf_set_frequency(dev, BLADERF_MODULE_RX, freq_hz);
  if (status != 0) {
    printf("open_board: Failed to set frequency: %s\n",
            bladerf_strerror(status));
    return(-1);
  }

  status = bladerf_set_gain(dev, BLADERF_MODULE_RX, gain);
  if (status != 0) {
    printf("open_board: Failed to set gain: %s\n",
            bladerf_strerror(status));
    return(-1);
  }

  status = bladerf_sync_config(dev, BLADERF_MODULE_RX, BLADERF_FORMAT_SC16_Q11, 2, LEN_BUF_IN_SAMPLE, 1, 3500);
  if (status != 0) {
     printf("open_board: Failed to configure sync interface: %s\n",
             bladerf_strerror(status));
     return(-1);
  }

  status = bladerf_enable_module(dev, BLADERF_MODULE_RX, true);
  if (status != 0) {
     printf("open_board: Failed to enable module: %s\n",
             bladerf_strerror(status));
     return(-1);
  }

  return(0);
}

inline int close_board(bladerf_device *dev) {
  // Disable TX module, shutting down our underlying TX stream
  int status = bladerf_enable_module(dev, BLADERF_MODULE_RX, false);
  if (status != 0) {
    printf("close_board: Failed to disable module: %s\n",
             bladerf_strerror(status));
    return(-1);
  }

  return(0);
}

void exit_board(bladerf_device *dev) {
  bladerf_close(dev);
  dev = NULL;
}

bladerf_device* config_run_board(uint64_t freq_hz, int gain, void **rf_dev) {
  bladerf_device *dev = NULL;
  return(dev);
}

void stop_close_board(bladerf_device* rf_dev){
  
}

#else //-----------------------------the board is HACKRF-----------------------------
char *board_name = "HACKRF";
#define MAX_GAIN 62
#define DEFAULT_GAIN 10
#define MAX_LNA_GAIN 40

typedef int8_t IQ_TYPE;
volatile IQ_TYPE rx_buf[LEN_BUF + LEN_BUF_MAX_NUM_PHY_SAMPLE];

int rx_callback(hackrf_transfer* transfer) {
  int i;
  int8_t *p = (int8_t *)transfer->buffer;
  for( i=0; i<transfer->valid_length; i++) {
    rx_buf[rx_buf_offset] = p[i];
    rx_buf_offset = (rx_buf_offset+1)&( LEN_BUF-1 ); //cyclic buffer
  }
  //printf("%d\n", transfer->valid_length); // !!!!it is 262144 always!!!! Now it is 4096. Defined in hackrf.c lib_device->buffer_size
  return(0);
}

int init_board() {
	int result = hackrf_init();
	if( result != HACKRF_SUCCESS ) {
		printf("open_board: hackrf_init() failed: %s (%d)\n", hackrf_error_name(result), result);
		print_usage();
		return(-1);
	}

  #ifdef _MSC_VER
    SetConsoleCtrlHandler( (PHANDLER_ROUTINE) sighandler, TRUE );
  #else
    signal(SIGINT, &sigint_callback_handler);
    signal(SIGILL, &sigint_callback_handler);
    signal(SIGFPE, &sigint_callback_handler);
    signal(SIGSEGV, &sigint_callback_handler);
    signal(SIGTERM, &sigint_callback_handler);
    signal(SIGABRT, &sigint_callback_handler);
  #endif

  return(0);
}

inline int open_board(uint64_t freq_hz, int gain, hackrf_device** device) {
  int result;

	result = hackrf_open(device);
	if( result != HACKRF_SUCCESS ) {
		printf("open_board: hackrf_open() failed: %s (%d)\n", hackrf_error_name(result), result);
    print_usage();
		return(-1);
	}

  result = hackrf_set_freq(*device, freq_hz);
  if( result != HACKRF_SUCCESS ) {
    printf("open_board: hackrf_set_freq() failed: %s (%d)\n", hackrf_error_name(result), result);
    print_usage();
    return(-1);
  }

  result = hackrf_set_sample_rate(*device, SAMPLE_PER_SYMBOL*1000000ul);
  if( result != HACKRF_SUCCESS ) {
    printf("open_board: hackrf_set_sample_rate() failed: %s (%d)\n", hackrf_error_name(result), result);
    print_usage();
    return(-1);
  }
  
  result = hackrf_set_baseband_filter_bandwidth(*device, SAMPLE_PER_SYMBOL*1000000ul/2);
  if( result != HACKRF_SUCCESS ) {
    printf("open_board: hackrf_set_baseband_filter_bandwidth() failed: %s (%d)\n", hackrf_error_name(result), result);
    print_usage();
    return(-1);
  }
  
  result = hackrf_set_vga_gain(*device, gain);
	result |= hackrf_set_lna_gain(*device, MAX_LNA_GAIN);
  if( result != HACKRF_SUCCESS ) {
    printf("open_board: hackrf_set_txvga_gain() failed: %s (%d)\n", hackrf_error_name(result), result);
    print_usage();
    return(-1);
  }

  return(0);
}

void exit_board(hackrf_device *device) {
	if(device != NULL)
	{
		hackrf_exit();
		printf("hackrf_exit() done\n");
	}
}

inline int close_board(hackrf_device *device) {
  int result;

	if(device != NULL)
	{
    result = hackrf_stop_rx(device);
    if( result != HACKRF_SUCCESS ) {
      printf("close_board: hackrf_stop_rx() failed: %s (%d)\n", hackrf_error_name(result), result);
      return(-1);
    }

		result = hackrf_close(device);
		if( result != HACKRF_SUCCESS )
		{
			printf("close_board: hackrf_close() failed: %s (%d)\n", hackrf_error_name(result), result);
			return(-1);
		}

    return(0);
	} else {
	  return(-1);
	}
}

inline int run_board(hackrf_device* device) {
  int result;

	result = hackrf_stop_rx(device);
	if( result != HACKRF_SUCCESS ) {
		printf("run_board: hackrf_stop_rx() failed: %s (%d)\n", hackrf_error_name(result), result);
		return(-1);
	}
  
  result = hackrf_start_rx(device, rx_callback, NULL);
  if( result != HACKRF_SUCCESS ) {
    printf("run_board: hackrf_start_rx() failed: %s (%d)\n", hackrf_error_name(result), result);
    return(-1);
  }

  return(0);
}

inline int config_run_board(uint64_t freq_hz, int gain, void **rf_dev) {
  hackrf_device *dev = NULL;
  
  (*rf_dev) = dev;
  
  if (init_board() != 0) {
    return(-1);
  }
  
  if ( open_board(freq_hz, gain, &dev) != 0 ) {
    (*rf_dev) = dev;
    return(-1);
  }

  (*rf_dev) = dev;
  if ( run_board(dev) != 0 ) {
    return(-1);
  }
  
  return(0);
}

void stop_close_board(hackrf_device* device){
  if (close_board(device)!=0){
    return;
  }
  exit_board(device);
}

#endif  //#ifdef USE_BLADERF
//----------------------------------board specific operation----------------------------------

//----------------------------------MISC MISC MISC----------------------------------
char* toupper_str(char *input_str, char *output_str) {
  int len_str = strlen(input_str);
  int i;

  for (i=0; i<=len_str; i++) {
    output_str[i] = toupper( input_str[i] );
  }

  return(output_str);
}

void octet_hex_to_bit(char *hex, char *bit) {
  char tmp_hex[3];

  tmp_hex[0] = hex[0];
  tmp_hex[1] = hex[1];
  tmp_hex[2] = 0;

  int n = strtol(tmp_hex, NULL, 16);

  bit[0] = 0x01&(n>>0);
  bit[1] = 0x01&(n>>1);
  bit[2] = 0x01&(n>>2);
  bit[3] = 0x01&(n>>3);
  bit[4] = 0x01&(n>>4);
  bit[5] = 0x01&(n>>5);
  bit[6] = 0x01&(n>>6);
  bit[7] = 0x01&(n>>7);
}

void int_to_bit(int n, uint8_t *bit) {
  bit[0] = 0x01&(n>>0);
  bit[1] = 0x01&(n>>1);
  bit[2] = 0x01&(n>>2);
  bit[3] = 0x01&(n>>3);
  bit[4] = 0x01&(n>>4);
  bit[5] = 0x01&(n>>5);
  bit[6] = 0x01&(n>>6);
  bit[7] = 0x01&(n>>7);
}

void byte_array_to_bit_array(uint8_t *byte_in, int num_byte, uint8_t *bit) {
  int i, j;
  j=0;
  for(i=0; i<num_byte*8; i=i+8) {
    int_to_bit(byte_in[j], bit+i);
    j++;
  }
}

int convert_hex_to_bit(char *hex, char *bit){
  int num_hex = strlen(hex);
  while(hex[num_hex-1]<=32 || hex[num_hex-1]>=127) {
    num_hex--;
  }

  if (num_hex%2 != 0) {
    printf("convert_hex_to_bit: Half octet is encountered! num_hex %d\n", num_hex);
    printf("%s\n", hex);
    return(-1);
  }

  int num_bit = num_hex*4;

  int i, j;
  for (i=0; i<num_hex; i=i+2) {
    j = i*4;
    octet_hex_to_bit(hex+i, bit+j);
  }

  return(num_bit);
}

void disp_bit(char *bit, int num_bit)
{
  int i, bit_val;
  for(i=0; i<num_bit; i++) {
    bit_val = bit[i];
    if (i%8 == 0 && i != 0) {
      printf(" ");
    } else if (i%4 == 0 && i != 0) {
      printf("-");
    }
    printf("%d", bit_val);
  }
  printf("\n");
}

void disp_bit_in_hex(char *bit, int num_bit)
{
  int i, a;
  for(i=0; i<num_bit; i=i+8) {
    a = bit[i] + bit[i+1]*2 + bit[i+2]*4 + bit[i+3]*8 + bit[i+4]*16 + bit[i+5]*32 + bit[i+6]*64 + bit[i+7]*128;
    //a = bit[i+7] + bit[i+6]*2 + bit[i+5]*4 + bit[i+4]*8 + bit[i+3]*16 + bit[i+2]*32 + bit[i+1]*64 + bit[i]*128;
    printf("%02x", a);
  }
  printf("\n");
}

void disp_hex(uint8_t *hex, int num_hex)
{
  int i;
  for(i=0; i<num_hex; i++)
  {
     printf("%02x", hex[i]);
  }
  printf("\n");
}

void disp_hex_in_bit(uint8_t *hex, int num_hex)
{
  int i, j, bit_val;

  for(j=0; j<num_hex; j++) {

    for(i=0; i<8; i++) {
      bit_val = (hex[j]>>i)&0x01;
      if (i==4) {
        printf("-");
      }
      printf("%d", bit_val);
    }

    printf(" ");

  }

  printf("\n");
}

void save_phy_sample(IQ_TYPE *IQ_sample, int num_IQ_sample, char *filename)
{
  int i;

  FILE *fp = fopen(filename, "w");
  if (fp == NULL) {
    printf("save_phy_sample: fopen failed!\n");
    return;
  }

  for(i=0; i<num_IQ_sample; i++) {
    if (i%64 == 0) {
      fprintf(fp, "\n");
    }
    fprintf(fp, "%d, ", IQ_sample[i]);
  }
  fprintf(fp, "\n");

  fclose(fp);
}

void load_phy_sample(IQ_TYPE *IQ_sample, int num_IQ_sample, char *filename)
{
  int i, tmp_val;

  FILE *fp = fopen(filename, "r");
  if (fp == NULL) {
    printf("load_phy_sample: fopen failed!\n");
    return;
  }

  i = 0;
  while( ~feof(fp) ) {
    if ( fscanf(fp, "%d,", &tmp_val) ) {
      IQ_sample[i] = tmp_val;
      i++;
    }
    if (num_IQ_sample != -1) {
      if (i==num_IQ_sample) {
        break;
      }
    }
    //printf("%d\n", i);
  }
  printf("%d I/Q are read.\n", i);

  fclose(fp);
}

void save_phy_sample_for_matlab(IQ_TYPE *IQ_sample, int num_IQ_sample, char *filename)
{
  int i;

  FILE *fp = fopen(filename, "w");
  if (fp == NULL) {
    printf("save_phy_sample_for_matlab: fopen failed!\n");
    return;
  }

  for(i=0; i<num_IQ_sample; i++) {
    if (i%64 == 0) {
      fprintf(fp, "...\n");
    }
    fprintf(fp, "%d ", IQ_sample[i]);
  }
  fprintf(fp, "\n");

  fclose(fp);
}
//----------------------------------MISC MISC MISC----------------------------------

//----------------------------------BTLE SPEC related--------------------------------
/**
 * Static table used for the table_driven implementation.
 *****************************************************************************/
static const uint_fast32_t crc_table[256] = {
    0x000000, 0x01b4c0, 0x036980, 0x02dd40, 0x06d300, 0x0767c0, 0x05ba80, 0x040e40,
    0x0da600, 0x0c12c0, 0x0ecf80, 0x0f7b40, 0x0b7500, 0x0ac1c0, 0x081c80, 0x09a840,
    0x1b4c00, 0x1af8c0, 0x182580, 0x199140, 0x1d9f00, 0x1c2bc0, 0x1ef680, 0x1f4240,
    0x16ea00, 0x175ec0, 0x158380, 0x143740, 0x103900, 0x118dc0, 0x135080, 0x12e440,
    0x369800, 0x372cc0, 0x35f180, 0x344540, 0x304b00, 0x31ffc0, 0x332280, 0x329640,
    0x3b3e00, 0x3a8ac0, 0x385780, 0x39e340, 0x3ded00, 0x3c59c0, 0x3e8480, 0x3f3040,
    0x2dd400, 0x2c60c0, 0x2ebd80, 0x2f0940, 0x2b0700, 0x2ab3c0, 0x286e80, 0x29da40,
    0x207200, 0x21c6c0, 0x231b80, 0x22af40, 0x26a100, 0x2715c0, 0x25c880, 0x247c40,
    0x6d3000, 0x6c84c0, 0x6e5980, 0x6fed40, 0x6be300, 0x6a57c0, 0x688a80, 0x693e40,
    0x609600, 0x6122c0, 0x63ff80, 0x624b40, 0x664500, 0x67f1c0, 0x652c80, 0x649840,
    0x767c00, 0x77c8c0, 0x751580, 0x74a140, 0x70af00, 0x711bc0, 0x73c680, 0x727240,
    0x7bda00, 0x7a6ec0, 0x78b380, 0x790740, 0x7d0900, 0x7cbdc0, 0x7e6080, 0x7fd440,
    0x5ba800, 0x5a1cc0, 0x58c180, 0x597540, 0x5d7b00, 0x5ccfc0, 0x5e1280, 0x5fa640,
    0x560e00, 0x57bac0, 0x556780, 0x54d340, 0x50dd00, 0x5169c0, 0x53b480, 0x520040,
    0x40e400, 0x4150c0, 0x438d80, 0x423940, 0x463700, 0x4783c0, 0x455e80, 0x44ea40,
    0x4d4200, 0x4cf6c0, 0x4e2b80, 0x4f9f40, 0x4b9100, 0x4a25c0, 0x48f880, 0x494c40,
    0xda6000, 0xdbd4c0, 0xd90980, 0xd8bd40, 0xdcb300, 0xdd07c0, 0xdfda80, 0xde6e40,
    0xd7c600, 0xd672c0, 0xd4af80, 0xd51b40, 0xd11500, 0xd0a1c0, 0xd27c80, 0xd3c840,
    0xc12c00, 0xc098c0, 0xc24580, 0xc3f140, 0xc7ff00, 0xc64bc0, 0xc49680, 0xc52240,
    0xcc8a00, 0xcd3ec0, 0xcfe380, 0xce5740, 0xca5900, 0xcbedc0, 0xc93080, 0xc88440,
    0xecf800, 0xed4cc0, 0xef9180, 0xee2540, 0xea2b00, 0xeb9fc0, 0xe94280, 0xe8f640,
    0xe15e00, 0xe0eac0, 0xe23780, 0xe38340, 0xe78d00, 0xe639c0, 0xe4e480, 0xe55040,
    0xf7b400, 0xf600c0, 0xf4dd80, 0xf56940, 0xf16700, 0xf0d3c0, 0xf20e80, 0xf3ba40,
    0xfa1200, 0xfba6c0, 0xf97b80, 0xf8cf40, 0xfcc100, 0xfd75c0, 0xffa880, 0xfe1c40,
    0xb75000, 0xb6e4c0, 0xb43980, 0xb58d40, 0xb18300, 0xb037c0, 0xb2ea80, 0xb35e40,
    0xbaf600, 0xbb42c0, 0xb99f80, 0xb82b40, 0xbc2500, 0xbd91c0, 0xbf4c80, 0xbef840,
    0xac1c00, 0xada8c0, 0xaf7580, 0xaec140, 0xaacf00, 0xab7bc0, 0xa9a680, 0xa81240,
    0xa1ba00, 0xa00ec0, 0xa2d380, 0xa36740, 0xa76900, 0xa6ddc0, 0xa40080, 0xa5b440,
    0x81c800, 0x807cc0, 0x82a180, 0x831540, 0x871b00, 0x86afc0, 0x847280, 0x85c640,
    0x8c6e00, 0x8ddac0, 0x8f0780, 0x8eb340, 0x8abd00, 0x8b09c0, 0x89d480, 0x886040,
    0x9a8400, 0x9b30c0, 0x99ed80, 0x985940, 0x9c5700, 0x9de3c0, 0x9f3e80, 0x9e8a40,
    0x972200, 0x9696c0, 0x944b80, 0x95ff40, 0x91f100, 0x9045c0, 0x929880, 0x932c40
};

uint64_t get_freq_by_channel_number(int channel_number) {
  uint64_t freq_hz;
  if ( channel_number == 37 ) {
    freq_hz = 2402000000ull;
  } else if (channel_number == 38) {
    freq_hz = 2426000000ull;
  } else if (channel_number == 39) {
    freq_hz = 2480000000ull;
  } else if (channel_number >=0 && channel_number <= 10 ) {
    freq_hz = 2404000000ull + channel_number*2000000ull;
  } else if (channel_number >=11 && channel_number <= 36 ) {
    freq_hz = 2428000000ull + (channel_number-11)*2000000ull;
  } else {
    freq_hz = 0xffffffffffffffff;
  }
  return(freq_hz);
}

typedef enum
{
    ADV_IND,
    ADV_DIRECT_IND,
    ADV_NONCONN_IND,
    SCAN_REQ,
    SCAN_RSP,
    CONNECT_REQ,
    ADV_SCAN_IND,
    RESERVED0,
    RESERVED1,
    RESERVED2,
    RESERVED3,
    RESERVED4,
    RESERVED5,
    RESERVED6,
    RESERVED7,
    RESERVED8
} PDU_TYPE;

typedef struct {
  uint8_t AdvA[6];
  uint8_t Data[31];
} ADV_PDU_PAYLOAD_TYPE_0_2_4_6;

typedef struct {
  uint8_t A0[6];
  uint8_t A1[6];
} ADV_PDU_PAYLOAD_TYPE_1_3;

typedef struct {
  uint8_t InitA[6];
  uint8_t AdvA[6];
  uint8_t AA[4];
  uint32_t CRCInit;
  uint8_t WinSize;
  uint16_t WinOffset;
  uint16_t Interval;
  uint16_t Latency;
  uint16_t Timeout;
  uint8_t ChM[5];
  uint8_t Hop;
  uint8_t SCA;
} ADV_PDU_PAYLOAD_TYPE_5;

typedef struct {
  uint8_t payload_byte[37];
} ADV_PDU_PAYLOAD_TYPE_R;

char *PDU_TYPE_STR[] = {
    "ADV_IND",
    "ADV_DIRECT_IND",
    "ADV_NONCONN_IND",
    "SCAN_REQ",
    "SCAN_RSP",
    "CONNECT_REQ",
    "ADV_SCAN_IND",
    "RESERVED0",
    "RESERVED1",
    "RESERVED2",
    "RESERVED3",
    "RESERVED4",
    "RESERVED5",
    "RESERVED6",
    "RESERVED7",
    "RESERVED8"
};

/**
 * Update the crc value with new data.
 *
 * \param crc      The current crc value.
 * \param data     Pointer to a buffer of \a data_len bytes.
 * \param data_len Number of bytes in the \a data buffer.
 * \return         The updated crc value.
 *****************************************************************************/
uint_fast32_t crc_update(uint_fast32_t crc, const void *data, size_t data_len) {
    const unsigned char *d = (const unsigned char *)data;
    unsigned int tbl_idx;

    while (data_len--) {
            tbl_idx = (crc ^ *d) & 0xff;
            crc = (crc_table[tbl_idx] ^ (crc >> 8)) & 0xffffff;

        d++;
    }
    return crc & 0xffffff;
}

uint_fast32_t crc24_byte(uint8_t *byte_in, int num_byte, int init_hex) {
  uint_fast32_t crc = init_hex;

  crc = crc_update(crc, byte_in, num_byte);

  return(crc);
}

void scramble_byte(uint8_t *byte_in, int num_byte, const uint8_t *scramble_table_byte, uint8_t *byte_out) {
  int i;
  for(i=0; i<num_byte; i++){
    byte_out[i] = byte_in[i]^scramble_table_byte[i];
  }
}
//----------------------------------BTLE SPEC related----------------------------------

//----------------------------------command line parameters----------------------------------
// Parse the command line arguments and return optional parameters as
// variables.
// Also performs some basic sanity checks on the parameters.
void parse_commandline(
  // Inputs
  int argc,
  char * const argv[],
  // Outputs
  int* chan,
  int* gain
) {
  printf("BTLE/BT4.0 Scanner(NO bladeRF support so far). Xianjun Jiao. putaoshu@gmail.com\n\n");
  
  // Default values
  (*chan) = DEFAULT_CHANNEL;

  (*gain) = DEFAULT_GAIN;

  while (1) {
    static struct option long_options[] = {
      {"help",         no_argument,       0, 'h'},
      {"chan",   required_argument, 0, 'c'},
      {"gain",         required_argument, 0, 'g'},
      {0, 0, 0, 0}
    };
    /* getopt_long stores the option index here. */
    int option_index = 0;
    int c = getopt_long (argc, argv, "hc:g:",
                     long_options, &option_index);

    /* Detect the end of the options. */
    if (c == -1)
      break;

    switch (c) {
      char * endp;
      case 0:
        // Code should only get here if a long option was given a non-null
        // flag value.
        printf("Check code!\n");
        goto abnormal_quit;
        break;
        
      case 'h':
        goto abnormal_quit;
        break;
        
      case 'c':
        (*chan) = strtol(optarg,&endp,10);
        break;
        
      case 'g':
        (*gain) = strtol(optarg,&endp,10);
        break;
        
      case '?':
        /* getopt_long already printed an error message. */
        goto abnormal_quit;
        
      default:
        goto abnormal_quit;
    }
    
  }

  if ( (*chan)<0 || (*chan)>MAX_CHANNEL_NUMBER ) {
    printf("channel number must be within 0~%d!\n", MAX_CHANNEL_NUMBER);
    goto abnormal_quit;
  }
  
  if ( (*gain)<0 || (*gain)>MAX_GAIN ) {
    printf("rx gain must be within 0~%d!\n", MAX_GAIN);
    goto abnormal_quit;
  }
  
  // Error if extra arguments are found on the command line
  if (optind < argc) {
    printf("Error: unknown/extra arguments specified on command line\n");
    goto abnormal_quit;
  }

  return;
  
abnormal_quit:
  print_usage();
  exit(-1);
}
//----------------------------------command line parameters----------------------------------

//----------------------------------receiver----------------------------------

//#define LEN_DEMOD_BUF_PREAMBLE_ACCESS ( (NUM_PREAMBLE_ACCESS_BYTE*8)-8 ) // to get 2^x integer
#define LEN_DEMOD_BUF_PREAMBLE_ACCESS 32
//#define LEN_DEMOD_BUF_PREAMBLE_ACCESS (NUM_PREAMBLE_ACCESS_BYTE*8)
typedef enum {
  RISE_EDGE,
  FALL_EDGE
} EDGE_TYPE;
static uint8_t demod_buf_preamble_access[SAMPLE_PER_SYMBOL][LEN_DEMOD_BUF_PREAMBLE_ACCESS];
uint8_t preamble_access_byte[NUM_PREAMBLE_ACCESS_BYTE] = {0xAA, 0xD6, 0xBE, 0x89, 0x8E};
uint8_t preamble_access_bit[NUM_PREAMBLE_ACCESS_BYTE*8];
uint8_t tmp_byte[2+37+3]; // header length + maximum payload length 37 + 3 octets CRC

bool edge_detect(IQ_TYPE *rxp, EDGE_TYPE edge_target, int avg_len, int th) {
  int fake_power[2] = {0, 0};
  int i, j, sample_idx;
  
  sample_idx = 0;
  for (i=0; i<2; i++) {
    for (j=0; j<avg_len; j++) {
      fake_power[i] = fake_power[i] + rxp[sample_idx]*rxp[sample_idx] + rxp[sample_idx+1]*rxp[sample_idx+1];
      sample_idx = sample_idx + 2;
    }
  }
  
  if (edge_target == RISE_EDGE) {
    if (fake_power[1] > fake_power[0]*th) {
      return(true);
    } else {
      return(false);
    }
  } else {
    if (fake_power[0] > fake_power[1]*th) {
      return(true);
    } else {
      return(false);
    }
  }
  
  return(false);
}

void demod_byte(IQ_TYPE* rxp, int num_byte, uint8_t *out_byte) {
  int i, j;
  int I0, Q0, I1, Q1;
  uint8_t bit_decision;
  int sample_idx = 0;
  
  for (i=0; i<num_byte; i++) {
    out_byte[i] = 0;
    for (j=0; j<8; j++) {
      I0 = rxp[sample_idx];
      Q0 = rxp[sample_idx+1];
      I1 = rxp[sample_idx+2];
      Q1 = rxp[sample_idx+3];
      bit_decision = (I0*Q1 - I1*Q0)>0? 1 : 0;
      out_byte[i] = out_byte[i] | (bit_decision<<j);

      sample_idx = sample_idx + SAMPLE_PER_SYMBOL*2;
    }
  }
}

inline int search_unique_bits(IQ_TYPE* rxp, int search_len, uint8_t *unique_bits, const int num_bits) {
  int i, sp, j, i0, q0, i1, q1, k, p, phase_idx;
  bool unequal_flag;
  const int demod_buf_len = num_bits;
  int demod_buf_offset = 0;
  
  //demod_buf_preamble_access[SAMPLE_PER_SYMBOL][LEN_DEMOD_BUF_PREAMBLE_ACCESS]
  memset(demod_buf_preamble_access, 0, SAMPLE_PER_SYMBOL*LEN_DEMOD_BUF_PREAMBLE_ACCESS);
  for(i=0; i<search_len*SAMPLE_PER_SYMBOL*2; i=i+(SAMPLE_PER_SYMBOL*2)) {
    sp = ( (demod_buf_offset-demod_buf_len+1)&(demod_buf_len-1) );
    //sp = (demod_buf_offset-demod_buf_len+1);
    //if (sp>=demod_buf_len)
    //  sp = sp - demod_buf_len;
    
    for(j=0; j<(SAMPLE_PER_SYMBOL*2); j=j+2) {
      i0 = rxp[i+j];
      q0 = rxp[i+j+1];
      i1 = rxp[i+j+2];
      q1 = rxp[i+j+3];
      
      phase_idx = j/2;
      demod_buf_preamble_access[phase_idx][demod_buf_offset] = (i0*q1 - i1*q0) > 0? 1: 0;
      
      k = sp;
      unequal_flag = false;
      for (p=0; p<demod_buf_len; p++) {
        if (demod_buf_preamble_access[phase_idx][k] != unique_bits[p]) {
          unequal_flag = true;
          break;
        }
        k = ( (k + 1)&(demod_buf_len-1) );
        //k = (k + 1);
        //if (k>=demod_buf_len)
        //  k = k - demod_buf_len;
      }
      
      if(unequal_flag==false) {
        return( i + j - (demod_buf_len-1)*SAMPLE_PER_SYMBOL*2 );
      }
      
    }

    demod_buf_offset  = ( (demod_buf_offset+1)&(demod_buf_len-1) );
    //demod_buf_offset  = (demod_buf_offset+1);
    //if (demod_buf_offset>=demod_buf_len)
    //  demod_buf_offset = demod_buf_offset - demod_buf_len;
  }

  return(-1);
}

int parse_adv_pdu_payload_byte(uint8_t *payload_byte, int num_payload_byte, int pdu_type, void *adv_pdu_payload) {
  int i;
  ADV_PDU_PAYLOAD_TYPE_0_2_4_6 *payload_type_0_2_4_6 = NULL;
  ADV_PDU_PAYLOAD_TYPE_1_3 *payload_type_1_3 = NULL;
  ADV_PDU_PAYLOAD_TYPE_5 *payload_type_5 = NULL;
  ADV_PDU_PAYLOAD_TYPE_R *payload_type_R = NULL;
  if (num_payload_byte<6) {
      //payload_parse_result_str = ['Payload Too Short (only ' num2str(length(payload_bits)) ' bits)'];
      printf("Error: Payload Too Short (only %d bytes)\n", num_payload_byte);
      return(-1);
  }

  if (pdu_type == 0 || pdu_type == 2 || pdu_type == 4 || pdu_type == 6) {
      payload_type_0_2_4_6 = (ADV_PDU_PAYLOAD_TYPE_0_2_4_6 *)adv_pdu_payload;
      
      //AdvA = reorder_bytes_str( payload_bytes(1 : (2*6)) );
      payload_type_0_2_4_6->AdvA[0] = payload_byte[5];
      payload_type_0_2_4_6->AdvA[1] = payload_byte[4];
      payload_type_0_2_4_6->AdvA[2] = payload_byte[3];
      payload_type_0_2_4_6->AdvA[3] = payload_byte[2];
      payload_type_0_2_4_6->AdvA[4] = payload_byte[1];
      payload_type_0_2_4_6->AdvA[5] = payload_byte[0];
      
      //AdvData = payload_bytes((2*6+1):end);
      for(i=0; i<(num_payload_byte-6); i++) {
        payload_type_0_2_4_6->Data[i] = payload_byte[6+i];
      }
      
      //payload_parse_result_str = ['AdvA:' AdvA ' AdvData:' AdvData];
  } else if (pdu_type == 1 || pdu_type == 3) {
      if (num_payload_byte!=12) {
          printf("Error: Payload length %d bytes. Need to be 12 for PDU Type %d\n", num_payload_byte, pdu_type);
          return(-1);
      }
      payload_type_1_3 = (ADV_PDU_PAYLOAD_TYPE_1_3 *)adv_pdu_payload;
      
      //AdvA = reorder_bytes_str( payload_bytes(1 : (2*6)) );
      payload_type_1_3->A0[0] = payload_byte[5];
      payload_type_1_3->A0[1] = payload_byte[4];
      payload_type_1_3->A0[2] = payload_byte[3];
      payload_type_1_3->A0[3] = payload_byte[2];
      payload_type_1_3->A0[4] = payload_byte[1];
      payload_type_1_3->A0[5] = payload_byte[0];
      
      //InitA = reorder_bytes_str( payload_bytes((2*6+1):end) );
      payload_type_1_3->A1[0] = payload_byte[11];
      payload_type_1_3->A1[1] = payload_byte[10];
      payload_type_1_3->A1[2] = payload_byte[9];
      payload_type_1_3->A1[3] = payload_byte[8];
      payload_type_1_3->A1[4] = payload_byte[7];
      payload_type_1_3->A1[5] = payload_byte[6];
      
      //payload_parse_result_str = ['AdvA:' AdvA ' InitA:' InitA];
  } else if (pdu_type == 5) {
      if (num_payload_byte!=34) {
          printf("Error: Payload length %d bytes. Need to be 34 for PDU Type %d\n", num_payload_byte, pdu_type);
          return(-1);
      }
      payload_type_5 = (ADV_PDU_PAYLOAD_TYPE_5 *)adv_pdu_payload;
      
      //InitA = reorder_bytes_str( payload_bytes(1 : (2*6)) );
      payload_type_5->InitA[0] = payload_byte[5];
      payload_type_5->InitA[1] = payload_byte[4];
      payload_type_5->InitA[2] = payload_byte[3];
      payload_type_5->InitA[3] = payload_byte[2];
      payload_type_5->InitA[4] = payload_byte[1];
      payload_type_5->InitA[5] = payload_byte[0];
      
      //AdvA = reorder_bytes_str( payload_bytes((2*6+1):(2*6+2*6)) );
      payload_type_5->AdvA[0] = payload_byte[11];
      payload_type_5->AdvA[1] = payload_byte[10];
      payload_type_5->AdvA[2] = payload_byte[9];
      payload_type_5->AdvA[3] = payload_byte[8];
      payload_type_5->AdvA[4] = payload_byte[7];
      payload_type_5->AdvA[5] = payload_byte[6];
      
      //AA = reorder_bytes_str( payload_bytes((2*6+2*6+1):(2*6+2*6+2*4)) );
      payload_type_5->AA[0] = payload_byte[15];
      payload_type_5->AA[1] = payload_byte[14];
      payload_type_5->AA[2] = payload_byte[13];
      payload_type_5->AA[3] = payload_byte[12];
      
      //CRCInit = payload_bytes((2*6+2*6+2*4+1):(2*6+2*6+2*4+2*3));
      payload_type_5->CRCInit = 0;
      payload_type_5->CRCInit = ( (payload_type_5->CRCInit << 8) | payload_byte[16] );
      payload_type_5->CRCInit = ( (payload_type_5->CRCInit << 8) | payload_byte[17] );
      payload_type_5->CRCInit = ( (payload_type_5->CRCInit << 8) | payload_byte[18] );
      
      //WinSize = payload_bytes((2*6+2*6+2*4+2*3+1):(2*6+2*6+2*4+2*3+2*1));
      payload_type_5->WinSize = payload_byte[19];
      
      //WinOffset = reorder_bytes_str( payload_bytes((2*6+2*6+2*4+2*3+2*1+1):(2*6+2*6+2*4+2*3+2*1+2*2)) );
      payload_type_5->WinOffset = 0;
      payload_type_5->WinOffset = ( (payload_type_5->WinOffset << 8) | payload_byte[21] );
      payload_type_5->WinOffset = ( (payload_type_5->WinOffset << 8) | payload_byte[20] );
      
      //Interval = reorder_bytes_str( payload_bytes((2*6+2*6+2*4+2*3+2*1+2*2+1):(2*6+2*6+2*4+2*3+2*1+2*2+2*2)) );
      payload_type_5->Interval = 0;
      payload_type_5->Interval = ( (payload_type_5->Interval << 8) | payload_byte[23] );
      payload_type_5->Interval = ( (payload_type_5->Interval << 8) | payload_byte[22] );
      
      //Latency = reorder_bytes_str( payload_bytes((2*6+2*6+2*4+2*3+2*1+2*2+2*2+1):(2*6+2*6+2*4+2*3+2*1+2*2+2*2+2*2)) );
      payload_type_5->Latency = 0;
      payload_type_5->Latency = ( (payload_type_5->Latency << 8) | payload_byte[25] );
      payload_type_5->Latency = ( (payload_type_5->Latency << 8) | payload_byte[24] );
      
      //Timeout = reorder_bytes_str( payload_bytes((2*6+2*6+2*4+2*3+2*1+2*2+2*2+2*2+1):(2*6+2*6+2*4+2*3+2*1+2*2+2*2+2*2+2*2)) );
      payload_type_5->Timeout = 0;
      payload_type_5->Timeout = ( (payload_type_5->Timeout << 8) | payload_byte[27] );
      payload_type_5->Timeout = ( (payload_type_5->Timeout << 8) | payload_byte[26] );
      
      //ChM = reorder_bytes_str( payload_bytes((2*6+2*6+2*4+2*3+2*1+2*2+2*2+2*2+2*2+1):(2*6+2*6+2*4+2*3+2*1+2*2+2*2+2*2+2*2+2*5)) );
      payload_type_5->ChM[0] = payload_byte[32];
      payload_type_5->ChM[1] = payload_byte[31];
      payload_type_5->ChM[2] = payload_byte[30];
      payload_type_5->ChM[3] = payload_byte[29];
      payload_type_5->ChM[4] = payload_byte[28];
      
      //tmp_bits = payload_bits((end-7) : end);
      //Hop = num2str( bi2de(tmp_bits(1:5), 'right-msb') );
      //SCA = num2str( bi2de(tmp_bits(6:end), 'right-msb') );
      payload_type_5->Hop = (payload_byte[33]&0x1F);
      payload_type_5->SCA = ((payload_byte[33]>>5)&0x07);
  } else {
      payload_type_R = (ADV_PDU_PAYLOAD_TYPE_R *)adv_pdu_payload;

      for(i=0; i<(num_payload_byte); i++) {
        payload_type_R->payload_byte[i] = payload_byte[i];
      }
      //printf("Warning: Reserved PDU type %d\n", pdu_type);
      //return(-1);
  }
  
  return(0);
}
void parse_adv_pdu_header_byte(uint8_t *byte_in, int *pdu_type, int *tx_add, int *rx_add, int *payload_len) {
//% pdy_type_str = {'ADV_IND', 'ADV_DIRECT_IND', 'ADV_NONCONN_IND', 'SCAN_REQ', 'SCAN_RSP', 'CONNECT_REQ', 'ADV_SCAN_IND', 'Reserved', 'Reserved', 'Reserved', 'Reserved', 'Reserved', 'Reserved', 'Reserved', 'Reserved'};
//pdu_type = bi2de(bits(1:4), 'right-msb');
(*pdu_type) = (byte_in[0]&0x0F);
//% disp(['   PDU Type: ' pdy_type_str{pdu_type+1}]);

//tx_add = bits(7);
//% disp(['     Tx Add: ' num2str(tx_add)]);
(*tx_add) = ( (byte_in[0]&0x40) != 0 );

//rx_add = bits(8);
//% disp(['     Rx Add: ' num2str(rx_add)]);
(*rx_add) = ( (byte_in[0]&0x80) != 0 );

//payload_len = bi2de(bits(9:14), 'right-msb');
(*payload_len) = (byte_in[1]&0x3F);
}

inline void receiver_init(void) {
  byte_array_to_bit_array(preamble_access_byte, 5, preamble_access_bit);
}

bool crc_check(uint8_t *tmp_byte, int body_len) {
    int crc24_checksum, crc24_received;
    crc24_checksum = crc24_byte(tmp_byte, body_len, 0xAAAAAA); // 0x555555 --> 0xaaaaaa. maybe because byte order
    crc24_received = 0;
    crc24_received = ( (crc24_received << 8) | tmp_byte[body_len+2] );
    crc24_received = ( (crc24_received << 8) | tmp_byte[body_len+1] );
    crc24_received = ( (crc24_received << 8) | tmp_byte[body_len+0] );
    return(crc24_checksum!=crc24_received);
}

void print_pdu_payload(void *adv_pdu_payload, int pdu_type, int payload_len, bool crc_flag) {
    int i;
    ADV_PDU_PAYLOAD_TYPE_5 *adv_pdu_payload_5;
    ADV_PDU_PAYLOAD_TYPE_1_3 *adv_pdu_payload_1_3;
    ADV_PDU_PAYLOAD_TYPE_0_2_4_6 *adv_pdu_payload_0_2_4_6;
    ADV_PDU_PAYLOAD_TYPE_R *adv_pdu_payload_R;
    // print payload out
    if (pdu_type==0 || pdu_type==2 || pdu_type==4 || pdu_type==6) {
      adv_pdu_payload_0_2_4_6 = (ADV_PDU_PAYLOAD_TYPE_0_2_4_6 *)(adv_pdu_payload);
      printf("AdvA:");
      for(i=0; i<6; i++) {
        printf("%02x", adv_pdu_payload_0_2_4_6->AdvA[i]);
      }
      printf(" Data:");
      for(i=0; i<(payload_len-6); i++) {
        printf("%02x", adv_pdu_payload_0_2_4_6->Data[i]);
      }
    } else if (pdu_type==1 || pdu_type==3) {
      adv_pdu_payload_1_3 = (ADV_PDU_PAYLOAD_TYPE_1_3 *)(adv_pdu_payload);
      printf("A0:");
      for(i=0; i<6; i++) {
        printf("%02x", adv_pdu_payload_1_3->A0[i]);
      }
      printf(" A1:");
      for(i=0; i<6; i++) {
        printf("%02x", adv_pdu_payload_1_3->A1[i]);
      }
    } else if (pdu_type==5) {
      adv_pdu_payload_5 = (ADV_PDU_PAYLOAD_TYPE_5 *)(adv_pdu_payload);
      printf("InitA:");
      for(i=0; i<6; i++) {
        printf("%02x", adv_pdu_payload_5->InitA[i]);
      }
      printf(" AdvA:");
      for(i=0; i<6; i++) {
        printf("%02x", adv_pdu_payload_5->AdvA[i]);
      }
      printf(" AA:");
      for(i=0; i<4; i++) {
        printf("%02x", adv_pdu_payload_5->AA[i]);
      }
      printf(" CRCInit:%06x WSize:%d WOffset:%d Interval:%d Latency:%d Timeout:%d", adv_pdu_payload_5->CRCInit, adv_pdu_payload_5->WinSize, adv_pdu_payload_5->WinOffset, adv_pdu_payload_5->Interval, adv_pdu_payload_5->Latency, adv_pdu_payload_5->Timeout);
      printf(" ChM:");
      for(i=0; i<5; i++) {
        printf("%02x", adv_pdu_payload_5->ChM[i]);
      }
      printf(" Hop:%d SCA:%d", adv_pdu_payload_5->Hop, adv_pdu_payload_5->SCA);
    } else {
      adv_pdu_payload_R = (ADV_PDU_PAYLOAD_TYPE_R *)(adv_pdu_payload);
      printf("Byte:");
      for(i=0; i<(payload_len); i++) {
        printf("%02x", adv_pdu_payload_R->payload_byte[i]);
      }
    }
    printf(" CRC%d\n", crc_flag);
}

void receiver(IQ_TYPE *rxp_in, int buf_len, int channel_number) {
  static int pkt_count = 0;
  static ADV_PDU_PAYLOAD_TYPE_5 adv_pdu_payload;
  static struct timeval time_current_pkt, time_pre_pkt;
  const int demod_buf_len = LEN_BUF_MAX_NUM_PHY_SAMPLE+(LEN_BUF/2);
  IQ_TYPE *rxp = rxp_in;
  int num_demod_byte, hit_idx, buf_len_eaten, pdu_type, tx_add, rx_add, payload_len, time_diff;
  int num_symbol_left = buf_len/(SAMPLE_PER_SYMBOL*2); //2 for IQ
  bool crc_flag;
  
  if (pkt_count == 0) { // the 1st time run
    gettimeofday(&time_current_pkt, NULL);
    time_pre_pkt = time_current_pkt;
  }

  buf_len_eaten = 0;
  while( 1 ) 
  {
    hit_idx = search_unique_bits(rxp, num_symbol_left, preamble_access_bit, LEN_DEMOD_BUF_PREAMBLE_ACCESS);
    if ( hit_idx == -1 ) {
      break;
    }
    //pkt_count++;
    //printf("hit %d\n", hit_idx);
    
    //printf("%d %d %d %d %d %d %d %d\n", rxp[hit_idx+0], rxp[hit_idx+1], rxp[hit_idx+2], rxp[hit_idx+3], rxp[hit_idx+4], rxp[hit_idx+5], rxp[hit_idx+6], rxp[hit_idx+7]);

    buf_len_eaten = buf_len_eaten + hit_idx;
    //printf("%d\n", buf_len_eaten);
    
    buf_len_eaten = buf_len_eaten + 8*NUM_PREAMBLE_ACCESS_BYTE*2*SAMPLE_PER_SYMBOL;// move to beginning of PDU header
    rxp = rxp_in + buf_len_eaten;
    
    num_demod_byte = 2; // PDU header has 2 octets
    buf_len_eaten = buf_len_eaten + 8*num_demod_byte*2*SAMPLE_PER_SYMBOL;
    //if ( buf_len_eaten > buf_len ) {
    if ( buf_len_eaten > demod_buf_len ) {
      break;
    }

    demod_byte(rxp, num_demod_byte, tmp_byte);
    //printf("%d %d %d %d %d %d %d %d\n", rxp[0], rxp[1], rxp[2], rxp[3], rxp[4], rxp[5], rxp[6], rxp[7]);
    //printf("%d %d %d %d %d %d %d %d\n", rxp[8+0], rxp[8+1], rxp[8+2], rxp[8+3], rxp[8+4], rxp[8+5], rxp[8+6], rxp[8+7]);
    scramble_byte(tmp_byte, num_demod_byte, scramble_table[channel_number], tmp_byte);
    rxp = rxp_in + buf_len_eaten;
    num_symbol_left = (buf_len-buf_len_eaten)/(SAMPLE_PER_SYMBOL*2);
    
    parse_adv_pdu_header_byte(tmp_byte, &pdu_type, &tx_add, &rx_add, &payload_len);
    
    if( payload_len<6 || payload_len>37 ) {
      //printf(" (should be 6~37, quit!)\n");
      continue;
    }
    
    //num_pdu_payload_crc_bits = (payload_len+3)*8;
    num_demod_byte = (payload_len+3);
    buf_len_eaten = buf_len_eaten + 8*num_demod_byte*2*SAMPLE_PER_SYMBOL;
    //if ( buf_len_eaten > buf_len ) {
    if ( buf_len_eaten > demod_buf_len ) {
      //printf("\n");
      break;
    }
    
    demod_byte(rxp, num_demod_byte, tmp_byte+2);
    scramble_byte(tmp_byte+2, num_demod_byte, scramble_table[channel_number]+2, tmp_byte+2);
    rxp = rxp_in + buf_len_eaten;
    num_symbol_left = (buf_len-buf_len_eaten)/(SAMPLE_PER_SYMBOL*2);
    
    crc_flag = crc_check(tmp_byte, payload_len+2);
    pkt_count++;
    
    gettimeofday(&time_current_pkt, NULL);
    time_diff = TimevalDiff(&time_current_pkt, &time_pre_pkt);
    time_pre_pkt = time_current_pkt;
    
    printf("%dus Pkt%d Ch%d AA:8E89BED6 PDU_t%d:%s T%d R%d PloadL%d ", time_diff, pkt_count, channel_number, pdu_type, PDU_TYPE_STR[pdu_type], tx_add, rx_add, payload_len);
    
    if (parse_adv_pdu_payload_byte(tmp_byte+2, payload_len, pdu_type, (void *)(&adv_pdu_payload) ) != 0 ) {
      continue;
    }
    print_pdu_payload((void *)(&adv_pdu_payload), pdu_type, payload_len, crc_flag);
  }
}
//----------------------------------receiver----------------------------------

//---------------------------for offline test--------------------------------------
IQ_TYPE tmp_buf[2097152];
//---------------------------for offline test--------------------------------------
int main(int argc, char** argv) {
  uint64_t freq_hz;
  int gain, chan, phase, rx_buf_offset_tmp;
  bool run_flag = false;
  void* rf_dev;
  IQ_TYPE *rxp;

  parse_commandline(argc, argv, &chan, &gain);
  freq_hz = get_freq_by_channel_number(chan);
  printf("cmd line input: chan %d, freq %ldMHz, rx %ddB (%s)\n", chan, freq_hz/1000000, gain, board_name);
  
  // run cyclic recv in background
  do_exit = false;
  if ( config_run_board(freq_hz, gain, &rf_dev) != 0 ){
    if (rf_dev != NULL) {
      goto program_quit;
    }
    else {
      return(1);
    }
  }
  // init receiver
  receiver_init();
  
  // scan
  do_exit = false;
  phase = 0;
  rx_buf_offset = 0;
  while(do_exit == false) { //hackrf_is_streaming(hackrf_dev) == HACKRF_TRUE?
    /*
    if ( (rx_buf_offset-rx_buf_offset_old) > 65536 || (rx_buf_offset-rx_buf_offset_old) < -65536 ) {
      printf("%d\n", rx_buf_offset);
      rx_buf_offset_old = rx_buf_offset;
    }
     * */
    // total buf len LEN_BUF = (8*4096)*2 =  (~ 8ms); tail length MAX_NUM_PHY_SAMPLE*2=LEN_BUF_MAX_NUM_PHY_SAMPLE
    
    rx_buf_offset_tmp = rx_buf_offset - LEN_BUF_MAX_NUM_PHY_SAMPLE;
    // cross point 0
    if (rx_buf_offset_tmp>=0 && rx_buf_offset_tmp<(LEN_BUF/2) && phase==1) {
      //printf("rx_buf_offset cross 0: %d %d %d\n", rx_buf_offset, (LEN_BUF/2), LEN_BUF_MAX_NUM_PHY_SAMPLE);
      phase = 0;
      
      memcpy((void *)(rx_buf+LEN_BUF), (void *)rx_buf, LEN_BUF_MAX_NUM_PHY_SAMPLE*sizeof(IQ_TYPE));
      rxp = (IQ_TYPE*)(rx_buf + (LEN_BUF/2));
      run_flag = true;
    }

    // cross point 1
    if (rx_buf_offset_tmp>=(LEN_BUF/2) && phase==0) {
      //printf("rx_buf_offset cross 1: %d %d %d\n", rx_buf_offset, (LEN_BUF/2), LEN_BUF_MAX_NUM_PHY_SAMPLE);
      phase = 1;

      rxp = (IQ_TYPE*)rx_buf;
      run_flag = true;
    }
    
    if (run_flag) {
      #if 0
      // ------------------------for offline test -------------------------------------
      //save_phy_sample(rx_buf+buf_sp, LEN_BUF/2, "/home/jxj/git/BTLE/matlab/sample_iq_4msps.txt");
      load_phy_sample(tmp_buf, 2097152, "/home/jxj/git/BTLE/matlab/sample_iq_4msps.txt");
      receiver(tmp_buf, 2097152, 37);
      break;
      // ------------------------for offline test -------------------------------------
      #endif
      
      // -----------------------------real online run--------------------------------
      //receiver(rxp, LEN_BUF_MAX_NUM_PHY_SAMPLE+(LEN_BUF/2), chan);
      receiver(rxp, (LEN_DEMOD_BUF_PREAMBLE_ACCESS-1)*2*SAMPLE_PER_SYMBOL+(LEN_BUF)/2, chan);
      // -----------------------------real online run--------------------------------
      
      run_flag = false;
    }
  }

program_quit:
  stop_close_board(rf_dev);
  
  return(0);
}

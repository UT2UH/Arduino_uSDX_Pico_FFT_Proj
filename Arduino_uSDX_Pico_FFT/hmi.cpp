/*
 * hmi.c
 *
 * Created: Apr 2021
 * Author: Arjan te Marvelde
 * May2022: adapted by Klaus Fensterseifer 
 * https://github.com/kaefe64/Arduino_uSDX_Pico_FFT_Proj
 * 
 * This file contains the HMI driver, processing user inputs.
 * It will also do the logic behind these, and write feedback to the LCD.
 *
 * The 4 auxiliary buttons have the following functions:
 * GP6 - Enter, confirm : Used to select menu items or make choices from a list
 * GP7 - Escape, cancel : Used to exit a (sub)menu or cancel the current action
 * GP8 - Left           : Used to move left, e.g. to select a digit
 * GP9 - Right			: Used to move right, e.g. to select a digit
 *
 * The rotary encoder (GP2, GP3) controls an up/down counter connected to some field. 
 * It may be that the encoder has a bushbutton as well, this can be connected to GP4.
 *     ___     ___
 * ___|   |___|   |___  A
 *   ___     ___     _
 * _|   |___|   |___|   B
 *
 * Encoder channel A triggers on falling edge. 
 * Depending on B level, count is incremented or decremented.
 * 
 * The PTT is connected to GP15 and will be active, except when VOX is used.
 *
 */
/*
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/timer.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
*/
#include "Arduino.h"
#include "relay.h"
#include "si5351.h"
#include "dsp.h"
//#include "lcd.h"
#include "hmi.h"
#include "dsp.h"
/*
#include "SPI.h"
#include "TFT_eSPI.h"
//#include "display.h"
//#include "kiss_fftr.h"
//#include "adc_fft.h"
#include "dma.h"
#include "pwm.h"
#include "adc.h"
#include "irq.h"
#include "time.h"
*/
#include "pico/multicore.h"
#include "SPI.h"
#include "TFT_eSPI.h"
#include "display_tft.h"

/*
 * GPIO assignments
 */
#define GP_ENC_A	2               // Encoder clock
#define GP_ENC_B	3               // Encoder direction
#define GP_AUX_0	6								// Enter, Confirm
#define GP_AUX_1	7								// Escape, Cancel
#define GP_AUX_2	8								// Left move
#define GP_AUX_3	9								// Right move
#define GP_PTT		15
#define GP_MASK_IN	((1<<GP_ENC_A)|(1<<GP_ENC_B)|(1<<GP_AUX_0)|(1<<GP_AUX_1)|(1<<GP_AUX_2)|(1<<GP_AUX_3)|(1<<GP_PTT))
//#define GP_MASK_PTT	(1<<GP_PTT)

#define ENCODER_FALL             10    //increment/decrement freq on falling of A encoder signal
#define ENCODER_FALL_AND_RISE    22    //increment/decrement freq on falling and rising fo A encoder signal
#define ENCODER_TYPE             ENCODER_FALL      //choose what encoder is used
//#define ENCODER_TYPE             ENCODER_FALL_AND_RISE      //choose what encoder is used


#define ENCODER_CW_A_FALL_B_LOW  10    //encoder type clockwise step when B low at falling of A
#define ENCODER_CW_A_FALL_B_HIGH 22    //encoder type clockwise step when B high at falling of A
#define ENCODER_DIRECTION        ENCODER_CW_A_FALL_B_HIGH   //direction related to B signal level
//#define ENCODER_DIRECTION        ENCODER_CW_A_FALL_B_LOW    //direction related to B signal level


/*
 * Event flags
 */
//#define GPIO_IRQ_ALL		(GPIO_IRQ_LEVEL_LOW|GPIO_IRQ_LEVEL_HIGH|GPIO_IRQ_EDGE_FALL|GPIO_IRQ_EDGE_RISE)
#define GPIO_IRQ_EDGE_ALL	(GPIO_IRQ_EDGE_FALL|GPIO_IRQ_EDGE_RISE)

/*
 * Display layout:
 *   +----------------+
 *   |USB 14074.0 R920| --> mode=USB, freq=14074.0kHz, state=Rx,S9+20dB
 *   |      Fast -10dB| --> ..., AGC=Fast, Pre=-10dB
 *   +----------------+
 * In this HMI state only tuning is possible, 
 *   using Left/Right for digit and ENC for value, Enter to commit change.
 * Press ESC to enter the submenu states (there is only one sub menu level):
 *
 * Submenu	Values								ENC		Enter			Escape	Left	Right
 * -----------------------------------------------------------------------------------------------
 * Mode		USB, LSB, AM, CW					change	commit			exit	prev	next
 * AGC		Fast, Slow, Off						change	commit			exit	prev	next
 * Pre		+10dB, 0, -10dB, -20dB, -30dB		change	commit			exit	prev	next
 * Vox		NoVOX, Low, Medium, High			change	commit			exit	prev	next
 *
 * --will be extended--
 */
 
/* State definitions */
#define HMI_S_TUNE			0
#define HMI_S_MODE			1
#define HMI_S_AGC			2
#define HMI_S_PRE			3
#define HMI_S_VOX			4
#define HMI_S_BPF			5
#define HMI_NSTATES			6

/* Event definitions */
#define HMI_E_NOEVENT		0
#define HMI_E_INCREMENT		1
#define HMI_E_DECREMENT		2
#define HMI_E_ENTER			3
#define HMI_E_ESCAPE		4
#define HMI_E_LEFT			5
#define HMI_E_RIGHT			6
#define HMI_E_PTTON			7
#define HMI_E_PTTOFF		8
#define HMI_PTT_ON      9
#define HMI_PTT_OFF     10
#define HMI_NEVENTS			11
//#define HMI_NEVENTS      9

/* Sub menu option string sets */
#define HMI_NMODE	4
#define HMI_NAGC	3
#define HMI_NPRE	5
#define HMI_NVOX	4
#define HMI_NBPF	5
char hmi_o_menu[HMI_NSTATES][8] = {"Tune","Mode","AGC","Pre","VOX"};	// Indexed by hmi_state
char hmi_o_mode[HMI_NMODE][8] = {"USB","LSB","AM","CW"};			// Indexed by hmi_sub[HMI_S_MODE]
                                                              //MODE_USB=0 MODE_LSB=1  MODE_AM=2  MODE_CW=3
char hmi_o_agc [HMI_NAGC][8] = {"NoGC","Slow","Fast"};					// Indexed by hmi_sub[HMI_S_AGC]
char hmi_o_pre [HMI_NPRE][8] = {"-30dB","-20dB","-10dB","0dB","+10dB"};	// Indexed by hmi_sub[HMI_S_PRE]
char hmi_o_vox [HMI_NVOX][8] = {"NoVOX","VOX-L","VOX-M","VOX-H"};		// Indexed by hmi_sub[HMI_S_VOX]
char hmi_o_bpf [HMI_NBPF][8] = {"<2.5","2-6","5-12","10-24","20-40"};

// Map option to setting
uint8_t hmi_pre[5] = {REL_ATT_30, REL_ATT_20, REL_ATT_10, REL_ATT_00, REL_PRE_10};
uint8_t hmi_bpf[5] = {REL_LPF2, REL_BPF6, REL_BPF12, REL_BPF24, REL_BPF40};

uint8_t  hmi_state, hmi_option;											// Current state and option selection
uint8_t  hmi_sub[HMI_NSTATES] = {4,1,2,3,0,2};							// Stored option selection per state
uint8_t  hmi_sub_old[HMI_NSTATES];          // Stored last option selection per state
bool	 hmi_update;

uint32_t hmi_freq;														// Frequency from Tune state
uint32_t hmi_step[7] = {10000000, 1000000, 100000, 10000, 1000, 100, 50};	// Frequency digit increments
#define HMI_MAXFREQ		30000000
#define HMI_MINFREQ		     100
//#define HMI_MULFREQ          4			// Factor between HMI and actual frequency
#define HMI_MULFREQ          1      // Factor between HMI and actual frequency
																		// Set to 1, 2 or 4 for certain types of mixer
#define PTT_DEBOUNCE	3											// Nr of cycles for debounce
int ptt_state;															// Debounce counter
bool ptt_active;														// Resulting state





/*
 * Some macros
 */
#ifndef MIN
#define MIN(x, y)        ((x)<(y)?(x):(y))  // Get min value
#endif
#ifndef MAX
#define MAX(x, y)        ((x)>(y)?(x):(y))  // Get max value
#endif

/*
 * HMI State Machine,
 * Handle event according to current state
 * Code needs to be optimized
 */
void hmi_handler(uint8_t event)
{
	/* Special case for TUNE state */
	if (hmi_state == HMI_S_TUNE)  //on main tune
	{

    if (event==HMI_PTT_ON)    
    {
      ptt_active = true;
    }
    if (event==HMI_PTT_OFF)   
    {
      ptt_active = false;
    }

/* I don't think this is needed (and I don't like to call I2C from the interrupt handler
		if (event==HMI_E_ENTER)											// Commit current value
		{
			SI_SETFREQ(0, HMI_MULFREQ*hmi_freq);						// Commit frequency
		}
*/ 
		if (event==HMI_E_ESCAPE)										// Enter submenus
		{
			hmi_sub[hmi_state] = hmi_option;							// Store selection (i.e. digit)
			hmi_state = HMI_S_MODE;										// Should remember last one
			hmi_option = hmi_sub[hmi_state];							// Restore selection of new state
		}
		if (event==HMI_E_INCREMENT)
		{
			if (hmi_freq < (HMI_MAXFREQ - hmi_step[hmi_option]))		// Boundary check
				hmi_freq += hmi_step[hmi_option];						// Increment selected digit
		}
		if (event==HMI_E_DECREMENT)
		{
			if (hmi_freq > (hmi_step[hmi_option] + HMI_MINFREQ))		// Boundary check
				hmi_freq -= hmi_step[hmi_option];						// Decrement selected digit
		}
		if (event==HMI_E_RIGHT)
			hmi_option = (hmi_option<6)?hmi_option+1:6;					// Digit to the right
		if (event==HMI_E_LEFT)
			hmi_option = (hmi_option>0)?hmi_option-1:0;					// Digit to the left
		//return;															// Early bail-out   changed to "else"
	}
  else  //in submenus
  {
  	/* Submenu states */
  	switch(hmi_state)
  	{
  	case HMI_S_MODE:
  		if (event==HMI_E_INCREMENT)
      {
  			hmi_option = (hmi_option<HMI_NMODE-1)?hmi_option+1:HMI_NMODE-1;
      }
  		if (event==HMI_E_DECREMENT)
  			hmi_option = (hmi_option>0)?hmi_option-1:0;
  		break;
  	case HMI_S_AGC:
  		if (event==HMI_E_INCREMENT)
  			hmi_option = (hmi_option<HMI_NAGC-1)?hmi_option+1:HMI_NAGC-1;
  		if (event==HMI_E_DECREMENT)
  			hmi_option = (hmi_option>0)?hmi_option-1:0;
  		break;
  	case HMI_S_PRE:
  		if (event==HMI_E_INCREMENT)
  			hmi_option = (hmi_option<HMI_NPRE-1)?hmi_option+1:HMI_NPRE-1;
  		if (event==HMI_E_DECREMENT)
  			hmi_option = (hmi_option>0)?hmi_option-1:0;
  		break;
  	case HMI_S_VOX:
  		if (event==HMI_E_INCREMENT)
  			hmi_option = (hmi_option<HMI_NVOX-1)?hmi_option+1:HMI_NVOX-1;
  		if (event==HMI_E_DECREMENT)
  			hmi_option = (hmi_option>0)?hmi_option-1:0;
  		break;
  	case HMI_S_BPF:
  		if (event==HMI_E_INCREMENT)
  			hmi_option = (hmi_option<HMI_NBPF-1)?hmi_option+1:HMI_NBPF-1;
  		if (event==HMI_E_DECREMENT)
  			hmi_option = (hmi_option>0)?hmi_option-1:0;
  		break;
  	}
  	
  	/* General actions for all submenus */
  	if (event==HMI_E_ENTER)
  	{
  		hmi_sub[hmi_state] = hmi_option;				// Store selected option	
      hmi_update = true;                      // Mark HMI updated
  	}
  	if (event==HMI_E_ESCAPE)
  	{
  		hmi_state = HMI_S_TUNE;										// Leave submenus
  		hmi_option = hmi_sub[hmi_state];							// Restore selection of new state
  	}
  	if (event==HMI_E_RIGHT)
  	{
  		hmi_state = (hmi_state<HMI_NSTATES-1)?(hmi_state+1):1;		// Change submenu
  		hmi_option = hmi_sub[hmi_state];							// Restore selection of new state
  	}
  	if (event==HMI_E_LEFT)
  	{
  		hmi_state = (hmi_state>1)?(hmi_state-1):HMI_NSTATES-1;		// Change submenu
  		hmi_option = hmi_sub[hmi_state];							// Restore selection of new state
  	}

  }
  
}

/*
 * GPIO IRQ callback routine
 * Sets the detected event and invokes the HMI state machine
 */
void hmi_callback(uint gpio, uint32_t events)
{
	uint8_t evt=HMI_E_NOEVENT;

	switch (gpio)
	{
	case GP_ENC_A:									// Encoder
		if (events&GPIO_IRQ_EDGE_FALL)
    {
#if ENCODER_DIRECTION == ENCODER_CW_A_FALL_B_HIGH
			evt = gpio_get(GP_ENC_B)?HMI_E_INCREMENT:HMI_E_DECREMENT;
#else
      evt = gpio_get(GP_ENC_B)?HMI_E_DECREMENT:HMI_E_INCREMENT;
#endif
    } 
#if ENCODER_TYPE == ENCODER_FALL_AND_RISE
    else if (events&GPIO_IRQ_EDGE_RISE)
    {  
#if ENCODER_DIRECTION == ENCODER_CW_A_FALL_B_HIGH
      evt = gpio_get(GP_ENC_B)?HMI_E_DECREMENT:HMI_E_INCREMENT;
#else
      evt = gpio_get(GP_ENC_B)?HMI_E_INCREMENT:HMI_E_DECREMENT;
#endif
    }
#endif
		break;
	case GP_AUX_0:									// Enter
		if (events&GPIO_IRQ_EDGE_FALL)
    {
			evt = HMI_E_ENTER;
    }
		break;
	case GP_AUX_1:									// Escape
		if (events&GPIO_IRQ_EDGE_FALL)
    {
			evt = HMI_E_ESCAPE;
    }
		break;
	case GP_AUX_2:									// Previous
		if (events&GPIO_IRQ_EDGE_FALL)
    {
			evt = HMI_E_LEFT;
    }
		break;
	case GP_AUX_3:									// Next
		if (events&GPIO_IRQ_EDGE_FALL)
    {
			evt = HMI_E_RIGHT;
    }
		break;

  case GP_PTT:                  // Next
/*
    if (events&GPIO_IRQ_EDGE_ALL)
    {
      evt = gpio_get(GP_PTT)?HMI_PTT_OFF:HMI_PTT_ON;
    }
*/
    if (events&GPIO_IRQ_EDGE_FALL)
    {
      evt = HMI_PTT_ON;
    }    
    if (events&GPIO_IRQ_EDGE_RISE)
    {
      evt = HMI_PTT_OFF;
    }    

    break;

	default:
		return;
	}
	
	hmi_handler(evt);								// Invoke state machine
}

/*
 * Initialize the User interface
 */
void hmi_init(void)
{
	/*
	 * Notes on using GPIO interrupts: 
	 * The callback handles interrupts for all GPIOs with IRQ enabled.
	 * Level interrupts don't seem to work properly.
	 * For debouncing, the GPIO pins should be pulled-up and connected to gnd with 100nF.
	 * PTT has separate debouncing logic
	 */

	// Init input GPIOs
	gpio_init_mask(GP_MASK_IN);
	
	// Enable pull-ups
	gpio_pull_up(GP_ENC_A);
	gpio_pull_up(GP_ENC_B);
	gpio_pull_up(GP_AUX_0);
	gpio_pull_up(GP_AUX_1);
	gpio_pull_up(GP_AUX_2);
	gpio_pull_up(GP_AUX_3);
	gpio_pull_up(GP_PTT);
	
	// Enable interrupt on level low
	gpio_set_irq_enabled(GP_ENC_A, GPIO_IRQ_EDGE_ALL, true);
	gpio_set_irq_enabled(GP_AUX_0, GPIO_IRQ_EDGE_ALL, true);
	gpio_set_irq_enabled(GP_AUX_1, GPIO_IRQ_EDGE_ALL, true);
	gpio_set_irq_enabled(GP_AUX_2, GPIO_IRQ_EDGE_ALL, true);
	gpio_set_irq_enabled(GP_AUX_3, GPIO_IRQ_EDGE_ALL, true);
//	gpio_set_irq_enabled(GP_PTT, GPIO_IRQ_EDGE_ALL, false);
  gpio_set_irq_enabled(GP_PTT, GPIO_IRQ_EDGE_ALL, true);

	// Set callback, one for all GPIO, not sure about correctness!
	gpio_set_irq_enabled_with_callback(GP_ENC_A, GPIO_IRQ_EDGE_ALL, true, hmi_callback);
		
	// Initialize LCD and set VFO
	hmi_state = HMI_S_TUNE;
	hmi_option = 4;									// Active kHz digit
	hmi_freq = 7050000UL;							// Initial frequency

	SI_SETFREQ(0, HMI_MULFREQ*hmi_freq);			// Set freq to 7074 kHz (depends on mixer type)
	SI_SETPHASE(0, 1);								// Set phase to 90deg (depends on mixer type)
	
	ptt_state = 0;
	ptt_active = false;
	
	dsp_setmode(hmi_sub[HMI_S_MODE]);  //MODE_USB=0 MODE_LSB=1  MODE_AM=2  MODE_CW=3
	dsp_setvox(hmi_sub[HMI_S_VOX]);
	dsp_setagc(hmi_sub[HMI_S_AGC]);	
	relay_setattn(hmi_pre[hmi_sub[HMI_S_PRE]]);
	relay_setband(hmi_bpf[hmi_sub[HMI_S_BPF]]);
	//hmi_update = false;   //hmi needs update after hardware devices updated

}





/*
 * Redraw the display, representing current state
 * This function is called regularly from the main loop.
 */
void hmi_evaluate(void)
{
	char s[32];
  int16_t rec_level;
  
  static uint32_t hmi_freq_old;
  static bool tx_enable_old = true;
  static uint8_t hmi_state_old;
  static uint8_t hmi_option_old;
  static int16_t agc_gain_old = 1;


   

  // Set parameters corresponding to latest entered option value 

  if(hmi_freq_old != hmi_freq)
  {
    SI_SETFREQ(0, HMI_MULFREQ*hmi_freq);
    //freq  (from encoder)
    sprintf(s, "%7.1f", (double)hmi_freq/1000.0);
    tft_writexy_plus(3, TFT_YELLOW, TFT_BLACK, 2,0,2,20,(uint8_t *)s);
    //cursor (writing the freq erase the cursor)
    tft_cursor_plus(3, TFT_YELLOW, 2+(hmi_option>4?6:hmi_option), 0, 2, 20);
    display_fft_graf_top();  //scale freqs
    hmi_freq_old = hmi_freq;
  }
  if(hmi_sub_old[HMI_S_MODE] != hmi_sub[HMI_S_MODE])    //mode (SSB AM CW)
  {
    dsp_setmode(hmi_sub[HMI_S_MODE]);  //MODE_USB=0 MODE_LSB=1  MODE_AM=2  MODE_CW=3
    sprintf(s, "%s  ", hmi_o_mode[hmi_sub[HMI_S_MODE]]);
    tft_writexy_(2, TFT_GREEN, TFT_BLACK, 0,1,(uint8_t *)s);
    display_fft_graf_top();  //scale freqs, mode changes the triangle
    hmi_sub_old[HMI_S_MODE] = hmi_sub[HMI_S_MODE];
  }
  if(hmi_sub_old[HMI_S_VOX] != hmi_sub[HMI_S_VOX])
  {
    dsp_setvox(hmi_sub[HMI_S_VOX]);
    hmi_sub_old[HMI_S_VOX] = hmi_sub[HMI_S_VOX];
  }
  if(hmi_sub_old[HMI_S_AGC] != hmi_sub[HMI_S_AGC])
  {
    dsp_setagc(hmi_sub[HMI_S_AGC]); 
    hmi_sub_old[HMI_S_AGC] = hmi_sub[HMI_S_AGC];
  }
  if(hmi_sub_old[HMI_S_BPF] != hmi_sub[HMI_S_BPF])
  {
    relay_setband(hmi_bpf[hmi_sub[HMI_S_BPF]]);
    sleep_ms(1);                  // I2C doesn't work without...
    hmi_sub_old[HMI_S_BPF] = hmi_sub[HMI_S_BPF];
  }
  if(hmi_sub_old[HMI_S_PRE] != hmi_sub[HMI_S_PRE])
  {  
    relay_setattn(hmi_pre[hmi_sub[HMI_S_PRE]]);
    hmi_sub_old[HMI_S_PRE] = hmi_sub[HMI_S_PRE];
  }



  //T or R  (using letters instead of arrow used on original project)
  if(tx_enable_old != tx_enabled)
  {
    if(tx_enabled == true)
    {
      sprintf(s, "T   ");
      tft_writexy_(2, TFT_RED, TFT_BLACK, 0,2,(uint8_t *)s);
    }
    else
    {
      sprintf(s, "R");
      tft_writexy_(2, TFT_GREEN, TFT_BLACK, 0,2,(uint8_t *)s);
    }
    agc_gain_old = agc_gain+1;

    tx_enable_old = tx_enabled;
  }

  
   
  //Smeter rec level
  if(tx_enabled == false)
  {
    if(agc_gain_old != agc_gain)
    {
      rec_level = AGC_GAIN_MAX - agc_gain;
      sprintf(s, "%d  ", rec_level);
      tft_writexy_(2, TFT_GREEN, TFT_BLACK, 1,2,(uint8_t *)s);
      agc_gain_old = agc_gain;
    }
  }




  if((hmi_state_old != hmi_state) || (hmi_option_old != hmi_option))
  {
  	//menu 
  	switch (hmi_state)
  	{
  	case HMI_S_TUNE:
  		sprintf(s, "%s   %s   %s", hmi_o_vox[hmi_sub[HMI_S_VOX]], hmi_o_agc[hmi_sub[HMI_S_AGC]], hmi_o_pre[hmi_sub[HMI_S_PRE]]);
      tft_writexy_(1, TFT_BLUE, TFT_BLACK,0,0,(uint8_t *)s);  
      //cursor
      tft_cursor_plus(3, TFT_YELLOW, 2+(hmi_option>4?6:hmi_option), 0, 2, 20);    
  		break;
  	case HMI_S_MODE:
  		sprintf(s, "Set Mode: %s        ", hmi_o_mode[hmi_option]);
      tft_writexy_(1, TFT_MAGENTA, TFT_BLACK,0,0,(uint8_t *)s);  
  		break;
  	case HMI_S_AGC:
  		sprintf(s, "Set AGC: %s        ", hmi_o_agc[hmi_option]);
      tft_writexy_(1, TFT_MAGENTA, TFT_BLACK,0,0,(uint8_t *)s);  
  		break;
  	case HMI_S_PRE:
  		sprintf(s, "Set Pre: %s        ", hmi_o_pre[hmi_option]);
      tft_writexy_(1, TFT_MAGENTA, TFT_BLACK,0,0,(uint8_t *)s);  
  		break;
  	case HMI_S_VOX:
  		sprintf(s, "Set VOX: %s        ", hmi_o_vox[hmi_option]);
      tft_writexy_(1, TFT_MAGENTA, TFT_BLACK,0,0,(uint8_t *)s);  
  		break;
  	case HMI_S_BPF:
  		sprintf(s, "Band: %d %s        ", hmi_option, hmi_o_bpf[hmi_option]);
      tft_writexy_(1, TFT_MAGENTA, TFT_BLACK,0,0,(uint8_t *)s);  
  	default:
  		break;
  	}
   
    hmi_state_old = hmi_state;
    hmi_option_old = hmi_option;
  } 




 
  if (fft_display_graf_new == 1) //design a new graphic only when a new line is ready from FFT
  {
    //plot waterfall graphic     
    display_fft_graf();  // warefall 110ms

    fft_display_graf_new = 0;  
    fft_samples_ready = 2;  //ready to start new sample collect
  }


 
  if (aud_samples_state == AUD_STATE_SAMP_RDY) //design a new graphic only when data is ready
  {
    //plot audio graphic     
    display_aud_graf();

    aud_samples_state = AUD_STATE_SAMP_IN;  
  }


/*

  
  // PTT debouncing 
  if (hmi_sub[HMI_S_VOX] == 0)            // No VOX active
  {
    gpio_set_dir(GP_PTT, false);          // PTT input
    if (gpio_get(GP_PTT))             // Get PTT level
    {
      if (ptt_state<PTT_DEBOUNCE)         // Increment debounce counter when high
        ptt_state++;
    }
    else 
    {
      if (ptt_state>0)              // Decrement debounce counter when low
        ptt_state--;
    }
    if (ptt_state == PTT_DEBOUNCE)          // Reset PTT when debounced level high
      ptt_active = false;
    if (ptt_state == 0)               // Set PTT when debounced level low
      ptt_active = true;
  }
  else
  {
    ptt_active = false;
    gpio_set_dir(GP_PTT, true);           // PTT output
  }

*/

}

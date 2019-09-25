#include "drv_max7456.h"
#include "drv_time.h"
#include "project.h"
#include "stdio.h"
#include "string.h"
#include "profile.h"
#include "float.h"
#include "util.h"
#include "rx.h"

#ifdef ENABLE_OSD

void osd_init(void) {
  spi_max7456_init(); //init spi
  max7456_init();     //init the max chip
  osd_intro();        //print the splash screen
}

/*screen elements characteristics written like registers in a 32bit binany number
except callsign which will take 6 addresses.  callsign bit 1 will be enable/disable,
bit 2 will be text/invert, and the remaining 30 bits will be 5 characters.  6 total addresses
will allow callsign text to fill the whole screen across
BIT
0			-		0 is display element inactive , 1 is display element active
1			-		0 is TEXT, 1 is INVERT
2:6		-		the X screen position (column)
7:10	-		the Y screen position	(row)
11:15	-		not currently used
16:31	-		available for two binary ascii characters
*/

//Flash Variables - 32bit					# of osd elements and flash memory start position in defines.h
unsigned long osd_element[OSD_NUMBER_ELEMENTS];
/*
elements 0-5 - Call Sign
element 6 Fuel Gauge volts
element 7 Filtered Volts
element 8 Exact Volts
*/

//pointers to flash variable array
unsigned long *callsign1 = osd_element;
unsigned long *callsign2 = (osd_element + 1);
unsigned long *callsign3 = (osd_element + 2);
unsigned long *callsign4 = (osd_element + 3);
unsigned long *callsign5 = (osd_element + 4);
unsigned long *callsign6 = (osd_element + 5);
unsigned long *fuelgauge_volts = (osd_element + 6);
unsigned long *filtered_volts = (osd_element + 7);
unsigned long *exact_volts = (osd_element + 8);

//screen element register decoding functions
uint8_t decode_attribute(uint32_t element) { //shifting right one bit and comparing the new bottom bit to the key above
  uint8_t decoded_element = ((element >> 1) & 0x01);
  if (decoded_element == 0x01)
    return INVERT;
  else
    return TEXT;
}

uint8_t decode_positionx(uint32_t element) { //shift 2 bits and grab the bottom 5
  return ((element >> 2) & 0x1F);            // this can be simplified to save memory if it debugs ok
}

uint32_t decode_positiony(uint32_t element) { //shift 7 bits and grab the bottom 4
  return ((element >> 7) & 0x0F);             // this can be simplified to save memory if it debugs ok
}
//******************************************************************************************************************************


//******************************************************************************************************************************
// case & state variables for switch logic and profile adjustments

extern int flash_feature_1; //currently used for auto entry into wizard menu
extern profile_t profile;
uint8_t osd_display_phase = 2;
uint8_t osd_wizard_phase = 0;
uint8_t osd_menu_phase = 0;
uint8_t osd_display_element = 0;
uint8_t display_trigger = 0;
uint8_t last_lowbatt_state = 2;
uint8_t osd_cursor;
uint8_t osd_select;
uint8_t increase_osd_value;
uint8_t decrease_osd_value;

#define BF_PIDS 0
const float bf_pids_increments[] = {0.0015924, 0.0015924, 0.0031847, 0.02, 0.02, 0.02, 0.0083333, 0.0083333, 0.0083333};
#define SW_RATES 1
const float sw_rates_increments[] = {10.0, 10.0, 10.0, 0.01, 0.01, 0.01, 0.01, 0.01, 0.01};
#define ROUNDED 3
const float rounded_increments[] = {0.01, 0.01, 0.01, 0.01, 0.01, 0.01, 0.01, 0.01, 0.01};

const uint8_t pid_submenu_map[] = {4, 4};	//describes the menu case to call next for each submenu option
const uint8_t rates_submenu_map[] = {7, 8};
const uint8_t stickboost_submenu_map[] = {14, 14};

const uint8_t main_menu_map[] = {11, 3, 5, 6, 9, 10, 12};		//case numbers for {vtx, pids, filters, rates, flight modes, osd elements, special features}
const uint8_t special_features_map[] = {13};					//case numbers for {stickboost}
#define MAIN_MENU 1
#define SUB_MENU 0

const uint8_t flight_mode_aux_limits[] = {11, 14, 14, 14, 14, 14, 14, 14, 14, 14};	//from aux_channel_t
const uint8_t flight_mode_aux_items[] = {0, 1, 2, 3, 4, 5, 7, 9, 10, 12};			//from aux_function_t
//******************************************************************************************************************************


//******************************************************************************************************************************
// osd utility functions

void osd_display_reset(void) {
  osd_wizard_phase = 0;    //reset the wizard
  osd_menu_phase = 0;      //reset menu to to main menu
  osd_display_phase = 2;   //jump to regular osd display next loop
  osd_display_element = 0; //start with first screen element
  last_lowbatt_state = 2;  //reset last lowbatt comparator
}

void osd_save_exit(void){
	osd_select = 0;
    osd_cursor = 0;
    osd_display_phase = 0;
	#ifdef FLASH_SAVE1
    extern int pid_gestures_used;
    extern int ledcommand;
    extern void flash_save(void);
    extern void flash_load(void);
    pid_gestures_used = 0;
    ledcommand = 1;
    flash_save();
    flash_load();
    // reset flash numbers for pids
    extern int number_of_increments[3][3];
    for (int i = 0; i < 3; i++)
      for (int j = 0; j < 3; j++)
        number_of_increments[i][j] = 0;
    // reset loop time - maybe not necessary cause it gets reset in the next screen clear
    extern unsigned long lastlooptime;
    lastlooptime = gettime();
    #endif
}

uint8_t user_selection(uint8_t element, uint8_t total) {
  if (osd_cursor != element) {
    if (osd_cursor < 1)
      osd_cursor = 1;
    if (osd_cursor > total)
      osd_cursor = total;
  }
  if (osd_cursor == element && osd_select == 0) {
    return INVERT;
  } else {
    return TEXT;
  }
}

uint8_t grid_selection(uint8_t element, uint8_t row) {
	if (osd_select == element && osd_cursor == row ){
		return INVERT;
	}else{
		return TEXT;
	}
}

uint16_t rp_kp_scale(float pid_rate){		//increment by 0.0015923566878981f
	uint16_t scaled_pid_value = round_num(pid_rate * 628.0f);
	return scaled_pid_value;
}

uint16_t yaw_kp_scale(float pid_rate){		//increment by 0.0031847133757962f
	uint16_t scaled_pid_value = round_num(pid_rate * 314.0f);
	return scaled_pid_value;
}

uint16_t ki_scale(float pid_rate){			//increment by 0.02f
	uint16_t scaled_pid_value = round_num(pid_rate * 50.0f);
	return scaled_pid_value;
}

uint16_t kd_scale(float pid_rate){			//increment by 0.0083333333333333f
	uint16_t scaled_pid_value = round_num(pid_rate * 120.0f);
	return scaled_pid_value;
}

float adjust_rounded_float(float input, float adjust_amount){
	float value = (int)(input * 100.0f + 0.5f);
	if (increase_osd_value){
		increase_osd_value = 0;
		osd_menu_phase = 1; //repaint the screen again
		return (float)(value+(100.0f * adjust_amount)) / 100.0f;
	}
	if (decrease_osd_value){
		decrease_osd_value = 0;
		osd_menu_phase = 1; //repaint the screen again
		return (float)(value-(100.0f * adjust_amount)) / 100.0f;
	}
	return input;
}


const char* get_aux_status (int input){
	static char* respond[] = {"CHANNEL 5  ", "CHANNEL 6  ", "CHANNEL 7  ", "CHANNEL 8  ", "CHANNEL 9  ", "CHANNEL 10 ", "CHANNEL 11 ", "CHANNEL 12 ", "CHANNEL 13 ", "CHANNEL 14 ", "CHANNEL 15 ", "CHANNEL 16 ", "GESTURE AUX", "ALWAYS ON  ", "ALWAYS OFF ", "ERROR      "};
	if(input == AUX_CHANNEL_0) return respond[0];
	if(input == AUX_CHANNEL_1) return respond[1];
	if(input == AUX_CHANNEL_2) return respond[2];
	if(input == AUX_CHANNEL_3) return respond[3];
	if(input == AUX_CHANNEL_4) return respond[4];
	if(input == AUX_CHANNEL_5) return respond[5];
	if(input == AUX_CHANNEL_6) return respond[6];
	if(input == AUX_CHANNEL_7) return respond[7];
	if(input == AUX_CHANNEL_8) return respond[8];
	if(input == AUX_CHANNEL_9) return respond[9];
	if(input == AUX_CHANNEL_10) return respond[10];
	if(input == AUX_CHANNEL_11) return respond[11];
	if(input == AUX_CHANNEL_12) return respond[12];
	if(input == AUX_CHANNEL_ON) return respond[13];
	if(input == AUX_CHANNEL_OFF) return respond[14];
	return respond[15];
}

vector_t *get_pid_term(uint8_t term) {
  switch (term) {
  case 1:
    return &profile.pid.pid_rates[profile.pid.pid_profile].kp;
  case 2:
    return &profile.pid.pid_rates[profile.pid.pid_profile].ki;
  case 3:
    return &profile.pid.pid_rates[profile.pid.pid_profile].kd;
  }
  return NULL;
}

vector_t *get_sw_rate_term(uint8_t term) {
  switch (term) {
  case 1:
    return &profile.rate.silverware.max_rate;
  case 2:
    return &profile.rate.silverware.acro_expo;
  case 3:
    return &profile.rate.silverware.angle_expo;
  }
  return NULL;
}

vector_t *get_bf_rate_term(uint8_t term) {
  switch (term) {
  case 1:
    return &profile.rate.betaflight.rc_rate;
  case 2:
    return &profile.rate.betaflight.super_rate;
  case 3:
    return &profile.rate.betaflight.expo;
  }
  return NULL;
}

vector_t *get_stick_profile_term(uint8_t term) {
  switch (term) {
  case 1:
    return &profile.pid.stick_rates[profile.pid.stick_profile].accelerator;
  case 2:
    return &profile.pid.stick_rates[profile.pid.stick_profile].transition;
  }
  return NULL;
}

void osd_vector_adjust ( vector_t *pointer, uint8_t rows, uint8_t columns, uint8_t special_case){

	if(osd_select > columns) {
		osd_select = columns;	//limit osd select variable from accumulating past 3 columns of adjustable items
		osd_menu_phase = 1; //repaint the screen again
	}
	uint8_t adjust_tracker = ((osd_cursor-1)*columns) + (osd_select - 1);
	uint8_t i;
	for (i = 1; i <= rows; i++){
		if (osd_cursor == i){	//osd_cursor always describes what row we are on.  when i lands on the active row, then we check to see what column we are on.
			uint8_t j;
			for (j = 1; j <= columns; j++){	//osd_select always describes what column we are on.  when j lands on the active column, then we check for a requested increase or decrease
				if (osd_select == j){
					if (special_case == BF_PIDS){
						if (increase_osd_value){
							pointer->axis[osd_select-1] = pointer->axis[osd_select-1] + bf_pids_increments[adjust_tracker];
							increase_osd_value = 0;
							decrease_osd_value = 0;
							osd_menu_phase = 1; //repaint the screen again
						}
						if (decrease_osd_value){
							pointer->axis[osd_select-1] = pointer->axis[osd_select-1] - bf_pids_increments[adjust_tracker];
							increase_osd_value = 0;
							decrease_osd_value = 0;
							osd_menu_phase = 1; //repaint the screen again
						}
					}
					if (special_case == SW_RATES){
						if (increase_osd_value || decrease_osd_value) pointer->axis[osd_select-1] = adjust_rounded_float(pointer->axis[osd_select-1], sw_rates_increments[adjust_tracker]);
					}
					if (special_case == ROUNDED){
						if (increase_osd_value || decrease_osd_value) pointer->axis[osd_select-1] = adjust_rounded_float(pointer->axis[osd_select-1], rounded_increments[adjust_tracker]);
					}
				}
			}
		}
	}

	if (osd_cursor == rows + 1){
		if (osd_select == 1){
		osd_save_exit();
		}
	}
}

void osd_submenu_select (uint8_t *pointer, uint8_t rows, const uint8_t next_menu[]){
	if (osd_select == 1){ //stick was pushed right to select a rate type
		osd_select = 0;	//reset the trigger
		uint8_t i;
		for (i = 1; i <= rows; i++){
			if(i == osd_cursor){
				osd_cursor = 0;	//reset the cursor
				*pointer = i-1;	//update profile
				osd_display_phase = next_menu[i-1];	//update display phase to the next menu screen
				osd_menu_phase = 0;	//clear the screen
			}
		}
	}
}

void osd_select_menu_item(uint8_t rows, const uint8_t menu_map[], uint8_t main_menu) {
  if (osd_select == 1 && flash_feature_1 == 1) //main mmenu
  {
    osd_select = 0; //reset the trigger
    uint8_t i;
    for (i = 1; i <= rows; i++){		//7 main menu items that lead to a new submenu
    	if(i == osd_cursor){
    		osd_cursor = 0;
    		osd_display_phase = menu_map[i-1];
    		osd_menu_phase = 0;
    	}
    }
    if (main_menu){
    	if(osd_cursor == rows + 1) flash_feature_1 = 0; //flag the setup wizard in main menu
    	if(osd_cursor == rows + 2) osd_save_exit();		//include save&exit in main menu
    }
  }
}

void osd_enum_adjust(uint8_t *pointer, uint8_t rows, const uint8_t increase_limit[]){
	if(osd_select > 1) {
		osd_select = 1;	//limit osd select variable from accumulating past 1 columns of adjustable items
		osd_menu_phase = 1; //repaint the screen again
	}
	uint8_t i;
	for (i = 1; i <= rows; i++){
		if (osd_cursor == i){	//osd_cursor always describes what row we are on.  when i lands on the active row, then we check to see if we adjust the value.
				if (osd_select == 1){
					uint8_t i = *pointer;
					if (increase_osd_value && i != increase_limit[osd_cursor-1])  {	//limits need to be 11 for arming, 14 for verything else
						i++;
						*pointer = i;
						increase_osd_value = 0;
						osd_menu_phase = 1; //repaint the screen again
						}
					if (decrease_osd_value && i != 0)  {	//limit is always 0 for an enum
						i--;
						*pointer = i;
						decrease_osd_value = 0;
						osd_menu_phase = 1; //repaint the screen again
						}
				}
		}
	}
	increase_osd_value = 0;
	decrease_osd_value = 0;
	if (osd_cursor == rows + 1){
		if (osd_select == 1){
		osd_save_exit();
		}
	}
}
//******************************************************************************************************************************


//******************************************************************************************************************************
// osd main display function

void osd_display(void) {
  //first check if video signal autodetect needs to run - run if necessary
  extern uint8_t lastsystem; //initialized at 99 for none then becomes 0 or 1 for ntsc/pal
  if (lastsystem > 1)        //if no camera was detected at boot up
  {
    osd_checksystem(); //try to detect camera
    if (lastsystem < 2) {
      osd_display_reset(); //camera has been detected while in the main loop and screen has been cleared again - reset screen cases
    }
  }

  //************OSD MENU DISPLAY ROUTINES HERE*************

  //grab out of scope variables for data that needs to be displayed
  //extern int flash_feature_1;
  //extern float vbattfilt;
  //extern float vbattfilt_corr;
  extern float vbatt_comp;
  extern float lipo_cell_count;
  extern int lowbatt;

  //just some values to test position/attribute/active until we start saving them to flash with the osd manu
  *callsign1 = 0xAB;        //	0001 01010 11
  *fuelgauge_volts = 0x72D; //‭  1110 01011 01‬

  switch (osd_display_phase) //phase starts at 2, RRR gesture subtracts 1 to enter the menu, RRR again or DDD subtracts 1 to clear the screen and return to regular display
  {
  case 0: //osd screen clears, resets to regular display, and resets wizard and menu starting points
    osd_clear();
    osd_display_reset();
    extern unsigned long lastlooptime;
    lastlooptime = gettime();
    break; //screen has been cleared for this loop - break out of display function

  case 1:                //osd menu is active
    if (flash_feature_1) //setup wizard
    {
      switch (osd_menu_phase) {
      case 0:
        osd_clear();
        extern unsigned long lastlooptime;
        lastlooptime = gettime();
        osd_menu_phase++;
        break;
      case 1:
        osd_print("MENU", INVERT, 13, 1); //function call returns text or invert, gets passed # of elements for wrap around,
        osd_menu_phase++;                 //and which element number this is
        break;
      case 2:
        osd_print("VTX", user_selection(1, 9), 7, 3); //selection_status (1,9)
        osd_menu_phase++;
        break;
      case 3:
        osd_print("PIDS", user_selection(2, 9), 7, 4);
        osd_menu_phase++;
        break;
      case 4:
        osd_print("FILTERS", user_selection(3, 9), 7, 5);
        osd_menu_phase++;
        break;
      case 5:
        osd_print("RATES", user_selection(4, 9), 7, 6);
        osd_menu_phase++;
        break;
      case 6:
        osd_print("FLIGHT MODES", user_selection(5, 9), 7, 7);
        osd_menu_phase++;
        break;
      case 7:
        osd_print("OSD ELEMENTS", user_selection(6, 9), 7, 8);
        osd_menu_phase++;
        break;
      case 8:
        osd_print("SPECIAL FEATURES", user_selection(7, 9), 7, 9);
        osd_menu_phase++;
        break;
      case 9:
        osd_print("SETUP WIZARD", user_selection(8, 9), 7, 10);
        osd_menu_phase++;
        break;
      case 10:
        osd_print("SAVE + EXIT", user_selection(9, 9), 7, 11);
        osd_menu_phase++;
        break;
      case 11:
        osd_select_menu_item(7,main_menu_map, MAIN_MENU);
        break;
      }
    } else {
      switch (osd_wizard_phase) {
      case 0:
        osd_clear();
        extern unsigned long lastlooptime;
        lastlooptime = gettime();
        osd_wizard_phase++;
        break;
      case 1:
        osd_print("SETUP WIZARD", INVERT, 9, 1);
        osd_wizard_phase++;
        break;
      case 2:
        osd_print("PROPS OFF", BLINK, 7, 6);
        osd_wizard_phase++;
        break;
      case 3:
        osd_print("THROTTLE UP", TEXT, 7, 8);
        osd_wizard_phase++;
        break;
      case 4:
        osd_print("TO CONTINUE", TEXT, 7, 9);
        osd_wizard_phase++;
        break;
      case 5:
        break;
      }
    }
    break; //osd menu or wizard has been displayed for this loop	- break out of display function

  case 2: //regular osd display
    switch (osd_display_element) {
    case 0:
      if ((*callsign1 & 0x01) == 0x01)
        osd_print("ALIENWHOOP", decode_attribute(*callsign1), decode_positionx(*callsign1), decode_positiony(*callsign1)); //todo - needs to be pulled from the new register flash method
      osd_display_element++;
      break; //screen has been displayed for this loop - break out of display function

    case 1:
      if ((*fuelgauge_volts & 0x01) == 1) {
        uint8_t osd_fuelgauge_volts[5];
        fast_fprint(osd_fuelgauge_volts, 4, vbatt_comp, 1);
        osd_fuelgauge_volts[4] = 'V';
        osd_print_data(osd_fuelgauge_volts, 5, decode_attribute(*fuelgauge_volts), decode_positionx(*fuelgauge_volts) + 3, decode_positiony(*fuelgauge_volts));
      }
      osd_display_element++;
      break;

    case 2:
      if ((*fuelgauge_volts & 0x01) == 1) {
        if (lowbatt != last_lowbatt_state) {
          uint8_t osd_cellcount[2] = {lipo_cell_count + 48, 'S'};
          if (!lowbatt) {
            osd_print_data(osd_cellcount, 2, decode_attribute(*fuelgauge_volts), decode_positionx(*fuelgauge_volts), decode_positiony(*fuelgauge_volts));
          } else {
            osd_print_data(osd_cellcount, 2, BLINK | INVERT, decode_positionx(*fuelgauge_volts), decode_positiony(*fuelgauge_volts));
          }
          last_lowbatt_state = lowbatt;
        }
      }
      osd_display_element++;
      break;

    case 3:
      display_trigger++;
      if (display_trigger == 0)
        osd_display_element = 1;
      break;
    }
    break;

  case 3:		//pids profile menu
      switch (osd_menu_phase) {
      case 0:
          osd_clear();
          extern unsigned long lastlooptime;
          lastlooptime = gettime();
          osd_menu_phase++;
          break;
      case 1:
    	  osd_print("PID PROFILES", INVERT, 9, 1);
    	  osd_menu_phase++;
    	  break;
      case 2:
          osd_print("PID PROFILE 1", user_selection(1, 2), 7, 4);
          osd_menu_phase++;
          break;
      case 3:
          osd_print("PID PROFILE 2", user_selection(2, 2), 7, 5);
          osd_menu_phase++;
          break;
      case 4:
    	  osd_submenu_select (&profile.pid.pid_profile, 2 , pid_submenu_map);
    	  break;
      }
    break;

  case 4:		//pids profiles
      switch (osd_menu_phase) {
      case 0:
          osd_clear();
          extern unsigned long lastlooptime;
          lastlooptime = gettime();
          osd_menu_phase++;
          break;
      case 1:
    	  if(profile.pid.pid_profile == PID_PROFILE_1){
    	  osd_print("PID PROFILE 1", INVERT, 9, 1);
    	  osd_menu_phase++;
    	  }else{
          osd_print("PID PROFILE 2", INVERT, 9, 1);
          osd_menu_phase++;
    	  }
    	  break;
      case 2:
          osd_print("ROLL", TEXT, 10, 4);
          osd_menu_phase++;
          break;
      case 3:
    	  osd_print("PITCH", TEXT, 16, 4);
          osd_menu_phase++;
          break;
      case 4:
    	  osd_print("YAW", TEXT, 23, 4);
          osd_menu_phase++;
          break;
      case 5:
          osd_print("KP", user_selection(1, 4), 4, 6);
          osd_menu_phase++;
          break;
      case 6:
    	  osd_print("KI", user_selection(2, 4), 4, 7);
          osd_menu_phase++;
          break;
      case 7:
          osd_print("KD", user_selection(3, 4), 4, 8);
          osd_menu_phase++;
          break;
      case 8:
          osd_print("SAVE AND EXIT", user_selection(4, 4), 2, 14);
          osd_menu_phase++;
          break;
      case 9:
    	  if (profile.pid.pid_profile == PID_PROFILE_1 || profile.pid.pid_profile == PID_PROFILE_2){
          uint8_t osd_roll_kp[4];
          fast_fprint(osd_roll_kp, 5, rp_kp_scale(profile.pid.pid_rates[profile.pid.pid_profile].kp.roll), 0);
          osd_print_data(osd_roll_kp, 4, grid_selection(1, 1), 10, 6);
    	  }
          osd_menu_phase++;
          break;
      case 10:
    	  if (profile.pid.pid_profile == PID_PROFILE_1 || profile.pid.pid_profile == PID_PROFILE_2){
          uint8_t osd_pitch_kp[4];
          fast_fprint(osd_pitch_kp, 5, rp_kp_scale(profile.pid.pid_rates[profile.pid.pid_profile].kp.pitch), 0);
          osd_print_data(osd_pitch_kp, 4, grid_selection(2, 1), 16, 6);
    	  }
          osd_menu_phase++;
          break;
      case 11:
    	  if (profile.pid.pid_profile == PID_PROFILE_1 || profile.pid.pid_profile == PID_PROFILE_2){
          uint8_t osd_yaw_kp[4];
          fast_fprint(osd_yaw_kp, 5, yaw_kp_scale(profile.pid.pid_rates[profile.pid.pid_profile].kp.yaw), 0);
          osd_print_data(osd_yaw_kp, 4, grid_selection(3, 1), 22, 6);
    	  }
          osd_menu_phase++;
          break;
      case 12:
    	  if (profile.pid.pid_profile == PID_PROFILE_1 || profile.pid.pid_profile == PID_PROFILE_2){
          uint8_t osd_roll_ki[4];
          fast_fprint(osd_roll_ki, 5, ki_scale(profile.pid.pid_rates[profile.pid.pid_profile].ki.roll), 0);
          osd_print_data(osd_roll_ki, 4, grid_selection(1, 2), 10, 7);
    	  }
          osd_menu_phase++;
          break;
      case 13:
    	  if (profile.pid.pid_profile == PID_PROFILE_1 || profile.pid.pid_profile == PID_PROFILE_2){
          uint8_t osd_pitch_ki[4];
          fast_fprint(osd_pitch_ki, 5, ki_scale(profile.pid.pid_rates[profile.pid.pid_profile].ki.pitch), 0);
          osd_print_data(osd_pitch_ki, 4, grid_selection(2, 2), 16, 7);
    	  }
          osd_menu_phase++;
          break;
      case 14:
    	  if (profile.pid.pid_profile == PID_PROFILE_1 || profile.pid.pid_profile == PID_PROFILE_2){
          uint8_t osd_yaw_ki[4];
          fast_fprint(osd_yaw_ki, 5, ki_scale(profile.pid.pid_rates[profile.pid.pid_profile].ki.yaw), 0);
          osd_print_data(osd_yaw_ki, 4, grid_selection(3, 2), 22, 7);
    	  }
          osd_menu_phase++;
          break;
      case 15:
    	  if (profile.pid.pid_profile == PID_PROFILE_1 || profile.pid.pid_profile == PID_PROFILE_2){
          uint8_t osd_roll_kd[4];
          fast_fprint(osd_roll_kd, 5, kd_scale(profile.pid.pid_rates[profile.pid.pid_profile].kd.roll), 0);
          osd_print_data(osd_roll_kd, 4, grid_selection(1, 3), 10, 8);
    	  }
          osd_menu_phase++;
          break;
      case 16:
    	  if (profile.pid.pid_profile == PID_PROFILE_1 || profile.pid.pid_profile == PID_PROFILE_2){
          uint8_t osd_pitch_kd[4];
          fast_fprint(osd_pitch_kd, 5, kd_scale(profile.pid.pid_rates[profile.pid.pid_profile].kd.pitch), 0);
          osd_print_data(osd_pitch_kd, 4, grid_selection(2, 3), 16, 8);
    	  }
          osd_menu_phase++;
          break;
      case 17:
    	  if (profile.pid.pid_profile == PID_PROFILE_1 || profile.pid.pid_profile == PID_PROFILE_2){
          uint8_t osd_yaw_kd[4];
          fast_fprint(osd_yaw_kd, 5, kd_scale(profile.pid.pid_rates[profile.pid.pid_profile].kd.yaw), 0);
          osd_print_data(osd_yaw_kd, 4, grid_selection(3, 3), 22, 8);
    	  }
          osd_menu_phase++;
          break;
      case 18:
    	  osd_vector_adjust(get_pid_term(osd_cursor), 3, 3, BF_PIDS);
    	  break;
      }
    break;

  case 5:		//filters menu
      switch (osd_menu_phase) {
      case 0:
          osd_clear();
          extern unsigned long lastlooptime;
          lastlooptime = gettime();
          osd_menu_phase++;
          break;
      case 1:
    	  osd_print("FILTERS", INVERT, 11, 1);
    	  osd_menu_phase++;
    	  break;
      case 2:
          osd_print("UNDER", user_selection(1, 2), 7, 4);
          osd_menu_phase++;
          break;
      case 3:
          osd_print("DEVELOPMENT", user_selection(2, 2), 7, 5);
          osd_menu_phase++;
          break;
      case 4:
    	  //osd_select_filters();
    	  break;
      }
    break;

  case 6:		//main rates menu
      switch (osd_menu_phase) {
      case 0:
          osd_clear();
          extern unsigned long lastlooptime;
          lastlooptime = gettime();
          osd_menu_phase++;
          break;
      case 1:
    	  osd_print("RATES", INVERT, 13, 1);
    	  osd_menu_phase++;
    	  break;
      case 2:
          osd_print("SILVERWARE", user_selection(1, 2), 7, 4);
          osd_menu_phase++;
          break;
      case 3:
          osd_print("BETAFLIGHT", user_selection(2, 2), 7, 5);
          osd_menu_phase++;
          break;
      case 4:
    	  osd_submenu_select (&profile.rate.mode, 2 , rates_submenu_map);
    	  break;
      }
    break;

  case 7:		//silverware rates submenu
      switch (osd_menu_phase) {
      case 0:
          osd_clear();
          extern unsigned long lastlooptime;
          lastlooptime = gettime();
          osd_menu_phase++;
          break;
      case 1:
    	  osd_print("SILVERWARE RATES", INVERT, 7, 1);
    	  osd_menu_phase++;
    	  break;
      case 2:
          osd_print("ROLL", TEXT, 14, 4);
          osd_menu_phase++;
          break;
      case 3:
    	  osd_print("PITCH", TEXT, 19, 4);
          osd_menu_phase++;
          break;
      case 4:
    	  osd_print("YAW", TEXT, 25, 4);
          osd_menu_phase++;
          break;
      case 5:
          osd_print("RATE", user_selection(1, 4), 2, 6);
          osd_menu_phase++;
          break;
      case 6:
    	  osd_print("ACRO EXPO", user_selection(2, 4), 2, 7);
          osd_menu_phase++;
          break;
      case 7:
          osd_print("ANGLE EXPO", user_selection(3, 4), 2, 8);
          osd_menu_phase++;
          break;
      case 8:
          osd_print("SAVE AND EXIT", user_selection(4, 4), 2, 14);
          osd_menu_phase++;
          break;
      case 9:
    	  if (profile.rate.mode == RATE_MODE_SILVERWARE){
          uint8_t osd_roll_rate[4];
          fast_fprint(osd_roll_rate, 5, profile.rate.silverware.max_rate.roll, 0);
          osd_print_data(osd_roll_rate, 4, grid_selection(1, 1), 14, 6);
    	  }
          osd_menu_phase++;
          break;
      case 10:
    	  if (profile.rate.mode == RATE_MODE_SILVERWARE){
          uint8_t osd_pitch_rate[4];
          fast_fprint(osd_pitch_rate, 5, profile.rate.silverware.max_rate.pitch, 0);
          osd_print_data(osd_pitch_rate, 4, grid_selection(2, 1), 19, 6);
    	  }
          osd_menu_phase++;
          break;
      case 11:
    	  if (profile.rate.mode == RATE_MODE_SILVERWARE){
          uint8_t osd_yaw_rate[4];
          fast_fprint(osd_yaw_rate, 5, profile.rate.silverware.max_rate.yaw, 0);
          osd_print_data(osd_yaw_rate, 4, grid_selection(3, 1), 24, 6);
    	  }
          osd_menu_phase++;
          break;
      case 12:
    	  if (profile.rate.mode == RATE_MODE_SILVERWARE){
          uint8_t osd_acro_expo_roll[4];
          fast_fprint(osd_acro_expo_roll, 4, profile.rate.silverware.acro_expo.roll + FLT_EPSILON, 2);
          osd_print_data(osd_acro_expo_roll, 4, grid_selection(1, 2), 14, 7);
    	  }
          osd_menu_phase++;
          break;
      case 13:
    	  if (profile.rate.mode == RATE_MODE_SILVERWARE){
          uint8_t osd_acro_expo_pitch[4];
          fast_fprint(osd_acro_expo_pitch, 4, profile.rate.silverware.acro_expo.pitch + FLT_EPSILON, 2);
          osd_print_data(osd_acro_expo_pitch, 4, grid_selection(2, 2), 19, 7);
    	  }
          osd_menu_phase++;
          break;
      case 14:
    	  if (profile.rate.mode == RATE_MODE_SILVERWARE){
          uint8_t osd_acro_expo_yaw[4];
          fast_fprint(osd_acro_expo_yaw, 4, profile.rate.silverware.acro_expo.yaw + FLT_EPSILON, 2);
          osd_print_data(osd_acro_expo_yaw, 4, grid_selection(3, 2), 24, 7);
    	  }
          osd_menu_phase++;
          break;
      case 15:
    	  if (profile.rate.mode == RATE_MODE_SILVERWARE){
          uint8_t osd_angle_expo_roll[4];
          fast_fprint(osd_angle_expo_roll, 4, profile.rate.silverware.angle_expo.roll + FLT_EPSILON, 2);
          osd_print_data(osd_angle_expo_roll, 4, grid_selection(1, 3), 14, 8);
    	  }
          osd_menu_phase++;
          break;
      case 16:
    	  if (profile.rate.mode == RATE_MODE_SILVERWARE){
          uint8_t osd_angle_expo_pitch[4];
          fast_fprint(osd_angle_expo_pitch, 4, profile.rate.silverware.angle_expo.pitch + FLT_EPSILON, 2);
          osd_print_data(osd_angle_expo_pitch, 4, grid_selection(2, 3), 19, 8);
    	  }
          osd_menu_phase++;
          break;
      case 17:
    	  if (profile.rate.mode == RATE_MODE_SILVERWARE){
          uint8_t osd_angle_expo_yaw[4];
          fast_fprint(osd_angle_expo_yaw, 4, profile.rate.silverware.angle_expo.yaw + FLT_EPSILON, 2);
          osd_print_data(osd_angle_expo_yaw, 4, grid_selection(3, 3), 24, 8);
    	  }
          osd_menu_phase++;
          break;
      case 18:
    	  osd_vector_adjust(get_sw_rate_term(osd_cursor), 3, 3, SW_RATES);
    	  break;
      }
    break;

  case 8:		//betaflight rates submenu
      switch (osd_menu_phase) {
      case 0:
          osd_clear();
          extern unsigned long lastlooptime;
          lastlooptime = gettime();
          osd_menu_phase++;
          break;
      case 1:
    	  osd_print("BETAFLIGHT RATES", INVERT, 7, 1);
    	  osd_menu_phase++;
    	  break;
      case 2:
          osd_print("ROLL", TEXT, 14, 4);
          osd_menu_phase++;
          break;
      case 3:
    	  osd_print("PITCH", TEXT, 19, 4);
          osd_menu_phase++;
          break;
      case 4:
    	  osd_print("YAW", TEXT, 25, 4);
          osd_menu_phase++;
          break;
      case 5:
          osd_print("RC RATE", user_selection(1, 4), 2, 6);
          osd_menu_phase++;
          break;
      case 6:
    	  osd_print("SUPER RATE", user_selection(2, 4), 2, 7);
          osd_menu_phase++;
          break;
      case 7:
          osd_print("EXPO", user_selection(3, 4), 2, 8);
          osd_menu_phase++;
          break;
      case 8:
          osd_print("SAVE AND EXIT", user_selection(4, 4), 2, 14);
          osd_menu_phase++;
          break;
      case 9:
    	  if (profile.rate.mode == RATE_MODE_BETAFLIGHT){
          uint8_t osd_roll_rate[4];
          fast_fprint(osd_roll_rate, 4, profile.rate.betaflight.rc_rate.roll + FLT_EPSILON, 2);
          osd_print_data(osd_roll_rate, 4, grid_selection(1, 1), 14, 6);
    	  }
          osd_menu_phase++;
          break;
      case 10:
    	  if (profile.rate.mode == RATE_MODE_BETAFLIGHT){
          uint8_t osd_pitch_rate[4];
          fast_fprint(osd_pitch_rate, 4, profile.rate.betaflight.rc_rate.pitch + FLT_EPSILON, 2);
          osd_print_data(osd_pitch_rate, 4, grid_selection(2, 1), 19, 6);
    	  }
          osd_menu_phase++;
          break;
      case 11:
    	  if (profile.rate.mode == RATE_MODE_BETAFLIGHT){
          uint8_t osd_yaw_rate[4];
          fast_fprint(osd_yaw_rate, 4, profile.rate.betaflight.rc_rate.yaw + FLT_EPSILON, 2);
          osd_print_data(osd_yaw_rate, 4, grid_selection(3, 1), 24, 6);
    	  }
          osd_menu_phase++;
          break;
      case 12:
    	  if (profile.rate.mode == RATE_MODE_BETAFLIGHT){
          uint8_t osd_acro_expo_roll[4];
          fast_fprint(osd_acro_expo_roll, 4, profile.rate.betaflight.super_rate.roll + FLT_EPSILON, 2);
          osd_print_data(osd_acro_expo_roll, 4, grid_selection(1, 2), 14, 7);
    	  }
          osd_menu_phase++;
          break;
      case 13:
    	  if (profile.rate.mode == RATE_MODE_BETAFLIGHT){
          uint8_t osd_acro_expo_pitch[4];
          fast_fprint(osd_acro_expo_pitch, 4, profile.rate.betaflight.super_rate.pitch + FLT_EPSILON, 2);
          osd_print_data(osd_acro_expo_pitch, 4, grid_selection(2, 2), 19, 7);
    	  }
          osd_menu_phase++;
          break;
      case 14:
    	  if (profile.rate.mode == RATE_MODE_BETAFLIGHT){
          uint8_t osd_acro_expo_yaw[4];
          fast_fprint(osd_acro_expo_yaw, 4, profile.rate.betaflight.super_rate.yaw + FLT_EPSILON, 2);
          osd_print_data(osd_acro_expo_yaw, 4, grid_selection(3, 2), 24, 7);
    	  }
          osd_menu_phase++;
          break;
      case 15:
    	  if (profile.rate.mode == RATE_MODE_BETAFLIGHT){
          uint8_t osd_angle_expo_roll[4];
          fast_fprint(osd_angle_expo_roll, 4, profile.rate.betaflight.expo.roll + FLT_EPSILON, 2);
          osd_print_data(osd_angle_expo_roll, 4, grid_selection(1, 3), 14, 8);
    	  }
          osd_menu_phase++;
          break;
      case 16:
    	  if (profile.rate.mode == RATE_MODE_BETAFLIGHT){
          uint8_t osd_angle_expo_pitch[4];
          fast_fprint(osd_angle_expo_pitch, 4, profile.rate.betaflight.expo.pitch + FLT_EPSILON, 2);
          osd_print_data(osd_angle_expo_pitch, 4, grid_selection(2, 3), 19, 8);
    	  }
          osd_menu_phase++;
          break;
      case 17:
    	  if (profile.rate.mode == RATE_MODE_BETAFLIGHT){
          uint8_t osd_angle_expo_yaw[4];
          fast_fprint(osd_angle_expo_yaw, 4, profile.rate.betaflight.expo.yaw + FLT_EPSILON, 2);
          osd_print_data(osd_angle_expo_yaw, 4, grid_selection(3, 3), 24, 8);
    	  }
          osd_menu_phase++;
          break;
      case 18:
    	  osd_vector_adjust(get_bf_rate_term(osd_cursor), 3, 3, ROUNDED);
    	  break;
      }
    break;

  case 9:		//flight modes menu
      switch (osd_menu_phase) {
      case 0:
          osd_clear();
          extern unsigned long lastlooptime;
          lastlooptime = gettime();
          osd_menu_phase++;
          break;
      case 1:
    	  osd_print("FLIGHT MODES", INVERT, 9, 1);
    	  osd_menu_phase++;
    	  break;
      case 2:
          osd_print("ARMING", user_selection(1, 11), 4, 2);
          osd_menu_phase++;
          break;
      case 3:
          osd_print("IDLE UP", user_selection(2, 11), 4, 3);
          osd_menu_phase++;
          break;
      case 4:
          osd_print("LEVELMODE", user_selection(3, 11), 4, 4);
          osd_menu_phase++;
          break;
      case 5:
          osd_print("RACEMODE", user_selection(4, 11), 4, 5);
          osd_menu_phase++;
          break;
      case 6:
          osd_print("HORIZON", user_selection(5, 11), 4, 6);
          osd_menu_phase++;
          break;
      case 7:
          osd_print("STICK BOOST", user_selection(6, 11), 4, 7);
          osd_menu_phase++;
          break;
      case 8:
          osd_print("HIGH RATES", user_selection(7, 11), 4, 8);
          osd_menu_phase++;
          break;
      case 9:
          osd_print("BUZZER", user_selection(8, 11), 4, 9);
          osd_menu_phase++;
          break;
      case 10:
          osd_print("TURTLE", user_selection(9, 11), 4, 10);
          osd_menu_phase++;
          break;
      case 11:
          osd_print("MOTOR TEST", user_selection(10, 11), 4, 11);
          osd_menu_phase++;
          break;
      case 12:
          osd_print("SAVE AND EXIT", user_selection(11, 11), 4, 14);
          osd_menu_phase++;
          break;
      case 13:
    	  osd_print(get_aux_status(profile.channel.aux[AUX_ARMING]), grid_selection(1, 1), 17, 2);
          osd_menu_phase++;
          break;
      case 14:
    	  osd_print(get_aux_status(profile.channel.aux[AUX_IDLE_UP]), grid_selection(1, 2), 17, 3);
          osd_menu_phase++;
          break;
      case 15:
    	  osd_print(get_aux_status(profile.channel.aux[AUX_LEVELMODE]), grid_selection(1, 3), 17, 4);
          osd_menu_phase++;
          break;
      case 16:
    	  osd_print(get_aux_status(profile.channel.aux[AUX_RACEMODE]), grid_selection(1, 4), 17, 5);
          osd_menu_phase++;
          break;
      case 17:
    	  osd_print(get_aux_status(profile.channel.aux[AUX_HORIZON]), grid_selection(1, 5), 17, 6);
          osd_menu_phase++;
          break;
      case 18:
    	  osd_print(get_aux_status(profile.channel.aux[AUX_PIDPROFILE]), grid_selection(1, 6), 17, 7);
          osd_menu_phase++;
          break;
      case 19:
    	  osd_print(get_aux_status(profile.channel.aux[AUX_RATES]), grid_selection(1, 7), 17, 8);
          osd_menu_phase++;
          break;
      case 20:
    	  osd_print(get_aux_status(profile.channel.aux[AUX_BUZZER_ENABLE]), grid_selection(1, 8), 17, 9);
          osd_menu_phase++;
          break;
      case 21:
    	  osd_print(get_aux_status(profile.channel.aux[AUX_STARTFLIP]), grid_selection(1, 9), 17, 10);
          osd_menu_phase++;
          break;
      case 22:
    	  osd_print(get_aux_status(profile.channel.aux[AUX_MOTORS_TO_THROTTLE_MODE]), grid_selection(1, 10), 17, 11);
          osd_menu_phase++;
          break;
      case 23:
    	  osd_enum_adjust(&profile.channel.aux[flight_mode_aux_items[osd_cursor-1]], 10, flight_mode_aux_limits);
    	  break;
      }
    break;

  case 10:		//osd elements menu
      switch (osd_menu_phase) {
      case 0:
          osd_clear();
          extern unsigned long lastlooptime;
          lastlooptime = gettime();
          osd_menu_phase++;
          break;
      case 1:
    	  osd_print("OSD ELEMENTS", INVERT, 9, 1);
    	  osd_menu_phase++;
    	  break;
      case 2:
          osd_print("UNDER", user_selection(1, 2), 7, 4);
          osd_menu_phase++;
          break;
      case 3:
          osd_print("DEVELOPMENT", user_selection(2, 2), 7, 5);
          osd_menu_phase++;
          break;
      case 4:
    	  //osd_select_elements();
    	  break;
      }
    break;

  case 11:		//vtx
      switch (osd_menu_phase) {
      case 0:
          osd_clear();
          extern unsigned long lastlooptime;
          lastlooptime = gettime();
          osd_menu_phase++;
          break;
      case 1:
    	  osd_print("VTX CONTROLS", INVERT, 9, 1);
    	  osd_menu_phase++;
    	  break;
      case 2:
          osd_print("UNDER", user_selection(1, 2), 7, 4);
          osd_menu_phase++;
          break;
      case 3:
          osd_print("DEVELOPMENT", user_selection(2, 2), 7, 5);
          osd_menu_phase++;
          break;
      case 4:
    	  //osd_select_vtx();
    	  break;
      }
    break;

  case 12:		//special features
      switch (osd_menu_phase) {
      case 0:
          osd_clear();
          extern unsigned long lastlooptime;
          lastlooptime = gettime();
          osd_menu_phase++;
          break;
      case 1:
          osd_print("SPECIAL FEATURES", INVERT, 7, 1);
          osd_menu_phase++;
          break;
      case 2:
          osd_print("STICK BOOST", user_selection(1, 1), 7, 4);
          osd_menu_phase++;
          break;
      case 3:
    	  osd_select_menu_item(1,special_features_map, SUB_MENU);
    	  break;
      }
      break;

  case 13:		//stick accelerator profiles
      switch (osd_menu_phase) {
      case 0:
          osd_clear();
          extern unsigned long lastlooptime;
          lastlooptime = gettime();
          osd_menu_phase++;
          break;
      case 1:
          osd_print("STICK BOOST PROFILES", INVERT, 5, 1);
          osd_menu_phase++;
          break;
      case 2:
          osd_print("AUX OFF PROFILE 1", user_selection(1, 2), 7, 4);
          osd_menu_phase++;
          break;
      case 3:
          osd_print("AUX ON  PROFILE 2", user_selection(2, 2), 7, 5);
          osd_menu_phase++;
          break;
      case 4:
    	  osd_submenu_select (&profile.pid.stick_profile, 2 , stickboost_submenu_map);
    	  break;
      }
      break;

  case 14:		//stick boost profiles
      switch (osd_menu_phase) {
      case 0:
          osd_clear();
          extern unsigned long lastlooptime;
          lastlooptime = gettime();
          osd_menu_phase++;
          break;
      case 1:
    	  if(profile.pid.stick_profile == STICK_PROFILE_1){
    	  osd_print("BOOST PROFILE 1", INVERT, 8, 1);
    	  osd_menu_phase++;
    	  }else{
          osd_print("BOOST PROFILE 2", INVERT, 8, 1);
          osd_menu_phase++;
    	  }
    	  break;
      case 2:
          osd_print("ROLL", TEXT, 14, 4);
          osd_menu_phase++;
          break;
      case 3:
    	  osd_print("PITCH", TEXT, 19, 4);
          osd_menu_phase++;
          break;
      case 4:
    	  osd_print("YAW", TEXT, 25, 4);
          osd_menu_phase++;
          break;
      case 5:
          osd_print("STICK", user_selection(1, 3), 2, 5);
          osd_menu_phase++;
          break;
      case 6:
          osd_print("ACCELERATOR", user_selection(1, 3), 2, 6);
          osd_menu_phase++;
          break;
      case 7:
          osd_print("STICK", user_selection(2, 3), 2, 7);
          osd_menu_phase++;
          break;
      case 8:
    	  osd_print("TRANSITION", user_selection(2, 3), 2, 8);
          osd_menu_phase++;
          break;
      case 9:
          osd_print("SAVE AND EXIT", user_selection(3, 3), 2, 14);
          osd_menu_phase++;
          break;
      case 10:
    	  if (profile.pid.stick_profile == STICK_PROFILE_1 || profile.pid.stick_profile == STICK_PROFILE_2){
          uint8_t osd_roll_accel[4];
          fast_fprint(osd_roll_accel, 4, profile.pid.stick_rates[profile.pid.stick_profile].accelerator.roll + FLT_EPSILON, 2);
          osd_print_data(osd_roll_accel, 4, grid_selection(1, 1), 14, 6);
    	  }
          osd_menu_phase++;
          break;
      case 11:
    	  if (profile.pid.stick_profile == STICK_PROFILE_1 || profile.pid.stick_profile == STICK_PROFILE_2){
          uint8_t osd_pitch_accel[4];
          fast_fprint(osd_pitch_accel, 4, profile.pid.stick_rates[profile.pid.stick_profile].accelerator.pitch + FLT_EPSILON, 2);
          osd_print_data(osd_pitch_accel, 4, grid_selection(2, 1), 19, 6);
    	  }
          osd_menu_phase++;
          break;
      case 12:
    	  if (profile.pid.stick_profile == STICK_PROFILE_1 || profile.pid.stick_profile == STICK_PROFILE_2){
          uint8_t osd_yaw_accel[4];
          fast_fprint(osd_yaw_accel, 4, profile.pid.stick_rates[profile.pid.stick_profile].accelerator.yaw + FLT_EPSILON, 2);
          osd_print_data(osd_yaw_accel, 4, grid_selection(3, 1), 24, 6);
    	  }
          osd_menu_phase++;
          break;
      case 13:
    	  if (profile.pid.stick_profile == STICK_PROFILE_1 || profile.pid.stick_profile == STICK_PROFILE_2){
          uint8_t osd_roll_trans[4];
          fast_fprint(osd_roll_trans, 4, profile.pid.stick_rates[profile.pid.stick_profile].transition.roll + FLT_EPSILON, 2);
          osd_print_data(osd_roll_trans, 4, grid_selection(1, 2), 14, 8);
    	  }
          osd_menu_phase++;
          break;
      case 14:
    	  if (profile.pid.stick_profile == STICK_PROFILE_1 || profile.pid.stick_profile == STICK_PROFILE_2){
          uint8_t osd_pitch_trans[4];
          fast_fprint(osd_pitch_trans, 4, profile.pid.stick_rates[profile.pid.stick_profile].transition.pitch + FLT_EPSILON, 2);
          osd_print_data(osd_pitch_trans, 4, grid_selection(2, 2), 19, 8);
    	  }
          osd_menu_phase++;
          break;
      case 15:
    	  if (profile.pid.stick_profile == STICK_PROFILE_1 || profile.pid.stick_profile == STICK_PROFILE_2){
          uint8_t osd_yaw_trans[4];
          fast_fprint(osd_yaw_trans, 4, profile.pid.stick_rates[profile.pid.stick_profile].transition.yaw + FLT_EPSILON, 2);
          osd_print_data(osd_yaw_trans, 4, grid_selection(3, 2), 24, 8);
    	  }
          osd_menu_phase++;
          break;
      case 16:
    	  osd_vector_adjust(get_stick_profile_term(osd_cursor), 2, 3, ROUNDED);
    	  break;
      }
    break;

  }

} //end osd_display()
//******************************************************************************************************************************
#endif

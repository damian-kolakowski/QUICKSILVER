#include "osd_render.h"

#include "control.h"
#include "debug.h"
#include "drv_osd.h"
#include "drv_time.h"
#include "filter.h"
#include "flash.h"
#include "float.h"
#include "osd_adjust.h"
#include "osd_menu.h"
#include "osd_menu_maps.h"
#include "profile.h"
#include "project.h"
#include "rx.h"
#include "stdio.h"
#include "string.h"
#include "util.h"
#include "vtx.h"

#ifdef ENABLE_OSD

typedef enum {
  CLEAR,
  DISARM,
  ARM,
  BOOST_1,
  BOOST_2,
  FAILSAFE,
  THRTL_SFTY,
  ARM_SFTY,
  LOW_BAT,
  MOTOR_TEST,
  TURTL,
  LOOP
} osd_status_labels_t;

typedef struct {
  uint8_t loop_warning;
  uint8_t aux_boost;
  uint8_t aux_motor_test;
  control_flags_t flags;
} osd_sys_status_t;

#define ICON_RSSI 0x1
#define ICON_CELSIUS 0xe
#define ICON_THROTTLE 0x4
#define ICON_AMP 0x9a

#define MAIN_MENU 1
#define SUB_MENU 0

#define HOLD 0
#define TEMP 1

extern profile_t profile;
extern vtx_settings_t vtx_settings;
extern vtx_settings_t vtx_settings_copy;

osd_system_t osd_system = OSD_SYS_NONE;

osd_state_t osd_state = {
    .element = OSD_CALLSIGN,

    .screen = OSD_SCREEN_REGULAR,
    .screen_history_size = 0,
    .screen_phase = 0,

    .cursor = 0,
    .cursor_history_size = 0,

    .selection = 0,
    .selection_increase = 0,
    .selection_decrease = 0,

    .reboot_fc_requested = 0,
};

static const char *osd_element_labels[] = {
    "CALLSIGN",
    "FUELGAUGE VOLTS",
    "FILTERED VOLTS",
    "GYRO TEMP",
    "FLIGHT MODE",
    "RSSI",
    "STOPWATCH",
    "SYSTEM STATUS",
    "THROTTLE",
    "VTX",
};

static uint8_t osd_attr(osd_element_t *el) {
  return el->attribute ? OSD_ATTR_INVERT : OSD_ATTR_TEXT;
}

uint32_t *osd_elements() {
  if (osd_system == OSD_SYS_HD) {
    return profile.osd.elements_hd;
  }
  return profile.osd.elements;
}

void osd_display_reset() {
  osd_state.element = OSD_CALLSIGN;

  osd_state.screen = OSD_SCREEN_REGULAR;
  osd_state.screen_phase = 0;
  osd_state.screen_history_size = 0;

  osd_state.cursor = 1;
  osd_state.cursor_history_size = 0;
}

static void osd_update_screen(osd_screens_t screen) {
  osd_state.screen = screen;
  osd_state.screen_phase = 0;
}

osd_screens_t osd_push_screen(osd_screens_t screen) {
  osd_state.screen_history[osd_state.screen_history_size] = osd_state.screen;
  osd_update_screen(screen);
  osd_state.screen_history_size++;

  osd_state.cursor = 1;

  return screen;
}

osd_screens_t osd_pop_screen() {
  if (osd_state.screen_history_size <= 1) {
    // first history entry is always the REGULAR display
    // clear everything off the screen and reset
    osd_update_screen(OSD_SCREEN_CLEAR);
    return OSD_SCREEN_CLEAR;
  }

  const uint32_t screen = osd_state.screen_history[osd_state.screen_history_size - 1];
  osd_update_screen(screen);
  osd_state.screen_history_size--;

  return screen;
}

void osd_handle_input(osd_input_t input) {
  switch (input) {
  case OSD_INPUT_UP:
    if (osd_state.selection) {
      osd_state.selection_increase = 1;
    } else {
      if (osd_state.cursor == 1) {
        osd_state.cursor = osd_state.cursor_max - osd_state.cursor_min + 1;
      } else {
        osd_state.cursor--;
      }
      osd_state.screen_phase = 1;
    }
    break;

  case OSD_INPUT_DOWN:
    if (osd_state.selection) {
      osd_state.selection_decrease = 1;
    } else {
      if (osd_state.cursor == (osd_state.cursor_max - osd_state.cursor_min + 1)) {
        osd_state.cursor = 1;
      } else {
        osd_state.cursor++;
      }
      osd_state.screen_phase = 1;
    }
    break;

  case OSD_INPUT_LEFT:
    if (osd_state.selection) {
      osd_state.selection--;
      osd_state.screen_phase = 1;
    } else {
      osd_pop_cursor();
      osd_pop_screen();
    }
    break;

  case OSD_INPUT_RIGHT:
    osd_state.selection++;
    osd_state.screen_phase = 1;
    break;
  }
}

static bool osd_is_selected(const osd_label_t *labels, const uint8_t size) {
  if (osd_state.screen_phase > 1 && osd_state.screen_phase <= osd_state.cursor_min) {
    return false;
  }

  if (osd_state.cursor == (osd_state.screen_phase - osd_state.cursor_min) && osd_state.selection == 0) {
    return true;
  }

  return false;
}

void osd_update_cursor(const osd_label_t *labels, const uint8_t size) {
  osd_state.cursor_min = size;
  osd_state.cursor_max = 0;

  for (uint32_t i = 0; i < size; i++) {
    if (labels[i].type != OSD_LABEL_ACTIVE) {
      continue;
    }
    if (i < osd_state.cursor_min) {
      osd_state.cursor_min = i;
    }
    if (i > osd_state.cursor_max) {
      osd_state.cursor_max = i;
    }
  }
}

uint8_t grid_selection(uint8_t element, uint8_t row) {
  if (osd_state.selection == element && osd_state.cursor == row) {
    return OSD_ATTR_INVERT;
  } else {
    return OSD_ATTR_TEXT;
  }
}

//************************************************************************************************************************************************************************************
//																					PRINT FUNCTIONS
//************************************************************************************************************************************************************************************

void print_osd_flightmode(osd_element_t *el) {
  const uint8_t flightmode_labels[5][21] = {
      {"   ACRO   "},
      {"  LEVEL   "},
      {" RACEMODE "},
      {" HORIZON  "},
      {"RM HORIZON"},
  };

  uint8_t flightmode;
  if (rx_aux_on(AUX_LEVELMODE)) {
    if (rx_aux_on(AUX_RACEMODE) && rx_aux_on(AUX_HORIZON))
      flightmode = 4;
    if (!rx_aux_on(AUX_RACEMODE) && rx_aux_on(AUX_HORIZON))
      flightmode = 3;
    if (rx_aux_on(AUX_RACEMODE) && !rx_aux_on(AUX_HORIZON))
      flightmode = 2;
    if (!rx_aux_on(AUX_RACEMODE) && !rx_aux_on(AUX_HORIZON))
      flightmode = 1;
  } else {
    flightmode = 0;
  }

  osd_transaction_t *txn = osd_txn_init();
  osd_txn_start(osd_attr(el), el->pos_x, el->pos_y);
  osd_txn_write_data(flightmode_labels[flightmode], 21);
  osd_txn_submit(txn);
}

void print_osd_rssi(osd_element_t *el) {
  static float rx_rssi_filt;
  if (flags.failsafe)
    state.rx_rssi = 0.0f;

  lpf(&rx_rssi_filt, state.rx_rssi, FILTERCALC(state.looptime * 1e6f * 133.0f, 2e6f)); // 2 second filtertime and 15hz refresh rate @4k, 30hz@ 8k loop

  osd_transaction_t *txn = osd_txn_init();
  osd_txn_start(osd_attr(el), el->pos_x, el->pos_y);
  osd_txn_write_uint(rx_rssi_filt - 0.5f, 4);
  osd_txn_write_char(ICON_RSSI);
  osd_txn_submit(txn);
}

void print_osd_armtime(osd_element_t *el) {
  uint32_t time_s = state.armtime;

  // Will only display up to 59:59 as realistically no quad will fly that long (currently).
  // Reset to zero at on reaching 1 hr
  while (time_s >= 3600) {
    time_s -= 3600;
  }

  osd_transaction_t *txn = osd_txn_init();
  osd_txn_start(osd_attr(el), el->pos_x, el->pos_y);

  const uint32_t minutes = time_s / 60;
  osd_txn_write_uint(minutes % 10, 1);
  osd_txn_write_uint(minutes / 10, 1);

  osd_txn_write_char(':');

  const uint32_t seconds = time_s % 60;
  osd_txn_write_uint(seconds % 10, 1);
  osd_txn_write_uint(seconds / 10, 1);

  osd_txn_submit(txn);
}

// print the current vtx settings as Band:Channel:Power
void print_osd_vtx(osd_element_t *el) {
  uint8_t str[5];

  switch (vtx_settings.band) {
  case VTX_BAND_A:
    str[0] = 'A';
    break;
  case VTX_BAND_B:
    str[0] = 'B';
    break;
  case VTX_BAND_E:
    str[0] = 'E';
    break;
  case VTX_BAND_F:
    str[0] = 'F';
    break;
  case VTX_BAND_R:
    str[0] = 'R';
    break;
  default:
    str[0] = 'M';
    break;
  }

  str[1] = ':';
  str[2] = vtx_settings.channel + 49;
  str[3] = ':';

  if (vtx_settings.pit_mode == 1) {
    str[4] = 21; // "pit", probably from Pitch, but we will use it here
  } else {
    if (vtx_settings.power_level == 4)
      str[4] = 36; // "max"
    else
      str[4] = vtx_settings.power_level + 49;
  }

  osd_transaction_t *txn = osd_txn_init();
  osd_txn_start(osd_attr(el), el->pos_x, el->pos_y);
  osd_txn_write_data(str, 5);
  osd_txn_submit(txn);
}

// 3 stage return - 0 = stick and hold, 1 = move on but come back to clear, 2 = status print done
uint8_t print_status(osd_element_t *el, uint8_t persistence, uint8_t label) {
  const uint8_t system_status_labels[12][21] = {
      {"               "},
      {" **DISARMED**  "},
      {"  **ARMED**    "},
      {" STICK BOOST 1 "},
      {" STICK BOOST 2 "},
      {" **FAILSAFE**  "},
      {"THROTTLE SAFETY"},
      {" ARMING SAFETY "},
      {"**LOW BATTERY**"},
      {"**MOTOR TEST** "},
      {"  **TURTLE**   "},
      {" **LOOPTIME**  "},
  };

  static uint8_t last_label;
  static uint8_t delay_counter = 25;
  if (last_label != label) { // critical to reset indexers if a print sequence is interrupted by a new request
    delay_counter = 25;
  }
  last_label = label;

  if (delay_counter == 25) {
    // First run, print the label
    osd_transaction_t *txn = osd_txn_init();
    osd_txn_start(osd_attr(el) | OSD_ATTR_BLINK, el->pos_x, el->pos_y);
    osd_txn_write_data(system_status_labels[label], 21);
    osd_txn_submit(txn);

    delay_counter--;

    return 0;
  }

  if (persistence == HOLD) {
    // label printed and we should hold
    return 2;
  }

  // label printed and its temporary
  if (!delay_counter) {
    // timer is elapsed, print clear label
    osd_transaction_t *txn = osd_txn_init();
    osd_txn_start(osd_attr(el), el->pos_x, el->pos_y);
    osd_txn_write_data(system_status_labels[0], 21);
    osd_txn_submit(txn);

    delay_counter = 25;

    return 2;
  }

  delay_counter--;
  return 1;
}

uint8_t print_osd_system_status(osd_element_t *el) {
  static uint8_t ready = 0;
  uint8_t print_stage = 2; // 0 makes the main osd function stick and non zero lets it pass on

  static osd_sys_status_t last_sys_status;
  static osd_sys_status_t printing = {
      .loop_warning = 0,
      .aux_boost = 0,
      .aux_motor_test = 0,
      .flags = {0},
  };

  extern uint8_t looptime_warning;

  // things happen here
  if (ready) {
    if ((flags.arm_state != last_sys_status.flags.arm_state) || printing.flags.arm_state) {
      last_sys_status.flags.arm_state = flags.arm_state;
      printing.flags.arm_state = 1;
      if (flags.arm_state)
        print_stage = print_status(el, TEMP, ARM);
      else
        print_stage = print_status(el, TEMP, DISARM);
      if (print_stage == 2)
        printing.flags.arm_state = 0;
      return print_stage;
    }
    if (((looptime_warning != last_sys_status.loop_warning) || printing.loop_warning) && (state.looptime_autodetect != LOOPTIME_8K)) { // mute warnings till we are on the edge of 4k->2k
      last_sys_status.loop_warning = looptime_warning;
      printing.loop_warning = 1;
      print_stage = print_status(el, TEMP, LOOP);
      if (print_stage == 2)
        printing.loop_warning = 0;
      return print_stage;
    }
    if (rx_aux_on(AUX_STICK_BOOST_PROFILE) != last_sys_status.aux_boost || printing.aux_boost) {
      last_sys_status.aux_boost = rx_aux_on(AUX_STICK_BOOST_PROFILE);
      printing.aux_boost = 1;
      if (rx_aux_on(AUX_STICK_BOOST_PROFILE))
        print_stage = print_status(el, TEMP, BOOST_2);
      else
        print_stage = print_status(el, TEMP, BOOST_1);
      if (print_stage == 2)
        printing.aux_boost = 0;
      return print_stage;
    }
    if (flags.failsafe != last_sys_status.flags.failsafe || printing.flags.failsafe) {
      last_sys_status.flags.failsafe = flags.failsafe;
      printing.flags.failsafe = 1;
      if (flags.failsafe)
        print_stage = print_status(el, HOLD, FAILSAFE);
      else
        print_stage = print_status(el, HOLD, CLEAR);
      if (print_stage == 2)
        printing.flags.failsafe = 0;
      return print_stage;
    }
    if ((flags.arm_safety && !flags.failsafe) || printing.flags.arm_safety) {
      printing.flags.arm_safety = 1;
      if (flags.arm_safety) {
        print_stage = print_status(el, HOLD, ARM_SFTY);
      } else {
        print_stage = print_status(el, HOLD, CLEAR);
        if (print_stage == 2)
          printing.flags.arm_safety = 0;
      }
      return print_stage;
    }
    if ((flags.throttle_safety && !flags.arm_safety) || printing.flags.throttle_safety) {
      printing.flags.throttle_safety = 1;
      if (flags.throttle_safety) {
        print_stage = print_status(el, HOLD, THRTL_SFTY);
      } else {
        print_stage = print_status(el, HOLD, CLEAR);
        if (print_stage == 2)
          printing.flags.throttle_safety = 0;
      }
      return print_stage;
    }
    if ((flags.lowbatt != last_sys_status.flags.lowbatt && !flags.arm_safety && !flags.throttle_safety && !flags.failsafe) || printing.flags.lowbatt) {
      last_sys_status.flags.lowbatt = flags.lowbatt;
      printing.flags.lowbatt = 1;
      if (flags.lowbatt)
        print_stage = print_status(el, HOLD, LOW_BAT);
      else
        print_stage = print_status(el, HOLD, CLEAR);
      if (print_stage == 2)
        printing.flags.lowbatt = 0;
      return print_stage;
    }
    if ((rx_aux_on(AUX_MOTOR_TEST) && !flags.arm_safety && !flags.throttle_safety && !flags.failsafe) || (printing.aux_motor_test && !flags.arm_safety && !flags.throttle_safety && !flags.failsafe)) {
      printing.aux_motor_test = 1;
      if (rx_aux_on(AUX_MOTOR_TEST)) {
        print_stage = print_status(el, HOLD, MOTOR_TEST);
      } else {
        print_stage = print_status(el, HOLD, CLEAR);
        if (print_stage == 2)
          printing.aux_motor_test = 0;
      }
      return print_stage;
    }
    if ((flags.turtle && !flags.arm_safety && !flags.throttle_safety && !flags.failsafe) || (printing.flags.turtle && !flags.arm_safety && !flags.throttle_safety && !flags.failsafe)) {
      printing.flags.turtle = 1;
      if (flags.turtle) {
        print_stage = print_status(el, HOLD, TURTL);
      } else {
        print_stage = print_status(el, HOLD, CLEAR);
        if (print_stage == 2)
          printing.flags.turtle = 0;
      }
      return print_stage;
    }
  }
  if (ready == 0) {
    last_sys_status.loop_warning = looptime_warning;
    last_sys_status.aux_boost = rx_aux_on(AUX_STICK_BOOST_PROFILE);
    last_sys_status.aux_motor_test = rx_aux_on(AUX_MOTOR_TEST);
    last_sys_status.flags = flags;
    ready = 1;
  }
  return ready;
}

void print_osd_menu_strings(const osd_label_t *labels, const uint8_t size) {
  if (osd_state.screen_phase > size) {
    return;
  }
  if (osd_state.screen_phase == 0) {
    if (osd_clear_async()) {
      osd_update_cursor(labels, size);
      osd_state.screen_phase++;
    }
    return;
  }

  const osd_label_t *label = &labels[osd_state.screen_phase - 1];

  osd_transaction_t *txn = osd_txn_init();

  switch (label->type) {
  case OSD_LABEL_HEADER:
    osd_txn_start(OSD_ATTR_INVERT, label->pos[0], label->pos[1]);
    break;

  case OSD_LABEL_INACTIVE:
    osd_txn_start(OSD_ATTR_TEXT, label->pos[0], label->pos[1]);
    break;

  case OSD_LABEL_ACTIVE: {
    const bool is_selected = osd_is_selected(labels, size);
    if (osd_system == OSD_SYS_HD) {
      if (is_selected) {
        osd_txn_start(OSD_ATTR_INVERT, label->pos[0] - 1, label->pos[1]);
        osd_txn_write_char('>');
      } else {
        osd_txn_start(OSD_ATTR_TEXT, label->pos[0] - 1, label->pos[1]);
        osd_txn_write_char(' ');
      }
    } else {
      if (is_selected) {
        osd_txn_start(OSD_ATTR_INVERT, label->pos[0], label->pos[1]);
      } else {
        osd_txn_start(OSD_ATTR_TEXT, label->pos[0], label->pos[1]);
      }
    }

    break;
  }
  }

  osd_txn_write_str(label->text);
  osd_txn_submit(txn);

  osd_state.screen_phase++;
}

void print_osd_adjustable_enums(uint8_t string_element_qty, uint8_t data_element_qty, const char data_to_print[21], const uint8_t grid[data_element_qty][2], const uint8_t print_position[data_element_qty][2]) {
  if (osd_state.screen_phase <= string_element_qty)
    return;
  if (osd_state.screen_phase > string_element_qty + data_element_qty)
    return;
  static uint8_t skip_loop = 0;
  if (osd_state.screen_phase == string_element_qty + 1 && skip_loop == 0) { // skip a loop to prevent dma collision with previous print function
    skip_loop++;
    return;
  }
  skip_loop = 0;
  uint8_t index = osd_state.screen_phase - string_element_qty - 1;

  osd_transaction_t *txn = osd_txn_init();
  osd_txn_start(grid_selection(grid[index][0], grid[index][1]), print_position[index][0], print_position[index][1]);
  osd_txn_write_str(data_to_print);
  osd_txn_submit(txn);

  osd_state.screen_phase++;
}

void print_osd_adjustable_float(uint8_t string_element_qty, uint8_t data_element_qty, float *pointer[], const uint8_t grid[data_element_qty][2], const uint8_t print_position[data_element_qty][2], uint8_t precision) {
  if (osd_state.screen_phase <= string_element_qty)
    return;
  if (osd_state.screen_phase > string_element_qty + data_element_qty)
    return;
  static uint8_t skip_loop = 0;
  if (osd_state.screen_phase == string_element_qty + 1 && skip_loop == 0) { // skip a loop to prevent dma collision with previous print function
    skip_loop++;
    return;
  }
  skip_loop = 0;

  uint8_t index = osd_state.screen_phase - string_element_qty - 1;

  osd_transaction_t *txn = osd_txn_init();
  osd_txn_start(grid_selection(grid[index][0], grid[index][1]), print_position[index][0], print_position[index][1]);
  osd_txn_write_float(*pointer[index] + FLT_EPSILON, 4, precision);
  osd_txn_submit(txn);

  osd_state.screen_phase++;
}

//************************************************************************************************************************************************************************************
//************************************************************************************************************************************************************************************
//																				MAIN OSD DISPLAY FUNCTION
//************************************************************************************************************************************************************************************
//************************************************************************************************************************************************************************************
void osd_init() {
  osd_device_init();

  // print the splash screen
  osd_intro();
}

static void osd_display_regular() {
  osd_element_t *el = (osd_element_t *)(osd_elements() + osd_state.element);
  if (osd_state.element < OSD_ELEMENT_MAX && !el->active) {
    osd_state.element++;
    return;
  }

  switch (osd_state.element) {
  case OSD_CALLSIGN: {
    osd_transaction_t *txn = osd_txn_init();
    osd_txn_start(osd_attr(el), el->pos_x, el->pos_y);
    osd_txn_write_str((const char *)profile.osd.callsign);
    osd_txn_submit(txn);
    osd_state.element++;
    break;
  }

  case OSD_FUELGAUGE_VOLTS: {
    osd_transaction_t *txn = osd_txn_init();
    if (!flags.lowbatt) {
      osd_txn_start(osd_attr(el), el->pos_x, el->pos_y);
    } else {
      osd_txn_start(OSD_ATTR_BLINK | OSD_ATTR_INVERT, el->pos_x, el->pos_y);
    }
    osd_txn_write_uint(state.lipo_cell_count, 1);
    osd_txn_write_char('S');

    osd_txn_start(osd_attr(el), el->pos_x + 3, el->pos_y);
    osd_txn_write_float(state.vbatt_comp, 4, 1);
    osd_txn_write_char('V');

    osd_txn_submit(txn);
    osd_state.element++;
    break;
  }

  case OSD_FILTERED_VOLTS: {
    osd_transaction_t *txn = osd_txn_init();
    if (!flags.lowbatt) {
      osd_txn_start(osd_attr(el), el->pos_x, el->pos_y);
    } else {
      osd_txn_start(OSD_ATTR_BLINK | OSD_ATTR_INVERT, el->pos_x, el->pos_y);
    }
    osd_txn_write_uint(state.lipo_cell_count, 1);
    osd_txn_write_char('S');

    osd_txn_start(osd_attr(el), el->pos_x + 3, el->pos_y);
    osd_txn_write_float(state.vbattfilt_corr, 4, 1);
    osd_txn_write_char('V');

    osd_txn_submit(txn);
    osd_state.element++;
    break;
  }

  case OSD_GYRO_TEMP: {
    osd_transaction_t *txn = osd_txn_init();
    osd_txn_start(osd_attr(el), el->pos_x, el->pos_y);
    osd_txn_write_uint(state.gyro_temp, 4);
    osd_txn_write_char(ICON_CELSIUS);
    osd_txn_submit(txn);
    osd_state.element++;
    break;
  }

  case OSD_FLIGHT_MODE: {
    print_osd_flightmode(el);
    osd_state.element++;
    break;
  }

  case OSD_RSSI: {
    print_osd_rssi(el);
    osd_state.element++;
    break;
  }

  case OSD_STOPWATCH: {
    print_osd_armtime(el);
    osd_state.element++;
    break;
  }

  case OSD_SYSTEM_STATUS: {
    if (print_osd_system_status(el))
      osd_state.element++;
    break;
  }

  case OSD_THROTTLE: {
    osd_transaction_t *txn = osd_txn_init();
    osd_txn_start(osd_attr(el), el->pos_x, el->pos_y);
    osd_txn_write_uint(state.throttle * 100.0f, 4);
    osd_txn_write_char(ICON_THROTTLE);
    osd_txn_submit(txn);
    osd_state.element++;
    break;
  }

  case OSD_VTX_CHANNEL: {
    print_osd_vtx(el);
    osd_state.element++;
    break;
  }

  case OSD_CURRENT_DRAW: {
    osd_transaction_t *txn = osd_txn_init();
    osd_txn_start(osd_attr(el), el->pos_x, el->pos_y);
    osd_txn_write_float(state.ibat_filtered / 1000.0f, 4, 2);
    osd_txn_write_char(ICON_AMP);
    osd_txn_submit(txn);
    osd_state.element++;
    break;
  }

  case OSD_ELEMENT_MAX: {
    // end of regular display - display_trigger counter sticks here till it wraps
    static uint8_t display_trigger = 0;
    display_trigger++;
    if (display_trigger == 0)
      osd_state.element = OSD_CALLSIGN;
    break;
  }
  }
}

void osd_display_rate_menu() {
  osd_menu_start();

  switch (profile_current_rates()->mode) {
  case RATE_MODE_SILVERWARE:
    osd_menu_header("SILVERWARE RATES");
    break;
  case RATE_MODE_BETAFLIGHT:
    osd_menu_header("BETAFLIGHT RATES");
    break;
  case RATE_MODE_ACTUAL:
    osd_menu_header("ACTUAL RATES");
    break;
  }

  const char *mode_labels[] = {
      "SILVERWARE",
      "BETAFLIGHT",
      "ACTUAL",
  };
  osd_menu_select(2, 3, "MODE");
  if (osd_menu_select_enum(14, 3, profile_current_rates()->mode, mode_labels)) {
    profile_current_rates()->mode = osd_menu_adjust_int(profile_current_rates()->mode, 1, 0, RATE_MODE_ACTUAL);
  }

  osd_menu_label(14, 5, "ROLL");
  osd_menu_label(19, 5, "PITCH");
  osd_menu_label(25, 5, "YAW");

  vec3_t *rate = profile_current_rates()->rate;
  switch (profile_current_rates()->mode) {
  case RATE_MODE_SILVERWARE: {
    osd_menu_select(2, 6, "RATE");
    if (osd_menu_select_vec3(13, 6, rate[SILVERWARE_MAX_RATE], 5, 0)) {
      rate[SILVERWARE_MAX_RATE] = osd_menu_adjust_vec3(rate[SILVERWARE_MAX_RATE], 10, 0.0, 1800.0);
    }

    osd_menu_select(2, 7, "ACRO EXPO");
    if (osd_menu_select_vec3(13, 7, rate[SILVERWARE_ACRO_EXPO], 5, 2)) {
      rate[SILVERWARE_ACRO_EXPO] = osd_menu_adjust_vec3(rate[SILVERWARE_ACRO_EXPO], 0.01, 0.0, 0.99);
    }

    osd_menu_select(2, 8, "ANGLE EXPO");
    if (osd_menu_select_vec3(13, 8, rate[SILVERWARE_ANGLE_EXPO], 5, 2)) {
      rate[SILVERWARE_ANGLE_EXPO] = osd_menu_adjust_vec3(rate[SILVERWARE_ANGLE_EXPO], 0.01, 0.0, 0.99);
    }
    break;
  }
  case RATE_MODE_BETAFLIGHT: {
    osd_menu_select(2, 6, "RC RATE");
    if (osd_menu_select_vec3(13, 6, rate[BETAFLIGHT_RC_RATE], 5, 2)) {
      rate[BETAFLIGHT_RC_RATE] = osd_menu_adjust_vec3(rate[BETAFLIGHT_RC_RATE], 0.01, 0.0, 3.0);
    }

    osd_menu_select(2, 7, "SUPER RATE");
    if (osd_menu_select_vec3(13, 7, rate[BETAFLIGHT_SUPER_RATE], 5, 2)) {
      rate[BETAFLIGHT_SUPER_RATE] = osd_menu_adjust_vec3(rate[BETAFLIGHT_SUPER_RATE], 0.01, 0.0, 3.0);
    }

    osd_menu_select(2, 8, "EXPO");
    if (osd_menu_select_vec3(13, 8, rate[BETAFLIGHT_EXPO], 5, 2)) {
      rate[BETAFLIGHT_EXPO] = osd_menu_adjust_vec3(rate[BETAFLIGHT_EXPO], 0.01, 0.0, 0.99);
    }
    break;
  }
  case RATE_MODE_ACTUAL: {
    osd_menu_select(2, 6, "CENTER");
    if (osd_menu_select_vec3(13, 6, rate[ACTUAL_CENTER_SENSITIVITY], 5, 2)) {
      rate[ACTUAL_CENTER_SENSITIVITY] = osd_menu_adjust_vec3(rate[ACTUAL_CENTER_SENSITIVITY], 1.0, 0.0, 500);
    }

    osd_menu_select(2, 7, "MAX RATE");
    if (osd_menu_select_vec3(13, 7, rate[ACTUAL_MAX_RATE], 5, 2)) {
      rate[ACTUAL_MAX_RATE] = osd_menu_adjust_vec3(rate[ACTUAL_MAX_RATE], 10, 0.0, 1800);
    }

    osd_menu_select(2, 8, "EXPO");
    if (osd_menu_select_vec3(13, 8, rate[ACTUAL_EXPO], 5, 2)) {
      rate[ACTUAL_EXPO] = osd_menu_adjust_vec3(rate[ACTUAL_EXPO], 0.01, 0.0, 0.99);
    }
    break;
  }
  }

  osd_menu_select_save_and_exit(2, 14);
  osd_menu_finish();
}

void osd_display() {
  if (!osd_is_ready()) {
    return;
  }

  // check if the system changed
  const osd_system_t sys = osd_check_system();
  if (sys != osd_system) {
    // sytem has changed, reset osd state
    osd_system = sys;
    osd_display_reset();
    return;
  }
  switch (osd_state.screen) {
  case OSD_SCREEN_CLEAR:
    if (osd_clear_async())
      osd_display_reset();
    break;

  case OSD_SCREEN_REGULAR:
    osd_display_regular();
    break;

    //**********************************************************************************************************************************************************************************************
    //																				OSD MENUS BELOW THIS POINT
    //**********************************************************************************************************************************************************************************************

  case OSD_SCREEN_MAIN_MENU: {
    osd_menu_start();
    osd_menu_header("MENU");

    osd_menu_select_screen(7, 3, "VTX", OSD_SCREEN_VTX);
    osd_menu_select_screen(7, 4, "PIDS", OSD_SCREEN_PID_PROFILE);
    osd_menu_select_screen(7, 5, "FILTERS", OSD_SCREEN_FILTERS);
    osd_menu_select_screen(7, 6, "RATES", OSD_SCREEN_RATE_PROFILES);
    osd_menu_select_screen(7, 7, "FLIGHT MODES", OSD_SCREEN_FLIGHT_MODES);
    osd_menu_select_screen(7, 8, "OSD ELEMENTS", OSD_SCREEN_ELEMENTS);
    osd_menu_select_screen(7, 9, "SPECIAL FEATURES", OSD_SCREEN_SPECIAL_FEATURES);
    osd_menu_select_screen(7, 10, "RC LINK", OSD_SCREEN_RC_LINK);

    osd_menu_select_save_and_exit(7, 11);
    osd_menu_finish();
    break;
  }

  case OSD_SCREEN_PID_PROFILE:
    osd_menu_start();
    osd_menu_header("PID PROFILES");

    if (osd_menu_button(7, 4, "PID PROFILE 1")) {
      profile.pid.pid_profile = PID_PROFILE_1;
      osd_push_cursor();
      osd_push_screen(OSD_SCREEN_PID);
    }

    if (osd_menu_button(7, 5, "PID PROFILE 2")) {
      profile.pid.pid_profile = PID_PROFILE_2;
      osd_push_cursor();
      osd_push_screen(OSD_SCREEN_PID);
    }

    osd_menu_finish();
    break;

  case OSD_SCREEN_PID:
    osd_menu_start();

    if (profile.pid.pid_profile == PID_PROFILE_1)
      osd_menu_header("PID PROFILE 1");
    else
      osd_menu_header("PID PROFILE 2");

    osd_menu_label(10, 4, "ROLL");
    osd_menu_label(16, 4, "PITCH");
    osd_menu_label(23, 4, "YAW");

    pid_rate_t *rates = profile_current_pid_rates();

    osd_menu_select(4, 6, "KP");
    if (osd_menu_select_vec3(8, 6, rates->kp, 6, 0)) {
      rates->kp = osd_menu_adjust_vec3(rates->kp, 1, 0.0, 400.0);
    }

    osd_menu_select(4, 7, "KI");
    if (osd_menu_select_vec3(8, 7, rates->ki, 6, 0)) {
      rates->ki = osd_menu_adjust_vec3(rates->ki, 1, 0.0, 100.0);
    }

    osd_menu_select(4, 8, "KD");
    if (osd_menu_select_vec3(8, 8, rates->kd, 6, 0)) {
      rates->kd = osd_menu_adjust_vec3(rates->kd, 1, 0.0, 120.0);
    }

    osd_menu_select_save_and_exit(7, 11);
    osd_menu_finish();
    break;

  case OSD_SCREEN_RATE_PROFILES:
    osd_menu_start();
    osd_menu_header("RATE PROFILES");

    if (osd_menu_button(7, 4, "PROFILE 1")) {
      profile.rate.profile = STICK_RATE_PROFILE_1;
      osd_push_cursor();
      osd_push_screen(OSD_SCREEN_RATES);
    }

    if (osd_menu_button(7, 5, "PROFILE 2")) {
      profile.rate.profile = STICK_RATE_PROFILE_2;
      osd_push_cursor();
      osd_push_screen(OSD_SCREEN_RATES);
    }

    osd_menu_finish();
    break;

  case OSD_SCREEN_RATES:
    osd_display_rate_menu();
    break;

  case OSD_SCREEN_FILTERS:
    osd_menu_start();
    osd_menu_header("FILTERS");

    osd_menu_select_screen(7, 4, "GYRO", OSD_SCREEN_GYRO_FILTER);
    osd_menu_select_screen(7, 5, "D-TERM", OSD_SCREEN_DTERM_FILTER);

    osd_menu_finish();
    break;

  case OSD_SCREEN_GYRO_FILTER: {
    osd_menu_start();
    osd_menu_header("GYRO FILTERS");

    const char *filter_type_labels[] = {
        "NONE",
        " PT1",
        " PT2",
    };

    osd_menu_select(4, 4, "PASS 1 TYPE");
    if (osd_menu_select_enum(18, 4, profile.filter.gyro[0].type, filter_type_labels)) {
      profile.filter.gyro[0].type = osd_menu_adjust_int(profile.filter.gyro[0].type, 1, 0, FILTER_LP2_PT1);
      osd_state.reboot_fc_requested = 1;
    }

    osd_menu_select(4, 5, "PASS 1 FREQ");
    if (osd_menu_select_float(18, 5, profile.filter.gyro[0].cutoff_freq, 4, 0)) {
      profile.filter.gyro[0].cutoff_freq = osd_menu_adjust_float(profile.filter.gyro[0].cutoff_freq, 10, 50, 500);
    }

    osd_menu_select(4, 6, "PASS 2 TYPE");
    if (osd_menu_select_enum(18, 6, profile.filter.gyro[1].type, filter_type_labels)) {
      profile.filter.gyro[1].type = osd_menu_adjust_int(profile.filter.gyro[1].type, 1, 0, FILTER_LP2_PT1);
      osd_state.reboot_fc_requested = 1;
    }

    osd_menu_select(4, 7, "PASS 2 FREQ");
    if (osd_menu_select_float(18, 7, profile.filter.gyro[1].cutoff_freq, 4, 0)) {
      profile.filter.gyro[1].cutoff_freq = osd_menu_adjust_float(profile.filter.gyro[1].cutoff_freq, 10, 50, 500);
    }

    osd_menu_select_save_and_exit(4, 14);
    osd_menu_finish();
    break;
  }

  case OSD_SCREEN_DTERM_FILTER:
    osd_menu_start();
    osd_menu_header("D-TERM FILTERS");

    const char *filter_type_labels[] = {
        "NONE",
        " PT1",
        " PT2",
    };

    osd_menu_select(4, 3, "PASS 1 TYPE");
    if (osd_menu_select_enum(18, 3, profile.filter.dterm[0].type, filter_type_labels)) {
      profile.filter.dterm[0].type = osd_menu_adjust_int(profile.filter.dterm[0].type, 1, 0, FILTER_LP2_PT1);
      osd_state.reboot_fc_requested = 1;
    }

    osd_menu_select(4, 4, "PASS 1 FREQ");
    if (osd_menu_select_float(18, 4, profile.filter.dterm[0].cutoff_freq, 4, 0)) {
      profile.filter.dterm[0].cutoff_freq = osd_menu_adjust_float(profile.filter.dterm[0].cutoff_freq, 10, 50, 500);
    }

    osd_menu_select(4, 5, "PASS 2 TYPE");
    if (osd_menu_select_enum(18, 5, profile.filter.dterm[1].type, filter_type_labels)) {
      profile.filter.dterm[1].type = osd_menu_adjust_int(profile.filter.dterm[1].type, 1, 0, FILTER_LP2_PT1);
      osd_state.reboot_fc_requested = 1;
    }

    osd_menu_select(4, 6, "PASS 2 FREQ");
    if (osd_menu_select_float(18, 6, profile.filter.dterm[1].cutoff_freq, 4, 0)) {
      profile.filter.dterm[1].cutoff_freq = osd_menu_adjust_float(profile.filter.dterm[1].cutoff_freq, 10, 50, 500);
    }

    osd_menu_select(4, 7, "DYNAMIC");
    if (osd_menu_select_enum(18, 7, profile.filter.dterm_dynamic_enable, filter_type_labels)) {
      profile.filter.dterm_dynamic_enable = osd_menu_adjust_int(profile.filter.dterm_dynamic_enable, 1, 0, FILTER_LP_PT1);
      osd_state.reboot_fc_requested = 1;
    }

    osd_menu_select(4, 8, "FREQ MIN");
    if (osd_menu_select_float(18, 8, profile.filter.dterm_dynamic_min, 4, 0)) {
      profile.filter.dterm_dynamic_min = osd_menu_adjust_float(profile.filter.dterm_dynamic_min, 10, 50, 500);
    }

    osd_menu_select(4, 9, "FREQ MAX");
    if (osd_menu_select_float(18, 9, profile.filter.dterm_dynamic_max, 4, 0)) {
      profile.filter.dterm_dynamic_max = osd_menu_adjust_float(profile.filter.dterm_dynamic_max, 10, 50, 500);
    }

    osd_menu_select_save_and_exit(4, 14);
    osd_menu_finish();
    break;

  case OSD_SCREEN_FLIGHT_MODES: {
    osd_menu_start();
    osd_menu_header("FLIGHT MODES");

    const char *channel_labels[] = {
        "CHANNEL 5  ",
        "CHANNEL 6  ",
        "CHANNEL 7  ",
        "CHANNEL 8  ",
        "CHANNEL 9  ",
        "CHANNEL 10 ",
        "CHANNEL 11 ",
        "CHANNEL 12 ",
        "CHANNEL 13 ",
        "CHANNEL 14 ",
        "CHANNEL 15 ",
        "CHANNEL 16 ",
        "ALWAYS OFF ",
        "ALWAYS ON  ",
        "GESTURE AUX",
    };

    osd_menu_select(4, 2, "ARMING");
    if (osd_menu_select_enum(17, 2, profile.receiver.aux[AUX_ARMING], channel_labels)) {
      profile.receiver.aux[AUX_ARMING] = osd_menu_adjust_int(profile.receiver.aux[AUX_ARMING], 1, AUX_CHANNEL_0, AUX_CHANNEL_11);
    }

    osd_menu_select(4, 3, "IDLE UP");
    if (osd_menu_select_enum(17, 3, profile.receiver.aux[AUX_IDLE_UP], channel_labels)) {
      profile.receiver.aux[AUX_IDLE_UP] = osd_menu_adjust_int(profile.receiver.aux[AUX_IDLE_UP], 1, AUX_CHANNEL_0, AUX_CHANNEL_GESTURE);
    }

    osd_menu_select(4, 4, "LEVELMODE");
    if (osd_menu_select_enum(17, 4, profile.receiver.aux[AUX_LEVELMODE], channel_labels)) {
      profile.receiver.aux[AUX_LEVELMODE] = osd_menu_adjust_int(profile.receiver.aux[AUX_LEVELMODE], 1, AUX_CHANNEL_0, AUX_CHANNEL_GESTURE);
    }

    osd_menu_select(4, 5, "RACEMODE");
    if (osd_menu_select_enum(17, 5, profile.receiver.aux[AUX_RACEMODE], channel_labels)) {
      profile.receiver.aux[AUX_RACEMODE] = osd_menu_adjust_int(profile.receiver.aux[AUX_RACEMODE], 1, AUX_CHANNEL_0, AUX_CHANNEL_GESTURE);
    }

    osd_menu_select(4, 6, "HORIZON");
    if (osd_menu_select_enum(17, 6, profile.receiver.aux[AUX_HORIZON], channel_labels)) {
      profile.receiver.aux[AUX_HORIZON] = osd_menu_adjust_int(profile.receiver.aux[AUX_HORIZON], 1, AUX_CHANNEL_0, AUX_CHANNEL_GESTURE);
    }

    osd_menu_select(4, 7, "STICK BOOST");
    if (osd_menu_select_enum(17, 7, profile.receiver.aux[AUX_STICK_BOOST_PROFILE], channel_labels)) {
      profile.receiver.aux[AUX_STICK_BOOST_PROFILE] = osd_menu_adjust_int(profile.receiver.aux[AUX_STICK_BOOST_PROFILE], 1, AUX_CHANNEL_0, AUX_CHANNEL_GESTURE);
    }

    osd_menu_select(4, 8, "BUZZER");
    if (osd_menu_select_enum(17, 8, profile.receiver.aux[AUX_BUZZER_ENABLE], channel_labels)) {
      profile.receiver.aux[AUX_BUZZER_ENABLE] = osd_menu_adjust_int(profile.receiver.aux[AUX_BUZZER_ENABLE], 1, AUX_CHANNEL_0, AUX_CHANNEL_GESTURE);
    }

    osd_menu_select(4, 9, "TURTLE");
    if (osd_menu_select_enum(17, 9, profile.receiver.aux[AUX_TURTLE], channel_labels)) {
      profile.receiver.aux[AUX_TURTLE] = osd_menu_adjust_int(profile.receiver.aux[AUX_TURTLE], 1, AUX_CHANNEL_0, AUX_CHANNEL_GESTURE);
    }

    osd_menu_select(4, 10, "MOTOR TEST");
    if (osd_menu_select_enum(17, 10, profile.receiver.aux[AUX_MOTOR_TEST], channel_labels)) {
      profile.receiver.aux[AUX_MOTOR_TEST] = osd_menu_adjust_int(profile.receiver.aux[AUX_MOTOR_TEST], 1, AUX_CHANNEL_0, AUX_CHANNEL_GESTURE);
    }

    osd_menu_select(4, 11, "FPV SWITCH");
    if (osd_menu_select_enum(17, 11, profile.receiver.aux[AUX_FPV_SWITCH], channel_labels)) {
      profile.receiver.aux[AUX_FPV_SWITCH] = osd_menu_adjust_int(profile.receiver.aux[AUX_FPV_SWITCH], 1, AUX_CHANNEL_0, AUX_CHANNEL_GESTURE);
    }

    osd_menu_select_save_and_exit(4, 14);
    osd_menu_finish();
    break;
  }

  case OSD_SCREEN_VTX:
    if (vtx_settings.detected) {
      populate_vtx_buffer_once();

      osd_menu_start();
      osd_menu_header("VTX CONTROLS");

      const char *band_labels[] = {"A", "B", "E", "F", "R"};
      osd_menu_select(4, 4, "BAND");
      if (osd_menu_select_enum(20, 4, vtx_settings_copy.band, band_labels)) {
        vtx_settings_copy.band = osd_menu_adjust_int(vtx_settings_copy.band, 1, VTX_BAND_A, VTX_BAND_R);
      }

      const char *channel_labels[] = {"1", "2", "3", "4", "5", "6", "7", "8"};
      osd_menu_select(4, 5, "CHANNEL");
      if (osd_menu_select_enum(20, 5, vtx_settings_copy.channel, channel_labels)) {
        vtx_settings_copy.channel = osd_menu_adjust_int(vtx_settings_copy.channel, 1, VTX_CHANNEL_1, VTX_CHANNEL_8);
      }

      const char *power_level_labels[] = {"1", "2", "3", "4"};
      osd_menu_select(4, 6, "POWER LEVEL");
      if (osd_menu_select_enum(20, 6, vtx_settings_copy.power_level, power_level_labels)) {
        vtx_settings_copy.power_level = osd_menu_adjust_int(vtx_settings_copy.power_level, 1, VTX_POWER_LEVEL_1, VTX_POWER_LEVEL_4);
      }

      const char *pit_mode_labels[] = {"OFF", "ON ", "N/A"};
      osd_menu_select(4, 7, "PITMODE");
      if (osd_menu_select_enum(20, 7, vtx_settings_copy.pit_mode, pit_mode_labels) && vtx_settings_copy.pit_mode != VTX_PIT_MODE_NO_SUPPORT) {
        vtx_settings_copy.pit_mode = osd_menu_adjust_int(vtx_settings_copy.pit_mode, 1, VTX_PIT_MODE_OFF, VTX_PIT_MODE_ON);
      }

      osd_menu_select_save_and_exit(4, 14);
      osd_menu_finish();
    } else {
      osd_menu_start();
      osd_menu_header("VTX CONTROLS");

      osd_menu_label(7, 4, "SMART AUDIO");
      osd_menu_label(7, 5, "NOT CONFIGURED");

      osd_menu_finish();

      if (osd_state.selection) {
        osd_state.selection = 0;
      }
    }
    break;

  case OSD_SCREEN_SPECIAL_FEATURES:
    osd_menu_start();
    osd_menu_header("SPECIAL FEATURES");

    osd_menu_select_screen(7, 3, "STICK BOOST", OSD_SCREEN_STICK_BOOST);
    osd_menu_select_screen(7, 4, "MOTOR BOOST", OSD_SCREEN_MOTOR_BOOST);
    osd_menu_select_screen(7, 5, "PID MODIFIERS", OSD_SCREEN_PID_MODIFIER);
    osd_menu_select_screen(7, 6, "DIGITAL IDLE", OSD_SCREEN_DIGITAL_IDLE);
    osd_menu_select_screen(7, 7, "LOW BATTERY", OSD_SCREEN_LOWBAT);
    osd_menu_select_screen(7, 8, "LEVEL MODE", OSD_SCREEN_LEVEL_MODE);
    osd_menu_select_screen(7, 9, "TURTLE THROTTLE", OSD_SCREEN_TURTLE_THROTTLE);

    osd_menu_finish();
    break;

  case OSD_SCREEN_STICK_BOOST:
    osd_menu_start();
    osd_menu_header("STICK BOOST PROFILES");

    if (osd_menu_button(7, 4, "AUX OFF PROFILE 1")) {
      profile.pid.stick_profile = STICK_PROFILE_OFF;
      osd_push_cursor();
      osd_push_screen(OSD_SCREEN_STICK_BOOST_ADJUST);
    }

    if (osd_menu_button(7, 5, "AUX ON  PROFILE 2")) {
      profile.pid.stick_profile = STICK_PROFILE_ON;
      osd_push_cursor();
      osd_push_screen(OSD_SCREEN_STICK_BOOST_ADJUST);
    }

    osd_menu_finish();
    break;

  case OSD_SCREEN_STICK_BOOST_ADJUST: {
    osd_menu_start();

    if (profile.pid.stick_profile == STICK_PROFILE_OFF)
      osd_menu_header("BOOST PROFILE 1");
    else
      osd_menu_header("BOOST PROFILE 2");

    osd_menu_label(14, 4, "ROLL");
    osd_menu_label(19, 4, "PITCH");
    osd_menu_label(25, 4, "YAW");

    stick_rate_t *rates = &profile.pid.stick_rates[profile.pid.stick_profile];

    osd_menu_select(2, 6, "ACCELERATOR");
    if (osd_menu_select_vec3(13, 6, rates->accelerator, 5, 2)) {
      rates->accelerator = osd_menu_adjust_vec3(rates->accelerator, 0.01, 0.0, 3.0);
    }

    osd_menu_select(2, 7, "TRANSITION");
    if (osd_menu_select_vec3(13, 7, rates->transition, 5, 2)) {
      rates->transition = osd_menu_adjust_vec3(rates->transition, 0.01, -1.0, -1.0);
    }

    osd_menu_select_save_and_exit(7, 11);
    osd_menu_finish();
    break;
  }

  case OSD_SCREEN_ELEMENTS:
    osd_menu_start();
    osd_menu_header("OSD ELEMENTS");

    osd_menu_select_screen(7, 4, "ADD OR REMOVE", OSD_SCREEN_ELEMENTS_ADD_REMOVE);
    osd_menu_select_screen(7, 5, "EDIT POSITIONS", OSD_SCREEN_ELEMENTS_POSITION);
    osd_menu_select_screen(7, 6, "EDIT TEXT STYLE", OSD_SCREEN_ELEMENTS_STYLE);
    osd_menu_select_screen(7, 7, "EDIT CALLSIGN", OSD_SCREEN_CALLSIGN);

    osd_menu_finish();
    break;

  case OSD_SCREEN_ELEMENTS_ADD_REMOVE: {
    osd_menu_start();
    osd_menu_header("OSD DISPLAY ITEMS");

    const char *active_labels[] = {
        "INACTIVE",
        "ACTIVE  ",
    };

    for (uint8_t i = 0; i < OSD_VTX_CHANNEL; i++) {
      osd_element_t *el = (osd_element_t *)(osd_elements() + i);

      osd_menu_select(4, 2 + i, osd_element_labels[i]);
      if (osd_menu_select_enum(20, 2 + i, el->active, active_labels)) {
        el->active = osd_menu_adjust_int(el->active, 1, 0, 1);
      }
    }

    osd_menu_select_save_and_exit(4, 14);
    osd_menu_finish();
    break;
  }

  case OSD_SCREEN_ELEMENTS_POSITION:
    osd_menu_start();

    osd_menu_highlight(1, 1, "OSD POSITIONS");
    osd_menu_label(18, 1, "ADJ X");
    osd_menu_label(24, 1, "ADJ Y");

    for (uint8_t i = 0; i < OSD_VTX_CHANNEL; i++) {
      osd_element_t *el = (osd_element_t *)(osd_elements() + i);

      osd_menu_select(3, 2 + i, osd_element_labels[i]);
      if (osd_menu_select_int(20, 2 + i, el->pos_x, 2)) {
        el->pos_x = osd_menu_adjust_int(el->pos_x, 1, 0, 30);
      }
      if (osd_menu_select_int(26, 2 + i, el->pos_y, 2)) {
        el->pos_y = osd_menu_adjust_int(el->pos_y, 1, 0, 15);
      }
    }

    osd_menu_select_save_and_exit(4, 14);
    osd_menu_finish();
    break;

  case OSD_SCREEN_ELEMENTS_STYLE:
    osd_menu_start();
    osd_menu_header("OSD TEXT STYLE");

    const char *attr_labels[] = {
        "NORMAL",
        "INVERT",
    };

    for (uint8_t i = 0; i < OSD_VTX_CHANNEL; i++) {
      osd_element_t *el = (osd_element_t *)(osd_elements() + i);

      osd_menu_select(4, 2 + i, osd_element_labels[i]);
      if (osd_menu_select_enum(20, 2 + i, el->attribute, attr_labels)) {
        el->attribute = osd_menu_adjust_int(el->attribute, 1, 0, 1);
      }
    }

    osd_menu_select_save_and_exit(4, 14);
    osd_menu_finish();
    break;

  case OSD_SCREEN_CALLSIGN:
    osd_menu_start();
    osd_menu_header("CALLSIGN");

    osd_menu_select(1, 5, "EDIT:");
    if (osd_menu_select_str(8, 5, (char *)profile.osd.callsign)) {
      osd_menu_adjust_str((char *)profile.osd.callsign);
    }
    osd_menu_label(8, 6, "-------------------");

    osd_menu_select_save_and_exit(4, 14);
    osd_menu_finish();
    break;

  case OSD_SCREEN_LOWBAT:
    print_osd_menu_strings(lowbatt_labels, lowbatt_labels_size);
    print_osd_adjustable_float(3, 1, low_batt_ptr, lowbatt_grid, lowbatt_data_positions, 1);
    if (osd_state.screen_phase == 5)
      osd_float_adjust(low_batt_ptr, 1, 1, lowbatt_adjust_limits, 0.1);
    break;

  case OSD_SCREEN_LEVEL_MODE:
    osd_menu_start();
    osd_menu_header("LEVEL MODE");

    osd_menu_select_screen(7, 4, "MAX ANGLE", OSD_SCREEN_LEVEL_MAX_ANGLE);
    osd_menu_select_screen(7, 5, "LEVEL STRENGTH", OSD_SCREEN_LEVEL_STRENGTH);

    osd_menu_finish();
    break;

  case OSD_SCREEN_MOTOR_BOOST:
    osd_menu_start();
    osd_menu_header("MOTOR BOOST TYPES");

    osd_menu_select_screen(7, 4, "TORQUE BOOST", OSD_SCREEN_TORQUE_BOOST);
    osd_menu_select_screen(7, 5, "THROTTLE BOOST", OSD_SCREEN_THROTTLE_BOOST);

    osd_menu_finish();
    break;

  case OSD_SCREEN_DIGITAL_IDLE:
    print_osd_menu_strings(motoridle_labels, motoridle_labels_size);
    print_osd_adjustable_float(3, 1, motoridle_ptr, motoridle_grid, motoridle_data_positions, 1);
    if (osd_state.screen_phase == 5)
      osd_float_adjust(motoridle_ptr, 1, 1, motoridle_adjust_limits, 0.1);
    break;

  case OSD_SCREEN_LEVEL_MAX_ANGLE:
    print_osd_menu_strings(maxangle_labels, maxangle_labels_size);
    print_osd_adjustable_float(3, 1, level_maxangle_ptr, maxangle_grid, maxangle_data_positions, 0);
    if (osd_state.screen_phase == 5)
      osd_float_adjust(level_maxangle_ptr, 1, 1, maxangle_adjust_limits, 1.0);
    break;

  case OSD_SCREEN_LEVEL_STRENGTH:
    print_osd_menu_strings(levelmode_labels, levelmode_labels_size);
    print_osd_adjustable_float(6, 4, level_pid_ptr, levelmode_grid, levelmode_data_positions, 1);
    if (osd_state.screen_phase == 11)
      osd_float_adjust(level_pid_ptr, 2, 2, levelmode_adjust_limits, 0.5);
    break;

  case OSD_SCREEN_TORQUE_BOOST:
    print_osd_menu_strings(torqueboost_labels, torqueboost_labels_size);
    print_osd_adjustable_float(3, 1, torqueboost_ptr, torqueboost_grid, torqueboost_data_positions, 1);
    if (osd_state.screen_phase == 5)
      osd_float_adjust(torqueboost_ptr, 1, 1, torqueboost_adjust_limits, 0.1);
    break;

  case OSD_SCREEN_THROTTLE_BOOST:
    print_osd_menu_strings(throttleboost_labels, throttleboost_labels_size);
    print_osd_adjustable_float(3, 1, throttleboost_ptr, throttleboost_grid, throttleboost_data_positions, 1);
    if (osd_state.screen_phase == 5)
      osd_float_adjust(throttleboost_ptr, 1, 1, throttleboost_adjust_limits, 0.5);
    break;

  case OSD_SCREEN_TURTLE_THROTTLE:
    print_osd_menu_strings(turtlethrottle_labels, turtlethrottle_labels_size);
    print_osd_adjustable_float(3, 1, turtlethrottle_ptr, turtlethrottle_grid, turtlethrottle_data_positions, 0);
    if (osd_state.screen_phase == 5)
      osd_float_adjust(turtlethrottle_ptr, 1, 1, turtlethrottle_adjust_limits, 10.0);
    break;

  case OSD_SCREEN_PID_MODIFIER:
    osd_menu_start();
    osd_menu_header("PID MODIFIERS");

    const char *modifier_state_labels[] = {
        " NONE ",
        "ACTIVE",
    };

    osd_menu_select(2, 4, "P-TERM VOLTAGE COMP");
    if (osd_menu_select_enum(23, 4, profile.voltage.pid_voltage_compensation, modifier_state_labels)) {
      profile.voltage.pid_voltage_compensation = osd_menu_adjust_int(profile.voltage.pid_voltage_compensation, 1, 0, 1);
    }

    osd_menu_select(2, 5, "THROTTLE D ATTENUATE");
    if (osd_menu_select_enum(23, 5, profile.pid.throttle_dterm_attenuation.tda_active, modifier_state_labels)) {
      profile.pid.throttle_dterm_attenuation.tda_active = osd_menu_adjust_int(profile.pid.throttle_dterm_attenuation.tda_active, 1, 0, 1);
    }

    osd_menu_select(2, 6, "TDA BREAKPOINT");
    if (osd_menu_select_float(23, 6, profile.pid.throttle_dterm_attenuation.tda_breakpoint, 4, 1)) {
      profile.pid.throttle_dterm_attenuation.tda_breakpoint = osd_menu_adjust_float(profile.pid.throttle_dterm_attenuation.tda_breakpoint, 0.05, 0, 1);
    }

    osd_menu_select(2, 7, "TDA PERCENT");
    if (osd_menu_select_float(23, 7, profile.pid.throttle_dterm_attenuation.tda_percent, 4, 1)) {
      profile.pid.throttle_dterm_attenuation.tda_percent = osd_menu_adjust_float(profile.pid.throttle_dterm_attenuation.tda_percent, 0.05, 0, 1);
    }

    osd_menu_select_save_and_exit(2, 14);
    osd_menu_finish();
    break;

  case OSD_SCREEN_RC_LINK:
    osd_menu_start();
    osd_menu_header("RC LINK");

    osd_menu_select_screen(7, 4, "STICK CALIBRATION", OSD_SCREEN_STICK_WIZARD);
    osd_menu_select_screen(7, 5, "RSSI SOURCE", OSD_SCREEN_RSSI);

    osd_menu_finish();
    break;

  case OSD_SCREEN_RSSI:
    print_osd_menu_strings(rssi_menu_labels, rssi_menu_labels_size);
    print_osd_adjustable_enums(4, 2, get_rssi_source_status(osd_state.screen_phase - 5), rssi_source_data_grid, rssi_source_data_positions);
    if (osd_state.screen_phase == 7)
      osd_enum_adjust(rssi_source_ptr, 2, rssi_source_limits);
    break;

  case OSD_SCREEN_STICK_WIZARD:
    osd_menu_start();
    osd_menu_header("STICK CALIBRATION");

    osd_menu_label(9, 5, "LEFT TO EXIT");
    osd_menu_label(9, 7, "RIGHT TO BEGIN");

    if (osd_menu_finish() && osd_state.selection) {
      request_stick_calibration_wizard();
      osd_push_screen(OSD_SCREEN_STICK_WIZARD_CALIBRATION);
      osd_state.selection = 0;
    }
    break;

  case OSD_SCREEN_STICK_WIZARD_CALIBRATION:
    osd_menu_start();
    osd_menu_header("RECORDING");

    osd_menu_label(9, 5, "MOVE STICKS");
    osd_menu_label(9, 7, "TO EXTENTS");

    if (osd_menu_finish() && state.stick_calibration_wizard == WAIT_FOR_CONFIRM) {
      osd_push_screen(OSD_SCREEN_STICK_CONFIRM);
    }
    break;

  case OSD_SCREEN_STICK_CONFIRM: // 5 sec to test / confirm calibration
    osd_menu_start();
    osd_menu_header("TESTING CALIBRATION");

    osd_menu_label(6, 5, "MOVE STICKS AGAIN");
    osd_menu_label(9, 7, "TO EXTENTS");

    if (osd_menu_finish() && ((state.stick_calibration_wizard == CALIBRATION_SUCCESS) || (state.stick_calibration_wizard == TIMEOUT))) {
      osd_push_screen(OSD_SCREEN_STICK_RESULT);
    }
    break;

  case OSD_SCREEN_STICK_RESULT: // results of calibration
    osd_menu_start();
    osd_menu_header("STICK CALIBRATION");

    if (state.stick_calibration_wizard == CALIBRATION_SUCCESS) {
      osd_menu_label(10, 4, "CALIBRATION");
      osd_menu_label(12, 6, "SUCCESS");
      osd_menu_label(7, 8, "PUSH LEFT TO EXIT");
    }
    if (state.stick_calibration_wizard == TIMEOUT) {
      osd_menu_label(10, 4, "CALIBRATION");
      osd_menu_label(12, 6, "FAILED");
      osd_menu_label(7, 8, "PUSH LEFT TO EXIT");
    }

    osd_menu_finish();

    if (osd_state.selection > 0) {
      osd_state.selection = 0;
    }
    break;
  }

  if (osd_state.screen != OSD_SCREEN_REGULAR && rx_aux_on(AUX_ARMING)) {
    flags.arm_safety = 1;
  }
}

#endif

#include <avr/sleep.h>
#include <EEPROM.h>

/*
 * A `shuttle-illumination` project implements a time-limited illumination of a Shuttle Atlantis plastic model
 * The hardware consists of:
 *    - 6F22 (a.k.a Krona) 9v battery;
 *    - Arduino Pro Mini 3v3 (with the voltage regulator and the power LED removed);
 *    - A power supply module (9v to 3v3 buck converter and a pair of MOSFETs implementing a `power on by button and power off by a signal from an Arduino` logic)
 *      (see https://circuitjournal.com/arduino-auto-power-off)
 *    - set of LEDs accompanied by the resistors and a transistor to drive the illumination of the Shuttle;
 *    - Power button; used to turn on the lights, set the illumination timer or turn off the lights;
 *        [long press in Idling mode] Turn on the light, set the timer (last timer value used)
 *        [short press in Active mode] Indicate current timer value (in minutes)
 *        [short press once again in Active mode less than 5 sec elapsed since the previous key press] Increase current timer value by one (in minutes)
 *        [long press in Active mode] Turn off the lights
 *    - Five LEDs to indicate the illumination timer value (displayed as a binary code in range from 1 to 32 minutes)
 *    - LightLevel button for adjusting the light level (short press to select next light level; there are five levels available)
*/

// Timers (minutes)
uint32_t initial_timer_value_minutes_ = 0;   // In minutes. The setting tells how many minutes we should work. Stored in EEPROM byte #0
uint32_t timer_start_time_ms_ = 0;           // Initialized with millis() when the timer is reset
                                             // The end time is calculated as `timer_start_time_ms_ + ( initial_timer_value_minutes_ * 60 * 1000 )`

uint32_t last_timer_value_on_display = 0;    // last value displayed by `TimerValueFadeIn`, `TimerValueDisplayChange` or `TimerValueFadeOut` functions

uint32_t last_pwr_btn_short_press_time = 0;  // Last time the power button was short pressed

// Illumination level
// 1 - low
// 5 - high
int lighting_level_ = 0;                     // Stored in EEPROM byte #1

// Current increment sign: +/-
// Tells if we must increment or decrement current value
// This is to perform smooth light level adjustment (like 1-2-3-4-5-4-3-2-1-2-3-4-5-4.. etc)
int lighting_level_increment_sign_ = 1;


// Ports definition
#define POWER_ON_BUTTON                  2
#define PAYLOAD_PWM                      3
#define LIGHT_LEVEL_SELECT_BUTTON        4
#define TIMER_INDICATOR_LED1             5      // LSBit
#define TIMER_INDICATOR_LED2             6
#define TIMER_INDICATOR_LED3             9
#define TIMER_INDICATOR_LED4             10
#define TIMER_INDICATOR_LED5             11     // MSBit
#define KEEP_POWERING                    12

static const int leds_addr_array_[5] = { TIMER_INDICATOR_LED1, TIMER_INDICATOR_LED2, TIMER_INDICATOR_LED3, TIMER_INDICATOR_LED4, TIMER_INDICATOR_LED5 };
const uint32_t timer_display_delay_pwm = 700;

enum PowerButtonPressType {
  LongKeyPress,
  ShortKeyPress,
  NoKeyPress
};

bool is_timer_value_displayed = false;
int timer_value_increment_sign_ = 1;

void _blink_internal_led()
{
  digitalWrite( LED_BUILTIN, HIGH );
  delay( 1 );
  digitalWrite( LED_BUILTIN, LOW );
}

void adjust_lighting_level_direction()
{
  if( lighting_level_ == 5 )  {
    lighting_level_increment_sign_ = -1;             // Switch to decrease mode
  } else if( lighting_level_ == 1 ) {
    lighting_level_increment_sign_ = 1;              // Switch to encrease mode
  }
}

void reset_general_timer()
{
  timer_start_time_ms_ = millis();
}

void setup_ports()
{
  // Hold the PSU powering asap
  pinMode( KEEP_POWERING, OUTPUT );
  digitalWrite( KEEP_POWERING, HIGH );
  
  pinMode( LED_BUILTIN, OUTPUT );

  pinMode( POWER_ON_BUTTON, INPUT_PULLUP );          // Although the external pull-up is already installed
  pinMode( PAYLOAD_PWM, OUTPUT );
  pinMode( LIGHT_LEVEL_SELECT_BUTTON, INPUT_PULLUP );
  pinMode( TIMER_INDICATOR_LED1, OUTPUT );
  pinMode( TIMER_INDICATOR_LED2, OUTPUT );
  pinMode( TIMER_INDICATOR_LED3, OUTPUT );
  pinMode( TIMER_INDICATOR_LED4, OUTPUT );
  pinMode( TIMER_INDICATOR_LED5, OUTPUT );
  
  // No illumination
  digitalWrite( PAYLOAD_PWM, LOW );
  
  // Binary value display LEDs 
  digitalWrite( TIMER_INDICATOR_LED1, LOW );
  digitalWrite( TIMER_INDICATOR_LED2, LOW );
  digitalWrite( TIMER_INDICATOR_LED3, LOW );
  digitalWrite( TIMER_INDICATOR_LED4, LOW );
  digitalWrite( TIMER_INDICATOR_LED5, LOW );

  // Some initial values
  initial_timer_value_minutes_ = EEPROM[ 0 ];
  lighting_level_ = EEPROM[ 1 ];

  if( initial_timer_value_minutes_ < 1 || initial_timer_value_minutes_ > 31 ) {
    // Incorrect value. Perform the very first initialization
    initial_timer_value_minutes_ = 10;
    EEPROM.update( 0, initial_timer_value_minutes_ );
  }

  if( lighting_level_ > 5 ) {
    // Incorrect value. Perform the very first initialization
    lighting_level_ = 3;
    EEPROM.update( 1, lighting_level_ );
  }
  adjust_lighting_level_direction();
  
  // Set initial lighting level
  SetNewLightLevel( 0, lighting_level_ );

  // Display current timer value
  ProcessShortKeyPress();

  // Init the timer
  reset_general_timer();
}

void save_settings_and_power_off()
{
  // Save settings (burn flash only if the values are different)
  if( EEPROM[ 0 ] != initial_timer_value_minutes_ ) {
    EEPROM.update( 0, initial_timer_value_minutes_ );
  }
  if( EEPROM[ 1 ] != lighting_level_ ) {
    EEPROM.update( 1, lighting_level_ );
  }

  // Immediate shut down
  digitalWrite( KEEP_POWERING, LOW );
}

enum PowerButtonPressType GetPowerButtonPress()
{
  bool already_turned_off_the_lights = false;
  // Jitter is evel and we're having deal with it right here
  if( digitalRead( POWER_ON_BUTTON ) == HIGH ) {
    return NoKeyPress;
  }

  // Okay, we have HIGH state. Let's count:
  // - the time it was pressed
  // - the time it was released after press
  uint32_t press_time = millis();
  uint32_t release_time = millis();

  int last_state = HIGH;
  for( ; ; ) {

    int state = digitalRead( POWER_ON_BUTTON );

    if( last_state != state ) {
      if( state == LOW ) {

        // Let's start counting the release timer from the begining
        release_time = millis();
      }
      last_state = state;
    }

    // Timing estimation

    if( ( millis() - release_time ) < 10 ) {
      // This is a very fresh moment since the state has changed. Ignore
      continue;
    }

    // [seems to be a crutch, but we need it here] If we see the button is down for more than 2 seconds,
    // we are guaranteed to execute power off sequence
    // So, let's turn off the lights. This provides a reasonable feedback to the consumer
    if(    ( ( millis() - press_time ) > 2000 )
        && !already_turned_off_the_lights
    ) {
      SetNewLightLevel( lighting_level_, 0 );
      already_turned_off_the_lights = true;
    }

    // The button was confidently released?
    if( state == HIGH ) {
      if( ( millis() - press_time ) > 2000 ) {
        return LongKeyPress;        
      } else {
        return ShortKeyPress;
      }
    }
  }
}

bool IsLightLevelButtonPressed()
{
  // Jitter is still an evil and we're having deal with it right here
  if( digitalRead( LIGHT_LEVEL_SELECT_BUTTON ) == HIGH ) {
    return false;
  }

  // Okay, we have HIGH state. Wait until the button is released
  uint32_t start_time = millis();
  uint32_t release_time = millis();

  int last_state = HIGH;
  for( ; ; ) {

    int state = digitalRead( LIGHT_LEVEL_SELECT_BUTTON );

    if( last_state != state ) {
      if( state == LOW ) {

        // Let's start conting the release timer from the beginning
        release_time = millis();
      }
      last_state = state;
    }

    // Timing estimation

    // 1. Consider that 10 ms is quite enough to conclude that the button is released
    if( ( millis() - release_time ) > 10 ) {
      return true;
    }

    // 2. If the button is pressed for more than 750 ms, don't wait for release
    if( ( millis() - start_time ) > 10 ) {
      return true;
    }
  }
}

void SetNewLightLevel( uint32_t prev_level, uint32_t new_level )
{
  if( prev_level > 5 || new_level > 5 ) {
    // Logic error
    return;
  }
  
  uint32_t raw_values[] = { 0, 15, 75, 135, 195, 255 };
  uint32_t from = raw_values[ prev_level ];
  uint32_t to = raw_values[ new_level ];

  const uint32_t delay_pwm = 3000;

  // We change the light level very smooth
  if( from < to ) {
    for( uint32_t k = from; k <= to; ++k ) {
      delayMicroseconds( delay_pwm );
      analogWrite( PAYLOAD_PWM, k );
    }
  } else {
    // Attention! signed int is mandatory
    for( int k = from; k >= (int)to; --k ) {
      delayMicroseconds( delay_pwm );
      analogWrite( PAYLOAD_PWM, k );
    }
  }
}

// This is the timer we display, decrement each minute and have a button to increment by one on keypress
uint32_t GetCurrentCountDownCounterValue()
{
  //
  //               _initial_          -             full minutes elapsed
  //
  return initial_timer_value_minutes_ - ( millis() - timer_start_time_ms_ ) / 1000 / 60;
}

bool IsItTimeToShutdown()
{
  return initial_timer_value_minutes_ * 60 * 1000 <= ( millis() - timer_start_time_ms_ );
}

// First short press in a series of presses to display current value
void TimerValueFadeIn( uint32_t timer_val )
{
  if( timer_val > 31 ) {
    // logic error. Ignore
    return;
  }

  // Turn off all the indicator leds
  for( int k = 0; k < 5; ++k ) {
    digitalWrite( leds_addr_array_[ k ], LOW );
  }

  // Fade in  
  for( int level = 0; level < 255; ++level )
  {
    for( int k = 0; k < 5; ++k ) {
      if( ( ( 0x01 << k ) & timer_val ) != 0 ) {
        analogWrite( leds_addr_array_[ k ], level );
        delayMicroseconds( timer_display_delay_pwm );
      }
    }
  }
  last_timer_value_on_display = timer_val;
}

void TimerValueFadeOut( uint32_t timer_val )
{
  if( timer_val > 31 ) {
    // logic error. Ignore
    return;
  }

  // Fade out
  for( int level = 255; level > 0; --level )
  {
    for( int k = 0; k < 5; ++k ) {
      if( ( ( 0x01 << k ) & timer_val ) != 0 ) {
        analogWrite( leds_addr_array_[ k ], level );
      }
    }
    delayMicroseconds( timer_display_delay_pwm );
  }

  // Turn off all the indicator leds
  for( int k = 0; k < 5; ++k ) {
    digitalWrite( leds_addr_array_[ k ], LOW );
  }

  last_timer_value_on_display = timer_val;
}

void TimerValueDisplayChange( uint32_t prev_val, uint32_t new_value )
{
  // Common PWM cycle both for bits that are going to be set and to be reset
  for( int pwm_rising = 0; pwm_rising <= 255; ++pwm_rising )
  {
    for( int k = 0; k < 5; ++k ) {

      // Test if the digit has been changed
      if( ( ( 0x01 << k ) & prev_val ) != ( ( 0x01 << k ) & new_value ) ) {

        if( ( ( 0x01 << k ) & new_value ) != 0 ) {
          // Setting the digit
          analogWrite( leds_addr_array_[ k ], pwm_rising );
        } else {
          // Resetting the digit
          analogWrite( leds_addr_array_[ k ], 255 - pwm_rising );
        }
      }
    }
    delayMicroseconds( timer_display_delay_pwm );
  }

  last_timer_value_on_display = new_value;
}

void ProcessShortKeyPress()
{
  // A. First press for a while - show current value

  // B. A press in a series - change the value

  // Register current time
  last_pwr_btn_short_press_time = millis();
    
  if( !is_timer_value_displayed ) {
    is_timer_value_displayed = true;

    // Display current value
    TimerValueFadeIn( GetCurrentCountDownCounterValue() );
  } else {

    // Change value by one, also remember as a new initial value to be stored prior to the shut down operation, reset timer
    initial_timer_value_minutes_ = GetCurrentCountDownCounterValue() + timer_value_increment_sign_;
    if( initial_timer_value_minutes_ >= 31 ) {
      timer_value_increment_sign_ = -1;
    } else if( initial_timer_value_minutes_ <= 1 ) {
      timer_value_increment_sign_ = 1;
    }
    reset_general_timer();

    TimerValueDisplayChange( last_timer_value_on_display, initial_timer_value_minutes_ );
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - 

void setup()
{
  // A primary sign of life
  _blink_internal_led();
  
  setup_ports();
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - 

void loop()
{
  // 1. Check if the timer has expired and we need to power off
  if( millis() > ( timer_start_time_ms_ + initial_timer_value_minutes_ * 60 * 1000 ) ) {

    save_settings_and_power_off();
    return;
  }

  // 2. Check if timer setup button wasn't press for a significant time
  if( ( ( millis() - last_pwr_btn_short_press_time ) > 5000 ) && is_timer_value_displayed ) {
    TimerValueFadeOut( last_timer_value_on_display );
    is_timer_value_displayed = false;
  }

  // 3. Check if the light level adjustment button is pressed
  if( IsLightLevelButtonPressed() ) {
    
    uint32_t prev_lighting_level = lighting_level_;
    lighting_level_ += lighting_level_increment_sign_;
    adjust_lighting_level_direction();

    // Apply new light level value
    SetNewLightLevel( prev_lighting_level, lighting_level_ );

    // User uctivity detected, this is reasonable to reset the timer
    reset_general_timer();
    
    return;
  }

  // 4. Check if power button is pressed and if was pressed - what kind of press it was (long or short)
  enum PowerButtonPressType press_type = GetPowerButtonPress();
  if( press_type == NoKeyPress ) {
    return;
  }

  if( press_type == ShortKeyPress ) {
    ProcessShortKeyPress();
    return;
  }

  else if( press_type == LongKeyPress ) {
    delay( 100 );
    save_settings_and_power_off();
    return;
  }
}

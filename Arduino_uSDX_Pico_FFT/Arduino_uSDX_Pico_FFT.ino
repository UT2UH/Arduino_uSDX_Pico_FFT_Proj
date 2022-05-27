
/*

  Arduino_uSDX_Pico.ino  
  
  uSDX_PICO running in Raspberry Pi Pico RP2040 with Arduino
  https://github.com/ArjanteMarvelde/uSDR-pico




Arquivo  ...ini.elf.uf2  gerado em  /tmp/arduino_build_...
Rodei uma vez
~/.arduino15/packages/arduino/hardware/mbed_rp2040/3.0.1$ sudo ./post_install.sh
para permitir gravar direto do Arduino IDE para o Pico

 
Mods in the TFT_eSPI LIBRARY:

----------------------------------------------------
Change Arduino/Libraries/TFT_eSPI/User_Setup_Select.h
Comment:
//#include <User_Setup.h>           // Default setup is root library folder
UnComment:
#include <User_Setups/Setup60_RP2040_ILI9341.h>    // Setup file for RP2040 with SPI ILI9341

----------------------------------------------------
On User_Setups/Setup60_RP2040_ILI9341.h
Uncomment:
#define ILI9341_DRIVER
#define TFT_RGB_ORDER TFT_RGB  // Colour order Red-Green-Blue
Choose the SPI pins (SPI1)
// For the Pico use these #define lines
#define TFT_SPI_PORT 1   // 0=SPI  1=SPI1
#define TFT_MISO  12  //0  RX
#define TFT_MOSI  11  //3  TX
#define TFT_SCLK  10  //2
#define TFT_CS   13  //20  // Chip select control pin
#define TFT_DC   4  //18  // Data Command control pin
#define TFT_RST  5  //19  // Reset pin (could connect to Arduino RESET pin)
Choose the character fonts are going to be used
#define LOAD_GLCD   // Font 1. Original Adafruit 8 pixel font needs ~1820 bytes in FLASH
#define LOAD_FONT2  // Font 2. Small 16 pixel high font, needs ~3534 bytes in FLASH, 96 characters
#define LOAD_FONT4  // Font 4. Medium 26 pixel high font, needs ~5848 bytes in FLASH, 96 characters

--------------------------------------------------------------
For ILI9341 + RP2040,  change  TFT_eSPI/TFT_Drivers/ILI9341_Defines.h
#define TFT_WIDTH  320  //240
#define TFT_HEIGHT 240  //320


*/

#include "uSDR.h"





//---------------------------------------------------------------------------
//---------------------------------------------------------------------------
void setup() {
  // initialize digital pin LED_BUILTIN as an output.
  //pinMode(LED_BUILTIN, OUTPUT);
  gpio_init_mask(1<<LED_BUILTIN);  
  gpio_set_dir(LED_BUILTIN, GPIO_OUT); 

  
  //uSDX.h -> Serialx = Serial1   //UART0  /dev/ttyUSB0
  Serialx.begin(115200);  
  //Serialx.begin(19200);  

  uint16_t tim = millis();
 
  //delay(1000);   // required for Serial1 too
  for(int i=0; i<10; i++)
  {
  //digitalWrite(LED_BUILTIN, HIGH);   // turn the LED on (HIGH is the voltage level)
  gpio_set_mask(1<<LED_BUILTIN);
  delay(50);                       // wait for a second
  //digitalWrite(LED_BUILTIN, LOW);    // turn the LED off by making the voltage LOW
  gpio_clr_mask(1<<LED_BUILTIN);
   delay(50);                       // wait for a second
  }
    
  while (!Serialx)  //Caution!!  with Serial, if no USB-serial open, it will be stuck here
  {  //wait for PC USB-serial to open
  //digitalWrite(LED_BUILTIN, HIGH);   // turn the LED on (HIGH is the voltage level)
  gpio_set_mask(1<<LED_BUILTIN);
  delay(250);
  //digitalWrite(LED_BUILTIN, LOW);    // turn the LED off by making the voltage LOW
  gpio_clr_mask(1<<LED_BUILTIN);
  delay(250);
  }

  Serialx.println("Arduino uSDX Pico");
  Serialx.println("\nSerial took " + String((millis() - tim)) + "ms to start");




//  analogReadResolution(12);
//  analogWriteResolution(12);
//  analogWrite(DAC0, 0);
//  analogWrite(DAC1, 0);


//  display_setup();
//  adc_fft_setup();



  uSDR_setup();

}



//---------------------------------------------------------------------------
//---------------------------------------------------------------------------
void loop(void)
{

  uSDR_loop();
//gpio_xor_mask(1<<LED_BUILTIN);
}


 
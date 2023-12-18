/* Arduino Matrx Slot Machine
 * Concept, symbols and payout table: Daniel J. Murphy
 * Hardware and Software design: John Bradnam (jbrad2089@gmail.com)
 * Designed to fit in a ATTINY85 - ueses 6850 bytes Flash (83%), 196 bytes SRAM (38%), Stack space 316 bytes
 */
//#define DEBUG   //Comment out when ready to release
#define REELS_IN_FLASH  //Comment out to have Reels define in RAM
//#define RESET_EEPROM //Uncomment to reinitialise EEPROM
#define ATTINY85  //Comment out for an UNO
  
#include <LedControl.h>
#include <TimerFreeTone.h>                                                      // https://bitbucket.org/teckel12/arduino-timer-free-tone/wiki/Home
#include <EEPROM.h>
#include "Wheel.h"
#include "Piano.h"

#define DIGITS 7  //Number of MAX7219 daisy chained
#define WHEELS 3  //Number of wheels on machine
#define MONEY 0   //Location of 8 Digit 7 Segment display in MAX7219 daisy chain

//Macro to calculate order of Matrix displays
// - a = Matrix 0 to WHEELS-1 : maps onto the first parameter in the LedControl library

// - MAX7219 Dot Matrix Module 4-in-1 Display (To use first three matrixes WHEELS - 1 - a)
//#define DIGIT(a) (WHEELS - 1 - a)

// - MAX7219 goes first to 8 Digit 7 Seg display and then to MAX7219 Dot Matrix Module 4-in-1 Display
//#define DIGIT(a) (WHEELS - a)

// - MAX7219 goes first to 8 Digit 7 Seg display and then to individual MAX7219 Dot Matrix Modules
#define DIGIT(a) (a + 1)

  #define BUTTON_PIN 7
  #define DIN 10
  #define CLK 8
  #define LOAD 9
  #define TONE_PIN 11
  
  #define BUZZER_DDR  DDRB                                            // This is for the slot machines piezo buzzer
  #define BUZZER_PORT PORTB
  #define BUZZER_PIN  DDB3


LedControl lc=LedControl(DIN,CLK,LOAD,DIGITS);

#define BRIGHTNESS 4  //0 to 15


//Close Encounters
#define NUM_NOTES 5
const int closeEncounters[] PROGMEM = {                             // notes in the melody:
    NOTE_A2, NOTE_B2, NOTE_G2, NOTE_G1, NOTE_D2                     // "Close Encounters" tones
};
  


//- Payout Table
/*  Probabilities based on a 1 credit wager
    Three spaceships:     1 / (25 * 25 * 25)    = 0.000064
    Any three symbols:            24 / 15625    = 0.001536
    Two spaceships:         (24 * 3) / 15625    = 0.004608
    One spaceship:      (24 * 24 * 3)/ 15625    = 0.110592
    Two symbols match: (23 * 3 * 24) / 15625    = 0.105984
    House win, 1 minus sum of all probabilities = 0.777216
    _
                                                   P   R   O   O   F
                                                   Actual    Actual    
        Winning Combination Payout   Probablility  Count     Probability
        =================== ======   ============  ========  ===========*/
#define THREE_SPACESHIP_PAYOUT 600 //    0.000064            0.00006860   see the excel spreadsheet  
#define THREE_SYMBOL_PAYOUT    122 //    0.001536            0.00151760   that accompanies this program.
#define TWO_SPACESHIP_PAYOUT    50 //    0.004608            0.00468740
#define ONE_SPACESHIP_PAYOUT     3 //    0.110592            0.11064389
#define TWO_SYMBOL_PAYOUT        2 //    0.105984            0.10575249


/* Timing constants that ontrol how the reels spin */
#define START_DELAY_TIME 10
#define INCREMENT_DELAY_TIME 5
#define PAUSE_TIME 1000
#define MAX_DELAY_BEFORE_STOP 100
#define MIN_SPIN_TIME 1000
#define MAX_SPIN_TIME 3000
#define FLASH_REPEAT 10
#define FLASH_TIME 100
#define DIGIT_DELAY_TIME 50

/* spinDigit holds the information for each wheel */
struct spinDigit 
{
  unsigned long delayTime;
  unsigned long spinTime;
  unsigned long frameTime;
  uint8_t row;
  uint8_t symbol;
  bool stopped;
};

spinDigit spin[WHEELS]; 

#define STARTING_CREDIT_BALANCE 5000    // Number of credits you have at "factory reset".
#define DEFAULT_HOLD            0       // default hold is zero, over time the machine pays out whatever is wagered
#define MINIMUM_WAGER           5       // 
#define WAGER_INCREMENT         5       //
#define MAGIC                   0xBAD   // Used to detect if EEPROM data is good

/* structure that gets stored in EEPROM */
struct retained
{
  unsigned long magic;                  // magic number
  unsigned long payedOut;               // sum of all payouts
  unsigned long wagered;                // sum of all wagers  (profit = payouts - wagers)
  unsigned long plays;                  // the number of spins
  unsigned long  twoMatchCount;         // number of times two symbols have matched
  unsigned int  threeMatchCount;        // number of times three symbols have matched
  unsigned long  shipOneMatchCount;     // number of times one ship has appeared
  unsigned int  shipTwoMatchCount;      // number of time two ships have appeared
  unsigned int  shipThreeMatchCount;    // number of times three ships have appeared (Jackpot!)
  unsigned long eepromWrites;           // number of times we've written to EEprom.  100,000 is the approximate maximum
  long creditBalance;                   // the credit balance.
  int hold;                             // the house advantage, in percent, usually between 1 and 15, 2 bytes  
  unsigned int seed;                    // random seed
};

/* global variables to reduce stack size on ATTiny85 */
retained stats;
unsigned long wagered;
unsigned long payout;
double owedExcess = 0;

//--------------------------------------------------------------------------------------------

//Setup LCD and the spin array.
void setup() 
{
  #ifdef DEBUG
    Serial.begin(115200);
  #endif

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(TONE_PIN, OUTPUT);

  //Read from EEPROM the initial stats
  readRetainedData(&stats);

  //Set up each LED Matrix. 
  randomSeed(stats.seed);
  for (uint8_t j = 0; j < DIGITS; j++)
  {
    lc.shutdown(j,false); //Wake up
    lc.setIntensity(j, BRIGHTNESS);
    lc.clearDisplay(j);

    if (j < WHEELS)
    {
      spin[j].row = random(0, SYMBOLS) << 3;  //Start each wheel on a random symbol
    }
  }
  
  //Display the current credit balance
  displayNumber(stats.creditBalance);  

  //Play splash screen
  playSplashScreen();

  //Show the openning set of wheeld
  for (uint8_t j = 0; j < WHEELS; j++)
  {
    displayWheelSymbol(j);
  }

}

//--------------------------------------------------------------------------------------------

//Main loop spins the wheels
void loop()
{

  waitOnButtonPress();
  
  spinTheWheels();
  #ifdef DEBUG
    for (uint8_t i = 0; i < WHEELS; i++)
    {
      Serial.print(String(spin[i].symbol) + " ");
    }
    Serial.println(" - Payout is " + String(payout));
  #endif
  
  delay(PAUSE_TIME / 2);

  //All stopped, time to pay out
  wagered = MINIMUM_WAGER;
  double winnings = wagered * (payout - (payout * (stats.hold / 100.0))); //winnings are the amount wagered times the payout minus the hold.
  long roundWinnings = (long) round(winnings);
  owedExcess += winnings - roundWinnings;                                 // owedExcess is the change; credits between -1 and 1.
  if (owedExcess >= 1 || owedExcess <= -1) 
  {                                                                       // if we can pay out some excess
    int roundOwedExcess = (int) round(owedExcess);
    roundWinnings += roundOwedExcess;                                     // add the rounded portion to the winnings
    owedExcess -= roundOwedExcess;                                        // subtract out what we added to continue to track the excess
  } 
  roundWinnings -= wagered;                                               // you pay for your bet whether you won or not!  
  stats.payedOut += roundWinnings;
  stats.wagered += wagered;
  adjustCreditBalance(stats.creditBalance + roundWinnings);
  updateRetainedData(&stats);
  
  //delay(PAUSE_TIME / 2);

}

//--------------------------------------------------------------------------------------------

//Spins all the wheels and updates payout
void spinTheWheels()
{
  //Reset wheels for the spin
  unsigned long totalTime = millis();
  for (uint8_t j = 0; j < WHEELS; j++)
  {
    totalTime = totalTime + random(MIN_SPIN_TIME, MAX_SPIN_TIME);
    spin[j].delayTime = START_DELAY_TIME;
    spin[j].spinTime = totalTime;
    spin[j].frameTime = millis() + spin[j].delayTime;
    spin[j].stopped = false;
  }
  
  bool allStopped = false;
  while (!allStopped)
  {
    //Scroll each symbol up
    for (uint8_t j = 0; j < WHEELS; j++)
    {
      if (!spin[j].stopped && millis() > spin[j].frameTime)
      {
        spin[j].frameTime = millis() + spin[j].delayTime;

        displayWheelSymbol(j);
        spin[j].row = (spin[j].row + 1) % TOTAL_SYMBOL_ROWS;

        beepWheel();
        
        if (millis() > spin[j].spinTime)
        {
          //Stop if delayTime exceeds MAX_DELAY_BEFORE_STOP
          //Only stop on complete symbol
          if (spin[j].delayTime > MAX_DELAY_BEFORE_STOP && (spin[j].row % 8) == 1)
          {
            spin[j].stopped = true;
            spin[j].symbol = spin[j].row >> 3;
            if (j == (WHEELS - 1))
            {
              //All wheels are now stopped
              allStopped = true;
              highlightWinAndCalculatePayout();
            }
          }
          else if (spin[j].delayTime <= MAX_DELAY_BEFORE_STOP)
          {
            spin[j].delayTime = spin[j].delayTime + INCREMENT_DELAY_TIME;
          }
        }
      }
    }
    yield();
  }
}

//--------------------------------------------------------------------------------------------

//Display the current symbol of the specified wheel
void displayWheelSymbol(int wheel)
{
  for (int8_t i = 7; i >= 0; i--)
  {
    lc.setRow(DIGIT(wheel), i, getReelRow((spin[wheel].row + i) % TOTAL_SYMBOL_ROWS));
  }
}

//--------------------------------------------------------------------------------------------

//Work out if the player has one anything
//If they have, flash winning sequence and update payout multiplier
void highlightWinAndCalculatePayout()
{
  payout = 0;
  uint8_t matches = 0;
  uint8_t symbol = 0;
  uint8_t spaceships = 0;
  for (uint8_t y = 0; y < WHEELS; y++)
  {
    if (spin[y].symbol == SPACESHIP)
    {
      spaceships++;
      matches = spaceships;
      symbol = SPACESHIP;
    }
    else if (spaceships == 0)
    {
      for (uint8_t x = 0; x < WHEELS; x++)
      {
        if (spin[y].symbol == spin[x].symbol && y != x)
        {
          matches++;
          symbol = spin[y].symbol;
        }
      }
    }
  }
  if (matches > 0)
  {
    switch (spaceships)
    {
      case 3: payout = THREE_SPACESHIP_PAYOUT; stats.shipThreeMatchCount++; winSound(5); break;
      case 2: payout = TWO_SPACESHIP_PAYOUT; stats.shipTwoMatchCount++; winSound(3); break;
      case 1: payout = ONE_SPACESHIP_PAYOUT; stats.shipOneMatchCount++; winSound(2); break;
      default:
        switch (matches)
        {
          case 3: payout = THREE_SYMBOL_PAYOUT; stats.threeMatchCount++; winSound(4); break;
          case 2: payout = TWO_SYMBOL_PAYOUT; stats.twoMatchCount++; winSound(1); break;
        }
        break;
    }
    flashSymbol(symbol);
  }
  //Count every spin
  stats.plays++;
}

//---------------------------------------------------------------------------

//Flashes any wheel that is showing the specified symbol
void flashSymbol(uint8_t symbol)
{
  bool on = true;
  uint8_t row = symbol << 3;
  for (uint8_t r = 0; r < FLASH_REPEAT; r++)
  {
    for (uint8_t j = 0; j < WHEELS; j++)
    {
      if (spin[j].symbol == symbol)
      {
        for (int8_t i = 7; i >= 0; i--)
        {
          if (on)
          {
            lc.setRow(DIGIT(j), i, 0);
          }
          else
          {
            lc.setRow(DIGIT(j), i, getReelRow((row + i) % TOTAL_SYMBOL_ROWS));
          }
        }
      }
    }
    on = !on;
    delay(FLASH_TIME);
  }
}

//---------------------------------------------------------------------------

//Play the opening anaimation
void playSplashScreen() 
{
  //Show aliens walking
  for (uint8_t k = 0; k < 1; k++)
  {
    for (uint8_t j = 0; j < WHEELS; j++)
    {
      for (int8_t i = 7; i >= 0; i--)
      {
        lc.setRow(DIGIT(j), i, getReelRow((ALIEN_1 << 3) + i));
      }
      delay(250);
      for (int8_t i = 7; i >= 0; i--)
      {
        lc.setRow(DIGIT(j), i, getReelRow((ALIEN_2 << 3) + i));
      }
      delay(250);
    }
    playMelody();
  }

  //Move ship from right to left clearing out the aliens
  for (int p = WHEELS * 8 - 1; p > -8; p--)
  {
    for (uint8_t c = 0; c < 8; c++)
    {
      //Display each column at position p
      int pc = p + c;
      if (pc >= 0 && pc < (WHEELS << 3))
      {
        for (uint8_t r = 0; r < 8; r++)
        {
          lc.setLed(DIGIT((pc >> 3)), r, pc & 0x07, getReelRow((SPACESHIP << 3) + r) & (1 << c));
        }
      }
      //Clear last column
      pc = pc + 1;
      if (pc >= 0 && pc < (WHEELS << 3))
      {
        lc.setColumn(DIGIT((pc >> 3)), pc & 07, 0);
      }
    }
    delay(10);
  }
  lc.clearDisplay(DIGIT(0));
  delay(2000);
}

//-----------------------------------------------------------------------------------

//Read a row from the reels either from FLASH memory or RAM
uint8_t getReelRow(uint8_t row)
{
  #ifdef REELS_IN_FLASH
    return pgm_read_byte(reel + row);
  #else
    return reel[row];
  #endif
}

//-----------------------------------------------------------------------------------

//Turn on and off buzzer quickly
void beepWheel() 
{                                                                   // Beep and flash LED green unless STATE_AUTO
  BUZZER_PORT |= (1 << BUZZER_PIN);                                           // turn on buzzer
  delay(20);
  BUZZER_PORT &= ~(1 << BUZZER_PIN);                                          // turn off the buzzer
}

//-----------------------------------------------------------------------------------

//Turn on and off buzzer quickly
void beepDigit() 
{                                                                   // Beep and flash LED green unless STATE_AUTO
  BUZZER_PORT |= (1 << BUZZER_PIN);                                           // turn on buzzer
  delay(5);
  BUZZER_PORT &= ~(1 << BUZZER_PIN);                                          // turn off the buzzer
}

//-----------------------------------------------------------------------------------

//Play the winning siren multiple times
void winSound(uint8_t repeat)
{
  for (uint8_t i = 0; i < repeat; i++)
  {
    playSiren();
  }
}

//-----------------------------------------------------------------------------------

//Play the siren sound
void playSiren() 
{
  #define MAX_NOTE                4978                                         // Maximum high tone in hertz. Used for siren.
  #define MIN_NOTE                31                                           // Minimum low tone in hertz. Used for siren.
  
  for (int note = MIN_NOTE; note <= MAX_NOTE; note += 5)
  {                       
    TimerFreeTone(TONE_PIN, note, 1);
  }
}

//-----------------------------------------------------------------------------------

//Play the "Close Encounters" melody
void playMelody() 
{
  for (int thisNote = 0; thisNote < NUM_NOTES; thisNote++) 
  {
    // to calculate the note duration, take one second divided by the note type.
    //e.g. quarter note = 1000 / 4, eighth note = 1000/8, etc.
    int noteDuration = 500;
    TimerFreeTone(TONE_PIN, pgm_read_byte(closeEncounters + thisNote) * 3, noteDuration); 
    delay(100);
  }
}

//-----------------------------------------------------------------------------------

//animate the change in credit balance
void adjustCreditBalance(long newBalance)
{
  unsigned int difference;
  int8_t direction;
  if (stats.creditBalance != newBalance)
  {
    if (stats.creditBalance > newBalance)
    {
      difference = stats.creditBalance - newBalance;
      direction = -1;
    }
    else
    {
      difference = newBalance - stats.creditBalance;
      direction = 1;
    }
    
    for (unsigned int i = 0; i < difference; i++)
    {
      stats.creditBalance += direction;
      displayNumber(stats.creditBalance);
      beepDigit();
      delay(DIGIT_DELAY_TIME);
    }
  }
}

//-----------------------------------------------------------------------------------

//Display a number on the 7 segment display
void displayNumber(long number)
{
  bool negative = (number < 0);
  number = abs(number);
  uint8_t digit;
  for (uint8_t i = 0; i < 8; i++)
  {
    if (number > 0 || i == 0)
    {
      digit = number % 10;
      lc.setDigit(MONEY, i, digit, false);
    }
    else if (negative)
    {
      lc.setChar(MONEY, i, '-', false);
      negative = false;
    }
    else
    {
      lc.setChar(MONEY, i, ' ', false);
    }
    number = number / 10;
  }
}

//-----------------------------------------------------------------------------------

//Read from EEPROM
void readRetainedData(retained* p)
{
  for (uint8_t addr = 0; addr < sizeof(retained); addr++)
  {
    *((byte *)p + addr) = EEPROM.read(addr);
  }
  #ifdef RESET_EEPROM
    p->magic = 0;
  #endif
  
  if (p->magic != MAGIC)
  {
    //Initialise the data
    p->magic = MAGIC;
    p->payedOut = 0;
    p->wagered = 0;
    p->plays = 0;
    p->twoMatchCount = 0;
    p->threeMatchCount = 0;
    p->twoMatchCount = 0;
    p->shipOneMatchCount = 0;
    p->shipTwoMatchCount = 0;
    p->shipThreeMatchCount = 0;
    p->eepromWrites = 1;
    p->creditBalance = STARTING_CREDIT_BALANCE;
    p->hold = DEFAULT_HOLD;
    p->seed = analogRead(A0);

    //Write it to EEPROM for the first time
    for (uint8_t addr = 0; addr < sizeof(retained); addr++)
    {
      EEPROM.write(addr, *((byte *)p + addr));
    }
  }

  //On power on, if the player has no money, reset the credit
  if (p->creditBalance <= 0)
  {
    p->creditBalance = STARTING_CREDIT_BALANCE;
  }
  
}

//-----------------------------------------------------------------------------------

//Update to EEPROM
void updateRetainedData(retained* p)
{
  //Record writes
  p->eepromWrites++;
  //store a new seed so that it isn't the same at next power on
  p->seed = random(0,65535);

  //Write it to EEPROM for the first time
  for (uint8_t addr = 0; addr < sizeof(retained); addr++)
  {
    EEPROM.update(addr, *((byte *)p + addr));
  }
}

//-----------------------------------------------------------------------------------

//Wait until player presses the button
void waitOnButtonPress()
{
  bool released = false;
  while (!released)
  {
    while (digitalRead(BUTTON_PIN) == HIGH)
    {
      yield();
      delay(10);
    }
    delay(20);
    if (digitalRead(BUTTON_PIN) == LOW)
    {
      while (digitalRead(BUTTON_PIN) == LOW)
      {
        yield();
        delay(10);
      }
      released = true;
    }
  }
}

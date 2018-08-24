/*

  Copyright © 2018 Razvan Murariu <razwww@gmail.com>
  This program is free software. It comes without any warranty, to
  the extent permitted by applicable law. You can redistribute it
  and/or modify it under the terms of the Do What The Fuck You Want
  To Public License, Version 2, as published by Sam Hocevar. See
  the COPYING file for more details.

  The circuit:
   Arduino Mega 2560
   LCD SDA pin to analog pin 20, blue wire
   LCD SCL pin to analog pin 21, green wire
   LCD VCC pin to 5V
   LCD GND pin to ground
   RTC/SD CS pin to pin 48
   RTC/SD MISO pin to digital 50, gray wire
   RTC/SD MOSI pin to pin 51, yellow wire
   RTC/SD SCK pin to digital pin 52, purple wire
   RTC/SD SCL pin to analog pin 21
   RTC/SD SDA pin to analog pin 20
   RTC/SD VCC pin to 5V
   RTC/SD GND pin to ground
   Network card MISO pin to digital pin 50, gray wire
   Network card MOSI pin to digital pin 51, yellow wire
   Network card SCS pin to digital pin 10
   Network card SCLK pin to pin 52, purple wire
   Temp sensor (interior) pin to digital pin 8
   Temp sensor (exterior) pin to digital pin 9
   Buttons to digital pins 2-7
   Relay to analog pin A0 & VIn
*/

#include <DHT.h>
#include <Bounce2.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <SPI.h>
#include <Ethernet2.h>
#include <SD.h>
#include <DS1307RTC.h>  // a basic DS1307 library that returns time as a time_t

//things that you may change:
#define displayTime 8000  // display duration of "other info"
#define lightTime 8000 // light duration
#define buttonDownInterval 3000 // how long to press the button for "about"
#define lightOnKeypress 1  //light for each button
#define prod 1 // in test mode: permanent backlight
const String secret="********";  // secret for URL

#define tempMin 5
#define tempMax 30
#define tempConfort 20
#define hysteresis 0.5

//if you change the number of elements in the arrays you must also change the program table
const int timeInterval[4] = {480, 510, 1320, 30}; // 8:00, 8:30, 22:00, 00:30
const int tempInterval[4] = {20, 18, 21, 19}; // corresponding temp for each time interval
const byte numberOfIntervals = sizeof(timeInterval) / sizeof(int);

const long writeTempInterval = 300000;  // 5 min, interval for writing temps to file
//end of things that you may change :)

#define modeAuto 0 // full auto mode, based on the intervals
#define modeManual 1 // manual mode, it will reset when the next interval starts
#define modeHold 2 // hold mode, permanent manual, it will reset when the RESET button is pushed
#define modeInfoH 3     // Info Humidity
#define modeInfoUp 4    // Uptime and RAM
#define modeInfoIp 5  // IP and mask
#define modeAbout 6   // About

#define DHTPin 8     // pin for temperature sensor
#define DHTType DHT22   // type of temperature sensor
#define DHTExtPin 9     // pin for exterior temperature sensor
#define DHTExtType DHT22   // type of exterior temperature sensor


#define DS1307_I2C_ADDRESS 0x68 // RTC
#define relayPort A0  // analog port for the relay

float temp; // real temperature (inside)
float tempExt; // real temperature (outside)
float tempAuto; // temperature according to the auto program
float tempManual; // temperature set with the +/- buttons
float humidityIn;
float humidityExt;
float hic; // heat index in Celsius
float hicExt;
byte mode = 0;
byte modeReturn = mode; // remember the mode when displaying info screens
byte modeOld = mode; // used for clearing the display when the mode changes
int dayOld;  // used for syncing NTP once a day
boolean relay = false;
boolean relayOld = relay; // used for logging the relay state change
int totalMinutes;
int previousMinutes;
byte otherInfo = 0;
// degree centigrade symbol
uint8_t degree[8] = {0x8, 0xf4, 0x8, 0x43, 0x4, 0x4, 0x43, 0x0};
// for non-I2C LCD byte degree[8] = {B01000, B10100, B01000, B00011, B00100, B00100, B00011, B00000};
// flame symbol
uint8_t flame[8] = {0x12, 0x09, 0x12, 0x09, 0x12, 0x09, 0x12, 0x1F};
// for non-I2C LCD byte flame[8] = {0b10010, 0b01001, 0b10010, 0b01001, 0b10010, 0b01001, 0b10010, 0b11111};
unsigned long timerLight = -1; // temporary store millis for display duration
unsigned long timerInfo = -1;
unsigned long button3Down = 0;  // for "about". If the button is pressed when time overflows it won't work
const String message = "               Made by Razvan & Coralia";
const uint8_t BUTTON_PINS[6] = {7, 6, 5, 4, 3, 2};
const int chipSelect = 48; // CS pin for SD card
File systemLog;
File temperatureLog;
String systemLogFilename;
String temperatureLogFilename;
String logString;
unsigned long previousMillis = 0;   // will store when temps were written to file

//ethernet settings
const boolean dhcp = 0;
const byte mac[] = { 0x90, 0xA2, 0xDA, 0x0D, 0x85, 0xD9 };   //physical mac address, mandatory for DHCP or fixed address
const byte ip[] = { ***, ***, ***, *** };                 // ip in lan
const byte dnserver[] = { 8, 8, 8, 8 };                //not really used, but needed in the initialization
const byte subnet[] = { ***, ***, ***, *** };              //subnet mask
const byte gateway[] = { ***, ***, ***, *** };              // default gateway
EthernetServer server(80);                       //server port

// for NTP
const unsigned int localPort = 8888;       // local port to listen for UDP packets
const char timeServer[] = "94.53.216.184"; // time.nist.gov NTP server
const int NTP_PACKET_SIZE = 48; // NTP time stamp is in the first 48 bytes of the message
byte packetBuffer[ NTP_PACKET_SIZE]; //buffer to hold incoming and outgoing packets
// A UDP instance to let us send and receive packets over UDP
EthernetUDP Udp;

// Initialize LCD
LiquidCrystal_I2C lcd(0x27, 2, 1, 0, 4, 5, 6, 7, 3, POSITIVE);  // 20x4

// Initialize DHT sensor
DHT dht(DHTPin, DHTType);
DHT dhtExt(DHTExtPin, DHTExtType);

Bounce * buttons = new Bounce[6];
Bounce debouncer = Bounce();  //for about

// returns the hour and minute when the next interval begins
void nextInterval(int &myHour, int &myMinute) {
  int nextAutoTime;
  if (totalMinutes > timeInterval[2] || totalMinutes < timeInterval[3]) {
    nextAutoTime = timeInterval[3];
  }
  else if (totalMinutes < timeInterval[0]) {
    nextAutoTime = timeInterval[0];
  }
  else if (totalMinutes < timeInterval[1]) {
    nextAutoTime = timeInterval[1];
  }
  else {
    nextAutoTime = timeInterval[2];
  }
  myHour = nextAutoTime / 60;
  myMinute = nextAutoTime % 60;
}

// - minTotalMinutes: the start of the interval to check
// - maxTotalMinutes: the end of the interval to check
// - tempAutoLocal: the auto temperature of the interval to check
void compareTemperatureAuto (int minTotalMinutes, int maxTotalMinutes, float tempAutoLocal) {
  if (mode != modeAuto) { // this is not our case and we exit the method
    return;
  }
  if (minTotalMinutes < maxTotalMinutes) {
    if (totalMinutes >= minTotalMinutes && totalMinutes < maxTotalMinutes) {
      if (temp < (tempAuto - hysteresis) ) {
        relay = 1;
      }
      else if (temp > (tempAuto + hysteresis) ) {
        relay = 0;
      }
      tempAuto = tempAutoLocal;
    }
  }
  else { // roll-over to the next day
    if (totalMinutes >= minTotalMinutes || totalMinutes < maxTotalMinutes) {
      if ( temp <= (tempAuto + hysteresis) ) {
        relay = 1;
      }
      else if ( temp >= (tempAuto + hysteresis) ) {
        relay = 0;
      }
      tempAuto = tempAutoLocal;
    }
  }
}

//function to display mode, clock, target temperature & others on the LCD
void displayMode () {
  char hourMinute [6];
  if (mode == modeAuto || mode == modeManual || mode == modeHold) {
    // current temp
    lcd.setCursor(0, 0);
    lcd.print(F("In="));
    lcd.print(temp, 1);
    lcd.print((char)0);

    lcd.setCursor(10, 0);
    lcd.print(F("Out="));
    lcd.print(tempExt, 1);
    lcd.print((char)0);

    lcd.setCursor(0, 2);
    lcd.print(F("Set"));


    //clock & time
    sprintf (hourMinute, "%02u:%02u", hour(), minute());
    lcd.setCursor(0, 3);
    lcd.print(hourMinute);
    lcd.print(F("  "));
    char dayMonthYear [11];
    sprintf (dayMonthYear, "%02u.%02u.%04u", day(), month(), year());
    lcd.print(dayMonthYear);

    // relay indicator
    lcd.setCursor(19, 3);
    relay ? lcd.print((char)1) : lcd.print(F(" "));
  }

  switch (mode) {
    int myHour, myMinute;
    case modeAuto:
      lcd.setCursor(4, 2);
      lcd.print(tempAuto,1);
      lcd.print((char)0);
      lcd.print(F(" auto>"));
      nextInterval(myHour, myMinute);
      sprintf (hourMinute, "%02u:%02u", hour(), minute());
      lcd.print(hourMinute);
      break;
    case modeManual:
      lcd.setCursor(4, 2);
      lcd.print(tempManual,1);
      lcd.print((char)0);
      lcd.print(F(" man.>"));
      nextInterval(myHour, myMinute);
      sprintf (hourMinute, "%02u:%02u", hour(), minute());
      lcd.print(hourMinute);
      break;
    case modeHold:
      lcd.setCursor(4, 2);
      lcd.print(tempManual,1);
      lcd.print((char)0);
      lcd.print(F(" HOLD"));
      break;
    case modeInfoH:         // first info screen
      lcd.setCursor(0, 0);
      lcd.print(F("Humidity:"));
      lcd.setCursor(0, 1);
      lcd.print(F("In="));
      lcd.print(humidityIn, 1);
      lcd.print(F("%"));
      lcd.setCursor(10, 1);
      lcd.print(F("Out="));
      lcd.print(humidityExt, 1);
      lcd.print(F("%"));
      
      lcd.setCursor(0, 2);
      lcd.print(F("Heat index:"));
      lcd.setCursor(0, 3);
      lcd.print(F("In="));
      lcd.print(hic);
      lcd.print((char)0);
      lcd.setCursor(10, 3);
      lcd.print(F("Out="));
      lcd.print(hicExt);
      lcd.print((char)0);
      break;
    case modeInfoUp:      // second info screen
      lcd.setCursor(0, 0);
      lcd.print(F("Up "));
      lcd.print(uptime());
      lcd.setCursor(0, 1);
      lcd.print(F("RAM free "));
      lcd.print(percentFreeRam());
      lcd.print(F("%"));
      break;
    case modeInfoIp:      // third info screen
      lcd.setCursor(0, 0);
      lcd.print(F("Network info: "));
      dhcp ? lcd.print(F("DCHP")) : lcd.print(F("static"));
      lcd.setCursor(0, 1);
      lcd.print(F("IP "));
      lcd.print(Ethernet.localIP());
      lcd.setCursor(0, 2);
      lcd.print(F("Mask "));
      lcd.print(Ethernet.subnetMask());
      lcd.setCursor(0, 3);
      lcd.print(F("GW "));
      lcd.print(Ethernet.gatewayIP());
      break;
    case modeAbout:   // easteregg :)
      for (int i=0; i<message.length(); i++) {
        lcd.clear();
        lcd.setCursor(3,0);
        lcd.print(F("Cronotermostat"));
        lcd.setCursor(0,2);
        lcd.print(message.substring(i, i+20));
        delay(300);
      }
      mode = modeReturn;
      break;
  }
}

void funcDisplayLastMode() {
  mode = modeReturn;
}

void noBacklight()
{
  // todo Coralia - refactor
  lcd.noBacklight();
}

void switchBack(long waitTime, unsigned long &timer, void (*function)()) {
  if ( timer != -1 && millis() - timer > waitTime) {
    // call parameter method
    (*function)();
    timer = -1;
  }
}

int freeRam () {
  extern int __heap_start, *__brkval;
  int v;
  return (int) &v - (__brkval == 0 ? (int) &__heap_start : (int) __brkval);
}

long percentFreeRam () {
  long freeR = freeRam();
  return ( freeR * 100 ) / 8192;   // for Uno/Nano: / 2048
}

void lightOn() {
  timerLight = millis();
  lcd.backlight();
}

void pushTempPlus () {
    if (mode == modeAuto) {
    mode = modeManual;
    // if the current program is cooler than the confort temp, go directy to confort temp
    if (tempAuto < tempConfort) {
      tempManual = tempConfort;
    }
    else {
      tempManual = tempAuto + 0.5;            
    }
  }
  else {
    tempManual = tempManual + 0.5;
  }
}

void pushTempMinus () {
    if (mode == modeAuto) {
    mode = modeManual;
    tempManual = tempAuto - 0.5;
  }
  else {
    tempManual = tempManual - 0.5;
  }
}

void pushHold () {
  if (mode == modeAuto) {
    mode = modeHold;
    tempManual = tempAuto;
  }
  else if (mode == modeManual) {
    mode = modeHold;
  }
  else if (tempAuto == tempManual) { //we already are in modeHold, so setureset to what it was before
    mode = modeAuto;
  }
  else {
    mode = modeManual;
  }
}

void getButton () {
  for (int i = 0; i < 6; i++)  {    // Update the Bounce instance
    buttons[i].update();
    if (buttons[i].rose()) {
      if (i == 3) {
        button3Down = 0;
      }
    }
    if ( buttons[i].fell() ) {    // If it fell, do the appropriate action

      if (i == 0) { //light
        lightOn();
        logInfo("physical button 'light'");
      }

      else if (i == 1) { // temp +
        if ( lightOnKeypress ) {
          lightOn();
        }
        pushTempPlus();
        logInfo("physical button 'temp+'");
      }

      else if (i == 2) { // temp -
        
        if ( lightOnKeypress ) {
          lightOn();
        }
        pushTempMinus();
        logInfo("physical button 'temp-'");
      }

      else if (i == 3) { // reset to auto + about
        if ( lightOnKeypress ) {
          lightOn();
        }
        mode = modeAuto;
        tempManual = tempAuto;
        button3Down = millis();// start counting for long press
        logInfo("physical button 'reset'");
      }

      else if (i == 4) { // mode hold
        if ( lightOnKeypress ) {
          lightOn();
        }
        pushHold();
        logInfo("physical button 'hold'");
      }

      else if (i == 5) { // other info
        
        if ( lightOnKeypress ) {
          lightOn();
        }
        switch (mode) {
          case modeAuto:
          case modeManual:
          case modeHold:
            modeReturn = mode;
            mode = modeInfoH;
            timerInfo = millis();
            break;
          case modeInfoH:
            mode = modeInfoUp;
            timerInfo = millis();
            break;
          case modeInfoUp:
            mode = modeInfoIp;
            timerInfo = millis();
            break;
          case modeInfoIp:
            mode = modeReturn;
            break;
        }
        logInfo("physical button 'other info'");
      }
    }
  }
  if (button3Down != 0 && (millis() - button3Down > buttonDownInterval) ) {
    modeReturn = mode;
    mode = modeAbout;
    timerInfo = millis();
    button3Down = 0;
    logInfo("physical button 'reset' long");
  }
}

String uptime() {  // todo? rollover
  long up = millis();
  long days = 0;
  long hours = 0;
  long mins = 0;
  mins = up / 1000 / 60;
  hours = mins / 60;
  days = hours / 24;
  mins = mins - (hours * 60);
  hours = hours - (days * 24);
  String uptime = "";
  if (days > 0) {
    uptime += days;
    uptime += "d ";
  }
  uptime += hours;
  uptime += "h ";
  uptime += mins;
  uptime += "m ";
  return uptime;
}

void listStatFiles(EthernetClient client) {
  File workingDir = SD.open("/data/");
  client.println(F("<ul>"));
    while(true) {
      File entry =  workingDir.openNextFile();
      if (! entry) {
        break;
      }
      client.print(F("<li><a href=\"/HC.htm?file="));
      client.print(entry.name());
      client.print(F("\">"));
      client.print(entry.name());
      client.println(F("</a></li>"));
      entry.close();
    }
  client.println(F("</ul>"));
  workingDir.close();
}

void listLogFiles(EthernetClient client) {
  File workingDir = SD.open("/logs/");
  client.println(F("<ul>"));
    while(true) {
      File entry =  workingDir.openNextFile();
      if (! entry) {
        break;
      }
      client.print(F("<li><a href=\""));
      client.print(entry.name());
      client.print(F("\">"));
      client.print(entry.name());
      client.println(F("</a></li>"));
      entry.close();
    }
  client.println(F("</ul>"));
  workingDir.close();
}

// standard response header
void httpOK(EthernetClient client, String type) {
  client.println(F("HTTP/1.1 200 OK"));
  client.println("Content-Type: text/"+type);
  client.println(F("Connection: close"));
  client.println();
  if (type == "html") {
    client.println(F("<!DOCTYPE html>"));
    client.println(F("<link rel=\"icon\" href=\"data:;base64,=\">"));  // avoid favicon requests
  }
}

// 404 response header
void http404(EthernetClient client) {
  client.println(F("HTTP/1.1 404 Not Found"));
  client.println(F("Content-Type: text/plain"));
  client.println();
  client.println(F("404 Not Found"));
}

void writeSD(String logString, String filename) {  // writes 'logString' to 'filename' on SD card
  File dataFile = SD.open(filename, FILE_WRITE);
  if (!dataFile) {
    Serial.print(F("error opening "));
    Serial.println(filename);
  }
  else {
    dataFile.println(logString);
    dataFile.close();
  }
}

void logInfo(String logString) {  // writes messages to console and to the .log file
  File dataFile = SD.open(systemLogFilename, FILE_WRITE);
  if (!dataFile) {
    Serial.print(F("error opening "));
    Serial.println(systemLogFilename);
  }
  else {
    String tempLog = "";
    char fullDateAndTime [20];
    sprintf (fullDateAndTime, "%04u.%02u.%02u %02u:%02u:%02u", year(), month(), day(), hour(), minute(), second());
    tempLog += fullDateAndTime;
    tempLog += " ";
    tempLog += now();
    tempLog += " ";
    tempLog += logString;
    dataFile.println(tempLog);
    dataFile.close();
    Serial.println(tempLog);
  }
}

// sync with NTP server
// send an NTP request to the time server at the given address
void sendNTPpacket(char* address) {
  // set all bytes in the buffer to 0
  memset(packetBuffer, 0, NTP_PACKET_SIZE);
  // Initialize values needed to form NTP request
  // (see URL above for details on the packets)
  packetBuffer[0] = 0b11100011;   // LI, Version, Mode
  packetBuffer[1] = 0;     // Stratum, or type of clock
  packetBuffer[2] = 6;     // Polling Interval
  packetBuffer[3] = 0xEC;  // Peer Clock Precision
  // 8 bytes of zero for Root Delay & Root Dispersion
  packetBuffer[12]  = 49;
  packetBuffer[13]  = 0x4E;
  packetBuffer[14]  = 49;
  packetBuffer[15]  = 52;

  // all NTP fields have been given values, now
  // you can send a packet requesting a timestamp:
  Udp.beginPacket(address, 123); //NTP requests are to port 123
  Udp.write(packetBuffer, NTP_PACKET_SIZE);
  Udp.endPacket();
}

String displayAddress(IPAddress address)
{
 return String(address[0]) + "." + 
        String(address[1]) + "." + 
        String(address[2]) + "." + 
        String(address[3]);
}

//for AJAX display SET temperature
void GetTemp(EthernetClient client)
{
  client.print(F("<strong>Set at: </strong>"));
  if ( mode == modeAuto ){
    client.print(tempAuto, 1);
  }
  if ( mode == modeManual || mode == modeHold ){
    client.print(tempManual, 1);
  }
  client.println(F(" &deg;C <br><br>"));
  
  client.print(F("<strong>Relay: </strong>"));
  if (relay) {
    client.print(F("&#9728;  ON  &#9728;"));
  }
  else {
    client.print(F("OFF"));
  }

  client.print(F("<br><br><strong>Program: </strong>"));
  if ( mode == modeAuto ){
    client.print(F("auto until "));
    int myHour, myMinute;
    nextInterval(myHour, myMinute);
    if (myHour < 10) {
      client.print(F("0"));
    }
    client.print(myHour, DEC);
    client.print(F(":"));
    if (myMinute < 10) {
      client.print(F("0"));
    }
    client.println(myMinute, DEC);
}
  
  if ( mode == modeManual){
    client.print(F("manual until "));
    int myHour, myMinute;
        nextInterval(myHour, myMinute);
    if (myHour < 10) {
      client.print(F("0"));
    }
    client.print(myHour, DEC);
    client.print(F(":"));
    if (myMinute < 10) {
      client.print(F("0"));
    }
    client.println(myMinute, DEC);
  }
  if (mode == modeHold ){
    client.print(F("HOLD"));
  }
}


void setup() {
  Wire.begin();
  Serial.begin(9600);

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  digitalWrite(relayPort, HIGH); //to avoid bouncing at startup
  pinMode(relayPort, OUTPUT);

  pinMode(2, OUTPUT);
  pinMode(53, OUTPUT);  //needed for the network card

  setSyncProvider(RTC.get); // get the time from the RTC
  
  // set the initial time for RTC:
  // setTime(hours, minutes, seconds, days, months, years);
//  setTime(23, 42, 0, 4, 4, 2018);
  

  lcd.begin(20, 4);
  if ( prod ) {
    lcd.noBacklight();
  }
  else {
    lcd.backlight();    
  }
  lcd.createChar(0, degree);
  lcd.createChar(1, flame);

  dht.begin();
  dhtExt.begin();

  for (int i = 0; i < 6; i++) {
    buttons[i].attach( BUTTON_PINS[i] , INPUT_PULLUP  );       //setup the bounce instance for the current button
    buttons[i].interval(25);              // interval in ms
  }

  Serial.println(F("Initializing SD card..."));
  if (!SD.begin(chipSelect)) {
    Serial.println(F("SD card initialization failed!"));
    return;
  }
  logInfo("SD card initialization done");

  if (dhcp == 0) {
    Ethernet.begin(mac,ip,dns,gateway,subnet);     // initialize Ethernet device with fixed address
  }
  else {
    Ethernet.begin(mac);  // initialize Ethernet device with DHCP
  }
  server.begin();                                // start to listen for clients
  logInfo("Server started");
  String tempIP = "IP address ";
  dhcp ? tempIP += "(DHCP): " : tempIP += "(static): ";
  tempIP += displayAddress(Ethernet.localIP());
  logInfo(tempIP);
  tempIP = "Subnet mask: ";
  tempIP += displayAddress(Ethernet.subnetMask());
  logInfo(tempIP);
  tempIP = "Gateway: ";
  tempIP += displayAddress(Ethernet.gatewayIP());
  logInfo(tempIP);

  Udp.begin(localPort);  // for NTP

  lightOn();  // light up once at startup
  
  delay(2000); // read DHT sensors, enable in prod
}

void loop() {

  // Reading temperature or humidity takes about 250 milliseconds
  // Sensor readings may also be up to 2 seconds 'old'

  // Read temperature as Celsius (the default)
  temp = dht.readTemperature();
  tempExt = dhtExt.readTemperature();
  
  humidityIn = dht.readHumidity();
  humidityExt = dhtExt.readHumidity();

  char yearMonth [8];
  sprintf (yearMonth, "%04u-%02u", year(), month());
  systemLogFilename = String ("logs/") + yearMonth + String(".log");
  temperatureLogFilename = String ("data/") + yearMonth + String(".csv");

  // regularly write temps to SD card
  unsigned long currentMillis = millis();
  if ( currentMillis - previousMillis >= writeTempInterval ) {
    previousMillis = currentMillis;
    String tempLog = "";
    tempLog += now();
    tempLog += ",";
    tempLog += (temp);
    tempLog += ",";
    tempLog += (tempExt);
    writeSD(tempLog, temperatureLogFilename);
  }
  
//NTP sync once a day
if (day() != dayOld) {
  dayOld = day();
  logInfo("Trying to sync the RTC...");
  sendNTPpacket(timeServer); // send an NTP packet to a time server

  // wait to see if a reply is available
  delay(1000);
  if (Udp.parsePacket()) {
    // We've received a packet, read the data from it
    Udp.read(packetBuffer, NTP_PACKET_SIZE); // read the packet into the buffer

    // the timestamp starts at byte 40 of the received packet and is four bytes,
    // or two words, long. First, extract the two words:

    unsigned long highWord = word(packetBuffer[40], packetBuffer[41]);
    unsigned long lowWord = word(packetBuffer[42], packetBuffer[43]);
    // combine the four bytes (two words) into a long integer
    // this is NTP time (seconds since Jan 1 1900):
    unsigned long secsSince1900 = highWord << 16 | lowWord;

    // Unix time starts on Jan 1 1970. In seconds, that's 2208988800:
    const unsigned long seventyYears = 2208988800UL;
    // subtract seventy years:
    unsigned long epoch = secsSince1900 - seventyYears;
    epoch += (3 * 3600L); // adjust for GMT+3
    epoch -= 1; // adjust for the delay above

    String logNTP = "RTC adjusted by ";
    int difference = epoch-now(); //was unsigned
    logNTP += difference;
    logNTP += (" seconds");
    logInfo (logNTP);
    RTC.set(epoch);

  }
  Ethernet.maintain();
}

  // Check if any reads failed and exit early (to try again).
  if (isnan(humidityIn) || isnan(temp)) {
    logInfo("Failed to read from DHT sensor!");
    return;
  }

  if (isnan(humidityExt) || isnan(tempExt)) {
    logInfo("Failed to read from outside DHT sensor!");
//    return;
  }

  // Compute heat index in Celsius (isFahreheit = false)
  hic = dht.computeHeatIndex(temp, humidityIn, false);
  hicExt = dhtExt.computeHeatIndex(tempExt, humidityExt, false);

  previousMinutes = totalMinutes;
  totalMinutes = hour() * 60 + minute();

  //main code
  switch (mode) {
    case modeAuto:
      compareTemperatureAuto(timeInterval[0], timeInterval[1], tempInterval[0]);
      compareTemperatureAuto(timeInterval[1], timeInterval[2], tempInterval[1]);
      compareTemperatureAuto(timeInterval[2], timeInterval[3], tempInterval[2]);
      compareTemperatureAuto(timeInterval[3], timeInterval[0], tempInterval[3]);
      break;
    case modeManual:
      if (temp <= (tempManual - hysteresis) ) {
        relay = 1;
      }
      else if (temp >= (tempManual + hysteresis) ) {
        relay = 0;
      }
      if ((totalMinutes != previousMinutes) && (
        totalMinutes == timeInterval[0] || totalMinutes == timeInterval[1] ||
          totalMinutes == timeInterval[2] || totalMinutes == timeInterval[3])) {
        mode = modeAuto;
      }
      break;
    case modeHold:
      if (temp <= (tempManual - hysteresis) ) {
        relay = 1;
      }
      else if (temp >= (tempManual + hysteresis) ) {
        relay = 0;
      }
      break;
  }

  //function to display mode & target temperature
  displayMode ();

  getButton();

  switchBack(displayTime, timerInfo, funcDisplayLastMode);
  switchBack(lightTime, timerLight, *noBacklight);

  if (mode == modeManual && tempManual == tempAuto) {
    mode = modeAuto;
  }

  if (mode != modeOld) {
    lcd.clear();
    modeOld = mode;
    switch (mode) {  // log the mode change
      case 0:
        logInfo("Mod AUTO");
        break;
      case 1:
        logInfo("Mod MANUAL");
        break;
      case 2:
        logInfo("Mod HOLD");
        break;
      case 3:
        logInfo("Mod INFO humidity");
        break;
      case 4:
        logInfo("Mod INFO uptime and RAM");
        break;
      case 5:
        logInfo("Mod INFO IP");
        break;
      case 6:
        logInfo("Mod ABOUT");
        break;
    }
  }

  if (relay == 0) {
    digitalWrite(relayPort, HIGH);
  }
  else if (relay == 1) {
    digitalWrite(relayPort, LOW);
  }

  if (relay != relayOld) {  // log the relay change
    relay ? logInfo("Relay ON") : logInfo("Relay OFF");
    relayOld = relay;
  }

  tempManual = constrain(tempManual, tempMin, tempMax);

  EthernetClient client = server.available();    // look for the client
    // webserver
    if (client) {  // got client?
      String response="";
      while (client.connected()) {
          if (client.available()) {
              char c=client.read();
              Serial.write(c);
              if (c=='\n') {
                  // end of the first line
                  break; 
              }
              response+=c;
          }
      }
      //only log server responses if it's not an ajax query
      if ( response.indexOf("ajax_text") < 0 ) {
        logInfo(response);
      }
      
      if (response.startsWith("GET /stat?secret=" + secret)) {
        httpOK(client, "html");  //standard response header
        client.println(F("<h2>List of files:</h2>"));
        listStatFiles(client); 
      }

      else if (response.startsWith("GET /logs?secret=" + secret)) {
        httpOK(client, "html");  //standard response header
        client.println(F("<h2>List of files:</h2>"));
        listLogFiles(client);
      }

      else if (response.indexOf("file=") > 0 && response.indexOf(".LOG") < 0) {
        String filename;
        int slash = response.indexOf('/');
        filename = response.substring(slash+1, slash+7);
      
        File file = SD.open(filename,FILE_READ);
        if (!file) {
          http404(client);
        }
        
        httpOK(client, "html");  //standard response header
                    
        while(file.available()) {
          int num_bytes_read;
          uint8_t byte_buffer[32];

          num_bytes_read=file.read(byte_buffer,32);
          client.write(byte_buffer,num_bytes_read);
        }
        file.close();
      }
      // feed CSV files requested by HC.htm
      else if (response.indexOf("file=") < 0 && response.indexOf(".CSV") > 0) {
        String filename;
        int slash = response.indexOf('/');
        filename = "data/" + response.substring(slash+1, slash+12);
      
        File file = SD.open(filename,FILE_READ);
        if (!file) {
          http404(client);
        }
        
        httpOK(client, "html");  //standard response header
        while(file.available()) {
          int num_bytes_read;
          uint8_t byte_buffer[32];

          num_bytes_read=file.read(byte_buffer,32);
          client.write(byte_buffer,num_bytes_read);
        }
        file.close();
      }
      // feed LOG files
      else if ( response.indexOf(".LOG") > 0) {
        String filename;
        int slash = response.indexOf('/');
        filename = "logs/" + response.substring(slash+1, slash+12);
      
        File file = SD.open(filename,FILE_READ);
        if (!file) {
          http404(client);
        }
        
        httpOK(client, "plain");  //standard response header            
        while(file.available()) {
          int num_bytes_read;
          uint8_t byte_buffer[32];

          num_bytes_read=file.read(byte_buffer,32);
          client.write(byte_buffer,num_bytes_read);
        }
        file.close();
      }

      else if (response.indexOf("ajax_text") > -1) {
        GetTemp(client);
      }

      else if (response.startsWith("GET /crono?secret=" + secret)) {
        httpOK(client, "html");  //standard response header
        client.println(F("<html>"));
        client.println(F("<head>"));
        client.println(F("<title>Cronotermostat</title>"));
        client.println(F("<script>"));

        client.println("function GetTemp() {");
        client.println("nocache = \"&nocache=\"\
                                         + Math.random() * 1000000;");
        client.println("var request = new XMLHttpRequest();");
        client.println("request.onreadystatechange = function() {");
        client.println("if (this.readyState == 4) {");
        client.println("if (this.status == 200) {");
        client.println("if (this.responseText != null) {");
        client.println("document.getElementById(\"ajax_div\")\
.innerHTML = this.responseText;");
        client.println("}}}}");
        client.println(
        "request.open(\"GET\", \"ajax_text\" + nocache, true);");
        client.println("request.send(null);");
        client.println("setTimeout('GetTemp()', 1000);");
        client.println("}");
        client.println("</script>");
        client.println("</head>");
        client.println("<body onload=\"GetTemp()\">");

        //temperature
        client.print(F("<table><tr><td colspan=\"2\"><strong>Temperature:</strong></td></tr><tr><td><strong>In = "));
        client.print(temp,1);
        client.print(F(" &deg;C </strong> &nbsp;&nbsp;&nbsp;&nbsp;"));
        client.print(F("</td><td>Out = "));
        client.print(tempExt,1);
        client.print(F(" &deg;C"));
        client.println(F("</td></tr>"));

        //humidity
        client.print(F("<tr><td colspan=\"2\"><strong>Humidity:</strong></td></tr><tr><td>In: "));
        client.print(humidityIn, 1);
        client.print(F(" %</td><td>Out: "));
        client.print(humidityExt, 0);
        client.println(F(" %</td></tr>"));
        client.print(F("<tr><td colspan=\"2\"><strong>Heat index:</strong></td></tr><tr><td>In: "));
        client.print(hic);
        client.print(F("</td><td>Out: "));
        client.print(hicExt);
        client.println(F("</td></tr><tr><td colspan=\"2\">&nbsp;</td></tr></table>"));

        //mode & next interval
        client.print(F("<div id=\"ajax_div\">Loading...</div>"));

        // buttons
        client.print(F("<br><br><input type=submit value=Plus style=width:80px;height:25px onClick=location.href='/crono?secret="));
        client.print(secret);
        client.println(F("?tempPlus;'> &nbsp;"));
        
        client.print(F("<input type=submit value=Minus style=width:80px;height:25px onClick=location.href='/crono?secret="));
        client.print(secret);
        client.println(F("?tempMinus;'> &nbsp;"));

        client.print(F("<input type=submit value=Reset style=width:80px;height:25px onClick=location.href='/crono?secret="));
        client.print(secret);
        client.println(F("?reset;'> &nbsp;"));

        client.print(F("<input type=submit value=Hold style=width:80px;height:25px onClick=location.href='/crono?secret="));
        client.print(secret);
        client.println(F("?hold;'>"));
        
        client.print(F("<br><br><a href=\"./stat?secret="));
        client.print(secret);
        client.println(F("\" target=_blank>Statistics</a>"));
        
        client.print(F("&nbsp;&nbsp;&nbsp; <a href=\"./logs?secret="));
        client.print(secret);
        client.println(F("\" target=_blank>Logs</a>"));

        
        // program table
        client.print(F("<br><br>Program: <br>"));
        client.print(F("<table><tr><td>"));
        client.print(timeInterval[0] / 60);
        client.print(F(":"));
        if ( timeInterval[0] % 60 < 10 ){
          client.print(F("0"));
        }
        client.print(timeInterval[0] % 60);
        client.print(F("</td><td></td><td>"));
        client.print(timeInterval[1] / 60);
        client.print(F(":"));
        if ( timeInterval[1] % 60 < 10 ){
          client.print(F("0"));
        }
        client.print(timeInterval[1] % 60);
        client.print(F("</td><td></td><td>"));
        client.print(timeInterval[2] / 60);
        client.print(F(":"));
        if ( timeInterval[2] % 60 < 10 ){
          client.print(F("0"));
        }
        client.print(timeInterval[2] % 60);
        client.print(F("</td><td></td><td>"));
        client.print(timeInterval[3] / 60);
        client.print(F(":"));
        if ( timeInterval[3] % 60 < 10 ){
          client.print(F("0"));
        }
        client.print(timeInterval[3] % 60);
        client.print(F("</td><td></td></tr><tr><td></td><td>"));
        client.print(tempInterval[0]);
        client.print(F("&deg;</td><td></td><td>"));
        client.print(tempInterval[1]);
        client.print(F("&deg;</td><td></td><td>"));
        client.print(tempInterval[2]);
        client.print(F("&deg;</td><td></td><td>"));
        client.print(tempInterval[3]);
        client.println(F("&deg;</td></tr></table>"));
        
        // Clock and date
        client.print(F("<br><br>It's "));
        char hourMinute [6];
        sprintf (hourMinute, "%02u:%02u", hour(), minute());
        client.print(hourMinute);
        client.print(F("&nbsp; &nbsp;"));
        char dayMonthYear [11];
        sprintf (dayMonthYear, "%02u.%02u.%04u", day(), month(), year());
        client.print(dayMonthYear);

        // uptime & RAM
        client.print(F("<br><br>Up "));
        client.println(uptime());
        client.print(F("<br><br>RAM "));
        client.print(percentFreeRam());
        client.println(F("% free<br><br>"));


        client.println(F("</body>"));
        client.println(F("</html>"));
      }
      else {
        http404(client);
      }

      delay(1);
      client.stop();

      if(response.indexOf("tempPlus") > 0) { //temp +
        pushTempPlus();
        logInfo("web button 'temp+'");
      }
      
      if(response.indexOf("tempMinus") > 0) { //temp -
        pushTempMinus();
        logInfo("web button 'temp-'");
      }

      if(response.indexOf("reset") > 0) { //reset
        mode = modeAuto;
        tempManual = tempAuto;
        logInfo("web button 'reset'");
      }

      if(response.indexOf("hold") > 0) { //hold
        pushHold();
        logInfo("web button 'hold'");
      }

      response="";  //clearing string for next read
    }
}

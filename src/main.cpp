/*
Matyas Potocky
and big thanks to Juraj Repcik

The ESP-Now is NOT point-to-point and its NOT encrypted. BE AWARE OF THAT.

Before compiling, create config.h (copy and edit config_template.h). 
*/

#include "OTA.h"
#include "config.h"
#include "bitmaps.h"
#include "FastLED.h"
#include "GxEPD2_BW.h"
#include <HTTPClient.h>
#include <Fonts/FreeMonoBold9pt7b.h>
#include <Fonts/FreeMonoBold12pt7b.h>
#include <Fonts/FreeMonoBold18pt7b.h>
#include <Fonts/FreeMono9pt7b.h>
#include "driver/touch_pad.h"
#include <esp_now.h>
#include <WiFi.h>

// array for addressable LEDs control
CRGB leds[4];
// Instantiate the GxEPD2_BW class for our display type

//TODO - choose correct display according to MB version.
#if defined(MakerBadgeVersionA) || defined(MakerBadgeVersionB)
#error "Define correct display type. TODO for version A and B. Did you get MakerBadge at Maker Faire Brno 2022 or later? try MakerBadgeVersionC (in config.h)"
#endif
#if defined(MakerBadgeVersionC) || defined(MakerBadgeVersionD)
GxEPD2_BW<GxEPD2_213_B74, GxEPD2_213_B74::HEIGHT> display(GxEPD2_213_B74(IO_disp_CS, IO_disp_DC, IO_disp_RST, IO_disp_BUSY));  // GDEM0213B74 122x250, SSD1680
#endif
float analogReadBatt();
void enter_sleep(uint16_t TimedWakeUpSec);
uint8_t readTouchPins(void);
uint8_t MakerBadgeSetupWiFi(void);
void MakerBadgeSetupOTA(void);
void DisplayBadge(void);
void CallbackTouch3(void){} //empty function.
void FWloadMode(void);
void ESP_Now(void);
String httpGETRequest(const char* serverName);
struct DispData httpParseReply(String payload);
void DisplayMenu(void);
void DisplayHomeAssistant(void);
void low_battery_shutdown(void);

//Structure for getting data from Home Assistant
struct DispData{
  bool valid = false;
  String RawState = "Empty";
  String LastChangedStr = "Empty";
  //float TamperatureOutside = 0;
  //uint16_t co2 = 0;
};

//States of menu
enum mbStates{Menu, HomeAssistant, Badge, FWupdate, ESP_Now_menu};


uint8_t TouchPins = 0x00;
uint8_t TouchPinsLast = 0x00;
uint16_t BattBar = 0;

//Variable for remembering current mode (mbStates) while in deep sleep. 
//The memory space for this var is in ULP (ultra-low-power coprocessor) that is powered even in deep sleep.
RTC_DATA_ATTR mbStates CurrentMode = Menu; //to store in ULP, kept during deep sleep

void setup() {
  //delay(3000); //uncomment to see first serial logs on USB serial, delay while windows recognizes the USB and conencts to serial
  //Serial.printf("[%d] Start\n",millis());

  //Serial
  Serial.begin(115200);
  //ADC
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  //LEDs
  pinMode(IO_led_disable,OUTPUT);
  digitalWrite(IO_led_disable,HIGH); //disabled for now, functions will enable it when needed
  FastLED.addLeds<WS2812B, IO_led, GRB>(leds, 4);

  //Display
#ifdef MakerBadgeVersionD
  pinMode(IO_EPD_power_disable,OUTPUT);
  digitalWrite(IO_EPD_power_disable,LOW); //enable power to EPD
#endif
  display.init(0); //enter 115200 instead 0 to see debug in console
  display.setRotation(3);
  display.setTextColor(GxEPD_BLACK);

#ifdef MakerBadgeVersionD
  //Battery voltage reading 
  //can be read right after High->Low transition of IO_BAT_meas_disable
  pinMode(IO_BAT_meas_disable,OUTPUT);
  digitalWrite(IO_BAT_meas_disable,HIGH);
#endif

  touch_pad_init(); //deinit is needed when going to sleep. Without deinit - extra 100uA. done in enter sleep function
  //touch wakeup, uncomment if you want to wake up on touch pin.
  //touchAttachInterrupt(IO_touch3,CallbackTouch3,TOUCH_TRESHOLD); //Middle touch input is wake up interrupt.
  //esp_sleep_enable_touchpad_wakeup(); //disabled intentionally - same effect as reset button.

  //Wake up and resume to last mode. If woken up from reset(not timer) - show menu.
  //Serial.printf("CurrentMode is:%d",CurrentMode);
  switch (esp_sleep_get_wakeup_cause()){
    case ESP_SLEEP_WAKEUP_TIMER:
      //restore last used mode.
      switch (CurrentMode){
        case HomeAssistant:
          DisplayHomeAssistant(); //sleeps and periodically updates
          break;
        case Badge:
          DisplayBadge(); //sleep forever
          break;
        case FWupdate:
          FWloadMode(); //forever powered on, shuts down on low battery
          break;
        case ESP_Now_menu:
          ESP_Now(); //forever blocking
          break;
      }
      break;
    case ESP_SLEEP_WAKEUP_TOUCHPAD:
      //falldown
      //define custom action for touch wake up case, and add break;
    default:
      //normal power-up after reset
      DisplayMenu(); //menu to select a mode. Blocking.
      enter_sleep(1); //sleeps for 1s and gets back to switch timer wakeup cause.
  }
} //end setup

void loop() {
  //intentionally empty
}

void DisplayMenu(void){
  digitalWrite(IO_led_disable,LOW);
  leds[0] = CRGB(5,10,0);
  leds[1] = CRGB(5,10,0);
  FastLED.show();
  display.setFont(&FreeMonoBold9pt7b);
  display.setFullWindow();
  display.firstPage();
  display.setTextWrap(true);
  BattBar = ((analogReadBatt()*10-32)*25);
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setCursor(0, 12);
    display.print("   Maker Badge Menu");
    display.drawBitmap(0, 0, smile, 19, 19, GxEPD_BLACK); //POZOR NA 4 A 5 PROMENOU MUSI TAM BYT VELIKOST OBRAZKU
    display.setCursor(0, 39);
    display.printf("   1. Home Assistant\n   2. Badge\n   3. FW update\n   4. ESP-Now");
    display.fillRect(0,20,250,2,GxEPD_BLACK);
    display.fillRect(0,DISP_Y-2,BattBar,2,GxEPD_BLACK);
  } while (display.nextPage());
  uint8_t flipLED = 1;
  uint32_t lastMillis = 0;
  uint16_t timeout = 100; //*0.6s = 600s = 10min
  while(1){
    delay(150);
    switch(readTouchPins()){
    //switch(1){
      case 0b00001: //key 1 (left)
        CurrentMode = HomeAssistant;
        return;
      case 0b00010: //2
        CurrentMode = Badge;
        return;
      case 0b00100: //3
        CurrentMode = FWupdate;
        return;
      case 0b01000: //4
        CurrentMode = ESP_Now_menu;
        return;
      //case 0b10000: //5
        //return;
      default:
        CurrentMode = Menu;
        break;
    }
    if ((lastMillis+600) < millis()){
      leds[  flipLED & 0x01   ] = CRGB(5,10,0);
      leds[!(flipLED++ & 0x01)] = CRGB(0,0,0);
      FastLED.show();
      lastMillis = millis();
      if (0 == timeout--)
        enter_sleep(0);
    }
  }
}

void DisplayHomeAssistant(void){
  //LEDs disabled
  DispData ActualDispData;

  if(MakerBadgeSetupWiFi()){
    enter_sleep(HA_UPDATE_PERIOD_SEC); //on fail
  }
  //No OTA is set up to save energy, FWupdate mode is for OTA
  display.setFont(&FreeMonoBold9pt7b);
  
  BattBar = ((analogReadBatt()*10-32)*25);
  //Serial.printf("Batt: %.3f, Bar:%d\n",analogReadBatt(),BattBar);
  //Serial.println(httpGETRequest("http://192.168.1.14:8123/api/")); //test of HA - should get API RUNNING
  ActualDispData = httpParseReply(httpGETRequest(HAreqURL));
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setCursor(0, 12);
    display.print("    Home Assistant");
    display.setFont(&FreeMonoBold12pt7b);
    display.fillRect(0,18,250,1,GxEPD_BLACK); //line below Home Assistant heading
    display.fillRect(0,DISP_Y-2,BattBar,2,GxEPD_BLACK);
    display.setCursor(0, 39);
    display.print(ActualDispData.RawState);
#if SHOW_LAST_UPDATE
    display.setFont(NULL); // default 5x7 system font?
    display.setCursor(23, DISP_Y-10);
    display.print(ActualDispData.LastChangedStr);
#endif
  } while (display.nextPage());

  enter_sleep(HA_UPDATE_PERIOD_SEC);
}

void DisplayBadge(void){
  digitalWrite(IO_led_disable,LOW);
  leds[0] = CRGB(00,10,0);
  FastLED.show();
  display.setFont(&FreeMonoBold18pt7b);
  display.setFullWindow();
  display.firstPage();
  display.setTextWrap(false);
  BattBar = ((analogReadBatt()*10-32)*25);
  do {
    display.fillScreen(GxEPD_WHITE);
    display.drawBitmap(150, -15, maker, 150, 150, GxEPD_BLACK); //WATCH OUT FOR 4 AND 5 LINES THERE MUST BE A PICTURE SIZE
    display.drawBitmap(0, 80, insta, 25, 25, GxEPD_BLACK); //WATCH OUT FOR 4 AND 5 LINES THERE MUST BE A PICTURE SIZE
    display.setFont(&FreeMonoBold18pt7b);
    display.setCursor(0, 40);
    display.print(BadgeName);
    //display.drawLine(226,7,226-6,7+6,GxEPD_BLACK);
    display.setFont(&FreeMonoBold12pt7b);
    display.setCursor(0, 70);
    display.print(BadgeLine2);
    display.setFont(&FreeMono9pt7b);
    display.setCursor(25, 100);
    display.print(BadgeLine3);
    display.fillRect(0,DISP_Y-2,BattBar,2,GxEPD_BLACK);
  } while (display.nextPage());
  digitalWrite(IO_led_disable,HIGH);
  display.powerOff();
  enter_sleep(0);
}

void FWloadMode(void){
  digitalWrite(IO_led_disable,LOW);
  leds[0] = CRGB(0,0,50); //Blue, connecting
  FastLED.show();
  MakerBadgeSetupOTA();
  display.setFont(&FreeMonoBold18pt7b);
  display.setFullWindow();
  display.firstPage();
  display.setTextWrap(false);
  BattBar = ((analogReadBatt()*10-32)*25);
  uint8_t mac[6];
  WiFi.macAddress(mac);
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setCursor(0, 30);
    display.print(" FW update");
    display.setFont(&FreeMonoBold9pt7b);
    display.setCursor(29, 60);
    display.print(WiFi.localIP().toString());
    display.setCursor(0, 80);
    display.printf("MakerBadge-%02x%02x%02x.local",mac[3], mac[4], mac[5]);
    display.setCursor(50, 106);
    display.printf("Batt %.2f V",analogReadBatt());
    display.fillRect(0,DISP_Y-2,BattBar,2,GxEPD_BLACK);
  } while (display.nextPage());
  uint8_t ledrotate = 0;
  while(1){
    if (WiFi.isConnected())
      leds[ledrotate++] = CRGB(20,20,0);
    else  
      leds[ledrotate++] = CRGB(20,0,0);
    FastLED.show();
    delay(600);
    FastLED.clear(true);
    if (ledrotate == 4) ledrotate = 0;
    analogReadBatt(); //for low voltage shutdown
  }
}

//ESP-Now starts here

void formatMacAddress(const uint8_t *macAddr, char *buffer, int maxLength)
// Formats MAC Address
{
  snprintf(buffer, maxLength, "%02x:%02x:%02x:%02x:%02x:%02x", macAddr[0], macAddr[1], macAddr[2], macAddr[3], macAddr[4], macAddr[5]);
}
 
 
void receiveCallback(const uint8_t *macAddr, const uint8_t *data, int dataLen)
// Called when data is received
{
  // Only allow a maximum of 250 characters in the message + a null terminating byte
  char buffer[ESP_NOW_MAX_DATA_LEN + 1];
  int msgLen = min(ESP_NOW_MAX_DATA_LEN, dataLen);
  strncpy(buffer, (const char *)data, msgLen);
 
  // Make sure we are null terminated
  buffer[msgLen] = 0;
 
  // Format the MAC address
  char macStr[18];
  formatMacAddress(macAddr, macStr, 18);
 
  // Send Debug log message to the serial port
  Serial.printf("Received message from: %s - %s\n", macStr, buffer);

  String hexstring = buffer;
  int number = (int) strtol( &hexstring[1], NULL, 16);
  int r = number >> 16;
  int g = number >> 8 & 0xFF;
  int b = number & 0xFF;

  if (serialdbg == 1){
  Serial.print("red is ");
  Serial.println(r);
  Serial.print("green is ");
  Serial.println(g);
  Serial.print("blue is ");
  Serial.println(b);
  }

  leds[0] = CRGB(r,g,b);
  FastLED.show();
}

void sentCallback(const uint8_t *macAddr, esp_now_send_status_t status)
// Called when data is sent
{
  char macStr[18];
  formatMacAddress(macAddr, macStr, 18);
  Serial.print("Last Packet Sent to: ");
  Serial.println(macStr);
  Serial.print("Last Packet Send Status: ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Delivery Success" : "Delivery Fail");
}
 
void broadcast(const String &message)
// Emulates a broadcast
{
  // Broadcast a message to every device in range
  uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  esp_now_peer_info_t peerInfo = {};
  memcpy(&peerInfo.peer_addr, broadcastAddress, 6);
  if (!esp_now_is_peer_exist(broadcastAddress))
  {
    esp_now_add_peer(&peerInfo);
  }
  // Send message
  esp_err_t result = esp_now_send(broadcastAddress, (const uint8_t *)message.c_str(), message.length());
 
  // Print results to serial monitor
  if (result == ESP_OK)
  {
    Serial.println("Broadcast message success");
  }
  else if (result == ESP_ERR_ESPNOW_NOT_INIT)
  {
    Serial.println("ESP-NOW not Init.");
  }
  else if (result == ESP_ERR_ESPNOW_ARG)
  {
    Serial.println("Invalid Argument");
  }
  else if (result == ESP_ERR_ESPNOW_INTERNAL)
  {
    Serial.println("Internal Error");
  }
  else if (result == ESP_ERR_ESPNOW_NO_MEM)
  {
    Serial.println("ESP_ERR_ESPNOW_NO_MEM");
  }
  else if (result == ESP_ERR_ESPNOW_NOT_FOUND)
  {
    Serial.println("Peer not found.");
  }
  else
  {
    Serial.println("Unknown error");
  }
}

void ESP_Now(void){
  digitalWrite(IO_led_disable,LOW);
  leds[0] = CRGB(0,0,50); //Blue, preparing
  FastLED.show();

  // Display
  display.setFont(&FreeMonoBold18pt7b);
  display.setFullWindow();
  display.firstPage();
  display.setTextWrap(false);
  BattBar = ((analogReadBatt()*10-32)*25);
  do {
    display.fillScreen(GxEPD_WHITE);
    display.drawBitmap(25, 0, esp_now, 200, 50, GxEPD_BLACK); //POZOR NA 4 A 5 PROMENOU MUSI TAM BYT VELIKOST OBRAZKU
    display.drawBitmap(50, 55, espressif, 150, 26, GxEPD_BLACK); //POZOR NA 4 A 5 PROMENOU MUSI TAM BYT VELIKOST OBRAZKU
    display.setFont(&FreeMono9pt7b);
    display.setCursor(2, 110);
    display.printf("Batt %.2f V",analogReadBatt());
    display.fillRect(0,DISP_Y-2,BattBar,2,GxEPD_BLACK);
  } while (display.nextPage());

  // Set ESP32 in STA mode to begin with
  WiFi.mode(WIFI_STA);
  Serial.println("ESP-NOW Broadcast Demo");
 
  // Print MAC address
  Serial.print("MAC Address: ");
  Serial.println(WiFi.macAddress());
 
  // Disconnect from WiFi
  WiFi.disconnect();
 
  // Initialize ESP-NOW
  if (esp_now_init() == ESP_OK)
  {
    Serial.println("ESP-NOW Init Success");
    esp_now_register_recv_cb(receiveCallback);
    esp_now_register_send_cb(sentCallback);
    leds[0] = CRGB(0,50,0); //Green, succeed
    FastLED.show();
    delay(500);
    leds[0] = CRGB(0,0,0); //Off, finished setup
    FastLED.show();
  }
  else
  {
    Serial.println("ESP-NOW Init Failed");
    leds[0] = CRGB(50,0,0); //Red, failed
    FastLED.show();
    delay(3000);
    ESP.restart();
  }

  // Loop
  while(1){
  String hexstring = LED1;
  int number = (int) strtol( &hexstring[1], NULL, 16);
  int r = number >> 16;
  int g = number >> 8 & 0xFF;
  int b = number & 0xFF;

  String hexstring2 = LED2;
  int number2 = (int) strtol( &hexstring2[1], NULL, 16);
  int rr = number2 >> 16;
  int gg = number2 >> 8 & 0xFF;
  int bb = number2 & 0xFF;

  String hexstring3 = LED3;
  int number3 = (int) strtol( &hexstring3[1], NULL, 16);
  int rrr = number3 >> 16;
  int ggg = number3 >> 8 & 0xFF;
  int bbb = number3 & 0xFF;

  String hexstring4 = LED4;
  int number4 = (int) strtol( &hexstring4[1], NULL, 16);
  int rrrr = number4 >> 16;
  int gggg = number4 >> 8 & 0xFF;
  int bbbb = number4 & 0xFF;

  String hexstring5 = LED5;
  int number5 = (int) strtol( &hexstring5[1], NULL, 16);
  int rrrrr = number5 >> 16;
  int ggggg = number5 >> 8 & 0xFF;
  int bbbbb = number5 & 0xFF;

    delay(150);
    // Transmit
    switch(readTouchPins()){
      case 0b00001: //1. Button
        Serial.println("1. BTN");
        broadcast(LED1);
        leds[0] = CRGB(r,g,b); // LED1
        FastLED.show();
        break;
      case 0b00010: //2. Button
        Serial.println("2. BTN");
        broadcast(LED2);
        leds[0] = CRGB(rr,gg,bb); //LED2
        FastLED.show();
        break;
      case 0b00100: //3. Button
        Serial.println("3. BTN");
        broadcast(LED3);
        leds[0] = CRGB(rrr,ggg,bbb); //LED3
        FastLED.show();
        break;
      case 0b01000: //4. Button
        Serial.println("4. BTN");
        broadcast(LED4);
        leds[0] = CRGB(rrrr,gggg,bbbb); //LED4
        FastLED.show();
        break;
      case 0b10000: //5. Button
        Serial.println("4. BTN");
        broadcast(LED5);
        leds[0] = CRGB(rrrrr,ggggg,bbbbb); //LED5
        FastLED.show();
        break;
      default:
        if (serialdbg == 1){
        Serial.println(" Do nothing, only repeat switch 4 read BTN");
        }
        break;  
      Serial.println(" BAF!"); // Never ever :) Pocuvaj, henty text nikedy neuvidis.
    }
    // Recive
    if (serialdbg == 1){
    Serial.println("repeat while and wait 4 push BTN or RECEIVE");
    }
  }
  Serial.println("vyskocil????");
}
//---------------------------------
float analogReadBatt(){
#ifdef MakerBadgeVersionD
  digitalWrite(IO_BAT_meas_disable,LOW);
  delayMicroseconds(150);
#endif
  uint16_t batt_adc = analogRead(AIN_batt);
#ifdef MakerBadgeVersionD
  digitalWrite(IO_BAT_meas_disable,HIGH);
#endif
  float battv = (BATT_V_CAL_SCALE*2.0*(2.50*batt_adc/4096));
  Serial.printf("Battv: %fV, Bat w/ calibration %fV, raw ADC %d\n",battv/BATT_V_CAL_SCALE,battv,batt_adc);
  if(battv<3.45){
    Serial.printf("Bat %fV, shutting down...\n",battv,batt_adc);
    //ESP_LOGE("MakerBadge","Batt %f V",battv); //log to HW UART
    low_battery_shutdown();
  }
  return battv; //volts float
}

//shutdown the system to protect battery
void low_battery_shutdown(void){
  //TODO display message on display
  enter_sleep(0);
}

//when 0, enters sleep without timed wakeup - sleeps forever.
void enter_sleep(uint16_t TimedWakeUpSec){
  touch_pad_deinit();
  digitalWrite(IO_led_disable,HIGH);
#ifdef MakerBadgeVersionD
  digitalWrite(IO_BAT_meas_disable,HIGH);
  digitalWrite(IO_EPD_power_disable,HIGH);
#endif
  WiFi.mode(WIFI_OFF);
  WiFi.disconnect(true,false);
  while(WiFi.isConnected()){} //wait on disconnect
  if (TimedWakeUpSec != 0){
    esp_sleep_enable_timer_wakeup(TimedWakeUpSec*1000000);
  }
  esp_deep_sleep_start();
}

/**
 * @brief Reads touch pins
 * 
 * @return uint8_t 0bxx54321 where 54321 are touch inputs. 1 is the one on the left.
 */
uint8_t readTouchPins() {
  uint8_t TouchResultMask = 0x00;
  for (int i = 0; i < 5; i++) {
    // (5 - i) because pins order is reversed
    if (touchRead(5 - i) > TOUCH_TRESHOLD ) {
      //Serial.printf("Captouch #%0d reading: %d\n", i, touchread);
      TouchResultMask |= 1<<i;
    }
    //Serial.printf("CT#%0d: %d//", i, touchread);
  }
  //Serial.printf("out:%x\n",TouchResultMask);
  return TouchResultMask;
}

uint8_t MakerBadgeSetupWiFi(void){
  if(setupWiFi("MakerBadge", mySSID, myPASSWORD)){
    digitalWrite(IO_led_disable,LOW);
    Serial.println("Connection failed");
    leds[0] = CRGB(100,0,0);
    FastLED.show();
    delay(500);
    return 1;
  } else {
    return 0;
  }
}

void MakerBadgeSetupOTA(void){
  if(0==MakerBadgeSetupWiFi()){
    setupOTA();
    leds[0] = CRGB(0,10,0);// CRGB::Green;
    FastLED.show();
  }

  Serial.println("OTA Initialized");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
  Serial.print("Hostname: ");
  Serial.println(ArduinoOTA.getHostname());
}

String httpGETRequest(const char* serverName) {
  WiFiClient client;
  HTTPClient http;
    
  // Your Domain name with URL path or IP address with path
  http.begin(client, serverName);
  
  // If you need Node-RED/server authentication, insert user and password below
  //http.setAuthorization("REPLACE_WITH_SERVER_USERNAME", "REPLACE_WITH_SERVER_PASSWORD");
  http.addHeader("Authorization",HAtoken); //authorization for home assistant

  // Send HTTP POST request
  int httpResponseCode = http.GET();
  
  String payload = "{}"; 
  
  if (httpResponseCode>0) {
    //Serial.print("HTTP Response code: ");
    //Serial.println(httpResponseCode);
    payload = http.getString();
  }
  else {
    Serial.print("Error code: ");
    Serial.println(httpResponseCode);
  }
  // Free resources
  http.end();

  return payload;
}

struct DispData httpParseReply(String payload){
  //find and parse: "state":"lalalalalalalla1234",
  //Serial.print("HTML GET REPLY:");
  //Serial.println(payload);

  DispData ActualDispData;

  int SearchIndex = payload.indexOf("\"state\":\"");
  if (SearchIndex == -1){
    ActualDispData.valid = false;
    ActualDispData.RawState = "not valid";
    return ActualDispData;
  }
  int SearchIndexEnd = payload.indexOf("\"",SearchIndex+10); //+10 for skipping "state":" string
  
  //Serial.printf("indexes %d - %d\n",SearchIndex,SearchIndexEnd);
  ActualDispData.valid = true;
  ActualDispData.RawState = payload.substring(SearchIndex+9,SearchIndexEnd);
  ActualDispData.RawState.replace("\\n","\n"); //home assistant cannot send new line. sends \n in text instead. Replace by real new line here.
  //Serial.print("ISOLATED STATE:");
  //Serial.println(StateStr);

#if SHOW_LAST_UPDATE
  SearchIndex = payload.indexOf("\"last_changed\":\"");
  if (SearchIndex != -1){
    SearchIndexEnd = payload.indexOf("\"",SearchIndex+20);
    ActualDispData.LastChangedStr = payload.substring(SearchIndex+16,SearchIndexEnd);
  }
#endif
  return ActualDispData;
}


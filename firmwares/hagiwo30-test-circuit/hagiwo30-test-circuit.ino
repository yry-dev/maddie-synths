
//Encoder setting
#define  ENCODER_OPTIMIZE_INTERRUPTS //countermeasure of encoder noise
#include <Encoder.h>
#include <Hagiwo30Common.h>

//Oled setting
#include<Wire.h>
#include<Adafruit_GFX.h>
#include<Adafruit_SSD1306.h>

Adafruit_SSD1306 display(hagiwo30::kScreenWidth, hagiwo30::kScreenHeight, &Wire, -1);

//rotery encoder
Encoder myEnc(hagiwo30::kEncoderPinA, hagiwo30::kEncoderPinB);//use 3pin 2pin
int oldPosition  = -999;
int newPosition = -999;
int i = 0;

//push button
bool sw = 0;//push button
hagiwo30::DebouncedActiveLowButton buttonDebounce(300, HIGH);

bool disp_reflesh = 1;//0=not reflesh display , 1= reflesh display , countermeasure of display reflesh bussy
int clk_val = 0;

void setup() {
 // OLED setting
 display.begin(SSD1306_SWITCHCAPVCC, hagiwo30::kOledAddress);
 display.setTextSize(1);
 display.setTextColor(WHITE);
 display.clearDisplay();
 display.setCursor(50, 50);
 display.print("Hello");
 display.display();
 //pin mode setting
 pinMode(hagiwo30::kEncoderSwitchPin, INPUT_PULLUP); //BUTTON
 pinMode(hagiwo30::kClockPin, INPUT);
 pinMode(hagiwo30::kOutCh1, OUTPUT); //CH1
 pinMode(hagiwo30::kOutCh2, OUTPUT); //CH2
 pinMode(hagiwo30::kOutCh3, OUTPUT); //CH3
 pinMode(hagiwo30::kOutCh4, OUTPUT); //CH4
 pinMode(hagiwo30::kOutCh5, OUTPUT); //CH5
 pinMode(hagiwo30::kOutCh6, OUTPUT); //CH6
 Serial.begin(115200);
}


void loop() {
 oldPosition = newPosition;
 //-----------------Rotery encoder read----------------------
 newPosition = myEnc.read()/hagiwo30::kEncoderCountsPerRotation;

 if ( newPosition   < oldPosition ) {//turn left
   Serial.println("left");
   oldPosition = newPosition;
 }
 if (digitalRead(hagiwo30::kClockPin)!= clk_val){
  clk_val = digitalRead(hagiwo30::kClockPin);
  Serial.println("CLOCK");
 }
 
 else if ( newPosition    > oldPosition ) {//turn right
   Serial.println("right");
 }
 sw = 1;
 buttonDebounce.update((uint8_t)digitalRead(hagiwo30::kEncoderSwitchPin), millis());
 if (buttonDebounce.fell()) {
   Serial.println("click");
   sw = 0;
 }
 if(millis()%1000 < 500){
  digitalWrite(hagiwo30::kOutCh1,HIGH);
  digitalWrite(hagiwo30::kOutCh2,HIGH);
  digitalWrite(hagiwo30::kOutCh3,HIGH);
  digitalWrite(hagiwo30::kOutCh4,HIGH);
  digitalWrite(hagiwo30::kOutCh5,HIGH);
  digitalWrite(hagiwo30::kOutCh6,HIGH);
 }
 else{
  digitalWrite(hagiwo30::kOutCh1,LOW);
  digitalWrite(hagiwo30::kOutCh2,LOW);
  digitalWrite(hagiwo30::kOutCh3,LOW);
  digitalWrite(hagiwo30::kOutCh4,LOW);
  digitalWrite(hagiwo30::kOutCh5,LOW);
  digitalWrite(hagiwo30::kOutCh6,LOW);
 }
}
 

 

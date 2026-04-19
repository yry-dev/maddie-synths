
//Encoder setting
#define  ENCODER_OPTIMIZE_INTERRUPTS //countermeasure of encoder noise
#include <Encoder.h>
#include <TestbildCommon.h>

//Oled setting
#include<Wire.h>
#include<Adafruit_GFX.h>
#include<Adafruit_SSD1306.h>
#include <digitalWriteFast.h>

Adafruit_SSD1306 display(testbild::kScreenWidth, testbild::kScreenHeight, &Wire, -1);

//rotery encoder
Encoder myEnc(testbild::kEncoderPinA, testbild::kEncoderPinB);//use 3pin 2pin
int oldPosition  = -999;
int newPosition = -999;
int i = 0;

//push button
bool sw = 0;//push button
testbild::DebouncedActiveLowButton buttonDebounce(300, HIGH);

bool disp_reflesh = 1;//0=not reflesh display , 1= reflesh display , countermeasure of display reflesh bussy
int clk_val = 0;

void setup() {
 // OLED setting
 display.begin(SSD1306_SWITCHCAPVCC, testbild::kOledAddress);
 display.setTextSize(1);
 display.setTextColor(WHITE);
 display.clearDisplay();
 display.setCursor(50, 50);
 display.print("Hello");
 display.display();
 //pin mode setting
 pinMode(testbild::kEncoderSwitchPin, INPUT_PULLUP); //BUTTON
 pinModeFast(testbild::kClockPin, INPUT);
 pinModeFast(testbild::kOutCh1, OUTPUT); //CH1
 pinModeFast(testbild::kOutCh2, OUTPUT); //CH2
 pinModeFast(testbild::kOutCh3, OUTPUT); //CH3
 pinModeFast(testbild::kOutCh4, OUTPUT); //CH4
 pinModeFast(testbild::kOutCh5, OUTPUT); //CH5
 pinModeFast(testbild::kOutCh6, OUTPUT); //CH6
 Serial.begin(115200);
}


void loop() {
 oldPosition = newPosition;
 //-----------------Rotery encoder read----------------------
 newPosition = myEnc.read()/testbild::kEncoderCountsPerRotation;

 if ( newPosition   < oldPosition ) {//turn left
   Serial.println("left");
   oldPosition = newPosition;
 }
 if (digitalReadFast(testbild::kClockPin)!= clk_val){
  clk_val = digitalReadFast(testbild::kClockPin);
  Serial.println("CLOCK");
 }
 
 else if ( newPosition    > oldPosition ) {//turn right
   Serial.println("right");
 }
 sw = 1;
 buttonDebounce.update((uint8_t)digitalRead(testbild::kEncoderSwitchPin), millis());
 if (buttonDebounce.fell()) {
   Serial.println("click");
   sw = 0;
 }
 if(millis()%1000 < 500){
  digitalWriteFast(testbild::kOutCh1,HIGH);
  digitalWriteFast(testbild::kOutCh2,HIGH);
  digitalWriteFast(testbild::kOutCh3,HIGH);
  digitalWriteFast(testbild::kOutCh4,HIGH);
  digitalWriteFast(testbild::kOutCh5,HIGH);
  digitalWriteFast(testbild::kOutCh6,HIGH);
 }
 else{
  digitalWriteFast(testbild::kOutCh1,LOW);
  digitalWriteFast(testbild::kOutCh2,LOW);
  digitalWriteFast(testbild::kOutCh3,LOW);
  digitalWriteFast(testbild::kOutCh4,LOW);
  digitalWriteFast(testbild::kOutCh5,LOW);
  digitalWriteFast(testbild::kOutCh6,LOW);
 }
}
 

 

#include <BLEDevice.h> 
#define ADDRESS "ff:ff:10:a8:9a:84"
#define RELAY_PIN 12 
#define SCAN_INTERVAL 100 
#define TARGET_RSSI -80 
#define MAX_MISSING_TIME 10000

int stater = 27; 
int buzzer = 14 ;
int indikator = 13;
int saklar = 26;

BLEScan* pBLEScan; 
uint32_t lastScanTime = 0;
boolean found = false;
uint32_t lastFoundTime = 0;
int rssi = 0;
int status_kontak = 0;
int laststatus1 = 0;
int laststatus = 0;
int readstater = 0;
int readsaklar = 0;
int status_indikator = 0;

class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks 
{ 
    void onResult(BLEAdvertisedDevice advertisedDevice) 
    { 
        if(advertisedDevice.getAddress().toString() == ADDRESS) 
        {  
            found = true; 
            advertisedDevice.getScan()->stop(); 
            rssi = advertisedDevice.getRSSI();
        } else {
           found=false;
        }
    } 
};

void setup() 
{ 
    Serial.begin(115200); 
  
    pinMode(RELAY_PIN, OUTPUT);
    pinMode(buzzer, OUTPUT); 
    pinMode(indikator, OUTPUT); 
    pinMode(saklar, INPUT_PULLUP); 
    pinMode(stater, INPUT_PULLUP);
    digitalWrite(buzzer, LOW); 
    digitalWrite(RELAY_PIN, LOW);

    BLEDevice::init(""); 
    pBLEScan = BLEDevice::getScan(); 
    pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks()); 
    pBLEScan->setActiveScan(true); 
    
}

void loop()
{   
  status_indikator = digitalRead(indikator);
  status_kontak = digitalRead(RELAY_PIN);
  readstater = digitalRead(stater);
  readsaklar = digitalRead(saklar);
    uint32_t now = millis(); 
 
    if(found){
        lastFoundTime = millis(); 
        found = false;
        if(rssi > TARGET_RSSI){ 
          if(readstater == LOW){
            digitalWrite(indikator, HIGH);
            Serial.println("kontak on");
            if(readsaklar == LOW) {
              if(readstater == LOW) {
                digitalWrite(RELAY_PIN, HIGH); 
                Serial.println("kontak on"); 
            }
          } 
            }
        }
      } 

     else if(now - lastFoundTime > MAX_MISSING_TIME){
      digitalWrite(indikator, LOW);
        Serial.println("starter off");
    } 
if (readsaklar == HIGH){
            digitalWrite(RELAY_PIN, LOW);
            Serial.println("kontak off");    
          }
    if(now - lastScanTime > SCAN_INTERVAL){ 
        lastScanTime = now;
        pBLEScan->start(1);
    } 
    buzzer_high();
    buzzer_low();
  }

void buzzer_high() {
    if (status_indikator != laststatus) {
    if (status_indikator == HIGH){
      digitalWrite(buzzer, HIGH);
      delay (300);
      digitalWrite(buzzer, LOW);
      delay (200);
      digitalWrite(buzzer, HIGH);
      delay (300);
      digitalWrite(buzzer, LOW);
      delay (200);
      digitalWrite(buzzer, HIGH);
      delay (300);
      digitalWrite(buzzer, LOW);
      delay (200);
    }
  }
  laststatus = status_indikator;
}

void buzzer_low(){
    if (status_indikator != laststatus1) {
    if (status_indikator == LOW) {
      digitalWrite(buzzer, HIGH);
      delay (500);
      digitalWrite(buzzer, LOW);
      delay (200);
    }
  }
  laststatus1 = status_indikator; 
}
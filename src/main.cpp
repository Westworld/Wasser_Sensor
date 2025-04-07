#include "Arduino.h"
#include <Wire.h> 
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <ESP8266WebServer.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <DallasTemperature.h>
#include "Bounce2.h"
#include "EEPROM.h"
#include <PubSubClient.h>
#include <ESP_Mail_Client.h>

#include "main.h"

#define UDPDEBUG 1
#ifdef UDPDEBUG
WiFiUDP udp;
const char * udpAddress = "192.168.0.63";
const int udpPort = 19814;
#endif

WiFiClient wifiClient;

#define NTP_SERVER "de.pool.ntp.org"
#define DefaultTimeZone "CET-1CEST,M3.5.0/02,M10.5.0/03"  
String MY_TZ = DefaultTimeZone ;

const char* mqtt_server = "192.168.0.63";
// MQTT_User and MQTT_Pass defined via platform.ini, external file, not uploaded to github
PubSubClient mqttclient(wifiClient);

struct tm timeinfo;
char time_last_restart_day = -1;
char SDLog_Lastday = -1;


#define SMTP_HOST "smtp.gmail.com"
#define SMTP_PORT 465
// The SMTP Session object used for Email sending 
SMTPSession smtp;
// Callback function to get the Email sending status 
void smtpCallback(SMTP_Status status);


const char* host = "192.168.0.46";  // war 34
const int httpPort = 80;  // war 8000

const char * wifihostname = "Wemos_Wasser";

#define sensor D5
#define sensorled D1
#define blinkled D2
#define temppin D6

#define EEPROM_SIZE 9


Bounce debouncer = Bounce();  
int laststatus = 0;
unsigned int blinker=0;
int ledon;
long wassercounter=0, wassercounterday=0;
unsigned int lastwasservalue=0;
unsigned int temptimer=0;
float tempvalue=127;
float lasttemp=127;
int tempcounter=0;
unsigned long wasserstarted=0;
int8_t wasseralarm = 0;  
int8_t heizungTempAlarm = 0; 
#define wasserAlert 400000  // 500000

char logString[200];

OneWire oneWire(temppin); 
DallasTemperature sensors(&oneWire);

WiFiServer server(80);

void setup() {
  Serial.begin(115200);
  Serial.println("Start"); 
  pinMode(sensor, INPUT);
  pinMode(sensorled, OUTPUT);
  pinMode(blinkled, OUTPUT);

  debouncer.attach(sensor);
  debouncer.interval(50); // interval in ms

  digitalWrite(sensorled,0);
  digitalWrite(blinkled,0);

  WIFI_Connect();
  
  blinker=millis();

  ArduinoOTA.setHostname(wifihostname);
  
  ArduinoOTA
    .onStart([]() {
       Serial.println("Start updating");
    });
    ArduinoOTA.onEnd([]() {
       Serial.println("End updating");
    });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
      sprintf(logString, "Progress: %u", (progress / (total / 100)));
      Serial.println(logString);
    });
    ArduinoOTA.onError([](ota_error_t error) {
      sprintf(logString, "Error[%u]: ", error);
      Serial.println(logString);
      if (error == OTA_AUTH_ERROR)  Serial.println("Auth Failed");
      else if (error == OTA_BEGIN_ERROR)  Serial.println("Begin Failed");
      else if (error == OTA_CONNECT_ERROR)  Serial.println("Connect Failed");
      else if (error == OTA_RECEIVE_ERROR)  Serial.println("Receive Failed");
      else if (error == OTA_END_ERROR)  Serial.println("End Failed");
    });

  ArduinoOTA.begin();  

  long rssi = WiFi.RSSI();
  sprintf(logString, " %ld", rssi);
  Serial.println(WiFi.localIP());  
  Serial.println(logString);

   mqttclient.setServer(mqtt_server, 1883);
   // mqttclient.setCallback(MQTT_callback);
   //Serial.printf("nach MQTT1");
   if (mqttclient.connect(wifihostname, MQTT_User, MQTT_Pass)) {
      //mqttclient.publish("outTopic","hello world");
      UDBDebug("MQTT connect successful"); 
      //mqttclient.subscribe("garage/TargetDoorState");
      //const char *TOPIC = "Haus/Sonoff POW R1/R2/Dreamer_Power/#";
      //const char *TOPIC = "garage/TargetDoorState/#";
      //mqttclient.subscribe(TOPIC);
      //mqttclient.subscribe("Haus/+/Power");
   }  
    else
       UDBDebug("MQTT connect error");  

  Flash_Read();

  if(!mygetLocalTime(&timeinfo, 1000)) {
    UDBDebug("Failed to obtain time");
  }
  else
  {
     SDLog_Lastday = timeinfo.tm_mday;
  }

  server.begin();
  sensors.begin();

  EMail_Send("HomeServer/Heizung/WasserStart");
}



//  ####################################################################

void loop() {
   if (WiFi.status() != WL_CONNECTED)
    {
      WIFI_Connect();
    }
    if (!mqttclient.loop()) {
      if (mqttclient.connect(wifihostname, MQTT_User, MQTT_Pass)) {
        UDBDebug("MQTT reconnect successful"); 
    }  
    else
    {
       UDBDebug("MQTT reconnect error");  
       delay(5000);
    }   
  };

    
  ArduinoOTA.handle();

  // einmal am Tag daycounter zurücksetzen
  if(!mygetLocalTime(&timeinfo, 1000)) {
    UDBDebug("Failed to obtain time");
  }
  else
  {
      if (timeinfo.tm_mday != SDLog_Lastday)  {
        SDLog_Lastday = timeinfo.tm_mday;
        wassercounterday = 0;
        heizungTempAlarm = 0;
        wasseralarm = 0;
        lastwasservalue = millis();
      } 
  }

  unsigned long zeit = millis();

  if ((blinker+2000)<zeit)
  {
    blinker=zeit;      
    if (ledon) {
      ledon=false;
      digitalWrite(blinkled, LOW);
    } else {
      ledon=true;
      digitalWrite(blinkled, HIGH);
      blinker=blinker+1500; 
    }
  }
  
  debouncer.update();

  int thestatus = debouncer.read();
  if (thestatus != laststatus) 
  { 
    Serial.println(thestatus);
    laststatus = thestatus;
    if (thestatus == 1)
    {
        digitalWrite(sensorled,1);
        wassercounter++;
        wassercounterday++;
        lastwasservalue = zeit;
        if (wasserstarted == 0)
          wasserstarted = zeit;
        SendeStatus();
        digitalWrite(sensorled,0);
    }
    else {
        digitalWrite(sensorled,0);
    }
   }

  if ((lastwasservalue != 0) && ((lastwasservalue+20000)<zeit) )
  {
    // 20 Sekunden ohne wasserverbrauch -> sichere aktuellen Stand
    Flash_Write();
    lastwasservalue = 0;
    wasseralarm = 0;
    wasserstarted = 0;
  }

  if ((temptimer+60000)<zeit)
  {
    temptimer=zeit;   

    int devices = sensors.getDeviceCount();
    //Serial.println(devices);
    sensors.requestTemperatures();
    if (devices>0) {
      tempvalue = round(sensors.getTempCByIndex(0));
      //Serial.println(tempvalue);
      SendeStatusTemp();
      if ((tempvalue>25) && (heizungTempAlarm == 0)){
            EMail_Send("HomeServer/Heizung/TempAlarm");
            heizungTempAlarm = 1; 
        }
    } 
    else
      Serial.println("no temp device found");
  }


      if ((wasserstarted != 0) && ((wasserstarted + wasserAlert)< zeit)) {
          // alarm !
          if (wasseralarm == 0) {
              EMail_Send("HomeServer/Heizung/WasserAlarm");  
              UDBDebug("Wasseralarm");
              MQTT_Send("HomeServer/Heizung/WasserAlarm",(long) wasserstarted); 
              wasseralarm = 1;    
          }     
      }



// Web Server - Request von außen
  // Check if a client has connected
  WiFiClient client = server.available();
  if (!client) {
    delay(1);
    return;
  }
  // if not, return loop here
 // Wait until the client sends some data
  Serial.println("new client");
  int timeout=0;
  while(!client.available()){
    delay(1);   
    timeout++;
    if(timeout>10000) {Serial.print("INFINITE LOOP BREAK!");  break;}
    }

 
  // Read the first line of the request
  String request = client.readStringUntil('\r');
  Serial.println(request);
  client.flush();
 
  // Match the request
    // Return the response
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html");
  client.println(""); //  do not forget this one
  client.println("<!DOCTYPE HTML>");
  client.println("<html>");

  String answer = "device status: ";
  answer += wassercounter;
    answer += "&wassercounterday=";
  answer += wassercounterday;
  answer += "&Temp=";
  answer += tempvalue;
   answer += "&time=";
  answer += lastwasservalue;
  client.print(answer);

  client.println("</html>");
  
  delay(1);
}


bool mygetLocalTime(struct tm * info, uint32_t ms)
{
    uint32_t start = millis();
    time_t now;
    while((millis()-start) <= ms) {
        time(&now);
        localtime_r(&now, info);
        if(info->tm_year > (2016 - 1900)){
            return true;
        }
        delay(10);
    }
    return false;
}


// #############################################################
void WIFI_Connect()
{
  digitalWrite(blinkled,1);
  WiFi.disconnect();
  Serial.println("Booting Sketch...");
  WiFi.mode(WIFI_STA);
  WiFi.hostname("WemosWasser");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
    // Wait for connection
  for (int i = 0; i < 25; i++)
  {
    if ( WiFi.status() != WL_CONNECTED ) {
      delay ( 250 );
      digitalWrite(blinkled,0);
      Serial.print ( "." );
      delay ( 250 );
      digitalWrite(blinkled,1);
    }
  }
  digitalWrite(blinkled,0);
  while (WiFi.waitForConnectResult() != WL_CONNECTED) {
    Serial.println("Connection Failed! Rebooting...");
    delay(5000);
    ESP.restart();
  }
}

// #############################################################

void SendeStatus() {
  digitalWrite(blinkled,1);

  MQTT_Send("HomeServer/Heizung/Wasser",wassercounter); 
  MQTT_Send("HomeServer/Heizung/WasserDay",wassercounterday);  
 // MQTT_Send("hm/set/CUX9002006:1/SET_TEMPERATURE",tempvalue);  
  digitalWrite(blinkled,0);
}

void SendeStatusTemp() {
  digitalWrite(blinkled,1);

  if (lasttemp == tempvalue)
  { if (tempcounter++ > 60)   {
      MQTT_Send("hm/set/CUX9002006:1/SET_TEMPERATURE",tempvalue);
      tempcounter = 0;
    }
  }
  else {
    MQTT_Send("hm/set/CUX9002006:1/SET_TEMPERATURE",tempvalue);  
    tempcounter = 0;
    }
  lasttemp = tempvalue;  
  digitalWrite(blinkled,0);
}

void UDBDebug(String message) {
#ifdef UDPDEBUG
  udp.beginPacket(udpAddress, udpPort);
  udp.write((const uint8_t* ) message.c_str(), (size_t) message.length());
  udp.endPacket();
#endif  
}


void MQTT_Send(char const * topic, String value) {
    //UDBDebug("MQTT " +String(topic)+" "+value); 
    Serial.println("MQTT " +String(topic)+" "+value) ;
    if (!mqttclient.publish(topic, value.c_str(), true)) {
       UDBDebug("Wasser MQTT error");  
    };
}

void MQTT_Send(char const * topic, float value) {
    char buffer[10];
    snprintf(buffer, 10, "%f", value);
    MQTT_Send(topic, buffer);
}

void MQTT_Send(char const * topic, int16_t value) {
    char buffer[10];
    snprintf(buffer, 10, "%d", value);
    MQTT_Send(topic, buffer);
}

void MQTT_Send(char const * topic, long value) {
    char buffer[10];
    snprintf(buffer, 10, "%ld", value);
    MQTT_Send(topic, buffer);
}

void Flash_Read() {
  // check if our structure
  //EEPROM.begin(EEPROM_SIZE);
  int8_t check =0;
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(0, check);
  UDBDebug("EEPROM star read check: "+String(check));

  if (check == 0x4E)  {
      EEPROM.get(1, wassercounter);
      EEPROM.get(5, wassercounterday);
      UDBDebug("EEPROM get day: "+String(wassercounterday));
  }    

  EEPROM.end();
}

void Flash_Write() {
    EEPROM.begin(EEPROM_SIZE);
    int8_t check =0x4E;
    EEPROM.put(0, check);
    EEPROM.put(1, wassercounter); 
    EEPROM.put(5, wassercounterday); 
    UDBDebug("EEPROM put: "+String(wassercounterday));
    EEPROM.commit();
    EEPROM.end();
}


// EMAIL

void EMail_Send(String textmessage) {
/* Declare the session config data */
  ESP_Mail_Session session;

  /* Set the session config */
  session.server.host_name = SMTP_HOST;
  session.server.port = SMTP_PORT;
  session.login.email = email_user;
  session.login.password = email_pass;
  session.login.user_domain = email_domain;

  /* Declare the message class */
  SMTP_Message message;

  /* Set the message headers */
  message.sender.name = "ESP";
  message.sender.email = email_user;
  message.subject = "HomeServer Wasser Alert";
  message.addRecipient(email_user, email_user);

  message.text.content = textmessage.c_str();
  message.text.charSet = "us-ascii";
  message.text.transfer_encoding = Content_Transfer_Encoding::enc_7bit;

  /* Connect to server with the session config */
  if (!smtp.connect(&session))
    return;

  /* Start sending Email and close the session */
  if (!MailClient.sendMail(&smtp, &message))
    UDBDebug("Wasser Error sending Email, " + smtp.errorReason());
}

/* Callback function to get the Email sending status */
void smtpCallback(SMTP_Status status)
{
  /* Print the current status */
  UDBDebug(String(status.info()));

  /* Print the sending result */
  if (status.success())
  {
    UDBDebug("Wasser Message sent success: "+String(status.completedCount()));
    UDBDebug("Wasser Message sent failed: "+String(status.failedCount()));
    // You need to clear sending result as the memory usage will grow up.
    smtp.sendingResult.clear();
  }
}
#include "ClusterDuck.h"

#ifdef CDPCFG_OLED_CLASS
  CDPCFG_OLED_CLASS u8x8(/* clock=*/ CDPCFG_PIN_OLED_CLOCK, /* data=*/ CDPCFG_PIN_OLED_DATA, /* reset=*/ CDPCFG_PIN_OLED_RESET);
#endif

#ifdef CDPCFG_PIN_LORA_SPI_SCK
  #include "SPI.h"
  SPIClass _spi;
  SPISettings _spiSettings;
  CDPCFG_LORA_CLASS lora = new Module(CDPCFG_PIN_LORA_CS, CDPCFG_PIN_LORA_DIO0, CDPCFG_PIN_LORA_RST, CDPCFG_PIN_LORA_DIO1, _spi, _spiSettings);
#else
  CDPCFG_LORA_CLASS lora = new Module(CDPCFG_PIN_LORA_CS, CDPCFG_PIN_LORA_DIO0, CDPCFG_PIN_LORA_RST, CDPCFG_PIN_LORA_DIO1);
#endif

IPAddress apIP(CDPCFG_AP_IP1, CDPCFG_AP_IP2, CDPCFG_AP_IP3, CDPCFG_AP_IP4);
AsyncWebServer webServer(CDPCFG_WEB_PORT);

auto tymer = timer_create_default();

byte transmission[CDPCFG_CDP_BUFSIZE];
int packetIndex = 0;

String ssid = "";
String password = "";

String ClusterDuck::_deviceId = "";

bool restartRequired = false;
size_t content_len;

//Username and password for /update
const char* http_username = CDPCFG_UPDATE_USERNAME;
const char* http_password = CDPCFG_UPDATE_PASSWORD;


ClusterDuck::ClusterDuck() {

}

void ClusterDuck::setDeviceId(String deviceId) {
  _deviceId = deviceId;

}

void ClusterDuck::begin(int baudRate) {
  Serial.begin(baudRate);
  Serial.print("Serial start ");
  Serial.println(baudRate, DEC);
}

void ClusterDuck::setupDisplay(String deviceType)  {
#ifndef CDPCFG_OLED_NONE
  u8x8.begin();
  u8x8.setFont(u8x8_font_chroma48medium8_r);

#ifdef CDPCFG_OLED_64x32
  // small display 64x32
  u8x8.setCursor(0, 2);
  u8x8.print("((>.<))");

  u8x8.setCursor(0, 4);
  u8x8.print("DT: " + deviceType);

  u8x8.setCursor(0, 5);
  u8x8.print("ID: " + _deviceId);

//  u8x8.setCursor(0, 4);
//  u8x8.print("ST: Online");

#else
  // default display size 128x64
  u8x8.setCursor(0, 1);
  u8x8.print("    ((>.<))    ");

  u8x8.setCursor(0, 2);
  u8x8.print("  Project OWL  ");

  u8x8.setCursor(0, 4);
  u8x8.print("Device: " + deviceType);

  u8x8.setCursor(0, 5);
  u8x8.print("Status: Online");

  u8x8.setCursor(0, 6);
  u8x8.print("ID:     " + _deviceId);

  u8x8.setCursor(0, 7);
  u8x8.print(duckMac(false));
#endif
#endif
}

// Initial LoRa settings
void ClusterDuck::setupLoRa(long BAND, int SS, int RST, int DI0, int DI1, int TxPower) {

#ifdef CDPCFG_PIN_LORA_SPI_SCK
  log_n("_spi.begin(CDPCFG_PIN_LORA_SPI_SCK, CDPCFG_PIN_LORA_SPI_MISO, CDPCFG_PIN_LORA_SPI_MOSI, CDPCFG_PIN_LORA_CS)");
  _spi.begin(CDPCFG_PIN_LORA_SPI_SCK, CDPCFG_PIN_LORA_SPI_MISO, CDPCFG_PIN_LORA_SPI_MOSI, CDPCFG_PIN_LORA_CS);
  lora = new Module(SS, DI0, RST, DI1, _spi, _spiSettings);
#else
  lora = new Module(SS, DI0, RST, DI1);
#endif

  Serial.println("Starting LoRa......");
  int state = lora.begin();

  //Initialize LoRa
  if (state == ERR_NONE) {
    Serial.println("LoRa online, Quack!");
  } else {
#ifndef CDPCFG_OLED_NONE
    u8x8.clear();
    u8x8.drawString(0, 0, "Starting LoRa failed!");
#endif
    Serial.print("Starting LoRa Failed!!!");
    Serial.println(state);
    restartDuck();
  }

  if (lora.setFrequency(CDPCFG_RF_LORA_FREQ) == ERR_INVALID_FREQUENCY) {
    Serial.println(F("Selected frequency is invalid for this module!"));
    while (true);
  }

  if (lora.setBandwidth(CDPCFG_RF_LORA_BW) == ERR_INVALID_BANDWIDTH) {
    Serial.println(F("Selected bandwidth is invalid for this module!"));
    while (true);
  }

  if (lora.setSpreadingFactor(CDPCFG_RF_LORA_SF) == ERR_INVALID_SPREADING_FACTOR) {
    Serial.println(F("Selected spreading factor is invalid for this module!"));
    while (true);
  }

  if (lora.setOutputPower(CDPCFG_RF_LORA_TXPOW) == ERR_INVALID_OUTPUT_POWER) {
    Serial.println(F("Selected output power is invalid for this module!"));
    while (true);
  }

  if (lora.setGain(CDPCFG_RF_LORA_GAIN) == ERR_INVALID_GAIN) {
    Serial.println(F("Selected gain is invalid for this module!"));
    while (true);
  }

  lora.setDio0Action(setFlag);

  state = lora.startReceive();

  if (state == ERR_NONE) {
    Serial.println("Listening for quacks");
  } else {
    Serial.print("failed, code ");
    Serial.println(state);
    restartDuck();
  }
}

//============= Used for receiving LoRa packets ==========
volatile bool receivedFlag = false;

// disable interrupt when it's not needed
volatile bool enableInterrupt = true;

void ClusterDuck::setFlag(void) {
  // check if the interrupt is enabled
  if(!enableInterrupt) {
    return;
  }
  // we got a packet, set the flag
  receivedFlag = true;
}
// =====================================

// handle the upload of the firmware
void handleFirmwareUpload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t lenght, bool final){
  // handle upload and update
  if (!index) {
    Serial.printf("Update: %s\n", filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) { //start with max available size
      Update.printError(Serial);
    }
  }
  // flashing firmware to ESP
  if (lenght){
    Update.write(data, lenght);
  }
  if (final) {
    if (Update.end(true)) { //true to set the size to the current progress
      Serial.printf("Update Success: %ub written\nRebooting...\n", index+lenght);
    } else {
      Update.printError(Serial);
    }
  }
}

//Setup WebServer
void ClusterDuck::setupWebServer(bool createCaptivePortal) {
  Serial.println("Setting up Web Server");

  webServer.onNotFound([&](AsyncWebServerRequest *request) {
    request->send(200, "text/html", portal);
  });

  webServer.on("/", HTTP_GET, [&](AsyncWebServerRequest *request) {
    request->send(200, "text/html", portal);
  });

// Update Firmware OTA
     webServer.on("/update", HTTP_GET, [&](AsyncWebServerRequest *request){
               if(!request->authenticate(http_username, http_password))
               return request->requestAuthentication();

                AsyncWebServerResponse *response = request->beginResponse(200, "text/html", update_page);

                request->send(response);
            });



webServer.on("/update", HTTP_POST, [&](AsyncWebServerRequest *request) {


                AsyncWebServerResponse *response = request->beginResponse((Update.hasError())?500:200, "text/plain", (Update.hasError())?"FAIL":"OK");
                response->addHeader("Connection", "close");
                response->addHeader("Access-Control-Allow-Origin", "*");
                request->send(response);
                restartRequired = true;
            }, [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {

                if (!index) {

                  lora.standby();
                  enableInterrupt = false;
                  Serial.println("Pause Lora");
                  Serial.println("startint OTA update");

                    content_len = request->contentLength();

                        int cmd = (filename.indexOf("spiffs") > -1) ? U_SPIFFS : U_FLASH;
                        if (!Update.begin(UPDATE_SIZE_UNKNOWN, cmd)) {

                            Update.printError(Serial);
                        }

                }

                if (Update.write(data, len) != len) {
                    Update.printError(Serial);
                    lora.startReceive();
                }

                if (final) {
                    if (Update.end(true)) {
                      ESP.restart();
                    esp_task_wdt_init(1,true);
                    esp_task_wdt_add(NULL);
                    while(true);

                    }
                }
            });

  // Captive Portal form submission
  webServer.on("/formSubmit", HTTP_POST, [&](AsyncWebServerRequest *request) {
    Serial.println("Submitting Form");

    int paramsNumber = request->params();
    String val = "";

    for (int i = 0; i < paramsNumber; i++) {
      AsyncWebParameter *p = request->getParam(i);
      Serial.printf("%s: %s", p->name().c_str(), p->value().c_str());
      Serial.println();

      val = val + p->value().c_str() + "*";
    }

    sendPayloadStandard(val);

    request->send(200, "text/html", portal);
  });

  webServer.on("/id", HTTP_GET, [&](AsyncWebServerRequest *request) {
    request->send(200, "text/html", _deviceId);
  });

  webServer.on("/restart", HTTP_GET, [&](AsyncWebServerRequest *request) {
    request->send(200, "text/plain", "Restarting...");
    delay(1000);
    restartDuck();
  });

  webServer.on("/mac", HTTP_GET, [&](AsyncWebServerRequest *request) {
    String mac = duckMac(true);
    request->send(200, "text/html", mac);
  });

	webServer.on("/wifi", HTTP_GET, [&](AsyncWebServerRequest *request) {

		AsyncResponseStream *response = request->beginResponseStream("text/html");
    response->print("<!DOCTYPE html><html><head><title>Update Wifi Credentials</title></head><body>");
    response->print("<p>Use this page to update your Wifi credentials</p>");


		response->print("<form action='/changeSSID' method='post'>");

		response->print("<label for='ssid'>SSID:</label><br>");
		response->print("<input name='ssid' type='text' placeholder='SSID' /><br><br>");

		response->print("<label for='pass'>Password:</label><br>");
		response->print("<input name='pass' type='text' placeholder='Password' /><br><br>");

    response->print("<input type='submit' value='Submit' />");

		response->print("</form>");

    response->print("</body></html>");
    request->send(response);
	});

	webServer.on("/changeSSID", HTTP_POST, [&](AsyncWebServerRequest *request) {
		int paramsNumber = request->params();
    String val = "";
		String SSID = "";
		String PASSWORD = "";

    for (int i = 0; i < paramsNumber; i++) {
      AsyncWebParameter *p = request->getParam(i);

      String name = String(p->name());
      String value = String(p->value());

			if (name == "ssid") {
        SSID = String(p->value());
			} else if (name == "pass") {
				PASSWORD = String(p->value());
			}
    }

		if (SSID != "" && PASSWORD != "") {
			setupInternet(SSID, PASSWORD);
      request->send(200, "text/plain", "Success");
		} else {
      request->send(500, "text/plain", "There was an error");
    }
	});


  webServer.begin();
}

void ClusterDuck::setupWifiAp(const char *AP) {
	WiFi.mode(WIFI_AP);
  WiFi.softAP(AP);
  delay(200); // wait for 200ms for the access point to start before configuring

  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));

  Serial.println("Created Wifi Access Point");
}

void ClusterDuck::setupDns() {
	dnsServer.start(DNS_PORT, "*", apIP);

	if (!MDNS.begin(DNS))
  {
    Serial.println("Error setting up MDNS responder!");
  }
  else
  {
    Serial.println("Created local DNS");
    MDNS.addService("http", "tcp", CDPCFG_WEB_PORT);
  }
}

void ClusterDuck::setupInternet(String SSID, String PASSWORD)
{
  ssid = SSID;
  password = PASSWORD;
  Serial.println();
  Serial.print("Connecting to ");
  Serial.print(SSID);

  if (SSID != "" && PASSWORD != "") {
  // Connect to Access Point
    WiFi.begin(SSID.c_str(), PASSWORD.c_str());

    while (WiFi.status() != WL_CONNECTED)
    {
      tymer.tick(); //Advance timer to reboot after awhile
      //TODO: Change this to make sure it is non-blocking for all other processes
    }

    // Connected to Access Point
    Serial.println("");
    Serial.println("DUCK CONNECTED TO INTERNET");
  }
}

//Setup OTA
void ClusterDuck::setupOTA(){

  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH)
      type = "sketch";
    else // U_SPIFFS
      type = "filesystem";

    // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
    Serial.println("Start updating " + type);
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });

  ArduinoOTA.begin();
}

//Setup premade DuckLink with default settings
void ClusterDuck::setupDuckLink() {
  setupDisplay("Duck");
  setupLoRa();
	setupWifiAp();
	setupDns();
  setupWebServer(true);
  setupOTA();

  Serial.println("Duck Online");
}

void ClusterDuck::runDuckLink() {
  ArduinoOTA.handle();
  processPortalRequest();
}

void ClusterDuck::setupDetect() {
  setupDisplay("Detector");
  setupLoRa();
  setupWifiAp();
	setupDns();
  setupWebServer(false);
  setupOTA();

  Serial.println("Detector Online");
}

int ClusterDuck::runDetect() {
  ArduinoOTA.handle();
  int val = 0;
  if(receivedFlag) {  //If LoRa packet received
    receivedFlag = false;
    enableInterrupt = false;
    int pSize = handlePacket();
    Serial.print("runDetect pSize ");
    Serial.println(pSize);
    if(pSize > 0) {
      for(int i=0; i < pSize; i++) {
        if(transmission[i] == iamhere_B) {
          val = lora.getRSSI();
        }
      }
    }
    enableInterrupt = true;
    Serial.println("runDetect startReceive");
    startReceive();
  }
  return val;
}

void ClusterDuck::processPortalRequest() {

  dnsServer.processNextRequest();

}

void ClusterDuck::setupMamaDuck() {
#ifdef CDPCFG_MESH_DEDUP_BLOOM
  setupBloomFilters();
#endif
  setupDisplay("Mama");
  setupLoRa();
	setupWifiAp();
	setupDns();
  setupWebServer(true);
  setupOTA();

  Serial.println("MamaDuck Online");

  tymer.every(CDPCFG_MILLIS_ALIVE, imAlive);
  tymer.every(CDPCFG_MILLIS_REBOOT, reboot);
}

#ifdef CDPCFG_MESH_DEDUP_BLOOM
void ClusterDuck::setupBloomFilters() {
  log_n("start");
  if (_bf == NULL) {
    log_n("alloc %i",CDPCFG_MESH_DEDUP_BLOOM_COUNT);
    _bf = (cdp_bf_t *)calloc(CDPCFG_MESH_DEDUP_BLOOM_COUNT,sizeof(cdp_bf_t));
  }
  _bf_active = 0;
  // simply cycle through the whole list to (re)init them all
  for (int i=0; i<CDPCFG_MESH_DEDUP_BLOOM_COUNT; i++) {
    log_n("cycle %l",i);
    cycleBloomFilters();
  }
}

void ClusterDuck::cycleBloomFilters() {
  log_n("start");
  if (_bf == NULL) {
    return;
  }
  // decide next active filter
  log_n("old active: %i",_bf_active);
  _bf_active = (_bf_active + 1) % CDPCFG_MESH_DEDUP_BLOOM_COUNT;
  log_n("new active: %i",_bf_active);
  // (re)init filter
  cdp_bf_init(CDPCFG_MESH_DEDUP_BLOOM_K, CDPCFG_MESH_DEDUP_BLOOM_M, CDPCFG_MESH_DEDUP_BLOOM_SALT, &_bf[_bf_active]);
}

bool ClusterDuck::checkBloomFilters(String msg) {
  log_n("start");
  // check_add to active filter
  log_n("add bf[%d].n=%d", _bf_active, _bf[_bf_active].n);
  bool have = cdp_bf_check_add(&_bf[_bf_active],(byte *)msg.c_str(),msg.length());
  if (have) { // if have, return true
    log_n("have active %d",_bf_active);
    return true;
  } else { // if added, check if full
    if (_bf[_bf_active].n > CDPCFG_MESH_DEDUP_BLOOM_MAXN) {
      // if full rotate and return false
      cycleBloomFilters();
      log_n("dont cycled");
      return false;
    }
    log_n("dont n=%d", _bf[_bf_active].n);
  }
  // check passive filters
  for (int i=(_bf_active+CDPCFG_MESH_DEDUP_BLOOM_COUNT-1)%CDPCFG_MESH_DEDUP_BLOOM_COUNT; i!=_bf_active; i=(i+CDPCFG_MESH_DEDUP_BLOOM_COUNT-1)%CDPCFG_MESH_DEDUP_BLOOM_COUNT) {
    // if have, return true
    log_n("chk bf[%d].n=%d", i, _bf[i].n);
    if (cdp_bf_check(&_bf[i],(byte *)msg.c_str(),msg.length())) {
      log_n("have passive %d",i);
      return true;
    }
  }
  // no matches found: return false
  log_n("dont final");
  return false;
}
#endif // CDPCFG_MESH_DEDUP_BLOOM

bool ClusterDuck::checkRelayPacket() {
  bool op = false;
#ifdef CDPCFG_MESH_DEDUP_PATH
  bool op1 = op = !idInPath(_lastPacket.path);
#endif // CDPCFG_MESH_DEDUP_PATH
#ifdef CDPCFG_MESH_DEDUP_BLOOM
  bool op2 = op = !checkBloomFilters(_lastPacket.senderId + "::" +
                                    _lastPacket.messageId + "::" +
                                    _lastPacket.payload);
#endif // CDPCFG_MESH_DEDUP_BLOOM
#if defined(CDPCFG_MESH_DEDUP_PATH) && defined(CDPCFG_MESH_DEDUP_BLOOM)
  // relay only if both strategies allow relaying
  op = op1 && op2;
  if (op1 != op2) {
    log_e("MISMATCH: path=%d bloom=%d res=%d", op1, op2, op);
  }
#endif // PATH + BLOOM
  return op;
}

void ClusterDuck::runMamaDuck() {
  ArduinoOTA.handle();
  if (enableInterrupt) {
    tymer.tick();
  }

  if(receivedFlag) {  //If LoRa packet received
    receivedFlag = false;
    enableInterrupt = false;
    int pSize = handlePacket();
    Serial.print("runMamaDuck pSize ");
    Serial.println(pSize);
    if(pSize > 0) {
      String msg = getPacketData(pSize);
      packetIndex = 0;
      if(msg != "ping" && checkRelayPacket()) {
        Serial.println("runMamaDuck relayPacket");
        sendPayloadStandard(_lastPacket.payload, _lastPacket.senderId, _lastPacket.messageId, _lastPacket.path);
        memset(transmission, 0x00, CDPCFG_CDP_BUFSIZE); //Reset transmission
        packetIndex = 0;

      }
    } else {
      // Serial.println("Byte code not recognized!");
      memset(transmission, 0x00, CDPCFG_CDP_BUFSIZE); //Reset transmission
      packetIndex = 0;

     }
    enableInterrupt = true;
    Serial.println("runMamaDuck startReceive");
    startReceive();
  }

  processPortalRequest();

}

String tohex(byte *data, int size) {
  String buf = "";
  buf.reserve(size*2);
  const char *cs = "0123456789abcdef";
  for (int i=0; i<size; i++) {
    byte val = data[i];
    buf += cs[(val>>4) & 0x0f];
    buf += cs[ val     & 0x0f];
  }
  return buf;
}

int ClusterDuck::handlePacket() {
  int pSize = lora.getPacketLength();
  memset(transmission, 0x00, CDPCFG_CDP_BUFSIZE); //Reset transmission
  packetIndex = 0;
  int state = lora.readData(transmission, pSize);

  if (state == ERR_NONE) {
    // packet was successfully received
    Serial.print("handlePacket pSize ");
    Serial.println(pSize);

    // dump received packet stats+data
    Serial.print("LORA RCV");
    Serial.print(" millis:");
    Serial.print(millis());
    Serial.print(" rssi:");
    Serial.print(lora.getRSSI());
    Serial.print(" snr:");
    Serial.print(lora.getSNR());
    Serial.print(" fe:");
    Serial.print(lora.getFrequencyError());
    Serial.print(" size:");
    Serial.print(pSize);
    Serial.print(" data:");
    Serial.println(tohex(transmission, pSize));

    return pSize;
  } else {
    // some other error occurred
    Serial.print("handlePacket Failed, code ");
    Serial.println(state);

    return -1;
  }
}

void ClusterDuck::sendPayloadMessage(String msg) {
  couple(senderId_B, _deviceId);
  couple(messageId_B, uuidCreator());
  couple(payload_B, msg);
  couple(path_B, _deviceId);

  Serial.print("sendPayloadMessage packetIndex ");
  Serial.println(packetIndex);
  startTransmit();

}

void ClusterDuck::sendPayloadStandard(String msg, String senderId, String messageId, String path) {
  if(senderId == "") senderId = _deviceId;
  if(messageId == "") messageId = uuidCreator();
  if(path == "") {
    path = _deviceId;
  } else {
    path = path + "," + _deviceId;
  }

  String total = senderId + messageId + path + msg;
  if(total.length() + 4 > CDPCFG_CDP_BUFSIZE) {
    Serial.println("Warning: message is too large!"); //TODO: do something
  }

  couple(senderId_B, senderId);
  couple(messageId_B, messageId);
  couple(payload_B, msg);
  couple(path_B, path);

  Serial.print("sendPayloadStandard packetIndex ");
  Serial.println(packetIndex);

  startTransmit();

}

void ClusterDuck::couple(byte byteCode, String outgoing) {
  int outgoingLen = outgoing.length() + 1;
  byte byteBuffer[outgoingLen];

  outgoing.getBytes(byteBuffer, outgoingLen);

  transmission[packetIndex] = byteCode; //add byte code
  packetIndex++;
  transmission[packetIndex] = (byte)outgoingLen; // add payload length
  packetIndex++;

  for(int i=0; i < outgoingLen; i++) {  // add payload
    transmission[packetIndex] = byteBuffer[i];
    packetIndex++;
  }

}

bool ClusterDuck::idInPath(String path) {
  Serial.print("idInPath '");
  Serial.print(path);
  Serial.print("' ");
  String temp = "";
  int len = path.length() + 1;
  char arr[len];
  path.toCharArray(arr, len);

  for (int i = 0; i < len; i++) {
    if (arr[i] == ',' || i == len - 1) {
      if (temp == _deviceId) {
        Serial.println("true");
        return true;
      }
      temp = "";
    } else {
      temp += arr[i];
    }
  }
  Serial.println("false");
  return false;
}

String ClusterDuck::getPacketData(int pSize) {
  String packetData = "";
  if(pSize == 0) {
    Serial.println("getPacketData Packet is empty!");
    return packetData;
  }
  packetIndex = 0;
  int len = 0;
  bool sId = false;
  bool mId = false;
  bool pLoad = false;
  bool pth = false;
  String msg = "";
  bool gotLen = false;

  for(int i=0; i < pSize; i++) {
    if(i > 0 && len == 0) {
      gotLen = false;
      if(sId) {
        _lastPacket.senderId  = msg;
        Serial.println("getPacketData User ID: " + _lastPacket.senderId);
        msg = "";
        sId = false;

      } else if(mId) {
        _lastPacket.messageId = msg;
        Serial.println("getPacketData Message ID: " + _lastPacket.messageId);
        msg = "";
        mId = false;
      } else if(pLoad) {
        _lastPacket.payload = msg;
        Serial.println("getPacketData Message: " + _lastPacket.payload);
        msg = "";
        pLoad = false;
      } else if(pth) {
        _lastPacket.path = msg;
        Serial.println("getPacketData Path: " + _lastPacket.path);
        msg = "";
        pth = false;
      }
    }
    if(transmission[i] == senderId_B){
      //Serial.println(transmission[1+i]);
      sId = true;
      len = transmission[i+1];
      Serial.println("getPacketData senderId_B Len = " + String(len));

    } else if(transmission[i] == messageId_B) {
      mId = true;
      len = transmission[i+1];
      Serial.println("getPacketData messageId_B Len = " + String(len));

    } else if(transmission[i] == payload_B) {
      pLoad = true;
      len = transmission[i+1];
      Serial.println("getPacketData payload_B Len = " + String(len));

    } else if(transmission[i] == path_B) {
      pth = true;
      len = transmission[i+1];
      Serial.println("getPacketData path_B Len = " + String(len));

    } else if(transmission[i] == ping_B) {
      if(_deviceId != "Det") {
        memset(transmission, 0x00, CDPCFG_CDP_BUFSIZE);
        packetIndex = 0;
        couple(iamhere_B, "1");
        startTransmit();
        Serial.println("getPacketData pong sent");
        packetData = "ping";
        return packetData;
      }
      memset(transmission, 0x00, CDPCFG_CDP_BUFSIZE);
      packetIndex = 0;
      packetData = "ping";

    } else if(transmission[i] == iamhere_B) {
      Serial.println("getPacketData pong received");
      memset(transmission, 0x00, CDPCFG_CDP_BUFSIZE);
      packetIndex = 0;
      packetData = "pong";
      return packetData;

      } else if(len > 0 && gotLen) {
      msg = msg + String((char)transmission[i]);
      len--;

    } else {
      gotLen = true;

    }
    packetIndex++;
  }

  if(len == 0) {
    if(sId) {
      _lastPacket.senderId  = msg;
      Serial.println("getPacketData len0 User ID: " + _lastPacket.senderId);
      msg = "";

    } else if(mId) {
      _lastPacket.messageId = msg;
      Serial.println("getPacketData len0 Message ID: " + _lastPacket.messageId);
      msg = "";

    } else if(pLoad) {
      _lastPacket.payload = msg;
      Serial.println("getPacketData len0 Message: " + _lastPacket.payload);
      msg = "";

    } else if(pth) {
      _lastPacket.path = msg;
      Serial.println("getPacketData len0 Path: " + _lastPacket.path);
      msg = "";
    }
  }

  return packetData;
}

/**
  restart
  Only restarts ESP
*/
void ClusterDuck::restartDuck()
{
  Serial.println("Restarting Duck...");
  ESP.restart();
}

//Timer reboot
bool ClusterDuck::reboot(void *) {
  String reboot = "REBOOT";
  Serial.println(reboot);
  sendPayloadMessage(reboot);
  restartDuck();

  return true;
}

bool ClusterDuck::imAlive(void *) {
  String alive = "Health Quack";
  Serial.print("imAlive sending");
  sendPayloadMessage(alive);

  return true;
}

//Get Duck MAC address
String ClusterDuck::duckMac(boolean format)
{
  char id1[15];
  char id2[15];

  uint64_t chipid = ESP.getEfuseMac(); // The chip ID is essentially its MAC address(length: 6 bytes).
  uint16_t chip = (uint16_t)(chipid >> 32);

  snprintf(id1, 15, "%04X", chip);
  snprintf(id2, 15, "%08X", (uint32_t)chipid);

  String ID1 = id1;
  String ID2 = id2;

  String unformattedMac = ID1 + ID2;

  if(format == true){
    String formattedMac = "";
    for(int i = 0; i < unformattedMac.length(); i++){
      if(i % 2 == 0 && i != 0){
        formattedMac += ":";
        formattedMac += unformattedMac[i];
      }
      else {
        formattedMac += unformattedMac[i];
      }
    }
    return formattedMac;
  } else {
    return unformattedMac;
  }
}

//Create a uuid
String ClusterDuck::uuidCreator() {
  String msg = "";
  int i;

  for (i = 0; i < CDPCFG_UUID_LEN; i++) {
      byte randomValue = random(0, 36);
      if (randomValue < 26) {
        msg = msg + char(randomValue + 'a');
      } else {
        msg = msg + char((randomValue - 26) + '0');
      }
  }
  Serial.print("uuidCreator ");
  Serial.println(msg);
  return msg;
}

//Getters
String ClusterDuck::getDeviceId() {
  return _deviceId;
}

Packet ClusterDuck::getLastPacket() {
  Packet packet = _lastPacket;
  _lastPacket = Packet();
  return packet;
}

volatile bool ClusterDuck::getFlag() {
  return receivedFlag;
}

volatile bool ClusterDuck::getInterrupt() {
  return enableInterrupt;
}

int ClusterDuck::getRSSI() {
  return lora.getRSSI();
}

String ClusterDuck::getSSID() {
  return ssid;
}

String ClusterDuck::getPassword() {
  return password;
}

//Setter
void ClusterDuck::flipFlag() {
  if (receivedFlag == true) {
    receivedFlag = false;
  } else {
    receivedFlag = true;
  }
}

void ClusterDuck::flipInterrupt() {
  if (enableInterrupt == true) {
    enableInterrupt = false;
  } else {
    enableInterrupt = true;
  }
}

void ClusterDuck::startReceive() {
  int state = lora.startReceive();

  if (state == ERR_NONE) {

  } else {
    Serial.print("startReceive failed, code ");
    Serial.println(state);
    restartDuck();
  }
}

void ClusterDuck::startTransmit() {
  bool oldEI = enableInterrupt;
  enableInterrupt = false;

  // dump send packet stats+data
  Serial.print("LORA SND");
  Serial.print(" millis:");
  Serial.print(millis());
  Serial.print(" size:");
  Serial.print(packetIndex);
  Serial.print(" data:");
  Serial.println(tohex(transmission, packetIndex));

  int state = lora.transmit(transmission, packetIndex);

  memset(transmission, 0x00, CDPCFG_CDP_BUFSIZE); //Reset transmission
  packetIndex = 0; //Reset packetIndex

  if (state == ERR_NONE) {
    // the packet was successfully transmitted
    Serial.println("startTransmit Packet sent");
  } else if (state == ERR_PACKET_TOO_LONG) {
    // the supplied packet was longer than 256 bytes
    Serial.println("startTransmit too long!");
  } else if (state == ERR_TX_TIMEOUT) {
    // timeout occured while transmitting packet
    Serial.println("startTransmit timeout!");
  } else {
    // some other error occurred
    Serial.print(F("startTransmit failed, code "));
    Serial.println(state);
  }

  if (oldEI) {
    enableInterrupt = true;
    startReceive();
  }
}

void ClusterDuck::ping() {
  couple(ping_B, "0");
  startTransmit();
}

// Setup LED
void ClusterDuck::setupLED() {
  ledcAttachPin(ledR, 1); // assign RGB led pins to channels
  ledcAttachPin(ledG, 2);
  ledcAttachPin(ledB, 3);

// Initialize channels
// channels 0-15, resolution 1-16 bits, freq limits depend on resolution
// ledcSetup(uint8_t channel, uint32_t freq, uint8_t resolution_bits);
  ledcSetup(1, 12000, 8); // 12 kHz PWM, 8-bit resolution
  ledcSetup(2, 12000, 8);
  ledcSetup(3, 12000, 8);
}

void ClusterDuck::setColor(int red, int green, int blue)
{
  ledcWrite(1, red);
  ledcWrite(2, green);
  ledcWrite(3, blue);
}

DNSServer ClusterDuck::dnsServer;
const char * ClusterDuck::DNS  = "duck";
const byte ClusterDuck::DNS_PORT = 53;



int ClusterDuck::_rssi = 0;
float ClusterDuck::_snr;
long ClusterDuck::_freqErr;
int ClusterDuck::_availableBytes;
int ClusterDuck::_packetSize = 0;
// LED
int ClusterDuck::ledR = CDPCFG_PIN_RGBLED_R;
int ClusterDuck::ledG = CDPCFG_PIN_RGBLED_G;
int ClusterDuck::ledB = CDPCFG_PIN_RGBLED_B;

Packet ClusterDuck::_lastPacket;

byte ClusterDuck::ping_B       = 0xF4;
byte ClusterDuck::senderId_B   = 0xF5;
byte ClusterDuck::messageId_B  = 0xF6;
byte ClusterDuck::payload_B    = 0xF7;
byte ClusterDuck::iamhere_B    = 0xF8;
byte ClusterDuck::path_B       = 0xF3;

String ClusterDuck::portal = MAIN_page;

#ifdef CDPCFG_MESH_DEDUP_BLOOM
cdp_bf_t *ClusterDuck::_bf;
byte ClusterDuck::_bf_active;
#endif


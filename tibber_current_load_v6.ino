#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <secrets-empty.h>  // bevat WIFI_SSID, WIFI_PASS, TIBBER_TOKEN, TIBBER_HOME_ID

const char* host = "websocket-api.tibber.com";
const int port = 443;
const char* path = "/v1-beta/gql/subscriptions";

// Baltimore CyberTrust Root
const char* root_ca = TIBBER_ROOT_CA;

WiFiClientSecure client;

void setup() {
  Serial.begin(9600);
  delay(1000);

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected!");

  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

  IPAddress ip;
  if (!WiFi.hostByName(host, ip)) {
    Serial.println("DNS lookup failed");
    return;
  }
  Serial.print("Resolved IP: ");
  Serial.println(ip);

  //client.setDebugLevel(2);
  client.setCACert(root_ca);

  if (!client.connect(host, port)) {
    Serial.println("Connection failed!");
    return;
  }
  Serial.println("Connected");


  // WebSocket handshake
  String wsKey = generateWebSocketKey();
  client.printf("GET %s HTTP/1.1\r\n", path);
  client.printf("Host: %s\r\n", host);
  client.println("Upgrade: websocket");
  client.println("Connection: Upgrade");
  client.println("Sec-WebSocket-Key: " + wsKey);
  client.println("Sec-WebSocket-Version: 13");
  client.println("Sec-WebSocket-Protocol: graphql-transport-ws");
  client.println("Authorization: Bearer " + String(TIBBER_TOKEN));
  client.println("User-Agent: Homey/10.0.0 com.tibber/1.8.3");
  client.println();

  Serial.println("Recieving headers");
  // Lees headers
  while (client.connected()) {
    String line = client.readStringUntil('\n');
    if (line == "\r") break;
    Serial.println(line);
  }
  sendGraphQLInit();

  Serial.println("WebSocket connected");
  delay(500);
}

bool subscriptionStarted = false;
unsigned long lastPing = 0;

void loop() {
  if (millis() - lastPing > 10000) {
    sendPing();
    lastPing = millis();
  }

  if (client.available()) {
    String payload = readWebSocketFrame();  // zie decoder hieronder

    if (payload.length() > 0) {
      Serial.println("Ontvangen payload:");
      Serial.println(payload);

      DynamicJsonDocument doc(1024);
      DeserializationError error = deserializeJson(doc, payload);
      if (error) {
        Serial.println("JSON parse error");
        return;
      }

      const char* type = doc["type"];
      if (strcmp(type, "connection_ack") == 0) {
        Serial.println("✅ Connection acknowledged door server");

        if (!subscriptionStarted) {
          sendGraphQLStart();
          subscriptionStarted = true;
        }
      } else if (strcmp(type, "next") == 0) {
        Serial.println("📦 Nieuwe meetdata ontvangen:");
        serializeJsonPretty(doc["payload"], Serial);
      } else if (strcmp(type, "error") == 0) {
        Serial.println("❌ Fout ontvangen:");
        serializeJsonPretty(doc, Serial);
      }
      else
      {
        Serial.print("ander type:");
        Serial.println(type);
        serializeJsonPretty(doc, Serial);
      }
    }
  }
  if (millis() - lastPing > 10000) {
    sendPing();
    lastPing = millis();
  }
}


String readWebSocketFrame() {
  if (!client.available()) return "";

  uint8_t b1 = client.read();
  uint8_t b2 = client.read();

  bool masked = b2 & 0x80;
  uint64_t payloadLength = b2 & 0x7F;

  if (payloadLength == 126) {
    payloadLength = ((uint16_t)client.read() << 8) | client.read();
  } else if (payloadLength == 127) {
    payloadLength = 0;
    for (int i = 0; i < 8; i++) {
      payloadLength = (payloadLength << 8) | client.read();
    }
  }

  uint8_t mask[4] = { 0, 0, 0, 0 };
  if (masked) {
    for (int i = 0; i < 4; i++) mask[i] = client.read();
  }

  String payload = "";
  for (uint64_t i = 0; i < payloadLength; i++) {
    char c = client.read();
    if (masked) c ^= mask[i % 4];
    payload += c;
  }

  return payload;
}


void sendWebSocketFrame(String payload) {
  size_t len = payload.length();
  uint8_t header[14];  // enough for extended header + mask
  size_t headerLen = 0;

  header[0] = 0x81;  // FIN + text frame
  uint8_t maskBit = 0x80;

  if (len <= 125) {
    header[1] = maskBit | len;
    headerLen = 2;
  } else if (len <= 65535) {
    header[1] = maskBit | 126;
    header[2] = (len >> 8) & 0xFF;
    header[3] = len & 0xFF;
    headerLen = 4;
  } else {
    header[1] = maskBit | 127;
    for (int i = 0; i < 8; i++) {
      header[2 + i] = (len >> (56 - 8 * i)) & 0xFF;
    }
    headerLen = 10;
  }

  // Generate random mask
  uint8_t mask[4];
  for (int i = 0; i < 4; i++) {
    mask[i] = random(0, 256);
    header[headerLen++] = mask[i];
  }

  // Mask the payload
  uint8_t maskedPayload[len];
  for (size_t i = 0; i < len; i++) {
    maskedPayload[i] = payload[i] ^ mask[i % 4];
  }

  client.write(header, headerLen);
  client.write(maskedPayload, len);
}




void sendGraphQLInit() {
  DynamicJsonDocument doc(256);
  doc["type"] = "connection_init";
  doc["payload"] = JsonObject();  // must be present

  String msg;
  serializeJson(doc, msg);
  sendWebSocketFrame(msg);
  Serial.println("Sent connection_init");
}

void sendGraphQLStart() {
  DynamicJsonDocument doc(1024);
  doc["id"] = "1";
  doc["type"] = "subscribe";

  JsonObject payload = doc.createNestedObject("payload");
  payload["query"] = String("subscription{liveMeasurement(homeId:\"") + TIBBER_HOME_ID + "\"){power}}";

  String msg;
  serializeJson(doc, msg);
  sendWebSocketFrame(msg);
  // Serializeer en verstuur
  String jsonString;
  serializeJson(doc, jsonString);

  Serial.println("Versturen abonnement: " + jsonString);

  Serial.println("Sent subscription start");
}

String generateWebSocketKey() {
  uint8_t randomBytes[16];
  for (int i = 0; i < 16; i++) {
    randomBytes[i] = random(0, 256);
  }

  // Base64 encode
  String key = "";
  const char base64Chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

  int i = 0;
  while (i < 16) {
    uint32_t buffer = (randomBytes[i++] << 16);
    if (i < 16) buffer |= (randomBytes[i++] << 8);
    if (i < 16) buffer |= randomBytes[i++];

    key += base64Chars[(buffer >> 18) & 0x3F];
    key += base64Chars[(buffer >> 12) & 0x3F];
    key += base64Chars[(buffer >> 6) & 0x3F];
    key += base64Chars[buffer & 0x3F];
  }

  return key.substring(0, 24);  // WebSocket spec vereist 24 tekens
}

void sendPing() {
  uint8_t pingFrame[2] = {0x89, 0x00};  // opcode 0x9 (ping), no payload
  client.write(pingFrame, 2);
  Serial.println("Sent ping");
}

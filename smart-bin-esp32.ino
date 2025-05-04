#include <WiFi.h>
#include <WiFiClientSecure.h> // Needed for HTTPS
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <HardwareSerial.h> // Required for Serial2
#include <TinyGPS++.h>      // GPS parsing library

// --- Configuration ---
const char* ssid = "SLT FIBRE .2.4G";         // Your WiFi SSID
const char* password = "EqsV2.0$442##";       // Your WiFi Password
const char* serverUrl = "https://wctsystem-backend.onrender.com"; // Your backend server URL (HTTPS)

// !!! VERY IMPORTANT: Replace this with the actual MongoDB _id for THIS specific bin !!!
const char* binId = "68167bd9578b5cc6100b2f74";

// HC-SR04 Pins (Using Option 1 - Right Side)
const int trigPin = 23;
const int echoPin = 22; // Remember the voltage divider for this pin!

// NEO-6M GPS Pins (Using Serial Port 2 - Right Side)
// RX2 = GPIO 16, TX2 = GPIO 17
HardwareSerial& gpsSerial = Serial2; // Use Serial2 for GPS

// Bin/Sensor Settings
const float MAX_BIN_DEPTH_CM = 37.5; // Your specific bin depth in cm
const unsigned long GPS_READ_TIMEOUT_MS = 1000; // How long to wait for GPS data in each loop cycle
const unsigned long DATA_SEND_INTERVAL_MS = 60000; // Send data every 60 seconds
const unsigned long ULTRASONIC_READ_INTERVAL_MS = 5000; // Read sensor every 5 seconds
const unsigned long LOCATION_SEND_INTERVAL_MS = 18000000; // Send location every 5 hours (5 * 60 * 60 * 1000)

// --- Global Variables ---
WiFiClientSecure client; // Use secure client for HTTPS
HTTPClient http;
TinyGPSPlus gps;
float currentFillLevel = -1.0; // Use float for potential precision, -1 indicates no valid reading yet
float lastSentFillLevel = -1.0;
unsigned long lastUltrasonicReadTime = 0;
unsigned long lastDataSendTime = 0;
unsigned long lastLocationSendTime = 0; // Track when location was last sent

// --- Function Prototypes ---
float getUltrasonicDistanceCm();
float calculateFillLevel(float distanceCm);
void connectWiFi();
void sendDataToBackend(float fillLevel);
void sendLocationToBackend();
void displayGPSInfo();
bool isGPSValid();

// --- Setup Function ---
void setup() {
  Serial.begin(115200); // Start Serial Monitor for debugging
  while (!Serial); // Wait for Serial to connect (important for some boards)
  Serial.println("\n\n--- WCTSystem Bin Sensor Booting ---");
  Serial.print("Target Bin ID: ");
  Serial.println(binId);
  Serial.print("Max Bin Depth: ");
  Serial.print(MAX_BIN_DEPTH_CM);
  Serial.println(" cm");

  // Initialize HC-SR04 pins
  pinMode(trigPin, OUTPUT);
  pinMode(echoPin, INPUT);
  Serial.println("HC-SR04 Pins Initialized (Trig: 23, Echo: 22).");

  // Initialize GPS Serial communication (Baud rate for NEO-6M is typically 9600)
  // gpsSerial.begin(BaudRate, Protocol, RX Pin, TX Pin);
  gpsSerial.begin(9600, SERIAL_8N1, 16, 17); // RX=16, TX=17
  if (!gpsSerial) {
    Serial.println("Serial2 initialization failed! Check pin definitions.");
    while (1); // Halt execution
  }
  Serial.println("GPS Serial Port Initialized (Serial2: RX=16, TX=17). Waiting for GPS fix...");

  // --- HTTPS Setup ---
  // WARNING: Skipping certificate validation. Not secure for production!
  // For production, replace setInsecure() with setCACert() and provide the root CA certificate.
  client.setInsecure();
  // Example (replace with actual cert):
  // const char* rootCACertificate = \
  // "-----BEGIN CERTIFICATE-----\n" \
  // "MIIDdzCCAl+gAwIBAgIEAgAAuTANBgkqhkiG9w0BAQUFADBaMQswCQYDVQQGEwJJ\n" \
  // "... (rest of certificate) ... \n" \
  // "-----END CERTIFICATE-----\n";
  // client.setCACert(rootCACertificate);

  // Connect to WiFi
  connectWiFi();
  
  // Initialize timing variables
  lastUltrasonicReadTime = 0;
  lastDataSendTime = 0;
  lastLocationSendTime = 0;

  Serial.println("Setup Complete. Entering loop...");
}

// --- Main Loop ---
void loop() {
  unsigned long currentTime = millis();

  // --- Read GPS ---
  unsigned long gpsStartTime = millis();
  while (gpsSerial.available() > 0 && millis() - gpsStartTime < GPS_READ_TIMEOUT_MS) {
    if (gps.encode(gpsSerial.read())) {
       // Data parsed, TinyGPS object updated. We'll display it before sending.
    }
  }

  // --- Read Ultrasonic Sensor periodically ---
  if (currentTime - lastUltrasonicReadTime >= ULTRASONIC_READ_INTERVAL_MS) {
    float distance = getUltrasonicDistanceCm();
    Serial.print("Measured Distance: ");
    if (distance >= 0) {
      Serial.print(distance, 2); // Print with 2 decimal places
      Serial.print(" cm");
      currentFillLevel = calculateFillLevel(distance);
      Serial.print(" | Calculated Fill Level: ");
      Serial.print(currentFillLevel, 1); // Print with 1 decimal place
      Serial.println("%");
    } else {
      Serial.println("Invalid Reading");
      // Keep last valid reading or handle error
    }
    lastUltrasonicReadTime = currentTime;
  }

  // --- Send Fill Level Data to Backend periodically ---
  bool fillLevelChanged = abs(currentFillLevel - lastSentFillLevel) > 1.0; // Send if changed by > 1%
  bool intervalPassed = currentTime - lastDataSendTime >= DATA_SEND_INTERVAL_MS;

  if (currentFillLevel >= 0 && (fillLevelChanged || intervalPassed)) {
      Serial.println("----------------------------------------");
      Serial.println("Preparing to send fill level data...");
      displayGPSInfo(); // Display current GPS info before sending (for debugging)
      sendDataToBackend(currentFillLevel);
      lastSentFillLevel = currentFillLevel; // Update last sent value
      lastDataSendTime = currentTime;       // Reset send timer
      Serial.println("----------------------------------------");
  }
  
  // --- Send Location Data to Backend periodically (every 5 hours) ---
  if (currentTime - lastLocationSendTime >= LOCATION_SEND_INTERVAL_MS) {
      if (isGPSValid()) {
          Serial.println("----------------------------------------");
          Serial.println("Preparing to send location data...");
          displayGPSInfo();
          sendLocationToBackend();
          lastLocationSendTime = currentTime; // Reset location send timer
          Serial.println("----------------------------------------");
      } else {
          Serial.println("Cannot send location - waiting for valid GPS fix");
      }
  }

  // Small delay
  delay(100);
}

// --- Function Definitions ---

// Connect to WiFi Network
void connectWiFi() {
  Serial.print("Connecting to WiFi: ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    attempts++;
    if (attempts > 30) { // Increased timeout to ~15 seconds
      Serial.println("\nFailed to connect to WiFi. Retrying...");
      attempts = 0;
      WiFi.disconnect(true); // Force disconnect
      delay(1000);
      WiFi.mode(WIFI_STA); // Ensure STA mode
      WiFi.begin(ssid, password);
    }
  }
  Serial.println("\nWiFi Connected!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
}

// Get distance from HC-SR04 sensor
float getUltrasonicDistanceCm() {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);

  // Reads the echoPin, returns the sound wave travel time in microseconds
  // Timeout after 30ms (prevents blocking if echo never received)
  long duration = pulseIn(echoPin, HIGH, 30000);

  if (duration <= 0) {
    Serial.println(" -> Ultrasonic pulseIn timeout or error.");
    return -1.0; // Indicate error or timeout
  }

  // Speed of sound = 343 m/s = 0.0343 cm/µs
  // Distance = (Time * SpeedOfSound) / 2 (for round trip)
  float distanceCm = (duration * 0.0343) / 2.0;

  return distanceCm;
}

// Calculate fill level percentage (REVISED FUNCTION)
float calculateFillLevel(float distanceCm) {
  if (distanceCm < 0 || distanceCm > (MAX_BIN_DEPTH_CM + 10.0) ) { // Check validity and range (allow 10cm overshoot)
    // If reading is invalid OR significantly larger than bin depth
    Serial.print(" -> Dist reading out of range (");
    Serial.print(distanceCm);
    Serial.print("cm), assuming empty.");
    return 0.0; // Treat out-of-range readings as empty
  }

  // Calculate the raw fill percentage: ((Total Depth - Empty Space) / Total Depth) * 100
  float fillLevel = ((MAX_BIN_DEPTH_CM - distanceCm) / MAX_BIN_DEPTH_CM) * 100.0;

  // Clamp the *calculated percentage* between 0 and 100
  fillLevel = max(0.0f, min(fillLevel, 100.0f));

  return fillLevel;
}


// Send data to the backend API via HTTPS
void sendDataToBackend(float fillLevel) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi disconnected. Attempting to reconnect...");
    connectWiFi();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("Reconnect failed. Skipping data send.");
      return;
    }
  }

  String endpoint = String(serverUrl) + "/api/bins/" + String(binId) + "/update-fill-level";
  Serial.print("Sending PUT request to: ");
  Serial.println(endpoint);

  // Prepare JSON payload
  StaticJsonDocument<100> jsonDoc;
  jsonDoc["fillLevel"] = round(fillLevel); // Send rounded integer fill level

  String requestBody;
  serializeJson(jsonDoc, requestBody);
  Serial.print("Request Body: ");
  Serial.println(requestBody);

  // Start the HTTPS request using the secure client
  http.begin(client, endpoint); // Pass the WiFiClientSecure object
  http.addHeader("Content-Type", "application/json");

  // Send the PUT request
  int httpResponseCode = http.PUT(requestBody);

  // Check the response
  if (httpResponseCode > 0) {
    Serial.print("HTTP Response Code: ");
    Serial.println(httpResponseCode);
    String responsePayload = http.getString();
    Serial.print("Response Payload: ");
    Serial.println(responsePayload);

    if (httpResponseCode == HTTP_CODE_OK || httpResponseCode == HTTP_CODE_NO_CONTENT || httpResponseCode == HTTP_CODE_CREATED) {
        Serial.println("Data sent successfully!");
    } else {
        Serial.print("Error sending data. HTTP Status: ");
        Serial.println(httpResponseCode); // Will show 404, 500, etc.
    }
  } else {
    Serial.print("HTTP Request failed. Error: ");
    Serial.println(http.errorToString(httpResponseCode).c_str());
  }

  // End the connection
  http.end();
}

// Send location data to the backend API via HTTPS
void sendLocationToBackend() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi disconnected. Attempting to reconnect...");
    connectWiFi();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("Reconnect failed. Skipping location update.");
      return;
    }
  }

  // Only send if we have valid GPS coordinates
  if (!isGPSValid()) {
    Serial.println("No valid GPS fix. Skipping location update.");
    return;
  }

  String endpoint = String(serverUrl) + "/api/bins/direct-update";
  Serial.print("Sending POST request to: ");
  Serial.println(endpoint);

  // Prepare JSON payload with location data in GeoJSON format
  StaticJsonDocument<200> jsonDoc;
  jsonDoc["binId"] = binId;
  
  // Create a nested "updates" object
  JsonObject updates = jsonDoc.createNestedObject("updates");
  
  // Create a nested "location" object
  JsonObject location = updates.createNestedObject("location");
  location["type"] = "Point";
  
  // Create coordinates array [longitude, latitude]
  JsonArray coordinates = location.createNestedArray("coordinates");
  coordinates.add(gps.location.lng()); // Longitude first in GeoJSON
  coordinates.add(gps.location.lat()); // Latitude second in GeoJSON

  String requestBody;
  serializeJson(jsonDoc, requestBody);
  Serial.print("Request Body: ");
  Serial.println(requestBody);

  // Start the HTTPS request using the secure client
  http.begin(client, endpoint); // Pass the WiFiClientSecure object
  http.addHeader("Content-Type", "application/json");

  // Send the POST request
  int httpResponseCode = http.POST(requestBody);

  // Check the response
  if (httpResponseCode > 0) {
    Serial.print("HTTP Response Code: ");
    Serial.println(httpResponseCode);
    String responsePayload = http.getString();
    Serial.print("Response Payload: ");
    Serial.println(responsePayload);

    if (httpResponseCode == HTTP_CODE_OK || httpResponseCode == HTTP_CODE_CREATED) {
        Serial.println("Location data sent successfully!");
    } else {
        Serial.print("Error sending location data. HTTP Status: ");
        Serial.println(httpResponseCode);
    }
  } else {
    Serial.print("HTTP Request failed. Error: ");
    Serial.println(http.errorToString(httpResponseCode).c_str());
  }

  // End the connection
  http.end();
}

// Helper function to display GPS info (for debugging)
void displayGPSInfo() {
  Serial.print("GPS Status: ");
  if (gps.location.isValid()) {
    Serial.print("Loc: ");
    Serial.print(gps.location.lat(), 6); // Print with 6 decimal places
    Serial.print(",");
    Serial.print(gps.location.lng(), 6);
  } else {
    Serial.print("Loc Invalid");
  }

  Serial.print(" | Date/Time: ");
  if (gps.date.isValid()) {
    Serial.printf("%02d/%02d/%d", gps.date.day(), gps.date.month(), gps.date.year());
  } else {
    Serial.print("Date Invalid");
  }
  Serial.print(" ");
  if (gps.time.isValid()) {
    Serial.printf("%02d:%02d:%02d", gps.time.hour(), gps.time.minute(), gps.time.second());
  } else {
     Serial.print("Time Invalid");
  }

   Serial.print(" | Sats: ");
   if (gps.satellites.isValid()) {
      Serial.print(gps.satellites.value());
   } else {
      Serial.print("N/A");
   }
   Serial.println(); // Newline after GPS info
}

// Check if GPS has a valid fix and coordinates
bool isGPSValid() {
  if (gps.location.isValid() && 
      gps.location.lat() != 0.0 && 
      gps.location.lng() != 0.0) {
    return true;
  }
  return false;
}
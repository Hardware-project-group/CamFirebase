#include <Arduino.h>
#include "esp_camera.h"
#include "FS.h"
#include "SD_MMC.h"
#include "WiFi.h"
#include "HTTPClient.h"
#include "base64.h"
#include "time.h"
#include <Preferences.h>
#include <WebServer.h>

// Define the camera model
#define CAMERA_MODEL_AI_THINKER
#include "camera_pins.h"

String getFolderName(String attempt);
void uploadImageToFirebaseStorage(const uint8_t *image, size_t len, const String &folderName, const String &fileName);
void uploadFolderToFirebase(const String &folderName);
void uploadMetadataToFirebaseDatabase(const String &downloadURL, const String &folderName, const String &fileName);
void captureAndSaveImage(const String &folderName, int imageIndex);
void capture(String fname);
void handler();
void sendIp(String ip);
void send();

// Network credentials
const char* ssid = "MSI5554";
const char* password = "12345678";
WebServer server(80);
HTTPClient http;

String foldernm;

// Firebase project details
const char* firebaseHost = "stocksync-a0ce8-default-rtdb.firebaseio.com/";
const char* firebaseAuth = "AIzaSyBMDn0sq9nH9TnwSKztGO0f91Ym6zzZD3w";
const char* firebaseStorageBucket = "stocksync-a0ce8.appspot.com";

// Time setup
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 5 * 3600; // GMT+5 hours
const int daylightOffset_sec = 30 * 60; // +30 minutes

const int numImagesPerAttempt = 4;

// Global variable to track attempt number
int attemptNumber = 1;

// Preferences object to store the attempt number persistently
Preferences preferences;

void startCamera() {
    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;
    config.pin_d0 = Y2_GPIO_NUM;
    config.pin_d1 = Y3_GPIO_NUM;
    config.pin_d2 = Y4_GPIO_NUM;
    config.pin_d3 = Y5_GPIO_NUM;
    config.pin_d4 = Y6_GPIO_NUM;
    config.pin_d5 = Y7_GPIO_NUM;
    config.pin_d6 = Y8_GPIO_NUM;
    config.pin_d7 = Y9_GPIO_NUM;
    config.pin_xclk = XCLK_GPIO_NUM;
    config.pin_pclk = PCLK_GPIO_NUM;
    config.pin_vsync = VSYNC_GPIO_NUM;
    config.pin_href = HREF_GPIO_NUM;
    config.pin_sccb_sda = SIOD_GPIO_NUM;
    config.pin_sccb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn = PWDN_GPIO_NUM;
    config.pin_reset = RESET_GPIO_NUM;
    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_JPEG;
    if (psramFound()) {
        config.frame_size = FRAMESIZE_UXGA;
        config.jpeg_quality = 10;
        config.fb_count = 2;
    } else {
        config.frame_size = FRAMESIZE_SVGA;
        config.jpeg_quality = 12;
        config.fb_count = 1;
    }
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("Camera init failed with error 0x%x\n", err);
        return;
    }
}

void setup() {
    Serial.begin(115200);
    if (!SD_MMC.begin()) {
        Serial.println("Card Mount Failed");
        return;
    }
    uint8_t cardType = SD_MMC.cardType();
    if (cardType == CARD_NONE) {
        Serial.println("No SD card attached");
        return;
    }
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(1000);
        Serial.println("Connecting to WiFi...");
    }
    Serial.println("Connected to WiFi");
    startCamera();
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        Serial.println("Failed to obtain time");
    } else {
        Serial.println("Time obtained successfully");
    }
    IPAddress ip = WiFi.localIP();
    String ipString = ip.toString();
    sendIp(ipString);
    // Initialize Preferences and get the attempt number
    server.on("/capture", HTTP_POST, handler);
    server.on("/send", HTTP_POST, send);
    server.begin();
}

void send(){
  uploadFolderToFirebase(foldernm);
}

void handler() {
    Serial.println("Request came");
    if (server.hasArg("ID")) {
        String paramValueStr = server.arg("ID");
        String folderName = getFolderName(paramValueStr);
        capture(folderName);
    }
}

void capture(String fname) {
    preferences.begin("camera", false);
    attemptNumber = preferences.getInt("attemptNumber", 1);
    Serial.println("Setup complete. Capturing images...");
    // Generate folder name based on attempt number
    fs::FS &fs = SD_MMC;
    fs.mkdir("/" + fname);
    for (int i = 0; i < numImagesPerAttempt; i++) {
        captureAndSaveImage(fname, i);
        delay(2000);
    }
    // Increment attempt number for the next session and save it persistently
    attemptNumber++;
    preferences.putInt("attemptNumber", attemptNumber);
    preferences.end();
}

void loop() {
    server.handleClient();
    // Loop is left empty because capturing and uploading images is handled in setup()
}

String getFolderName(String attempt) {
    foldernm =  String(attempt);
    return String(attempt);
}

void captureAndSaveImage(const String& folderName, int imageIndex) {
    camera_fb_t * fb = esp_camera_fb_get();
    if (!fb) {
        Serial.println("Camera capture failed");
        return;
    }
    int randomNumber = random(1, 1000001); // Generate random number between 1 and 1,000,000
    String path = "/" + folderName + "/image" + String(randomNumber) + ".jpg";
    fs::FS &fs = SD_MMC;
    File file = fs.open(path.c_str(), FILE_WRITE);
    if (!file) {
        Serial.println("Failed to open file in writing mode");
    } else {
        file.write(fb->buf, fb->len);
        Serial.printf("Saved file to path: %s\n", path.c_str());
    }
    file.close();
    esp_camera_fb_return(fb);
}

void uploadImageToFirebaseStorage(const uint8_t *image, size_t len, const String& folderName, const String& fileName) {
    String url = "https://firebasestorage.googleapis.com/v0/b/" + String(firebaseStorageBucket) + "/o/" + folderName + "%2F" + fileName + "?uploadType=media&name=" + folderName + "/" + fileName;
    HTTPClient http;
    http.begin(url);
    http.addHeader("Content-Type", "image/jpeg");
    int httpResponseCode = http.POST((uint8_t*)image, len);
    if (httpResponseCode > 0) {
        String response = http.getString();
        Serial.println(httpResponseCode);
        Serial.println(response);
        int start = response.indexOf("\"downloadTokens\":\"") + 18;
        int end = response.indexOf("\"", start);
        String downloadURL = "https://firebasestorage.googleapis.com/v0/b/" + String(firebaseStorageBucket) + "/o/" + folderName + "%2F" + fileName + "?alt=media&token=" + response.substring(start, end);
        uploadMetadataToFirebaseDatabase(downloadURL, folderName, fileName);
    } else {
        Serial.print("Error on sending POST: ");
        Serial.println(httpResponseCode);
        Serial.println(http.errorToString(httpResponseCode).c_str());
    }
    http.end();
}

void uploadMetadataToFirebaseDatabase(const String& downloadURL, const String& folderName, const String& fileName) {
    String url = "https://" + String(firebaseHost) + "/images.json?auth=" + String(firebaseAuth);
    HTTPClient http;
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    String jsonPayload = "{\"url\":\"" + downloadURL + "\", \"folder\":\"" + folderName + "\", \"file\":\"" + fileName + "\"}";
    int httpResponseCode = http.POST(jsonPayload);
    if (httpResponseCode > 0) {
        String response = http.getString();
        Serial.println(httpResponseCode);
        Serial.println(response);
    } else {
        Serial.print("Error on sending POST: ");
        Serial.println(httpResponseCode);
        Serial.println(http.errorToString(httpResponseCode).c_str());
    }
    http.end();
}

void uploadFolderToFirebase(const String& folderName) {
    fs::FS &fs = SD_MMC;
    File root = fs.open("/" + folderName);
    if (!root) {
        Serial.printf("Failed to open directory: %s\n", folderName.c_str());
        return;
    }
    File file = root.openNextFile();
    while (file) {
        size_t fileSize = file.size();
        uint8_t *buffer = (uint8_t*) malloc(fileSize);
        file.read(buffer, fileSize);
        String fileName = file.name();
        fileName = fileName.substring(fileName.lastIndexOf('/') + 1); // Extract just the filename
        uploadImageToFirebaseStorage(buffer, fileSize, folderName, fileName);
        free(buffer);
        file.close();
        file = root.openNextFile();
    }
    Serial.printf("Folder %s has been uploaded to Firebase.\n", folderName.c_str());
}

void sendIp(String ip){
    http.begin("http://192.168.137.1:5000/SendIp");
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    String postdataIP = "Board=ESP3&ip=" + ip;
    int httpResponseCode = http.POST(postdataIP);
    Serial.println("IpSend Statues: ");
    Serial.println(httpResponseCode);
    http.end();
}
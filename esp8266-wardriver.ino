#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <FS.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// Настройки OLED дисплея
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// Веб-сервер
ESP8266WebServer server(80);

// Настройки точки доступа
const char* ap_ssid = "Wardriver-AP";
const char* ap_password = "wardriver123";

// Файл для хранения данных
const char* dataFile = "/wardriver_data.txt";
const char* cacheFile = "/network_cache.txt";

// Переменные для сканирования
unsigned long lastScanTime = 0;
const unsigned long scanInterval = 10000; // 10 секунд
int totalNetworks = 0;
int networksInFile = 0;

// Настройки фильтрации дубликатов
const int CACHE_SIZE = 200; // Максимальное количество сетей в кэше
const unsigned long MIN_TIME_BETWEEN_SAVES = 300000; // 5 минут в миллисекундах
const int MIN_RSSI_DIFF = 10; // Минимальная разница RSSI для сохранения

// Структура для кэширования сетей
struct NetworkCache {
  String bssid;
  unsigned long lastSaved;
  int lastRssi;
  bool saved;
};

NetworkCache networkCache[CACHE_SIZE];
int cacheCount = 0;

void setup() {
  Serial.begin(115200);
  
  // Инициализация OLED
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("SSD1306 allocation failed"));
    for(;;);
  }
  
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(0,0);
  display.println("Wardriver v1.1");
  display.println("With duplicate check");
  display.display();
  delay(2000);
  
  // Инициализация файловой системы
  if(!SPIFFS.begin()) {
    Serial.println("Failed to mount file system");
    display.clearDisplay();
    display.setCursor(0,0);
    display.println("FS Error!");
    display.display();
    delay(2000);
    return;
  }
  
  // Загрузка кэша из файла
  loadNetworkCache();
  
  // Создание точки доступа
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(ap_ssid, ap_password);
  
  // Настройка веб-сервера
  setupWebServer();
  
  // Чтение количества сетей из файла
  countNetworksInFile();
  
  // Начальная информация на дисплее
  updateDisplay();
}

void loop() {
  server.handleClient();
  
  unsigned long currentTime = millis();
  if(currentTime - lastScanTime >= scanInterval) {
    scanNetworks();
    lastScanTime = currentTime;
  }
}

bool shouldSaveNetwork(const String& bssid, int rssi, unsigned long timestamp) {
  // Поиск сети в кэше
  for(int i = 0; i < cacheCount; i++) {
    if(networkCache[i].bssid == bssid) {
      unsigned long timeDiff = timestamp - networkCache[i].lastSaved;
      int rssiDiff = abs(rssi - networkCache[i].lastRssi);
      
      // Проверяем критерии сохранения
      if(timeDiff >= MIN_TIME_BETWEEN_SAVES || rssiDiff >= MIN_RSSI_DIFF) {
        // Обновляем кэш
        networkCache[i].lastSaved = timestamp;
        networkCache[i].lastRssi = rssi;
        networkCache[i].saved = true;
        return true;
      }
      
      // Обновляем время последнего обнаружения
      networkCache[i].lastSaved = timestamp;
      return false;
    }
  }
  
  // Сеть не найдена в кэше, добавляем новую
  if(cacheCount < CACHE_SIZE) {
    networkCache[cacheCount].bssid = bssid;
    networkCache[cacheCount].lastSaved = timestamp;
    networkCache[cacheCount].lastRssi = rssi;
    networkCache[cacheCount].saved = true;
    cacheCount++;
    return true;
  } else {
    // Если кэш полен, ищем самую старую запись для замены
    int oldestIndex = 0;
    unsigned long oldestTime = networkCache[0].lastSaved;
    
    for(int i = 1; i < CACHE_SIZE; i++) {
      if(networkCache[i].lastSaved < oldestTime) {
        oldestTime = networkCache[i].lastSaved;
        oldestIndex = i;
      }
    }
    
    // Заменяем старую запись
    networkCache[oldestIndex].bssid = bssid;
    networkCache[oldestIndex].lastSaved = timestamp;
    networkCache[oldestIndex].lastRssi = rssi;
    networkCache[oldestIndex].saved = true;
    return true;
  }
}

void saveNetworkCache() {
  File file = SPIFFS.open(cacheFile, "w");
  if(!file) {
    Serial.println("Failed to open cache file for writing");
    return;
  }
  
  for(int i = 0; i < cacheCount; i++) {
    String cacheEntry = networkCache[i].bssid + "," + 
                        String(networkCache[i].lastSaved) + "," + 
                        String(networkCache[i].lastRssi) + "\n";
    file.print(cacheEntry);
  }
  
  file.close();
  Serial.println("Network cache saved");
}

void loadNetworkCache() {
  if(!SPIFFS.exists(cacheFile)) {
    Serial.println("No cache file found, starting fresh");
    return;
  }
  
  File file = SPIFFS.open(cacheFile, "r");
  if(!file) {
    Serial.println("Failed to open cache file for reading");
    return;
  }
  
  cacheCount = 0;
  while(file.available() && cacheCount < CACHE_SIZE) {
    String line = file.readStringUntil('\n');
    if(line.length() > 0) {
      int firstComma = line.indexOf(',');
      int secondComma = line.indexOf(',', firstComma + 1);
      
      if(firstComma != -1 && secondComma != -1) {
        networkCache[cacheCount].bssid = line.substring(0, firstComma);
        networkCache[cacheCount].lastSaved = line.substring(firstComma + 1, secondComma).toInt();
        networkCache[cacheCount].lastRssi = line.substring(secondComma + 1).toInt();
        networkCache[cacheCount].saved = false;
        cacheCount++;
      }
    }
  }
  
  file.close();
  Serial.println("Network cache loaded: " + String(cacheCount) + " entries");
}

void scanNetworks() {
  display.clearDisplay();
  display.setCursor(0,0);
  display.println("Scanning...");
  display.println("Cache: " + String(cacheCount));
  display.display();
  
  int n = WiFi.scanNetworks(false, true);
  totalNetworks = n;
  
  if(n == 0) {
    display.clearDisplay();
    display.setCursor(0,0);
    display.println("No networks");
    display.println("found");
    display.display();
    return;
  }
  
  File file = SPIFFS.open(dataFile, "a");
  if(!file) {
    Serial.println("Failed to open file for writing");
    return;
  }
  
  unsigned long timestamp = millis();
  int savedThisScan = 0;
  int duplicatesSkipped = 0;
  
  for(int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    int32_t rssi = WiFi.RSSI(i);
    uint8_t encryptionType = WiFi.encryptionType(i);
    uint8_t* bssid = WiFi.BSSID(i);
    
    // Форматирование MAC-адреса
    String mac = "";
    for(int j = 0; j < 6; j++) {
      if(bssid[j] < 0x10) mac += "0";
      mac += String(bssid[j], HEX);
      if(j < 5) mac += ":";
    }
    mac.toUpperCase();
    
    // Форматирование типа шифрования
    String encType;
    switch(encryptionType) {
      case ENC_TYPE_NONE: encType = "OPEN"; break;
      case ENC_TYPE_WEP: encType = "WEP"; break;
      case ENC_TYPE_TKIP: encType = "WPA"; break;
      case ENC_TYPE_CCMP: encType = "WPA2"; break;
      case ENC_TYPE_AUTO: encType = "AUTO"; break;
      default: encType = "UNKNOWN";
    }
    
    // Проверяем, нужно ли сохранять эту сеть
    if(shouldSaveNetwork(mac, rssi, timestamp)) {
      // Формируем строку данных
      String data = String(timestamp) + "," + 
                    ssid + "," + 
                    mac + "," + 
                    String(rssi) + "," + 
                    encType + "\n";
      
      // Записываем в файл
      file.print(data);
      savedThisScan++;
      networksInFile++;
      
      // Выводим информацию на дисплей
      display.clearDisplay();
      display.setCursor(0,0);
      display.println("Scanning: " + String(i+1) + "/" + String(n));
      display.println("Saved: " + String(savedThisScan));
      display.println("Dups: " + String(duplicatesSkipped));
      
      // Исправленная строка: используем тернарный оператор вместо min()
      int ssidLen = ssid.length();
      int displayLen = (ssidLen > 16) ? 16 : ssidLen;
      display.println(ssid.substring(0, displayLen));
      
      int macLen = mac.length();
      displayLen = (macLen > 16) ? 16 : macLen;
      display.println(mac.substring(0, displayLen));
      display.display();
    } else {
      duplicatesSkipped++;
      
      // Периодически обновляем дисплей
      if(i % 5 == 0) {
        display.clearDisplay();
        display.setCursor(0,0);
        display.println("Scanning: " + String(i+1) + "/" + String(n));
        display.println("Saved: " + String(savedThisScan));
        display.println("Dups: " + String(duplicatesSkipped));
        display.println("Skipping duplicate...");
        display.display();
      }
    }
    
    delay(50); // Небольшая задержка для стабильности
  }
  
  file.close();
  
  // Сохраняем обновленный кэш
  saveNetworkCache();
  
  // Итоговая статистика на дисплее
  display.clearDisplay();
  display.setCursor(0,0);
  display.println("Scan complete!");
  display.println("Found: " + String(n));
  display.println("Saved: " + String(savedThisScan));
  display.println("Skipped: " + String(duplicatesSkipped));
  display.println("Total: " + String(networksInFile));
  display.display();
  delay(2000);
  
  // Обновляем дисплей
  updateDisplay();
}

void updateDisplay() {
  display.clearDisplay();
  display.setCursor(0,0);
  display.println("Wardriver v1.1");
  display.println("AP: " + String(ap_ssid));
  display.println("IP: " + WiFi.softAPIP().toString());
  display.println("Saved: " + String(networksInFile));
  display.println("Cache: " + String(cacheCount));
  display.display();
}

void countNetworksInFile() {
  if(!SPIFFS.exists(dataFile)) {
    networksInFile = 0;
    return;
  }
  
  File file = SPIFFS.open(dataFile, "r");
  if(!file) {
    networksInFile = 0;
    return;
  }
  
  int count = 0;
  while(file.available()) {
    if(file.read() == '\n') count++;
  }
  file.close();
  networksInFile = count;
}

void setupWebServer() {
  // Главная страница
  server.on("/", HTTP_GET, []() {
    String html = "<!DOCTYPE html><html><head>";
    html += "<meta charset='UTF-8'>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<title>Wardriver Data</title>";
    html += "<style>";
    html += "body { font-family: Arial, sans-serif; margin: 20px; }";
    html += "h1 { color: #333; }";
    html += ".container { max-width: 600px; margin: 0 auto; }";
    html += ".info { background: #f0f0f0; padding: 15px; border-radius: 5px; margin-bottom: 20px; }";
    html += ".stats-grid { display: grid; grid-template-columns: repeat(2, 1fr); gap: 10px; margin: 15px 0; }";
    html += ".stat-box { background: #e8f4f8; padding: 10px; border-radius: 5px; text-align: center; }";
    html += ".button { display: inline-block; padding: 10px 20px; background: #4CAF50; color: white; text-decoration: none; border-radius: 5px; margin-right: 10px; margin-bottom: 10px; }";
    html += ".button.delete { background: #f44336; }";
    html += ".button.cache { background: #2196F3; }";
    html += ".button:hover { opacity: 0.8; }";
    html += "</style></head><body>";
    html += "<div class='container'>";
    html += "<h1>📡 Wardriver Data Collector v1.1</h1>";
    html += "<div class='info'>";
    html += "<p><strong>Device:</strong> ESP8266 Wardriver with Duplicate Filter</p>";
    
    html += "<div class='stats-grid'>";
    html += "<div class='stat-box'><strong>Networks found:</strong><br>" + String(totalNetworks) + "</div>";
    html += "<div class='stat-box'><strong>Networks saved:</strong><br>" + String(networksInFile) + "</div>";
    html += "<div class='stat-box'><strong>Cache size:</strong><br>" + String(cacheCount) + "/" + String(CACHE_SIZE) + "</div>";
    
    File file = SPIFFS.open(dataFile, "r");
    int fileSize = 0;
    if(file) {
      fileSize = file.size();
      file.close();
    }
    html += "<div class='stat-box'><strong>File size:</strong><br>" + String(fileSize) + " bytes</div>";
    html += "</div>";
    
    html += "<p><strong>Duplicate filter:</strong> Active (5 min / 10 dBm threshold)</p>";
    html += "</div>";
    
    html += "<a class='button' href='/download'>📥 Download Data</a>";
    html += "<a class='button' href='/view'>👁️ View Data</a>";
    html += "<a class='button cache' href='/clearcache' onclick='return confirm(&quot;Clear network cache?&quot;)'>🗑️ Clear Cache</a>";
    html += "<a class='button delete' href='/delete' onclick='return confirm(&quot;Delete all data?&quot;)'>🗑️ Delete Data</a>";
    html += "<a class='button' href='/scan'>🔄 Scan Now</a>";
    html += "</div></body></html>";
    server.send(200, "text/html", html);
  });
  
  // Скачивание файла
  server.on("/download", HTTP_GET, []() {
    File file = SPIFFS.open(dataFile, "r");
    if(!file) {
      server.send(404, "text/plain", "File not found");
      return;
    }
    
    server.sendHeader("Content-Type", "text/csv");
    server.sendHeader("Content-Disposition", "attachment; filename=wardriver_data.csv");
    server.sendHeader("Connection", "close");
    
    server.streamFile(file, "text/csv");
    file.close();
  });
  
  // Просмотр данных
  server.on("/view", HTTP_GET, []() {
    File file = SPIFFS.open(dataFile, "r");
    if(!file) {
      server.send(404, "text/plain", "File not found");
      return;
    }
    
    String html = "<!DOCTYPE html><html><head>";
    html += "<meta charset='UTF-8'>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<title>View Data</title>";
    html += "<style>";
    html += "body { font-family: monospace; margin: 20px; }";
    html += "table { border-collapse: collapse; width: 100%; margin-top: 20px; font-size: 12px; }";
    html += "th, td { border: 1px solid #ddd; padding: 6px; text-align: left; }";
    html += "th { background-color: #4CAF50; color: white; position: sticky; top: 0; }";
    html += "tr:nth-child(even) { background-color: #f2f2f2; }";
    html += ".rssi-good { color: green; }";
    html += ".rssi-ok { color: orange; }";
    html += ".rssi-poor { color: red; }";
    html += "</style>";
    html += "<script>";
    html += "function formatTime(timestamp) {";
    html += "  var date = new Date(parseInt(timestamp));";
    html += "  return date.toLocaleTimeString();";
    html += "}";
    html += "function getRSSIClass(rssi) {";
    html += "  if(rssi >= -50) return 'rssi-good';";
    html += "  if(rssi >= -70) return 'rssi-ok';";
    html += "  return 'rssi-poor';";
    html += "}";
    html += "</script>";
    html += "</head><body>";
    html += "<h1>📋 Collected Data</h1>";
    html += "<a href='/'>← Back</a>";
    html += "<div style='height: 400px; overflow-y: auto;'>";
    html += "<table>";
    html += "<tr><th>Time</th><th>SSID</th><th>MAC</th><th>RSSI</th><th>Encryption</th></tr>";
    
    int rowCount = 0;
    while(file.available() && rowCount < 1000) { // Ограничиваем показ 1000 строк
      String line = file.readStringUntil('\n');
      if(line.length() > 0) {
        html += "<tr>";
        int commaPos;
        String cells[5];
        
        for(int i = 0; i < 5; i++) {
          commaPos = line.indexOf(',');
          cells[i] = line.substring(0, commaPos);
          line = line.substring(commaPos + 1);
        }
        
        // Время
        html += "<td><script>document.write(formatTime(" + cells[0] + "))</script></td>";
        
        // SSID
        html += "<td>" + cells[1] + "</td>";
        
        // MAC
        html += "<td>" + cells[2] + "</td>";
        
        // RSSI с цветовым кодированием
        int rssi = cells[3].toInt();
        String rssiClass = "rssi-poor";
        if(rssi >= -50) rssiClass = "rssi-good";
        else if(rssi >= -70) rssiClass = "rssi-ok";
        html += "<td class='" + rssiClass + "'>" + cells[3] + " dBm</td>";
        
        // Шифрование
        html += "<td>" + cells[4] + "</td>";
        
        html += "</tr>";
        rowCount++;
      }
    }
    
    html += "</table>";
    html += "</div>";
    html += "<p>Showing " + String(rowCount) + " rows</p>";
    html += "</body></html>";
    
    server.send(200, "text/html", html);
    file.close();
  });
  
  // Очистка кэша
  server.on("/clearcache", HTTP_GET, []() {
    cacheCount = 0;
    if(SPIFFS.remove(cacheFile)) {
      server.sendHeader("Location", "/");
      server.send(302, "text/plain", "Cache cleared");
    } else {
      server.send(500, "text/plain", "Failed to clear cache");
    }
  });
  
  // Удаление данных
  server.on("/delete", HTTP_GET, []() {
    if(SPIFFS.remove(dataFile)) {
      networksInFile = 0;
      server.sendHeader("Location", "/");
      server.send(302, "text/plain", "Deleted");
    } else {
      server.send(500, "text/plain", "Delete failed");
    }
  });
  
  // Ручной запуск сканирования
  server.on("/scan", HTTP_GET, []() {
    scanNetworks();
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "Scanning...");
  });
  
  server.begin();
  Serial.println("HTTP server started");
}
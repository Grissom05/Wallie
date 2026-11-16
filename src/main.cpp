#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <vector>
#include <time.h>
#include <esp_sntp.h>

// --- CREDENZIALI WI-FI ---
const char* ssid = "Lorenzo's Galaxy A34";
const char* password = "LolloPazzo";

// --- IMPOSTAZIONI NTP (ITALIA) ---
const char* ntpServer1 = "ntp1.inrim.it"; 
const char* ntpServer2 = "pool.ntp.org";  
const char* fusoOrarioItalia = "CET-1CEST,M3.5.0,M10.5.0/3";
const uint32_t INTERVALLO_SYNC_NTP = 3600000UL; 

WebServer server(80);

// --- STRUTTURE DATI ---
struct Medicina {
  unsigned long id;
  String name;
  int slot;
  int stock;
  int capacity;
  int low;
  int dose;
  bool water;
  std::vector<String> times;
  std::vector<int> days; // 0=Lunedì, ..., 6=Domenica
  bool giaSuonataOggi;
};

struct Profilo {
  String name = "";
  String notify = "";
  String pin = "1234";
  bool registrato = false;
};

struct DispenserState {
  int slots = 3;       
  String water = "ok"; 
};

Profilo profiloUtente;
DispenserState dispenserState;
std::vector<Medicina> listaMedicine;

char ultimoSyncStr[16] = "In attesa...";
volatile bool orologioSincronizzato = false;

// --- GESTIONE ALLARME LED ---
bool allarmeAttivo = false;
bool statoLed = false;
unsigned long inizioAllarmeMillis = 0;
unsigned long ultimoBlinkMillis = 0;
const unsigned long DURATA_ALLARME = 10000; 
const unsigned long INTERVALLO_BLINK = 250; 

// --- INTERRUPT PULSANTE BOOT ---
const int BOOT_BUTTON = 0;
volatile bool fermaAllarmeFlag = false;
volatile unsigned long ultimoInterruptMillis = 0;

void IRAM_ATTR fermaAllarmeISR() {
  unsigned long ora = millis();
  if (ora - ultimoInterruptMillis > 200) {
    fermaAllarmeFlag = true;
    ultimoInterruptMillis = ora;
  }
}

void avviaAllarme() {
  allarmeAttivo = true;
  inizioAllarmeMillis = millis();
  ultimoBlinkMillis = millis();
  statoLed = true;
  rgbLedWrite(RGB_BUILTIN, 255, 0, 100);
  Serial.println("\n[ALARM] Allarme attivato! LED RGB in lampeggio per 10 secondi.");
}

void spegniAllarme() {
  allarmeAttivo = false;
  statoLed = false;
  rgbLedWrite(RGB_BUILTIN, 0, 0, 0);
  Serial.println("[ALARM] Allarme terminato. LED spento.");
}

void callbackSincronizzazioneNTP(struct timeval *tv) {
  orologioSincronizzato = true;
  time_t now = time(nullptr);
  struct tm* t = localtime(&now);
  snprintf(ultimoSyncStr, sizeof(ultimoSyncStr), "%02d:%02d:%02d", t->tm_hour, t->tm_min, t->tm_sec);
  Serial.printf("\n[NTP] Orario sincronizzato: %s\n", ultimoSyncStr);
}

// ==========================================
// PERSISTENZA DEI DATI (LittleFS)
// ==========================================
void salvaDati() {
  JsonDocument doc;
  
  doc["prof"]["name"] = profiloUtente.name;
  doc["prof"]["notify"] = profiloUtente.notify;
  doc["prof"]["pin"] = profiloUtente.pin;
  doc["prof"]["reg"] = profiloUtente.registrato;

  JsonArray meds = doc["meds"].to<JsonArray>();
  for (auto& m : listaMedicine) {
    JsonObject obj = meds.add<JsonObject>();
    obj["id"] = m.id;
    obj["name"] = m.name;
    obj["slot"] = m.slot;
    obj["stock"] = m.stock;
    obj["capacity"] = m.capacity;
    obj["low"] = m.low;
    obj["dose"] = m.dose;
    obj["water"] = m.water;
    
    JsonArray tArr = obj["times"].to<JsonArray>();
    for (auto& t : m.times) tArr.add(t);
    
    JsonArray dArr = obj["days"].to<JsonArray>();
    for (auto& d : m.days) dArr.add(d);
  }

  File file = LittleFS.open("/config.json", "w");
  if (file) {
    serializeJson(doc, file);
    file.close();
    Serial.println("[FS] Dati salvati con successo in /config.json");
  } else {
    Serial.println("[FS] ERRORE: Impossibile scrivere su /config.json");
  }
}

void caricaDati() {
  File file = LittleFS.open("/config.json", "r");
  if (!file) {
    Serial.println("[FS] Nessun file /config.json trovato. Uso i default.");
    return; 
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, file);
  file.close();
  
  if (err) {
    Serial.printf("[FS] ERRORE deserializzazione JSON: %s\n", err.c_str());
    return;
  }

  profiloUtente.name = doc["prof"]["name"].as<String>();
  profiloUtente.notify = doc["prof"]["notify"].as<String>();
  profiloUtente.pin = doc["prof"]["pin"].as<String>();
  profiloUtente.registrato = doc["prof"]["reg"].as<bool>();

  listaMedicine.clear();
  JsonArray medsArray = doc["meds"].as<JsonArray>();
  
  for (JsonObject obj : medsArray) {
    Medicina m;
    m.id = obj["id"].as<unsigned long>();
    m.name = obj["name"].as<String>();
    m.slot = obj["slot"].as<int>();
    m.stock = obj["stock"].as<int>();
    m.capacity = obj["capacity"].as<int>();
    m.low = obj["low"].as<int>();
    m.dose = obj["dose"].as<int>();
    m.water = obj["water"].as<bool>();
    
    JsonArray timesArray = obj["times"].as<JsonArray>();
    for (JsonVariant v : timesArray) m.times.push_back(v.as<String>());
    
    JsonArray daysArray = obj["days"].as<JsonArray>();
    for (JsonVariant v : daysArray) m.days.push_back(v.as<int>());
    
    m.giaSuonataOggi = false;
    listaMedicine.push_back(m);
  }
  
  Serial.printf("[FS] Dati caricati. Utente: %s | Medicine in memoria: %d\n", profiloUtente.name.c_str(), listaMedicine.size());
}

// ==========================================
// ROTTE FISSE API
// ==========================================
void handleApiProfileGet() {
  Serial.println("[API] GET /api/profile");
  if (!profiloUtente.registrato) {
    Serial.println("  -> 404 Utente non registrato");
    server.send(404, "application/json", "{\"error\":\"Not registered\"}");
    return;
  }
  JsonDocument doc;
  doc["name"] = profiloUtente.name;
  doc["notify"] = profiloUtente.notify;
  String res; 
  serializeJson(doc, res);
  server.send(200, "application/json", res);
}

void handleApiProfilePut() {
  Serial.println("[API] PUT /api/profile");
  JsonDocument doc;
  deserializeJson(doc, server.arg("plain"));
  
  if (doc["name"].is<String>()) profiloUtente.name = doc["name"].as<String>();
  if (doc["notify"].is<String>()) profiloUtente.notify = doc["notify"].as<String>();
  
  Serial.printf("  -> Profilo aggiornato: Nome='%s', Notify='%s'\n", profiloUtente.name.c_str(), profiloUtente.notify.c_str());
  salvaDati();
  server.send(200, "application/json", "{\"status\":\"ok\"}");
}

void handleApiLogin() {
  Serial.println("[API] POST /api/login");
  JsonDocument doc;
  deserializeJson(doc, server.arg("plain"));
  
  String attemptedPin = doc["pin"].as<String>();
  if (profiloUtente.registrato && attemptedPin == profiloUtente.pin) {
    Serial.println("  -> Accesso CONSENTITO");
    server.send(200, "application/json", "{\"token\":\"esp32-secure-token\"}");
  } else {
    Serial.println("  -> Accesso NEGATO (PIN errato)");
    server.send(403, "application/json", "{\"error\":\"Wrong PIN\"}");
  }
}

void handleApiRegister() {
  Serial.println("[API] POST /api/register");
  JsonDocument doc;
  deserializeJson(doc, server.arg("plain"));
  
  profiloUtente.name = doc["name"].as<String>();
  profiloUtente.pin = doc["pin"].as<String>();
  profiloUtente.registrato = true;
  
  Serial.printf("  -> Nuovo utente registrato: %s\n", profiloUtente.name.c_str());
  salvaDati();
  server.send(200, "application/json", "{\"token\":\"esp32-secure-token\"}");
}

void handleApiPinPut() {
  Serial.println("[API] PUT /api/pin");
  JsonDocument doc;
  deserializeJson(doc, server.arg("plain"));
  
  if (doc["oldPin"].as<String>() == profiloUtente.pin) {
    profiloUtente.pin = doc["newPin"].as<String>();
    Serial.println("  -> PIN modificato con successo");
    salvaDati();
    server.send(200, "application/json", "{\"status\":\"ok\"}");
  } else {
    Serial.println("  -> Cambio PIN negato (vecchio PIN errato)");
    server.send(403, "application/json", "{\"error\":\"Wrong PIN\"}");
  }
}

void handleApiState() {
  Serial.println("[API] GET /api/state");
  JsonDocument doc;
  doc["slots"] = dispenserState.slots;
  doc["water"] = dispenserState.water;
  String res; serializeJson(doc, res);
  server.send(200, "application/json", res);
}

void handleApiGetMedicines() {
  Serial.println("[API] GET /api/medicines");
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (auto& m : listaMedicine) {
    JsonObject obj = arr.add<JsonObject>();
    obj["id"] = m.id;
    obj["name"] = m.name;
    obj["slot"] = m.slot;
    obj["stock"] = m.stock;
    obj["capacity"] = m.capacity;
    obj["low"] = m.low;
    obj["dose"] = m.dose;
    obj["water"] = m.water;
    
    JsonArray tArr = obj["times"].to<JsonArray>();
    for (auto& t : m.times) tArr.add(t);
    
    JsonArray dArr = obj["days"].to<JsonArray>();
    for (auto& d : m.days) dArr.add(d);
  }
  String res; 
  serializeJson(doc, res);
  server.send(200, "application/json", res);
}

void handleApiAddMedicine() {
  Serial.println("[API] POST /api/medicines (Aggiunta)");
  JsonDocument doc;
  deserializeJson(doc, server.arg("plain"));
  
  Medicina m;
  m.id = millis(); // Genera ID casuale basato su uptime
  m.name = doc["name"].as<String>();
  m.slot = doc["slot"].as<int>();
  m.stock = doc["stock"].as<int>();
  m.capacity = doc["capacity"].as<int>();
  m.low = doc["low"].as<int>();
  m.dose = doc["dose"].as<int>();
  m.water = doc["water"].as<bool>();
  
  JsonArray timesArray = doc["times"].as<JsonArray>();
  for (JsonVariant v : timesArray) m.times.push_back(v.as<String>());
  
  JsonArray daysArray = doc["days"].as<JsonArray>();
  for (JsonVariant v : daysArray) m.days.push_back(v.as<int>());
  
  m.giaSuonataOggi = false;

  listaMedicine.push_back(m);
  
  Serial.printf("  -> Aggiunta medicina: '%s' in slot %d (ID: %lu)\n", m.name.c_str(), m.slot, m.id);
  salvaDati();

  doc["id"] = m.id; 
  String res; 
  serializeJson(doc, res);
  server.send(200, "application/json", res);
}

void handleApiDispense() {
  Serial.println("[API] POST /api/dispense (Test erogazione manuale)");
  server.send(200, "application/json", "{\"status\":\"dispensed\"}");
}

// ==========================================
// DISPATCHER ROTTE DINAMICHE
// ==========================================
void handleNotFound() {
  String uri = server.uri();
  
  // POST /api/slots/<slot>/load
  if (server.method() == HTTP_POST && uri.startsWith("/api/slots/") && uri.endsWith("/load")) {
    Serial.printf("[API] POST %s (Richiesta carico slot)\n", uri.c_str());
    server.send(204, "text/plain", "");
    return;
  }
  
  // PUT o DELETE /api/medicines/<id>
  if (uri.startsWith("/api/medicines/")) {
    unsigned long targetId = uri.substring(15).toInt();

    // Modifica Medicina
    if (server.method() == HTTP_PUT) {
      Serial.printf("[API] PUT %s (Modifica medicina ID: %lu)\n", uri.c_str(), targetId);
      JsonDocument doc;
      deserializeJson(doc, server.arg("plain"));
      for (auto& m : listaMedicine) {
        if (m.id == targetId) {
          m.name = doc["name"].as<String>();
          m.slot = doc["slot"].as<int>();
          m.stock = doc["stock"].as<int>();
          m.capacity = doc["capacity"].as<int>();
          m.low = doc["low"].as<int>();
          m.dose = doc["dose"].as<int>();
          m.water = doc["water"].as<bool>();
          
          m.times.clear();
          JsonArray timesArray = doc["times"].as<JsonArray>();
          for (JsonVariant v : timesArray) m.times.push_back(v.as<String>());
          
          m.days.clear();
          JsonArray daysArray = doc["days"].as<JsonArray>();
          for (JsonVariant v : daysArray) m.days.push_back(v.as<int>());
          
          Serial.printf("  -> Modificata medicina: '%s' (Slot %d)\n", m.name.c_str(), m.slot);
          salvaDati();
          server.send(200, "application/json", server.arg("plain"));
          return;
        }
      }
    }
    
    // Elimina Medicina
    if (server.method() == HTTP_DELETE) {
      Serial.printf("[API] DELETE %s (Eliminazione medicina ID: %lu)\n", uri.c_str(), targetId);
      for (size_t i = 0; i < listaMedicine.size(); i++) {
        if (listaMedicine[i].id == targetId) {
          Serial.printf("  -> Eliminata medicina: '%s'\n", listaMedicine[i].name.c_str());
          listaMedicine.erase(listaMedicine.begin() + i);
          salvaDati();
          server.send(204, "text/plain", "");
          return;
        }
      }
    }
  }

  Serial.printf("[API] ERRORE 404: Route non trovata -> %s\n", uri.c_str());
  server.send(404, "application/json", "{\"error\":\"Not Found\"}");
}

// ==========================================
// SETUP & LOOP
// ==========================================
void setup() {
  setCpuFrequencyMhz(80);
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n\n--- AVVIO WALLIE DISPENSER ---");

  pinMode(BOOT_BUTTON, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BOOT_BUTTON), fermaAllarmeISR, FALLING);
  rgbLedWrite(RGB_BUILTIN, 0, 0, 0);

  if (!LittleFS.begin(true)) {
    Serial.println("[SYSTEM] Errore montaggio LittleFS!");
    return;
  }
  
  caricaDati();

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(true); 
  WiFi.begin(ssid, password);
  Serial.printf("[WIFI] Connessione a %s ", ssid);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.printf("\n[WIFI] IP Dispenser: http://%s\n", WiFi.localIP().toString().c_str());

  sntp_set_sync_interval(INTERVALLO_SYNC_NTP); 
  sntp_set_time_sync_notification_cb(callbackSincronizzazioneNTP); 
  configTzTime(fusoOrarioItalia, ntpServer1, ntpServer2);

  // Mappatura file statici
  server.serveStatic("/", LittleFS, "/index.html");
  server.serveStatic("/index.html", LittleFS, "/index.html");
  server.serveStatic("/register.html", LittleFS, "/register.html");
  server.serveStatic("/dashboard.html", LittleFS, "/dashboard.html");
  server.serveStatic("/favicon.png", LittleFS, "/favicon.png");
  server.serveStatic("/logo.png", LittleFS, "/logo.png");

  // Mappatura API fisse
  server.on("/api/profile", HTTP_GET, handleApiProfileGet);
  server.on("/api/profile", HTTP_PUT, handleApiProfilePut);
  server.on("/api/login", HTTP_POST, handleApiLogin);
  server.on("/api/register", HTTP_POST, handleApiRegister);
  server.on("/api/state", HTTP_GET, handleApiState);
  server.on("/api/medicines", HTTP_GET, handleApiGetMedicines);
  server.on("/api/medicines", HTTP_POST, handleApiAddMedicine);
  server.on("/api/pin", HTTP_PUT, handleApiPinPut);
  server.on("/api/dispense", HTTP_POST, handleApiDispense);
  
  // Mappatura API dinamiche
  server.onNotFound(handleNotFound);

  server.begin();
  Serial.println("[SYSTEM] Web Server avviato e in ascolto.");
}

void loop() {
  server.handleClient();
  unsigned long oraMillis = millis();

  if (fermaAllarmeFlag) {
    fermaAllarmeFlag = false;
    if (allarmeAttivo) {
      Serial.println("[INTERRUPT] Pulsante BOOT premuto. Interruzione manuale!");
      spegniAllarme();
    }
  }

  // Controllo schedulazione sveglie
  if (orologioSincronizzato) {
    time_t now = time(nullptr);
    struct tm* t = localtime(&now);
    
    char bufferOraCorrente[6];
    snprintf(bufferOraCorrente, sizeof(bufferOraCorrente), "%02d:%02d", t->tm_hour, t->tm_min);

    for (size_t i = 0; i < listaMedicine.size(); i++) {
      bool orarioTrovato = false;
      for (const String& oraMed : listaMedicine[i].times) {
        if (oraMed == String(bufferOraCorrente)) {
          orarioTrovato = true;
          break;
        }
      }

      if (orarioTrovato) {
        if (!listaMedicine[i].giaSuonataOggi) {
          int wday = (t->tm_wday == 0) ? 6 : t->tm_wday - 1;
          
          if (listaMedicine[i].days[wday] == 1) {
            Serial.printf("[ALARM] Scaduto orario %s per medicina: %s\n", bufferOraCorrente, listaMedicine[i].name.c_str());
            listaMedicine[i].giaSuonataOggi = true;
            avviaAllarme();
          }
        }
      } else {
        listaMedicine[i].giaSuonataOggi = false;
      }
    }
  }

  // Blink hardware non bloccante
  if (allarmeAttivo) {
    if (oraMillis - inizioAllarmeMillis >= DURATA_ALLARME) {
      spegniAllarme();
    } else if (oraMillis - ultimoBlinkMillis >= INTERVALLO_BLINK) {
      ultimoBlinkMillis = oraMillis;
      statoLed = !statoLed;
      if (statoLed) rgbLedWrite(RGB_BUILTIN, 255, 0, 100); 
      else rgbLedWrite(RGB_BUILTIN, 0, 0, 0);     
    }
  }

  delay(2); 
}
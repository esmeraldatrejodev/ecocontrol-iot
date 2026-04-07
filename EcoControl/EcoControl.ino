#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "DHT.h"
#include <WiFi.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <NTPClient.h>
#include <WiFiUDP.h>

// ==================== CONFIGURACIÓN OLED ====================
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define SCREEN_ADDRESS 0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ==================== CONFIGURACIÓN DHT11 ====================
#define DHTPIN 15
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);

// ==================== CONFIGURACIÓN DE PINES ====================
#define SOIL_PIN    32
#define LDR_PIN     34
#define BUTTON_PIN  22
#define LED_VERDE   18
#define LED_ROJO    19
#define BUZZER      23

// ==================== CALIBRACIÓN SENSOR SUELO ====================
#define HUMEDAD_SECO    3200
#define HUMEDAD_HUMEDO  1400
#define HUMEDAD_MINIMA  45

// ==================== CONFIGURACIÓN MQTT ====================
const char* mqtt_broker   = "38.247.148.240";
const char* mqtt_username = "user_esmeralda";
const char* mqtt_password = "esmeralda123";
const int   mqtt_port     = 1883;
const char* topic         = "esp32/ecocontrol/EsmeTrejo";
const char* topicPromedio = "esp32/ecocontrol/EsmeTrejo/promedio";
const char* deviceID      = "nodo_EsmeTrejo";
const char* tipoPlanta    = "Cactus";

// ==================== CONFIGURACIÓN BOTÓN LARGO ====================
#define TIEMPO_PULSACION_LARGA 3000

// ==================== CONFIGURACIÓN BUZZER ====================
#define TIEMPO_SONIDO     300
#define TIEMPO_SILENCIO   3000
#define FRECUENCIA_BUZZER 4000

// ==================== OBJETOS GLOBALES ====================
WiFiClient   espClient;
PubSubClient mqttClient(espClient);
WiFiUDP      ntpUDP;
NTPClient    timeClient(ntpUDP, "pool.ntp.org", -21600, 60000);

// ==================== VARIABLES GLOBALES ====================
bool          alertaSuelo          = false;
bool          buzzerActivo         = false;
unsigned long tiempoAnteriorBuzzer = 0;
unsigned long tiempoBotonPresionado = 0;
bool          botonSostenido       = false;
unsigned long ultimoEnvioMQTT      = 0;
const unsigned long INTERVALO_MQTT = 5000;

// ==================== ACUMULADORES DE PROMEDIO ====================
float       acumTemp         = 0;
float       acumHumAire      = 0;
long        acumHumSuelo     = 0;
long        acumLuz          = 0;
int         contadorMuestras = 0;
const int   MUESTRAS_HORA    = 720;

// ==================== OLED: MENSAJES DE ESTADO ====================

void mostrarMensaje(String linea1, String linea2 = "", String linea3 = "") {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);

    display.setCursor(0, 10);
    display.println(linea1);

    if (linea2 != "") {
        display.setCursor(0, 25);
        display.println(linea2);
    }

    if (linea3 != "") {
        display.setCursor(0, 40);
        display.println(linea3);
    }

    display.display();
}

// ==================== OLED: DATOS DE SENSORES ====================

void mostrarDatos(float temp, float humAire, int humSuelo, int luz) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);

    display.setCursor(0, 0);
    display.println("ESP32 Monitor");
    display.println("----------------");

    display.setCursor(0, 16);
    display.print("Temp:      ");
    display.print(temp, 1);
    display.println(" C");

    display.setCursor(0, 26);
    display.print("Hum Aire:  ");
    display.print(humAire, 1);
    display.println(" %");

    display.setCursor(0, 36);
    display.print("Hum Suelo: ");
    display.print(humSuelo);
    display.print(" %");

    display.setCursor(110, 36);
    if (alertaSuelo) display.print("!");
    else             display.print(" ");

    display.setCursor(0, 46);
    display.print("Luz:       ");
    display.print(luz);
    display.println(" %");

    display.setCursor(0, 56);
    if (WiFi.status() == WL_CONNECTED) display.print("WiFi OK | MQTT OK");
    else                               display.print("Sin conexion");

    display.display();
}

// ==================== WIFI: CONECTAR CON PORTAL ====================

void conectarWiFi(bool forzarPortal = false) {
    WiFiManager wifiManager;

    wifiManager.setAPCallback([](WiFiManager* wm) {
        mostrarMensaje(
            "Abre tu WiFi y",
            "conectate a:",
            "EcoControl-Portal"
        );
        Serial.println("Portal WiFiManager activo");
    });

    wifiManager.setSaveConfigCallback([]() {
        mostrarMensaje("Conectado!", "Iniciando...");
        Serial.println("Nueva red guardada");
        delay(2000);
    });

    mostrarMensaje("Buscando", "conexion...");
    Serial.println("Buscando conexion WiFi...");

    bool conectado;

    if (forzarPortal) {
        conectado = wifiManager.startConfigPortal("EcoControl-Portal", "12345678");
    } else {
        conectado = wifiManager.autoConnect("EcoControl-Portal", "12345678");
    }

    if (conectado) {
        mostrarMensaje("Conectado!", "Red: " + WiFi.SSID());
        Serial.println("WiFi conectado: " + WiFi.SSID());
        delay(2000);
    } else {
        mostrarMensaje("Sin conexion", "Modo offline");
        Serial.println("Sin conexion WiFi");
        delay(2000);
    }
}

// ==================== MQTT: RECONECTAR ====================

void reconnectMQTT() {
    if (WiFi.status() != WL_CONNECTED) return;

    int intentos = 0;
    while (!mqttClient.connected() && intentos < 3) {
        Serial.print("Conectando MQTT...");
        String clientId = "ESP32-EcoControl-" + String(random(1000));
        if (mqttClient.connect(clientId.c_str(), mqtt_username, mqtt_password)) {
            Serial.println(" Conectado :)");
        } else {
            Serial.print(" Fallo, estado=");
            Serial.println(mqttClient.state());
            delay(2000);
            intentos++;
        }
    }
}

// ==================== ALERTAS ====================

void manejarAlertas(int humSuelo) {
    alertaSuelo = (humSuelo < HUMEDAD_MINIMA);

    if (!alertaSuelo) {
        digitalWrite(LED_VERDE, HIGH);
        digitalWrite(LED_ROJO, LOW);
        noTone(BUZZER);
        buzzerActivo = false;
    } else {
        digitalWrite(LED_VERDE, LOW);
        digitalWrite(LED_ROJO, HIGH);
    }
}

void actualizarBuzzer() {
    if (!alertaSuelo) return;

    unsigned long ahora = millis();

    if (buzzerActivo && (ahora - tiempoAnteriorBuzzer >= TIEMPO_SONIDO)) {
        noTone(BUZZER);
        buzzerActivo = false;
        tiempoAnteriorBuzzer = ahora;
    } else if (!buzzerActivo && (ahora - tiempoAnteriorBuzzer >= TIEMPO_SILENCIO)) {
        tone(BUZZER, FRECUENCIA_BUZZER);
        buzzerActivo = true;
        tiempoAnteriorBuzzer = ahora;
    }
}

// ==================== BOTÓN: PULSACIÓN LARGA ====================

void manejarBoton() {
    bool presionado = digitalRead(BUTTON_PIN) == LOW;

    if (presionado && !botonSostenido) {
        tiempoBotonPresionado = millis();
        botonSostenido = true;
    }

    if (presionado && botonSostenido) {
        unsigned long tiempoSostenido = millis() - tiempoBotonPresionado;

        if (tiempoSostenido < TIEMPO_PULSACION_LARGA) {
            int progreso = map(tiempoSostenido, 0, TIEMPO_PULSACION_LARGA, 0, 128);
            display.clearDisplay();
            display.setTextSize(1);
            display.setTextColor(SSD1306_WHITE);
            display.setCursor(0, 20);
            display.println("Mantener para");
            display.println("cambiar WiFi...");
            display.drawRect(0, 50, 128, 10, SSD1306_WHITE);
            display.fillRect(0, 50, progreso, 10, SSD1306_WHITE);
            display.display();
        }

        if (tiempoSostenido >= TIEMPO_PULSACION_LARGA) {
            botonSostenido = false;
            mostrarMensaje("Reiniciando", "WiFi...");
            delay(1000);

            WiFi.disconnect(true);
            delay(500);
            conectarWiFi(true);

            timeClient.begin();
            reconnectMQTT();
        }
    }

    if (!presionado && botonSostenido) {
        botonSostenido = false;
    }
}

// ==================== SETUP ====================

void setup() {
    Serial.begin(115200);
    Wire.begin(4, 2);

    if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
        Serial.println("Error OLED");
        while (true);
    }

    display.setTextColor(SSD1306_WHITE);
    display.clearDisplay();
    display.display();

    pinMode(BUTTON_PIN, INPUT_PULLUP);
    pinMode(LDR_PIN,    INPUT);
    pinMode(SOIL_PIN,   INPUT);
    pinMode(LED_VERDE,  OUTPUT);
    pinMode(LED_ROJO,   OUTPUT);
    pinMode(BUZZER,     OUTPUT);
    digitalWrite(LED_VERDE, LOW);
    digitalWrite(LED_ROJO,  LOW);

    dht.begin();

    conectarWiFi(false);

    timeClient.begin();
    timeClient.update();

    mqttClient.setServer(mqtt_broker, mqtt_port);
    reconnectMQTT();
}

// ==================== LOOP ====================

void loop() {
    if (!mqttClient.connected()) reconnectMQTT();
    mqttClient.loop();

    timeClient.update();

    manejarBoton();

    // ── Leer sensores ──
    float temperatura = dht.readTemperature();
    float humAire     = dht.readHumidity();

    if (isnan(temperatura)) temperatura = 0;
    if (isnan(humAire))     humAire     = 0;

    int soilRaw  = analogRead(SOIL_PIN);
    int humSuelo = constrain(map(soilRaw, HUMEDAD_SECO, HUMEDAD_HUMEDO, 0, 100), 0, 100);

    int ldrRaw = analogRead(LDR_PIN);
    int luz    = constrain(map(ldrRaw, 0, 4095, 100, 0), 0, 100);

    int rssi = WiFi.RSSI();

    manejarAlertas(humSuelo);
    actualizarBuzzer();

    // ── Publicar MQTT cada 5 segundos ──
    unsigned long ahora = millis();
    if (ahora - ultimoEnvioMQTT >= INTERVALO_MQTT) {
        ultimoEnvioMQTT = ahora;

        // Fecha y hora
        time_t epochTime = timeClient.getEpochTime();
        struct tm* ptm   = gmtime(&epochTime);
        char fechahora[20];
        sprintf(fechahora, "%04d-%02d-%02d %02d:%02d:%02d",
                ptm->tm_year + 1900,
                ptm->tm_mon  + 1,
                ptm->tm_mday,
                ptm->tm_hour,
                ptm->tm_min,
                ptm->tm_sec);

        // JSON dato normal
        StaticJsonDocument<256> doc;
        doc["id"]          = deviceID;
        doc["temp"]        = round(temperatura * 100.0) / 100.0;
        doc["hum_aire"]    = round(humAire     * 100.0) / 100.0;
        doc["hum_suelo"]   = humSuelo;
        doc["luz"]         = luz;
        doc["fechahora"]   = fechahora;
        doc["rssi"]        = rssi;
        doc["tipo_planta"] = tipoPlanta;

        char buffer[256];
        serializeJson(doc, buffer);

        if (mqttClient.publish(topic, buffer)) {
            Serial.print("Publicado: ");
            Serial.println(buffer);
        } else {
            Serial.println("Error al publicar MQTT");
        }

        // ── Acumular valores para promedio ──
        acumTemp     += temperatura;
        acumHumAire  += humAire;
        acumHumSuelo += humSuelo;
        acumLuz      += luz;
        contadorMuestras++;

        Serial.print("Muestras acumuladas: ");
        Serial.print(contadorMuestras);
        Serial.print(" / ");
        Serial.println(MUESTRAS_HORA);

        // ── Cuando se llega a 720 muestras enviar promedio ──
        if (contadorMuestras >= MUESTRAS_HORA) {

            float promedioTemp     = acumTemp     / MUESTRAS_HORA;
            float promedioHumAire  = acumHumAire  / MUESTRAS_HORA;
            int   promedioHumSuelo = acumHumSuelo / MUESTRAS_HORA;
            int   promedioLuz      = acumLuz      / MUESTRAS_HORA;

            // Fecha y hora del momento del promedio
            time_t epochTimeProm = timeClient.getEpochTime();
            struct tm* ptmProm   = gmtime(&epochTimeProm);
            char fechahoraProm[20];
            sprintf(fechahoraProm, "%04d-%02d-%02d %02d:%02d:%02d",
                    ptmProm->tm_year + 1900,
                    ptmProm->tm_mon  + 1,
                    ptmProm->tm_mday,
                    ptmProm->tm_hour,
                    ptmProm->tm_min,
                    ptmProm->tm_sec);

            // JSON promedio horario
            StaticJsonDocument<300> docProm;
            docProm["id"]          = deviceID;
            docProm["tipo"]        = "promedio_horario";
            docProm["temp"]        = round(promedioTemp    * 100.0) / 100.0;
            docProm["hum_aire"]    = round(promedioHumAire * 100.0) / 100.0;
            docProm["hum_suelo"]   = promedioHumSuelo;
            docProm["luz"]         = promedioLuz;
            docProm["muestras"]    = contadorMuestras;
            docProm["fechahora"]   = fechahoraProm;
            docProm["tipo_planta"] = tipoPlanta;

            char bufferProm[300];
            serializeJson(docProm, bufferProm);

            if (mqttClient.publish(topicPromedio, bufferProm)) {
                Serial.println("Promedio horario enviado:");
                Serial.println(bufferProm);
            } else {
                Serial.println("Error al publicar promedio");
            }

            // ── Reiniciar acumuladores ──
            acumTemp         = 0;
            acumHumAire      = 0;
            acumHumSuelo     = 0;
            acumLuz          = 0;
            contadorMuestras = 0;
        }
    }

    // ── Actualizar OLED ──
    mostrarDatos(temperatura, humAire, humSuelo, luz);

    delay(100);
}
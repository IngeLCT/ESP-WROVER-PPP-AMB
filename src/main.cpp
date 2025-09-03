#define ENABLE_USER_AUTH
#define ENABLE_DATABASE
#define ENABLE_ESP_SSLCLIENT

#include <Arduino.h>
#include <PPP.h>
#include <Wire.h>
#include <FirebaseClient.h>
#include "ExampleFunctions.h" // Provides the functions used in the examples.
#include <SensirionCore.h>
#include <SensirionI2CSen5x.h>
#include "SparkFun_SCD4x_Arduino_Library.h"
#include <ArduinoJson.h>
#include "../include/secrets.h"

#define PPP_MODEM_APN "internet.itelcel.com"
#define PPP_MODEM_PIN ""

// LilyGO TTGO T-A7670 development board (ESP32 with SIMCom A7670)
#define PPP_MODEM_RST 5
#define PPP_MODEM_RST_LOW true // active LOW
#define MODEM_RESET_LEVEL HIGH
#define BOARD_POWERON_PIN (12)
#define BOARD_PWRKEY_PIN (4)
#define PPP_MODEM_RST_DELAY 200
#define MODEM_DTR_PIN (25)
#define PPP_MODEM_TX 26
#define PPP_MODEM_RX 27
#define PPP_MODEM_RTS -1
#define PPP_MODEM_CTS -1
#define PPP_MODEM_FC ESP_MODEM_FLOW_CONTROL_NONE
#define PPP_MODEM_MODEL PPP_MODEM_SIM7600

String token = OPENWIDE_TOKEN;

NetworkClient net_client;
ESP_SSLClient ssl_client;

using AsyncClient = AsyncClientClass;
AsyncClient aClient(ssl_client);

UserAuth user_auth(API_KEY, USER_EMAIL, USER_PASSWORD, 3000 /* expire period in seconds (<3600) */);

FirebaseApp app;
RealtimeDatabase Database;
AsyncResult databaseResult;

//-------Sensor Data-----------------
SensirionI2CSen5x sen5x;
SCD4x scd4x;

float pm1p0 = 0, pm2p5 = 0, pm4p0 = 0, pm10p0 = 0;
float humidity = 0, t2 = 0, voc = 0, nox = 0;
uint16_t last_co2 = 0;
float last_temp = 0, last_rh = 0;
String ubicacion = "Desconocido-Desconocido";
String formattedDate = "NoSePudoEstablecer";
String formattedTime = "NoSePudoEstablecer";
String ESPID = "7775034710";

// Variables globales para MCC, MNC, TAC, CellID
int g_mcc = 0;
int g_mnc = 0;
long g_tac = 0;   // Tracking Area Code (decimal)
long g_cellId = 0; // Cell ID

// Timing
unsigned long startMillis = 0;
unsigned long lastTime = 0;
unsigned long timerDelay = 20000;

char elapsedTimeStr[16];

// Variable para controlar la primera ejecución de sendData
bool firstDataSent = false;

bool Clean = false;
// Nuevo flag para evitar repetir la petición de borrado
bool removeRequested = false;

// Nuevos flags para comprobación inicial de historial
bool initialCheckRequested = false;
bool initialCheckDone = false;

void onEvent(arduino_event_id_t event, arduino_event_info_t info)
{
    switch (event)
    {
    case ARDUINO_EVENT_PPP_START:
        Serial.println("PPP Started");
        break;
    case ARDUINO_EVENT_PPP_CONNECTED:
        Serial.println("PPP Connected");
        break;
    case ARDUINO_EVENT_PPP_GOT_IP:
        Serial.println("PPP Got IP");
        break;
    case ARDUINO_EVENT_PPP_LOST_IP:
        Serial.println("PPP Lost IP");
        break;
    case ARDUINO_EVENT_PPP_DISCONNECTED:
        Serial.println("PPP Disconnected");
        break;
    case ARDUINO_EVENT_PPP_STOP:
        Serial.println("PPP Stopped");
        break;
    default:
        break;
    }
}

void parseUeInfo(String ueInfo) {
    // Dividir en tokens por coma
    int index = 0;
    int lastIndex = 0;
    int tokenId = 0;
    while ((index = ueInfo.indexOf(',', lastIndex)) != -1) {
        String token = ueInfo.substring(lastIndex, index);
        token.trim();

        if (tokenId == 2) { // MCC-MNC
            int dash = token.indexOf('-');
            g_mcc = token.substring(0, dash).toInt();
            g_mnc = token.substring(dash + 1).toInt();
        }
        if (tokenId == 3) { // TAC (hex)
            g_tac = strtol(token.c_str(), NULL, 0); // convierte "0x232" → 562
        }
        if (tokenId == 4) { // CellID (decimal)
            g_cellId = token.toInt();
        }

        lastIndex = index + 1;
        tokenId++;
    }
    // Último token también
    String token = ueInfo.substring(lastIndex);
    if (tokenId == 4) {
        g_cellId = token.toInt();
    }

    Serial.printf("Parsed MCC=%d, MNC=%d, TAC=%ld, CellID=%ld\n", g_mcc, g_mnc, g_tac, g_cellId);
}

bool initModemPPP(){
    
    pinMode(BOARD_POWERON_PIN, OUTPUT);
    digitalWrite(BOARD_POWERON_PIN, HIGH);
    pinMode(PPP_MODEM_RST, OUTPUT);
    digitalWrite(PPP_MODEM_RST, !MODEM_RESET_LEVEL);
    delay(100);
    digitalWrite(PPP_MODEM_RST, MODEM_RESET_LEVEL);
    delay(2600);
    digitalWrite(PPP_MODEM_RST, !MODEM_RESET_LEVEL);
    pinMode(MODEM_DTR_PIN, OUTPUT);
    digitalWrite(MODEM_DTR_PIN, LOW);
    pinMode(BOARD_PWRKEY_PIN, OUTPUT);
    digitalWrite(BOARD_PWRKEY_PIN, LOW);
    delay(100);
    digitalWrite(BOARD_PWRKEY_PIN, HIGH);
    delay(100);
    digitalWrite(BOARD_PWRKEY_PIN, LOW);

    // Listen for modem events
    Network.onEvent(onEvent);

    // Configure the modem
    PPP.setApn(PPP_MODEM_APN);
    PPP.setPin(PPP_MODEM_PIN);
    PPP.setResetPin(PPP_MODEM_RST, PPP_MODEM_RST_LOW, PPP_MODEM_RST_DELAY);
    PPP.setPins(PPP_MODEM_TX, PPP_MODEM_RX, PPP_MODEM_RTS, PPP_MODEM_CTS, PPP_MODEM_FC);

    Serial.println("Starting the modem. It might take a while!");
    if(!PPP.begin(PPP_MODEM_MODEL)){
        Serial.println("No se pudo iniciar el modem!");
        return false;
	}

    Serial.print("Manufacturer: ");
    Serial.println(PPP.cmd("AT+CGMI", 10000));

    Serial.print("Model: ");
    Serial.println(PPP.moduleName());
    Serial.print("IMEI: ");
    Serial.println(PPP.IMEI());

    bool attached = PPP.attached();
    if (!attached)
    {
        int i = 0;
        unsigned int s = millis();
        Serial.print("Waiting to connect to network");
        while (!attached && ((++i) < 600))
        {
            Serial.print(".");
            delay(100);
            attached = PPP.attached();
        }
        Serial.print((millis() - s) / 1000.0, 1);
        Serial.println("s");
        attached = PPP.attached();
    }

    Serial.print("Attached: ");
    Serial.println(attached);
    Serial.print("State: ");
    Serial.println(PPP.radioState());

    Serial.println("Enviando AT+CSPI?");
    String cspiResp = PPP.cmd("AT+CPSI?", 5000);
    Serial.println(cspiResp);
	parseUeInfo(cspiResp);  // llena las variables globales

    if (attached)
    {
        Serial.print("Operator: ");
        Serial.println(PPP.operatorName());
        Serial.print("IMSI: ");
        Serial.println(PPP.IMSI());
        Serial.print("RSSI: ");
        Serial.println(PPP.RSSI());
        int ber = PPP.BER();
        if (ber > 0)
        {
            Serial.print("BER: ");
            Serial.println(ber);
            Serial.print("NetMode: ");
            Serial.println(PPP.networkMode());
        }

        Serial.println("Switching to data mode...");
        PPP.mode(ESP_MODEM_MODE_CMUX); // Data and Command mixed mode
        if (!PPP.waitStatusBits(ESP_NETIF_CONNECTED_BIT, 1000))
        {
            Serial.println("Fallo la conexion a internet!");
            return false;
		    
        }
        else
        {
            Serial.println("Conectado a internet!");
        }
    }
    else
    {
        Serial.println("Fallo la conexion Network!");
		return false;
    }
    return true;
}

void inicializarSensores() {
    Wire.begin(18, 19);  // (Verde y Amarillo)
    sen5x.begin(Wire);
    sen5x.deviceReset();
    sen5x.startMeasurement();
    scd4x.begin();
}

void leerSensores() {
    sen5x.readMeasuredValues(pm1p0, pm2p5, pm4p0, pm10p0, humidity, t2, voc, nox);
    if (scd4x.readMeasurement()) {
        last_co2 = scd4x.getCO2();
        last_temp = scd4x.getTemperature();
        last_rh = scd4x.getHumidity();
    }

}

void calcularTiempoTranscurrido() {
    unsigned long totalSeconds = (millis() - startMillis) / 1000;
    sprintf(elapsedTimeStr, "%02lu:%02lu:%02lu", totalSeconds / 3600, (totalSeconds % 3600) / 60, totalSeconds % 60);
}

bool makeHttpRequestOpenCellID() {

    String host = "us1.unwiredlabs.com";
    const int httpsPort = 443;
    String path = "/v2/process.php";

    // Construir JSON
    String payload;
    payload += "{";
    payload += "\"token\":\"" + token + "\",";
    payload += "\"radio\":\"lte\",";
    payload += "\"mcc\":" + String(g_mcc) + ",";
    payload += "\"mnc\":" + String(g_mnc) + ",";
    payload += "\"cells\":[{\"lac\":" + String(g_tac) + ",\"cid\":" + String(g_cellId) + "}],";
    payload += "\"address\":2";
    payload += "}";

    ssl_client.setInsecure();
    ssl_client.setDebugLevel(0);
    ssl_client.setBufferSizes(2048 /* rx */, 1024 /* tx */);
    ssl_client.setClient(&net_client);

    const int maxRetries = 5;
    int retryCount = 0;
    bool connected = false;

    while (retryCount < maxRetries && !connected) {
        if (ssl_client.connect(host.c_str(), httpsPort)) { // 
            Serial.println("Conectado al host: " + String(host));
            connected = true;
        } else {
            retryCount++;
            Serial.printf("❌ Error conectando con Unwiredlabs (intento %d/%d)\n", retryCount, maxRetries);
            if (retryCount < maxRetries) {
                delay(1000); // Esperar 1 segundo antes del próximo intento
            }
        }
    }

    if (!connected) {
        Serial.printf("❌ Fallaron todos los intentos de conexión (%d/%d)\n", maxRetries, maxRetries);
        return false;
    }

    // Solicitud POST
    Serial.println("Enviando POST de Ubicacion");

    ssl_client.print("POST " + path + " HTTP/1.1\r\n");
    ssl_client.print("Host: " + host + "\r\n");
    ssl_client.print("Content-Type: application/json\r\n");
    ssl_client.print("Connection: close\r\n");
    ssl_client.print("Content-Length: " + String(payload.length()) + "\r\n\r\n");
    ssl_client.print(payload);

    unsigned long timeout = millis();
    while (!ssl_client.available() && millis() - timeout < 5000) {
        delay(10);
    }

    String response;
    while (ssl_client.available()) {
        char c = ssl_client.read();
        response += c;
    }

    ssl_client.stop();
    Serial.println("Respuesta: " + response);

    // Buscar el header Date en la respuesta
    int dateIndex = response.indexOf("Date: ");
    if (dateIndex != -1) {
        // Extraer la línea completa del Date (hasta el próximo \r\n)
        int endLine = response.indexOf("\r\n", dateIndex);
        String dateLine = response.substring(dateIndex + 6, endLine);  // +6 para saltar "Date: "

        Serial.println("Header Date extraído: " + dateLine);

        // Parsing manual: formato "Tue, 19 Aug 2025 17:09:41 GMT"
        // Ignorar día de la semana (hasta la coma)
        int commaIndex = dateLine.indexOf(",");
        if (commaIndex != -1) {
            dateLine = dateLine.substring(commaIndex + 2);  // +2 para saltar ", "
        }

        // Separar componentes: DD Mon YYYY HH:MM:SS GMT
        String dayStr = dateLine.substring(0, 2);  // DD
        String monthAbbr = dateLine.substring(3, 6);  // Mon
        String yearStr = dateLine.substring(7, 11);  // YYYY
        String timeStr = dateLine.substring(12, 20);  // HH:MM:SS
        // Ignorar " GMT" al final

        // Convertir mes abreviado a número (1-12)
        int monthNum = 0;
        if (monthAbbr == "Jan") monthNum = 1;
        else if (monthAbbr == "Feb") monthNum = 2;
        else if (monthAbbr == "Mar") monthNum = 3;
        else if (monthAbbr == "Apr") monthNum = 4;
        else if (monthAbbr == "May") monthNum = 5;
        else if (monthAbbr == "Jun") monthNum = 6;
        else if (monthAbbr == "Jul") monthNum = 7;
        else if (monthAbbr == "Aug") monthNum = 8;
        else if (monthAbbr == "Sep") monthNum = 9;
        else if (monthAbbr == "Oct") monthNum = 10;
        else if (monthAbbr == "Nov") monthNum = 11;
        else if (monthAbbr == "Dec") monthNum = 12;

        if (monthNum == 0) {
            Serial.println("❌ Error parseando mes");
        } else {
            // Parsear números
            int day = dayStr.toInt();
            int year = yearStr.toInt();
            int hour = timeStr.substring(0, 2).toInt();
            int minute = timeStr.substring(3, 5).toInt();
            int second = timeStr.substring(6, 8).toInt();

            // Ajustar timezone: restar 6 horas (UTC-6)
            hour -= 6;
            if (hour < 0) {
                hour += 24;  // Ajustar hora negativa
                day -= 1;    // Restar un día
                if (day < 1) {
                    // Manejo simple de fin de mes (sin leap years o meses exactos, ajusta si necesitas precisión)
                    monthNum -= 1;
                    if (monthNum < 1) {
                        monthNum = 12;
                        year -= 1;
                    }
                    // Asumir 30 días por mes para simplicidad (puedes mejorar esto)
                    day = 30 + day;  // Ej: si day=0, day=30 del mes anterior
                }
            }

            // Formatear fecha: dd-mm-yyyy (con ceros)
            formattedDate = (day < 10 ? "0" : "") + String(day) + "-" +
                                (monthNum < 10 ? "0" : "") + String(monthNum) + "-" +
                                String(year);

            // Formatear hora: HH:MM:SS (con ceros)
            formattedTime = (hour < 10 ? "0" : "") + String(hour) + ":" +
                                (minute < 10 ? "0" : "") + String(minute) + ":" +
                                (second < 10 ? "0" : "") + String(second);

        }
    } else {
        Serial.println("❌ No se encontró header Date");
    }

    // Separar encabezados y cuerpo
    int bodyIndex = response.indexOf("\r\n\r\n");
    //Serial.println("Body index: " + bodyIndex);
    if (bodyIndex == -1 || bodyIndex + 4 >= response.length()) {
        Serial.println("❌ No se encontró el cuerpo del mensaje HTTP");
        return false;
    }

    String body = response.substring(bodyIndex + 4);

    // Procesar chunked encoding
    String jsonBody;
    bool inChunk = false;
    int chunkSize = 0;
    int i = 0;
    //Serial.println("Body: " + body);
    while (i < body.length()) {
        if (!inChunk) {
            // Leer tamaño del chunk (en hexadecimal)
            String sizeStr;
            while (i < body.length() && body[i] != '\r') {
                sizeStr += body[i];
                i++;
            }
            i += 2; // Saltar \r\n
            chunkSize = strtol(sizeStr.c_str(), nullptr, 16);
            if (chunkSize == 0) break; // Fin de chunks
            inChunk = true;
        } else {
            // Leer el contenido del chunk
            int bytesToRead = chunkSize;
            if (i + bytesToRead > body.length()) {
                bytesToRead = body.length() - i; // Asegurar no leer más allá del cuerpo
            }
            jsonBody += body.substring(i, i + bytesToRead);
            i += bytesToRead;
            inChunk = false;
            i += 2; // Saltar \r\n después del chunk
        }
    }

    JsonDocument doc;

    // Deserializar el JSON limpio
    //Serial.println("jsonBody: " + jsonBody);
    DeserializationError error = deserializeJson(doc, jsonBody);
    if (error) {
        Serial.println("Error al parsear JSON: " + String(error.c_str()));
        return false;
    }

    if (doc["address_detail"].is<JsonObject>()) {
        // Verificar si los campos city y state existen y son String
        if (doc["address_detail"]["city"].is<String>() && doc["address_detail"]["state"].is<String>()) {
            String city = doc["address_detail"]["city"].as<String>();
            String state = doc["address_detail"]["state"].as<String>();
            ubicacion = city + "-" + state;
            Serial.println("Ubicacion: "+ ubicacion);
        } 
        else {
            Serial.println("❌ Error: Campos 'city' o 'state' no encontrados o tipo incorrecto en address_detail");
        }
    } 
    else {
        Serial.println("❌ Error: Campo 'address_detail' no encontrado o no es un objeto");
    }
    return true;
}

void processData(AsyncResult &aResult) {
    if (!aResult.isResult()) return;
    if (aResult.isEvent())
        Firebase.printf("Event task: %s, msg: %s, code: %d\n", aResult.uid().c_str(), aResult.eventLog().message().c_str(), aResult.eventLog().code());
    if (aResult.isDebug())
        Firebase.printf("Debug task: %s, msg: %s\n", aResult.uid().c_str(), aResult.debug().c_str());
    if (aResult.isError())
        Firebase.printf("Error task: %s, msg: %s, code: %d\n", aResult.uid().c_str(), aResult.error().message().c_str(), aResult.error().code());
    if (aResult.available())
        Firebase.printf("task: %s, payload: %s\n", aResult.uid().c_str(), aResult.c_str());

    // Manejo de respuesta de la comprobación inicial de historial
    if (aResult.uid().length() && strcmp(aResult.uid().c_str(), "getHistTask") == 0) {
        initialCheckRequested = false;
        // payload "null" => no hay historial
        String payload = aResult.c_str();
        if (payload == "null" || payload.length() == 0) {
            Clean = true;
            initialCheckDone = true;
            Serial.println("Historial vacío: listo para push inicial.");
        } else {
            // existe historial -> solicitar borrado
            Database.remove(aClient, "/historial_mediciones", processData, "removeTask");
            removeRequested = true;
            Serial.println("Historial existe: solicitando borrado...");
            // esperamos a que el callback removeTask confirme (se marcará Clean ahí)
        }
        return;
    }

    // Confirmar borrado cuando el uid sea "removeTask"
    if (aResult.uid().length() && strcmp(aResult.uid().c_str(), "removeTask") == 0) {
        removeRequested = false; // siempre limpiar flag de petición
        if (!aResult.isError()) {
            Clean = true;
            initialCheckDone = true; // ya podemos enviar el primer push
            Serial.println("✅ Borrado confirmado (callback).");
        } else {
            Firebase.printf("❌ removeTask error: %s (%d)\n", aResult.error().message().c_str(), aResult.error().code());
        }
        return;
    }

    // resto del manejo existente (setJsonTask, pushJsonTask, etc.)
}

// Función para actualizar solo /ultima_medicion mientras se espera la comprobación inicial
void updateUltimaOnly() {
    object_t json, obj1, obj2, obj3, obj4, obj5, obj6, obj7, obj8, obj9, obj10;
    JsonWriter writer;

    writer.create(obj1, "pm1p0", pm1p0);
    writer.create(obj2, "pm2p5", pm2p5);
    writer.create(obj3, "pm4p0", pm4p0);
    writer.create(obj4, "pm10p0", pm10p0);
    writer.create(obj5, "voc", voc);
    writer.create(obj6, "nox", nox);
    writer.create(obj7, "cTe", round(last_temp * 100) / 100.0);
    writer.create(obj8, "cHu", (int)last_rh);
    writer.create(obj9, "co2", last_co2);
    writer.create(obj10, "tiempo", String(elapsedTimeStr));
    writer.join(json, 10, obj1, obj2, obj3, obj4, obj5, obj6, obj7, obj8, obj9, obj10);

    Database.set<object_t>(aClient, "/ultima_medicion", json, processData, "setOnlyTask");
    Serial.println(" /ultima_medicion actualizada (solo)");
}

void sendData() {

    if (isnan(nox)) nox = 0.0;
    if (isnan(voc)) voc = 0.0;
    if (isnan(last_temp)) last_temp = 0.0;
    if (isnan(last_rh)) last_rh = 0.0;
    if (last_co2 == 0xFFFF) last_co2 = 0;

    if(firstDataSent == false){

        object_t json, obj1, obj2, obj3, obj4, obj5, obj6, obj7, obj8, obj9, obj10, obj11, obj12, obj13, obj14;
        JsonWriter writer;

        writer.create(obj1, "pm1p0", pm1p0);
        writer.create(obj2, "pm2p5", pm2p5);
        writer.create(obj3, "pm4p0", pm4p0);
        writer.create(obj4, "pm10p0", pm10p0);
        writer.create(obj5, "voc", voc);
        writer.create(obj6, "nox", nox);
        writer.create(obj7, "cTe", round(last_temp * 100) / 100.0);
        writer.create(obj8, "cHu", (int)last_rh);
        writer.create(obj9, "co2", last_co2);
        writer.create(obj10, "fecha", formattedDate);
        writer.create(obj11, "inicio", formattedTime);
        writer.create(obj12, "ciudad", ubicacion);
        writer.create(obj13, "tiempo", String(elapsedTimeStr));
        writer.create(obj14, "id", String(ESPID));
        writer.join(json, 14, obj1, obj2, obj3, obj4, obj5, obj6, obj7, obj8, obj9, obj10, obj11, obj12, obj13, obj14);

        // Siempre actualizar ultima_medicion
        Database.set<object_t>(aClient, "/ultima_medicion", json, processData, "setJsonTask");
        Serial.println(" /ultima_medicion actualizada");

        // Push a historial SOLO si ya se confirmó el borrado inicial (Clean == true)
        if (Clean) {
            Database.push<object_t>(aClient, "/historial_mediciones", json, processData, "pushJsonTask");
            Serial.println(" /historial_mediciones push enviado");
        } else {
            Serial.println("Historial diferido: esperando confirmación de borrado (Clean==true)");
        }

        firstDataSent = true;
        Serial.println("Primer envio");
    }
    else { // subsequent sends
        object_t json, obj1, obj2, obj3, obj4, obj5, obj6, obj7, obj8, obj9, obj10;
        JsonWriter writer;

        writer.create(obj1, "pm1p0", pm1p0);
        writer.create(obj2, "pm2p5", pm2p5);
        writer.create(obj3, "pm4p0", pm4p0);
        writer.create(obj4, "pm10p0", pm10p0);
        writer.create(obj5, "voc", voc);
        writer.create(obj6, "nox", nox);
        writer.create(obj7, "cTe", round(last_temp * 100) / 100.0);
        writer.create(obj8, "cHu", (int)last_rh);
        writer.create(obj9, "co2", last_co2);
        writer.create(obj10, "tiempo", String(elapsedTimeStr));
        writer.join(json, 10, obj1, obj2, obj3, obj4, obj5, obj6, obj7, obj8, obj9, obj10);

        // Siempre actualizar ultima_medicion
        Database.set<object_t>(aClient, "/ultima_medicion", json, processData, "setJsonTask");
        Serial.println(" /ultima_medicion actualizada");

        // Push a historial SOLO si ya se confirmó el borrado inicial (Clean == true)
        if (Clean) {
            Database.push<object_t>(aClient, "/historial_mediciones", json, processData, "pushJsonTask");
            Serial.println(" /historial_mediciones push enviado");
        } else {
            Serial.println("Historial diferido: esperando confirmación de borrado (Clean==true)");
        }
    }
}

void setup() {

	Serial.begin(115200);

    if(!initModemPPP()){
        Serial.println("Falloo al iniciar el modem, Reiniciando ESP32");
        delay(5000);
        esp_restart();
    }

	if (makeHttpRequestOpenCellID() == false) {
		Serial.println("❌ Error en la solicitud HTTP,Reiniciando ESP32");
		delay(1000);
		esp_restart();
    }

    Serial.println("Fecha y Tiempo de Inicio establecidos correctamente!");

    //Inicializar Sensores I2C
    inicializarSensores();

    Firebase.printf("Firebase Client v%s\n", FIREBASE_CLIENT_VERSION);

    ssl_client.setInsecure();
    ssl_client.setDebugLevel(0);
    ssl_client.setBufferSizes(2048 /* rx */, 1024 /* tx */);
    ssl_client.setClient(&net_client);

    Serial.println("Initializing app...");

    //initializeApp(aClient, app, getAuth(user_auth), auth_debug_print, "🔐 authTask");

    initializeApp(aClient, app, getAuth(user_auth), 120 * 1000, auth_debug_print);

    app.getApp<RealtimeDatabase>(Database);
    Database.url(DATABASE_URL);

    // Solicitar borrado inicial de /historial_mediciones antes de enviar cualquier dato
    Database.remove(aClient, "/historial_mediciones", processData, "removeTask");
    removeRequested = true;
    Serial.println("Solicitud de borrado inicial enviada (removeTask). Esperando confirmación antes de enviar datos...");

    startMillis = millis();
}

void loop() {

    app.loop();

    unsigned long cumillis = millis();
    if (app.ready() && cumillis - lastTime >= timerDelay) {
        if (!Clean) {
            // Aún no se confirmó el borrado inicial: leer sensores (opcional) pero NO enviar datos
            //leerSensores();
            //calcularTiempoTranscurrido();
            Serial.println("Esperando confirmación de borrado inicial antes de enviar datos...");
        } else {
            // Borrado confirmado: enviar set y push como antes
            leerSensores();
            calcularTiempoTranscurrido();
            sendData();
        }
        lastTime = cumillis;
    }

}

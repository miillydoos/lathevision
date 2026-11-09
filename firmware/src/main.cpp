// ==========================================================
// LatheVision - programa principal do ESP32
// Conecta no Wi-Fi e no broker MQTT, lê os sensores e publica
// um JSON com as leituras a cada segundo.
// ==========================================================
#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include "config.h"
#include "sensores.h"

WiFiClient clienteWifi;
PubSubClient mqtt(clienteWifi);

// O painel assina exatamente estes tópicos (ver docs/js/config.js)
const char *TOPICO_TELEMETRIA = "torno/" TORNO_ID "/telemetria";
const char *TOPICO_STATUS     = "torno/" TORNO_ID "/status";

void conectaWifi() {
  Serial.printf("Conectando ao Wi-Fi %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print('.');
  }
  Serial.printf("\nWi-Fi conectado. IP: %s\n", WiFi.localIP().toString().c_str());
}

void conectaMqtt() {
  while (!mqtt.connected()) {
    // Identificador único por placa, para não derrubar outra conexão com o mesmo nome
    String idCliente = String("lathevision-") + TORNO_ID + "-" +
                       String((uint32_t)ESP.getEfuseMac(), HEX);

    Serial.print("Conectando ao broker MQTT... ");

    // Testamento (LWT): se o ESP cair sem avisar, o próprio broker publica "offline"
    if (mqtt.connect(idCliente.c_str(), TOPICO_STATUS, 1, true, "offline")) {
      Serial.println("OK");
      mqtt.publish(TOPICO_STATUS, "online", true);
    } else {
      Serial.printf("falhou (estado %d). Nova tentativa em 3 s\n", mqtt.state());
      delay(3000);
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(2000);   // dá tempo de o Monitor Serial abrir pela USB antes das primeiras mensagens

  setupSensores();
  conectaWifi();

  mqtt.setServer(MQTT_SERVER, MQTT_PORT);
  mqtt.setBufferSize(512);   // o JSON tem cerca de 200 caracteres
  conectaMqtt();
}

void loop() {
  // Mantém a conexão viva
  if (WiFi.status() != WL_CONNECTED) conectaWifi();
  if (!mqtt.connected()) conectaMqtt();
  mqtt.loop();

  // Publica uma vez por intervalo definido em config.h
  static unsigned long ultimaPublicacao = 0;
  if (millis() - ultimaPublicacao < PUBLICA_INTERVALO_MS) return;
  ultimaPublicacao = millis();

  Leituras l = lerSensores();

  char json[320];
  snprintf(json, sizeof(json),
           "{\"corrente_a\":%.2f,\"vibracao_g\":%.3f,\"rpm\":%.0f,\"sentido\":%d,"
           "\"temperatura_c\":%.2f,\"distancia_cm\":%.1f,\"oleo_pct\":%.1f,"
           "\"rssi\":%d,\"uptime_s\":%lu}",
           l.correnteA, l.vibracaoG, l.rpm, l.sentido,
           l.temperaturaC, l.distanciaCm, l.oleoPct,
           WiFi.RSSI(), millis() / 1000UL);

  if (mqtt.publish(TOPICO_TELEMETRIA, json)) {
    Serial.println(json);
  } else {
    Serial.println("Falha ao publicar a telemetria");
  }
}

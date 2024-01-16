/*
Send Intertechno commands via OOK Tx module like FS1000A
Initiate via mqtt command or web request 
Use builtin led to represent health status
*/

#include <Arduino.h>

#include "app.h"

#define HEALTH_LED_INVERTED false
#define HEALTH_LED_PIN LED_BUILTIN
#define HEALTH_LED_CHANNEL 0

// Web Updater
#include <ESPAsyncWebServer.h>
#include <WiFi.h>
#include <WiFiClient.h>

// Time sync
#include <time.h>

// Reset reason
#include "rom/rtc.h"

// Health LED
#include "Breathing.h"
const uint32_t health_ok_interval = 5000;
const uint32_t health_err_interval = 1000;
Breathing health_led(health_ok_interval, HEALTH_LED_PIN, HEALTH_LED_INVERTED, HEALTH_LED_CHANNEL);
bool enabledBreathing = true;  // global flag to switch breathing animation on or off

// Infrastructure
#include <Syslog.h>
#include <WiFiManager.h>
#include <FileSys.h>

FileSys fileSys;

// Web status page and OTA updater
#define WEBSERVER_PORT 80

AsyncWebServer web_server(WEBSERVER_PORT);
bool shouldReboot = false;  // after updates...

// publish to mqtt broker
#include <PubSubClient.h>

WiFiClient wifiMqtt;
PubSubClient mqtt(wifiMqtt);

// Syslog
WiFiUDP logUDP;
Syslog syslog(logUDP, SYSLOG_PROTO_IETF);
char msg[512];  // one buffer for all syslog messages

char start_time[30];


void slog(const char *message, uint16_t pri = LOG_INFO) {
    static bool log_infos = true;
    
    if (pri < LOG_INFO || log_infos) {
        Serial.println(message);
        syslog.log(pri, message);
    }

    if (log_infos && millis() > 10 * 60 * 1000) {
        log_infos = false;  // log infos only for first 10 minutes
        slog("Switch off info level messages", LOG_NOTICE);
    }
}


void publish( const char *topic, const char *payload ) {
    if (mqtt.connected() && !mqtt.publish(topic, payload)) {
        slog("Mqtt publish failed");
    }
}


// check and report RSSI and BSSID changes
bool handle_wifi() {
    static const uint32_t reconnectInterval = 10000;  // try reconnect every 10s
    static const uint32_t reconnectLimit = 60;        // try restart after 10min
    static uint32_t reconnectPrev = 0;
    static uint32_t reconnectCount = 0;

    bool currConnected = WiFi.isConnected();

    if (currConnected) {
        reconnectCount = 0;
    }
    else {
        uint32_t now = millis();
        if (reconnectCount == 0 || now - reconnectPrev > reconnectInterval) {
            WiFi.reconnect();
            reconnectCount++;
            if (reconnectCount > reconnectLimit) {
                Serial.println("Failed to reconnect WLAN, about to reset");
                for (int i = 0; i < 20; i++) {
                    digitalWrite(HEALTH_LED_PIN, (i & 1) ? LOW : HIGH);
                    delay(100);
                }
                ESP.restart();
                while (true)
                    ;
            }
            reconnectPrev = now;
        }
    }

    return currConnected;
}


char web_msg[80] = "";  // main web page displays and then clears this

// Standard web page
const char *main_page() {
    static const char fmt[] =
        "<!doctype html>\n"
        "<html lang=\"en\">\n"
        " <head>\n"
        "  <meta charset=\"utf-8\">\n"
        "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
        "  <link href=\"data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAABAAAAAQAgMAAABinRfyAAAADFBMVEUqYbutnpTMuq/70SQgIef5AAAAVUlEQVQIHWOAAPkvDAyM3+Y7MLA7NV5g4GVqKGCQYWowYTBhapBhMGB04GE4/0X+M8Pxi+6XGS67XzzO8FH+iz/Dl/q/8gx/2S/UM/y/wP6f4T8QAAB3Bx3jhPJqfQAAAABJRU5ErkJggg==\" rel=\"icon\" type=\"image/x-icon\" />\n"
        "  <link href=\"bootstrap.min.css\" rel=\"stylesheet\">\n"
        "  <title>" PROGNAME " v" VERSION "</title>\n"
        " </head>\n"
        " <body>\n"
        "  <div class=\"container\">\n"
        "   <form action=\"/change\" method=\"post\" enctype=\"multipart/form-data\" id=\"form\">\n"
        "    <div class=\"row\">\n"
        "     <div class=\"col-12\">\n"
        "      <h1>" PROGNAME " v" VERSION "</h1>\n"
        "     </div>\n"
        "    </div>\n"
        "    <div class=\"row my-4\">\n"
        "     <div class=\"col-3\" mr-auto></div>\n"
        "     <div class=\"col-2\" mr-auto>%c 1</div>\n"
        "     <div class=\"col-2\" mr-auto>\n"
        "      <button class=\"btn btn-primary\" button type=\"submit\" name=\"button\" value=\"button-01\">On</button>\n"
        "     </div>\n"
        "     <div class=\"col-2\" mr-auto>\n"
        "      <button class=\"btn btn-primary\" button type=\"submit\" name=\"button\" value=\"button-00\">Off</button>\n"
        "     </div>\n"
        "     <div class=\"col-3\" mr-auto></div>\n"
        "    </div>\n"
        "    <div class=\"row my-4\">\n"
        "     <div class=\"col-3\" mr-auto></div>\n"
        "     <div class=\"col-2\" mr-auto>%c 2</div>\n"
        "     <div class=\"col-2\" mr-auto>\n"
        "      <button class=\"btn btn-primary\" button type=\"submit\" name=\"button\" value=\"button-11\">On</button>\n"
        "     </div>\n"
        "     <div class=\"col-2\" mr-auto>\n"
        "      <button class=\"btn btn-primary\" button type=\"submit\" name=\"button\" value=\"button-10\">Off</button>\n"
        "     </div>\n"
        "     <div class=\"col-3\" mr-auto></div>\n"
        "    </div>\n"
        "    <div class=\"row my-4\">\n"
        "     <div class=\"col-3\" mr-auto></div>\n"
        "     <div class=\"col-2\" mr-auto>%c 3</div>\n"
        "     <div class=\"col-2\" mr-auto>\n"
        "      <button class=\"btn btn-primary\" button type=\"submit\" name=\"button\" value=\"button-21\">On</button>\n"
        "     </div>\n"
        "     <div class=\"col-3\" mr-auto>\n"
        "      <button class=\"btn btn-primary\" button type=\"submit\" name=\"button\" value=\"button-20\">Off</button>\n"
        "     </div>\n"
        "     <div class=\"col-3\" mr-auto></div>\n"
        "    </div>\n"
        "    <div class=\"row my-4\">\n"
        "     <div class=\"col-3\" mr-auto></div>\n"
        "     <div class=\"col-2\" mr-auto>%c All</div>\n"
        "     <div class=\"col-2\" mr-auto>\n"
        "      <button class=\"btn btn-primary\" button type=\"submit\" name=\"button\" value=\"button-x1\">On</button>\n"
        "     </div>\n"
        "     <div class=\"col-3\" mr-auto>\n"
        "      <button class=\"btn btn-primary\" button type=\"submit\" name=\"button\" value=\"button-x0\">Off</button>\n"
        "     </div>\n"
        "     <div class=\"col-3\" mr-auto></div>\n"
        "    </div>\n"
        "   </form>\n"
        "   <div class=\"accordion\" id=\"infos\">\n"
        "    <div class=\"accordion-item\">\n"
        "     <h2 class=\"accordion-header\" id=\"heading1\">\n"
        "      <button class=\"accordion-button\" type=\"button\" data-bs-toggle=\"collapse\" data-bs-target=\"#infos1\" aria-expanded=\"true\" aria-controls=\"infos1\">\n"
        "       Infos\n"
        "      </button>\n"
        "     </h2>\n"
        "     <div id=\"infos1\" class=\"accordion-collapse collapse\" aria-labelledby=\"heading1\" data-bs-parent=\"#infos\">\n"
        "      <div class=\"accordion-body\">\n"
        "       <form action=\"/change\" method=\"post\" enctype=\"multipart/form-data\" id=\"form\">\n"
        "        <div class=\"row\">\n"
        "         <div class=\"col-3\" mr-auto>Family</div>\n"
        "         <div class=\"col-2\" mr-auto>\n"
        "          <button class=\"btn btn-primary\" button type=\"submit\" name=\"button\" value=\"button-a\">A</button>\n"
        "         </div>\n"
        "         <div class=\"col-2\" mr-auto>\n"
        "          <button class=\"btn btn-primary\" button type=\"submit\" name=\"button\" value=\"button-b\">B</button>\n"
        "         </div>\n"
        "         <div class=\"col-2\" mr-auto>\n"
        "          <button class=\"btn btn-primary\" button type=\"submit\" name=\"button\" value=\"button-c\">C</button>\n"
        "         </div>\n"
        "         <div class=\"col-2\" mr-auto>\n"
        "          <button class=\"btn btn-primary\" button type=\"submit\" name=\"button\" value=\"button-d\">D</button>\n"
        "         </div>\n"
        "         <div class=\"col-1\" mr-auto>\n"
        "        </div>\n"
        "       </form>\n"
        "       <div class=\"row\">\n"
        "        <div class=\"col\"><label for=\"update\">Post firmware image to</label></div>\n"
        "        <div class=\"col\" id=\"update\"><a href=\"/update\">/update</a></div>\n"
        "       </div>\n"
        "       <div class=\"row\">\n"
        "        <div class=\"col\"><label for=\"start\">Last start time</label></div>\n"
        "        <div class=\"col\" id=\"start\">%s</div>\n"
        "       </div>\n"
        "       <div class=\"row\">\n"
        "        <div class=\"col\"><label for=\"web\">Last web update</label></div>\n"
        "        <div class=\"col\" id=\"web\">%s</div>\n"
        "       </div>\n"
        "       <div class=\"row mt-4\">\n"
        "        <div class=\"col\">\n"
        "         <form action=\"breathe\" method=\"post\">\n"
        "          <button class=\"btn btn-primary\" button type=\"submit\" name=\"button\" value=\"breathe\">Toggle Breath</button>\n"
        "         </form>\n"
        "        </div>\n"
        "        <div class=\"col\">\n"
        "         <form action=\"wipe\" method=\"post\">\n"
        "          <button class=\"btn btn-primary\" button type=\"submit\" name=\"button\" value=\"wipe\">Wipe WLAN</button>\n"
        "         </form>\n"
        "        </div>\n"
        "        <div class=\"col\">\n"
        "         <form action=\"reset\" method=\"post\">\n"
        "          <button class=\"btn btn-primary\" button type=\"submit\" name=\"button\" value=\"reset\">Reset ESP</button>\n"
        "         </form>\n"
        "        </div>\n"
        "       </div>\n"
        "      </div>\n"
        "     </div>\n"
        "    </div>\n"
        "   </div>\n"
        "   <div class=\"alert alert-primary alert-dismissible fade show\" role=\"alert\">\n"
        "    <strong>Status</strong> %s\n"
        "    <button type=\"button\" class=\"btn-close\" data-bs-dismiss=\"alert\" aria-label=\"Close\">\n"
        "     <span aria-hidden=\"true\"></span>\n"
        "    </button>\n"
        "   </div>\n"
        "   <div class=\"row\"><small>... by <a href=\"https://github.com/joba-1\">Joachim Banzhaf</a>, " __DATE__ " " __TIME__ "</small></div>\n"
        "  </div>\n"
        "  <script src=\"jquery.min.js\"></script>\n"
        "  <script src=\"bootstrap.bundle.min.js\"></script>\n"
        " </body>\n"
        "</html>\n";
    static char page[sizeof(fmt) + 500] = "";
    static char curr_time[30];
    char family = 'A' + (app_get_addr() >> 4);
    time_t now;
    time(&now);
    strftime(curr_time, sizeof(curr_time), "%FT%T", localtime(&now));
    snprintf(page, sizeof(page), fmt, family, family, family, family, start_time, curr_time, web_msg);
    *web_msg = '\0';
    return page;
}

// Define web pages for update, reset or for event infos
void setup_webserver() {
    // css and js files
    web_server.serveStatic("/bootstrap.min.css", fileSys, "/bootstrap.min.css").setCacheControl("max-age=600");
    web_server.serveStatic("/bootstrap.bundle.min.js", fileSys, "/bootstrap.bundle.min.js").setCacheControl("max-age=600");
    web_server.serveStatic("/jquery.min.js", fileSys, "/jquery.min.js").setCacheControl("max-age=600");

    // change switch
    web_server.on("/change", HTTP_POST, [](AsyncWebServerRequest *request) {
        uint16_t prio = LOG_INFO;

        String arg = request->arg("button");
        if (!arg.isEmpty()) {
            bool button = true;
            const char *payload;
            uint8_t addr = app_get_addr() & 0xf0;
            if (arg.equals("button-00")) {
                payload = app_send_to(false, addr);
                mqtt.publish(MQTT_TOPIC "/change", payload);
            }
            else if (arg.equals("button-01")) {
                payload = app_send_to(true, addr); 
                mqtt.publish(MQTT_TOPIC "/change", payload);
            }
            else if (arg.equals("button-10")) {
                payload = app_send_to(false, addr | 1); 
                mqtt.publish(MQTT_TOPIC "/change", payload);
            }
            else if (arg.equals("button-11")) {
                payload = app_send_to(true, addr | 1); 
                mqtt.publish(MQTT_TOPIC "/change", payload);
            }
            else if (arg.equals("button-20")) {
                payload = app_send_to(false, addr | 2); 
                mqtt.publish(MQTT_TOPIC "/change", payload);
            }
            else if (arg.equals("button-21")) {
                payload = app_send_to(true, addr | 2); 
                mqtt.publish(MQTT_TOPIC "/change", payload);
            }
            else if (arg.equals("button-x0")) {
                payload = app_send_to(false, addr); 
                mqtt.publish(MQTT_TOPIC "/change", payload);
                payload = app_send_to(false, addr | 1); 
                mqtt.publish(MQTT_TOPIC "/change", payload);
                payload = app_send_to(false, addr | 2); 
                mqtt.publish(MQTT_TOPIC "/change", payload);
            }
            else if (arg.equals("button-x1")) {
                payload = app_send_to(true, addr); 
                mqtt.publish(MQTT_TOPIC "/change", payload);
                payload = app_send_to(true, addr | 1); 
                mqtt.publish(MQTT_TOPIC "/change", payload);
                payload = app_send_to(true, addr | 2); 
                mqtt.publish(MQTT_TOPIC "/change", payload);
            }
            else if (arg.equals("button-a")) {
                app_addr(0x00); 
            }
            else if (arg.equals("button-b")) {
                app_addr(0x10); 
            }
            else if (arg.equals("button-c")) {
                app_addr(0x20); 
            }
            else if (arg.equals("button-d")) {
                app_addr(0x30); 
            }
            else {
                button = false;
            }
            if( button ) { 
                snprintf(web_msg, sizeof(web_msg), "Button '%s' pressed", arg.c_str());
                slog(web_msg, prio);
            }
            request->redirect("/");  
        }
    });

    // Call this page to reset the ESP
    web_server.on("/reset", HTTP_POST, [](AsyncWebServerRequest *request) {
        slog("RESET ESP32", LOG_NOTICE);
        request->send(200, "text/html",
                        "<html>\n"
                        " <head>\n"
                        "  <title>" PROGNAME " v" VERSION "</title>\n"
                        "  <meta http-equiv=\"refresh\" content=\"7; url=/\"> \n"
                        " </head>\n"
                        " <body>Resetting...</body>\n"
                        "</html>\n");
        delay(200);
        ESP.restart();
    });

    // Index page
    web_server.on("/", [](AsyncWebServerRequest *request) { 
        request->send(200, "text/html", main_page());
    });

    // Toggle breathing status led if you dont like it or ota does not work
    web_server.on("/breathe", HTTP_ANY, [](AsyncWebServerRequest *request) {
        enabledBreathing = !enabledBreathing; 
        app_breathe(enabledBreathing);
        snprintf(web_msg, sizeof(web_msg), "%s", enabledBreathing ? "breathing enabled" : "breathing disabled");
        request->send(204, "text/html", "");
    });

    web_server.on("/wipe", HTTP_POST, [](AsyncWebServerRequest *request) {
        WiFiManager wm;
        wm.resetSettings();
        slog("Wipe WLAN config and reset ESP32", LOG_NOTICE);
        request->send(200, "text/html",
                        "<html>\n"
                        " <head>\n"
                        "  <title>" PROGNAME " v" VERSION "</title>\n"
                        "  <meta http-equiv=\"refresh\" content=\"7; url=/\"> \n"
                        " </head>\n"
                        " <body>Wipe WLAN config. Connect to AP '" HOSTNAME "' and connect to http://192.168.4.1</body>\n"
                        "</html>\n");
        delay(200);
        ESP.restart();
    });

    // Firmware Update Form
    web_server.on("/update", HTTP_GET, [](AsyncWebServerRequest *request){
        static const char page[] =
            "<!doctype html>\n"
            "<html lang=\"en\">\n"
            " <head>\n"
            "  <meta charset=\"utf-8\">\n"
            "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
            "  <link href=\"data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAABAAAAAQAgMAAABinRfyAAAADFBMVEUqYbutnpTMuq/70SQgIef5AAAAVUlEQVQIHWOAAPkvDAyM3+Y7MLA7NV5g4GVqKGCQYWowYTBhapBhMGB04GE4/0X+M8Pxi+6XGS67XzzO8FH+iz/Dl/q/8gx/2S/UM/y/wP6f4T8QAAB3Bx3jhPJqfQAAAABJRU5ErkJggg==\" rel=\"icon\" type=\"image/x-icon\" />\n"
            "  <link href=\"bootstrap.min.css\" rel=\"stylesheet\">\n"
            "  <title>" PROGNAME " v" VERSION "</title>\n"
            " </head>\n"
            " <body>\n"
            "  <div class=\"container\">\n"
            "    <div class=\"row\">\n"
            "     <div class=\"col-12\">\n"
            "      <h1>" PROGNAME " v" VERSION "</h1>\n"
            "     </div>\n"
            "    </div>\n"
            "    <form method=\"POST\" action=\"/update\" enctype=\"multipart/form-data\">\n"
            "     <div class=\"row\">\n"
            "      <div class=\"col\">\n"
            "       <input type=\"file\" name=\"update\">\n"
            "      </div>\n"
            "      <div class=\"col\">\n"
            "       <input type=\"submit\" value=\"Update\">\n"
            "      </div>\n"
            "     </div>\n"
            "    </form>\n"
            "  </div>\n"
            "  <script src=\"bootstrap.bundle.min.js\"></script>\n"
            " </body>\n"
            "</html>\n";
 
        request->send(200, "text/html", page);
    });

    web_server.on("/update", HTTP_POST, [](AsyncWebServerRequest *request){
        shouldReboot = !Update.hasError();
        AsyncWebServerResponse *response = request->beginResponse(200, "text/plain", shouldReboot ? "OK" : "FAIL");
        response->addHeader("Connection", "close");
        request->send(response);
    },[](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final){
        if(!index){
            Serial.printf("Update Start: %s\n", filename.c_str());
            if(!Update.begin((ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000)){
                Update.printError(Serial);
                snprintf(web_msg, sizeof(web_msg), "%s", Update.errorString());
                request->redirect("/");  
            }
        }
        if(!Update.hasError()){
            if(Update.write(data, len) != len){
                Update.printError(Serial);
                snprintf(web_msg, sizeof(web_msg), "%s", Update.errorString());
                request->redirect("/");  
            }
        }
        if(final){
            if(Update.end(true)){
                Serial.printf("Update Success: %uB\n", index+len);
                snprintf(web_msg, sizeof(web_msg), "Update Success: %u Bytes", index+len);
            } else {
                Update.printError(Serial);
                snprintf(web_msg, sizeof(web_msg), "%s", Update.errorString());
            }
            request->redirect("/"); 
        }
    });

    // Catch all page
    web_server.onNotFound( [](AsyncWebServerRequest *request) { 
        snprintf(web_msg, sizeof(web_msg), "%s", "<h2>page not found</h2>\n");
        request->send(404, "text/html", main_page()); 
    });

    web_server.begin();

    snprintf(msg, sizeof(msg), "Serving HTTP on port %d", WEBSERVER_PORT);
    slog(msg, LOG_NOTICE);
}


// check ntp status
// return true if time is valid
bool check_ntptime() {
    static bool have_time = false;

    bool valid_time = time(0) > 1582230020;

    if (!have_time && valid_time) {
        have_time = true;
        time_t now = time(NULL);
        strftime(start_time, sizeof(start_time), "%FT%T", localtime(&now));
        snprintf(msg, sizeof(msg), "Got valid time at %s", start_time);
        slog(msg, LOG_NOTICE);
        if (mqtt.connected()) {
            publish(MQTT_TOPIC "/StartTime", start_time);
        }
    }

    return have_time;
}


// Reset reason can be quite useful...
// Messages from arduino core example
void print_reset_reason(int core) {
  switch (rtc_get_reset_reason(core)) {
    case 1  : slog("Vbat power on reset");break;
    case 3  : slog("Software reset digital core");break;
    case 4  : slog("Legacy watch dog reset digital core");break;
    case 5  : slog("Deep Sleep reset digital core");break;
    case 6  : slog("Reset by SLC module, reset digital core");break;
    case 7  : slog("Timer Group0 Watch dog reset digital core");break;
    case 8  : slog("Timer Group1 Watch dog reset digital core");break;
    case 9  : slog("RTC Watch dog Reset digital core");break;
    case 10 : slog("Instrusion tested to reset CPU");break;
    case 11 : slog("Time Group reset CPU");break;
    case 12 : slog("Software reset CPU");break;
    case 13 : slog("RTC Watch dog Reset CPU");break;
    case 14 : slog("for APP CPU, reseted by PRO CPU");break;
    case 15 : slog("Reset when the vdd voltage is not stable");break;
    case 16 : slog("RTC Watch dog reset digital core and rtc module");break;
    default : slog("Reset reason unknown");
  }
}


// Called on incoming mqtt messages
void mqtt_callback(char* topic, byte* payload, unsigned int length) {

    typedef struct cmd { const char *name; void (*action)(void); } cmd_t;
    
    static cmd_t cmds[] = { 
        { "000", [](){ app_send_to(false, 0x00); } },
        { "001", [](){ app_send_to(true,  0x00); } },
        { "010", [](){ app_send_to(false, 0x01); } },
        { "011", [](){ app_send_to(true,  0x01); } },
        { "020", [](){ app_send_to(false, 0x02); } },
        { "021", [](){ app_send_to(true,  0x02); } },

        { "100", [](){ app_send_to(false, 0x10); } },
        { "101", [](){ app_send_to(true,  0x10); } },
        { "110", [](){ app_send_to(false, 0x11); } },
        { "111", [](){ app_send_to(true,  0x11); } },
        { "120", [](){ app_send_to(false, 0x12); } },
        { "121", [](){ app_send_to(true,  0x12); } },

        { "200", [](){ app_send_to(false, 0x20); } },
        { "201", [](){ app_send_to(true,  0x20); } },
        { "210", [](){ app_send_to(false, 0x21); } },
        { "211", [](){ app_send_to(true,  0x21); } },
        { "220", [](){ app_send_to(false, 0x22); } },
        { "221", [](){ app_send_to(true,  0x22); } },

        { "300", [](){ app_send_to(false, 0x30); } },
        { "301", [](){ app_send_to(true,  0x30); } },
        { "310", [](){ app_send_to(false, 0x31); } },
        { "311", [](){ app_send_to(true,  0x31); } },
        { "320", [](){ app_send_to(false, 0x32); } },
        { "321", [](){ app_send_to(true,  0x32); } }
    };

    if (strcasecmp(MQTT_TOPIC "/cmd", topic) == 0) {
        for (auto &cmd: cmds) {
            if (strncasecmp(cmd.name, (char *)payload, length) == 0) {
                snprintf(msg, sizeof(msg), "Execute mqtt command '%s'", cmd.name);
                slog(msg, LOG_INFO);
                (*cmd.action)();
                return;
            }
        }
    }

    snprintf(msg, sizeof(msg), "Ignore mqtt %s: '%.*s'", topic, length, (char *)payload);
    slog(msg, LOG_WARNING);
}


bool handle_mqtt( bool time_valid ) {
    static const int32_t interval = 5000;  // if disconnected try reconnect this often in ms
    static uint32_t prev = -interval;      // first connect attempt without delay

    if (mqtt.connected()) {
        mqtt.loop();
        return true;
    }

    uint32_t now = millis();
    if (now - prev > interval) {
        prev = now;

        if (mqtt.connect(HOSTNAME, MQTT_TOPIC "/LWT", 0, true, "Offline")
            && mqtt.publish(MQTT_TOPIC "/LWT", "Online", true)
            && mqtt.publish(MQTT_TOPIC "/Hostname", HOSTNAME)
            && mqtt.publish(MQTT_TOPIC "/Version", VERSION)
            && (!time_valid || mqtt.publish(MQTT_TOPIC "/StartTime", start_time))
            && mqtt.subscribe(MQTT_TOPIC "/cmd")) {
            snprintf(msg, sizeof(msg), "Connected to MQTT broker %s:%d using topic %s", MQTT_SERVER, MQTT_PORT, MQTT_TOPIC);
            slog(msg, LOG_NOTICE);
            return true;
        }

        int error = mqtt.state();
        mqtt.disconnect();
        snprintf(msg, sizeof(msg), "Connect to MQTT broker %s:%d failed with code %d", MQTT_SERVER, MQTT_PORT, error);
        slog(msg, LOG_ERR);
    }

    return false;
}


void handle_reboot() {
    static const int32_t reboot_delay = 1000;  // if should_reboot wait this long in ms
    static uint32_t start = 0;                 // first detected should_reboot

    if (shouldReboot) {
        uint32_t now = millis();
        if (!start) {
            start = now;
        }
        else {
            if (now - start > reboot_delay) {
                ESP.restart();
            }
        }
    }
}


// Startup
void setup() {
    pinMode(HEALTH_LED_PIN, OUTPUT);
    digitalWrite(HEALTH_LED_PIN, HEALTH_LED_INVERTED ? LOW : HIGH);

    Serial.begin(BAUDRATE);
    Serial.println("\nStarting " PROGNAME " v" VERSION " " __DATE__ " " __TIME__);

    String host(HOSTNAME);
    host.toLowerCase();
    WiFi.hostname(host.c_str());
    WiFi.mode(WIFI_STA);

    // Syslog setup
    syslog.server(SYSLOG_SERVER, SYSLOG_PORT);
    syslog.deviceHostname(WiFi.getHostname());
    syslog.appName("Joba1");
    syslog.defaultPriority(LOG_KERN);

    digitalWrite(HEALTH_LED_PIN, HEALTH_LED_INVERTED ? HIGH : LOW);

    WiFiManager wm;
    wm.setConfigPortalTimeout(180);
    if (!wm.autoConnect(WiFi.getHostname(), WiFi.getHostname())) {
        Serial.println("Failed to connect WLAN, about to reset");
        for (int i = 0; i < 20; i++) {
            digitalWrite(HEALTH_LED_PIN, (i & 1) ? HIGH : LOW);
            delay(100);
        }
        ESP.restart();
        while (true)
            ;
    }

    digitalWrite(HEALTH_LED_PIN, HEALTH_LED_INVERTED ? LOW : HIGH);

    char msg[80];
    snprintf(msg, sizeof(msg), "%s Version %s, WLAN IP is %s", PROGNAME, VERSION,
        WiFi.localIP().toString().c_str());
    slog(msg, LOG_NOTICE);

    configTime(3600, 3600, NTP_SERVER);  // MEZ/MESZ

    fileSys.begin();

    app_setup();

    setup_webserver();

    mqtt.setServer(MQTT_SERVER, MQTT_PORT);
    mqtt.setCallback(mqtt_callback);

    print_reset_reason(0);
    print_reset_reason(1);

    enabledBreathing = app_get_breathe();
    health_led.limits(1, health_led.range() / 2);  // only barely off to 50% brightness
    health_led.begin();

    slog("Setup done", LOG_NOTICE);
}


// Main loop
void loop() {
    bool health = true;

    bool have_time = check_ntptime();

    health &= handle_mqtt(have_time);
    health &= handle_wifi();

    if (have_time && enabledBreathing) {
        health_led.interval(health ? health_ok_interval : health_err_interval);
        health_led.handle();
    }

    handle_reboot();
}
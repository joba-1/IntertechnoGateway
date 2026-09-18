/*
Send Intertechno commands via OOK Tx module like FS1000A
Initiate via mqtt command or web request 
Use builtin led to represent health status
*/

#include <Arduino.h>

#include "app.h"
#include "web_icons.h"

#define HEALTH_LED_INVERTED false
#define HEALTH_LED_PIN LED_BUILTIN
#define HEALTH_LED_CHANNEL 0

// WiFiManager must be included before ESPAsyncWebServer to avoid
// HTTP_DELETE/GET/... enum conflicts between WebServer.h (pulls in
// ESP-IDF http_parser.h) and ESPAsyncWebServer's WebRequestMethod enum.
#include <WiFiManager.h>

// Web Updater
#include <ESPAsyncWebServer.h>
#ifdef ESP32
#include <WiFi.h>
#endif
#include <WiFiClient.h>

// Time sync
#include <time.h>

// Reset reason
#ifdef ESP32
#include "rom/rtc.h"
#endif

// Health LED
#include "Breathing.h"
const uint32_t health_ok_interval = 5000;
const uint32_t health_err_interval = 1000;
Breathing health_led(health_ok_interval, HEALTH_LED_PIN, HEALTH_LED_INVERTED, HEALTH_LED_CHANNEL);
bool enabledBreathing = true;  // global flag to switch breathing animation on or off

// Infrastructure
#include <Syslog.h>
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


volatile bool discovery_due = true;  // (re)announce the switches to Home Assistant from loop()

// Home Assistant MQTT discovery: one retained config per switch, with the unique_ids of the former YAML entries.
// No "device" block on purpose: with one, HA prefixes every friendly name with the device name.
void publish_discovery() {
    char topic[48];
    char name[128];
    char json[640];

    for (uint8_t family = 0; family <= 3; family++) {
        for (uint8_t device = 0; device <= 2; device++) {
            size_t n = 0;
            for (const char *p = app_get_name(family << 4 | device); *p && n < sizeof(name) - 2; p++) {
                if ((unsigned char)*p < 0x20) {
                    continue;
                }
                if (*p == '"' || *p == '\\') {
                    name[n++] = '\\';
                }
                name[n++] = *p;
            }
            name[n] = '\0';

            char id = 'a' + family;
            int code_f = family, code_d = device;
            snprintf(topic, sizeof(topic), "homeassistant/switch/itgw_%c%d/config", id, device + 1);
            snprintf(json, sizeof(json),
                "{\"name\":\"%s\",\"unique_id\":\"itgw_%c%d\","
                "\"command_topic\":\"" MQTT_TOPIC "/cmd\",\"payload_on\":\"%d%d1\",\"payload_off\":\"%d%d0\","
                "\"state_topic\":\"" MQTT_TOPIC "/change\",\"state_on\":\"%d%d1\",\"state_off\":\"%d%d0\","
                "\"availability_topic\":\"" MQTT_TOPIC "/LWT\",\"payload_available\":\"Online\",\"payload_not_available\":\"Offline\"}",
                name, id, device + 1, code_f, code_d, code_f, code_d, code_f, code_d, code_f, code_d);
            if (!mqtt.publish(topic, json, true)) {
                slog("Home Assistant discovery publish failed", LOG_ERR);
                return;
            }
        }
    }

    discovery_due = false;
    slog("Published Home Assistant discovery", LOG_INFO);
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
    static const char option_fmt[] = "           <option value=\"%s\">%s</option>\n";
    static const char label_fmt[] = 
        "       <form action=\"/set\" method=\"post\" enctype=\"multipart/form-data\">\n"
        "        <div class=\"input-group\">\n"
        "         <select id=\"selectLabel\" name=\"label\" class=\"form-select\" style=\"max-width: 6rem\" aria-label=\"Switch\">\n"
        "%s"
        "         </select>\n"
        "         <input type=\"text\" id=\"inputName\" name=\"name\" class=\"form-control\" placeholder=\"Name shown on this page\" aria-label=\"Name\">\n"
        "         <button class=\"btn btn-primary\" type=\"submit\" name=\"button\" value=\"name\">Set</button>\n"
        "        </div>\n"
        "       </form>\n";
    static const char page_fmt[] =
        "<!doctype html>\n"
        "<html lang=\"en\">\n"
        " <head>\n"
        "  <meta charset=\"utf-8\">\n"
        "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
        "  "
        ICON_LINKS_HTML
        "  <link href=\"bootstrap.min.css\" rel=\"stylesheet\">\n"
        "  <title>" PROGNAME " v" VERSION "</title>\n"
        "  <script>\n"
        "   // dark from civil dusk to civil dawn (sun 6 deg below the horizon) near Stuttgart, +-20 min, browser clock\n"
        "   function isNight(d) {\n"
        "    var lat = 48.8 * Math.PI / 180, lon = 9.1, rad = Math.PI / 180;\n"
        "    var day = (Date.UTC(d.getFullYear(), d.getMonth(), d.getDate()) - Date.UTC(d.getFullYear(), 0, 0)) / 864e5;\n"
        "    var decl = -23.44 * Math.cos(2 * Math.PI * (day + 10) / 365) * rad;\n"
        "    var cosH = (Math.sin(-6 * rad) - Math.sin(lat) * Math.sin(decl)) / (Math.cos(lat) * Math.cos(decl));\n"
        "    var half = Math.acos(Math.max(-1, Math.min(1, cosH))) * 12 / Math.PI;\n"
        "    var noon = 12 - lon / 15, utc = d.getUTCHours() + d.getUTCMinutes() / 60;\n"
        "    return utc < noon - half || utc > noon + half;\n"
        "   }\n"
        "   function theme() { document.documentElement.setAttribute('data-bs-theme', isNight(new Date()) ? 'dark' : 'light'); }\n"
        "   theme();\n"
        "   setInterval(theme, 60000);\n"
        "  </script>\n"
        " </head>\n"
        " <body>\n"
        "  <div class=\"container\">\n"
        "   <form id=\"switches\" action=\"/change\" method=\"post\" enctype=\"multipart/form-data\">\n"
        "    <div class=\"row\">\n"
        "     <div class=\"col-12\">\n"
        "      <h1>" PROGNAME " v" VERSION "</h1>\n"
        "     </div>\n"
        "    </div>\n"
        "    <div class=\"row my-3 align-items-center\">\n"
        "     <div class=\"col-2 text-end\"><span id=\"state-1\" class=\"badge border bg-body-secondary text-body-secondary\">?</span></div>\n"
        "     <div class=\"col-4\">%s</div>\n"
        "     <div class=\"col-3\"><button class=\"btn btn-outline-success w-100\" type=\"submit\" name=\"button\" value=\"button-1-on\" data-dev=\"1\" data-on=\"1\"><span class=\"spinner-border spinner-border-sm d-none\"></span> On</button></div>\n"
        "     <div class=\"col-3\"><button class=\"btn btn-outline-secondary w-100\" type=\"submit\" name=\"button\" value=\"button-1-off\" data-dev=\"1\" data-on=\"0\"><span class=\"spinner-border spinner-border-sm d-none\"></span> Off</button></div>\n"
        "    </div>\n"
        "    <div class=\"row my-3 align-items-center\">\n"
        "     <div class=\"col-2 text-end\"><span id=\"state-2\" class=\"badge border bg-body-secondary text-body-secondary\">?</span></div>\n"
        "     <div class=\"col-4\">%s</div>\n"
        "     <div class=\"col-3\"><button class=\"btn btn-outline-success w-100\" type=\"submit\" name=\"button\" value=\"button-2-on\" data-dev=\"2\" data-on=\"1\"><span class=\"spinner-border spinner-border-sm d-none\"></span> On</button></div>\n"
        "     <div class=\"col-3\"><button class=\"btn btn-outline-secondary w-100\" type=\"submit\" name=\"button\" value=\"button-2-off\" data-dev=\"2\" data-on=\"0\"><span class=\"spinner-border spinner-border-sm d-none\"></span> Off</button></div>\n"
        "    </div>\n"
        "    <div class=\"row my-3 align-items-center\">\n"
        "     <div class=\"col-2 text-end\"><span id=\"state-3\" class=\"badge border bg-body-secondary text-body-secondary\">?</span></div>\n"
        "     <div class=\"col-4\">%s</div>\n"
        "     <div class=\"col-3\"><button class=\"btn btn-outline-success w-100\" type=\"submit\" name=\"button\" value=\"button-3-on\" data-dev=\"3\" data-on=\"1\"><span class=\"spinner-border spinner-border-sm d-none\"></span> On</button></div>\n"
        "     <div class=\"col-3\"><button class=\"btn btn-outline-secondary w-100\" type=\"submit\" name=\"button\" value=\"button-3-off\" data-dev=\"3\" data-on=\"0\"><span class=\"spinner-border spinner-border-sm d-none\"></span> Off</button></div>\n"
        "    </div>\n"
        "    <div class=\"row my-3 align-items-center\">\n"
        "     <div class=\"col-2 text-end\"></div>\n"
        "     <div class=\"col-4\">All %c</div>\n"
        "     <div class=\"col-3\"><button class=\"btn btn-outline-success w-100\" type=\"submit\" name=\"button\" value=\"button-x-on\" data-dev=\"x\" data-on=\"1\"><span class=\"spinner-border spinner-border-sm d-none\"></span> On</button></div>\n"
        "     <div class=\"col-3\"><button class=\"btn btn-outline-secondary w-100\" type=\"submit\" name=\"button\" value=\"button-x-off\" data-dev=\"x\" data-on=\"0\"><span class=\"spinner-border spinner-border-sm d-none\"></span> Off</button></div>\n"
        "    </div>\n"
        "   </form>\n"
        "   <div class=\"accordion my-4\" id=\"infos\">\n"
        "    <div class=\"accordion-item\">\n"
        "     <h2 class=\"accordion-header\" id=\"heading1\">\n"
        "      <button class=\"accordion-button collapsed\" type=\"button\" data-bs-toggle=\"collapse\" data-bs-target=\"#infos1\" aria-expanded=\"false\" aria-controls=\"infos1\">\n"
        "       Configuration\n"
        "      </button>\n"
        "     </h2>\n"
        "     <div id=\"infos1\" class=\"accordion-collapse collapse\" aria-labelledby=\"heading1\" data-bs-parent=\"#infos\">\n"
        "      <div class=\"accordion-body\">\n"
        "       <h6 class=\"text-body-secondary\">Family</h6>\n"
        "       <form action=\"/change\" method=\"post\" enctype=\"multipart/form-data\" class=\"mb-4\">\n"
        "        <div class=\"btn-group w-100\" role=\"group\" aria-label=\"Family\" id=\"families\">\n"
        "         <button class=\"btn btn-outline-primary\" type=\"submit\" name=\"button\" value=\"button-a\">A</button>\n"
        "         <button class=\"btn btn-outline-primary\" type=\"submit\" name=\"button\" value=\"button-b\">B</button>\n"
        "         <button class=\"btn btn-outline-primary\" type=\"submit\" name=\"button\" value=\"button-c\">C</button>\n"
        "         <button class=\"btn btn-outline-primary\" type=\"submit\" name=\"button\" value=\"button-d\">D</button>\n"
        "        </div>\n"
        "       </form>\n"
        "       <h6 class=\"text-body-secondary\">Switch names</h6>\n"
        "       <div class=\"mb-4\">\n"
        "%s"
        "       </div>\n"
        "       <h6 class=\"text-body-secondary\">Device</h6>\n"
        "       <table class=\"table table-sm mb-4\">\n"
        "        <tbody>\n"
        "         <tr><td class=\"text-body-secondary\">Firmware or filesystem update</td><td><a href=\"/update\">/update</a></td></tr>\n"
        "         <tr><td class=\"text-body-secondary\">Last start</td><td>%s</td></tr>\n"
        "         <tr><td class=\"text-body-secondary\">Page loaded</td><td>%s</td></tr>\n"
        "        </tbody>\n"
        "       </table>\n"
        "       <div class=\"d-flex flex-wrap gap-2\">\n"
        "        <form action=\"breathe\" method=\"post\"><button class=\"btn btn-outline-secondary\" type=\"submit\" name=\"button\" value=\"breathe\">Toggle LED breathing</button></form>\n"
        "        <form action=\"wipe\" method=\"post\" onsubmit=\"return confirm('Forget the WLAN credentials? The gateway then opens its own access point for setup.')\"><button class=\"btn btn-outline-warning\" type=\"submit\" name=\"button\" value=\"wipe\">Wipe WLAN</button></form>\n"
        "        <form action=\"reset\" method=\"post\" onsubmit=\"return confirm('Restart the gateway?')\"><button class=\"btn btn-outline-danger\" type=\"submit\" name=\"button\" value=\"reset\">Reset ESP</button></form>\n"
        "       </div>\n"
        "      </div>\n"
        "     </div>\n"
        "    </div>\n"
        "   </div>\n"
        "%s"
        "   <div class=\"row\"><small>... by <a href=\"https://github.com/joba-1\">Joachim Banzhaf</a>, " __DATE__ " " __TIME__ "</small></div>\n"
        "  </div>\n"
        "  <script src=\"jquery.min.js\"></script>\n"
        "  <script src=\"bootstrap.bundle.min.js\"></script>\n"
        "  <script>\n"
        "   var busy = false, timer;\n"
        "   function paint(s) {\n"
        "    busy = false;\n"
        "    s.devices.forEach(function(d) {\n"
        "     var on = $('#switches [data-dev=' + d.n + '][data-on=1]'), off = $('#switches [data-dev=' + d.n + '][data-on=0]');\n"
        "     on.toggleClass('btn-success', d.state == 1).toggleClass('btn-outline-success', d.state != 1);\n"
        "     off.toggleClass('btn-secondary', d.state == 0).toggleClass('btn-outline-secondary', d.state != 0);\n"
        "     on.find('.spinner-border').toggleClass('d-none', d.wanted != 1);\n"
        "     off.find('.spinner-border').toggleClass('d-none', d.wanted != 0);\n"
        "     $('#state-' + d.n).text(d.state == 1 ? 'on' : d.state == 0 ? 'off' : '?')\n"
        "      .attr('class', 'badge ' + (d.state == 1 ? 'text-bg-success' : d.state == 0 ? 'text-bg-secondary' : 'border bg-body-secondary text-body-secondary'));\n"
        "     if (d.wanted >= 0) busy = true;\n"
        "    });\n"
        "    $('#families button').removeClass('active').filter('[value=button-' + s.family.toLowerCase() + ']').addClass('active');\n"
        "    [0, 1].forEach(function(v) {\n"
        "     var any = s.devices.some(function(d) { return d.wanted == v; });\n"
        "     $('#switches [data-dev=x][data-on=' + v + '] .spinner-border').toggleClass('d-none', !any);\n"
        "    });\n"
        "   }\n"
        "   function poll() {\n"
        "    clearTimeout(timer);\n"
        "    $.getJSON('state', paint).always(function() { timer = setTimeout(poll, busy ? 400 : 3000); });\n"
        "   }\n"
        "   $('#switches button').on('click', function(e) {\n"
        "    e.preventDefault();\n"
        "    var b = $(this), on = b.data('on');\n"
        "    var targets = b.data('dev') == 'x' ? $('#switches [data-on=' + on + ']') : b;\n"
        "    targets.find('.spinner-border').removeClass('d-none');\n"
        "    busy = true;\n"
        "    $.post('change', { button: b.val(), ajax: 1 }).always(poll);\n"
        "   });\n"
        "   poll();\n"
        "   $.get('get?label=' + $('#selectLabel').val(), function(txt){$('#inputName').val(txt)});\n"
        "   $('#selectLabel').change(function() {\n"
        "    $.get('get?label=' + $(this).val(), function(txt){$('#inputName').val(txt)});\n"
        "   });\n"
        "  </script>\n"
        " </body>\n"
        "</html>\n";

    static const char alert_fmt[] =
        "   <div class=\"alert alert-primary alert-dismissible fade show\" role=\"alert\">\n"
        "    <strong>Status</strong> %s\n"
        "    <button type=\"button\" class=\"btn-close\" data-bs-dismiss=\"alert\" aria-label=\"Close\">\n"
        "     <span aria-hidden=\"true\"></span>\n"
        "    </button>\n"
        "   </div>\n";

    static char option[sizeof(option_fmt) + 2];
    static char label[sizeof(label_fmt) + sizeof(option) * 4 * 3];
    static char alert[sizeof(alert_fmt) + sizeof(web_msg)];
    static char page[sizeof(page_fmt) + sizeof(label) + sizeof(alert) + 100];
    static char curr_time[30];

    String options;
    for (uint8_t family=0; family<=3; ++family) {
        for (uint8_t device=0; device<=2; ++device) {
            char label[3] = { (char)('A' + family), (char)('1' + device), '\0' };
            snprintf(option, sizeof(option), option_fmt, label, label);
            options += option;
        }
    }
    snprintf(label, sizeof(label), label_fmt, options.c_str());
     
    time_t now;
    time(&now);
    strftime(curr_time, sizeof(curr_time), "%FT%T", localtime(&now));

    uint8_t family = app_get_addr() >> 4;

    // the status alert only appears when there is something to report (update result, 404, ...)
    if (*web_msg) {
        snprintf(alert, sizeof(alert), alert_fmt, web_msg);
    }
    else {
        *alert = '\0';
    }

    snprintf(page, sizeof(page), page_fmt, app_get_name(family<<4), app_get_name((family<<4) + 1),
        app_get_name((family<<4) + 2), 'A' + family, label, start_time, curr_time, alert);

    *web_msg = '\0';

    return page;
}

// Define web pages for update, reset or for event infos
void setup_webserver() {
    // css and js files
    web_server.serveStatic("/bootstrap.min.css", fileSys, "/bootstrap.min.css").setCacheControl("max-age=600");
    web_server.serveStatic("/bootstrap.bundle.min.js", fileSys, "/bootstrap.bundle.min.js").setCacheControl("max-age=600");
    web_server.serveStatic("/jquery.min.js", fileSys, "/jquery.min.js").setCacheControl("max-age=600");

    // get switch name
    web_server.on("/get", HTTP_GET, [](AsyncWebServerRequest *request) {
        const char *name = "";
        String arg = request->arg("label");
        // snprintf(web_msg, sizeof(web_msg), "/get?label='%s'", arg.c_str());
        // slog(web_msg, LOG_INFO);
        if (arg.length() == 2) {
            uint8_t family = arg[0] - 'A';
            uint8_t device = arg[1] - '1';
            if( family <= 3 && device <= 2 ) {
                name = app_get_name(family<<4 | device);
            }
        }
        request->send(200, "text/plain", name);
       });

    // set switch name
    web_server.on("/set", HTTP_POST, [](AsyncWebServerRequest *request) {
        const char *label = NULL;
        const char *name = NULL;
        for( size_t i=0; i < request->params(); ++i) {
            // snprintf(web_msg, sizeof(web_msg), "p[%i:%s]=%s", i, request->getParam(i)->name().c_str(), request->getParam(i)->value().c_str());
            // slog(web_msg, LOG_INFO);
            // web_msg[0] = '\0';
            if (request->getParam(i)->name().equals("label")) {
                label = request->getParam(i)->value().c_str();
            } else if (request->getParam(i)->name().equals("name")) {
                name = request->getParam(i)->value().c_str();
            }
        }
        if (label && name && strlen(label) == 2) {
            uint8_t family = label[0] - 'A';
            uint8_t device = label[1] - '1';
            if( family <= 3 && device <= 2 ) {
                if( !name[0] ) {
                    app_name(family<<4 | device, label);
                }
                else {
                    app_name(family<<4 | device, name);
                }
                discovery_due = true;
            }
        }
        request->redirect("/");
       });

    // change switch
    web_server.on("/change", HTTP_POST, [](AsyncWebServerRequest *request) {
        uint16_t prio = LOG_INFO;

        String arg = request->arg("button");
        bool button = false;
        if (!arg.isEmpty()) {
            button = true;
            uint8_t addr = app_get_addr() & 0xf0;
            if (arg.equals("button-1-off")) {
                app_request(false, addr);
            }
            else if (arg.equals("button-1-on")) {
                app_request(true, addr);
            }
            else if (arg.equals("button-2-off")) {
                app_request(false, addr | 1);
            }
            else if (arg.equals("button-2-on")) {
                app_request(true, addr | 1);
            }
            else if (arg.equals("button-3-off")) {
                app_request(false, addr | 2);
            }
            else if (arg.equals("button-3-on")) {
                app_request(true, addr | 2);
            }
            else if (arg.equals("button-x-off")) {
                app_request(false, addr);
                app_request(false, addr | 1);
                app_request(false, addr | 2);
            }
            else if (arg.equals("button-x-on")) {
                app_request(true, addr);
                app_request(true, addr | 1);
                app_request(true, addr | 2);
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
                char line[80];  // not the global msg: this runs in the AsyncTCP task, concurrent to loop()
                snprintf(line, sizeof(line), "Button '%s' pressed", arg.c_str());
                slog(line, prio);
            }
        }
        if (request->hasArg("ajax")) {
            request->send(button ? 204 : 400);
        }
        else {
            request->redirect("/");
        }
    });

    // state of the switches of the selected family for the page script
    web_server.on("/state", HTTP_GET, [](AsyncWebServerRequest *request) {
        uint8_t family = app_get_addr() & 0xf0;
        char json[160];
        int len = snprintf(json, sizeof(json), "{\"family\":\"%c\",\"devices\":[", 'A' + (family >> 4));
        for (uint8_t device = 0; device < 3; device++) {
            int8_t wanted, state;
            app_get_state(family | device, &wanted, &state);
            len += snprintf(json + len, sizeof(json) - len, "%s{\"n\":%u,\"wanted\":%d,\"state\":%d}",
                device ? "," : "", device + 1, wanted, state);
        }
        snprintf(json + len, sizeof(json) - len, "]}");
        request->send(200, "application/json", json);
    });

    // Call this page to reset the ESP
    web_server.on("/reset", HTTP_POST, [](AsyncWebServerRequest *request) {
        slog("RESET ESP32", LOG_NOTICE);
        request->send(200, "text/html",
                        "<html>\n"
                        " <head>\n"
                        ICON_LINKS_HTML
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
                        ICON_LINKS_HTML
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
            "  "
            ICON_LINKS_HTML
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
            // pio uploadfs sends spiffs.bin/littlefs.bin; ?fs=1 forces a filesystem update for any file name
            bool is_fs = filename.indexOf("spiffs") >= 0 || filename.indexOf("littlefs") >= 0 || request->hasParam("fs");
            Serial.printf("Update Start: %s (%s)\n", filename.c_str(), is_fs ? "filesystem" : "firmware");
            bool started;
            if (is_fs) {
#ifdef ESP32
                fileSys.end();
                started = Update.begin(UPDATE_SIZE_UNKNOWN, U_SPIFFS);
#else
                started = false;
                snprintf(web_msg, sizeof(web_msg), "Filesystem update not supported on this board");
#endif
            }
            else {
                started = Update.begin((ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000);
            }
            if(!started){
                Update.printError(Serial);
#ifdef ESP32
                const char *msg = Update.errorString();
#else
                const char *msg = Update.getErrorString().c_str();
#endif
                snprintf(web_msg, sizeof(web_msg), "%s", msg);
                request->redirect("/");  
            }
        }
        if(!Update.hasError()){
            if(Update.write(data, len) != len){
                Update.printError(Serial);
#ifdef ESP32
                const char *msg = Update.errorString();
#else
                const char *msg = Update.getErrorString().c_str();
#endif
                snprintf(web_msg, sizeof(web_msg), "%s", msg);
                request->redirect("/");  
            }
        }
        if(final){
            if(Update.end(true)){
                Serial.printf("Update Success: %uB\n", index+len);
                snprintf(web_msg, sizeof(web_msg), "Update Success: %u Bytes", index+len);
            } else {
                Update.printError(Serial);
#ifdef ESP32
                const char *msg = Update.errorString();
#else
                const char *msg = Update.getErrorString().c_str();
#endif
                snprintf(web_msg, sizeof(web_msg), "%s", msg);
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


#ifdef ESP32
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
#endif


// Called on incoming mqtt messages
void mqtt_callback(char* topic, byte* payload, unsigned int length) {

    // payload is exactly 3 digits: family 0-3, device 0-2, off/on 0-1 (e.g. "201" = C1 on)
    if (strcasecmp(MQTT_TOPIC "/cmd", topic) == 0 && length == 3
        && payload[0] >= '0' && payload[0] <= '3'
        && payload[1] >= '0' && payload[1] <= '2'
        && (payload[2] == '0' || payload[2] == '1')) {
        uint8_t addr = ((payload[0] - '0') << 4) | (payload[1] - '0');
        app_request(payload[2] == '1', addr);
        snprintf(msg, sizeof(msg), "Queue mqtt command '%.3s'", (char *)payload);
        slog(msg, LOG_INFO);
        return;
    }

    snprintf(msg, sizeof(msg), "Ignore mqtt %s: '%.*s'", topic, length, (char *)payload);
    slog(msg, LOG_WARNING);
}


bool handle_mqtt( bool time_valid ) {
    static const int32_t interval = 5000;  // if disconnected try reconnect this often in ms
    static uint32_t prev = -interval;      // first connect attempt without delay

    if (mqtt.connected()) {
        // mqtt.loop() reads one message per call: drain them all, so a burst of commands
        // collapses to the latest one per switch before loop() sends anything
        for (uint8_t i = 0; i < 20 && mqtt.loop() && wifiMqtt.available(); i++) {
        }
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
            discovery_due = true;
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
    mqtt.setBufferSize(768);  // discovery configs are ~550 bytes, PubSubClient defaults to 256

#ifdef ESP32
    print_reset_reason(0);
    print_reset_reason(1);
#endif

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

    if (discovery_due && mqtt.connected()) {
        publish_discovery();
    }

    const char *sent = app_handle();
    if (sent) {
        publish(MQTT_TOPIC "/change", sent);
        snprintf(msg, sizeof(msg), "Sent command '%s'", sent);
        slog(msg, LOG_INFO);
    }

    if (have_time && enabledBreathing) {
        health_led.interval(health ? health_ok_interval : health_err_interval);
        health_led.handle();
    }

    handle_reboot();
}
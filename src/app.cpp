#include <Arduino.h>

#ifndef PIN_DATA
#define PIN_DATA 16
#endif

#include <app.h>


#include <Preferences.h>
Preferences prefs;

// family_code   (high 4bit)  [ 0x0 - 0xF ] defined as [ A - P ] by Intertechno
// device_number (low 4bit)   [ 0x0 - 0xF ] defined as [ 1 - 16 ] or 
//                                          device_group [ 1 - 4 ] and device [ 1 - 4 ]
//                                          with device_number=(device_group-1)*4 + device-1
uint8_t addr = 0;
bool breathe = true;
char change[4] = "000";  // family, device, on/off like "000" for A1,off and "111" for B2,on
String names[4][3][2];   // [family][device][key, value] like A1, ...


#include <intertechno.h>

struct it its{ 
    .pin = PIN_DATA,
#if defined(F_CPU) && F_CPU == 8000000
    .rf_cal_on = -7,
    .rf_cal_off = -7,
#elif defined(F_CPU) && F_CPU == 16000000
    .rf_cal_on = -1,
    .rf_cal_off = -1,
#else 
    // you're on your own. 
    // just make sure one full sequence takes 54.6ms, 
    // and the same sequence without the trailing 31 LO signals 41.6ms
    .rf_cal_on = -1,
    .rf_cal_off = -1,
#endif
};


void app_setup() {
    prefs.begin("IntertechnoGW", false);
    addr = prefs.getUChar("address", 0);
    breathe = prefs.getBool("breathe", true);
    for (uint8_t family=0; family<=3; ++family) {
        for (uint8_t device=0; device<=2; ++device) {
            char label[3] = { (char)('A' + family), (char)('1' + device), '\0' };
            names[family][device][0] = label;
            names[family][device][1] = prefs.getString(label, label);
        }
    }

    pinMode(its.pin, OUTPUT);
    digitalWrite(its.pin, LOW);
}


void app_addr( uint8_t value ) {
    prefs.putUChar("address", value);
    addr = value;
}


uint8_t app_get_addr() {
    return addr;
}


void app_breathe( bool value ) {
    prefs.putBool("breathe", value);
    breathe = value;
}


bool app_get_breathe() {
    return breathe;
}


const char *app_send( bool on )
{
    return app_send_to(on, addr);
}


const char *app_send_to( bool on, uint8_t addr )
{
    uint8_t family = addr >> 4;
    uint8_t device = addr & 0x0f;

    rf_tx_cmd(its, addr, on ? INTERTECHNO_CMD_ON : INTERTECHNO_CMD_OFF);

    change[0] = '0' + family;
    change[1] = '0' + device;
    change[2] = on ? '1' : '0';

    return change;
}


void app_name( uint8_t addr, const String &name ) {
    uint8_t family = addr >> 4;
    uint8_t device = addr & 0x0f;

    if( family > 3 || device > 2 ) {
        return;
    }

    char label[3] = { (char)('A' + family), (char)('1' + device), '\0' };
    prefs.putString(label, name);
    names[family][device][1] = name;
}


const char *app_get_name( uint8_t addr ) {
    uint8_t family = addr >> 4;
    uint8_t device = addr & 0x0f;

    if( family > 3 || device > 2 ) {
        return "";
    }
    
    return names[family][device][1].c_str();
}

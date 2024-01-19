#include <FileSys.h>

#ifdef USE_SPIFFS
    #include <SPIFFS.h>
#else
    #include <LittleFS.h>
#endif

FileSys::FileSys() :
#ifdef USE_SPIFFS
    _fs(SPIFFS)
#else
    _fs(LittleFS)
#endif
{}

FileSys::operator fs::FS &() { 
    return _fs; 
}

bool FileSys::begin( bool formatOnFail ) {
    bool rc = false;

#ifdef USE_SPIFFS
    if (!SPIFFS.begin())
#else
    if (!LittleFS.begin())
#endif
    {
        Serial.println("Mount failed");
    }
    else {
        File file = _fs.open("/boot.msg", "r");
        if (!file) {
            Serial.println("Failed to open /boot.msg for reading");
        }
        else {
            if (file.isDirectory()) {
                Serial.println("Cannot open directory");
            }
            else {
                rc = true;
                while (file.available()) {
                    Serial.write(file.read());
                }
            }
            file.close();
        }
    }
    Serial.println("Setup FS done");

    return rc;
}

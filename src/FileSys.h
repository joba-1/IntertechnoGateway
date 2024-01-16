#ifndef FileSys_h
#define FileSys_h

#include "FS.h"

// define USE_SPIFFS for SPIFFS, else LittleFS
class FileSys {
    public:
        FileSys();

        // Mount filesystem and read /boot.msg
        bool begin( bool formatOnFail = false );

        // Use this object anywhere a fs::FS object can be used
        operator fs::FS&();

    private:
        fs::FS &_fs;
};

#endif
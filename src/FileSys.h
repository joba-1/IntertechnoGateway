#ifndef FileSys_h
#define FileSys_h

#include "FS.h"

// define USE_SPIFFS for SPIFFS, else LittleFS
class FileSys {
    public:
        FileSys();

        // Mount filesystem and read /boot.msg
        bool begin( bool formatOnFail = false );

        // Unmount, e.g. before the partition is overwritten by an update
        void end();

        // Use this object anywhere a fs::FS object can be used
        operator fs::FS&();

    private:
        fs::FS &_fs;
};

#endif
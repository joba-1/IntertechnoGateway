#ifndef Breathing_h
#define Breathing_h

#include <Arduino.h>

/*
Handle a breathing led (or whatever is connected to the pwm pin)
*/
class Breathing {
    public:
        // Define the controlled hardware
        Breathing(uint32_t interval_ms, uint8_t pwm_pin, bool inverted = false, uint8_t pwm_channel = 0);

        void begin();   // init the hardware and start the interval
        void handle();  // adjust the duty cycle if needed

        void interval( uint32_t interval_ms );  // set the duration of a breathing cycle
        void limits( uint32_t min_duty, uint32_t max_duty );  // set duty limits to control minimum and maximum brightness of a cycle
        uint32_t range();  // get the available duty range, might be needed to calculate limits

    private:
        uint32_t _interval_ms;
        uint8_t _pwm_pin;
        bool _inverted;
        uint8_t _pwm_channel;
        uint32_t _start;
        uint32_t _prev_duty;
        uint32_t _min_duty;
        uint32_t _max_duty;
};

#endif

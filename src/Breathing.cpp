#include <Breathing.h>

#define PWM_FREQ 25000

#ifndef PWMRANGE
#define PWMRANGE 1023
#endif

#ifndef PWMBITS
#define PWMBITS 10
#endif

Breathing::Breathing(uint32_t interval_ms, uint8_t pwm_pin, bool inverted, uint8_t pwm_channel) :
    _interval_ms(interval_ms), _pwm_pin(pwm_pin), _inverted(inverted), _pwm_channel(pwm_channel), _min_duty(0), _max_duty(PWMRANGE) {
}

void Breathing::begin() {
    #if defined(ESP32)
        ledcAttachPin(_pwm_pin, _pwm_channel);
        ledcSetup(_pwm_channel, PWM_FREQ, PWMBITS);
    #else
        analogWriteRange(PWMRANGE);
        pinMode(_pwm_pin, OUTPUT);
    #endif
    _start = millis();
    _prev_duty = _inverted ? PWMRANGE : 0;
    ledcWrite(_pwm_channel, _prev_duty);
}

void Breathing::handle() {
    // map elapsed in breathing intervals
    uint32_t now = millis();
    uint32_t elapsed = now - _start;
    while (elapsed > _interval_ms) {
        _start += _interval_ms;
        elapsed -= _interval_ms;
    }

    // map min brightness to max brightness twice in one breathe interval
    uint32_t duty = (_max_duty - _min_duty) * elapsed * 2 / _interval_ms + _min_duty;
    if (duty > _max_duty) {
        // second range: breathe out aka get darker
        duty = 2 * _max_duty - duty;
    }

    duty = duty * duty / _max_duty;  // generally reduce lower brightness levels

    if (duty != _prev_duty) {
        // adjust pwm duty cycle
        _prev_duty = duty;
        if (_inverted) {
            duty = PWMRANGE - duty;
        }
        #if defined(ESP32)
            ledcWrite(_pwm_channel, duty);
        #else
            analogWrite(_pwm_pin, duty);
        #endif
    }
}

void Breathing::interval( uint32_t interval_ms ) {
    _interval_ms = interval_ms;
}

void Breathing::limits( uint32_t min_duty, uint32_t max_duty ) {
    _min_duty = min_duty;
    _max_duty = max_duty;
}

uint32_t Breathing::range() {
    return PWMRANGE;
}

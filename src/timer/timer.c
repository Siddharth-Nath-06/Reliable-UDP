#include "timer.h"
#include <windows.h>


//this is a windows api that ive used to check how much time has passed 
//gives us a reliable way to check how much time has passed 
static uint64_t get_time_ms(void)
{
    static LARGE_INTEGER frequency;
    static bool initialized = false;

    if (!initialized) {
        QueryPerformanceFrequency(&frequency);
        initialized = true;
    }

    LARGE_INTEGER current_time;
    QueryPerformanceCounter(&current_time);

    // this means numver of ticks divided by the frequency of ticks which
    //basically tells u how many seconds have elapsed and muliply it by
    //1000 to convert into milliseconds 1000

    return (uint64_t)(
        (current_time.QuadPart * 1000) / frequency.QuadPart
    );
}

//creatd a structure for our timer
typedef struct {
    bool active;
    bool running;
    uint64_t duration_ms;
    uint64_t start_time_ms;
} Timer;

//this means that there can be at max 100 timers at a time
#define MAX_TIMERS 100


static Timer timers[MAX_TIMERS];
static TimerId next_timer_id = 1;

//this function will be used to create a timer for a specific packet
TimerId timer_create(void)
{
    if (next_timer_id > MAX_TIMERS) {
        return 0;
    }

    //this means I assign an id and then increment its value
    TimerId id = next_timer_id++;

    timers[id - 1].active = true;
    timers[id - 1].running = false;
    timers[id - 1].duration_ms = 0;
    timers[id - 1].start_time_ms = 0;

    return id;
}

//now this is used to start a specific timer
bool timer_start(TimerId id, uint64_t duration_ms)
{
    if (id == 0 || id > MAX_TIMERS) {
        return false;
    }

    //this I have done so that I dont have to write timers[id-1] everywhgere, the timer that i am referring to is stored in 'timer' 
    Timer *timer = &timers[id - 1];

    //if its not active (which means not created) or if it is currently running, dont start it 
    if (!timer->active || timer->running) {
        return false;
    }


    timer->duration_ms = duration_ms;
    timer->start_time_ms = get_time_ms();
    timer->running = true;

    return true;
}


bool timer_restart(TimerId id, uint64_t duration_ms)
{
    if (id == 0 || id > MAX_TIMERS) {
        return false;
    }

    Timer *timer = &timers[id - 1];

    if (!timer->active) {
        return false;
    }

    timer->duration_ms = duration_ms;
    timer->start_time_ms = get_time_ms();
    timer->running = true;

    return true;
}

bool timer_cancel(TimerId id)
{
    if (id == 0 || id > MAX_TIMERS) {
        return false;
    }

    Timer *timer = &timers[id - 1];

    if (!timer->active || !timer->running) {
        return false;
    }

    timer->running = false;

    return true;
}

bool timer_expired(TimerId id)
{
    if (id == 0 || id > MAX_TIMERS) {
        return false;
    }

    Timer *timer = &timers[id - 1];

    if (!timer->active || !timer->running) {
        return false;
    }

    uint64_t current_time_ms = get_time_ms();

    if (current_time_ms - timer->start_time_ms >= timer->duration_ms) {
        return true;
    }

    return false;
}
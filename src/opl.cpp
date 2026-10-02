// opl.cpp - the AdLib: a YM3812 (OPL2), emulated by Aaron Giles' ymfm
// (BSD-3-Clause, third_party/ymfm). A C face over it for audio.c.
//
// The only part of the chip that has to run in step with the guest rather
// than the audio stream is its timers: SD_Startup detects the AdLib by
// starting timer 1 and expecting the status register to show it expired
// ~80 us later. Any armed timer is simply reported expired at the next status
// read, which is all the detection loop can observe.
#include "ymfm_opl.h"
#include <stdint.h>

namespace {
struct Iface : ymfm::ymfm_interface {
    bool armed[2] = {false, false};
    void ymfm_set_timer(uint32_t tnum, int32_t duration) override { if (tnum < 2) armed[tnum] = duration >= 0; }
    void expire() {
        for (uint32_t t = 0; t < 2; t++)
            if (armed[t]) { armed[t] = false; m_engine->engine_timer_expired(t); }
    }
};
Iface g_iface;
ymfm::ym3812 *g_chip;
}

extern "C" void opl_init(void) { g_chip = new ymfm::ym3812(g_iface); g_chip->reset(); }
extern "C" void opl_write(int data_port, uint8_t v) { g_chip->write(data_port ? 1 : 0, v); }
extern "C" uint8_t opl_status(void) { g_iface.expire(); return g_chip->read_status(); }

// One sample at the chip's native rate (clock / 72).
extern "C" int opl_sample(void)
{
    ymfm::ym3812::output_data o;
    g_chip->generate(&o);
    return o.data[0];
}

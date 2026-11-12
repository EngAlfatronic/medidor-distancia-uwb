// Small filters shared by both boards.
#pragma once

#include <Arduino.h>

// Trimmed mean over the last AVG_SAMPLES readings (the highest and lowest fifth are
// discarded). If JUMP_COUNT readings in a row are more than JUMP_M away from the
// average, all on the same side, the board really moved: restart the window so the
// value follows.
#define AVG_SAMPLES 80 // 2 s at 40 Hz
#define JUMP_M 0.25f
#define JUMP_COUNT 8

struct RangeFilter
{
    float samples[AVG_SAMPLES];
    uint8_t count = 0;
    uint8_t idx = 0;
    int8_t jump_run = 0; // signed: +n above the average, -n below
    float avg = 0;

    void reset()
    {
        count = 0;
        idx = 0;
        jump_run = 0;
    }

    float push(float d)
    {
        float diff = d - avg;
        if (count > 0 && fabsf(diff) > JUMP_M)
        {
            if (diff > 0)
                jump_run = jump_run > 0 ? jump_run + 1 : 1;
            else
                jump_run = jump_run < 0 ? jump_run - 1 : -1;

            if (abs(jump_run) >= JUMP_COUNT)
                reset();
        }
        else
        {
            jump_run = 0;
        }

        samples[idx] = d;
        idx = (idx + 1) % AVG_SAMPLES;
        if (count < AVG_SAMPLES)
            count++;

        // Sorted copy (insertion sort; the window is tiny).
        float sorted[AVG_SAMPLES];
        for (uint8_t i = 0; i < count; i++)
        {
            float v = samples[i];
            int8_t j = i - 1;
            while (j >= 0 && sorted[j] > v)
            {
                sorted[j + 1] = sorted[j];
                j--;
            }
            sorted[j + 1] = v;
        }

        uint8_t trim = count / 5;
        float sum = 0;
        for (uint8_t i = trim; i < count - trim; i++)
            sum += sorted[i];
        avg = sum / (count - 2 * trim);
        return avg;
    }
};

// Exponential moving average.
struct Ema
{
    float value = 0;
    bool valid = false;

    void reset() { valid = false; }

    float push(float x, float alpha)
    {
        value = valid ? value + (x - value) * alpha : x;
        valid = true;
        return value;
    }
};

// The clock offset between the two crystals drifts slowly, but each per-frame estimate
// is noisy, and in single-sided ranging that noise is multiplied by the reply delay.
// Low-pass it (time constant ~1.2 s at 40 Hz) instead of using the raw estimate.
#define CLOCK_OFFSET_ALPHA 0.02f
#define CLOCK_OFFSET_RESYNC 5e-6f // estimate this far from the filter -> restart it

struct ClockOffsetFilter
{
    float raw = 0;
    float filt = 0;
    bool valid = false;

    float push(float estimate)
    {
        raw = estimate;
        if (!valid || fabsf(raw - filt) > CLOCK_OFFSET_RESYNC)
        {
            filt = raw;
            valid = true;
        }
        else
        {
            filt += (raw - filt) * CLOCK_OFFSET_ALPHA;
        }
        return filt;
    }
};

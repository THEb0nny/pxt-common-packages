#include "pxt.h"
#include "SoundOutput.h"
#include "melody.h"

//#define LOG DMESG
#define LOG NOLOG

namespace music {

SINGLETON(WSynthesizer);

typedef int (*gentone_t)(uintptr_t userData, uint32_t position);

static int noiseTone(uintptr_t userData, uint32_t position) {
    (void)userData;
    (void)position;
    // see https://en.wikipedia.org/wiki/Xorshift
    static uint32_t x = 0xf01ba80;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return (x & 0xffff) - 0x7fff;
}

static int sineTone(uintptr_t userData, uint32_t position) {
    (void)userData;
    int32_t p = position;
    if (p >= 512) {
	p -= 512;
    }
    if (p > 256) {
	p = 512 - p;
    }

    // Approximate sin(x * pi / 2) with the odd polynomial y = cx^5 + bx^3 + ax
    // using the constraint y(1) = 1 => a = 1 - b - c
    //   => y = c x^5 + b x^3 + (1 - b - c) * x
    //
    // Do a least-squares fit of this to sin(x * pi / 2) in the range 0..1
    // inclusive, using 21 evenly spaced points. Resulting approximation:
    //
    // sin(x*pi/2) ~= 0.0721435357258*x**5 - 0.642443736562*x**3 + 1.57030020084*x

    // Scale the constants by 32767 to match the desired output range.
    constexpr int32_t c = 0.0721435357258 * 32767;
    constexpr int32_t b = -0.642443736562 * 32767;
    constexpr int32_t a = 1.57030020084 * 32767;

    // Calculate using y = ((c * x^2 + b) * x^2 + a) * x
    //
    // The position p is x * 256, so after each multiply with p we need to
    // shift right by 8 bits to keep the decimal point in the same place.  (The
    // approximation has a negative error near x=1 which helps avoid overflow.)
    int32_t p2 = p * p;
    int32_t u = (c * p2 >> 16) + b;
    int32_t v = (u * p2 >> 16) + a;
    int32_t w = v * p >> 8;

    // The result is within 7/32767 or 0.02%, signal-to-error ratio about 38 dB.
    return position >= 512 ? -w : w;
}

static int sawtoothTone(uintptr_t userData, uint32_t position) {
    (void)userData;
    return (position << 6) - 0x7fff;
}

static int triangleTone(uintptr_t userData, uint32_t position) {
    (void)userData;
    return position < 512 ? (position << 7) - 0x7fff : ((1023 - position) << 7) - 0x7fff;
}

static int squareWaveTone(uintptr_t wave, uint32_t position) {
    return position < (102 * (wave - SW_SQUARE_10 + 1)) ? -0x7fff : 0x7fff;
}

static int silenceTone(uintptr_t userData, uint32_t position) {
    (void)userData;
    (void)position;
    return 0;
}

static gentone_t getWaveFn(uint8_t wave) {
    switch (wave) {
    case SW_TRIANGLE:
        return triangleTone;
    case SW_SAWTOOTH:
        return sawtoothTone;
    case SW_NOISE:
        return noiseTone;
    case SW_SINE:
        return sineTone;
    default:
        if (SW_SQUARE_10 <= wave && wave <= SW_SQUARE_50)
            return squareWaveTone;
        else
            return silenceTone;
    }
}

#define CLAMP(lo, v, hi) ((v) = ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v)))

int WSynthesizer::updateQueues() {
    const int maxTime = 0xffffff;
    while (1) {
        WaitingSound *p;
        int minLeft = maxTime;
        for (p = waiting; p; p = p->next) {
            int timeLeft =
                p->state == SoundState::Waiting ? p->startSampleNo - currSample : maxTime;
            if (timeLeft <= 0) {
                break;
            }
            if (timeLeft < minLeft)
                minLeft = timeLeft;
        }
        if (p) {
            PlayingSound *snd;
            int minIdx = -1;
            for (unsigned i = 0; i < MAX_SOUNDS; ++i) {
                snd = &playingSounds[i];
                if (snd->sound == NULL)
                    break;
                if (minIdx == -1 ||
                    playingSounds[minIdx].startSampleNo < playingSounds[i].startSampleNo)
                    minIdx = i;
                snd = NULL;
            }
            // if we didn't find a free slot, expel the oldest sound
            if (!snd)
                snd = &playingSounds[minIdx];
            if (snd->sound)
                snd->sound->state = SoundState::Done;
            snd->sound = p;
            p->state = SoundState::Playing;
            snd->startSampleNo = currSample;
            snd->currInstr = (SoundInstruction *)p->instructions->data;
            snd->instrEnd = snd->currInstr + p->instructions->length / sizeof(SoundInstruction);
            snd->prevVolume = -1;
        } else {
            // no more sounds to move
            return minLeft;
        }
    }
}

int WSynthesizer::fillSamples(int16_t *dst, int numsamples) {
    if (numsamples <= 0)
        return 1;

    int timeLeft = updateQueues();
    int res = waiting != NULL;

    // if there's a pending sound to be started somewhere during numsamples,
    // split the call into two
    if (timeLeft < numsamples) {
        fillSamples(dst, timeLeft);
        LOG("M split %d", timeLeft);
        fillSamples(dst + timeLeft, numsamples - timeLeft);
        return 1;
    }

    memset(dst, 0, numsamples * 2);

    uint32_t samplesPerMS = (sampleRate << 8) / 1000;
    float toneStepMult = (1024.0 * (1 << 16)) / sampleRate;
    const int MAXVAL = (1 << (OUTPUT_BITS - 1)) - 1;

    for (unsigned i = 0; i < MAX_SOUNDS; ++i) {
        PlayingSound *snd = &playingSounds[i];
        if (snd->sound == NULL)
            continue;

        res = 1;

        SoundInstruction *instr = NULL;
        gentone_t fn = NULL;
        snd->currInstr--;
        uint32_t toneStep = 0;
        int32_t toneDelta = 0;
        int32_t volumeStep = 0;
        uint32_t tonePosition = snd->tonePosition;
        uint32_t samplesLeft = 0;
        uint8_t wave = 0;
        int32_t volume = 0;
        uint32_t prevFreq = 0;
        uint32_t prevEndFreq = 0;

        for (int j = 0; j < numsamples; ++j) {
            if (samplesLeft == 0) {
                snd->currInstr++;
                if (snd->currInstr >= snd->instrEnd) {
                    break;
                }
                SoundInstruction copy = *snd->currInstr;
                instr = &copy;
                CLAMP(20, instr->frequency, 20000);
                CLAMP(20, instr->endFrequency, 20000);
                CLAMP(0, instr->startVolume, 1023);
                CLAMP(0, instr->endVolume, 1023);
                CLAMP(1, instr->duration, 60000);

                wave = instr->soundWave;
                fn = getWaveFn(wave);

                samplesLeft = (uint32_t)(instr->duration * samplesPerMS >> 8);
                // make sure the division is signed
                volumeStep = (int)((instr->endVolume - instr->startVolume) << 16) / (int)samplesLeft;

                if (j == 0 && snd->prevVolume != -1) {
                    // restore previous state
                    samplesLeft = snd->samplesLeftInCurr;
                    volume = snd->prevVolume;
                    toneStep = snd->prevToneStep;
                    toneDelta = snd->prevToneDelta;
                    prevFreq = instr->frequency;
                    prevEndFreq = instr->endFrequency;
                } else {
                    LOG("#sampl %d %p", samplesLeft, snd->currInstr);
                    volume = instr->startVolume << 16;
                    LOG("%d-%dHz %d-%d vol %d+%d", instr->frequency, instr->endFrequency,
                        instr->startVolume, instr->endVolume, volume, volumeStep);
                    if (prevFreq != instr->frequency || prevEndFreq != instr->endFrequency) {
                        toneStep = (uint32_t)(toneStepMult * instr->frequency);
                        if (instr->frequency != instr->endFrequency) {
                            uint32_t endToneStep = (uint32_t)(toneStepMult * instr->endFrequency);
                            toneDelta = (int32_t)(endToneStep - toneStep) / (int32_t)samplesLeft;
                        } else {
                            toneDelta = 0;
                        }
                        prevFreq = instr->frequency;
                        prevEndFreq = instr->endFrequency;
                    }
                }
            }

            int v = fn(wave, (tonePosition >> 16) & 1023);
            v = (v * (volume >> 16)) >> (10 + (16 - OUTPUT_BITS));

            // if (v > MAXVAL)
            //    target_panic(123);

            dst[j] += v;

            tonePosition += toneStep;
            toneStep += toneDelta;
            volume += volumeStep;
            samplesLeft--;
        }

        if (snd->currInstr >= snd->instrEnd) {
            snd->sound->state = SoundState::Done;
            snd->sound = NULL;
        } else {
            snd->tonePosition = tonePosition;
            if (samplesLeft == 0)
                samplesLeft++; // avoid infinite loop in next iteration
            snd->samplesLeftInCurr = samplesLeft;
            snd->prevVolume = volume;
            snd->prevToneDelta = toneDelta;
            snd->prevToneStep = toneStep;
        }
    }

    currSample += numsamples;

    for (int j = 0; j < numsamples; ++j) {
        if (dst[j] > MAXVAL)
            dst[j] = MAXVAL;
        else if (dst[j] < -MAXVAL)
            dst[j] = -MAXVAL;
    }

    return res;
}

//%
void enableAmp(int enabled) {
    // this is also compiled on linux
#ifdef LOOKUP_PIN
    auto pin = LOOKUP_PIN(SPEAKER_AMP);
    if (pin) {
        if (PIN(SPEAKER_AMP) & CFG_PIN_CONFIG_ACTIVE_LO)
            enabled = !enabled;
        pin->setDigitalValue(enabled);
    }
#endif
}

//%
void forceOutput(int outp) {
    auto snd = getWSynthesizer();
    snd->out.setOutput(outp);
}

//%
void queuePlayInstructions(int when, Buffer buf) {
    auto snd = getWSynthesizer();

    registerGCObj(buf);

    auto p = new WaitingSound;
    p->state = SoundState::Waiting;
    p->instructions = buf;
    p->startSampleNo = snd->currSample + when * snd->sampleRate / 1000;

    LOG("Queue %dms now=%d off=%d %p sampl:%dHz", when, snd->currSample, p->startSampleNo - snd->currSample,
        buf->data, snd->sampleRate);

    target_disable_irq();
    // add new sound to queue
    p->next = snd->waiting;
    snd->waiting = p;
    // remove sounds that have already been fully played
    while (p) {
        while (p->next && p->next->state == SoundState::Done) {
            auto todel = p->next;
            p->next = todel->next;
            unregisterGCObj(todel->instructions);
            delete todel;
        }
        p = p->next;
    }
    target_enable_irq();

    snd->poke();
}

//%
void stopPlaying() {
    LOG("stop playing!");

    auto snd = getWSynthesizer();

    target_disable_irq();
    auto p = snd->waiting;
    snd->waiting = NULL;
    for (unsigned i = 0; i < MAX_SOUNDS; ++i) {
        snd->playingSounds[i].sound = NULL;
    }
    while (p) {
        auto n = p->next;
        unregisterGCObj(p->instructions);
        delete p;
        p = n;
    }
    target_enable_irq();
}

WSynthesizer::WSynthesizer() : upstream(NULL), out(*this) {
    currSample = 0;
    active = false;
    sampleRate = out.dac.getSampleRate();
    memset(&playingSounds, 0, sizeof(playingSounds));
    waiting = NULL;
    PXT_REGISTER_RESET(stopPlaying);
}

} // namespace music

namespace jacdac {
__attribute__((weak)) void setJackRouterOutput(int output) {}
} // namespace jacdac

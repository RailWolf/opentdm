// MT19337 PRNG
// Author : skullernet
// License: public domain

#include "q_shared.h"

#define N 624
#define M 397

static uint32 mt_state[N];
static uint32 mt_index;

void init_genrand(uint32 seed)
{
    int i;

    mt_index = N;
    mt_state[0] = seed;
    for (i = 1; i < N; i++)
        mt_state[i] = seed = 1812433253U * (seed ^ seed >> 30) + i;
}

uint32 genrand_int32(void)
{
    uint32 x, y;
    int i;

    if (mt_index >= N) {
        mt_index = 0;

#define STEP(j, k) do {                         \
        x  = mt_state[i] &  (1U << 31);         \
        x += mt_state[j] & ((1U << 31) - 1);    \
        y  = x >> 1;                            \
        y ^= 0x9908B0DF & -(x & 1);             \
        mt_state[i] = mt_state[k] ^ y;          \
    } while (0)

        for (i = 0; i < N - M; i++)
            STEP(i + 1, i + M);
        for (     ; i < N - 1; i++)
            STEP(i + 1, i - N + M);
        STEP(0, M - 1);
    }

    y = mt_state[mt_index++];
    y ^= y >> 11;
    y ^= y <<  7 & 0x9D2C5680;
    y ^= y << 15 & 0xEFC60000;
    y ^= y >> 18;

    return y;
}

int32 genrand_int31(void)
{
    return genrand_int32() >> 1;
}

double genrand_float32_full(void)
{
    return genrand_int32() * (1.0 / 4294967295.0);
}

double genrand_float32_notone(void)
{
    return genrand_int32() * (1.0 / 4294967296.0);
}

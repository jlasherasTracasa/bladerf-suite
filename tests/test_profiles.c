/*
 * test_profiles.c — Colocación del canal de audio de un perfil dentro de la banda capturada.
 * Con fuente de fichero el centro no se mueve; un chan_freq fuera de banda debe ir a la
 * señal detectada más cercana dentro de banda o, sin señales, a centro + tasa/4.
 */
#include "test_util.h"

#include "profiles.h"

int main(void)
{
    int64_t f = 0;
    const int64_t c = 100400000;
    const double r = 2400000;

    CHECK(!profile_pick_audio_freq(100300000, c, r, NULL, 0, &f));
    CHECK_EQ(f, 100300000);

    /* fuera de banda y sin señales: centro + tasa/4 */
    CHECK(profile_pick_audio_freq(156800000, c, r, NULL, 0, &f));
    CHECK_EQ(f, c + 600000);

    /* con señales: la más cercana al canal pedido dentro de banda; ignora las del borde */
    int64_t sg[] = {c - 1190000, c - 300000, c + 900000};
    CHECK(profile_pick_audio_freq(c + 5000000, c, r, sg, 3, &f));
    CHECK_EQ(f, c + 900000);
    CHECK(profile_pick_audio_freq(c - 5000000, c, r, sg, 3, &f));
    CHECK_EQ(f, c - 300000);

    /* todas las perfiles con audio: el resultado siempre cae en banda */
    for (int i = 0; i < PROFILE_COUNT; i++) {
        if (!PROFILES[i].mode)
            continue;
        profile_pick_audio_freq(PROFILES[i].chan_freq, c, r, NULL, 0, &f);
        CHECK(f >= c - r / 2 && f <= c + r / 2);
    }
    return TEST_SUMMARY();
}

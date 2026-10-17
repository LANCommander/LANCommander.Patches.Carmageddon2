/* Draw-distance patches.
 *
 * - Track columns: RenderTrack (0x40d7c0) only adds the track columns (with the scenery and
 *   non-car objects in them) under the view frustum out to yon_z * gYon_factor. The graphics
 *   menu / OPTIONS.TXT YonFactor sets gYon_factor through SetYonFactor (1.0, 0.5, 0.25, 0.125).
 *   [Render] MinYonFactor raises the floor.
 * - Far clip: SetYon (0x446b70, the menu / OPTIONS.TXT Yon) clamps to >= 5. [Render] MinYon
 *   raises that floor so the menu can't pull the yon back in.
 * - Grooves (moving gates, doors, lifts...): GrooveThisDelic (0x478c80) skips both path and object
 *   animation when PointOutOfSight says the groove is beyond yon^2 (or 6 units for the non
 *   "distance" mode) or behind the camera. A frozen gate keeps colliding wherever it stopped,
 *   so AI far from the player hits gates that should be open. [Render] AnimateAllGrooves.
 */
#include "c2patch.h"

#define gYon_factor   (*(float *)0x655e60)
#define gCamera_yon   (*(float *)0x761f4c)
#define gCamera_list  ((char **)0x74c7b0)   /* br_actor *[2] */

static float g_min_yon_factor;
static float g_min_yon;

static void __stdcall SetYonFactor_hook(float factor)
{
    gYon_factor = factor < g_min_yon_factor ? g_min_yon_factor : factor;
}

static void __stdcall SetYon_hook(float yon)
{
    int i;
    if (yon < 5.f) {
        yon = 5.f;
    }
    if (yon < g_min_yon) {
        yon = g_min_yon;
    }
    for (i = 0; i < 2; i++) {
        if (gCamera_list[i]) {
            char *camera = *(char **)(gCamera_list[i] + 0x5c);   /* br_actor.type_data */
            *(float *)(camera + 0xc) = yon;                      /* br_camera.yon_z */
        }
    }
    gCamera_yon = yon;
}

void c2_render_install(void)
{
    static const uint8_t set_yon_factor[] = {0x8b, 0x44, 0x24, 0x04, 0xa3, 0x60, 0x5e, 0x65, 0x00};
    static const uint8_t set_yon[] = {0xd9, 0x44, 0x24, 0x04, 0xd8, 0x15, 0x1c, 0x9d, 0x58, 0x00};
    static const uint8_t groove_jnz[] = {0x0f, 0x85, 0x3d, 0x01, 0x00, 0x00};
    static const uint8_t nop6[] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90};

    g_min_yon_factor = c2_ini_float("Render", "MinYonFactor", 1.0f);
    if (g_min_yon_factor > 0.f && c2_expect(0x40dff0, set_yon_factor, sizeof set_yon_factor)) {
        c2_jmp(0x40dff0, SetYonFactor_hook);
        if (gYon_factor < g_min_yon_factor) {
            gYon_factor = g_min_yon_factor;
        }
        c2_log("render: MinYonFactor %.3f", g_min_yon_factor);
    }
    g_min_yon = c2_ini_float("Render", "MinYon", 0.f);
    if (g_min_yon > 0.f && c2_expect(0x446b70, set_yon, sizeof set_yon)) {
        c2_jmp(0x446b70, SetYon_hook);
        c2_log("render: MinYon %.1f", g_min_yon);
    }
    if (c2_ini_int("Render", "AnimateAllGrooves", 1)
        && c2_patch(0x478cd8, groove_jnz, nop6, sizeof nop6)) {
        c2_log("render: grooves animate at any distance");
    }
}

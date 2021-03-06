/*
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * For more information, please refer to <http://unlicense.org>
 */

/* This example illustrates the backward computation of a transmitted through
 * a simple geometry composed of two layers: Standard Rock and Air. The Air
 * medium has an exponential density profile. If a maximum kinetic energy is
 * provided the flux is integrated between kinetic_min and kinetic_max.
 * Otherwise a point estimate of the flux is done, at the provided kinetic
 * energy.
 */

/* Standard library includes */
#include <errno.h>
#include <float.h>
#include <math.h>
#include <stdlib.h>
/* The PUMAS API */
#include "pumas.h"

#ifndef M_PI
/* Define pi, if unknown */
#define M_PI 3.14159265358979323846
#endif

/* Altitude at which the primary flux is sampled */
#define PRIMARY_ALTITUDE 1E+03

/* A handle for the PUMAS simulation context */
static struct pumas_context * context = NULL;

/* Gracefully exit to the OS */
static int exit_gracefully(int rc)
{
        pumas_context_destroy(&context);
        pumas_finalise();
        exit(rc);
}

/* Error handler for PUMAS with a graceful exit */
static void handle_error(
    enum pumas_return rc, pumas_function_t * caller, const char * message)
{
        /* Dump the error summary */
        fputs("pumas: library error. See details below\n", stderr);
        fprintf(stderr, "error: %s\n", message);

        /* Exit to the OS */
        exit_gracefully(EXIT_FAILURE);
}

/* The uniform rock medium. Note that the geomagnetic field can be neglected
 * in rocks
 */
static double locals_rock(struct pumas_medium * medium,
    struct pumas_state * state, struct pumas_locals * locals)
{
        /* Set the medium density */
        locals->density = 2.65E+03;

        /* Propose a maximum stepping distance. Returning zero or less indicates
         * a uniform medium
         */
        return 0.;
}

/* The non uniform atmosphere, using an exponential model */
static double locals_air(struct pumas_medium * medium,
    struct pumas_state * state, struct pumas_locals * locals)
{
        /* Set the geomagnetic field, assumed uniform */
        locals->magnet[0] = 0.;
        locals->magnet[1] = 2E-05;
        locals->magnet[2] = -4E-05;

        /* Set the atmosphere density, depending on the altitude a.s.l. */
        const double rho0 = 1.205;
        const double h = 12E+03;
        locals->density = rho0 * exp(-state->position[2] / h);

        /* Propose a maximum stepping distance as 1 percent of the projected
         * attenuation length, for the density
         */
        const double eps = 5E-02;
        const double uz = fabs(state->direction[2]);
        return 1E-02 * h / ((uz <= eps) ? eps : uz);
}

/* The media container */
static struct pumas_medium media[2] = { { 0, &locals_rock },
        { 1, &locals_air } };

/* A simple medium with a flat rock layer and a flat atmosphere */
static double rock_thickness = 0.;

static double medium2(struct pumas_context * context,
    struct pumas_state * state, struct pumas_medium ** medium_ptr)
{
        /* Check the muon position and direction */
        const double z = state->position[2];
        const double uz = state->direction[2];

        double step = 1E+03;
        if (z < 0.) {
                /* The muon is outside of the simulation area */
                if (medium_ptr != NULL) *medium_ptr = NULL;
                step = -1.;
        } else if (z < rock_thickness) {
                if (medium_ptr != NULL) *medium_ptr = media;
                if (uz > FLT_EPSILON)
                        /* The muon is backward downgoing. The next boundary is
                         * the rock bottom. */
                        step = z / uz;
                else if (uz < -FLT_EPSILON)
                        /* The muon is backward upgoing. The next boundary is
                         * the rock-air interface. */
                        step = (z - rock_thickness) / uz;
        } else if (z < PRIMARY_ALTITUDE) {
                if (medium_ptr != NULL) *medium_ptr = media + 1;
                if (uz > FLT_EPSILON)
                        /* The muon is backward downgoing. The next boundary is
                         * the rock-air interface. */
                        step = (z - rock_thickness) / uz;
                else if (uz < -FLT_EPSILON)
                        /* The muon is backward upgoing. The next boundary is
                         * the air top. */
                        step = (z - PRIMARY_ALTITUDE) / uz;
        } else {
                /* The muon is outside of the simulation area */
                if (medium_ptr != NULL) *medium_ptr = NULL;
                step = -1.;
        }

        return step;
}

/* A basic Pseudo Random Number Generator (PRNG) providing a uniform
 * distribution over [0, 1]
 */
static double uniform01(struct pumas_context * context)
{
        return rand() / (double)RAND_MAX;
}

/* Gaisser's flux model, see e.g. the PDG */
static double flux_gaisser(double cos_theta, double Emu)
{
        const double ec = 1.1 * Emu * cos_theta;
        const double rpi = 1. + ec / 115.;
        const double rK = 1. + ec / 850.;
        return 1.4E+03 * pow(Emu, -2.7) * (1. / rpi + 0.054 / rK);
}

/* Volkova's parameterization of cos(theta*) */
static double cos_theta_star(double cos_theta)
{
        const double p[] = { 0.102573, -0.068287, 0.958633, 0.0407253,
                0.817285 };
        const double cs2 =
            (cos_theta * cos_theta + p[0] * p[0] + p[1] * pow(cos_theta, p[2]) +
                p[3] * pow(cos_theta, p[4])) /
            (1. + p[0] * p[0] + p[1] + p[3]);
        return cs2 > 0. ? sqrt(cs2) : 0.;
}

/*
 * Guan et al. parameterization of the sea level flux of atmospheric muons
 * Reference: https://arxiv.org/abs/1509.06176
 */
static double flux_gccly(double cos_theta, double kinetic_energy)
{
        const double Emu = kinetic_energy + 0.10566;
        const double cs = cos_theta_star(cos_theta);
        return pow(1. + 3.64 / (Emu * pow(cs, 1.29)), -2.7) *
            flux_gaisser(cs, Emu);
}

/* The executable main entry point */
int main(int narg, char * argv[])
{
        /* Check the number of arguments */
        if (narg < 4) {
                fprintf(stderr,
                    "Usage: %s ROCK_THICKNESS ELEVATION "
                    "KINETIC_ENERGY[_MIN] [KINETIC_ENERGY_MAX]\n",
                    argv[0]);
                exit_gracefully(EXIT_FAILURE);
        }

        /* Parse the arguments */
        rock_thickness = strtod(argv[1], NULL);
        if ((rock_thickness < 0.) || (rock_thickness > PRIMARY_ALTITUDE)) {
                errno = EINVAL;
                perror("rock thickness");
                exit_gracefully(EXIT_FAILURE);
        }
        const double elevation = strtod(argv[2], NULL);
        const double kinetic_min = strtod(argv[3], NULL);
        const double kinetic_max =
            (narg >= 5) ? strtod(argv[4], NULL) : kinetic_min;

        /* Set the error handler callback. Whenever an error occurs during a
         * PUMAS function call, the supplied error handler will be evaluated,
         * resulting in an exit to the OS
         */
        pumas_error_handler_set(&handle_error);

        /* Initialise PUMAS from a binary dump, e.g. generated by the `load`
         * example
         */
        const char * dump_file = "materials/dump";
        FILE * fid = fopen(dump_file, "rb");
        if (fid == NULL) {
                perror(dump_file);
                exit_gracefully(EXIT_FAILURE);
        }
        pumas_load(fid);
        fclose(fid);

        /* Map the PUMAS material indices */
        pumas_material_index("StandardRock", &media[0].material);
        pumas_material_index("Air", &media[1].material);

        /* Create a new PUMAS simulation context */
        pumas_context_create(&context, 0);

        /* Configure the context for a backward transport */
        context->forward = 0;

        /* Set the medium callback */
        context->medium = &medium2;

        /* Provide a PRNG for the Monte-Carlo simulation */
        context->random = &uniform01;

        /* Enable external limit on the kinetic energy */
        context->event |= PUMAS_EVENT_LIMIT_KINETIC;

        /* Run the Monte-Carlo */
        const double cos_theta = cos((90. - elevation) / 180. * M_PI);
        const double sin_theta = sqrt(1. - cos_theta * cos_theta);
        const double rk = log(kinetic_max / kinetic_min);
        double w = 0., w2 = 0.;
        const int n = 10000;
        int i;
        for (i = 0; i < n; i++) {
                /* Set the muon final state */
                double kf, wf;
                if (rk) {
                        /* The final state kinetic energy is randomised over
                         * a log-uniform distribution. The Monte-Carlo weight is
                         * initialised according to this generating bias PDF,
                         * i.e. wf = 1 / PDF(kf).
                         */
                        kf = kinetic_min * exp(rk * uniform01(context));
                        wf = kf * rk;
                } else {
                        /* A point estimate is computed, for a fixed final
                         * state energy.
                         */
                        kf = kinetic_min;
                        wf = 1;
                }
                struct pumas_state state = { .charge = -1.,
                        .kinetic = kf,
                        .weight = wf,
                        .direction = { -sin_theta, 0., -cos_theta } };

                /* Transport the muon backwards */
                const double kinetic_threshold = kinetic_max * 1E+03;
                while (state.kinetic < kinetic_threshold - FLT_EPSILON) {
                        if (state.kinetic < 1E+02 - FLT_EPSILON) {
                                /* Below 100 GeV do a detailed simulation
                                 * à la Geant4, including transverse transport
                                 */
                                context->scheme = PUMAS_SCHEME_DETAILED;
                                context->longitudinal = 0;
                                context->kinetic_limit = 1E+02;
                        } else {
                                /* Do a fast simulation à la MUM */
                                context->scheme = PUMAS_SCHEME_HYBRID;
                                context->longitudinal = 1;
                                context->kinetic_limit = kinetic_threshold;
                        }
                        enum pumas_event event;
                        struct pumas_medium * medium[2];
                        pumas_transport(context, &state, &event, medium);

                        /* Check if the muon has exit the simulation area */
                        if (event == PUMAS_EVENT_MEDIUM) {
                                if (medium[1] == NULL) {
                                        /* Update the integrated flux */
                                        const double wi = state.weight *
                                        flux_gccly(-state.direction[2],
                                            state.kinetic);
                                        w += wi;
                                        w2 += wi * wi;
                                        break;
                                }
                        } else if (event != PUMAS_EVENT_LIMIT_KINETIC) {
                                /* This should not happen */
                                fprintf(stderr,
                                    "error: unexpected PUMAS event `%d`\n",
                                    event);
                                exit_gracefully(EXIT_FAILURE);
                        }
                }
        }

        /* Print the (integrated) flux */
        w /= n;
        const double sigma =
            (rock_thickness <= 0.) ? 0. : sqrt(((w2 / n) - w * w) / n);
        const char * unit = rk ? "" : "GeV^{-1} ";
        printf("Flux : %.5lE \\pm %.5lE %sm^{-2} s^{-2} sr^{-1}\n", w, sigma,
            unit);

        /* Exit to the OS */
        exit_gracefully(EXIT_SUCCESS);
}

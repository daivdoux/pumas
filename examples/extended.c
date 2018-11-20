/* The standard C89 library */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
/* The PUMAS API */
#include "pumas.h"

/* Extension of a PUMAS medium with a uniform density and statistics records */
struct extended_medium {
        struct pumas_medium base;
        double density;
        double distance;
};

/* Medium callback implementing a simple box */
static double medium(struct pumas_context * context, struct pumas_state * state,
    struct pumas_medium ** medium_ptr)
{
        if (medium_ptr != NULL) {
                /* Get the current medium */
                const double * const r = state->position;
                struct extended_medium * extended = context->user_data;

                if ((fabs(r[0]) > 4.) || (fabs(r[1]) > 4.) ||
                    (fabs(r[2]) > 4.))
                        *medium_ptr = NULL;
                else if ((fabs(r[0]) > 1.) || (fabs(r[1]) > 1.) ||
                         (fabs(r[2]) > 1.))
                        *medium_ptr = (struct pumas_medium *)(extended + 1);
                else
                        *medium_ptr = (struct pumas_medium *)extended;
        }

        return 1.; /* For this example, we don't compute the exact geometric
                    * distance to the border. Instead we let PUMAS solve this
                    * numerically.
                    */
}

/* Generic callback for a uniform density */
static double locals_uniform(struct pumas_medium * medium,
    struct pumas_state * state, struct pumas_locals * locals)
{
        struct extended_medium * extended =
            (struct extended_medium *)medium;
        locals->density = extended->density;

        return 0.;
}

static double uniform01(struct pumas_context * context)
{
        return rand() / (double)RAND_MAX;
}

/* Error handler for PUMAS with an abrupt exit */
static void handle_error(
    enum pumas_return rc, pumas_function_t * caller, const char * message)
{
        /* Dump the error summary */
        fputs("pumas: library error. See details below\n", stderr);
        fprintf(stderr, "error: %s\n", message);

        /* Exit to the OS */
        exit(EXIT_FAILURE);
}

int main()
{
        /* Initialise the PUMAS library */
        pumas_error_handler_set(&handle_error);
        pumas_initialise(PUMAS_PARTICLE_MUON, "materials/mdf/standard.xml",
            "materials/dedx/muon");

        /* Create a new simulation context with extra storage for the
         * geometry
         */
        struct pumas_context * context;
        pumas_context_create(&context, 2 * sizeof(struct extended_medium));

        /* Configure the context */
        context->medium = &medium;
        context->random = &uniform01;
        context->longitudinal = 1;
        context->forward = 0;
        context->scheme = PUMAS_SCHEME_HYBRID;

        /* Flag a transport break on medium change */
        context->event |= PUMAS_EVENT_MEDIUM;

        /* Configure the geometry */
        struct extended_medium * extended = context->user_data;

        pumas_material_index("StandardRock", &extended[0].base.material);
        extended[0].base.locals = &locals_uniform;
        extended[0].density = 2.65E+03;
        extended[0].distance = 0.;

        pumas_material_index("Air", &extended[1].base.material);
        extended[1].base.locals = &locals_uniform;
        extended[1].density = 1.205E+00;
        extended[1].distance = 0.;

        /* Do the transport */
        struct pumas_state state = { .charge = -1., .kinetic = 1., .weight = 1.,
            .direction = {0., 0., 1.} };
        for (;;) {
                struct pumas_medium * media[2];
                const double distance0 = state.distance;
                pumas_transport(context, &state, NULL, media);

                if (media[0] != NULL) {
                        extended = (struct extended_medium *)media[0];
                        extended->distance += state.distance - distance0;
                }
                if (media[1] == NULL)
                        break;
        }

        /* Show the statistics */
        puts("# Medium statistics");
        int i;
        for (i = 0, extended = context->user_data; i < 2; i++, extended++) {
                const char * material;
                pumas_material_name(extended->base.material, &material);
                printf("- %-12s : %.5lE\n", material, extended->distance);
        }

        /* Free and return to the OS */
        pumas_context_destroy(&context);
        pumas_finalise();
        exit(EXIT_SUCCESS);
}

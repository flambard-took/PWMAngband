/*
 * File: c-option.c
 * Purpose: Options table and definitions.
 *
 * Copyright (c) 1997 Ben Harrison
 * Copyright (c) 2019 MAngband and PWMAngband Developers
 *
 * This work is free software; you can redistribute it and/or modify it
 * under the terms of either:
 *
 * a) the GNU General Public License as published by the Free Software
 *    Foundation, version 2, or
 *
 * b) the "Angband licence":
 *    This software may be copied and distributed for educational, research,
 *    and not for profit purposes provided that this copyright and statement
 *    are included in all such copies.  Other copyrights may also apply.
 */


#include "c-angband.h"


/*
 * Set an option, return true if successful
 */
bool option_set(bool *opts, const char *name, size_t val)
{
    size_t opt;

    /* Try normal options first */
    if (opts)
    {
        for (opt = 0; opt < OPT_MAX; opt++)
        {
            if (!option_name(opt) || !streq(option_name(opt), name)) continue;
            opts[opt] = (val? true: false);

            return true;
        }

        return false;
    }

    if (streq(name, "hp_warn_factor"))
    {
        /* Bounds */
        if (val > 9) val = 9;
        player->opts.hitpoint_warn = val;

        return true;
    }
    if (streq(name, "delay_factor"))
    {
        /* Bounds */
        if (val > 255) val = 255;
        player->opts.delay_factor = val;

        return true;
    }
    if (streq(name, "lazymove_delay"))
    {
        /* Bounds */
        if (val > 9) val = 9;
        player->opts.lazymove_delay = val;

        return true;
    }

    return false;
}


/*
 * Set player default options
 */
void options_init_defaults(struct player_options *opts)
{
    int i;

    /* 40ms for the delay factor */
    opts->delay_factor = 40;

    /* 30% of HP */
    opts->hitpoint_warn = 3;

    /* Initialize extra parameters */
    for (i = ITYPE_NONE; i < ITYPE_MAX; i++) opts->ignore_lvl[i] = IGNORE_BAD;
}


/*
 * Initialise options package
 */
void init_options(bool *opts)
{
    size_t opt;

    /* Allocate options to pages */
    option_init();

    /* Set defaults */
    for (opt = 0; opt < OPT_MAX; opt++)
        opts[opt] = option_normal(opt);
}

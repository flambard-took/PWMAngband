/*
 * File: gen-cave.c
 * Purpose: Generation of dungeon levels
 *
 * Copyright (c) 1997 Ben Harrison, James E. Wilson, Robert A. Koeneke
 * Copyright (c) 2013 Erik Osheim, Nick McConnell
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


#include "s-angband.h"
#include <math.h>


/*
 * In this file, we use the SQUARE_WALL flags to the info field in
 * cave->squares, which should only be applied to granite. SQUARE_WALL_SOLID
 * indicates the wall should not be tunnelled; SQUARE_WALL_INNER is the
 * inward-facing wall of a room; SQUARE_WALL_OUTER is the outer wall of a room.
 *
 * We use SQUARE_WALL_SOLID to prevent multiple corridors from piercing a wall
 * in two adjacent locations, which would be messy, and SQUARE_WALL_OUTER
 * to indicate which walls surround rooms, and may thus be pierced by corridors
 * entering or leaving the room.
 *
 * Note that a tunnel which attempts to leave a room near the edge of the
 * dungeon in a direction toward that edge will cause "silly" wall piercings,
 * but will have no permanently incorrect effects, as long as the tunnel can
 * eventually exit from another side. And note that the wall may not come back
 * into the room by the hole it left through, so it must bend to the left or
 * right and then optionally re-enter the room (at least 2 grids away). This is
 * not a problem since every room that is large enough to block the passage of
 * tunnels is also large enough to allow the tunnel to pierce the room itself
 * several times.
 *
 * Note that no two corridors may enter a room through adjacent grids, they
 * must either share an entryway or else use entryways at least two grids
 * apart. This prevents large (or "silly") doorways.
 *
 * Traditionally, to create rooms in the dungeon, it was divided up into
 * "blocks" of 11x11 grids each, and all rooms were required to occupy a
 * rectangular group of blocks. As long as each room type reserved a
 * sufficient number of blocks, the room building routines would not need to
 * check bounds. Note that in classic generation most of the normal rooms
 * actually only use 23x11 grids, and so reserve 33x11 grids.
 *
 * Note that a lot of the original motivation for the block system was the
 * fact that there was only one size of map available, 22x66 grids, and the
 * dungeon level was divided up into nine of these in three rows of three.
 * Now that the map can be resized and enlarged, and dungeon levels themselves
 * can be different sizes, much of this original motivation has gone. Blocks
 * can still be used, but different cave profiles can set their own block
 * sizes. The classic generation method still uses the traditional blocks; the
 * main motivation for using blocks now is for the aesthetic effect of placing
 * rooms on a grid.
 */


static void add_stairs(struct chunk *c, int feat)
{
    random_value *dir;

    if (feat == FEAT_MORE) dir = &((struct cave_profile *)dun->profile)->down;
    else dir = &((struct cave_profile *)dun->profile)->up;

    alloc_stairs(c, feat, dir->base + damroll(dir->dice, dir->sides));
}


/*
 * Check whether a square has one of the tunnelling helper flags
 *
 * c is the current chunk
 * (y, x) are the co-ordinates
 * flag is the relevant flag
 */
static bool square_is_granite_with_flag(struct chunk *c, struct loc *grid, int flag)
{
    if (square(c, grid)->feat != FEAT_GRANITE) return false;
    if (!sqinfo_has(square(c, grid)->info, flag)) return false;

    return true;
}


/*
 * Places a streamer of rock through dungeon.
 *
 * c is the current chunk
 * feat is the base feature (FEAT_MAGMA or FEAT_QUARTZ)
 * chance is the number of regular features per one gold
 *
 * Note that their are actually six different terrain features used to
 * represent streamers. Three each of magma and quartz, one for basic vein, one
 * with hidden gold, and one with known gold. The hidden gold types are
 * currently unused.
 */
static void build_streamer(struct chunk *c, int feat, int chance)
{
    int dir;
    struct loc grid;

    /* Hack -- choose starting point */
    loc_init(&grid, rand_spread(c->width / 2, 15), rand_spread(c->height / 2, 10));

    /* Choose a random direction */
    dir = ddd[randint0(8)];

    /* Place streamer into dungeon */
    while (true)
    {
        int i;
        struct loc change;

        /* One grid per density */
        for (i = 0; i < dun->profile->str.den; i++)
        {
            int d = dun->profile->str.rng;

            /* Pick a nearby grid */
            find_nearby_grid(c, &change, &grid, d, d);

            /* Only convert walls */
            if (square_isrock(c, &change))
            {
                /* Turn the rock into the vein type */
                square_set_feat(c, &change, feat);

                /* Sometimes add known treasure */
                if (one_in_(chance)) square_upgrade_mineral(c, &change);
            }
        }

        /* Advance the streamer */
        grid.y += ddy[dir];
        grid.x += ddx[dir];

        /* Stop at dungeon edge */
        if (!square_in_bounds(c, &grid)) break;
    }
}


/*
 * Places a streamer through dungeon.
 *
 * c is the current chunk
 * feat is the base feature (FEAT_LAVA or FEAT_WATER or FEAT_SANDWALL)
 * flag is the dungeon flag allowing the streamer to be generated
 */
static void add_streamer(struct chunk *c, int feat, int flag)
{
    struct worldpos dpos;
    struct location *dungeon;

    /* Get the dungeon */
    wpos_init(&dpos, &c->wpos.grid, 0);
    dungeon = get_dungeon(&dpos);

    /* Place streamer into dungeon */
    if (dungeon && c->wpos.depth && df_has(dungeon->flags, flag))
    {
        int i, max = 3 + randint0(3);

        for (i = 0; i < max; i++)
        {
            if (one_in_(3)) build_streamer(c, feat, 0);
        }
    }
}


/*
 * Fill a level with floor/wall type specific to a dungeon.
 *
 * c is the current chunk
 * use_floor, when TRUE, tells the function to use floor type terrains instead of walls
 */
static void fill_level(struct chunk *c, bool use_floor)
{
    struct worldpos dpos;
    struct location *dungeon;
    int count;
    struct loc begin, end;
    struct loc_iterator iter;

    /* Get the dungeon */
    wpos_init(&dpos, &c->wpos.grid, 0);
    dungeon = get_dungeon(&dpos);

    /* No dungeon here, leave basic floors/walls */
    if (!dungeon || !c->wpos.depth) return;

    /* Count features */
    for (count = 0; count < 3; count++)
    {
        struct dun_feature *feature = (use_floor? &dungeon->floors[count]: &dungeon->walls[count]);

        /* Break if not valid */
        if (!feature->percent) break;
    }

    /* Nothing to do */
    if (!count) return;

    loc_init(&begin, 1, 1);
    loc_init(&end, c->width - 1, c->height - 1);
    loc_iterator_first(&iter, &begin, &end);

    /* Fill the level */
    do
    {
        int i, chance;
        bool valid;

        /* Require a valid grid */
        valid = (use_floor? square_isfloor(c, &iter.cur): square_isrock(c, &iter.cur));
        if (!valid || square_isvault(c, &iter.cur) || square(c, &iter.cur)->mon ||
            square(c, &iter.cur)->obj)
        {
            continue;
        }

        /* Basic chance */
        chance = randint0(100);

        /* Process all features */
        for (i = 0; i < count; i++)
        {
            struct dun_feature *feature = (use_floor? &dungeon->floors[i]: &dungeon->walls[i]);

            /* Fill the level with that feature */
            if (feature->percent > chance)
            {
                square_set_feat(c, &iter.cur, feature->feat);
                break;
            }

            chance -= feature->percent;
        }
    }
    while (loc_iterator_next_strict(&iter));
}


static bool square_isperm_outer(struct chunk *c, struct loc *grid)
{
    return (square_isperm(c, grid) && !square_iswall_inner(c, grid));
}


static bool pierce_outer_locate(struct chunk *c, struct loc *tmp_grid, struct loc *offset,
    struct loc *grid1)
{
    struct loc grid;

    /* Get the "next" location */
    loc_sum(&grid, tmp_grid, offset);

    /* Stay in bounds */
    if (!square_in_bounds(c, &grid)) return false;

    /* Hack -- avoid solid permanent walls */
    if (square_isperm_outer(c, &grid)) return false;

    /* Hack -- avoid outer/solid granite walls */
    if (square_is_granite_with_flag(c, &grid, SQUARE_WALL_OUTER)) return false;
    if (square_is_granite_with_flag(c, &grid, SQUARE_WALL_SOLID)) return false;

    /* Accept this location */
    if (grid1) loc_copy(grid1, tmp_grid);
    return true;
}


static void pierce_outer_save(struct chunk *c, struct loc *grid1)
{
    struct loc begin, end;
    struct loc_iterator iter;

    /* Save the wall location */
    if (dun->wall_n < z_info->wall_pierce_max)
    {
        loc_copy(&dun->wall[dun->wall_n], grid1);
        dun->wall_n++;
    }

    loc_init(&begin, grid1->x - 1, grid1->y - 1);
    loc_init(&end, grid1->x + 1, grid1->y + 1);
    loc_iterator_first(&iter, &begin, &end);

    /* Forbid re-entry near this piercing */
    do
    {
        /* Be sure we are "in bounds" */
        if (!square_in_bounds_fully(c, &iter.cur)) continue;

        /* Convert adjacent "outer" walls as "solid" walls */
        if (square_is_granite_with_flag(c, &iter.cur, SQUARE_WALL_OUTER))
            set_marked_granite(c, &iter.cur, SQUARE_WALL_SOLID);
    }
    while (loc_iterator_next(&iter));
}


static bool pierce_outer_wide(struct chunk *c, struct loc *grid1, struct loc *offset, int sign)
{
    struct loc grid, next;

    /* Get an adjacent location */
    loc_init(&grid, grid1->x + sign * offset->y, grid1->y + sign * offset->x);

    /* Must be a valid "outer" wall */
    if (!square_in_bounds_fully(c, &grid)) return false;
    if (square_is_granite_with_flag(c, &grid, SQUARE_WALL_SOLID)) return false;
    if (!square_is_granite_with_flag(c, &grid, SQUARE_WALL_OUTER)) return false;

    /* Get the "next" location */
    loc_init(&next, grid.x + sign * offset->y, grid.y + sign * offset->x);

    /* Must be a valid location inside the room (to avoid piercing corners) */
    if (!square_in_bounds_fully(c, &next)) return false;
    if (!square_isroom(c, &next)) return false;

    /* Accept this location */
    return pierce_outer_locate(c, &grid, offset, NULL);
}


static bool possible_wide_tunnel(struct chunk *c, struct loc *grid1, struct loc *offset, int sign)
{
    struct loc grid;

    /* Get adjacent location */
    loc_init(&grid, grid1->x + sign * offset->y, grid1->y + sign * offset->x);

    /* Must be a valid granite wall */
    if (!square_in_bounds_fully(c, &grid)) return false;
    if (!square_isrock(c, &grid)) return false;

    /* Hack -- avoid outer/solid granite walls */
    if (square_is_granite_with_flag(c, &grid, SQUARE_WALL_OUTER)) return false;
    if (square_is_granite_with_flag(c, &grid, SQUARE_WALL_SOLID)) return false;

    /* Accept this location */
    return true;
}


/*
 * Constructs a tunnel between two points
 *
 * c is the current chunk
 * grid1 is the location of the first point
 * grid2 is the location of the second point
 *
 * This function must be called BEFORE any streamers are created, since we use
 * granite with the special SQUARE_WALL flags to keep track of legal places for
 * corridors to pierce rooms.
 *
 * We queue the tunnel grids to prevent door creation along a corridor which
 * intersects itself.
 *
 * We queue the wall piercing grids to prevent a corridor from leaving
 * a room and then coming back in through the same entrance.
 *
 * We pierce grids which are outer walls of rooms, and when we do so, we change
 * all adjacent outer walls of rooms into solid walls so that no two corridors
 * may use adjacent grids for exits.
 *
 * The solid wall check prevents corridors from chopping the corners of rooms
 * off, as well as silly door placement, and excessively wide room entrances.
 */
static void build_tunnel(struct chunk *c, struct loc *first, struct loc *second)
{
    int i;
    int main_loop_count = 0;
    struct loc start, tmp_grid, offset, cur_offset, grid1, grid2;
    int sign = 1, feat, length = 0;

    /* Used to prevent excessive door creation along overlapping corridors. */
    bool door_flag = false;

    loc_copy(&grid1, first);
    loc_copy(&grid2, second);

    /* Reset the arrays */
    dun->tunn_n = 0;
    dun->wall_n = 0;

    /* Save the starting location */
    loc_copy(&start, &grid1);

    /* Start out in the correct direction */
    correct_dir(&offset, &grid1, &grid2);

    loc_copy(&cur_offset, &offset);

    /* Keep going until done (or bored) */
    while (!loc_eq(&grid1, &grid2))
    {
        /* Hack -- paranoia -- prevent infinite loops */
        if (main_loop_count++ > 2000) break;

        /* Allow bends in the tunnel */
        if (magik(dun->profile->tun.chg))
        {
            /* Get the correct direction */
            correct_dir(&offset, &grid1, &grid2);

            /* Random direction */
            if (magik(dun->profile->tun.rnd)) rand_dir(&offset);
        }

        /* Get the next location */
        loc_sum(&tmp_grid, &grid1, &offset);

        /* Be sure we are "in bounds" */
        while (!square_in_bounds(c, &tmp_grid))
        {
            /* Get the correct direction */
            correct_dir(&offset, &grid1, &grid2);

            /* Random direction */
            if (magik(dun->profile->tun.rnd)) rand_dir(&offset);

            /* Get the next location */
            loc_sum(&tmp_grid, &grid1, &offset);
        }

        if (loc_eq(&offset, &cur_offset))
            length++;
        else
        {
            loc_copy(&cur_offset, &offset);
            length = 0;
        }

        /* Avoid the edge of the dungeon */
        if (square_isperm_outer(c, &tmp_grid)) continue;

        /* Avoid "solid" granite walls */
        if (square_is_granite_with_flag(c, &tmp_grid, SQUARE_WALL_SOLID)) continue;

        /* Pierce "outer" walls of rooms */
        if (square_is_granite_with_flag(c, &tmp_grid, SQUARE_WALL_OUTER))
        {
            if (!pierce_outer_locate(c, &tmp_grid, &offset, &grid1)) continue;

            /* HIGHLY EXPERIMENTAL: turn-based mode (for single player games) */
            if (TURN_BASED) pierce_outer_save(c, &grid1);

            /* PWMAngband: try to create wide openings */
            else if (pierce_outer_wide(c, &grid1, &offset, sign))
            {
                struct loc next;

                pierce_outer_save(c, &grid1);

                /* Current adjacent location accepted */
                loc_init(&next, grid1.x + sign * offset.y, grid1.y + sign * offset.x);
                pierce_outer_save(c, &next);
            }
            else if (pierce_outer_wide(c, &grid1, &offset, -sign))
            {
                struct loc next;

                pierce_outer_save(c, &grid1);

                /* Other adjacent location accepted */
                sign = -sign;
                loc_init(&next, grid1.x + sign * offset.y, grid1.y + sign * offset.x);
                pierce_outer_save(c, &next);
            }
            else
            {
                pierce_outer_save(c, &grid1);

                /* No adjacent location accepted: duplicate the entry for later */
                pierce_outer_save(c, &grid1);
            }
        }

        /* Travel quickly through rooms */
        else if (square_isroom(c, &tmp_grid))
        {
            /* Accept the location */
            loc_copy(&grid1, &tmp_grid);
        }

        /* Tunnel through all other walls */
        else if (square_isrock(c, &tmp_grid))
        {
            /* Accept this location */
            loc_copy(&grid1, &tmp_grid);

            /* Save the tunnel location */
            if (dun->tunn_n < z_info->tunn_grid_max)
            {
                loc_copy(&dun->tunn[dun->tunn_n], &grid1);
                dun->tunn_n++;
            }

            /* HIGHLY EXPERIMENTAL: turn-based mode (for single player games) */
            if (TURN_BASED) {}

            /* PWMAngband: try to create wide tunnels */
            else if ((dun->tunn_n < z_info->tunn_grid_max) &&
                possible_wide_tunnel(c, &grid1, &offset, sign))
            {
                struct loc next;

                loc_init(&next, grid1.x + sign * offset.y, grid1.y + sign * offset.x);
                loc_copy(&dun->tunn[dun->tunn_n], &next);
                dun->tunn_n++;

                /* Add some holes for possible stair placement in long corridors */
                if ((length >= 10) && one_in_(20) && (dun->tunn_n < z_info->tunn_grid_max) &&
                    possible_wide_tunnel(c, &next, &offset, sign))
                {
                    loc_init(&dun->tunn[dun->tunn_n], grid1.x + sign * offset.y * 2,
                        grid1.y + sign * offset.x * 2);
                    dun->tunn_flag[dun->tunn_n] = 1;
                    dun->tunn_n++;
                    length = 0;
                }
            }

            /* Allow door in next grid */
            door_flag = false;
        }

        /* Handle corridor intersections or overlaps */
        else
        {
            /* Accept the location */
            loc_copy(&grid1, &tmp_grid);

            /* Collect legal door locations */
            if (!door_flag)
            {
                /* Save the door location */
                if (dun->door_n < z_info->level_door_max)
                {
                    loc_copy(&dun->door[dun->door_n], &grid1);
                    dun->door_n++;
                }

                /* HIGHLY EXPERIMENTAL: turn-based mode (for single player games) */
                if (TURN_BASED) {}

                /* PWMAngband: try to create wide intersections */
                else
                {
                    struct loc next;

                    loc_init(&next, grid1.x + sign * offset.y, grid1.y + sign * offset.x);
                    if (square_in_bounds_fully(c, &next) && (dun->door_n < z_info->level_door_max))
                    {
                        loc_copy(&dun->door[dun->door_n], &next);
                        dun->door_n++;
                    }
                }

                /* No door in next grid */
                door_flag = true;
            }

            /* Hack -- allow pre-emptive tunnel termination */
            if (!magik(dun->profile->tun.con))
            {
                /* Offset between grid1 and start */
                loc_diff(&tmp_grid, &grid1, &start);

                /* Terminate the tunnel */
                if ((ABS(tmp_grid.x) > 10) || (ABS(tmp_grid.y) > 10)) break;
            }
        }
    }

    /* Turn the tunnel into corridor */
    for (i = 0; i < dun->tunn_n; i++)
    {
        /* Clear previous contents, add a floor */
        square_set_feat(c, &dun->tunn[i], FEAT_FLOOR);

        /* Add some holes for possible stair placement in long corridors */
        if (dun->tunn_flag[i]) sqinfo_on(square(c, &dun->tunn[i])->info, SQUARE_STAIRS);
    }

    /* Apply the piercings that we found */
    for (i = 0; i < dun->wall_n; i++)
    {
        /* Convert to floor grid */
        square_set_feat(c, &dun->wall[i], FEAT_FLOOR);

        /* HIGHLY EXPERIMENTAL: turn-based mode (for single player games) */
        if (TURN_BASED) {}

        /* PWMAngband: for wide openings, duplicate the door feature */
        else if (i % 2)
        {
            if (feat) square_set_feat(c, &dun->wall[i], feat);
            feat = 0;
            continue;
        }

        /* Place a random door */
        if (magik(dun->profile->tun.pen))
        {
            place_random_door(c, &dun->wall[i]);
            feat = square(c, &dun->wall[i])->feat;
        }
        else
            feat = 0;
    }
}


/*
 * Count the number of corridor grids adjacent to the given grid.
 *
 * This routine currently only counts actual "empty floor" grids which are not
 * in rooms.
 *
 * c is the current chunk
 * grid1 is the location
 *
 * TODO: count stairs, open doors, closed doors?
 */
static int next_to_corr(struct chunk *c, struct loc *grid1)
{
    int i, k = 0;

    my_assert(square_in_bounds(c, grid1));

    /* Scan adjacent grids */
    for (i = 0; i < 4; i++)
    {
        struct loc grid;

        /* Extract the location */
        loc_sum(&grid, grid1, &ddgrid_ddd[i]);

        /* Count only floors which aren't part of rooms */
        if (square_isfloor(c, &grid) && !square_isroom(c, &grid)) k++;
    }

    /* Return the number of corridors */
    return k;
}


/*
 * Returns whether a doorway can be built in a space.
 *
 * c is the current chunk
 * grid is the location
 *
 * To have a doorway, a space must be adjacent to at least two corridors and be
 * between two walls.
 */
static bool possible_doorway(struct chunk *c, struct loc *grid)
{
    struct loc grid1, grid2;

    my_assert(square_in_bounds(c, grid));

    if (next_to_corr(c, grid) < 2) return false;

    next_grid(&grid1, grid, DIR_N);
    next_grid(&grid2, grid, DIR_S);
    if (square_isstrongwall(c, &grid1) && square_isstrongwall(c, &grid2))
        return true;

    next_grid(&grid1, grid, DIR_W);
    next_grid(&grid2, grid, DIR_E);
    if (square_isstrongwall(c, &grid1) && square_isstrongwall(c, &grid2))
        return true;

    return false;
}


/*
 * Returns whether a wide doorway can be built in a space.
 *
 * To have a wide doorway, a space must be adjacent to three corridors and a wall.
 */
static bool possible_wide_doorway(struct chunk *c, struct loc *grid, struct loc *choice)
{
    struct loc next;

    my_assert(square_in_bounds(c, grid));

    if (next_to_corr(c, grid) != 3) return false;

    next_grid(&next, grid, DIR_N);
    if (square_isstrongwall(c, &next))
    {
        next_grid(choice, grid, DIR_S);
        return true;
    }
    next_grid(&next, grid, DIR_S);
    if (square_isstrongwall(c, &next))
    {
        next_grid(choice, grid, DIR_N);
        return true;
    }
    next_grid(&next, grid, DIR_W);
    if (square_isstrongwall(c, &next))
    {
        next_grid(choice, grid, DIR_E);
        return true;
    }
    next_grid(&next, grid, DIR_E);
    if (square_isstrongwall(c, &next))
    {
        next_grid(choice, grid, DIR_W);
        return true;
    }
    return false;
}


/*
 * Places door or trap at y, x position if at least 2 walls found
 *
 * c is the current chunk
 * grid is the location
 */
static void try_door(struct chunk *c, struct loc *grid)
{
    struct loc grid1, grid2;

    my_assert(square_in_bounds(c, grid));

    if (square_isstrongwall(c, grid)) return;
    if (square_isroom(c, grid)) return;
    if (square_isplayertrap(c, grid)) return;
    if (square_isdoor(c, grid)) return;

    if (magik(dun->profile->tun.jct))
    {
        if (possible_doorway(c, grid))
            place_random_door(c, grid);

        /* HIGHLY EXPERIMENTAL: turn-based mode (for single player games) */
        else if (TURN_BASED) {}

        /* PWMAngband: for wide intersections, we need two valid adjacent spaces that face each other */
        else if (possible_wide_doorway(c, grid, &grid1) &&
            possible_wide_doorway(c, &grid1, &grid2) && loc_eq(&grid2, grid))
        {
            place_random_door(c, grid);
            square_set_feat(c, &grid1, square(c, grid)->feat);
        }
    }
    else if (CHANCE(dun->profile->tun.jct, 500))
    {
        if (possible_doorway(c, grid))
            place_trap(c, grid, -1, c->wpos.depth);

        /* HIGHLY EXPERIMENTAL: turn-based mode (for single player games) */
        else if (TURN_BASED) {}

        /* PWMAngband: for wide intersections, we need two valid adjacent spaces that face each other */
        else if (possible_wide_doorway(c, grid, &grid1) &&
            possible_wide_doorway(c, &grid1, &grid2) && loc_eq(&grid2, grid))
        {
            place_trap(c, grid, -1, c->wpos.depth);
            place_trap(c, &grid1, -1, c->wpos.depth);
        }
    }
}


static void ensure_connectedness(struct chunk *c);


/*
 * Remove unused holes in corridors.
 *
 * c is the current chunk
 */
static void remove_unused_holes(struct chunk *c)
{
    struct loc begin, end;
    struct loc_iterator iter;

    loc_init(&begin, 1, 1);
    loc_init(&end, c->width - 1, c->height - 1);
    loc_iterator_first(&iter, &begin, &end);

    do
    {
        if (sqinfo_has(square(c, &iter.cur)->info, SQUARE_STAIRS))
        {
            int k = 0;
            struct loc grid;

            next_grid(&grid, &iter.cur, DIR_S);
            if (feat_is_wall(square(c, &grid)->feat)) k++;
            next_grid(&grid, &iter.cur, DIR_SE);
            if (feat_is_wall(square(c, &grid)->feat)) k++;
            next_grid(&grid, &iter.cur, DIR_E);
            if (feat_is_wall(square(c, &grid)->feat)) k++;
            next_grid(&grid, &iter.cur, DIR_NE);
            if (feat_is_wall(square(c, &grid)->feat)) k++;
            next_grid(&grid, &iter.cur, DIR_N);
            if (feat_is_wall(square(c, &grid)->feat)) k++;
            next_grid(&grid, &iter.cur, DIR_NW);
            if (feat_is_wall(square(c, &grid)->feat)) k++;
            next_grid(&grid, &iter.cur, DIR_W);
            if (feat_is_wall(square(c, &grid)->feat)) k++;
            next_grid(&grid, &iter.cur, DIR_SW);
            if (feat_is_wall(square(c, &grid)->feat)) k++;

            /* Remove unused holes in corridors */
            if (square_isempty(c, &iter.cur) && (k == 5))
                square_set_feat(c, &iter.cur, FEAT_GRANITE);

            sqinfo_off(square(c, &iter.cur)->info, SQUARE_STAIRS);
        }
    }
    while (loc_iterator_next_strict(&iter));
}


/*
 * Generate a new dungeon level
 *
 * p is the player
 * wpos is the position on the world map
 */
struct chunk *classic_gen(struct player *p, struct worldpos *wpos, int min_height, int min_width)
{
    int i, j, k;
    struct loc grid;
    int by, bx = 0, tby, tbx, key, rarity, built;
    int num_rooms, size_percent;
    int dun_unusual = dun->profile->dun_unusual;
    bool **blocks_tried;
    struct chunk *c;

    /*
     * Possibly generate fewer rooms in a smaller area via a scaling factor.
     * Since we scale row_blocks and col_blocks by the same amount, dun->profile->dun_rooms
     * gives the same "room density" no matter what size the level turns out
     * to be.
     */
    i = randint1(10) + wpos->depth / 24;
    if (is_quest(wpos->depth)) size_percent = 100;
    else if (i < 2) size_percent = 75;
    else if (i < 3) size_percent = 80;
    else if (i < 4) size_percent = 85;
    else if (i < 5) size_percent = 90;
    else if (i < 6) size_percent = 95;
    else size_percent = 100;

    /* Scale the various generation variables */
    num_rooms = dun->profile->dun_rooms * size_percent / 100;
    dun->block_hgt = dun->profile->block_size;
    dun->block_wid = dun->profile->block_size;
    c = cave_new(z_info->dungeon_hgt, z_info->dungeon_wid);
    memcpy(&c->wpos, wpos, sizeof(struct worldpos));
    player_cave_new(p, z_info->dungeon_hgt, z_info->dungeon_wid);

    /* Fill cave area with basic granite */
    fill_rectangle(c, 0, 0, c->height - 1, c->width - 1, FEAT_GRANITE, SQUARE_NONE);

    /* Actual maximum number of rooms on this level */
    dun->row_blocks = c->height / dun->block_hgt;
    dun->col_blocks = c->width / dun->block_wid;

    /* Initialize the room table */
    dun->room_map = mem_zalloc(dun->row_blocks * sizeof(bool*));
    for (i = 0; i < dun->row_blocks; i++)
        dun->room_map[i] = mem_zalloc(dun->col_blocks * sizeof(bool));

    /* Initialize the block table */
    blocks_tried = mem_zalloc(dun->row_blocks * sizeof(bool*));
    for (i = 0; i < dun->row_blocks; i++)
        blocks_tried[i] = mem_zalloc(dun->col_blocks * sizeof(bool));

    /* No rooms yet, pits or otherwise. */
    dun->pit_num = 0;
    dun->cent_n = 0;

    /*
     * Build some rooms. Note that the theoretical maximum number of rooms
     * in this profile is currently 36, so built never reaches num_rooms,
     * and room generation is always terminated by having tried all blocks
     */
    built = 0;
    while (built < num_rooms)
    {
        /* Count the room blocks we haven't tried yet. */
        j = 0;
        tby = 0;
        tbx = 0;
        for (by = 0; by < dun->row_blocks; by++)
        {
            for (bx = 0; bx < dun->col_blocks; bx++)
            {
                if (blocks_tried[by][bx]) continue;
                j++;
                if (one_in_(j))
                {
                    tby = by;
                    tbx = bx;
                }
            }
        }
        bx = tbx;
        by = tby;

        /* If we've tried all blocks we're done. */
        if (j == 0) break;

        if (blocks_tried[by][bx]) quit_fmt("generation: inconsistent blocks");

        /* Mark that we are trying this block. */
        blocks_tried[by][bx] = true;

        /* Roll for random key (to be compared against a profile's cutoff) */
        key = randint0(100);

        /*
         * We generate a rarity number to figure out how exotic to make the
         * room. This number has a depth/dun_unusual chance of being > 0,
         * a depth^2/dun_unusual^2 chance of being > 1, up to dun->profile->max_rarity.
         */
        i = 0;
        rarity = 0;
        while ((i == rarity) && (i < dun->profile->max_rarity))
        {
            if (randint0(dun_unusual) < 50 + wpos->depth / 2) rarity++;
            i++;
        }

        /*
         * Once we have a key and a rarity, we iterate through out list of
         * room profiles looking for a match (whose cutoff > key and whose
         * rarity > this rarity). We try building the room, and if it works
         * then we are done with this iteration. We keep going until we find
         * a room that we can build successfully or we exhaust the profiles.
         */
        for (i = 0; i < dun->profile->n_room_profiles; i++)
        {
            struct room_profile profile = dun->profile->room_profiles[i];

            if (profile.rarity > rarity) continue;
            if (profile.cutoff <= key) continue;

            if (room_build(p, c, by, bx, profile, false))
            {
                built++;
                break;
            }
        }
    }

    for (i = 0; i < dun->row_blocks; i++)
    {
        mem_free(blocks_tried[i]);
        mem_free(dun->room_map[i]);
    }
    mem_free(blocks_tried);
    mem_free(dun->room_map);

    /* Generate permanent walls around the edge of the generated area */
    draw_rectangle(c, 0, 0, c->height - 1, c->width - 1, FEAT_PERM, SQUARE_NONE);

    /* Hack -- scramble the room order */
    for (i = 0; i < dun->cent_n; i++)
    {
        int pick1 = randint0(dun->cent_n);
        int pick2 = randint0(dun->cent_n);
        struct loc tmp;

        loc_copy(&tmp, &dun->cent[pick1]);
        loc_copy(&dun->cent[pick1], &dun->cent[pick2]);
        loc_copy(&dun->cent[pick2], &tmp);
    }

    /* Start with no tunnel doors */
    dun->door_n = 0;

    /* Hack -- connect the first room to the last room */
    loc_copy(&grid, &dun->cent[dun->cent_n - 1]);

    /* Connect all the rooms together */
    for (i = 0; i < dun->cent_n; i++)
    {
        /* Connect the room to the previous room */
        build_tunnel(c, &dun->cent[i], &grid);

        /* Remember the "previous" room */
        loc_copy(&grid, &dun->cent[i]);
    }

    /* Place intersection doors */
    for (i = 0; i < dun->door_n; i++)
    {
        /* Try placing doors */
        next_grid(&grid, &dun->door[i], DIR_W);
        try_door(c, &grid);
        next_grid(&grid, &dun->door[i], DIR_E);
        try_door(c, &grid);
        next_grid(&grid, &dun->door[i], DIR_N);
        try_door(c, &grid);
        next_grid(&grid, &dun->door[i], DIR_S);
        try_door(c, &grid);
    }

    ensure_connectedness(c);

    /* Add some magma streamers */
    for (i = 0; i < dun->profile->str.mag; i++)
        build_streamer(c, FEAT_MAGMA, dun->profile->str.mc);

    /* Add some quartz streamers */
    for (i = 0; i < dun->profile->str.qua; i++)
        build_streamer(c, FEAT_QUARTZ, dun->profile->str.qc);

    /* Add some streamers */
    add_streamer(c, FEAT_LAVA, DF_LAVA_RIVER);
    add_streamer(c, FEAT_WATER, DF_WATER_RIVER);
    add_streamer(c, FEAT_SANDWALL, DF_SAND_VEIN);

    /* Tweak floors and walls */
    fill_level(c, false);
    fill_level(c, true);

    /* Place stairs near some walls */
    add_stairs(c, FEAT_MORE);
    add_stairs(c, FEAT_LESS);

    /* Remove holes in corridors that were not used for stair placement */
    remove_unused_holes(c);

    /* General amount of rubble, traps and monsters */
    k = MAX(MIN(wpos->depth / 3, 10), 2);

    /* Put some rubble in corridors */
    alloc_objects(p, c, SET_CORR, TYP_RUBBLE, randint1(k), wpos->depth, 0);

    /* Place some traps in the dungeon, reduce frequency by factor of 5 */
    alloc_objects(p, c, SET_CORR, TYP_TRAP, randint1(k) / 5, wpos->depth, 0);

    /* Place some fountains in rooms */
    alloc_objects(p, c, SET_ROOM, TYP_FOUNTAIN, randint0(1 + k / 2), wpos->depth, 0);

    /* Determine the character location */
    new_player_spot(c, p);

    /* Pick a base number of monsters */
    i = z_info->level_monster_min + randint1(8) + k;

    /* Put some monsters in the dungeon */
    for (; i > 0; i--)
        pick_and_place_distant_monster(p, c, 0, MON_ASLEEP);

    /* Put some objects in rooms */
    alloc_objects(p, c, SET_ROOM, TYP_OBJECT, Rand_normal(z_info->room_item_av, 3), wpos->depth,
        ORIGIN_FLOOR);

    /* Put some objects/gold in the dungeon */
    alloc_objects(p, c, SET_BOTH, TYP_OBJECT, Rand_normal(z_info->both_item_av, 3), wpos->depth,
        ORIGIN_FLOOR);
    alloc_objects(p, c, SET_BOTH, TYP_GOLD, Rand_normal(z_info->both_gold_av, 3), wpos->depth,
        ORIGIN_FLOOR);

    /* Apply illumination */
    player_cave_clear(p, true);
    cave_illuminate(p, c, true);

    return c;
}


/* ------------------ LABYRINTH ---------------- */


/*
 * Given an adjoining wall (a wall which separates two labyrinth cells)
 * set a and b to point to the cell indices which are separated. Used by
 * labyrinth_gen().
 *
 * i is the wall index
 * w is the width of the labyrinth
 * a, b are the two cell indices
 */
static void lab_get_adjoin(int i, int w, int *a, int *b)
{
    struct loc grid, next;

    i_to_grid(i, w, &grid);
    if (grid.x % 2 == 0)
    {
        next_grid(&next, &grid, DIR_N);
        *a = grid_to_i(&next, w);
        next_grid(&next, &grid, DIR_S);
        *b = grid_to_i(&next, w);
    }
    else
    {
        next_grid(&next, &grid, DIR_W);
        *a = grid_to_i(&next, w);
        next_grid(&next, &grid, DIR_E);
        *b = grid_to_i(&next, w);
    }
}


/*
 * Return whether a grid is in a tunnel.
 *
 * c is the current chunk
 * grid is the location
 *
 * For our purposes a tunnel is a horizontal or vertical path, not an
 * intersection. Thus, we want the squares on either side to walls in one
 * case (e.g. up/down) and open in the other case (e.g. left/right). We don't
 * want a square that represents an intersection point.
 *
 * The high-level idea is that these are squares which can't be avoided (by
 * walking diagonally around them).
 */
static bool lab_is_tunnel(struct chunk *c, struct loc *grid)
{
    bool west, east, north, south;
    struct loc next;

    next_grid(&next, grid, DIR_W);
    west = square_isopen(c, &next);
    next_grid(&next, grid, DIR_E);
    east = square_isopen(c, &next);
    next_grid(&next, grid, DIR_N);
    north = square_isopen(c, &next);
    next_grid(&next, grid, DIR_S);
    south = square_isopen(c, &next);

    return ((north == south) && (west == east) && (north != west));
}


/*
 * Helper function for lab_is_wide_tunnel.
 */
static bool lab_is_wide_tunnel_aux(struct chunk *c, struct loc *grid, bool recursive,
    struct loc *choice)
{
    bool west, east, north, south;
    struct loc next;

    next_grid(&next, grid, DIR_W);
    west = square_isopen(c, &next);
    next_grid(&next, grid, DIR_E);
    east = square_isopen(c, &next);
    next_grid(&next, grid, DIR_N);
    north = square_isopen(c, &next);
    next_grid(&next, grid, DIR_S);
    south = square_isopen(c, &next);

    if (west && east && north && !south)
    {
        if (recursive)
        {
            loc_init(choice, 0, -1);
            next_grid(&next, grid, DIR_N);
            return lab_is_wide_tunnel_aux(c, &next, false, choice);
        }
        return true;
    }
    if (west && east && !north && south)
    {
        if (recursive)
        {
            loc_init(choice, 0, 1);
            next_grid(&next, grid, DIR_S);
            return lab_is_wide_tunnel_aux(c, &next, false, choice);
        }
        return true;
    }
    if (west && !east && north && south)
    {
        if (recursive)
        {
            loc_init(choice, -1, 0);
            next_grid(&next, grid, DIR_W);
            return lab_is_wide_tunnel_aux(c, &next, false, choice);
        }
        return true;
    }
    if (!west && east && north && south)
    {
        if (recursive)
        {
            loc_init(choice, 1, 0);
            next_grid(&next, grid, DIR_E);
            return lab_is_wide_tunnel_aux(c, &next, false, choice);
        }
        return true;
    }
    return false;
}


/*
 * Return whether (x, y) is in a wide tunnel.
 */
static bool lab_is_wide_tunnel(struct chunk *c, struct loc *grid, struct loc *choice)
{
    return lab_is_wide_tunnel_aux(c, grid, true, choice);
}


/*
 * Build a labyrinth chunk of a given height and width
 *
 * p is the player
 * wpos is the position on the world map
 * (h, w) are the dimensions of the chunk
 * lit is whether the labyrinth is lit
 * soft is true if we use regular walls, false if permanent walls
 * wide is true if the labyrinth has wide corridors
 */
static struct chunk *labyrinth_chunk(struct player *p, struct worldpos *wpos, int h, int w,
    bool lit, bool soft, bool wide)
{
    int i, j, k;
    struct loc grid;

    /* This is the number of squares in the labyrinth */
    int n = h * w;

    /*
     * NOTE: 'sets' and 'walls' are too large... we only need to use about
     * 1/4 as much memory. However, in that case, the addressing math becomes
     * a lot more complicated, so let's just stick with this because it's
     * easier to read.
     */

    /*
     * 'sets' tracks connectedness; if sets[i] == sets[j] then cells i and j
     * are connected to each other in the maze.
     */
    int *sets;

    /* 'walls' is a list of wall coordinates which we will randomize */
    int *walls;

    /* The labyrinth chunk */
    struct chunk *c = cave_new((wide? h * 2: h) + 2, (wide? w * 2: w) + 2);

    memcpy(&c->wpos, wpos, sizeof(struct worldpos));
    player_cave_new(p, (wide? h * 2: h) + 2, (wide? w * 2: w) + 2);

    /* Allocate our arrays */
    sets = mem_zalloc(n * sizeof(int));
    walls = mem_zalloc(n * sizeof(int));

    /* Bound with perma-rock */
    draw_rectangle(c, 0, 0, (wide? h * 2: h) + 1, (wide? w * 2: w) + 1, FEAT_PERM, SQUARE_NONE);

    /* Fill the labyrinth area with rock */
    if (soft)
        fill_rectangle(c, 1, 1, h, w, FEAT_GRANITE, SQUARE_WALL_SOLID);
    else
        fill_rectangle(c, 1, 1, h, w, FEAT_PERM, SQUARE_NONE);

    /* Initialize each wall. */
    for (i = 0; i < n; i++)
    {
        walls[i] = i;
        sets[i] = -1;
    }

    /* Cut out a grid of 1x1 rooms which we will call "cells" */
    for (grid.y = 0; grid.y < h; grid.y += 2)
    {
        for (grid.x = 0; grid.x < w; grid.x += 2)
        {
            struct loc diag;

            k = grid_to_i(&grid, w);
            next_grid(&diag, &grid, DIR_SE);
            sets[k] = k;
            square_set_feat(c, &diag, FEAT_FLOOR);
            if (lit) sqinfo_on(square(c, &diag)->info, SQUARE_GLOW);
        }
    }

    /* Shuffle the walls, using Knuth's shuffle. */
    shuffle(walls, n);

    /*
     * For each adjoining wall, look at the cells it divides. If they aren't
     * in the same set, remove the wall and join their sets.
     *
     * This is a randomized version of Kruskal's algorithm.
     */
    for (i = 0; i < n; i++)
    {
        int a, b;

        j = walls[i];

        /* If this cell isn't an adjoining wall, skip it */
        i_to_grid(j, w, &grid);
        if ((grid.x < 1 && grid.y < 1) || (grid.x > w - 2 && grid.y > h - 2)) continue;
        if (grid.x % 2 == grid.y % 2) continue;

        /* Figure out which cells are separated by this wall */
        lab_get_adjoin(j, w, &a, &b);

        /* If the cells aren't connected, kill the wall and join the sets */
        if (sets[a] != sets[b])
        {
            int sa = sets[a];
            int sb = sets[b];
            struct loc diag;

            next_grid(&diag, &grid, DIR_SE);
            square_set_feat(c, &diag, FEAT_FLOOR);
            if (lit) sqinfo_on(square(c, &diag)->info, SQUARE_GLOW);

            for (k = 0; k < n; k++)
            {
                if (sets[k] == sb) sets[k] = sa;
            }
        }
    }

    /* Hack -- allow wide corridors */
    if (wide)
    {
        /* Simply stretch the original labyrinth area */
        for (grid.y = h; grid.y >= 1; grid.y--)
        {
            for (grid.x = w; grid.x >= 1; grid.x--)
            {
                struct loc stretch;

                loc_init(&stretch, grid.x * 2, grid.y * 2);
                square(c, &stretch)->feat = square(c, &grid)->feat;
                sqinfo_wipe(square(c, &stretch)->info);
                sqinfo_copy(square(c, &stretch)->info, square(c, &grid)->info);
                loc_init(&stretch, grid.x * 2 - 1, grid.y * 2);
                square(c, &stretch)->feat = square(c, &grid)->feat;
                sqinfo_wipe(square(c, &stretch)->info);
                sqinfo_copy(square(c, &stretch)->info, square(c, &grid)->info);
                loc_init(&stretch, grid.x * 2, grid.y * 2 - 1);
                square(c, &stretch)->feat = square(c, &grid)->feat;
                sqinfo_wipe(square(c, &stretch)->info);
                sqinfo_copy(square(c, &stretch)->info, square(c, &grid)->info);
                loc_init(&stretch, grid.x * 2 - 1, grid.y * 2 - 1);
                square(c, &stretch)->feat = square(c, &grid)->feat;
                sqinfo_wipe(square(c, &stretch)->info);
                sqinfo_copy(square(c, &stretch)->info, square(c, &grid)->info);
            }
        }
    }

    /* Generate a door for every 100 squares in the labyrinth */
    for (i = n / 100; i > 0; i--)
    {
        /* Try 10 times to find a useful place for a door, then place it */
        for (j = 0; j < 10; j++)
        {
            find_empty(c, &grid);

            /* Hack -- for wide corridors, place two doors */
            if (wide)
            {
                struct loc choice, next;

                if (lab_is_wide_tunnel(c, &grid, &choice))
                {
                    place_closed_door(c, &grid);
                    loc_sum(&next, &grid, &choice);
                    place_closed_door(c, &next);
                    break;
                }
                continue;
            }

            if (lab_is_tunnel(c, &grid))
            {
                place_closed_door(c, &grid);
                break;
            }
        }
    }

    /* Unlit labyrinths will have some good items */
    if (!lit)
        alloc_objects(p, c, SET_BOTH, TYP_GOOD, Rand_normal(3, 2), wpos->depth, ORIGIN_LABYRINTH);

    /* Hard (non-diggable) labyrinths will have some great items */
    if (!soft)
        alloc_objects(p, c, SET_BOTH, TYP_GREAT, Rand_normal(2, 1), wpos->depth, ORIGIN_LABYRINTH);

    /* Deallocate our lists */
    mem_free(sets);
    mem_free(walls);

    return c;
}


/*
 * Build a labyrinth level.
 *
 * p is the player
 * wpos is the position on the world map
 *
 * Note that if the function returns false, a level wasn't generated.
 * Labyrinths use the dungeon level's number to determine whether to generate
 * themselves (which means certain level numbers are more likely to generate
 * labyrinths than others).
 */
struct chunk *labyrinth_gen(struct player *p, struct worldpos *wpos, int min_height, int min_width)
{
    int i, k;
    struct chunk *c;

    /* Most labyrinths have wide corridors */
    bool wide = (TURN_BASED? false: magik(90));
    int hmax = (wide? z_info->dungeon_hgt / 2 - 2: z_info->dungeon_hgt - 3);
    int wmax = (wide? z_info->dungeon_wid / 2 - 2: z_info->dungeon_wid - 3);

    /*
     * Size of the actual labyrinth part must be odd.
     *
     * NOTE: these are not the actual dungeon size, but rather the size of the
     * area we're generating a labyrinth in (which doesn't count the enclosing
     * outer walls.
     */
    int h = 15 + randint0(wpos->depth / 10) * 2;
    int w = 51 + randint0(wpos->depth / 10) * 2;

    /* Most labyrinths are lit */
    bool lit = ((randint0(wpos->depth) < 25) || (randint0(2) < 1));

    /* Many labyrinths are known */
    bool known = (lit && (randint0(wpos->depth) < 25));

    /* Most labyrinths have soft (diggable) walls */
    bool soft = ((randint0(wpos->depth) < 35) || (randint0(3) < 2));

    /* Enforce minimum dimensions */
    h = MAX(h, min_height);
    w = MAX(w, min_width);

    /* Enforce maximum dimensions */
    h = MIN(h, hmax);
    w = MIN(w, wmax);

    /* Generate the actual labyrinth */
    c = labyrinth_chunk(p, wpos, h, w, lit, soft, wide);

    /* Hack -- allow wide corridors */
    if (wide)
    {
        h *= 2;
        w *= 2;
    }

    /* Tweak floors and walls */
    fill_level(c, false);
    fill_level(c, true);

    /* Place stairs near some walls */
    add_stairs(c, FEAT_MORE);
    add_stairs(c, FEAT_LESS);

    /* General amount of rubble, traps and monsters */
    k = MAX(MIN(wpos->depth / 3, 10), 2);

    /* Scale number of monsters items by labyrinth size */
    k = (3 * k * (h * w)) / (z_info->dungeon_hgt * z_info->dungeon_wid);

    /* Put some rubble in corridors */
    alloc_objects(p, c, SET_BOTH, TYP_RUBBLE, randint1(k), wpos->depth, 0);

    /* Place some traps in the dungeon */
    alloc_objects(p, c, SET_CORR, TYP_TRAP, randint1(k), wpos->depth, 0);

    /* Determine the character location */
    new_player_spot(c, p);

    /* Put some monsters in the dungeon */
    for (i = z_info->level_monster_min + randint1(8) + k; i > 0; i--)
        pick_and_place_distant_monster(p, c, 0, MON_ASLEEP);

    /* Put some objects/gold in the dungeon */
    alloc_objects(p, c, SET_BOTH, TYP_OBJECT, Rand_normal(k * 6, 2), wpos->depth,
        ORIGIN_LABYRINTH);
    alloc_objects(p, c, SET_BOTH, TYP_GOLD, Rand_normal(k * 3, 2), wpos->depth, ORIGIN_LABYRINTH);
    alloc_objects(p, c, SET_BOTH, TYP_GOOD, randint1(2), wpos->depth, ORIGIN_LABYRINTH);

    /* Notify if we want the player to see the maze layout */
    player_cave_clear(p, true);
    if (known) c->light_level = true;

    return c;
}


/* ---------------- CAVERNS ---------------------- */


/*
 * Initialize the dungeon array, with a random percentage of squares open.
 *
 * c is the current chunk
 * density is the percentage of floors we are aiming for
 */
static void init_cavern(struct chunk *c, int density)
{
    int h = c->height;
    int w = c->width;
    int size = h * w;
    int count = (size * density) / 100;

    /* Fill the entire chunk with rock */
    fill_rectangle(c, 0, 0, h - 1, w - 1, FEAT_GRANITE, SQUARE_WALL_SOLID);

    while (count > 0)
    {
        struct loc grid;

        loc_init(&grid, randint1(w - 2), randint1(h - 2));
        if (square_isrock(c, &grid))
        {
            square_set_feat(c, &grid, FEAT_FLOOR);
            count--;
        }
    }
}


/*
 * Return the number of walls (0-8) adjacent to this square.
 *
 * c is the current chunk
 * grid is the location
 */
static int count_adj_walls(struct chunk *c, struct loc *grid)
{
    int d;
    int count = 0;

    for (d = 0; d < 8; d++)
    {
        struct loc adj;

        loc_sum(&adj, grid, &ddgrid_ddd[d]);
        if (square_isfloor(c, &adj)) continue;
        count++;
    }

    return count;
}


/*
 * Run a single pass of the cellular automata rules (4,5) on the dungeon.
 *
 * c is the chunk being mutated
 */
static void mutate_cavern(struct chunk *c)
{
    struct loc begin, end;
    struct loc_iterator iter;
    int h = c->height;
    int w = c->width;
    int *temp = mem_zalloc(h * w * sizeof(int));

    loc_init(&begin, 1, 1);
    loc_init(&end, w - 1, h - 1);
    loc_iterator_first(&iter, &begin, &end);

    do
    {
        int count = count_adj_walls(c, &iter.cur);

        if (count > 5)
            temp[grid_to_i(&iter.cur, w)] = FEAT_GRANITE;
        else if (count < 4)
            temp[grid_to_i(&iter.cur, w)] = FEAT_FLOOR;
        else
            temp[grid_to_i(&iter.cur, w)] = square(c, &iter.cur)->feat;
    }
    while (loc_iterator_next_strict(&iter));

    loc_iterator_first(&iter, &begin, &end);

    do
    {
        if (temp[grid_to_i(&iter.cur, w)] == FEAT_GRANITE)
            set_marked_granite(c, &iter.cur, SQUARE_WALL_SOLID);
        else
            square_set_feat(c, &iter.cur, temp[grid_to_i(&iter.cur, w)]);
    }
    while (loc_iterator_next_strict(&iter));

    mem_free(temp);
}


/*
 * Fill an int[] with a single value.
 *
 * data is the array
 * value is what it's being filled with
 * size is the array length
 */
static void array_filler(int *data, int value, int size)
{
    int i;

    for (i = 0; i < size; i++) data[i] = value;
}


/*
 * Determine if we need to worry about coloring a point, or can ignore it.
 *
 * c is the current chunk
 * colors is the array of current point colors
 * grid is the location
 */
static int ignore_point(struct chunk *c, int *colors, struct loc *grid)
{
    int n = grid_to_i(grid, c->width);

    if (!square_in_bounds(c, grid)) return true;
    if (colors[n]) return true;
    if (square_ispassable(c, grid)) return false;
    if (square_isdoor(c, grid)) return false;
    return true;
}


/*
 * Color a particular point, and all adjacent points.
 *
 * c is the current chunk
 * colors is the array of current point colors
 * counts is the array of current color counts
 * grid is the location
 * color is the color we are coloring
 * diagonal controls whether we can progress diagonally
 */
static void build_color_point(struct chunk *c, int *colors, int *counts, struct loc *grid,
    int color, bool diagonal)
{
    int h = c->height;
    int w = c->width;
    int size = h * w;
    struct queue *queue = q_new(size);
    int *added = mem_zalloc(size * sizeof(int));

    array_filler(added, 0, size);

    q_push_int(queue, grid_to_i(grid, w));

    counts[color] = 0;

    while (q_len(queue) > 0)
    {
        int i;
        struct loc grid1;
        int n1 = q_pop_int(queue);

        i_to_grid(n1, w, &grid1);

        if (ignore_point(c, colors, &grid1)) continue;

        colors[n1] = color;
        counts[color]++;

        for (i = 0; i < (diagonal? 8: 4); i++)
        {
            struct loc grid2;
            int n2;

            loc_sum(&grid2, &grid1, &ddgrid_ddd[i]);
            n2 = grid_to_i(&grid2, w);
            if (ignore_point(c, colors, &grid2)) continue;
            if (added[n2]) continue;

            q_push_int(queue, n2);
            added[n2] = 1;
        }
    }

    q_free(queue);
    mem_free(added);
}


/*
 * Create a color for each "NESW contiguous" region of the dungeon.
 *
 * c is the current chunk
 * colors is the array of current point colors
 * counts is the array of current color counts
 * diagonal controls whether we can progress diagonally
 */
static void build_colors(struct chunk *c, int *colors, int *counts, bool diagonal)
{
    int color = 1;
    struct loc begin, end;
    struct loc_iterator iter;

    loc_init(&begin, 0, 0);
    loc_init(&end, c->width, c->height);
    loc_iterator_first(&iter, &begin, &end);

    do
    {
        if (ignore_point(c, colors, &iter.cur)) continue;
        build_color_point(c, colors, counts, &iter.cur, color, diagonal);
        color++;
    }
    while (loc_iterator_next_strict(&iter));
}


/*
 * Find and delete all small (<9 square) open regions.
 *
 * c is the current chunk
 * colors is the array of current point colors
 * counts is the array of current color counts
 */
static void clear_small_regions(struct chunk *c, int *colors, int *counts)
{
    int i;
    int h = c->height;
    int w = c->width;
    int size = h * w;
    int *deleted = mem_zalloc(size * sizeof(int));
    struct loc begin, end;
    struct loc_iterator iter;

    array_filler(deleted, 0, size);

    for (i = 0; i < size; i++)
    {
        if (counts[i] < 9)
        {
            deleted[i] = 1;
            counts[i] = 0;
        }
    }

    loc_init(&begin, 1, 1);
    loc_init(&end, c->width - 1, c->height - 1);
    loc_iterator_first(&iter, &begin, &end);

    do
    {
        i = grid_to_i(&iter.cur, w);

        if (!deleted[colors[i]]) continue;

        colors[i] = 0;
        if (!square_isperm(c, &iter.cur))
            set_marked_granite(c, &iter.cur, SQUARE_WALL_SOLID);
    }
    while (loc_iterator_next_strict(&iter));

    mem_free(deleted);
}


/*
 * Return the number of colors which have active cells.
 *
 * counts is the array of current color counts
 * size is the total area
 */
static int count_colors(int *counts, int size)
{
    int i;
    int num = 0;

    for (i = 0; i < size; i++) if (counts[i] > 0) num++;
    return num;
}


/*
 * Return the first color which has one or more active cells.
 *
 * counts is the array of current color counts
 * size is the total area
 */
static int first_color(int *counts, int size)
{
    int i;

    for (i = 0; i < size; i++) if (counts[i] > 0) return i;
    return -1;
}


/*
 * Find all cells of 'fromcolor' and repaint them to 'tocolor'.
 *
 * colors is the array of current point colors
 * counts is the array of current color counts
 * from is the color to change
 * to is the color to change to
 * size is the total area
 */
static void fix_colors(int *colors, int *counts, int from, int to, int size)
{
    int i;

    for (i = 0; i < size; i++) if (colors[i] == from) colors[i] = to;
    counts[to] += counts[from];
    counts[from] = 0;
}


/*
 * Create a tunnel connecting a region to one of its nearest neighbors.
 *
 * c is the current chunk
 * colors is the array of current point colors
 * counts is the array of current color counts
 * color is the color of the region we want to connect
 * new_color is the color of the region we want to connect to (if used)
 */
static void join_region(struct chunk *c, int *colors, int *counts, int color, int new_color)
{
    int i;
    int h = c->height;
    int w = c->width;
    int size = h * w;

    /* Allocate a processing queue */
    struct queue *queue = q_new(size);

    /* Allocate an array to keep track of handled squares, and which square we reached them from */
    int *previous = mem_zalloc(size * sizeof(int));

    array_filler(previous, -1, size);

    /* Push all squares of the given color onto the queue */
    for (i = 0; i < size; i++)
    {
        if (colors[i] == color)
        {
            q_push_int(queue, i);
            previous[i] = i;
        }
    }

    /* Process all squares into the queue */
    while (q_len(queue) > 0)
    {
        /* Get the current square and its color */
        int n1 = q_pop_int(queue);
        int color2 = colors[n1];

        /* If we're not looking for a specific color, any new one will do */
        if ((new_color == -1) && color2 && (color2 != color))
            new_color = color2;

        /* See if we've reached a square with a new color */
        if (color2 == new_color)
        {
            /* Step backward through the path, turning stone to tunnel */
            while (colors[n1] != color)
            {
                struct loc grid, gridp;

                i_to_grid(n1, w, &grid);

                colors[n1] = color;
                if (!square_isperm(c, &grid) && !square_isvault(c, &grid))
                    square_set_feat(c, &grid, FEAT_FLOOR);
                n1 = previous[n1];

                /* Hack -- create broad corridors */
                i_to_grid(n1, w, &gridp);
                if (gridp.y != grid.y) grid.x++;
                else grid.y++;
                if (square_in_bounds_fully(c, &grid) && !square_isperm(c, &grid) &&
                    !square_isvault(c, &grid))
                {
                    square_set_feat(c, &grid, FEAT_FLOOR);
                }
            }

            /* Update the color mapping to combine the two colors */
            fix_colors(colors, counts, color2, color, size);

            /* We're done now */
            break;
        }

        /* If we haven't reached a new color, add all the unprocessed adjacent squares to our queue */
        for (i = 0; i < 4; i++)
        {
            int n2;
            struct loc grid0, grid;

            i_to_grid(n1, w, &grid0);

            /* Move to the adjacent square */
            loc_sum(&grid, &grid0, &ddgrid_ddd[i]);

            /* Make sure we stay inside the boundaries */
            if (!square_in_bounds(c, &grid)) continue;

            /* If the cell hasn't already been processed, add it to the queue */
            n2 = grid_to_i(&grid, w);
            if (previous[n2] >= 0) continue;
            q_push_int(queue, n2);
            previous[n2] = n1;
        }
    }

    /* Free the memory we've allocated */
    q_free(queue);
    mem_free(previous);
}


/*
 * Start connecting regions, stopping when the cave is entirely connected.
 *
 * c is the current chunk
 * colors is the array of current point colors
 * counts is the array of current color counts
 */
static void join_regions(struct chunk *c, int *colors, int *counts)
{
    int h = c->height;
    int w = c->width;
    int size = h * w;
    int num = count_colors(counts, size);

    /*
     * While we have multiple colors (i.e. disconnected regions), join one of
     * the regions to another one.
     */
    while (num > 1)
    {
        int color = first_color(counts, size);

        join_region(c, colors, counts, color, -1);
        num--;
    }
}


/*
 * Make sure that all the regions of the dungeon are connected.
 *
 * c is the current chunk
 *
 * This function colors each connected region of the dungeon, then uses that
 * information to join them into one connected region.
 */
static void ensure_connectedness(struct chunk *c)
{
    int size = c->height * c->width;
    int *colors = mem_zalloc(size * sizeof(int));
    int *counts = mem_zalloc(size * sizeof(int));

    build_colors(c, colors, counts, true);
    join_regions(c, colors, counts);

    mem_free(colors);
    mem_free(counts);
}


#define MAX_CAVERN_TRIES 10


/*
 * The cavern generator's main function.
 *
 * p is the player
 * wpos is the position on the world map
 * (h, w) the chunk's dimensions
 *
 * Returns a pointer to the generated chunk.
 */
static struct chunk *cavern_chunk(struct player *p, struct worldpos *wpos, int h, int w)
{
    int i;
    int size = h * w;
    int limit = size / 13;
    int density = rand_range(25, 40);
    int times = rand_range(3, 6);
    int *colors = mem_zalloc(size * sizeof(int));
    int *counts = mem_zalloc(size * sizeof(int));
    int tries;
    struct chunk *c = cave_new(h, w);

    memcpy(&c->wpos, wpos, sizeof(struct worldpos));
    player_cave_new(p, h, w);

    /* Start trying to build caverns */
    for (tries = 0; tries < MAX_CAVERN_TRIES; tries++)
    {
        /* Build a random cavern and mutate it a number of times */
        init_cavern(c, density);
        for (i = 0; i < times; i++) mutate_cavern(c);

        /* If there are enough open squares then we're done */
        if (c->feat_count[FEAT_FLOOR] >= limit) break;
    }

    /* If we couldn't make a big enough cavern then fail */
    if (tries == MAX_CAVERN_TRIES)
    {
        mem_free(colors);
        mem_free(counts);
        cave_free(c);
        return NULL;
    }

    build_colors(c, colors, counts, false);
    clear_small_regions(c, colors, counts);
    join_regions(c, colors, counts);

    mem_free(colors);
    mem_free(counts);

    return c;
}


/*
 * Make a cavern level.
 *
 * p is the player
 * wpos is the position on the world map
 */
struct chunk *cavern_gen(struct player *p, struct worldpos *wpos, int min_height, int min_width)
{
    int i, k;
    int h = rand_range(z_info->dungeon_hgt / 2, (z_info->dungeon_hgt * 3) / 4);
    int w = rand_range(z_info->dungeon_wid / 2, (z_info->dungeon_wid * 3) / 4);
    struct chunk *c;

    /* If we're too shallow then don't do it */
    if (wpos->depth < 15) return NULL;

    /* Enforce minimum dimensions */
    h = MAX(h, min_height);
    w = MAX(w, min_width);

    /* Try to build the cavern, fail gracefully */
    c = cavern_chunk(p, wpos, h, w);
    if (!c) return NULL;

    /* Surround the level with perma-rock */
    draw_rectangle(c, 0, 0, h - 1, w - 1, FEAT_PERM, SQUARE_NONE);

    /* Tweak floors and walls */
    fill_level(c, false);
    fill_level(c, true);

    /* Place stairs near some walls */
    add_stairs(c, FEAT_MORE);
    add_stairs(c, FEAT_LESS);

    /* General amount of rubble, traps and monsters */
    k = MAX(MIN(wpos->depth / 3, 10), 2);

    /* Scale number of monsters items by cavern size */
    k = MAX((4 * k * (h * w)) / (z_info->dungeon_hgt * z_info->dungeon_wid), 6);

    /* Put some rubble in corridors */
    alloc_objects(p, c, SET_BOTH, TYP_RUBBLE, randint1(k), wpos->depth, 0);

    /* Place some traps in the dungeon */
    alloc_objects(p, c, SET_CORR, TYP_TRAP, randint1(k), wpos->depth, 0);

    /* Determine the character location */
    new_player_spot(c, p);

    /* Put some monsters in the dungeon */
    for (i = randint1(8) + k; i > 0; i--)
        pick_and_place_distant_monster(p, c, 0, MON_ASLEEP);

    /* Put some objects/gold in the dungeon */
    alloc_objects(p, c, SET_BOTH, TYP_OBJECT, Rand_normal(k, 2), wpos->depth + 5, ORIGIN_CAVERN);
    alloc_objects(p, c, SET_BOTH, TYP_GOLD, Rand_normal(k / 2, 2), wpos->depth, ORIGIN_CAVERN);
    alloc_objects(p, c, SET_BOTH, TYP_GOOD, randint0(k / 4), wpos->depth, ORIGIN_CAVERN);

    /* Clear the flags for each cave grid */
    player_cave_clear(p, true);

    return c;
}


/* ------------------ TOWN ---------------- */


/*
 * Builds a store at a given pseudo-location
 *
 * c is the current chunk
 * n is which shop it is
 * grid is the grid of this store in the store layout
 */
static void build_store(struct chunk *c, int n, struct loc *grid)
{
    int feat;
    struct loc door;
    struct store *s = &stores[n];

    /* Hack -- make tavern as large as possible */
    int rad = ((s->type == STORE_TAVERN)? 3: 1);

    /* Determine door location */
    door.y = rand_range(grid->y - rad, grid->y + rad);
    door.x = (((door.y == grid->y - rad) || (door.y == grid->y + rad))?
        rand_range(grid->x - rad, grid->x + rad): (grid->x - rad + rad * 2 * randint0(2)));

    /* Build an invulnerable rectangular building */
    fill_rectangle(c, grid->y - rad, grid->x - rad, grid->y + rad, grid->x + rad, FEAT_PERM,
        SQUARE_NONE);

    /* Hack -- make tavern empty */
    if (s->type == STORE_TAVERN)
    {
        struct loc begin, end;
        struct loc_iterator iter;

        loc_init(&begin, grid->x - 2, grid->y - 2);
        loc_init(&end, grid->x + 2, grid->y + 2);
        loc_iterator_first(&iter, &begin, &end);

        do
        {
            /* Create the tavern, make it PvP-safe */
            square_add_safe(c, &iter.cur);

            /* Declare this to be a room */
            sqinfo_on(square(c, &iter.cur)->info, SQUARE_VAULT);
            sqinfo_on(square(c, &iter.cur)->info, SQUARE_NOTRASH);
            sqinfo_on(square(c, &iter.cur)->info, SQUARE_ROOM);
        }
        while (loc_iterator_next(&iter));

        /* Hack -- have everyone start in the tavern */
        square_set_join_down(c, grid);
    }

    /* Clear previous contents, add a store door */
    for (feat = 0; feat < z_info->f_max; feat++)
    {
        if (feat_is_shop(feat) && (f_info[feat].shopnum == n + 1))
            square_set_feat(c, &door, feat);
    }
}


/*
 * Locate an empty square in a given rectangle.
 *
 * c current chunk
 * grid found grid
 * top_left top left grid of rectangle
 * bottom_right bottom right grid of rectangle
 */
static bool find_empty_range(struct chunk *c, struct loc *grid, struct loc *top_left,
    struct loc *bottom_right)
{
    return cave_find_in_range(c, grid, top_left, bottom_right, square_isempty);
}


/*
 * Generate the town for the first time, and place the player
 *
 * p is the player
 * c is the current chunk
 */
static void town_gen_layout(struct player *p, struct chunk *c)
{
    int n;
    int num_lava, num_rubble;
    struct loc begin, end, grid, top_left, bottom_right;
    struct loc_iterator iter;

    /* Boundary */
    int feat_outer = (((cfg_diving_mode > 1) || dynamic_town(&c->wpos))? FEAT_PERM:
        FEAT_PERM_CLEAR);

    /* Town dimensions (PWMAngband: make town twice as big as Angband) */
    int town_hgt = 44;
    int town_wid = 132;

    /* Town limits */
    int y1 = (c->height - town_hgt) / 2;
    int x1 = (c->width - town_wid) / 2;
    int y2 = (c->height + town_hgt) / 2;
    int x2 = (c->width + town_wid) / 2;

    u32b tmp_seed = Rand_value;
    bool rand_old = Rand_quick;

    /* Hack -- use the "simple" RNG */
    Rand_quick = true;

    /* Hack -- induce consistant town */
    Rand_value = seed_wild + world_index(&c->wpos) * 600 + c->wpos.depth * 37;

    loc_init(&top_left, x1, y1);
    loc_init(&bottom_right, x2, y2);

    num_lava = 3 + randint0(3);
    num_rubble = 3 + randint0(3);

    /* Create boundary */
    draw_rectangle(c, 0, 0, c->height - 1, c->width - 1, feat_outer, SQUARE_NONE);

    /* Initialize to ROCK for build_streamer precondition */
    fill_rectangle(c, 1, 1, c->height - 2, c->width - 2, FEAT_GRANITE, SQUARE_WALL_SOLID);

    /* Make some lava streamers */
    for (n = 0; n < 3 + num_lava; n++) build_streamer(c, FEAT_LAVA, 0);

    /* Make a town-sized starburst room. */
    generate_starburst_room(c, y1, x1, y2, x2, false, FEAT_FLOOR, false);

    loc_init(&begin, 1, 1);
    loc_init(&end, c->width - 1, c->height - 1);
    loc_iterator_first(&iter, &begin, &end);

    /* Turn off room illumination flag */
    do
    {
        if (square_isfloor(c, &iter.cur))
            sqinfo_off(square(c, &iter.cur)->info, SQUARE_ROOM);
        else if (!square_isperm(c, &iter.cur) && !square_isfiery(c, &iter.cur))
            square_set_feat(c, &iter.cur, FEAT_PERM_STATIC);
    }
    while (loc_iterator_next_strict(&iter));

    /* Place stores */
    for (n = 0; n < store_max; n++)
    {
        int tries = 1000;
        struct loc store;
        struct store *s = &stores[n];

        /*
         * PWMAngband: tavern has radius 3, other stores have radius 1; then we leave at least
         * a space of 3 squares around stores.
         */
        int rad = ((s->type == STORE_TAVERN)? 6: 4);

        /* Skip player store */
        if (s->type == STORE_PLAYER) continue;

        /* Find an empty place */
        while (tries)
        {
            bool found_non_floor = false;

            find_empty_range(c, &store, &top_left, &bottom_right);

            loc_init(&begin, store.x - rad, store.y - rad);
            loc_init(&end, store.x + rad, store.y + rad);
            loc_iterator_first(&iter, &begin, &end);

            do
            {
                if (!square_isfloor(c, &iter.cur)) found_non_floor = true;
            }
            while (loc_iterator_next(&iter));

            if (!found_non_floor) break;
            tries--;
        }

        /* Build a store */
        my_assert(tries);
        build_store(c, n, &store);
    }

    /* Place a few piles of rubble */
    for (n = 0; n < num_rubble; n++)
    {
        int tries = 1000;

        /* Find an empty place */
        while (tries)
        {
            bool found_non_floor = false;

            find_empty_range(c, &grid, &top_left, &bottom_right);

            loc_init(&begin, grid.x - 2, grid.y - 2);
            loc_init(&end, grid.x + 2, grid.y + 2);
            loc_iterator_first(&iter, &begin, &end);

            do
            {
                if (!square_isfloor(c, &iter.cur)) found_non_floor = true;
            }
            while (loc_iterator_next(&iter));

            if (!found_non_floor) break;
            tries--;
        }

        /* Place rubble at random */
        my_assert(tries);

        loc_init(&begin, grid.x - 1, grid.y - 1);
        loc_init(&end, grid.x + 1, grid.y + 1);
        loc_iterator_first(&iter, &begin, &end);

        do
        {
            if (one_in_(1 + ABS(grid.x - iter.cur.x) + ABS(grid.y - iter.cur.y)))
                square_set_feat(c, &iter.cur, FEAT_PASS_RUBBLE);
        }
        while (loc_iterator_next(&iter));
    }

    /* Place the stairs in the north wall */
    loc_init(&grid, rand_spread(c->width / 2, town_wid / 3), 2);
    while (square_isperm(c, &grid) || square_isfiery(c, &grid)) grid.y++;
    grid.y--;

    /* Place a staircase */
    square_set_downstairs(c, &grid);

    /* Hack -- the players start on the stairs while recalling */
    square_set_join_rand(c, &grid);

    /* PWMAngband: dynamically generated towns also get an up staircase */
    if (dynamic_town(&c->wpos))
    {
        /* Place the stairs in the south wall */
        loc_init(&grid, rand_spread(c->width / 2, town_wid / 3), c->height - 3);
        while (square_isperm(c, &grid) || square_isfiery(c, &grid)) grid.y--;
        grid.y++;

        /* Place a staircase */
        square_set_upstairs(c, &grid);

        /* Determine the character location */
        new_player_spot(c, p);
    }

    /* PWMAngband: cover the base town in dirt, and make some exits */
    else
    {
        int pos;

        loc_init(&begin, 1, 1);
        loc_init(&end, c->width - 1, c->height - 1);
        loc_iterator_first(&iter, &begin, &end);

        /* Cover the town in dirt */
        do
        {
            if (square_isfloor(c, &iter.cur)) square_add_dirt(c, &iter.cur);
        }
        while (loc_iterator_next_strict(&iter));

        /* Make some exits (wilderness) */
        if (cfg_diving_mode < 2)
        {
            /* Place a vertical opening in the south wall */
            pos = rand_spread(c->width / 2, town_wid / 3);
            for (grid.x = pos - 2; grid.x <= pos + 2; grid.x++)
            {
                grid.y = c->height - 3;
                while (square_isperm(c, &grid) || square_isfiery(c, &grid))
                {
                    square_add_dirt(c, &grid);
                    grid.y--;
                }
            }

            /* Place horizontal openings in the west and east walls */
            pos = rand_spread(c->height / 2, town_hgt / 3);
            for (grid.y = pos - 2; grid.y <= pos + 2; grid.y++)
            {
                grid.x = 2;
                while (square_isperm(c, &grid) || square_isfiery(c, &grid))
                {
                    square_add_dirt(c, &grid);
                    grid.x++;
                }
            }

            pos = rand_spread(c->height / 2, town_hgt / 3);
            for (grid.y = pos - 2; grid.y <= pos + 2; grid.y++)
            {
                grid.x = c->width - 3;
                while (square_isperm(c, &grid) || square_isfiery(c, &grid))
                {
                    square_add_dirt(c, &grid);
                    grid.x--;
                }
            }

            /* Surround with dirt (make irregular borders) */
            for (grid.x = 1; grid.x <= c->width - 2; grid.x++)
            {
                n = randint1(3);
                for (grid.y = 1; grid.y <= n; grid.y++) square_add_dirt(c, &grid);
                n = randint1(3);
                for (grid.y = c->height - 1 - n; grid.y <= c->height - 2; grid.y++)
                    square_add_dirt(c, &grid);
            }
            for (grid.y = 1; grid.y <= c->height - 2; grid.y++)
            {
                n = randint1(3);
                for (grid.x = 1; grid.x <= n; grid.x++) square_add_dirt(c, &grid);
                n = randint1(3);
                for (grid.x = c->width - 1 - n; grid.x <= c->width - 2; grid.x++)
                    square_add_dirt(c, &grid);
            }
        }
    }

    /* Hack -- use the "complex" RNG */
    Rand_value = tmp_seed;
    Rand_quick = rand_old;
}


/*
 * Town logic flow for generation of new town.
 *
 * p is the player
 * wpos is the position on the world map
 *
 * Returns a pointer to the generated chunk.
 *
 * We start with a fully wiped cave of normal floors. This function does NOT do
 * anything about the owners of the stores, nor the contents thereof. It only
 * handles the physical layout.
 *
 * PWMAngband: the layout for Angband's new town is also used to dynamically generate towns
 * for ironman servers at 1000ft, 2000ft, 3000ft and 4000ft.
 */
struct chunk *town_gen(struct player *p, struct worldpos *wpos, int min_height, int min_width)
{
    int i, residents;
    bool daytime;

    /* Make a new chunk */
    struct chunk *c = cave_new(z_info->dungeon_hgt, z_info->dungeon_wid);

    memcpy(&c->wpos, wpos, sizeof(struct worldpos));

    /* Base town */
    if (wpos->depth == 0)
    {
        residents = (is_daytime()? z_info->town_monsters_day: z_info->town_monsters_night);
        daytime = is_daytime();
    }

    /* Dynamically generate town */
    else
    {
        residents = 0;
        daytime = true;
    }

    player_cave_new(p, z_info->dungeon_hgt, z_info->dungeon_wid);

    /* Build stuff */
    town_gen_layout(p, c);

    /* Apply illumination */
    player_cave_clear(p, true);
    cave_illuminate(p, c, daytime);

    /* Make some residents */
    for (i = 0; i < residents; i++)
        pick_and_place_distant_monster(p, c, 0, MON_ASLEEP);

    return c;
}


/* ------------------ MODIFIED ---------------- */


/*
 * The main modified generation algorithm
 *
 * p is the player
 * wpos is the position on the world map
 * (height, width) are the dimensions of the chunk
 */
static struct chunk *modified_chunk(struct player *p, struct worldpos *wpos, int height, int width)
{
    int i;
    struct loc grid;
    int by = 0, bx = 0, key, rarity;
    int num_floors;
    int num_rooms = dun->profile->n_room_profiles;
    int dun_unusual = dun->profile->dun_unusual;

    /* Make the cave */
    struct chunk *c = cave_new(height, width);

    memcpy(&c->wpos, wpos, sizeof(struct worldpos));
    player_cave_new(p, height, width);

    /* Set the intended number of floor grids based on cave floor area */
    num_floors = c->height * c->width / 7;

    /* Fill cave area with basic granite */
    fill_rectangle(c, 0, 0, c->height - 1, c->width - 1, FEAT_GRANITE, SQUARE_NONE);

    /* Generate permanent walls around the generated area (temporarily!) */
    draw_rectangle(c, 0, 0, c->height - 1, c->width - 1, FEAT_PERM, SQUARE_NONE);

    /* Actual maximum number of blocks on this level */
    dun->row_blocks = c->height / dun->block_hgt;
    dun->col_blocks = c->width / dun->block_wid;

    /* Initialize the room table */
    dun->room_map = mem_zalloc(dun->row_blocks * sizeof(bool*));
    for (i = 0; i < dun->row_blocks; i++)
        dun->room_map[i] = mem_zalloc(dun->col_blocks * sizeof(bool));

    /* No rooms yet, pits or otherwise. */
    dun->pit_num = 0;
    dun->cent_n = 0;

    /* Build rooms until we have enough floor grids and at least two rooms */
    while ((c->feat_count[FEAT_FLOOR] < num_floors) || (dun->cent_n < 2))
    {
        /* Roll for random key (to be compared against a profile's cutoff) */
        key = randint0(100);

        /*
         * We generate a rarity number to figure out how exotic to make the
         * room. This number has a depth/dun_unusual chance of being > 0,
         * a depth^2/dun_unusual^2 chance of being > 1, up to dun->profile->max_rarity.
         */
        i = 0;
        rarity = 0;
        while ((i == rarity) && (i < dun->profile->max_rarity))
        {
            if (randint0(dun_unusual) < 50 + wpos->depth / 2) rarity++;
            i++;
        }

        /*
         * Once we have a key and a rarity, we iterate through out list of
         * room profiles looking for a match (whose cutoff > key and whose
         * rarity > this rarity). We try building the room, and if it works
         * then we are done with this iteration. We keep going until we find
         * a room that we can build successfully or we exhaust the profiles.
         */
        for (i = 0; i < num_rooms; i++)
        {
            struct room_profile profile = dun->profile->room_profiles[i];

            if (profile.rarity > rarity) continue;
            if (profile.cutoff <= key) continue;

            if (room_build(p, c, by, bx, profile, true)) break;
        }
    }

    for (i = 0; i < dun->row_blocks; i++)
        mem_free(dun->room_map[i]);
    mem_free(dun->room_map);

    /* Hack -- scramble the room order */
    for (i = 0; i < dun->cent_n; i++)
    {
        int pick1 = randint0(dun->cent_n);
        int pick2 = randint0(dun->cent_n);
        struct loc tmp;

        loc_copy(&tmp, &dun->cent[pick1]);
        loc_copy(&dun->cent[pick1], &dun->cent[pick2]);
        loc_copy(&dun->cent[pick2], &tmp);
    }

    /* Start with no tunnel doors */
    dun->door_n = 0;

    /* Hack -- connect the first room to the last room */
    loc_copy(&grid, &dun->cent[dun->cent_n - 1]);

    /* Connect all the rooms together */
    for (i = 0; i < dun->cent_n; i++)
    {
        /* Connect the room to the previous room */
        build_tunnel(c, &dun->cent[i], &grid);

        /* Remember the "previous" room */
        loc_copy(&grid, &dun->cent[i]);
    }

    /* Place intersection doors */
    for (i = 0; i < dun->door_n; i++)
    {
        /* Try placing doors */
        next_grid(&grid, &dun->door[i], DIR_W);
        try_door(c, &grid);
        next_grid(&grid, &dun->door[i], DIR_E);
        try_door(c, &grid);
        next_grid(&grid, &dun->door[i], DIR_N);
        try_door(c, &grid);
        next_grid(&grid, &dun->door[i], DIR_S);
        try_door(c, &grid);
    }

    ensure_connectedness(c);

    /* Turn the outer permanent walls back to granite */
    draw_rectangle(c, 0, 0, c->height - 1, c->width - 1, FEAT_GRANITE, SQUARE_NONE);

    return c;
}


/*
 * Generate a new dungeon level
 *
 * p is the player
 * wpos is the position on the world map
 *
 * This is sample code to illustrate some of the new dungeon generation
 * methods; I think it actually produces quite nice levels. New stuff:
 *
 * - different sized levels
 * - independence from block size: the block size can be set to any number
 *   from 1 (no blocks) to about 15; beyond that it struggles to generate
 *   enough floor space
 * - the find_space function, called from the room builder functions, allows
 *   the room to find space for itself rather than the generation algorithm
 *   allocating it; this helps because the room knows better what size it is
 * - a count is now kept of grids of the various terrains, allowing dungeon
 *   generation to terminate when enough floor is generated
 * - there are three new room types - huge rooms, rooms of chambers
 *   and interesting rooms - as well as many new vaults
 * - there is the ability to place specific monsters and objects in vaults and
 *   interesting rooms, as well as to make general monster restrictions in
 *   areas or the whole dungeon
 */
struct chunk *modified_gen(struct player *p, struct worldpos *wpos, int min_height, int min_width)
{
    int i, k;
    int size_percent, y_size, x_size;
    struct chunk *c;

    /* Scale the level */
    i = randint1(10) + wpos->depth / 24;
    if (is_quest(wpos->depth)) size_percent = 100;
    else if (i < 2) size_percent = 75;
    else if (i < 3) size_percent = 80;
    else if (i < 4) size_percent = 85;
    else if (i < 5) size_percent = 90;
    else if (i < 6) size_percent = 95;
    else size_percent = 100;
    y_size = z_info->dungeon_hgt * (size_percent - 5 + randint0(10)) / 100;
    x_size = z_info->dungeon_wid * (size_percent - 5 + randint0(10)) / 100;

    /* Enforce minimum dimensions */
    y_size = MAX(y_size, min_height);
    x_size = MAX(x_size, min_width);

    /* Set the block height and width */
    dun->block_hgt = dun->profile->block_size;
    dun->block_wid = dun->profile->block_size;

    c = modified_chunk(p, wpos, MIN(z_info->dungeon_hgt, y_size), MIN(z_info->dungeon_wid, x_size));

    /* Generate permanent walls around the edge of the generated area */
    draw_rectangle(c, 0, 0, c->height - 1, c->width - 1, FEAT_PERM, SQUARE_NONE);

    /* Add some magma streamers */
    for (i = 0; i < dun->profile->str.mag; i++)
        build_streamer(c, FEAT_MAGMA, dun->profile->str.mc);

    /* Add some quartz streamers */
    for (i = 0; i < dun->profile->str.qua; i++)
        build_streamer(c, FEAT_QUARTZ, dun->profile->str.qc);

    /* Add some streamers */
    add_streamer(c, FEAT_LAVA, DF_LAVA_RIVER);
    add_streamer(c, FEAT_WATER, DF_WATER_RIVER);
    add_streamer(c, FEAT_SANDWALL, DF_SAND_VEIN);

    /* Tweak floors and walls */
    fill_level(c, false);
    fill_level(c, true);

    /* Place stairs near some walls */
    add_stairs(c, FEAT_MORE);
    add_stairs(c, FEAT_LESS);

    /* Remove holes in corridors that were not used for stair placement */
    remove_unused_holes(c);

    /* General amount of rubble, traps and monsters */
    k = MAX(MIN(wpos->depth / 3, 10), 2);

    /* Put some rubble in corridors */
    alloc_objects(p, c, SET_CORR, TYP_RUBBLE, randint1(k), wpos->depth, 0);

    /* Place some traps in the dungeon, reduce frequency by factor of 5 */
    alloc_objects(p, c, SET_CORR, TYP_TRAP, randint1(k) / 5, wpos->depth, 0);

    /* Place some fountains in rooms */
    alloc_objects(p, c, SET_ROOM, TYP_FOUNTAIN, randint0(1 + k / 2), wpos->depth, 0);

    /* Determine the character location */
    new_player_spot(c, p);

    /* Pick a base number of monsters */
    i = z_info->level_monster_min + randint1(8) + k;

    /* Put some monsters in the dungeon */
    for (; i > 0; i--)
        pick_and_place_distant_monster(p, c, 0, MON_ASLEEP);

    /* Put some objects in rooms */
    alloc_objects(p, c, SET_ROOM, TYP_OBJECT, Rand_normal(z_info->room_item_av, 3), wpos->depth,
        ORIGIN_FLOOR);

    /* Put some objects/gold in the dungeon */
    alloc_objects(p, c, SET_BOTH, TYP_OBJECT, Rand_normal(z_info->both_item_av, 3), wpos->depth,
        ORIGIN_FLOOR);
    alloc_objects(p, c, SET_BOTH, TYP_GOLD, Rand_normal(z_info->both_gold_av, 3), wpos->depth,
        ORIGIN_FLOOR);

    /* Apply illumination */
    player_cave_clear(p, true);
    cave_illuminate(p, c, true);

    return c;
}


/* ------------------ MORIA ---------------- */


/*
 * The main moria generation algorithm
 *
 * p is the player
 * wpos is the position on the world map
 * (height, width) are the dimensions of the chunk
 */
static struct chunk *moria_chunk(struct player *p, struct worldpos *wpos, int height, int width)
{
    int i;
    struct loc grid;
    int by = 0, bx = 0, key, rarity;
    int num_floors;
    int num_rooms = dun->profile->n_room_profiles;
    int dun_unusual = dun->profile->dun_unusual;

    /* Make the cave */
    struct chunk *c = cave_new(height, width);

    memcpy(&c->wpos, wpos, sizeof(struct worldpos));
    player_cave_new(p, height, width);

    /* Set the intended number of floor grids based on cave floor area */
    num_floors = c->height * c->width / 7;

    /* Fill cave area with basic granite */
    fill_rectangle(c, 0, 0, c->height - 1, c->width - 1, FEAT_GRANITE, SQUARE_NONE);

    /* Generate permanent walls around the generated area (temporarily!) */
    draw_rectangle(c, 0, 0, c->height - 1, c->width - 1, FEAT_PERM, SQUARE_NONE);

    /* Actual maximum number of blocks on this level */
    dun->row_blocks = c->height / dun->block_hgt;
    dun->col_blocks = c->width / dun->block_wid;

    /* Initialize the room table */
    dun->room_map = mem_zalloc(dun->row_blocks * sizeof(bool*));
    for (i = 0; i < dun->row_blocks; i++)
        dun->room_map[i] = mem_zalloc(dun->col_blocks * sizeof(bool));

    /* No rooms yet, pits or otherwise. */
    dun->pit_num = 0;
    dun->cent_n = 0;

    /* Build rooms until we have enough floor grids */
    while (c->feat_count[FEAT_FLOOR] < num_floors)
    {
        /* Roll for random key (to be compared against a profile's cutoff) */
        key = randint0(100);

        /*
         * We generate a rarity number to figure out how exotic to make the
         * room. This number has a depth/dun_unusual chance of being > 0,
         * a depth^2/dun_unusual^2 chance of being > 1, up to dun->profile->max_rarity.
         */
        i = 0;
        rarity = 0;
        while ((i == rarity) && (i < dun->profile->max_rarity))
        {
            if (randint0(dun_unusual) < 50 + wpos->depth / 2) rarity++;
            i++;
        }

        /*
         * Once we have a key and a rarity, we iterate through out list of
         * room profiles looking for a match (whose cutoff > key and whose
         * rarity > this rarity). We try building the room, and if it works
         * then we are done with this iteration. We keep going until we find
         * a room that we can build successfully or we exhaust the profiles.
         */
        for (i = 0; i < num_rooms; i++)
        {
            struct room_profile profile = dun->profile->room_profiles[i];

            if (profile.rarity > rarity) continue;
            if (profile.cutoff <= key) continue;

            if (room_build(p, c, by, bx, profile, true)) break;
        }
    }

    for (i = 0; i < dun->row_blocks; i++)
        mem_free(dun->room_map[i]);
    mem_free(dun->room_map);

    /* Hack -- scramble the room order */
    for (i = 0; i < dun->cent_n; i++)
    {
        int pick1 = randint0(dun->cent_n);
        int pick2 = randint0(dun->cent_n);
        struct loc tmp;

        loc_copy(&tmp, &dun->cent[pick1]);
        loc_copy(&dun->cent[pick1], &dun->cent[pick2]);
        loc_copy(&dun->cent[pick2], &tmp);
    }

    /* Start with no tunnel doors */
    dun->door_n = 0;

    /* Hack -- connect the first room to the last room */
    loc_copy(&grid, &dun->cent[dun->cent_n - 1]);

    /* Connect all the rooms together */
    for (i = 0; i < dun->cent_n; i++)
    {
        /* Connect the room to the previous room */
        build_tunnel(c, &dun->cent[i], &grid);

        /* Remember the "previous" room */
        loc_copy(&grid, &dun->cent[i]);
    }

    /* Place intersection doors */
    for (i = 0; i < dun->door_n; i++)
    {
        /* Try placing doors */
        next_grid(&grid, &dun->door[i], DIR_W);
        try_door(c, &grid);
        next_grid(&grid, &dun->door[i], DIR_E);
        try_door(c, &grid);
        next_grid(&grid, &dun->door[i], DIR_N);
        try_door(c, &grid);
        next_grid(&grid, &dun->door[i], DIR_S);
        try_door(c, &grid);
    }

    ensure_connectedness(c);

    /* Turn the outer permanent walls back to granite */
    draw_rectangle(c, 0, 0, c->height - 1, c->width - 1, FEAT_GRANITE, SQUARE_NONE);

    return c;
}


/*
 * Generate a new dungeon level
 *
 * p is the player
 * wpos is the position on the world map
 *
 * This produces Oangband-style moria levels.
 *
 * Most rooms on these levels are large, ragged-edged and roughly oval-shaped.
 *
 * Monsters are mostly "Moria dwellers" - orcs, ogres, trolls and giants.
 *
 * Apart from the room and monster changes, generation is similar to modified
 * levels. A good way of selecting these instead of modified (similar to
 * labyrinth levels are selected) would be
 *    if ((c->depth >= 10) && (c->depth < 40) && one_in_(40))
 */
struct chunk *moria_gen(struct player *p, struct worldpos *wpos, int min_height, int min_width)
{
    int i, k;
    int size_percent, y_size, x_size;
    struct chunk *c;

    /* Scale the level */
    i = randint1(10) + wpos->depth / 24;
    if (is_quest(wpos->depth)) size_percent = 100;
    else if (i < 2) size_percent = 75;
    else if (i < 3) size_percent = 80;
    else if (i < 4) size_percent = 85;
    else if (i < 5) size_percent = 90;
    else if (i < 6) size_percent = 95;
    else size_percent = 100;
    y_size = z_info->dungeon_hgt * (size_percent - 5 + randint0(10)) / 100;
    x_size = z_info->dungeon_wid * (size_percent - 5 + randint0(10)) / 100;

    /* Enforce minimum dimensions */
    y_size = MAX(y_size, min_height);
    x_size = MAX(x_size, min_width);

    /* Set the block height and width */
    dun->block_hgt = dun->profile->block_size;
    dun->block_wid = dun->profile->block_size;

    c = moria_chunk(p, wpos, MIN(z_info->dungeon_hgt, y_size), MIN(z_info->dungeon_wid, x_size));

    /* Generate permanent walls around the edge of the generated area */
    draw_rectangle(c, 0, 0, c->height - 1, c->width - 1, FEAT_PERM, SQUARE_NONE);

    /* Add some magma streamers */
    for (i = 0; i < dun->profile->str.mag; i++)
        build_streamer(c, FEAT_MAGMA, dun->profile->str.mc);

    /* Add some quartz streamers */
    for (i = 0; i < dun->profile->str.qua; i++)
        build_streamer(c, FEAT_QUARTZ, dun->profile->str.qc);

    /* Add some streamers */
    add_streamer(c, FEAT_LAVA, DF_LAVA_RIVER);
    add_streamer(c, FEAT_WATER, DF_WATER_RIVER);
    add_streamer(c, FEAT_SANDWALL, DF_SAND_VEIN);

    /* Tweak floors and walls */
    fill_level(c, false);
    fill_level(c, true);

    /* Place stairs near some walls */
    add_stairs(c, FEAT_MORE);
    add_stairs(c, FEAT_LESS);

    /* Remove holes in corridors that were not used for stair placement */
    remove_unused_holes(c);

    /* General amount of rubble, traps and monsters */
    k = MAX(MIN(wpos->depth / 3, 10), 2);

    /* Put some rubble in corridors */
    alloc_objects(p, c, SET_CORR, TYP_RUBBLE, randint1(k), wpos->depth, 0);

    /* Place some traps in the dungeon, reduce frequency by factor of 5 */
    alloc_objects(p, c, SET_CORR, TYP_TRAP, randint1(k) / 5, wpos->depth, 0);

    /* Place some fountains in rooms */
    alloc_objects(p, c, SET_ROOM, TYP_FOUNTAIN, randint0(1 + k / 2), wpos->depth, 0);

    /* Determine the character location */
    new_player_spot(c, p);

    /* Pick a base number of monsters */
    i = z_info->level_monster_min + randint1(8) + k;

    /* Moria levels have a high proportion of cave dwellers. */
    mon_restrict(p, "Moria dwellers", wpos->depth, true);

    /* Put some monsters in the dungeon */
    for (; i > 0; i--)
        pick_and_place_distant_monster(p, c, 0, MON_ASLEEP);

    /* Remove our restrictions. */
    mon_restrict(p, NULL, wpos->depth, false);

    /* Put some objects in rooms */
    alloc_objects(p, c, SET_ROOM, TYP_OBJECT, Rand_normal(z_info->room_item_av, 3), wpos->depth,
        ORIGIN_FLOOR);

    /* Put some objects/gold in the dungeon */
    alloc_objects(p, c, SET_BOTH, TYP_OBJECT, Rand_normal(z_info->both_item_av, 3), wpos->depth,
        ORIGIN_FLOOR);
    alloc_objects(p, c, SET_BOTH, TYP_GOLD, Rand_normal(z_info->both_gold_av, 3), wpos->depth,
        ORIGIN_FLOOR);

    /* Apply illumination */
    player_cave_clear(p, true);
    cave_illuminate(p, c, true);

    return c;
}


/* ------------------ MANGBAND TOWN ---------------- */


/*
 * Builds a feature at a given pseudo-location
 *
 * c is the current chunk
 * n is which feature it is
 * yy, xx the row and column of this feature in the feature layout
 *
 * Currently, there is a main street horizontally through the middle of town,
 * and all the shops face it (e.g. the shops on the north side face south).
 */
static void build_feature(struct chunk *c, int n, int yy, int xx)
{
    int dy, dx;
    int feat;

    /* Determine spacing based on town size */
    int y_space = z_info->dungeon_hgt / z_info->town_hgt;
    int x_space = z_info->dungeon_wid / z_info->town_wid;

    /* Find the "center" of the feature */
    int y0 = yy * y_space + y_space / 2;
    int x0 = xx * x_space + x_space / 2;

    /* Determine the feature boundaries */
    int y1 = y0 - randint1(3);
    int y2 = y0 + randint1(3);
    int x1 = x0 - randint1(5);
    int x2 = x0 + randint1(5);

    struct loc begin, end, grid;
    struct loc_iterator iter;

    int type = ((n < store_max - 2)? stores[n].type: -1);

    /* Hack -- make forest/tavern as large as possible */
    if ((n == store_max - 1) || (type == STORE_TAVERN))
    {
        y1 = y0 - 3;
        y2 = y0 + 3;
        x1 = x0 - 5;
        x2 = x0 + 5;
    }

    /* House (at least 2x2) */
    if (n == store_max)
    {
        while (y2 - y1 == 2)
        {
            y1 = y0 - randint1((yy == 0)? 3: 2);
            y2 = y0 + randint1((yy == 1)? 3: 2);
        }
        while (x2 - x1 == 2)
        {
            x1 = x0 - randint1(5);
            x2 = x0 + randint1(5);
        }
    }

    /* Determine door location, based on which side of the street we're on */
    dy = (((yy % 2) == 0)? y2: y1);
    dx = rand_range(x1, x2);

    /* Build an invulnerable rectangular building */
    fill_rectangle(c, y1, x1, y2, x2, FEAT_PERM, SQUARE_NONE);

    /* Hack -- make tavern empty */
    if (type == STORE_TAVERN)
    {
        loc_init(&begin, x1 + 1, y1 + 1);
        loc_init(&end, x2, y2);
        loc_iterator_first(&iter, &begin, &end);

        do
        {
            /* Create the tavern, make it PvP-safe */
            square_add_safe(c, &iter.cur);

            /* Declare this to be a room */
            sqinfo_on(square(c, &iter.cur)->info, SQUARE_VAULT);
            sqinfo_on(square(c, &iter.cur)->info, SQUARE_NOTRASH);
            sqinfo_on(square(c, &iter.cur)->info, SQUARE_ROOM);
        }
        while (loc_iterator_next_strict(&iter));

        /* Hack -- have everyone start in the tavern */
        loc_init(&grid, (x1 + x2) / 2, (y1 + y2) / 2);
        square_set_join_down(c, &grid);
    }

    /* Pond */
    if (n == store_max - 2)
    {
        /* Create the pond */
        fill_rectangle(c, y1, x1, y2, x2, FEAT_WATER, SQUARE_NONE);

        /* Make the pond not so "square" */
        loc_init(&grid, x1, y1);
        square_add_dirt(c, &grid);
        loc_init(&grid, x2, y1);
        square_add_dirt(c, &grid);
        loc_init(&grid, x1, y2);
        square_add_dirt(c, &grid);
        loc_init(&grid, x2, y2);
        square_add_dirt(c, &grid);

        return;
    }

    /* Forest */
    if (n == store_max - 1)
    {
        int xc, yc, max_dis;
        int size = (y2 - y1 + 1) * (x2 - x1 + 1);
        struct loc center;

        loc_init(&begin, x1, y1);
        loc_init(&end, x2, y2);

        /* Find the center of the forested area */
        xc = (x1 + x2) / 2;
        yc = (y1 + y2) / 2;
        loc_init(&center, xc, yc);

        /* Find the max distance from center */
        max_dis = distance(&end, &center);

        loc_iterator_first(&iter, &begin, &end);

        do
        {
            int chance;

            /* Put some grass */
            square_add_grass(c, &iter.cur);

            /* Calculate chance of a tree */
            chance = 100 * (distance(&iter.cur, &center));
            chance /= max_dis;
            chance = 80 - chance;
            chance *= size;

            /* Put some trees */
            if (CHANCE(chance, 100 * size)) square_add_tree(c, &iter.cur);
        }
        while (loc_iterator_next(&iter));

        return;
    }

    /* House */
    if (n == store_max)
    {
        int house, price;

        loc_init(&begin, x1 + 1, y1 + 1);
        loc_init(&end, x2, y2);
        loc_iterator_first(&iter, &begin, &end);

        do
        {
            /* Fill with safe floor */
            square_add_safe(c, &iter.cur);

            /* Declare this to be a room */
            sqinfo_on(square(c, &iter.cur)->info, SQUARE_VAULT);
            sqinfo_on(square(c, &iter.cur)->info, SQUARE_ROOM);
        }
        while (loc_iterator_next_strict(&iter));

        /* Remember price */
        price = (x2 - x1 - 1) * (y2 - y1 - 1);
        price *= 20;
        price *= 80 + randint1(40);

        /* Hack -- only create houses that aren't already loaded from disk */
        loc_init(&grid, dx, dy);
        house = pick_house(&c->wpos, &grid);
        if (house == -1)
        {
            struct house_type h_local;

            square_colorize_door(c, &grid, 0);

            /* Get an empty house slot */
            house = house_add(false);

            /* Setup house info */
            loc_init(&h_local.grid_1, x1 + 1, y1 + 1);
            loc_init(&h_local.grid_2, x2 - 1, y2 - 1);
            loc_init(&h_local.door, dx, dy);
            memcpy(&h_local.wpos, &c->wpos, sizeof(struct worldpos));
            h_local.price = price;
            h_local.ownerid = 0;
            h_local.ownername[0] = '\0';
            h_local.color = 0;
            h_local.state = HOUSE_NORMAL;
            h_local.free = 0;

            /* Add a house to our houses list */
            house_set(house, &h_local);
        }
        else
        {
            /* Tag owned house door */
            square_colorize_door(c, &grid, house_get(house)->color);
        }

        return;
    }

    /* Building with stairs */
    if (n == store_max + 1)
    {
        loc_init(&begin, x1, y1);
        loc_init(&end, x2, y2);
        loc_iterator_first(&iter, &begin, &end);

        do
        {
            /* Create the area */
            if (magik(50))
                square_add_grass(c, &iter.cur);
            else
                square_set_feat(c, &iter.cur, FEAT_FLOOR);
        }
        while (loc_iterator_next(&iter));

        loc_init(&grid, (x1 + x2) / 2, (y1 + y2) / 2);

        /* Place a staircase */
        square_set_downstairs(c, &grid);

        /* Hack -- the players start on the stairs while recalling */
        square_set_join_rand(c, &grid);

        return;
    }

    loc_init(&grid, dx, dy);

    /* Clear previous contents, add a store door */
    for (feat = 0; feat < z_info->f_max; feat++)
    {
        if (feat_is_shop(feat) && (f_info[feat].shopnum == n + 1))
            square_set_feat(c, &grid, feat);
    }
}


/*
 * Build a road.
 */
static void place_street(struct chunk *c, int line, bool vert)
{
    int y1, y2, x1, x2;
    struct loc begin, end;
    struct loc_iterator iter;

    /* Vertical streets */
    if (vert)
    {
        x1 = line * z_info->dungeon_wid / z_info->town_wid - 2;
        x2 = line * z_info->dungeon_wid / z_info->town_wid + 2;

        y1 = 5;
        y2 = c->height - 5;
    }
    else
    {
        y1 = line * z_info->dungeon_hgt / z_info->town_hgt - 2;
        y2 = line * z_info->dungeon_hgt / z_info->town_hgt + 2;

        x1 = 5;
        x2 = c->width - 5;
    }

    loc_init(&begin, x1, y1);
    loc_init(&end, x2, y2);
    loc_iterator_first(&iter, &begin, &end);

    do
    {
        if (square(c, &iter.cur)->feat != FEAT_STREET) square_add_grass(c, &iter.cur);
    }
    while (loc_iterator_next(&iter));

    if (vert)
    {
        x1++;
        x2--;
    }
    else
    {
        y1++;
        y2--;
    }

    fill_rectangle(c, y1, x1, y2, x2, FEAT_STREET, SQUARE_NONE);
}


/*
 * Generate the starting town for the first time
 *
 * c is the current chunk
 */
static void mang_town_gen_layout(struct chunk *c)
{
    int y, x, n, k;
    int *rooms;
    int n_stores = store_max - 2; /* store_max - 2 stores */
    int n_rows = 2;
    int n_cols = n_stores / n_rows;
    int size = (c->height - 2) * (c->width - 2);
    int chance;
    struct loc begin, end;
    struct loc_iterator iter;

    /* Determine spacing based on town size */
    int y0 = (z_info->town_hgt - n_rows) / 2;
    int x0 = (z_info->town_wid - n_cols) / 2;

    u32b tmp_seed = Rand_value;
    bool rand_old = Rand_quick;

    /* Hack -- use the "simple" RNG */
    Rand_quick = true;

    /* Hack -- induce consistant town */
    Rand_value = seed_wild + world_index(&c->wpos) * 600;

    /* Create boundary */
    draw_rectangle(c, 0, 0, c->height - 1, c->width - 1, FEAT_PERM_CLEAR, SQUARE_NONE);

    /* Create some floor */
    fill_rectangle(c, 1, 1, c->height - 2, c->width - 2, FEAT_FLOOR, SQUARE_NONE);

    /* Calculate chance of a tree */
    chance = 4 * size;

    loc_init(&begin, 1, 1);
    loc_init(&end, c->width - 1, c->height - 1);
    loc_iterator_first(&iter, &begin, &end);

    /* Hack -- start with basic floors */
    do
    {
        /* Clear all features, set to "empty floor" */
        square_add_dirt(c, &iter.cur);

        /* Generate some trees */
        if (CHANCE(chance, 100 * size)) square_add_tree(c, &iter.cur);

        /* Generate grass patches */
        else if (magik(75)) square_add_grass(c, &iter.cur);
    }
    while (loc_iterator_next_strict(&iter));

    /* Place horizontal "streets" */
    for (y = 1; y <= z_info->town_hgt / 2; y = y + 2) place_street(c, y, false);
    for (y = z_info->town_hgt - 1; y > z_info->town_hgt / 2; y = y - 2) place_street(c, y, false);

    /* Place vertical "streets" */
    for (x = 1; x <= z_info->town_wid / 2; x = x + 2) place_street(c, x, true);
    for (x = z_info->town_wid - 1; x > z_info->town_wid / 2; x = x - 2) place_street(c, x, true);

    /* Prepare an array of remaining features, and count them */
    rooms = mem_zalloc(z_info->town_wid * z_info->town_hgt * sizeof(int));
    for (n = 0; n < n_stores; n++)
        rooms[n] = n; /* n_stores stores */
    for (n = n_stores; n < n_stores + 6; n++)
        rooms[n] = n_stores; /* 6 ponds */
    for (n = n_stores + 6; n < n_stores + 9; n++)
        rooms[n] = n_stores + 1; /* 3 forests */
    for (n = n_stores + 9; n < z_info->town_wid * z_info->town_hgt - 1; n++)
        rooms[n] = n_stores + 2; /* houses */
    rooms[n++] = n_stores + 3; /* stairs */

    /* Place rows of stores */
    for (y = y0; y < y0 + n_rows; y++)
    {
        for (x = x0; x < x0 + n_cols; x++)
        {
            /* Pick a remaining store */
            k = randint0(n - z_info->town_wid * z_info->town_hgt + n_stores);

            /* Build that store at the proper location */
            build_feature(c, rooms[k], y, x);

            /* Shift the stores down, remove one store */
            n--;
            rooms[k] = rooms[n - z_info->town_wid * z_info->town_hgt + n_stores];
        }
    }

    /* Place rows of features */
    for (y = 0; y < z_info->town_hgt; y++)
    {
        for (x = 0; x < z_info->town_wid; x++)
        {
            /* Make sure we haven't already placed this one */
            if ((y >= y0) && (y < y0 + n_rows) && (x >= x0) && (x < x0 + n_cols)) continue;

            /* Pick a remaining feature */
            k = randint0(n) + n_stores;

            /* Build that feature at the proper location */
            build_feature(c, rooms[k], y, x);

            /* Shift the features down, remove one feature */
            n--;
            rooms[k] = rooms[n + n_stores];
        }
    }

    mem_free(rooms);

    /* Hack -- use the "complex" RNG */
    Rand_value = tmp_seed;
    Rand_quick = rand_old;
}


/*
 * Town logic flow for generation of MAngband-style town.
 *
 * p is the player
 * wpos is the position on the world map
 *
 * Returns a pointer to the generated chunk.
 *
 * We start with a fully wiped cave of normal floors. This function does NOT do
 * anything about the owners of the stores, nor the contents thereof. It only
 * handles the physical layout.
 *
 * Hack -- since boundary walls are a 'good thing' for many of the algorithms
 * used, the feature FEAT_PERM_CLEAR was created. It is used to create an
 * invisible boundary wall for town and wilderness levels, keeping the
 * algorithms happy, and the players fooled.
 */
struct chunk *mang_town_gen(struct player *p, struct worldpos *wpos, int min_height, int min_width)
{
    int i;
    int residents = (is_daytime()? z_info->town_monsters_day: z_info->town_monsters_night);

    /* Make a new chunk */
    struct chunk *c = cave_new(z_info->dungeon_hgt, z_info->dungeon_wid);

    memcpy(&c->wpos, wpos, sizeof(struct worldpos));

    player_cave_new(p, z_info->dungeon_hgt, z_info->dungeon_wid);

    /* Build stuff */
    mang_town_gen_layout(c);

    /* Apply illumination */
    player_cave_clear(p, true);
    cave_illuminate(p, c, is_daytime());

    /* Make some residents */
    for (i = 0; i < residents; i++)
        pick_and_place_distant_monster(p, c, 0, MON_ASLEEP);

    return c;
}


/*
 * Make an arena level.
 *
 * p is the player
 * wpos is the position on the world map
 */
struct chunk *arena_gen(struct player *p, struct worldpos *wpos, int min_height, int min_width)
{
    int i, j, k;
    struct loc grid;
    int by, bx = 0, tby, tbx, key, rarity, built;
    int num_rooms;
    int dun_unusual = dun->profile->dun_unusual;
    bool **blocks_tried;
    struct chunk *c;

    /* Most arena levels are lit */
    bool lit = ((randint0(wpos->depth) < 25) || magik(90));

    /* Scale the various generation variables */
    num_rooms = dun->profile->dun_rooms;
    dun->block_hgt = dun->profile->block_size;
    dun->block_wid = dun->profile->block_size;
    c = cave_new(z_info->dungeon_hgt, z_info->dungeon_wid);
    memcpy(&c->wpos, wpos, sizeof(struct worldpos));
    player_cave_new(p, z_info->dungeon_hgt, z_info->dungeon_wid);

    /* Fill cave area with basic granite */
    fill_rectangle(c, 0, 0, c->height - 1, c->width - 1, FEAT_GRANITE, SQUARE_NONE);
    fill_rectangle(c, 1, 1, c->height - 2, c->width - 2, FEAT_FLOOR, SQUARE_NONE);

    /* Actual maximum number of rooms on this level */
    dun->row_blocks = c->height / dun->block_hgt;
    dun->col_blocks = c->width / dun->block_wid;

    /* Initialize the room table */
    dun->room_map = mem_zalloc(dun->row_blocks * sizeof(bool*));
    for (i = 0; i < dun->row_blocks; i++)
        dun->room_map[i] = mem_zalloc(dun->col_blocks * sizeof(bool));

    /* Initialize the block table */
    blocks_tried = mem_zalloc(dun->row_blocks * sizeof(bool*));
    for (i = 0; i < dun->row_blocks; i++)
        blocks_tried[i] = mem_zalloc(dun->col_blocks * sizeof(bool));

    /* No rooms yet, pits or otherwise. */
    dun->pit_num = 0;
    dun->cent_n = 0;

    /*
     * Build some rooms. Note that the theoretical maximum number of rooms
     * in this profile is currently 36, so built never reaches num_rooms,
     * and room generation is always terminated by having tried all blocks
     */
    built = 0;
    while (built < num_rooms)
    {
        /* Count the room blocks we haven't tried yet. */
        j = 0;
        tby = 0;
        tbx = 0;
        for (by = 0; by < dun->row_blocks; by++)
        {
            for (bx = 0; bx < dun->col_blocks; bx++)
            {
                if (blocks_tried[by][bx]) continue;
                j++;
                if (one_in_(j))
                {
                    tby = by;
                    tbx = bx;
                }
            }
        }
        bx = tbx;
        by = tby;

        /* If we've tried all blocks we're done. */
        if (j == 0) break;

        if (blocks_tried[by][bx]) quit_fmt("generation: inconsistent blocks");

        /* Mark that we are trying this block. */
        blocks_tried[by][bx] = true;

        /* Roll for random key (to be compared against a profile's cutoff) */
        key = randint0(100);

        /*
         * We generate a rarity number to figure out how exotic to make the
         * room. This number has a depth/dun_unusual chance of being > 0,
         * a depth^2/dun_unusual^2 chance of being > 1, up to dun->profile->max_rarity.
         */
        i = 0;
        rarity = 0;
        while ((i == rarity) && (i < dun->profile->max_rarity))
        {
            if (randint0(dun_unusual) < 50 + wpos->depth / 2) rarity++;
            i++;
        }

        /*
         * Once we have a key and a rarity, we iterate through out list of
         * room profiles looking for a match (whose cutoff > key and whose
         * rarity > this rarity). We try building the room, and if it works
         * then we are done with this iteration. We keep going until we find
         * a room that we can build successfully or we exhaust the profiles.
         */
        for (i = 0; i < dun->profile->n_room_profiles; i++)
        {
            struct room_profile profile = dun->profile->room_profiles[i];

            if (profile.rarity > rarity) continue;
            if (profile.cutoff <= key) continue;

            if (room_build(p, c, by, bx, profile, false))
            {
                built++;
                break;
            }
        }
    }

    for (i = 0; i < dun->row_blocks; i++)
    {
        mem_free(blocks_tried[i]);
        mem_free(dun->room_map[i]);
    }
    mem_free(blocks_tried);
    mem_free(dun->room_map);

    /* Generate permanent walls around the edge of the generated area */
    draw_rectangle(c, 0, 0, c->height - 1, c->width - 1, FEAT_PERM, SQUARE_NONE);

    /* Hack -- scramble the room order */
    for (i = 0; i < dun->cent_n; i++)
    {
        int pick1 = randint0(dun->cent_n);
        int pick2 = randint0(dun->cent_n);
        struct loc tmp;

        loc_copy(&tmp, &dun->cent[pick1]);
        loc_copy(&dun->cent[pick1], &dun->cent[pick2]);
        loc_copy(&dun->cent[pick2], &tmp);
    }

    /* Start with no tunnel doors */
    dun->door_n = 0;

    /* Hack -- connect the first room to the last room */
    loc_copy(&grid, &dun->cent[dun->cent_n - 1]);

    /* Connect all the rooms together */
    for (i = 0; i < dun->cent_n; i++)
    {
        /* Connect the room to the previous room */
        build_tunnel(c, &dun->cent[i], &grid);

        /* Remember the "previous" room */
        loc_copy(&grid, &dun->cent[i]);
    }

    /* Place intersection doors */
    for (i = 0; i < dun->door_n; i++)
    {
        /* Try placing doors */
        next_grid(&grid, &dun->door[i], DIR_W);
        try_door(c, &grid);
        next_grid(&grid, &dun->door[i], DIR_E);
        try_door(c, &grid);
        next_grid(&grid, &dun->door[i], DIR_N);
        try_door(c, &grid);
        next_grid(&grid, &dun->door[i], DIR_S);
        try_door(c, &grid);
    }

    ensure_connectedness(c);

    /* Tweak floors */
    fill_level(c, true);

    /* Place stairs near some walls */
    add_stairs(c, FEAT_MORE);
    add_stairs(c, FEAT_LESS);

    /* Remove holes in corridors that were not used for stair placement */
    remove_unused_holes(c);

    /* General amount of rubble, traps and monsters */
    k = MAX(MIN(wpos->depth / 3, 10), 2);

    /* Put some rubble in corridors */
    alloc_objects(p, c, SET_CORR, TYP_RUBBLE, randint1(k), wpos->depth, 0);

    /* Place some traps in the dungeon, reduce frequency by factor of 5 */
    alloc_objects(p, c, SET_CORR, TYP_TRAP, randint1(k) / 5, wpos->depth, 0);

    /* Place some fountains in rooms */
    alloc_objects(p, c, SET_ROOM, TYP_FOUNTAIN, randint0(1 + k / 2), wpos->depth, 0);

    /* Determine the character location */
    new_player_spot(c, p);

    /* Pick a base number of monsters */
    i = z_info->level_monster_min + randint1(8) + k;

    /* Put some monsters in the dungeon */
    for (; i > 0; i--)
        pick_and_place_distant_monster(p, c, 0, MON_ASLEEP);

    /* Put some objects in rooms */
    alloc_objects(p, c, SET_ROOM, TYP_OBJECT, Rand_normal(z_info->room_item_av, 3), wpos->depth,
        ORIGIN_FLOOR);

    /* Put some objects/gold in the dungeon */
    alloc_objects(p, c, SET_BOTH, TYP_OBJECT, Rand_normal(z_info->both_item_av, 3), wpos->depth,
        ORIGIN_FLOOR);
    alloc_objects(p, c, SET_BOTH, TYP_GOLD, Rand_normal(z_info->both_gold_av, 3), wpos->depth,
        ORIGIN_FLOOR);

    /* Apply illumination */
    player_cave_clear(p, true);
    if (lit) c->light_level = true;

    return c;
}

/*
 * File: project-obj.c
 * Purpose: Projection effects on objects
 *
 * Copyright (c) 1997 Ben Harrison, James E. Wilson, Robert A. Koeneke
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


/*
 * Destroys a type of item on a given percent chance.
 * The chance 'cperc' is in hundredths of a percent (1-in-10000)
 * Note that missiles are no longer necessarily all destroyed
 *
 * Returns number of items destroyed.
 */
int inven_damage(struct player *p, int type, int cperc)
{
    int j, k, amt;
    struct object *obj = p->gear;
    char o_name[NORMAL_WID];
    bool damage;

    /* No chance means no damage */
    if (cperc <= 0) return 0;

    /* Count the casualties */
    k = 0;

    /* Scan through the gear */
    while (obj)
    {
        struct object *next = obj->next;

        if (object_is_equipped(p->body, obj))
        {
            obj = next;
            continue;
        }

        /* Hack -- for now, skip artifacts */
        if (obj->artifact)
        {
            obj = next;
            continue;
        }

        /* Give this item slot a shot at death if it is vulnerable */
        if ((obj->el_info[type].flags & EL_INFO_HATES) &&
            !(obj->el_info[type].flags & EL_INFO_IGNORE))
        {
            /* Chance to destroy this item */
            int chance = cperc;

            /* Track if it is damaged instead of destroyed */
            damage = false;

            /*
             * Analyze the type to see if we just damage it
             * We also check for rods to reduce chance
             */
            if (tval_is_enchantable_weapon(obj) && !tval_is_ammo(obj))
            {
                /* Chance to damage it */
                if (CHANCE(cperc, 10000))
                {
                    /* Damage the item */
                    obj->to_h--;
                    obj->to_d--;

                    /* Damaged! */
                    damage = true;
                }
                else
                {
                    obj = next;
                    continue;
                }
            }
            else if (tval_is_armor(obj))
            {
                /* Chance to damage it */
                if (CHANCE(cperc, 10000))
                {
                    /* Damage the item */
                    obj->to_a--;

                    /* Damaged! */
                    damage = true;
                }
                else
                {
                    obj = next;
                    continue;
                }
            }
            else if (tval_is_rod(obj))
                chance = (chance / 4);

            /* Damage instead of destroy */
            if (damage)
            {
                p->upkeep->update |= (PU_BONUS);
                p->upkeep->redraw |= (PR_EQUIP);

                /* Casualty count */
                amt = obj->number;
            }

            /* ... or count the casualties */
            else for (amt = j = 0; j < obj->number; ++j)
            {
                if (CHANCE(chance, 10000)) amt++;
            }

            /* Some casualties */
            if (amt)
            {
                struct object *destroyed;
                bool none_left = false;

                /* Get a description */
                object_desc(p, o_name, sizeof(o_name), obj, ODESC_BASE);

                /* Message */
                msgt(p, MSG_DESTROY, "%sour %s (%c) %s %s!",
                    ((obj->number > 1) ? ((amt == obj->number) ? "All of y" :
                    (amt > 1 ? "Some of y" : "One of y")) : "Y"), o_name,
                    gear_to_label(p, obj), ((amt > 1) ? "were" : "was"),
                    (damage? "damaged": "destroyed"));

                /* Damage already done? */
                if (damage)
                {
                    obj = next;
                    continue;
                }

                /* Destroy "amt" items */
                destroyed = gear_object_for_use(p, obj, amt, false, &none_left);
                object_delete(&destroyed);

                /* Count the casualties */
                k += amt;
            }
        }

        obj = next;
    }

    /* Return the casualty count */
    return (k);
}


/*
 * Object handlers
 */


struct monster_race *get_race(const char *name)
{
    s16b race;

    for (race = 1; race < z_info->r_max; race++)
    {
        struct monster_race *r = &r_info[race];

        if (!r->name) continue;
        if (!strcmp(r->name, name)) return r;
    }

    return NULL;
}


typedef struct project_object_handler_context_s
{
    struct source *origin;
    int r;
    struct chunk *cave;
    struct loc grid;
    int dam;
    int type;
    struct object *obj;
    bool observed;
    bool obvious;
    bool do_kill;
    bool ignore;
    const char *note_kill;
} project_object_handler_context_t;


typedef void (*project_object_handler_f)(project_object_handler_context_t *);


/*
 * Project an effect onto an object.
 *
 * context is the project_o context.
 * element is for elements that will destroy an object, or that it will ignore.
 * singular_verb is the verb that is displayed when one object is destroyed.
 * plural_verb is the verb that is displayed in multiple objects are destroyed.
 */
static void project_object_elemental(project_object_handler_context_t *context, int element,
    const char *singular_verb, const char *plural_verb)
{
    if (context->obj->el_info[element].flags & EL_INFO_HATES)
    {
        context->do_kill = true;
        context->note_kill = VERB_AGREEMENT(context->obj->number, singular_verb, plural_verb);
        if (context->obj->el_info[element].flags & EL_INFO_IGNORE) context->ignore = true;
    }
}


/* Acid -- lots of things */
static void project_object_handler_ACID(project_object_handler_context_t *context)
{
    project_object_elemental(context, ELEM_ACID, "melts", "melt");
}


/* Elec -- rings and wands */
static void project_object_handler_ELEC(project_object_handler_context_t *context)
{
    project_object_elemental(context, ELEM_ELEC, "is destroyed", "are destroyed");
}


/* Fire -- flammable objects */
static void project_object_handler_FIRE(project_object_handler_context_t *context)
{
    project_object_elemental(context, ELEM_FIRE, "burns up", "burn up");
}


/* Cold -- potions and flasks */
static void project_object_handler_COLD(project_object_handler_context_t *context)
{
    project_object_elemental(context, ELEM_COLD, "shatters", "shatter");
}


static void project_object_handler_POIS(project_object_handler_context_t *context) {}
static void project_object_handler_LIGHT(project_object_handler_context_t *context) {}
static void project_object_handler_DARK(project_object_handler_context_t *context) {}


/* Sound -- potions and flasks */
static void project_object_handler_SOUND(project_object_handler_context_t *context)
{
    project_object_elemental(context, ELEM_SOUND, "shatters", "shatter");
}


/* Shards -- potions and flasks */
static void project_object_handler_SHARD(project_object_handler_context_t *context)
{
    project_object_elemental(context, ELEM_SHARD, "shatters", "shatter");
}


static void project_object_handler_NEXUS(project_object_handler_context_t *context) {}
static void project_object_handler_NETHER(project_object_handler_context_t *context) {}
static void project_object_handler_CHAOS(project_object_handler_context_t *context) {}
static void project_object_handler_DISEN(project_object_handler_context_t *context) {}
static void project_object_handler_WATER(project_object_handler_context_t *context) {}


/* Ice -- potions and flasks */
static void project_object_handler_ICE(project_object_handler_context_t *context)
{
    project_object_elemental(context, ELEM_ICE, "shatters", "shatter");
}


static void project_object_handler_GRAVITY(project_object_handler_context_t *context) {}
static void project_object_handler_INERTIA(project_object_handler_context_t *context) {}


/* Force -- potions and flasks */
static void project_object_handler_FORCE(project_object_handler_context_t *context)
{
    project_object_elemental(context, ELEM_FORCE, "shatters", "shatter");
}


static void project_object_handler_TIME(project_object_handler_context_t *context) {}


/* Fire + Elec */
static void project_object_handler_PLASMA(project_object_handler_context_t *context)
{
    project_object_elemental(context, ELEM_FIRE, "burns up", "burn up");
    if (context->obj->el_info[ELEM_ELEC].flags & EL_INFO_HATES) context->ignore = false;
    project_object_elemental(context, ELEM_ELEC, "is destroyed", "are destroyed");
}


/* Fire + Cold */
static void project_object_handler_METEOR(project_object_handler_context_t *context)
{
    project_object_elemental(context, ELEM_FIRE, "burns up", "burn up");
    if (context->obj->el_info[ELEM_COLD].flags & EL_INFO_HATES) context->ignore = false;
    project_object_elemental(context, ELEM_COLD, "shatters", "shatter");
}


static void project_object_handler_MISSILE(project_object_handler_context_t *context) {}


/* Mana -- destroys everything */
static void project_object_handler_MANA(project_object_handler_context_t *context)
{
    context->do_kill = true;
    context->note_kill = VERB_AGREEMENT(context->obj->number, "is destroyed", "are destroyed");
}


static void project_object_handler_HOLY_ORB(project_object_handler_context_t *context) {}
static void project_object_handler_ARROW_X(project_object_handler_context_t *context) {}
static void project_object_handler_ARROW_1(project_object_handler_context_t *context) {}
static void project_object_handler_ARROW_2(project_object_handler_context_t *context) {}
static void project_object_handler_ARROW_3(project_object_handler_context_t *context) {}
static void project_object_handler_ARROW_4(project_object_handler_context_t *context) {}
static void project_object_handler_BOULDER(project_object_handler_context_t *context) {}
static void project_object_handler_LIGHT_WEAK(project_object_handler_context_t *context) {}
static void project_object_handler_DARK_WEAK(project_object_handler_context_t *context) {}
static void project_object_handler_KILL_WALL(project_object_handler_context_t *context) {}
static void project_object_handler_KILL_DOOR(project_object_handler_context_t *context) {}


/* Unlock chests */
static void project_object_handler_KILL_TRAP(project_object_handler_context_t *context)
{
    /* Chests are noticed only if trapped or locked */
    if (is_locked_chest(context->obj))
    {
        /* Disarm or Unlock */
        unlock_chest(context->obj);

        /* Identify */
        if (context->origin->player)
            object_notice_everything_aux(context->origin->player, context->obj, true, false);

        /* Notice */
        if (context->observed)
        {
            msg(context->origin->player, "Click!");
            context->obvious = true;
        }
    }
}


static void project_object_handler_MAKE_DOOR(project_object_handler_context_t *context) {}
static void project_object_handler_MAKE_TRAP(project_object_handler_context_t *context) {}
static void project_object_handler_STONE_WALL(project_object_handler_context_t *context) {}


/* Can this race be raised? */
static bool valid_race(project_object_handler_context_t *context, struct monster_race *race)
{
    int level = monster_level(&context->cave->wpos), summon_level;

    if (!race) return false;

    /* Skip uniques and monsters that can't be generated */
    if (monster_is_unique(race)) return false;
    if (rf_has(race->flags, RF_PWMANG_BASE) && !cfg_base_monsters) return false;
    if (rf_has(race->flags, RF_PWMANG_EXTRA) && !cfg_extra_monsters) return false;

    /* Raised monsters can't be too powerful */
    if (context->origin->player)
        summon_level = (level + context->origin->player->lev) / 2 + 5;
    else
        summon_level = (level + context->origin->monster->level) / 2 + 5;
    return (race->level <= summon_level);
}


/* Is the race consistent with the corpse? */
static bool consistent_corpse(struct monster_race *race, struct monster_race *corpse)
{
    if (streq(race->name, "zombified kobold"))
        return (corpse->base == lookup_monster_base("kobold"));

    if (streq(race->name, "zombified orc") || streq(race->name, "mummified orc"))
        return (corpse->base == lookup_monster_base("orc"));

    if (streq(race->name, "zombified human") || streq(race->name, "mummified human"))
        return (corpse->base == lookup_monster_base("person"));

    if (streq(race->name, "mummified troll"))
        return (corpse->base == lookup_monster_base("troll"));

    return true;
}


/* Raise dead */
static void project_object_handler_RAISE(project_object_handler_context_t *context)
{
    struct monster_race *race = NULL;
    struct chunk *c = context->cave;
    struct loc grid;

    /* Raise dead prohibited in towns and special levels */
    if (forbid_special(&c->wpos)) return;

    /* Skeletons and corpses can be raised */
    if (!tval_is_skeleton(context->obj) && !tval_is_corpse(context->obj)) return;

    /* Skeletons must match an existing skeleton race */
    if (tval_is_skeleton(context->obj))
    {
        if (strstr(context->obj->kind->name, "Human"))
            race = get_race("skeleton human");
        else if (strstr(context->obj->kind->name, "Elf"))
            race = get_race("ice skeleton");
        else if (strstr(context->obj->kind->name, "Kobold"))
            race = get_race("skeleton kobold");
        else if (strstr(context->obj->kind->name, "Orc"))
            race = get_race("skeleton orc");
        else if (context->obj->sval == lookup_sval(context->obj->tval, "Skull"))
        {
            /* Give a chance of getting a powerful skull druj */
            if (one_in_(3))
                race = get_race("skull druj");
            else
                race = get_race("flying skull");
        }
        else if (strstr(context->obj->kind->name, "Troll"))
            race = get_race("skeleton troll");
        else if (strstr(context->obj->kind->name, "Ettin"))
            race = get_race("skeleton ettin");
    }

    /* Humanoid corpses can be raised too */
    else if (context->obj->sval == lookup_sval(context->obj->tval, "corpse (humanoid)"))
    {
        /* Rotten corpses only produce... rotting corpses :) */
        if (context->obj->decay <= context->obj->timeout / 5)
            race = get_race("rotting corpse");

        /* Otherwise raise a zombie, wraith or lich */
        else
        {
            struct monster_race *corpse = &r_info[context->obj->pval];
            int tries = 20;

            /* Try hard to raise something */
            while (tries)
            {
                while (true)
                {
                    race = &r_info[randint1(z_info->r_max - 1)];
                    if (!race->name) continue;

                    /* Animated corpses can be any of non unique z, W or L */
                    if (race->base == lookup_monster_base("wraith")) break;
                    if (race->base == lookup_monster_base("lich")) break;
                    if ((race->base == lookup_monster_base("zombie")) &&
                        consistent_corpse(race, corpse))
                    {
                        break;
                    }
                }

                if (valid_race(context, race)) break;
                race = NULL;
                tries--;
            }
        }
    }

    /* Give a chance of getting a powerful dracolich from dragon corpses */
    else
    {
        struct monster_race *corpse = &r_info[context->obj->pval];

        if (one_in_(3) && rf_has(corpse->flags, RF_DRAGON))
            race = get_race("dracolich");
    }

    /* Failure */
    if (!valid_race(context, race)) return;

    /* Raising dead costs mana */
    if (context->origin->player && !OPT(context->origin->player, risky_casting) &&
        (race->level > (context->origin->player->csp - context->origin->player->spell_cost)))
    {
        return;
    }

    /* Look for a location */
    if (!summon_location(c, &grid, &context->grid, 60)) return;

    /* Place a new monster */
    if (place_new_monster(context->origin->player, c, &grid, race, 0, ORIGIN_DROP_SUMMON))
    {
        context->do_kill = true;

        /* Handle monsters raised by the player */
        if (context->origin->player)
        {
            struct monster *mon;

            msg(context->origin->player, "A monster rises from the grave!");

            /* Hack -- get new monster */
            mon = square_monster(c, &grid);

            /* Raised monsters are mostly neutral */
            if (magik(80)) monster_set_master(mon, context->origin->player, MSTATUS_GUARD);

            /* Use some mana */
            context->origin->player->spell_cost += race->level;
        }
    }
}


static void project_object_handler_AWAY_EVIL(project_object_handler_context_t *context) {}
static void project_object_handler_AWAY_SPIRIT(project_object_handler_context_t *context) {}
static void project_object_handler_AWAY_ALL(project_object_handler_context_t *context) {}
static void project_object_handler_TURN_UNDEAD(project_object_handler_context_t *context) {}
static void project_object_handler_TURN_LIVING(project_object_handler_context_t *context) {}
static void project_object_handler_TURN_ALL(project_object_handler_context_t *context) {}
static void project_object_handler_DISP_UNDEAD(project_object_handler_context_t *context) {}
static void project_object_handler_DISP_EVIL(project_object_handler_context_t *context) {}
static void project_object_handler_DISP_ALL(project_object_handler_context_t *context) {}
static void project_object_handler_SLEEP_UNDEAD(project_object_handler_context_t *context) {}
static void project_object_handler_SLEEP_EVIL(project_object_handler_context_t *context) {}
static void project_object_handler_SLEEP_ALL(project_object_handler_context_t *context) {}
static void project_object_handler_MON_CLONE(project_object_handler_context_t *context) {}
static void project_object_handler_MON_POLY(project_object_handler_context_t *context) {}
static void project_object_handler_MON_HEAL(project_object_handler_context_t *context) {}
static void project_object_handler_MON_SPEED(project_object_handler_context_t *context) {}
static void project_object_handler_MON_SLOW(project_object_handler_context_t *context) {}
static void project_object_handler_MON_CONF(project_object_handler_context_t *context) {}
static void project_object_handler_MON_HOLD(project_object_handler_context_t *context) {}
static void project_object_handler_MON_STUN(project_object_handler_context_t *context) {}
static void project_object_handler_MON_DRAIN(project_object_handler_context_t *context) {}
static void project_object_handler_MON_CRUSH(project_object_handler_context_t *context) {}
static void project_object_handler_PSI(project_object_handler_context_t *context) {}
static void project_object_handler_PSI_DRAIN(project_object_handler_context_t *context) {}
static void project_object_handler_CURSE(project_object_handler_context_t *context) {}
static void project_object_handler_CURSE2(project_object_handler_context_t *context) {}
static void project_object_handler_DRAIN(project_object_handler_context_t *context) {}
static void project_object_handler_COMMAND(project_object_handler_context_t *context) {}
static void project_object_handler_TELE_TO(project_object_handler_context_t *context) {}
static void project_object_handler_TELE_LEVEL(project_object_handler_context_t *context) {}
static void project_object_handler_MON_BLIND(project_object_handler_context_t *context) {}
static void project_object_handler_DRAIN_MANA(project_object_handler_context_t *context) {}
static void project_object_handler_FORGET(project_object_handler_context_t *context) {}
static void project_object_handler_BLAST(project_object_handler_context_t *context) {}
static void project_object_handler_SMASH(project_object_handler_context_t *context) {}
static void project_object_handler_CONTROL(project_object_handler_context_t *context) {}
static void project_object_handler_PROJECT(project_object_handler_context_t *context) {}
static void project_object_handler_TREES(project_object_handler_context_t *context) {}
static void project_object_handler_AWAY_ANIMAL(project_object_handler_context_t *context) {}


static const project_object_handler_f object_handlers[] =
{
    #define ELEM(a, b, c, d) project_object_handler_##a,
    #include "../common/list-elements.h"
    #undef ELEM
    #define PROJ(a) project_object_handler_##a,
    #include "../common/list-projections.h"
    #undef PROJ
    NULL
};


/*
 * Called from project() to affect objects
 *
 * Called for projections with the PROJECT_ITEM flag set, which includes
 * beam, ball and breath effects.
 *
 * origin is the origin of the effect
 * r is the distance from the centre of the effect
 * c is the current cave
 * (y, x) the coordinates of the grid being handled
 * dam is the "damage" from the effect at distance r from the centre
 * typ is the projection (PROJ_) type
 *
 * Returns whether the effects were obvious
 *
 * Note that this function determines if the player can see anything that
 * happens by taking into account: blindness, line-of-sight, and illumination.
 */
bool project_o(struct source *origin, int r, struct chunk *c, struct loc *grid, int dam, int typ)
{
    struct object *obj = square_object(c, grid), *next;
    bool obvious = false;

    /* Scan all objects in the grid */
    while (obj)
    {
        bool ignore = false;
        bool do_kill = false;
        const char *note_kill = NULL;
        project_object_handler_context_t context;
        project_object_handler_f object_handler;
        bool is_art = false;
        bool observed = false;

        next = obj->next;

        if (origin->player)
            observed = (square_isseen(origin->player, grid) && !ignore_item_ok(origin->player, obj));

        /* Check for artifact */
        if (obj->artifact) is_art = true;

        context.origin = origin;
        context.r = r;
        context.cave = c;
        loc_copy(&context.grid, grid);
        context.dam = dam;
        context.type = typ;
        context.obj = obj;
        context.observed = observed;
        context.obvious = obvious;
        context.do_kill = do_kill;
        context.ignore = ignore;
        context.note_kill = note_kill;

        object_handler = object_handlers[typ];

        if (object_handler != NULL) object_handler(&context);

        obvious = context.obvious;
        do_kill = context.do_kill;
        ignore = context.ignore;
        note_kill = context.note_kill;

        /* Attempt to destroy the object */
        if (do_kill)
        {
            char o_name[NORMAL_WID];

            /* Effect observed */
            if (observed)
            {
                obvious = true;
                object_desc(origin->player, o_name, sizeof(o_name), obj, ODESC_BASE);
            }

            /* Artifacts, and other objects, get to resist */
            if (is_art || ignore)
            {
                /* Observe the resist */
                if (observed)
                {
                    msg(origin->player, "The %s %s unaffected!", o_name,
                        VERB_AGREEMENT(obj->number, "is", "are"));
                    obj->known->el_info[typ].flags |= EL_INFO_IGNORE;
                }
            }

            /* Reveal mimics */
            else if (obj->mimicking_m_idx)
                become_aware(origin->player, c, cave_monster(c, obj->mimicking_m_idx));

            /* Kill it */
            else
            {
                /* Describe if needed */
                if (observed && note_kill)
                    msgt(origin->player, MSG_DESTROY, "The %s %s!", o_name, note_kill);

                /* Delete the object */
                square_excise_object(c, grid, obj);
                object_delete(&obj);

                /* Redraw */
                square_note_spot(c, grid);
                square_light_spot(c, grid);
            }
        }

        /* Next object */
        obj = next;
    }

    /* Return "Anything seen?" */
    return (obvious);
}
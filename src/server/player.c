/*
 * File: player.c
 * Purpose: Player implementation
 *
 * Copyright (c) 2011 elly+angband@leptoquark.net. See COPYING.
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


static const char *stat_name_list[] =
{
    #define STAT(a) #a,
    #include "../common/list-stats.h"
    #undef STAT
    NULL
};


int stat_name_to_idx(const char *name)
{
    int i;

    for (i = 0; stat_name_list[i]; i++)
    {
        if (!my_stricmp(name, stat_name_list[i])) return i;
    }

    return -1;
}


const char *stat_idx_to_name(int type)
{
    my_assert(type >= 0);
    my_assert(type < STAT_MAX);

    return stat_name_list[type];
}


/*
 * Increases a stat
 */
bool player_stat_inc(struct player *p, int stat)
{
    int v = p->stat_cur[stat];

    /* Cannot go above 18/100 */
    if (v >= 18+100) return false;

    /* Increase linearly */
    if (v < 18) p->stat_cur[stat]++;
    else if (v < 18+90)
    {
        int gain = (((18+100) - v) / 2 + 3) / 2;

        /* Paranoia */
        if (gain < 1) gain = 1;

        /* Apply the bonus */
        p->stat_cur[stat] += randint1(gain) + gain / 2;

        /* Maximal value */
        if (p->stat_cur[stat] > 18+99) p->stat_cur[stat] = 18+99;
    }
    else p->stat_cur[stat] = 18+100;

    /* Bring up the maximum too */
    if (p->stat_cur[stat] > p->stat_max[stat]) p->stat_max[stat] = p->stat_cur[stat];

    /* Recalculate bonuses */
    p->upkeep->update |= (PU_BONUS);

    return true;
}


/*
 * Decreases a stat
 */
bool player_stat_dec(struct player *p, int stat, bool permanent)
{
    int cur, max, res;

    cur = p->stat_cur[stat];
    max = p->stat_max[stat];

    /* Damage "current" value */
    if (cur > 18+10) cur -= 10;
    else if (cur > 18) cur = 18;
    else if (cur > 3) cur -= 1;

    res = (cur != p->stat_cur[stat]);

    /* Damage "max" value */
    if (permanent)
    {
        if (max > 18+10) max -= 10;
        else if (max > 18) max = 18;
        else if (max > 3) max -= 1;

        res = (max != p->stat_max[stat]);
    }

    /* Apply changes */
    if (res)
    {
        p->stat_cur[stat] = cur;
        p->stat_max[stat] = max;
        p->upkeep->update |= (PU_BONUS);
        p->upkeep->redraw |= (PR_STATS);
    }

    return res;
}


/*
 * Advance experience levels and print experience
 */
static void adjust_level(struct player *p)
{
    char buf[NORMAL_WID];
    bool redraw = false;

    /* Hack -- lower limit */
    if (p->exp < 0) p->exp = 0;

    /* Hack -- lower limit */
    if (p->max_exp < 0) p->max_exp = 0;

    /* Hack -- upper limit */
    if (p->exp > PY_MAX_EXP) p->exp = PY_MAX_EXP;

    /* Hack -- upper limit */
    if (p->max_exp > PY_MAX_EXP) p->max_exp = PY_MAX_EXP;

    /* Hack -- maintain "max" experience */
    if (p->exp > p->max_exp) p->max_exp = p->exp;

    /* Redraw experience */
    p->upkeep->redraw |= (PR_EXP);

    /* Handle stuff */
    handle_stuff(p);

    /* Lose levels while possible */
    while ((p->lev > 1) && (p->exp < adv_exp(p->lev - 1, p->expfact)))
    {
        /* Lose a level */
        p->lev--;

        /* Dragon */
        if (player_has(p, PF_DRAGON)) poly_dragon(p, true);

        /* Redraw */
        redraw = true;
    }

    /* Gain levels while possible */
    while ((p->lev < PY_MAX_LEVEL) && (p->exp >= adv_exp(p->lev, p->expfact)))
    {
        /* Gain a level */
        p->lev++;

        /* Dragon */
        if (player_has(p, PF_DRAGON)) poly_dragon(p, true);

        /* Save the highest level */
        if (p->lev > p->max_lev)
        {
            struct source who_body;
            struct source *who = &who_body;

            p->max_lev = p->lev;

            /* Message */
            msgt(p, MSG_LEVEL, "Welcome to level %d.", p->lev);
            strnfmt(buf, sizeof(buf), "%s has attained level %d.", p->name, p->lev);
            msg_broadcast(p, buf, MSG_BROADCAST_LEVEL);

            /* Restore stats */
            source_player(who, get_player_index(get_connection(p->conn)), p);
            effect_simple(EF_RESTORE_STAT, who, "0", STAT_STR, 0, 0, 0, 0, NULL);
            effect_simple(EF_RESTORE_STAT, who, "0", STAT_INT, 0, 0, 0, 0, NULL);
            effect_simple(EF_RESTORE_STAT, who, "0", STAT_WIS, 0, 0, 0, 0, NULL);
            effect_simple(EF_RESTORE_STAT, who, "0", STAT_DEX, 0, 0, 0, 0, NULL);
            effect_simple(EF_RESTORE_STAT, who, "0", STAT_CON, 0, 0, 0, 0, NULL);

            /* Record this event in the character history */
            if (!(p->lev % 5))
            {
                strnfmt(buf, sizeof(buf), "Reached level %d", p->lev);
                history_add_unique(p, buf, HIST_GAIN_LEVEL);
            }
        }

        /* Redraw */
        redraw = true;
    }

    /* Redraw - Do it only once to avoid socket buffer overflow */
    if (redraw)
    {
        /* Update some stuff */
        p->upkeep->update |= (PU_BONUS | PU_SPELLS | PU_MONSTERS);

        /* Redraw some stuff */
        p->upkeep->redraw |= (PR_LEV | PR_TITLE | PR_EXP | PR_STATS | PR_EQUIP | PR_SPELL | PR_PLUSSES);
    }

    /* Handle stuff */
    handle_stuff(p);
}


/*
 * Gain experience
 */
void player_exp_gain(struct player *p, s32b amount)
{
    /* Gain some experience */
    p->exp += amount;

    /* Slowly recover from experience drainage */
    if (p->exp < p->max_exp)
    {
        /* Gain max experience (10%) */
        p->max_exp += amount / 10;
    }

    /* Adjust experience levels */
    adjust_level(p);
}


/*
 * Lose experience
 */
void player_exp_lose(struct player *p, s32b amount, bool permanent)
{
    /* Never drop below zero experience */
    if (amount > p->exp) amount = p->exp;

    /* Lose some experience */
    p->exp -= amount;
    if (permanent) p->max_exp -= amount;

    /* Adjust experience levels */
    adjust_level(p);
}


/*
 * Obtain the "flags" for the player as if he was an item
 */
void player_flags(struct player *p, bitflag f[OF_SIZE])
{
    /* Clear */
    of_wipe(f);

    /* Add racial flags */
    of_copy(f, p->race->flags);
    of_union(f, p->clazz->flags);

    /* Some classes become immune to fear at a certain plevel */
    if (player_has(p, PF_BRAVERY_30) && (p->lev >= 30))
        of_on(f, OF_PROT_FEAR);

    /* Ent */
    if (player_has(p, PF_GIANT))
    {
        if (p->lev >= 5) of_on(f, OF_SEE_INVIS);
        if (p->lev >= 10) of_on(f, OF_ESP_ANIMAL);
        if (p->lev >= 15) of_on(f, OF_ESP_ORC);
        if (p->lev >= 20) of_on(f, OF_ESP_TROLL);
        if (p->lev >= 25) of_on(f, OF_ESP_GIANT);
        if (p->lev >= 30) of_on(f, OF_ESP_DRAGON);
        if (p->lev >= 35) of_on(f, OF_ESP_DEMON);
        if (p->lev >= 40) of_on(f, OF_ESP_UNDEAD);
        if (p->lev >= 45) of_on(f, OF_ESP_EVIL);
        if (p->lev == PY_MAX_LEVEL) of_on(f, OF_ESP_RADIUS);
    }

    /* Thunderlord */
    if (player_has(p, PF_THUNDERLORD))
    {
        if (p->lev >= 5) of_on(f, OF_ESP_DRAGON);
    }

    /* Unencumbered monks get nice abilities */
    if (monk_armor_ok(p))
    {
        /* Levitation at level 10 */
        if (p->lev >= 10) of_on(f, OF_FEATHER);

        /* Fear resistance at level 15 */
        if (p->lev >= 15) of_on(f, OF_PROT_FEAR);

        /* Confusion resistance at level 20 */
        if (p->lev >= 20) of_on(f, OF_PROT_CONF);

        /* Free action at level 25 */
        if (p->lev >= 25) of_on(f, OF_FREE_ACT);
    }

    /* Ghost */
    if (p->ghost)
    {
        of_on(f, OF_SEE_INVIS);
        of_on(f, OF_HOLD_LIFE);
        of_on(f, OF_FREE_ACT);
        of_on(f, OF_PROT_FEAR);

        /* PWMAngband */
        of_on(f, OF_PROT_BLIND);
        of_on(f, OF_PROT_CONF);
        of_on(f, OF_PROT_STUN);
        of_on(f, OF_FEATHER);
        of_on(f, OF_SUST_STR);
        of_on(f, OF_SUST_INT);
        of_on(f, OF_SUST_WIS);
        of_on(f, OF_SUST_DEX);
        of_on(f, OF_SUST_CON);
    }

    /* Handle polymorphed players */
    if (p->poly_race)
    {
        int m;

        for (m = 0; m < z_info->mon_blows_max; m++)
        {
            /* Skip non-attacks */
            if (!p->poly_race->blow[m].method) continue;

            if (streq(p->poly_race->blow[m].effect->name, "EXP_10")) of_on(f, OF_HOLD_LIFE);
            if (streq(p->poly_race->blow[m].effect->name, "EXP_20")) of_on(f, OF_HOLD_LIFE);
            if (streq(p->poly_race->blow[m].effect->name, "EXP_40")) of_on(f, OF_HOLD_LIFE);
            if (streq(p->poly_race->blow[m].effect->name, "EXP_80")) of_on(f, OF_HOLD_LIFE);
        }

        if (rf_has(p->poly_race->flags, RF_REGENERATE)) of_on(f, OF_REGEN);
        if (rf_has(p->poly_race->flags, RF_IM_NETHER)) of_on(f, OF_HOLD_LIFE);
        if (rf_has(p->poly_race->flags, RF_IM_WATER))
        {
            of_on(f, OF_PROT_CONF);
            of_on(f, OF_PROT_STUN);
        }
        if (rf_has(p->poly_race->flags, RF_IM_PLASMA)) of_on(f, OF_PROT_STUN);
        if (rf_has(p->poly_race->flags, RF_NO_FEAR)) of_on(f, OF_PROT_FEAR);
        if (rf_has(p->poly_race->flags, RF_NO_STUN)) of_on(f, OF_PROT_STUN);
        if (rf_has(p->poly_race->flags, RF_NO_CONF)) of_on(f, OF_PROT_CONF);
        if (rf_has(p->poly_race->flags, RF_NO_SLEEP)) of_on(f, OF_FREE_ACT);
        if (rsf_has(p->poly_race->spell_flags, RSF_BR_NETH)) of_on(f, OF_HOLD_LIFE);
        if (rsf_has(p->poly_race->spell_flags, RSF_BR_LIGHT)) of_on(f, OF_PROT_BLIND);
        if (rsf_has(p->poly_race->spell_flags, RSF_BR_DARK)) of_on(f, OF_PROT_BLIND);
        if (rsf_has(p->poly_race->spell_flags, RSF_BR_SOUN)) of_on(f, OF_PROT_STUN);
        if (rsf_has(p->poly_race->spell_flags, RSF_BR_CHAO)) of_on(f, OF_PROT_CONF);
        if (rsf_has(p->poly_race->spell_flags, RSF_BR_INER)) of_on(f, OF_FREE_ACT);
        if (rsf_has(p->poly_race->spell_flags, RSF_BR_GRAV))
        {
            of_on(f, OF_FEATHER);
            of_on(f, OF_PROT_STUN);
        }
        if (rsf_has(p->poly_race->spell_flags, RSF_BR_PLAS)) of_on(f, OF_PROT_STUN);
        if (rsf_has(p->poly_race->spell_flags, RSF_BR_WALL)) of_on(f, OF_PROT_STUN);
        if (rsf_has(p->poly_race->spell_flags, RSF_BR_WATE))
        {
            of_on(f, OF_PROT_CONF);
            of_on(f, OF_PROT_STUN);
        }
    }
}


/*
 * Number of connected players
 */
int NumPlayers;


/*
 * An array for player structures
 *
 * Player index is in [1..NumPlayers]
 */
static struct player **Players;


void init_players(void)
{
    Players = mem_zalloc(MAX_PLAYERS * sizeof(struct player*));
}


void free_players(void)
{
    mem_free(Players);
}


struct player *player_get(int id)
{
    return (((id > 0) && (id < MAX_PLAYERS))? Players[id]: NULL);
}


void player_set(int id, struct player *p)
{
    if ((id > 0) && (id < MAX_PLAYERS)) Players[id] = p;
}


/*
 * Record the original (pre-ghost) cause of death
 */
void player_death_info(struct player *p, const char *died_from)
{
    my_strcpy(p->death_info.title, get_title(p), sizeof(p->death_info.title));
    p->death_info.max_lev = p->max_lev;
    p->death_info.lev = p->lev;
    p->death_info.max_exp = p->max_exp;
    p->death_info.exp = p->exp;
    p->death_info.au = p->au;
    p->death_info.max_depth = p->max_depth;
    memcpy(&p->death_info.wpos, &p->wpos, sizeof(struct worldpos));
    my_strcpy(p->death_info.died_from, died_from, sizeof(p->death_info.died_from));
    time(&p->death_info.time);
    my_strcpy(p->death_info.ctime, ctime(&p->death_info.time), sizeof(p->death_info.ctime));
}


/*
 * Return a version of the player's name safe for use in filesystems.
 */
void player_safe_name(char *safe, size_t safelen, const char *name)
{
    size_t i;
    size_t limit = 0;

    if (name) limit = strlen(name);

    /* Limit to maximum size of safename buffer */
    limit = MIN(limit, safelen);

    for (i = 0; i < limit; i++)
    {
        char c = name[i];

        /* Convert all non-alphanumeric symbols */
        if (!isalpha((unsigned char)c) && !isdigit((unsigned char)c)) c = '_';

        /* Build "base_name" */
        safe[i] = c;
    }

    /* Terminate */
    safe[i] = '\0';

    /* Require a "base" name */
    if (!safe[0]) my_strcpy(safe, "PLAYER", safelen);
}


void player_cave_new(struct player *p, int height, int width)
{
    struct loc grid;

    if (p->cave->allocated) player_cave_free(p);

    p->cave->height = height;
    p->cave->width = width;

    p->cave->squares = mem_zalloc(p->cave->height * sizeof(struct player_square*));
    p->cave->noise.grids = mem_zalloc(p->cave->height * sizeof(u16b*));
    p->cave->scent.grids = mem_zalloc(p->cave->height * sizeof(u16b*));
    for (grid.y = 0; grid.y < p->cave->height; grid.y++)
    {
        p->cave->squares[grid.y] = mem_zalloc(p->cave->width * sizeof(struct player_square));
        for (grid.x = 0; grid.x < p->cave->width; grid.x++)
            square_p(p, &grid)->info = mem_zalloc(SQUARE_SIZE * sizeof(bitflag));
        p->cave->noise.grids[grid.y] = mem_zalloc(p->cave->width * sizeof(u16b));
        p->cave->scent.grids[grid.y] = mem_zalloc(p->cave->width * sizeof(u16b));
    }
    p->cave->allocated = true;
}


/*
 * Initialize player struct
 */
void init_player(struct player *p, int conn, bool old_history, bool no_recall)
{
    int i;
    char history[N_HIST_LINES][N_HIST_WRAP];
    connection_t *connp = get_connection(conn);

    /* Free player structure */
    cleanup_player(p);

    /* Wipe the player */
    if (old_history) memcpy(history, p->history, N_HIST_LINES * N_HIST_WRAP);
    memset(p, 0, sizeof(struct player));
    if (old_history) memcpy(p->history, history, N_HIST_LINES * N_HIST_WRAP);

    p->scr_info = mem_zalloc((z_info->dungeon_hgt + ROW_MAP + 1) * sizeof(cave_view_type*));
    p->trn_info = mem_zalloc((z_info->dungeon_hgt + ROW_MAP + 1) * sizeof(cave_view_type*));
    for (i = 0; i < z_info->dungeon_hgt + ROW_MAP + 1; i++)
    {
        p->scr_info[i] = mem_zalloc((z_info->dungeon_wid + COL_MAP) * sizeof(cave_view_type));
        p->trn_info[i] = mem_zalloc((z_info->dungeon_wid + COL_MAP) * sizeof(cave_view_type));
    }

    /* Allocate player sub-structs */
    p->upkeep = mem_zalloc(sizeof(struct player_upkeep));
    p->upkeep->inven = mem_zalloc((z_info->pack_size + 1) * sizeof(struct object *));
    p->upkeep->quiver = mem_zalloc(z_info->quiver_size * sizeof(struct object *));
    p->timed = mem_zalloc(TMD_MAX * sizeof(s16b));
    p->obj_k = object_new();
    p->obj_k->brands = mem_zalloc(z_info->brand_max * sizeof(bool));
    p->obj_k->slays = mem_zalloc(z_info->slay_max * sizeof(bool));
    p->obj_k->curses = mem_zalloc(z_info->curse_max * sizeof(struct curse_data));

    /* Allocate memory for lore array */
    p->lore = mem_zalloc(z_info->r_max * sizeof(struct monster_lore));
    for (i = 0; i < z_info->r_max; i++)
    {
        p->lore[i].blows = mem_zalloc(z_info->mon_blows_max * sizeof(byte));
        p->lore[i].blow_known = mem_zalloc(z_info->mon_blows_max * sizeof(bool));
    }
    p->current_lore.blows = mem_zalloc(z_info->mon_blows_max * sizeof(byte));
    p->current_lore.blow_known = mem_zalloc(z_info->mon_blows_max * sizeof(byte));

    /* Allocate memory for artifact array */
    p->art_info = mem_zalloc(z_info->a_max * sizeof(byte));

    /* Allocate memory for randart arrays */
    p->randart_info = mem_zalloc((z_info->a_max + 9) * sizeof(byte));
    p->randart_created = mem_zalloc((z_info->a_max + 9) * sizeof(byte));

    /* Allocate memory for dungeon flags array */
    p->obj_aware = mem_zalloc(z_info->k_max * sizeof(bool));
    p->note_aware = mem_zalloc(z_info->k_max * sizeof(quark_t));
    p->obj_tried = mem_zalloc(z_info->k_max * sizeof(bool));
    p->kind_ignore = mem_zalloc(z_info->k_max * sizeof(byte));
    p->kind_everseen = mem_zalloc(z_info->k_max * sizeof(byte));
    p->ego_ignore_types = mem_zalloc(z_info->e_max * sizeof(byte*));
    for (i = 0; i < z_info->e_max; i++)
        p->ego_ignore_types[i] = mem_zalloc(ITYPE_MAX * sizeof(byte));
    p->ego_everseen = mem_zalloc(z_info->e_max * sizeof(byte));

    /* Allocate memory for visuals */
    p->f_attr = mem_zalloc(z_info->f_max * sizeof(byte_lit));
    p->f_char = mem_zalloc(z_info->f_max * sizeof(char_lit));
    p->t_attr = mem_zalloc(z_info->trap_max * sizeof(byte_lit));
    p->t_char = mem_zalloc(z_info->trap_max * sizeof(char_lit));
    p->k_attr = mem_zalloc(z_info->k_max * sizeof(byte));
    p->k_char = mem_zalloc(z_info->k_max * sizeof(char));
    p->d_attr = mem_zalloc(z_info->k_max * sizeof(byte));
    p->d_char = mem_zalloc(z_info->k_max * sizeof(char));
    p->r_attr = mem_zalloc(z_info->r_max * sizeof(byte));
    p->r_char = mem_zalloc(z_info->r_max * sizeof(char));

    /* Allocate memory for object and monster lists */
    p->mflag = mem_zalloc(z_info->level_monster_max * MFLAG_SIZE * sizeof(bitflag));
    p->mon_det = mem_zalloc(z_info->level_monster_max * sizeof(byte));

    /* Allocate memory for current cave grid info */
    p->cave = mem_zalloc(sizeof(struct player_cave));

    /* Allocate memory for wilderness knowledge */
    p->wild_map = mem_zalloc((2 * radius_wild + 1) * sizeof(byte *));
    for (i = 0; i <= 2 * radius_wild; i++)
        p->wild_map[i] = mem_zalloc((2 * radius_wild + 1) * sizeof(byte));

    /* Allocate memory for home storage */
    p->home = mem_zalloc(sizeof(struct store));
    memcpy(p->home, &stores[store_max - 2], sizeof(struct store));
    p->home->stock = NULL;

    /* Analyze every object */
    for (i = 0; i < z_info->k_max; i++)
    {
        struct object_kind *kind = &k_info[i];

        /* Skip "empty" objects */
        if (!kind->name) continue;

        /* No flavor yields aware */
        if (!kind->flavor) p->obj_aware[i] = true;
    }

    /* Always start with a well fed player */
    p->food = PY_FOOD_FULL - 1;

    /* Assume no feeling */
    p->feeling = -1;

    /* Update the wilderness map */
    if ((cfg_diving_mode > 1) || no_recall)
        wild_set_explored(p, base_wpos());
    else
    {
        wild_set_explored(p, start_wpos());

        /* On "fast" wilderness servers, we also know the location of the base town */
        if (cfg_diving_mode == 1) wild_set_explored(p, base_wpos());
    }

    /* Copy channels pointer */
    p->on_channel = Conn_get_console_channels(conn);

    /* Clear old channels */
    for (i = 0; i < MAX_CHANNELS; i++)
        p->on_channel[i] = 0;

    /* Listen on the default chat channel */
    p->on_channel[0] |= UCM_EAR;

    /* Copy his connection info */
    p->conn = conn;

    /* Default to the first race/class in the edit file */
    p->race = player_id2race(0);
    p->clazz = player_id2class(0);

    monmsg_init(p);
    monster_list_init(p);
    object_list_init(p);

    /* Initialize extra parameters */
    for (i = ITYPE_NONE; i < ITYPE_MAX; i++) p->opts.ignore_lvl[i] = IGNORE_BAD;

    for (i = 0; i < z_info->k_max; i++)
        add_autoinscription(p, i, connp->Client_setup.note_aware[i]);
}


/*
 * Free player struct
 */
void cleanup_player(struct player *p)
{
    int i;

    if (!p) return;

    /* Free the things that are always initialised */
    if (p->obj_k) object_free(p->obj_k);
    mem_free(p->timed);
    if (p->upkeep)
    {
        mem_free(p->upkeep->inven);
        mem_free(p->upkeep->quiver);
    }
    mem_free(p->upkeep);
    p->upkeep = NULL;

    /* Free the things that are only sometimes initialised */
    player_spells_free(p);
    object_pile_free(p->gear);
    mem_free(p->body.slots);

    /* Stop all file perusal and interactivity */
    string_free(p->interactive_file);

    /* PWMAngband */
    for (i = 0; p->scr_info && (i < z_info->dungeon_hgt + ROW_MAP + 1); i++)
    {
        mem_free(p->scr_info[i]);
        mem_free(p->trn_info[i]);
    }
    mem_free(p->scr_info);
    mem_free(p->trn_info);
    for (i = 0; i < N_HISTORY_FLAGS; i++)
        mem_free(p->hist_flags[i]);
    for (i = 0; p->lore && (i < z_info->r_max); i++)
    {
        mem_free(p->lore[i].blows);
        mem_free(p->lore[i].blow_known);
    }
    mem_free(p->lore);
    mem_free(p->current_lore.blows);
    mem_free(p->current_lore.blow_known);
    mem_free(p->art_info);
    mem_free(p->randart_info);
    mem_free(p->randart_created);
    mem_free(p->obj_aware);
    mem_free(p->note_aware);
    mem_free(p->obj_tried);
    mem_free(p->kind_ignore);
    mem_free(p->kind_everseen);
    for (i = 0; p->ego_ignore_types && (i < z_info->e_max); i++)
        mem_free(p->ego_ignore_types[i]);
    mem_free(p->ego_ignore_types);
    mem_free(p->ego_everseen);
    mem_free(p->f_attr);
    mem_free(p->f_char);
    mem_free(p->t_attr);
    mem_free(p->t_char);
    mem_free(p->k_attr);
    mem_free(p->k_char);
    mem_free(p->d_attr);
    mem_free(p->d_char);
    mem_free(p->r_attr);
    mem_free(p->r_char);
    mem_free(p->mflag);
    mem_free(p->mon_det);
    for (i = 0; p->wild_map && (i <= 2 * radius_wild); i++)
        mem_free(p->wild_map[i]);
    mem_free(p->wild_map);
    if (p->home) object_pile_free(p->home->stock);
    mem_free(p->home);

    /* Free the history */
    history_clear(p);

    /* Free the cave */
    if (p->cave)
    {
        player_cave_free(p);
        mem_free(p->cave);
    }

    monmsg_cleanup(p);
    monster_list_finalize(p);
    object_list_finalize(p);
}


void player_cave_free(struct player *p)
{
    struct loc grid;

    if (!p->cave->allocated) return;

    for (grid.y = 0; grid.y < p->cave->height; grid.y++)
    {
        for (grid.x = 0; grid.x < p->cave->width; grid.x++)
        {
            mem_free(square_p(p, &grid)->info);
            square_forget_pile(p, &grid);
            square_forget_trap(p, &grid);
        }
        mem_free(p->cave->squares[grid.y]);
        mem_free(p->cave->noise.grids[grid.y]);
        mem_free(p->cave->scent.grids[grid.y]);
    }
    mem_free(p->cave->squares);
    mem_free(p->cave->noise.grids);
    mem_free(p->cave->scent.grids);
    p->cave->allocated = false;
}


/*
 * Clear the flags for each cave grid
 */
void player_cave_clear(struct player *p, bool full)
{
    struct loc begin, end;
    struct loc_iterator iter;

    /* Assume no feeling */
    if (full) p->feeling = -1;

    /* Reset number of feeling squares */
    if (full) p->cave->feeling_squares = 0;

    loc_init(&begin, 0, 0);
    loc_init(&end, p->cave->width, p->cave->height);
    loc_iterator_first(&iter, &begin, &end);

    /* Clear flags and flow information. */
    do
    {
        /* Erase feat */
        square_forget(p, &iter.cur);

        /* Erase object */
        square_forget_pile(p, &iter.cur);

        /* Erase trap */
        square_forget_trap(p, &iter.cur);

        /* Erase flags */
        if (full)
            sqinfo_wipe(square_p(p, &iter.cur)->info);
        else
        {
            /* Erase flags (no bounds checking) */
            sqinfo_off(square_p(p, &iter.cur)->info, SQUARE_SEEN);
            sqinfo_off(square_p(p, &iter.cur)->info, SQUARE_VIEW);
            sqinfo_off(square_p(p, &iter.cur)->info, SQUARE_DTRAP);
        }

        /* Erase flow */
        if (full)
        {
            p->cave->noise.grids[iter.cur.y][iter.cur.x] = 0;
            p->cave->scent.grids[iter.cur.y][iter.cur.x] = 0;
        }
    }
    while (loc_iterator_next_strict(&iter));

    /* Memorize the content of owned houses */
    memorize_houses(p);
}


bool player_square_in_bounds(struct player *p, struct loc *grid)
{
    return ((grid->x >= 0) && (grid->x < p->cave->width) &&
        (grid->y >= 0) && (grid->y < p->cave->height));
}


bool player_square_in_bounds_fully(struct player *p, struct loc *grid)
{
    return ((grid->x > 0) && (grid->x < p->cave->width - 1) &&
        (grid->y > 0) && (grid->y < p->cave->height - 1));
}
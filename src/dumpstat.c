/*---------------------------------------------------------------------------
 * Gamedriver - Dump object statistics.
 *
 *---------------------------------------------------------------------------
 * Function to compute the memory usage of objects and dump their
 * statistics.
 *---------------------------------------------------------------------------
 */

#include "driver.h"
#include "typedefs.h"

#include <stdio.h>

#include "dumpstat.h"
#include "array.h"
#include "closure.h"
#include "exec.h"
#include "filestat.h"
#include "mapping.h"
#include "mstrings.h"
#include "object.h"
#include "ptrtable.h"
#include "simulate.h"
#include "stdstrings.h"
#include "smalloc.h" /* malloced_size() */
#include "svalue.h"

/*-------------------------------------------------------------------------*/

static size_t svalue_size(svalue_t *, mp_int *); /* forward */

/* Auxiliary structure for counting a mapping */

struct svalue_size_locals
{
    mp_uint composite;   /* Return: total composite memory usage of all elmts */
    mp_uint total;       /* Return: total memory usage of all elmts */
    int     num_values;  /* Passed: width of the mapping */
};

/*-------------------------------------------------------------------------*/

static struct pointer_table *ptable;
  /* The pointer_table to register all arrays and mappings.
   */

/*-------------------------------------------------------------------------*/
static void
svalue_size_map_filter (svalue_t *key, svalue_t *values, void *extra)

/* Called for all keys in a mapping, it counts the size of the values
 * and returns the total in extra->num_values.
 */

{
    struct svalue_size_locals *locals;
    mp_int total;
    int i;

    locals = (struct svalue_size_locals*)extra;
    locals->composite += svalue_size(key, &total);
    locals->total += total;
    for(i = locals->num_values; --i >= 0; )
    {
        locals->composite += svalue_size(values++, &total) + sizeof(svalue_t);
        locals->total += total + sizeof(svalue_t);
    }
}

/*-------------------------------------------------------------------------*/
static size_t
svalue_size (svalue_t *v, mp_int * pTotal)

/* Compute the memory usage of *<v> (modified to reflect data sharing),
 * calling svalue_size() recursively if necessary, and return it.
 * The size of *v itself is not included.
 * *<pUnshared> and *<pShared> are set to the total unshared and shared
 * datasize.
 */

{
    mp_int i, composite, total, overhead;

    *pTotal = 0;

    total = overhead = composite = 0;

    switch(v->type)
    {
    case T_OBJECT:
    case T_NUMBER:
    case T_FLOAT:
        return 0;

    case T_STRING:
    case T_SYMBOL:
        *pTotal = mstr_mem_size(v->u.str);
        return *pTotal / v->u.str->info.ref;

    case T_MAPPING:
    {
        struct svalue_size_locals locals;

        if (NULL == register_pointer(ptable, v->u.map) ) return 0;

        overhead = (mp_uint)mapping_overhead(v->u.map);
        locals.total = 0;
        locals.composite = 0;
        locals.num_values = v->u.map->num_values;
        walk_mapping(v->u.map, svalue_size_map_filter, &locals);
    
        *pTotal = locals.total + overhead;
        return (overhead + locals.composite) / v->u.map->ref;
    }

    case T_POINTER:
    case T_QUOTED_ARRAY:
    {
        if (v->u.vec == &null_vector) return 0;
        if (NULL == register_pointer(ptable, v->u.vec) ) return 0;
#ifdef MALLOC_smalloc
        overhead = malloced_size(v->u.vec) * sizeof(p_int);
#else
        overhead = sizeof *v->u.vec - sizeof v->u.vec->item +
          sizeof(svalue_t) * v->u.vec->size + sizeof(char *);
#endif
        for (i=0; i < (mp_int)VEC_SIZE(v->u.vec); i++) {
            composite += svalue_size(&v->u.vec->item[i], &total);
            *pTotal += total;
        }
        *pTotal += overhead;
        return (overhead + composite) / v->u.vec->ref;
    }

    case T_CLOSURE:
    {
        int num_values;
        svalue_t *svp;
        lambda_t *l;

        if (!CLOSURE_MALLOCED(v->x.closure_type)) return 0;
        if (!CLOSURE_REFERENCES_CODE(v->x.closure_type))
        {
            /* CLOSURE_LFUN || CLOSURE_IDENTIFIER || CLOSURE_PRELIMINARY */
            composite = sizeof *v->u.lambda + sizeof(char *);
            *pTotal = composite;
            return composite / v->u.lambda->ref;
        }
        /* CLOSURE_LAMBDA */
        composite = overhead = 0;
        l = v->u.lambda;
        if (v->x.closure_type == CLOSURE_BOUND_LAMBDA)
        {
            total = sizeof *l - sizeof l->function + sizeof l->function.lambda;
            *pTotal += total;
            composite += total / l->ref;
            l = l->function.lambda;
        }
        num_values = EXTRACT_UCHAR(&l->function.code[0]);
        if (num_values == 0xff)
            num_values = ((svalue_t *)l)[-0xff].u.number;
        svp = (svalue_t *)l - num_values;
        if (NULL == register_pointer(ptable, svp)) return 0;
#ifdef MALLOC_smalloc
        overhead = malloced_size(svp) * sizeof(p_int);
#else
        overhead = sizeof(svalue_t) * num_values + sizeof (char *);
        {
            bytecode_p p = &l->function.code[2];
            do {
                ++p;
                switch(GET_CODE(p)) {
                  case F_RETURN:
                  case F_RETURN0:
                    break;
                  default:
                    continue;
                }
                break;
            } while (1);
            overhead += p - (bytecode_p)l
                     + (sizeof(bytecode_p) - 1) & ~(sizeof(bytecode_p) - 1);
        }
#endif

        while (--num_values >= 0)
        {
            composite += svalue_size(svp++, &total);
            *pTotal += total;
        }

        *pTotal += overhead;
        return (overhead + composite) / l->ref;
    }

    default:
        fatal("Illegal type: %d\n", v->type);
    }

    /*NOTREACHED*/
    return 0;
}

/*-------------------------------------------------------------------------*/
mp_int
data_size (object_t *ob, mp_int * pTotal)

/* Compute the memory usage (modified to reflect shared data use)
 * of the data held by object <ob> and return it.
 * If the object is swapped out or has no variables, return 0.
 *
 * If <pTotal> is given, *<pTotal> is set to the raw datasize.
 */

{
    mp_int total = sizeof(p_int); /* smalloc overhead */
    int i;
    svalue_t *svp;

    if (pTotal != NULL)
        *pTotal = 0;

    if (ob->flags & O_SWAPPED || !(i = ob->prog->num_variables) )
        return 0;
    ptable = new_pointer_table();
    if (!ptable)
        error("(dumpstat) Out of memory for new pointer table.\n");
    for (svp = ob->variables; --i >= 0; svp++)
    {
        mp_int tmp;
        total += svalue_size(svp, &tmp) + sizeof (svalue_t);
        if (pTotal != NULL)
            *pTotal += tmp + sizeof(svalue_t);
    }
    free_pointer_table(ptable);
    return total;
} /* data_size() */

/*-------------------------------------------------------------------------*/
mp_int
program_string_size (program_t *prog, mp_int * pOverhead, mp_int * pData)

/* Compute the composite data size of all strings in program <prog>
 * which must be swapped in.
 * Set *<pOverhead> to the size of the overhead in the program structure,
 * and *<pData> to the raw data size of the strings.
 */

{
    int i;
    mp_int rc, data;

    rc = data = 0;

    for (i = prog->num_strings; i--; )
    {
        string_t * str = prog->strings[i];
        mp_int size;

        size = mstr_mem_size(str);
        data += size;
        rc += size / str->info.ref;
    }

    *pOverhead = prog->num_strings * sizeof(char *);
    *pData = data;
    return rc;
} /* program_string_size() */

/*-------------------------------------------------------------------------*/
Bool
dumpstat (string_t *fname)

/* Called by the command parser, this function dumps statistics about
 * all objects into the file $MUDLIB/<fname>.
 * Return TRUE on success, FALSE if <fname> can't be written.
 */

{
    FILE *f;
    object_t *ob;

    static char *swapstrings[] =
        {"", "PROG SWAPPED", "VAR SWAPPED", "SWAPPED", };

    fname = check_valid_path(fname, current_object, STR_OBJDUMP, MY_TRUE);
    if (!fname)
        return MY_FALSE;
    f = fopen(get_txt(fname), "w");
    if (!f)
        return MY_FALSE;
    FCOUNT_WRITE(get_txt(fname));

    for (ob = obj_list; ob; ob = ob->next_all)
    {
        mp_int compsize, totalsize, overhead;

#ifdef DEBUG
        if (ob->flags & O_DESTRUCTED) /* TODO: Can't happen */
            continue;
#endif
        compsize = data_size(ob, &totalsize);

        if (!O_PROG_SWAPPED(ob)
         && (ob->prog->ref == 1 || !(ob->flags & (O_CLONE|O_REPLACED))))
        {
             overhead = ob->prog->total_size;
        }
        else
        {
            overhead = 0;
        }

        overhead += sizeof (object_t);

        fprintf(f, "%-20s %5ld (%5ld) ref %2ld %s "
                 , get_txt(ob->name)
                 , compsize + overhead, totalsize + overhead
                 , ob->ref
                 , ob->flags & O_HEART_BEAT ? "HB" : "  "
        );
        if (ob->super)
            fprintf(f, "%s ", get_txt(ob->super->name));
        else
            fprintf(f, "-- ");

        if (ob->gigaticks)
            fprintf(f, " (%lu%09lu)", ob->gigaticks, ob->ticks);
        else
            fprintf(f, " (%lu)", ob->ticks);
        fprintf(f, " %s\n",
                swapstrings[(O_PROG_SWAPPED(ob)?1:0) | (O_VAR_SWAPPED(ob)?2:0)]
        );
    }
    fclose(f);
    return MY_TRUE;
}

/***************************************************************************/


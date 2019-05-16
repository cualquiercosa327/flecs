#include <assert.h>
#include "include/private/flecs.h"

/** Notify systems that a table has changed its active state */
static
void activate_table(
    ecs_world_t *world,
    ecs_table_t *table,
    ecs_entity_t system,
    bool activate)
{
    if (system) {
        ecs_system_activate_table(world, system, table, activate);
    } else {
        ecs_vector_t *systems = table->frame_systems;
        
        if (systems) {
            ecs_entity_t *buffer = ecs_vector_first(systems);
            uint32_t i, count = ecs_vector_count(systems);
            for (i = 0; i < count; i ++) {
                ecs_system_activate_table(world, buffer[i], table, activate);
            }
        }
    }
}

static
void notify_systems_of_realloc(
    ecs_world_t *world,
    ecs_table_t *table)
{
    world->should_resolve = true;
}

static
ecs_table_column_t* new_columns(
    ecs_world_t *world,
    ecs_stage_t *stage,
    ecs_vector_t *type)
{
    ecs_table_column_t *result = ecs_os_calloc(
        sizeof(ecs_table_column_t), ecs_vector_count(type) + 1);

    ecs_assert(result != NULL, ECS_OUT_OF_MEMORY, NULL);

    ecs_entity_t *buf = ecs_vector_first(type);
    uint32_t i, count = ecs_vector_count(type);

    /* First column is reserved for storing entity id's */
    result[0].size = sizeof(ecs_entity_t);
    result[0].data = NULL;

    for (i = 0; i < count; i ++) {
        ecs_entity_info_t info = {.entity = buf[i]};
        EcsComponent *component = ecs_get_ptr_intern(
            world, stage, &info, EEcsComponent, false, false);

        if (component) {
            if (component->size) {
                /* Regular column data */
                result[i + 1].size = component->size;
            }
        }
    }
    
    return result;
}

/* -- Private functions -- */

ecs_table_column_t* ecs_table_get_columns(
    ecs_world_t *world,
    ecs_stage_t *stage,
    ecs_table_t *table)
{
    if (!world->in_progress) {
        return table->columns;
    } else {
        ecs_type_t type_id = table->type_id;
        ecs_table_column_t *columns = ecs_map_get(stage->data_stage, type_id);

        if (!columns) {
            ecs_vector_t *type = table->type;
            columns = new_columns(world, stage, type);
            ecs_map_set(stage->data_stage, type_id, columns);
        }

        return columns;
    }
}

void ecs_table_eval_columns(
    ecs_world_t *world,
    ecs_stage_t *stage,
    ecs_table_t *table)
{
    ecs_vector_t *type = table->type;
    ecs_entity_t *buf = ecs_vector_first(type);
    int32_t i, count = ecs_vector_count(type);

    bool prefab_set = false;
    ecs_entity_t exclude_prefab = 0;

    /* Walk array backwards to properly detect prefab parents. It is
    * guaranteed that a prefab parent flag is created after a prefab
    * parent. Therefore, the id of the flag is guaranteed to be higher than
    * the prefab, which, because components in a type are ordered by id,
    * guarantees that the prefab comes before the flag. Because it is more
    * convenient to know about the flag before the prefab, walk the type
    * backwards, as this way we know immediately whether a prefab should be
    * treated as a regular container, or as an actual prefab - in the
    * latter case we should register the table type in the prefab index. */
    for (i = count - 1; i >= 0; i --) {
        ecs_entity_t c = buf[i];

        ecs_assert(c <= world->last_handle, ECS_INVALID_HANDLE, NULL);

        if (c == ecs_entity(EcsPrefab)) {
            table->flags |= EcsTableIsPrefab;
        }
        
        /* Only if creating columns in the main stage, register prefab */
        if (!ecs_has(world, c, EcsComponent)) {
            if (c != exclude_prefab && ecs_has(world, c, EcsPrefab)) {
                /* Tables can contain at most one prefab */
                ecs_assert(prefab_set == false, ECS_MORE_THAN_ONE_PREFAB, 
                            ecs_get_id(world, c));

                prefab_set = true;

                /* Register type with prefab index for quick lookups */
                ecs_map_set64(world->prefab_index, table->type_id, c);

                table->flags |= EcsTableHasPrefab;
            
            } else if (ecs_has(world, c, EcsPrefabParent)) {
                EcsPrefabParent *pparent = ecs_get_ptr(
                        world, c, EcsPrefabParent);
                exclude_prefab = pparent->parent;
                ecs_assert(exclude_prefab != 0, ECS_INTERNAL_ERROR, NULL);
            }
        }
    }
}

int ecs_table_init(
    ecs_world_t *world,
    ecs_stage_t *stage,
    ecs_table_t *table)
{
    ecs_vector_t *type = ecs_type_get(world, stage, table->type_id);
    ecs_assert(type != NULL, ECS_INTERNAL_ERROR, "invalid type id of table");

    table->frame_systems = NULL;
    table->type = type;
    table->columns = new_columns(world, stage, type);
    table->flags = 0;

    if (stage == &world->main_stage && !world->is_merging) {
        /* If world is merging, column evaluation is delayed and invoked
         * explicitly by the merge process. The reason for this is that the
         * column evaluation may rely on entities to have certain components,
         * which could have been added while in progress and thus need to be 
         * merged first. */ 
        ecs_table_eval_columns(world, stage, table);
    }

    return 0;
}

void ecs_table_deinit(
    ecs_world_t *world,
    ecs_table_t *table)
{
    uint32_t count = ecs_vector_count(table->columns[0].data);
    if (count) {
        ecs_notify(
            world, &world->main_stage, world->type_sys_remove_index, 
            table->type_id, table, table->columns, 0, count);
    }
}

void ecs_table_free(
    ecs_world_t *world,
    ecs_table_t *table)
{
    uint32_t i, column_count = ecs_vector_count(table->type);
    (void)world;
    
    for (i = 0; i < column_count + 1; i ++) {
        ecs_vector_free(table->columns[i].data);
    }

    ecs_os_free(table->columns);

    ecs_vector_free(table->frame_systems);
}

void ecs_table_register_system(
    ecs_world_t *world,
    ecs_table_t *table,
    ecs_entity_t system)
{
    /* Register system with the table */
    ecs_entity_t *h = ecs_vector_add(&table->frame_systems, &handle_arr_params);
    if (h) *h = system;

    if (ecs_vector_count(table->columns[0].data)) {
        activate_table(world, table, system, true);
    }
}

uint32_t ecs_table_insert(
    ecs_world_t *world,
    ecs_table_t *table,
    ecs_table_column_t *columns,
    ecs_entity_t entity)
{
    uint32_t column_count = ecs_vector_count(table->type);

    /* Fist add entity to column with entity ids */
    ecs_entity_t *e = ecs_vector_add(&columns[0].data, &handle_arr_params);
    if (!e) {
        return -1;
    }

    *e = entity;

    /* Add elements to each column array */
    uint32_t i;
    bool reallocd = false;

    for (i = 1; i < column_count + 1; i ++) {
        uint32_t size = columns[i].size;
        if (size) {
            ecs_vector_params_t params = {.element_size = size};
            void *old_vector = columns[i].data;

            if (!ecs_vector_add(&columns[i].data, &params)) {
                return -1;
            }
            
            if (old_vector != columns[i].data) {
                reallocd = true;
            }
        }
    }

    uint32_t index = ecs_vector_count(columns[0].data) - 1;

    if (!world->in_progress && !index) {
        activate_table(world, table, 0, true);
    }

    if (reallocd && table->columns == columns) {
        notify_systems_of_realloc(world, table);
    }

    /* Return index of last added entity */
    return index + 1;
}

void ecs_table_delete(
    ecs_world_t *world,
    ecs_table_t *table,
    int32_t index)
{
    ecs_table_column_t *columns = table->columns;
    ecs_vector_t *entity_column = columns[0].data;
    uint32_t count = ecs_vector_count(entity_column);

    if (index < 0) {
        index *= -1;
    }

    index --;

    ecs_assert(count != 0, ECS_INTERNAL_ERROR, NULL);

    count --;
    
    ecs_assert(index <= count, ECS_INTERNAL_ERROR, NULL);

    uint32_t column_last = ecs_vector_count(table->type) + 1;
    uint32_t i;

    if (index != count) {        
        /* Move last entity in array to index */
        ecs_entity_t *entities = ecs_vector_first(entity_column);
        ecs_entity_t to_move = entities[count];
        entities[index] = to_move;

        for (i = 1; i < column_last; i ++) {
            if (columns[i].size) {
                ecs_vector_params_t params = {.element_size = columns[i].size};
                ecs_vector_remove_index(columns[i].data, &params, index);
            }
        }

        /* Last entity in table is now moved to index of removed entity */
        ecs_row_t row;
        row.type_id = table->type_id;
        row.index = index + 1;
        ecs_map_set64(world->main_stage.entity_index, to_move, ecs_from_row(row));

        /* Decrease size of entity column */
        ecs_vector_remove_last(entity_column);

    /* This is the last entity in the table, just decrease column counts */
    } else {
        ecs_vector_remove_last(entity_column);

        for (i = 1; i < column_last; i ++) {
            if (columns[i].size) {
                ecs_vector_remove_last(columns[i].data);
            }
        }
    }
    
    if (!world->in_progress && !count) {
        activate_table(world, table, 0, false);
    }
}

uint32_t ecs_table_grow(
    ecs_world_t *world,
    ecs_table_t *table,
    ecs_table_column_t *columns,
    uint32_t count,
    ecs_entity_t first_entity)
{
    uint32_t column_count = ecs_vector_count(table->type);

    /* Fist add entity to column with entity ids */
    ecs_entity_t *e = ecs_vector_addn(&columns[0].data, &handle_arr_params, count);
    if (!e) {
        return -1;
    }

    uint32_t i;
    for (i = 0; i < count; i ++) {
        e[i] = first_entity + i;
    }

    bool reallocd = false;

    /* Add elements to each column array */
    for (i = 1; i < column_count + 1; i ++) {
        ecs_vector_params_t params = {.element_size = columns[i].size};
        void *old_vector = columns[i].data;

        if (!ecs_vector_addn(&columns[i].data, &params, count)) {
            return -1;
        }

        if (old_vector != columns[i].data) {
            reallocd = true;
        }
    }

    uint32_t row_count = ecs_vector_count(columns[0].data);
    if (!world->in_progress && row_count == count) {
        activate_table(world, table, 0, true);
    }

    if (reallocd && table->columns == columns) {
        notify_systems_of_realloc(world, table);
    }

    /* Return index of first added entity */
    return row_count - count + 1;
}

int16_t ecs_table_dim(
    ecs_table_t *table,
    uint32_t count)
{
    ecs_table_column_t *columns = table->columns;
    uint32_t column_count = ecs_vector_count(table->type);

    if (!ecs_vector_set_size(&columns[0].data, &handle_arr_params, count)) {
        return -1;
    }

    uint32_t i;
    for (i = 1; i < column_count + 1; i ++) {
        ecs_vector_params_t params = {.element_size = columns[i].size};
        if (!ecs_vector_set_size(&columns[i].data, &params, count)) {
            return -1;
        }
    }

    return 0;
}

uint64_t ecs_table_count(
    ecs_table_t *table)
{
    return ecs_vector_count(table->columns[0].data);
}

uint32_t ecs_table_row_size(
    ecs_table_t *table)
{
    uint32_t i, count = ecs_vector_count(table->type);
    uint32_t size = 0;

    for (i = 0; i < count; i ++) {
        size += table->columns[i].size;
    }

    return size;
}

uint32_t ecs_table_rows_dimensioned(
    ecs_table_t *table)
{
    return ecs_vector_size(table->columns[0].data);
}

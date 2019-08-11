
#include "flecs_private.h"

static
ecs_entity_t components_contains(
    ecs_world_t *world,
    ecs_type_t table_type,
    ecs_type_t type,
    ecs_entity_t *entity_out,
    bool match_all)
{
    uint32_t i, count = ecs_vector_count(table_type);
    ecs_entity_t *array = ecs_vector_first(table_type);

    for (i = 0; i < count; i ++) {
        ecs_entity_t entity = array[i];

        if (entity & ECS_CHILDOF) {
            entity &= ECS_ENTITY_MASK;

            ecs_record_t *row = ecs_get_entity(world, NULL, entity);
            ecs_assert(row != 0, ECS_INTERNAL_ERROR, NULL);

            if (row->table) {
                ecs_entity_t component = ecs_type_contains(
                    world, row->table->type, type, match_all, true);

                if (component != 0) {
                    if (entity_out) *entity_out = entity;
                    return component;
                }
            }
        }
    }

    return 0;
}

/* Get actual entity on which specified component is stored */
ecs_entity_t ecs_get_entity_for_component(
    ecs_world_t *world,
    ecs_entity_t entity,
    ecs_type_t type,
    ecs_entity_t component)
{
    if (entity) {
        ecs_record_t *row = ecs_get_entity(world, NULL, entity);
        ecs_assert(row != NULL, ECS_INTERNAL_ERROR, NULL);
        type = row->table->type;
    }

    ecs_entity_t *array = ecs_vector_first(type);
    uint32_t i, count = ecs_vector_count(type);

    for (i = 0; i < count; i ++) {
        if (array[i] == component) {
            break;
        }
    }

    if (i == count) {
        entity = ecs_find_entity_in_prefabs(world, entity, type, component, 0);
    }

    return entity;
}

/** Add table to system, compute offsets for system components in table rows */
static
void add_table(
    ecs_world_t *world,
    ecs_query_t *query,
    ecs_table_t *table)
{
    ecs_matched_table_t *table_data;
    ecs_type_t table_type = table->type;
    uint32_t column_count = ecs_vector_count(query->sig.columns);

    /* Initially always add table to inactive group. If the system is registered
     * with the table and the table is not empty, the table will send an
     * activate signal to the system. */
    table_data = ecs_vector_add(&query->tables, &matched_table_params);
    table_data->table = table;
    table_data->references = NULL;

    /* Array that contains the system column to table column mapping */
    table_data->columns = ecs_os_malloc(sizeof(uint32_t) * column_count);

    /* Store the components of the matched table. In the case of OR expressions,
     * components may differ per matched table. */
    table_data->components = ecs_os_malloc(sizeof(ecs_entity_t) * column_count);

    /* Walk columns parsed from the system signature */
    ecs_signature_column_t *columns = ecs_vector_first(query->sig.columns);
    uint32_t c, count = ecs_vector_count(query->sig.columns);

    for (c = 0; c < count; c ++) {
        ecs_signature_column_t *column = &columns[c];
        ecs_entity_t entity = 0, component = 0;
        ecs_signature_from_kind_t from = column->from;
        ecs_signature_op_kind_t op = column->op;

        /* NOT operators are converted to EcsFromEmpty */
        ecs_assert(op != EcsOperNot || from == EcsFromEmpty, 
            ECS_INTERNAL_ERROR, NULL);

        /* Column that retrieves data from self or a fixed entity */
        if (from == EcsFromSelf || from == EcsFromEntity || 
            from == EcsFromOwned || from == EcsFromShared) 
        {
            if (op == EcsOperAnd) {
                component = column->is.component;
            } else if (op == EcsOperOptional) {
                component = column->is.component;
            } else if (op == EcsOperOr) {
                component = ecs_type_contains(
                    world, table_type, column->is.type, 
                    false, true);
            }

            if (from == EcsFromEntity) {
                entity = column->source;
            }

        /* Column that just passes a handle to the system (no data) */
        } else if (from == EcsFromEmpty) {
            component = column->is.component;
            table_data->columns[c] = 0;

        /* Column that retrieves data from a dynamic entity */
        } else if (from == EcsFromContainer || from == EcsCascade) {
            if (op == EcsOperAnd ||
                op == EcsOperOptional)
            {
                component = column->is.component;

                ecs_components_contains_component(
                    world, table_type, component, ECS_CHILDOF, &entity);

            } else if (op == EcsOperOr) {
                component = components_contains(
                    world,
                    table_type,
                    column->is.type,
                    &entity,
                    false);
            }

        /* Column that retrieves data from a system */
        } else if (from == EcsFromSystem) {
            if (op == EcsOperAnd) {
                component = column->is.component;
            }

            entity = query->system;
        }

        /* This column does not retrieve data from a static entity (either
         * EcsFromSystem or EcsFromContainer) and is not just a handle */
        if (!entity && from != EcsFromEmpty) {
            if (component) {
                /* Retrieve offset for component */
                table_data->columns[c] = ecs_type_index_of(table->type, component);

                /* If column is found, add one to the index, as column zero in
                 * a table is reserved for entity id's */
                if (table_data->columns[c] != -1) {
                    table_data->columns[c] ++;

                    /* Check if component is a tag. If it is, set table_data to
                     * zero, so that a system won't try to access the data */
                    EcsComponent *data = ecs_get_ptr(
                        world, component, EcsComponent);

                    if (!data || !data->size) {
                        table_data->columns[c] = 0;
                    }
                }
                
                /* ecs_table_column_offset may return -1 if the component comes
                 * from a prefab. If so, the component will be resolved as a
                 * reference (see below) */
            }
        }

        if (op == EcsOperOptional) {
            /* If table doesn't have the field, mark it as no data */
            if (!ecs_type_has_entity_intern(
                world, table_type, component, true))
            {
                table_data->columns[c] = 0;
            }
        }

        /* Check if a the component is a reference. If 'entity' is set, the
         * component must be resolved from another entity, which is the case
         * for FromEntity and FromContainer. 
         * 
         * If no entity is set but the component is not found in the table, it
         * must come from a prefab. This is guaranteed, as at this point it is
         * already validated that the table matches with the system.
         * 
         * If the column kind is FromSingleton, the entity will be 0, but still
         * a reference needs to be added to the singleton component.
         * 
         * If the column kind is Cascade, there may not be an entity in case the
         * current table contains root entities. In that case, still add a
         * reference field. The application can, after the table has matched,
         * change the set of components, so that this column will turn into a
         * reference. Having the reference already linked to the system table
         * makes changing this administation easier when the change happens.
         * */
        if (entity || table_data->columns[c] == -1 || from == EcsCascade) {
            if (ecs_has(world, component, EcsComponent)) {
                EcsComponent *component_data = ecs_get_ptr(
                        world, component, EcsComponent);
                
                if (component_data->size) {
                    ecs_entity_t e;
                    ecs_reference_t *ref = ecs_vector_add(
                            &table_data->references, &reference_params);

                    /* Find the entity for the component */
                    if (from == EcsFromEntity) {
                        e = entity;
                    } else if (from == EcsCascade) {
                        e = entity;
                    } else {
                        e = ecs_get_entity_for_component(
                            world, entity, table_type, component);

                        if (from != EcsCascade) {
                            ecs_assert(e != 0, ECS_INTERNAL_ERROR, NULL);
                        }
                    }

                    ref->entity = e;
                    ref->component = component;
                    
                    if (e != ECS_INVALID_ENTITY) {
                        ecs_entity_info_t info = {.entity = e};

                        ref->cached_ptr = ecs_get_ptr_intern(
                            world, 
                            &world->main_stage,
                            &info,
                            component,
                            false,
                            true);

                        ecs_set_watch(world, &world->main_stage, e);                     
                    } else {
                        ref->cached_ptr = NULL;
                    }

                    /* Negative number indicates ref instead of offset to ecs_data */
                    table_data->columns[c] = -ecs_vector_count(table_data->references);
                    query->sig.has_refs = true;
                }
            }
        }

        /* component_data index is not offset by anything */
        table_data->components[c] = component;
    }
}


/* Match table with system */
static
bool match_table(
    ecs_world_t *world,
    ecs_table_t *table,
    ecs_query_t *query)
{
    ecs_type_t type, table_type = table->type;

    if (!query->sig.match_disabled && ecs_type_has_entity_intern(
        world, table_type, EEcsDisabled, false))
    {
        /* Don't match disabled entities */
        return false;
    }

    if (!query->sig.match_prefab && ecs_type_has_entity_intern(
        world, table_type, EEcsPrefab, false))
    {
        /* Don't match prefab entities */
        return false;
    }

    /* Test if table has SELF columns in either owned or inherited components */
    type = query->and_from_self;
    if (type && !ecs_type_contains(
        world, table_type, type, true, true))
    {
        return false;
    }

    /* Test if table has OWNED columns in owned components */
    type = query->and_from_owned;
    if (type && !ecs_type_contains(
        world, table_type, type, true, false))
    {
        return false;
    }  

    /* Test if table has SHARED columns in shared components */
    type = query->and_from_shared;
    if (type && ecs_type_contains(
        world, table_type, type, true, false))
    {
        /* If table has owned components that override the SHARED component, the
         * table won't match. */
        return false;
    } else if (type && !ecs_type_contains(
        world, table_type, type, true, true))
    {
        /* If the table does not have owned components, ensure that a SHARED
         * component can be found in prefabs. If not, the table doesn't match. */
        return false;
    }

    uint32_t i, column_count = ecs_vector_count(query->sig.columns);
    ecs_signature_column_t *buffer = ecs_vector_first(query->sig.columns);

    for (i = 0; i < column_count; i ++) {
        ecs_signature_column_t *elem = &buffer[i];
        ecs_signature_from_kind_t from = elem->from;
        ecs_signature_op_kind_t op = elem->op;

        if (op == EcsOperAnd) {
            if (from == EcsFromSelf || from == EcsFromOwned || 
                from == EcsFromShared) 
            {
                /* Already validated */
            } else if (from == EcsFromContainer) {
                if (!ecs_components_contains_component(
                    world, table_type, elem->is.component, ECS_CHILDOF, NULL))
                {
                    return false;
                }
            } else if (from == EcsFromEntity) {
                ecs_type_t type = ecs_get_type(world, elem->source);
                if (!ecs_type_has_entity(world, type, elem->is.component)) {
                    return false;
                }
            }
        } else if (op == EcsOperOr) {
            type = elem->is.type;
            if (from == EcsFromSelf) {
                if (!ecs_type_contains(
                    world, table_type, type, false, true))
                {
                    return false;
                }
            } else if (from == EcsFromContainer) {
                if (!components_contains(
                    world, table_type, type, NULL, false))
                {
                    return false;
                }
            }
        } else if (op == EcsOperNot) {
            if (from == EcsFromEntity) {
                ecs_type_t type = ecs_get_type(world, elem->source);
                if (ecs_type_has_entity(world, type, elem->is.component)) {
                    return false;
                }
            }
        }
    }

    type = query->not_from_self;
    if (type && ecs_type_contains(world, table_type, type, false, true))
    {
        return false;
    }

    type = query->not_from_owned;
    if (type && ecs_type_contains(world, table_type, type, false, false))
    {
        return false;
    }

    type = query->not_from_shared;
    if (type && !ecs_type_contains(world, table_type, type, false, false))
    {
        if (ecs_type_contains(world, table_type, type, false, true)) {
            return false;
        }
    }        

    type = query->not_from_component;
    if (type && components_contains(
        world, table_type, type, NULL, false))
    {
        return false;
    }

    return true;
}

/** Match existing tables against system (table is created before system) */
static
void match_tables(
    ecs_world_t *world,
    ecs_query_t *query)
{
    uint32_t i, count = ecs_sparse_count(world->tables);

    for (i = 0; i < count; i ++) {
        ecs_table_t *table = ecs_sparse_get(world->tables, ecs_table_t, i);

        if (match_table(world, table, query)) {
            add_table(world, query, table);
        }
    }
}

static
void postprocess(
    ecs_world_t *world,
    ecs_query_t *query)
{
    int i, count = ecs_vector_count(query->sig.columns);
    ecs_signature_column_t *columns = ecs_vector_first(query->sig.columns);

    for (i = 0; i < count; i ++) {
        ecs_signature_column_t *elem = &columns[i];
        ecs_signature_from_kind_t from = elem->from;
        ecs_signature_op_kind_t op = elem->op;

        /* AND (default) and optional columns are stored the same way */
        if (from == EcsFromEntity) {
            ecs_set_watch(world, &world->main_stage, elem->source);
        } else if (from == EcsCascade) {
            query->sig.cascade_by = i + 1;
        } else if (op == EcsOperOr) {
            /* Nothing to be done here */
        } else if (op == EcsOperNot) {
            if (from == EcsFromSelf) {
                query->not_from_self =
                    ecs_type_add_intern(
                        world, NULL, query->not_from_self, 
                        elem->is.component);
            } else if (from == EcsFromOwned) {
                query->not_from_owned =
                    ecs_type_add_intern(
                        world, NULL, query->not_from_owned, 
                        elem->is.component);
            } else if (from == EcsFromShared) {
                query->not_from_shared =
                    ecs_type_add_intern(
                        world, NULL, query->not_from_shared, 
                        elem->is.component);                    
            } else if (from == EcsFromEntity) {
                /* Nothing to be done here */
            } else {
                query->not_from_component =
                    ecs_type_add_intern(
                        world, NULL, query->not_from_component, 
                        elem->is.component);
            }
        } else if (op == EcsOperAnd) {
            if (from == EcsFromSelf) {
                query->and_from_self = ecs_type_add_intern(
                    world, NULL, query->and_from_self, 
                    elem->is.component);
            } else if (from == EcsFromOwned) {
                query->and_from_owned = ecs_type_add_intern(
                    world, NULL, query->and_from_owned, 
                    elem->is.component);
            } else if (from == EcsFromShared) {
                query->and_from_shared = ecs_type_add_intern(
                    world, NULL, query->and_from_shared, 
                    elem->is.component);
            } else if (from == EcsFromSystem) {
                query->and_from_system = ecs_type_add_intern(
                    world, NULL, query->and_from_system, 
                    elem->is.component);
            }
        }
    }
}

/* -- Private API -- */

ecs_query_t* ecs_new_query(
    ecs_world_t *world,
    ecs_signature_t *sig)
{
    ecs_query_t *result = ecs_sparse_add(world->queries, ecs_query_t);
    result->sig = *sig;

    postprocess(world, result);

    match_tables(world, result);

    /* Transfer ownership of signature to query */
    sig->owned = false;

    return result;
}

void ecs_query_free(
    ecs_query_t *query)
{
    ecs_signature_free(&query->sig);
    ecs_vector_free(query->tables);
}

/** Match new table against system (table is created after system) */
void ecs_query_match_table(
    ecs_world_t *world,
    ecs_query_t *query,
    ecs_table_t *table)
{
    if (match_table(world, table, query)) {
        add_table(world, query, table);
    }
}

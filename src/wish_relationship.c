/**
 * Copyright (C) 2018, ControlThings Oy Ab
 * Copyright (C) 2018, André Kaustell
 * Copyright (C) 2018, Jan Nyman
 * Copyright (C) 2018, Jepser Lökfors
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * @license Apache-2.0
 */
#include "wish_relationship.h"
#include "wish_core.h"
#include "wish_platform.h"
#include "string.h"
#include "wish_debug.h"
#include "utlist.h"

void wish_relationship_init(wish_core_t* core) {
    core->relationship_db = wish_platform_malloc(sizeof(wish_relationship_t) * WISH_RELATIONSHIP_DB_LEN);
}

void wish_relationship_destroy(wish_core_t* core) {
    wish_platform_free(core->relationship_db);
}

void wish_relationship_req_add(wish_core_t* core, wish_relationship_req_t* req) {
    wish_relationship_req_t* rel = wish_platform_malloc(sizeof(wish_relationship_req_t));
    memcpy(rel, req, sizeof(wish_relationship_req_t));
    
    DL_APPEND(core->relationship_req_db, rel);
}


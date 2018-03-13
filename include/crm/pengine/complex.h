/* 
 * Copyright (C) 2004 Andrew Beekhof <andrew@beekhof.net>
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */
#ifndef PENGINE_COMPLEX__H
#  define PENGINE_COMPLEX__H

enum pe_obj_types {
    pe_unknown = -1,
    pe_native = 0,
    pe_group = 1,
    pe_clone = 2,
    pe_container = 3,
};

typedef struct resource_object_functions_s {
    gboolean(*unpack) (resource_t *, pe_working_set_t *);
    resource_t *(*find_rsc) (resource_t * parent, const char *search, node_t * node, int flags);
    /* parameter result must be free'd */
    char *(*parameter) (resource_t *, node_t *, gboolean, const char *, pe_working_set_t *);
    void (*print) (resource_t *, const char *, long, void *);
     gboolean(*active) (resource_t *, gboolean);
    enum rsc_role_e (*state) (const resource_t *, gboolean);
    node_t *(*location) (resource_t *, GListPtr *, gboolean);
    void (*free) (resource_t *);
} resource_object_functions_t;

extern resource_object_functions_t resource_class_functions[];
void get_meta_attributes(GHashTable * meta_hash, resource_t * rsc, node_t * node,
                         pe_working_set_t * data_set);
void get_rsc_attributes(GHashTable * meta_hash, resource_t * rsc, node_t * node,
                        pe_working_set_t * data_set);

#ifdef ENABLE_VERSIONED_ATTRS
void pe_get_versioned_attributes(xmlNode * meta_hash, resource_t * rsc, node_t * node,
                                 pe_working_set_t * data_set);
#endif

typedef struct resource_alloc_functions_s resource_alloc_functions_t;

gboolean is_parent(resource_t *child, resource_t *rsc);
resource_t *uber_parent(resource_t * rsc);

#endif

/*
 * Copyright (C) 2014 Intel Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) version 3.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301  USA
 */

#include <syncevo/GVariantSupport.h>

#include <syncevo/declarations.h>
SE_BEGIN_CXX

#ifdef HAVE_GLIB
#if GLIB_CHECK_VERSION(2,32,0)

GVariantCXX HashTable2Variant(const GHashTable *hashTable) throw ()
{
    GVariantBuilder builder;
    GHashTableIter iter;
    const gchar *key;
    GVariant *value;

    if (!hashTable) {
        return GVariantCXX();
    }

    g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);

    g_hash_table_iter_init(&iter, (GHashTable *)hashTable);
    while (g_hash_table_iter_next(&iter, (gpointer *)&key, (gpointer *)&value)) {
        g_variant_builder_add(&builder, "{sv}", key, g_variant_ref(value));
    }
    GVariantStealCXX variant(g_variant_builder_end(&builder));
    return variant;
}

GHashTableCXX Variant2HashTable(GVariant *variant) throw ()
{
    if (!variant) {
        return GHashTableCXX();
    }

    GVariantIter iter;
    GVariant *value;
    gchar *key;

    GHashTableStealCXX hashTable(g_hash_table_new_full(g_str_hash,
                                                       g_str_equal,
                                                       g_free,
                                                       (GDestroyNotify)g_variant_unref));
    g_variant_iter_init(&iter, variant);
    while (g_variant_iter_next(&iter, "{sv}", &key, &value)) {
        g_hash_table_insert(hashTable, g_strdup(key), g_variant_ref(value));
    }
    return hashTable;
}

#endif // GLIB_CHECK_VERSION
#endif // HAVE_GLIB

SE_END_CXX

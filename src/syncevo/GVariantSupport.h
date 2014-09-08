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

#ifndef INCL_GVARIANT_SUPPORT
#define INCL_GVARIANT_SUPPORT

#include <syncevo/GLibSupport.h>

#ifdef HAVE_GLIB
SE_GLIB_TYPE(GVariant, g_variant)
#endif

#include <syncevo/declarations.h>
SE_BEGIN_CXX

#ifdef HAVE_GLIB

// G_VARIANT_TYPE_VARDICT was introduced in glib 2.32
#if GLIB_CHECK_VERSION(2,32,0)

// Originally from google-oauth2-example.c, which is also under LGPL
// 2.1, or any later version.
GVariantCXX HashTable2Variant(const GHashTable *hashTable) throw ();

/**
 * The created GHashTable maps strings to GVariants which are
 * reference counted, so when adding or setting values, use g_variant_ref_sink(g_variant_new_...()).
 */
GHashTableCXX Variant2HashTable(GVariant *variant) throw ();

#endif // GLIB_CHECK_VERSION

/**
 * The created GHashTable maps strings to strings. Both key and values
 * are owned by the hash table. Will throw errors if the variant
 * has entries with a different kind of key or value.
 */
GHashTableCXX Variant2StrHashTable(GVariant *variant);

#endif // HAVE_GLIB

SE_END_CXX

#endif // INCL_GVARIANT_SUPPORT

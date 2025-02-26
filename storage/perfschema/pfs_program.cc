/* Copyright (c) 2013, 2023, Oracle and/or its affiliates.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License, version 2.0,
  as published by the Free Software Foundation.

  This program is also distributed with certain software (including
  but not limited to OpenSSL) that is licensed under separate terms,
  as designated in a particular file or component or in included license
  documentation.  The authors of MySQL hereby grant you an additional
  permission to link the program and your derivative works with the
  separately licensed software that they have included with MySQL.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License, version 2.0, for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software Foundation,
  51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA */

/**
  @file storage/perfschema/pfs_program.cc
  Statement Digest data structures (implementation).
*/

/*
  This code needs extra visibility in the lexer structures
*/

#include "my_global.h"
#include "my_sys.h"
#include "pfs_instr.h"
#include "pfs_program.h"
#include "pfs_global.h"
#include "sql_string.h"
#include "pfs_setup_object.h"
#include "pfs_buffer_container.h"
#include "mysqld.h"                //system_charset_info
#include <string.h>

LF_HASH program_hash;
static bool program_hash_inited= false;

/**
  Initialize table EVENTS_STATEMENTS_SUMMARY_BY_PROGRAM.
  @param param performance schema sizing
*/
int init_program(const PFS_global_param *param)
{
  if (global_program_container.init(param->m_program_sizing))
    return 1;

  reset_esms_by_program();
  return 0;
}

/** Cleanup table EVENTS_STATEMENTS_SUMMARY_BY_PROGRAM. */
void cleanup_program(void)
{
  global_program_container.cleanup();
}

C_MODE_START
static const uchar *program_hash_get_key(const void *entry, size_t *length,
                                         my_bool)
{
  const PFS_program * const *typed_entry;
  const PFS_program *program;
  const void *result;
  typed_entry= static_cast<const PFS_program* const *> (entry);
  assert(typed_entry != NULL);
  program= *typed_entry;
  assert(program != NULL);
  *length= program->m_key.m_key_length;
  result= program->m_key.m_hash_key;
  return reinterpret_cast<const uchar *>(result);
}
C_MODE_END

/**
  Initialize the program hash.
  @return 0 on success
*/
int init_program_hash(const PFS_global_param *param)
{
  if ((! program_hash_inited) && (param->m_program_sizing != 0))
  {
    lf_hash_init(&program_hash, sizeof(PFS_program*), LF_HASH_UNIQUE,
                 0, 0, program_hash_get_key, &my_charset_bin);
    program_hash_inited= true;
  }
  return 0;
}

/** Cleanup the program hash. */
void cleanup_program_hash(void)
{
  if (program_hash_inited)
  {
    lf_hash_destroy(&program_hash);
    program_hash_inited= false;
  }
}

static void set_program_key(PFS_program_key *key,
                            enum_object_type object_type,
                            const char *object_name, uint object_name_length,
                            const char *schema_name, uint schema_name_length)
{
  assert(object_name_length <= COL_OBJECT_NAME_SIZE);
  assert(schema_name_length <= COL_OBJECT_SCHEMA_SIZE);

  /*
    To make sure generated key is case insensitive,
    convert object_name/schema_name to lowercase.
   */

  char *ptr= &key->m_hash_key[0];
  const char *end= ptr + sizeof(key->m_hash_key) - 1;

  ptr[0]= object_type;
  ptr++;

  if (object_name_length > 0)
    ptr+= system_charset_info->casedn(object_name, object_name_length,
                                      ptr, end - ptr);
  ptr[0]= 0;
  ptr++;

  if (schema_name_length > 0)
    ptr+= system_charset_info->opt_casedn(schema_name, schema_name_length,
                                          ptr, end - ptr,
                                          lower_case_table_names);
  ptr[0]= 0;
  ptr++;

  key->m_key_length= static_cast<uint>(ptr - &key->m_hash_key[0]);
}



void PFS_program::reset_data()
{
  m_sp_stat.reset();
  m_stmt_stat.reset();
}

static void fct_reset_esms_by_program(PFS_program *pfs)
{
  pfs->reset_data();
}

void reset_esms_by_program()
{
  global_program_container.apply_all(fct_reset_esms_by_program);
}

static LF_PINS* get_program_hash_pins(PFS_thread *thread)
{
  if (unlikely(thread->m_program_hash_pins == NULL))
  {
    if (! program_hash_inited)
      return NULL;
    thread->m_program_hash_pins= lf_hash_get_pins(&program_hash);
  }
  return thread->m_program_hash_pins;
}

PFS_program*
find_or_create_program(PFS_thread *thread,
                      enum_object_type object_type,
                      const char *object_name,
                      uint object_name_length,
                      const char *schema_name,
                      uint schema_name_length)
{
  bool is_enabled, is_timed;

  LF_PINS *pins= get_program_hash_pins(thread);
  if (unlikely(pins == NULL))
  {
    global_program_container.m_lost++;
    return NULL;
  }

  /* Prepare program key */
  PFS_program_key key;
  set_program_key(&key, object_type,
                  object_name, object_name_length,
                  schema_name, schema_name_length);

  PFS_program **entry;
  PFS_program *pfs= NULL;
  uint retry_count= 0;
  const uint retry_max= 3;
  pfs_dirty_state dirty_state;

search:
  entry= reinterpret_cast<PFS_program**>
    (lf_hash_search(&program_hash, pins,
                    key.m_hash_key, key.m_key_length));

  if (entry && (entry != MY_ERRPTR))
  {
    /* If record already exists then return its pointer. */
    pfs= *entry;
    lf_hash_search_unpin(pins);
    return pfs;
  }

  lf_hash_search_unpin(pins);

  /*
    First time while inserting this record to program array we need to
    find out if it is enabled and timed.
  */
  lookup_setup_object(thread, object_type,
                      schema_name, schema_name_length,
                      object_name, object_name_length,
                      &is_enabled, &is_timed);

  /* Else create a new record in program stat array. */
  pfs= global_program_container.allocate(& dirty_state);
  if (pfs != NULL)
  {
    /* Do the assignments. */
    memcpy(pfs->m_key.m_hash_key, key.m_hash_key, key.m_key_length);
    pfs->m_key.m_key_length= key.m_key_length;
    pfs->m_type= object_type;

    pfs->m_object_name= pfs->m_key.m_hash_key + 1;
    pfs->m_object_name_length= object_name_length;
    pfs->m_schema_name= pfs->m_object_name + object_name_length + 1;
    pfs->m_schema_name_length= schema_name_length;
    pfs->m_enabled= is_enabled;
    pfs->m_timed= is_timed;

    /* Insert this record. */
    pfs->m_lock.dirty_to_allocated(& dirty_state);
    int res= lf_hash_insert(&program_hash, pins, &pfs);

    if (likely(res == 0))
    {
      return pfs;
    }

    global_program_container.deallocate(pfs);

    if (res > 0)
    {
      /* Duplicate insert by another thread */
      if (++retry_count > retry_max)
      {
        /* Avoid infinite loops */
        global_program_container.m_lost++;
        return NULL;
      }
       goto search;
    }
    /* OOM in lf_hash_insert */
    global_program_container.m_lost++;
    return NULL;
  }

  return NULL;
}

void drop_program(PFS_thread *thread,
                 enum_object_type object_type,
                 const char *object_name,
                 uint object_name_length,
                 const char *schema_name,
                 uint schema_name_length)
{
  LF_PINS *pins= get_program_hash_pins(thread);
  if (unlikely(pins == NULL))
    return;

  /* Prepare program key */
  PFS_program_key key;
  set_program_key(&key, object_type,
                  object_name, object_name_length,
                  schema_name, schema_name_length);

  PFS_program **entry;
  entry= reinterpret_cast<PFS_program**>
    (lf_hash_search(&program_hash, pins,
                    key.m_hash_key, key.m_key_length));

  if (entry && (entry != MY_ERRPTR))
  {
    PFS_program *pfs= NULL;
    pfs= *entry;

    lf_hash_delete(&program_hash, pins,
                   key.m_hash_key, key.m_key_length);
    global_program_container.deallocate(pfs);
  }

  lf_hash_search_unpin(pins);
  return;
}

void PFS_program::refresh_setup_object_flags(PFS_thread *thread)
{
  lookup_setup_object(thread, m_type,
                      m_schema_name, m_schema_name_length,
                      m_object_name, m_object_name_length,
                      &m_enabled, &m_timed);
}

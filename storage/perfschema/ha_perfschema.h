/* Copyright (c) 2008, 2023, Oracle and/or its affiliates.

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
  51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

#ifndef HA_PERFSCHEMA_H
#define HA_PERFSCHEMA_H

#include "handler.h"                            /* class handler */
#include "table.h"
#include "sql_class.h"

/**
  @file storage/perfschema/ha_perfschema.h
  Performance schema storage engine (declarations).

  @defgroup Performance_schema_engine Performance Schema Engine
  @ingroup Performance_schema_implementation
  @{
*/
struct PFS_engine_table_share;
class PFS_engine_table;
/** Name of the performance schema engine. */
extern const char *pfs_engine_name;

/** A handler for a PERFORMANCE_SCHEMA table. */
class ha_perfschema final : public handler
{
public:
  /**
    Create a new performance schema table handle on a table.
    @param hton storage engine handler singleton
    @param share table share
  */
  ha_perfschema(handlerton *hton, TABLE_SHARE *share);

  ~ha_perfschema();

  /** Capabilities of the performance schema tables. */
  ulonglong table_flags(void) const override
  {
    /*
      About HA_FAST_KEY_READ:

      The storage engine ::rnd_pos() method is fast to locate records by key,
      so HA_FAST_KEY_READ is technically true, but the record content can be
      overwritten between ::rnd_next() and ::rnd_pos(), because all the P_S
      data is volatile.
      The HA_FAST_KEY_READ flag is not advertised, to force the optimizer
      to cache records instead, to provide more consistent records.
      For example, consider the following statement:
      - select * from P_S.EVENTS_WAITS_HISTORY_LONG where THREAD_ID=<n>
      order by ...
      With HA_FAST_KEY_READ, it can return records where "THREAD_ID=<n>"
      is false, because the where clause was evaluated to true after
      ::rnd_pos(), then the content changed, then the record was fetched by
      key using ::rnd_pos().
      Without HA_FAST_KEY_READ, the optimizer reads all columns and never
      calls ::rnd_pos(), so it is guaranteed to return only thread <n>
      records.
    */
    return HA_NO_TRANSACTIONS | HA_REC_NOT_IN_SEQ | HA_NO_AUTO_INCREMENT |
      HA_PRIMARY_KEY_REQUIRED_FOR_DELETE;
  }

  /**
    Operations supported by indexes.
    None, there are no indexes.
  */
  ulong index_flags(uint , uint , bool ) const override
  { return 0; }

  uint max_supported_record_length(void) const override
  { return HA_MAX_REC_LENGTH; }

  uint max_supported_keys(void) const override
  { return 0; }

  uint max_supported_key_parts(void) const override
  { return 0; }

  uint max_supported_key_length(void) const override
  { return 0; }

  ha_rows estimate_rows_upper_bound(void) override
  { return HA_POS_ERROR; }

  IO_AND_CPU_COST scan_time(void)  override
  {
    return {0.0, 1.0};
  }

  /**
    Open a performance schema table.
    @param name the table to open
    @param mode unused
    @param test_if_locked unused
    @return 0 on success
  */
  int open(const char *name, int mode, uint test_if_locked) override;

  /**
    Close a table handle.
    @sa open.
  */
  int close(void) override;

  /**
    Write a row.
    @param buf the row to write
    @return 0 on success
  */
  int write_row(const uchar *buf) override;

  void use_hidden_primary_key() override;

  /**
    Update a row.
    @param old_data the row old values
    @param new_data the row new values
    @return 0 on success
  */
  int update_row(const uchar *old_data, const uchar *new_data) override;

  /**
    Delete a row.
    @param buf the row to delete
    @return 0 on success
  */
  int delete_row(const uchar *buf) override;

  int rnd_init(bool scan) override;

  /**
    Scan end.
    @sa rnd_init.
  */
  int rnd_end(void) override;

  /**
    Iterator, fetch the next row.
    @param[out] buf the row fetched.
    @return 0 on success
  */
  int rnd_next(uchar *buf) override;

  /**
    Iterator, fetch the row at a given position.
    @param[out] buf the row fetched.
    @param pos the row position
    @return 0 on success
  */
  int rnd_pos(uchar *buf, uchar *pos) override;

  /**
    Read the row current position.
    @param record the current row
  */
  void position(const uchar *record) override;

  int info(uint) override;

  int delete_all_rows(void) override;

  int truncate() override;

  int delete_table(const char *from) override;

  int rename_table(const char * from, const char * to) override;

  int create(const char *name, TABLE *form,
             HA_CREATE_INFO *create_info) override;

  THR_LOCK_DATA **store_lock(THD *thd, THR_LOCK_DATA **to,
                             enum thr_lock_type lock_type) override;

  uint8 table_cache_type(void) override
  { return HA_CACHE_TBL_NOCACHE; }

  my_bool register_query_cache_table
    (THD *, const char *, uint , qc_engine_callback *engine_callback,
     ulonglong *) override
  {
    *engine_callback= 0;
    return FALSE;
  }

  void print_error(int error, myf errflags) override;

private:
  /**
     Check if the caller is a replication thread or the caller is called
     by a client thread executing base64 encoded BINLOG'... statement.

     In theory, performance schema tables are not supposed to be replicated.
     This is true and enforced starting with MySQL 5.6.10.
     In practice, in previous versions such as MySQL 5.5 (GA) or earlier 5.6
     (non GA) DML on performance schema tables could end up written in the binlog,
     both in STATEMENT and ROW format.
     While these records are not supposed to be there, they are found when:
     - performing replication from a 5.5 master to a 5.6 slave during
       upgrades
     - performing replication from 5.5 (performance_schema enabled)
       to a 5.6 slave
     - performing point in time recovery in 5.6 with old archived logs.

     This API detects when the code calling the performance schema storage
     engine is a slave thread or whether the code calling is the client thread
     executing a BINLOG'.. statement.

     This API acts as a late filter for the above mentioned cases.

     For ROW format, @see Rows_log_event::do_apply_event()

  */
  bool is_executed_by_slave() const
  {
    assert(table != NULL);
    assert(table->in_use != NULL);
    return table->in_use->slave_thread;

  }

  /** MySQL lock */
  THR_LOCK_DATA m_thr_lock;
  /** Performance schema table share for this table handler. */
  const PFS_engine_table_share *m_table_share;
  /** Performance schema table cursor. */
  PFS_engine_table *m_table;
};

/** @} */
#endif


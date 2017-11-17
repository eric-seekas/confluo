#ifndef DIALOG_DIALOG_TABLE_H_
#define DIALOG_DIALOG_TABLE_H_

#include <math.h>

#include <functional>
#include <numeric>
#include <thread>

#include "configuration_params.h"
#include "exceptions.h"
#include "monolog.h"
#include "parser/expression_compiler.h"
#include "radix_tree.h"
#include "record_batch.h"
#include "schema.h"
#include "storage.h"
#include "string_map.h"
#include "trigger.h"
#include "types/type_manager.h"
#include "alert_index.h"
#include "atomic_multilog_metadata.h"
#include "data_log.h"
#include "filter.h"
#include "filter_log.h"
#include "index_log.h"
#include "optional.h"
#include "parser/schema_parser.h"
#include "parser/trigger_compiler.h"
#include "periodic_task.h"
#include "planner/query_planner.h"
#include "read_tail.h"
#include "task_pool.h"
#include "time_utils.h"
#include "string_utils.h"

using namespace ::confluo::monolog;
using namespace ::confluo::index;
using namespace ::confluo::monitor;
using namespace ::confluo::parser;
using namespace ::confluo::planner;
using namespace ::utils;

namespace confluo {

class atomic_multilog {
 public:
  typedef data_log data_log_type;
  typedef schema_t schema_type;
  typedef read_tail read_tail_type;
  typedef metadata_writer metadata_writer_type;

  typedef size_t filter_id_t;
  typedef std::pair<size_t, size_t> trigger_id_t;

  typedef alert_index::alert_list alert_list;

  /**
   * Constructor that initializes atomic multilog
   * @param name The atomic multilog name
   * @param schema The schema for the atomic multilog
   * @param path The path of the atomic multilog
   * @param storage The storage mode of the atomic multilog
   * @param pool The pool of tasks
   */
  atomic_multilog(const std::string& name, const std::vector<column_t>& schema,
                  const std::string& path, const storage::storage_mode& storage,
                  task_pool& pool)
      : name_(name),
        schema_(schema),
        data_log_("data_log", path, storage),
        rt_(path, storage),
        metadata_(path, storage.id),
        planner_(&data_log_, &indexes_, &schema_),
        mgmt_pool_(pool),
        monitor_task_("monitor") {
    metadata_.write_schema(schema_);
    monitor_task_.start(std::bind(&atomic_multilog::monitor_task, this),
                        configuration_params::MONITOR_PERIODICITY_MS);
  }

  atomic_multilog(const std::string& name, const std::string& schema,
                  const std::string& path, const storage::storage_mode& storage,
                  task_pool& pool)
      : atomic_multilog(name, parser::parse_schema(schema), path, storage, pool) {
  }

  // Management ops
  /**
   * Adds index to the atomic multilog
   * @param field_name The name of the field in the atomic multilog
   * @throw ex Management exception
   */
  void add_index(const std::string& field_name, double bucket_size =
                     configuration_params::INDEX_BUCKET_SIZE) {
    optional<management_exception> ex;
    auto ret =
        mgmt_pool_.submit(
            [field_name, bucket_size, &ex, this] {

              uint16_t idx;
              try {
                idx = schema_.get_field_index(field_name);
              } catch (std::exception& e) {
                ex = management_exception("Could not add index for " + field_name + " : " + e.what());
                return;
              }

              column_t& col = schema_[idx];
              bool success = col.set_indexing();
              if (success) {
                uint16_t index_id = UINT16_MAX;
                if (col.type().is_valid()) {
                  index_id = indexes_.push_back(new radix_index(
                          col.type().size, 256));
                } else {
                  ex = management_exception("Index not supported for field type");
                }
                col.set_indexed(index_id, bucket_size);
                metadata_.write_index_info(field_name, bucket_size);
              } else {
                ex = management_exception("Could not index " + field_name + ": already indexed/indexing");
                return;
              }
            });

    ret.wait();
    if (ex.has_value())
      throw ex.value();
  }

  /**
   * Removes index from the atomic multilog
   * @param field_name The name of the field in the atomic multilog
   * @throw ex Management exception
   */
  void remove_index(const std::string& field_name) {
    optional<management_exception> ex;
    auto ret =
        mgmt_pool_.submit(
            [field_name, &ex, this] {
              uint16_t idx;
              try {
                idx = schema_.get_field_index(field_name);
              } catch (std::exception& e) {
                ex = management_exception("Could not remove index for " + field_name + " : " + e.what());
                return;
              }

              if (!schema_[idx].disable_indexing()) {
                ex = management_exception("Could not remove index for " + field_name + ": No index exists");
                return;
              }
            });
    ret.wait();
    if (ex.has_value())
      throw ex.value();
  }

  /**
   * Checks whether column of atomic multilog is indexed
   * @param field_name The name of the field
   * @return True if column is indexed, false otherwise
   * @throw ex Management exception
   */
  bool is_indexed(const std::string& field_name) {
    optional<management_exception> ex;
    uint16_t idx;
    try {
      idx = schema_.get_field_index(field_name);
    } catch (std::exception& e) {
      THROW(management_exception, "Field name does not exist");
    }
    column_t& col = schema_[idx];
    return col.is_indexed();
  }

  /**
   * Adds filter to the atomic multilog
   * @param filter_name The name of the filter
   * @param filter_expr The expression to filter out elements in the atomic multilog
   * @throw ex Management exception
   */
  void add_filter(const std::string& filter_name,
                  const std::string& filter_expr) {
    optional<management_exception> ex;
    auto ret =
        mgmt_pool_.submit(
            [filter_name, filter_expr, &ex, this] {
              filter_id_t filter_id;
              if (filter_map_.get(filter_name, filter_id) != -1) {
                ex = management_exception("Filter " + filter_name + " already exists.");
                return;
              }
              auto t = parser::parse_expression(filter_expr);
              auto cexpr = parser::compile_expression(t, schema_);
              filter_id = filters_.push_back(new filter(cexpr, default_filter));
              metadata_.write_filter_info(filter_name, filter_expr);
              if (filter_map_.put(filter_name, filter_id) == -1) {
                ex = management_exception("Could not add filter " + filter_name + " to filter map.");
                return;
              }
            });
    ret.wait();
    if (ex.has_value())
      throw ex.value();
  }

  /**
   * Removes filter from the atomic multilog
   * @param filter_name The name of the filter
   * @throw ex Management exception
   */
  void remove_filter(const std::string& filter_name) {
    optional<management_exception> ex;
    auto ret = mgmt_pool_.submit([filter_name, &ex, this] {
      filter_id_t filter_id;
      if (filter_map_.get(filter_name, filter_id) == -1) {
        ex = management_exception("Filter " + filter_name + " does not exist.");
        return;
      }
      bool success = filters_.at(filter_id)->invalidate();
      if (!success) {
        ex = management_exception("Filter already invalidated.");
        return;
      }
      filter_map_.remove(filter_name, filter_id);
    });
    ret.wait();
    if (ex.has_value())
      throw ex.value();
  }

  /**
   * Adds trigger to the atomic multilog
   * @param trigger_name The name of the trigger
   * @param filter_name The name of the filter
   * @param trigger_expr The trigger expression to be executed
   * @throw ex Management exception
   */
  void add_trigger(const std::string& trigger_name,
                   const std::string& filter_name,
                   const std::string& trigger_expr,
                   const uint64_t periodicity_ms =
                       configuration_params::MONITOR_PERIODICITY_MS) {

    if (periodicity_ms < configuration_params::MONITOR_PERIODICITY_MS) {
      throw management_exception(
          "Trigger periodicity (" + std::to_string(periodicity_ms)
              + "ms) cannot be less than monitor periodicity ("
              + std::to_string(configuration_params::MONITOR_PERIODICITY_MS)
              + "ms)");
    }

    if (periodicity_ms % configuration_params::MONITOR_PERIODICITY_MS != 0) {
      throw management_exception(
          "Trigger periodicity (" + std::to_string(periodicity_ms)
              + "ms) must be a multiple of monitor periodicity ("
              + std::to_string(configuration_params::MONITOR_PERIODICITY_MS)
              + "ms)");
    }

    optional<management_exception> ex;
    auto ret =
        mgmt_pool_.submit(
            [trigger_name, filter_name, trigger_expr, periodicity_ms, &ex, this] {
              trigger_id_t trigger_id;
              if (trigger_map_.get(trigger_name, trigger_id) != -1) {
                ex = management_exception("Trigger " + trigger_name + " already exists.");
                return;
              }
              filter_id_t filter_id;
              if (filter_map_.get(filter_name, filter_id) == -1) {
                ex = management_exception("Filter " + filter_name + " does not exist.");
                return;
              }
              trigger_id.first = filter_id;
              auto ct = parser::compile_trigger(parser::parse_trigger(trigger_expr), schema_);
              const column_t& col = schema_[ct.field_name];
              trigger* t = new trigger(trigger_name, filter_name, trigger_expr, ct.agg, col.name(), col.idx(), col.type(), ct.relop, ct.threshold, periodicity_ms);
              trigger_id.second = filters_.at(filter_id)->add_trigger(t);
              metadata_.write_trigger_info(trigger_name, filter_name, ct.agg, ct.field_name, ct.relop,
                  ct.threshold, periodicity_ms);
              if (trigger_map_.put(trigger_name, trigger_id) == -1) {
                ex = management_exception("Could not add trigger " + filter_name + " to trigger map.");
                return;
              }
            });
    ret.wait();
    if (ex.has_value())
      throw ex.value();
  }

  /**
   * Removes trigger from the atomic multilog
   * @param trigger_name The name of the trigger
   * @throw Management exception
   */
  void remove_trigger(const std::string& trigger_name) {
    optional<management_exception> ex;
    auto ret =
        mgmt_pool_.submit(
            [trigger_name, &ex, this] {
              trigger_id_t trigger_id;
              if (trigger_map_.get(trigger_name, trigger_id) == -1) {
                ex = management_exception("Trigger " + trigger_name + " does not exist.");
                return;
              }
              bool success = filters_.at(trigger_id.first)->remove_trigger(trigger_id.second);
              if (!success) {
                ex = management_exception("Trigger already invalidated.");
                return;
              }
              trigger_map_.remove(trigger_name, trigger_id);
            });
    ret.wait();
    if (ex.has_value())
      throw ex.value();
  }

  // Query ops
  /**
   * Appends a batch of records to the atomic multilog
   * @param batch The record batch to be added
   * @return The offset where the batch is located
   */
  size_t append_batch(record_batch& batch) {
    size_t record_size = schema_.record_size();
    size_t batch_bytes = batch.nrecords * record_size;
    size_t log_offset = data_log_.reserve(batch_bytes);
    size_t cur_offset = log_offset;
    for (record_block& block : batch.blocks) {
      data_log_.write(cur_offset,
                      reinterpret_cast<const uint8_t*>(block.data.data()),
                      block.data.length());
      update_aux_record_block(cur_offset, block, record_size);
      cur_offset += block.data.length();
    }

    data_log_.flush(log_offset, batch_bytes);
    rt_.advance(log_offset, batch_bytes);
    return log_offset;
  }

  /**
   * Appends data to the atomic multilog
   * @param data The data to be stored
   * @return The offset of where the data is located
   */
  size_t append(void* data) {
    size_t record_size = schema_.record_size();
    size_t offset = data_log_.append((const uint8_t*) data, record_size);

    ro_data_ptr ptr;
    data_log_.ptr(offset, ptr);
    record_t r = schema_.apply(offset, ptr);

    size_t nfilters = filters_.size();
    for (size_t i = 0; i < nfilters; i++)
      if (filters_.at(i)->is_valid())
        filters_.at(i)->update(r);

    for (const field_t& f : r)
      if (f.is_indexed())
        indexes_.at(f.index_id())->insert(f.get_key(), offset);

    data_log_.flush(offset, record_size);
    rt_.advance(offset, record_size);
    return offset;
  }

  /**
   * Reads the data from the atomic multilog
   * @param offset The location of where the data is stored
   * @param version The tail pointer's location
   * @param ptr The read-only pointer to store in
   */
  void read(uint64_t offset, uint64_t& version, ro_data_ptr& ptr) const {
    version = rt_.get();
    if (offset < version) {
      data_log_.cptr(offset, ptr);
      record_t r = schema_.apply(offset, ptr);
      return;
    }
    ptr.init(nullptr, 0, nullptr);
  }

  /**
   * Reads the data based on the offset
   * @param offset The location of the data
   * @param ptr The read-only pointer to store in
   */
  void read(uint64_t offset, ro_data_ptr& ptr) const {
    uint64_t version;
    read(offset, version, ptr);
  }

  /**
   * Executes the filter expression
   * @param expr The filter expression
   * @return The result of applying the filter to the atomic multilog
   */
  lazy::stream<record_t> execute_filter(const std::string& expr) const {
    uint64_t version = rt_.get();
    auto t = parser::parse_expression(expr);
    auto cexpr = parser::compile_expression(t, schema_);
    query_plan plan = planner_.plan(cexpr);
    return plan.execute(version);
  }

  lazy::stream<record_t> query_filter(const std::string& filter_name,
                                      uint64_t begin_ms,
                                      uint64_t end_ms) const {
    size_t filter_id;
    if (filter_map_.get(filter_name, filter_id) == -1) {
      throw invalid_operation_exception(
          "Filter " + filter_name + " does not exist.");
    }

    auto res = filters_.at(filter_id)->lookup_range(begin_ms, end_ms);
    uint64_t version = rt_.get();
    auto version_check = [version](uint64_t offset) -> bool {
      return offset < version;
    };

    data_log const* d = &data_log_;
    schema_t const* s = &schema_;
    auto to_record = [d, s](uint64_t offset) -> record_t {
      ro_data_ptr ptr;
      d->cptr(offset, ptr);
      return s->apply(offset, ptr);
    };

    return lazy::container_to_stream(res).filter(version_check).map(to_record);
  }

  /**
   * Executes a query filter expression
   * @param filter_name The name of the filter
   * @param expr The filter expression
   * @param begin_ms Beginning of time-range in ms
   * @param end_ms End of time-range in ms
   * @return A stream contanining the results of the filter
   */
  lazy::stream<record_t> query_filter(const std::string& filter_name,
                                      const std::string& expr,
                                      uint64_t begin_ms,
                                      uint64_t end_ms) const {
    auto t = parser::parse_expression(expr);
    auto e = parser::compile_expression(t, schema_);
    auto expr_check = [e](const record_t& r) -> bool {
      return e.test(r);
    };
    return query_filter(filter_name, begin_ms, end_ms).filter(expr_check);
  }

  /**
   * Gets the alert list
   * @param ts_block_begin The beginning of the block
   * @param ts_block_end The end of the block
   * @return A list of alerts in the block range
   */
  alert_list get_alerts(uint64_t ts_block_begin, uint64_t ts_block_end) const {
    return alerts_.get_alerts(ts_block_begin, ts_block_end);
  }

  /**
   * Gets the name of the atomic multilog
   * @return The atomic multilog name
   */
  const std::string& get_name() const {
    return name_;
  }

  /**
   * Gets the atomic multilog schema
   * @return The schema of the atomic multilog
   */
  const schema_t& get_schema() const {
    return schema_;
  }

  /**
   * Gets the number of records in the atomic multilog
   * @return The number of records
   */
  size_t num_records() const {
    return rt_.get() / schema_.record_size();
  }

  /**
   * Gets the record size
   * @return The record size of the schema
   */
  size_t record_size() const {
    return schema_.record_size();
  }

 protected:
  /**
   * Updates the record block
   * @param log_offset The offset of the log
   * @param block The record block
   * @param record_size The size of each record
   */
  void update_aux_record_block(uint64_t log_offset, record_block& block,
                               size_t record_size) {
    schema_snapshot snap = schema_.snapshot();
    for (size_t i = 0; i < filters_.size(); i++) {
      if (filters_.at(i)->is_valid()) {
        filters_.at(i)->update(log_offset, snap, block, record_size);
      }
    }

    for (size_t i = 0; i < schema_.size(); i++) {
      if (snap.is_indexed(i)) {
        radix_index* idx = indexes_.at(snap.index_id(i));
        // Handle timestamp differently
        // TODO: What if indexing requested for finer granularity?
        if (i == 0) {  // Timestamp
          auto& refs = idx->get_or_create(snap.time_key(block.time_block));
          size_t idx = refs->reserve(block.nrecords);
          for (size_t j = 0; j < block.nrecords; j++) {
            refs->set(idx + j, log_offset + j * record_size);
          }
        } else {
          for (size_t j = 0; j < block.nrecords; j++) {
            size_t block_offset = j * record_size;
            size_t record_offset = log_offset + block_offset;
            void* rec_ptr = reinterpret_cast<uint8_t*>(&block.data[0])
                + block_offset;
            idx->insert(snap.get_key(rec_ptr, i), record_offset);
          }
        }
      }
    }
  }

  /**
   * Monitors the task executed on the atomic multilog
   */
  void monitor_task() {
    uint64_t cur_ms = time_utils::cur_ms();
    uint64_t version = rt_.get();
    size_t nfilters = filters_.size();
    for (size_t i = 0; i < nfilters; i++) {
      filter* f = filters_.at(i);
      if (f->is_valid()) {
        size_t ntriggers = f->num_triggers();
        for (size_t tid = 0; tid < ntriggers; tid++) {
          trigger* t = f->get_trigger(tid);
          if (t->is_valid() && cur_ms % t->periodicity_ms() == 0) {
            for (uint64_t ms = cur_ms - configuration_params::MONITOR_WINDOW_MS;
                ms <= cur_ms; ms++) {
              if (ms % t->periodicity_ms() == 0) {
                check_time_bucket(f, t, tid, cur_ms, version);
              }
            }
          }
        }
      }
    }
  }

  void check_time_bucket(filter* f, trigger* t, size_t tid,
                         uint64_t time_bucket, uint64_t version) {
    size_t window_size = t->periodicity_ms();
    for (uint64_t ms = time_bucket - window_size; ms <= time_bucket; ms++) {
      const aggregated_reflog* ar = f->lookup(ms);
      if (ar != nullptr) {
        numeric agg = ar->get_aggregate(tid, version);
        if (numeric::relop(t->op(), agg, t->threshold())) {
          alerts_.add_alert(ms, t->trigger_name(), t->trigger_expr(), agg,
                            version);
        }
      }
    }
  }

  std::string name_;
  schema_type schema_;

  data_log_type data_log_;
  read_tail_type rt_;
  metadata_writer_type metadata_;

  // In memory structures
  filter_log filters_;
  index_log indexes_;
  alert_index alerts_;

  string_map<filter_id_t> filter_map_;
  string_map<trigger_id_t> trigger_map_;

  query_planner planner_;

  // Manangement
  task_pool& mgmt_pool_;
  periodic_task monitor_task_;
};

}

#endif /* DIALOG_DIALOG_TABLE_H_ */
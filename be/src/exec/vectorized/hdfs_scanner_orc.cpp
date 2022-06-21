// This file is licensed under the Elastic License 2.0. Copyright 2021-present, StarRocks Limited.

#include "exec/vectorized/hdfs_scanner_orc.h"

#include <utility>

#include "exec/exec_node.h"
#include "fs/fs.h"
#include "gen_cpp/orc_proto.pb.h"
#include "storage/chunk_helper.h"
#include "util/runtime_profile.h"
#include "util/timezone_utils.h"

namespace starrocks::vectorized {
class ORCHdfsFileStream : public orc::InputStream {
public:
    // |file| must outlive ORCHdfsFileStream
    ORCHdfsFileStream(RandomAccessFile* file, uint64_t length, HdfsScanStats* stats)
            : _file(std::move(file)), _length(length), _stats(stats), _cache_buffer(0), _cache_offset(0) {}

    ~ORCHdfsFileStream() override = default;

    uint64_t getLength() const override { return _length; }

    // refers to paper `Delta Lake: High-Performance ACID Table Storage over Cloud Object Stores`
    uint64_t getNaturalReadSize() const override { return 1 * 1024 * 1024; }

    // It's for read size after doing seek.
    // When doing read after seek, we make assumption that we are doing random read because of seeking row group.
    // And if we still use NaturalReadSize we probably read many row groups
    // after the row group we want to read, and that will amplify read IO bytes.

    // So the best way is to reduce read size, hopefully we just read that row group in one shot.
    // We also have chance that we may not read enough at this shot, then we fallback to NaturalReadSize to read.
    // The cost is, there is a extra IO, and we read 1/4 of NaturalReadSize more data.
    // And the potential gain is, we save 3/4 of NaturalReadSize IO bytes.

    // Normally 256K can cover a row group of a column(like integer or double, but maybe not string)
    // And this value can not be too small because if we can not read a row group in a single shot,
    // we will fallback to read in normal size, and we pay cost of a extra read.

    uint64_t getNaturalReadSizeAfterSeek() const override { return 256 * 1024; }

    void prepareCache(orc::InputStream::PrepareCacheScope scope, uint64_t offset, uint64_t length) override {
        const size_t cache_max_size = config::orc_file_cache_max_size;
        if (length > cache_max_size) return;
        if (canUseCacheBuffer(offset, length)) return;

        // If this stripe is small, probably other stripes are also small
        // we combine those reads into one, and try to read several stripes in one shot.
        if (scope == orc::InputStream::PrepareCacheScope::READ_FULL_STRIPE) {
            length = std::min(_length - offset, cache_max_size);
        }

        _cache_buffer.resize(length);
        _cache_offset = offset;
        doRead(_cache_buffer.data(), length, offset);
    }

    bool canUseCacheBuffer(uint64_t offset, uint64_t length) {
        if ((_cache_buffer.size() != 0) && (offset >= _cache_offset) &&
            ((offset + length) <= (_cache_offset + _cache_buffer.size()))) {
            return true;
        }
        return false;
    }

    void read(void* buf, uint64_t length, uint64_t offset) override {
        if (canUseCacheBuffer(offset, length)) {
            size_t idx = offset - _cache_offset;
            memcpy(buf, _cache_buffer.data() + idx, length);
        } else {
            doRead(buf, length, offset);
        }
    }

    void doRead(void* buf, uint64_t length, uint64_t offset) {
        SCOPED_RAW_TIMER(&_stats->io_ns);
        _stats->io_count += 1;
        if (buf == nullptr) {
            throw orc::ParseError("Buffer is null");
        }

        Status status = _file->read_at_fully(offset, buf, length);
        if (!status.ok()) {
            auto msg = strings::Substitute("Failed to read $0: $1", _file->filename(), status.to_string());
            throw orc::ParseError(msg);
        }
        _stats->bytes_read += length;
    }

    const std::string& getName() const override { return _file->filename(); }

private:
    RandomAccessFile* _file;
    uint64_t _length;
    HdfsScanStats* _stats;
    std::vector<char> _cache_buffer;
    uint64_t _cache_offset;
};

class OrcRowReaderFilter : public orc::RowReaderFilter {
public:
    OrcRowReaderFilter(const HdfsScannerParams& scanner_params, const HdfsScannerContext& scanner_ctx,
                       OrcChunkReader* reader);
    bool filterOnOpeningStripe(uint64_t stripeIndex, const orc::proto::StripeInformation* stripeInformation) override;
    bool filterOnPickRowGroup(size_t rowGroupIdx, const std::unordered_map<uint64_t, orc::proto::RowIndex>& rowIndexes,
                              const std::map<uint32_t, orc::BloomFilterIndex>& bloomFilters) override;
    bool filterMinMax(size_t rowGroupIdx, const std::unordered_map<uint64_t, orc::proto::RowIndex>& rowIndexes,
                      const std::map<uint32_t, orc::BloomFilterIndex>& bloomFilter);
    bool filterOnPickStringDictionary(const std::unordered_map<uint64_t, orc::StringDictionary*>& sdicts) override;

    bool is_slot_evaluated(SlotId id) { return _dict_filter_eval_cache.find(id) != _dict_filter_eval_cache.end(); }
    void onStartingPickRowGroups() override;
    void onEndingPickRowGroups() override;
    void setWriterTimezone(const std::string& tz) override;

private:
    const HdfsScannerParams& _scanner_params;
    const HdfsScannerContext& _scanner_ctx;
    uint64_t _current_stripe_index;
    bool _init_use_dict_filter_slots;
    std::vector<pair<SlotDescriptor*, uint64_t>> _use_dict_filter_slots;
    friend class HdfsOrcScanner;
    std::unordered_map<SlotId, FilterPtr> _dict_filter_eval_cache;
    bool _can_do_filter_on_orc_cvb; // cvb: column vector batch.
    // key: end of range.
    // value: start of range.
    // ranges are not overlapped.
    // check `offset` in a range or not:
    // 1. use `upper_bound` to find the first range.end > `offset`
    // 2. and check if range.start <= `offset`
    std::map<uint64_t, uint64_t> _scan_ranges;
    OrcChunkReader* _reader;
    int64_t _writer_tzoffset_in_seconds;
};

void OrcRowReaderFilter::onStartingPickRowGroups() {}

void OrcRowReaderFilter::onEndingPickRowGroups() {}

void OrcRowReaderFilter::setWriterTimezone(const std::string& tz) {
    cctz::time_zone writer_tzinfo;
    if (tz == "" || !TimezoneUtils::find_cctz_time_zone(tz, writer_tzinfo)) {
        _writer_tzoffset_in_seconds = _reader->tzoffset_in_seconds();
    } else {
        _writer_tzoffset_in_seconds = TimezoneUtils::to_utc_offset(writer_tzinfo);
    }
}

OrcRowReaderFilter::OrcRowReaderFilter(const HdfsScannerParams& scanner_params, const HdfsScannerContext& scanner_ctx,
                                       OrcChunkReader* reader)
        : _scanner_params(scanner_params),
          _scanner_ctx(scanner_ctx),
          _current_stripe_index(0),
          _init_use_dict_filter_slots(false),
          _can_do_filter_on_orc_cvb(true),
          _reader(reader),
          _writer_tzoffset_in_seconds(reader->tzoffset_in_seconds()) {
    if (_scanner_params.min_max_tuple_desc != nullptr) {
        VLOG_FILE << "OrcRowReaderFilter: min_max_tuple_desc = " << _scanner_params.min_max_tuple_desc->debug_string();
        for (ExprContext* ctx : _scanner_params.min_max_conjunct_ctxs) {
            VLOG_FILE << "OrcRowReaderFilter: min_max_ctx = " << ctx->root()->debug_string();
        }
    }
    for (const auto& r : _scanner_params.scan_ranges) {
        _scan_ranges.insert(std::make_pair(r->offset + r->length, r->offset));
    }
}

bool OrcRowReaderFilter::filterOnOpeningStripe(uint64_t stripeIndex,
                                               const orc::proto::StripeInformation* stripeInformation) {
    _current_stripe_index = stripeIndex;
    uint64_t offset = stripeInformation->offset();
    // range end must > offset
    auto it = _scan_ranges.upper_bound(offset);
    if ((it != _scan_ranges.end()) && (offset >= it->second) && (offset < it->first)) {
        return false;
    }
    return true;
}

bool OrcRowReaderFilter::filterMinMax(size_t rowGroupIdx,
                                      const std::unordered_map<uint64_t, orc::proto::RowIndex>& rowIndexes,
                                      const std::map<uint32_t, orc::BloomFilterIndex>& bloomFilter) {
    TupleDescriptor* min_max_tuple_desc = _scanner_params.min_max_tuple_desc;
    ChunkPtr min_chunk = vectorized::ChunkHelper::new_chunk(*min_max_tuple_desc, 0);
    ChunkPtr max_chunk = vectorized::ChunkHelper::new_chunk(*min_max_tuple_desc, 0);
    for (size_t i = 0; i < min_max_tuple_desc->slots().size(); i++) {
        SlotDescriptor* slot = min_max_tuple_desc->slots()[i];
        int32_t column_index = _reader->get_column_id_by_name(slot->col_name());
        if (column_index >= 0) {
            auto row_idx_iter = rowIndexes.find(column_index);
            // there is no column stats, skip filter process.
            if (row_idx_iter == rowIndexes.end()) {
                return false;
            }
            const orc::proto::ColumnStatistics& stats = row_idx_iter->second.entry(rowGroupIdx).statistics();
            ColumnPtr min_col = min_chunk->columns()[i];
            ColumnPtr max_col = max_chunk->columns()[i];
            DCHECK(!min_col->is_constant() && !max_col->is_constant());
            int64_t tz_offset_in_seconds = _reader->tzoffset_in_seconds() - _writer_tzoffset_in_seconds;
            Status st = _reader->decode_min_max_value(slot, stats, min_col, max_col, tz_offset_in_seconds);
            if (!st.ok()) {
                return false;
            }
        } else {
            // search partition columns.
            int part_idx = 0;
            const int part_size = _scanner_ctx.partition_columns.size();
            for (part_idx = 0; part_idx < part_size; part_idx++) {
                if (_scanner_ctx.partition_columns[part_idx].col_name == slot->col_name()) {
                    break;
                }
            }
            // not found in partition columns.
            if (part_idx == part_size) {
                min_chunk->columns()[i]->append_nulls(1);
                max_chunk->columns()[i]->append_nulls(1);
            } else {
                auto* const_column = vectorized::ColumnHelper::as_raw_column<vectorized::ConstColumn>(
                        _scanner_ctx.partition_values[part_idx]);
                min_chunk->columns()[i]->append(*const_column->data_column(), 0, 1);
                max_chunk->columns()[i]->append(*const_column->data_column(), 0, 1);
            }
        }
    }

    VLOG_FILE << "stripe = " << _current_stripe_index << ", row_group = " << rowGroupIdx
              << ", min_chunk = " << min_chunk->debug_row(0) << ", max_chunk = " << max_chunk->debug_row(0);
    for (auto& min_max_conjunct_ctx : _scanner_ctx.min_max_conjunct_ctxs) {
        // TODO: add a warning log here
        auto min_col = EVALUATE_NULL_IF_ERROR(min_max_conjunct_ctx, min_max_conjunct_ctx->root(), min_chunk.get());
        auto max_col = EVALUATE_NULL_IF_ERROR(min_max_conjunct_ctx, min_max_conjunct_ctx->root(), max_chunk.get());
        auto min = min_col->get(0).get_int8();
        auto max = max_col->get(0).get_int8();
        if (min == 0 && max == 0) {
            return true;
        }
    }
    return false;
}
bool OrcRowReaderFilter::filterOnPickRowGroup(size_t rowGroupIdx,
                                              const std::unordered_map<uint64_t, orc::proto::RowIndex>& rowIndexes,
                                              const std::map<uint32_t, orc::BloomFilterIndex>& bloomFilters) {
    if (_scanner_params.min_max_tuple_desc != nullptr) {
        if (filterMinMax(rowGroupIdx, rowIndexes, bloomFilters)) {
            VLOG_FILE << "OrcRowReaderFilter: skip row group " << rowGroupIdx << ", stripe " << _current_stripe_index;
            return true;
        }
    }
    return false;
}

// Hive ORC char type will pad trailing spaces.
// https://docs.cloudera.com/documentation/enterprise/6/6.3/topics/impala_char.html
static inline size_t remove_trailing_spaces(const char* s, size_t size) {
    while (size > 0 && s[size - 1] == ' ') size--;
    return size;
}

bool OrcRowReaderFilter::filterOnPickStringDictionary(
        const std::unordered_map<uint64_t, orc::StringDictionary*>& sdicts) {
    if (sdicts.empty()) return false;

    if (!_init_use_dict_filter_slots) {
        for (auto& col : _scanner_ctx.materialized_columns) {
            SlotDescriptor* slot = col.slot_desc;
            if (!_scanner_ctx.can_use_dict_filter_on_slot(slot)) {
                continue;
            }
            int32_t column_index = _reader->get_column_id_by_name(col.col_name);
            if (column_index < 0) {
                continue;
            }
            _use_dict_filter_slots.emplace_back(std::make_pair(slot, column_index));
        }
        _init_use_dict_filter_slots = true;
    }

    _dict_filter_eval_cache.clear();

    for (auto& p : _use_dict_filter_slots) {
        SlotDescriptor* slot_desc = p.first;
        SlotId slot_id = slot_desc->id();
        uint64_t column_index = p.second;
        const auto& it = sdicts.find(column_index);
        if (it == sdicts.end()) {
            continue;
        }
        // create chunk
        orc::StringDictionary* dict = it->second;
        if (dict->dictionaryOffset.size() > _reader->runtime_state()->chunk_size()) {
            continue;
        }
        vectorized::ChunkPtr dict_value_chunk = std::make_shared<vectorized::Chunk>();
        // always assume there is a possibility of null value in ORC column.
        // and we evaluate with null always.
        ColumnPtr column_ptr = vectorized::ColumnHelper::create_column(slot_desc->type(), true);
        dict_value_chunk->append_column(column_ptr, slot_id);

        NullableColumn* nullable_column = down_cast<NullableColumn*>(column_ptr.get());
        BinaryColumn* dict_value_column = down_cast<BinaryColumn*>(nullable_column->data_column().get());

        // copy dict and offset to column.
        Bytes& bytes = dict_value_column->get_bytes();
        Offsets& offsets = dict_value_column->get_offset();

        const char* content_data = dict->dictionaryBlob.data();
        size_t content_size = dict->dictionaryBlob.size();
        bytes.reserve(content_size);
        const uint8_t* start = reinterpret_cast<const uint8_t*>(content_data);
        const uint8_t* end = reinterpret_cast<const uint8_t*>(content_data + content_size);

        size_t offset_size = dict->dictionaryOffset.size();
        size_t dict_size = offset_size - 1;
        const int64_t* offset_data = dict->dictionaryOffset.data();
        offsets.resize(offset_size);

        if (slot_desc->type().type == TYPE_CHAR) {
            // for char type, dict strings are also padded with spaces.
            // we also have to strip space off. For example
            // | hello      |  world      | yes     |, we have to compact to
            // | hello | world | yes |
            size_t total_size = 0;
            const char* p_start = reinterpret_cast<const char*>(start);
            for (size_t i = 0; i < dict_size; i++) {
                const char* s = p_start + offset_data[i];
                size_t old_size = offset_data[i + 1] - offset_data[i];
                size_t new_size = remove_trailing_spaces(s, old_size);
                bytes.insert(bytes.end(), s, s + new_size);
                offsets[i] = total_size;
                total_size += new_size;
            }
            offsets[dict_size] = total_size;
        } else {
            bytes.insert(bytes.end(), start, end);
            // type mismatch, have to use loop to assign.
            for (size_t i = 0; i < offset_size; i++) {
                offsets[i] = offset_data[i];
            }
        }

        // first (dict_size) th items are all not-null
        nullable_column->null_column()->append_default(dict_size);
        // and last one is null.
        nullable_column->append_default();
        DCHECK(nullable_column->size() == (dict_size + 1));

        VLOG_FILE << "OrcRowReaderFilter: stripe = " << _current_stripe_index
                  << ", slot = " << slot_desc->debug_string()
                  << ", dict values = " << dict_value_column->debug_string();

        FilterPtr* filter_ptr = nullptr;
        if (_can_do_filter_on_orc_cvb) {
            // not sure it's best practice.
            FilterPtr tmp;
            _dict_filter_eval_cache[slot_id] = tmp;
            filter_ptr = &_dict_filter_eval_cache[slot_id];
        }

        // do evaluation with dictionary.
        ExecNode::eval_conjuncts(_scanner_ctx.conjunct_ctxs_by_slot.at(slot_id), dict_value_chunk.get(), filter_ptr);
        if (dict_value_chunk->num_rows() == 0) {
            // release memory early.
            _dict_filter_eval_cache.clear();
            VLOG_FILE << "OrcRowReaderFilter: skip stripe by dict filter, stripe " << _current_stripe_index
                      << ", on slot = " << slot_desc->debug_string();
            return true;
        }
        DCHECK(filter_ptr->get() != nullptr);
    }

    return false;
}

Status HdfsOrcScanner::do_open(RuntimeState* runtime_state) {
    auto input_stream =
            std::make_unique<ORCHdfsFileStream>(_file.get(), _scanner_params.scan_ranges[0]->file_length, &_stats);
    SCOPED_RAW_TIMER(&_stats.reader_init_ns);
    std::unique_ptr<orc::Reader> reader;
    try {
        orc::ReaderOptions options;
        reader = orc::createReader(std::move(input_stream), options);
    } catch (std::exception& e) {
        auto s = strings::Substitute("HdfsOrcScanner::do_open failed. reason = $0", e.what());
        LOG(WARNING) << s;
        return Status::InternalError(s);
    }

    std::unordered_set<std::string> known_column_names;
    OrcChunkReader::build_column_name_set(&known_column_names, _scanner_params.hive_column_names, reader->getType());
    _scanner_ctx.set_columns_from_file(known_column_names);
    ASSIGN_OR_RETURN(auto skip, _scanner_ctx.should_skip_by_evaluating_not_existed_slots());
    if (skip) {
        LOG(INFO) << "HdfsOrcScanner: do_open. skip file for non existed slot conjuncts.";
        _should_skip_file = true;
        // no need to intialize following context.
        return Status::OK();
    }

    // create orc reader for further reading.
    int src_slot_index = 0;
    bool has_conjunct_ctxs_by_slot = (_conjunct_ctxs_by_slot.size() != 0);
    for (const auto& it : _scanner_params.materialize_slots) {
        const std::string& name = it->col_name();
        if (known_column_names.find(name) == known_column_names.end()) continue;
        if (has_conjunct_ctxs_by_slot && _conjunct_ctxs_by_slot.find(it->id()) == _conjunct_ctxs_by_slot.end()) {
            _lazy_load_ctx.lazy_load_slots.emplace_back(it);
            _lazy_load_ctx.lazy_load_indices.emplace_back(src_slot_index);
            // reserve room for later set in `OrcChunkReader`
            _lazy_load_ctx.lazy_load_orc_positions.emplace_back(0);
        } else {
            _lazy_load_ctx.active_load_slots.emplace_back(it);
            _lazy_load_ctx.active_load_indices.emplace_back(src_slot_index);
            // reserve room for later set in `OrcChunkReader`
            _lazy_load_ctx.active_load_orc_positions.emplace_back(0);
        }
        _src_slot_descriptors.emplace_back(it);
        src_slot_index++;
    }

    _orc_reader = std::make_unique<OrcChunkReader>(runtime_state, _src_slot_descriptors);
    _orc_row_reader_filter = std::make_shared<OrcRowReaderFilter>(_scanner_params, _scanner_ctx, _orc_reader.get());
    _orc_reader->disable_broker_load_mode();
    _orc_reader->set_row_reader_filter(_orc_row_reader_filter);
    _orc_reader->set_read_chunk_size(runtime_state->chunk_size());
    _orc_reader->set_runtime_state(runtime_state);
    _orc_reader->set_current_file_name(_scanner_params.scan_ranges[0]->relative_path);
    RETURN_IF_ERROR(_orc_reader->set_timezone(_scanner_ctx.timezone));
    if (_use_orc_sargs) {
        std::vector<Expr*> conjuncts;
        for (const auto& it : _conjunct_ctxs_by_slot) {
            for (const auto& it2 : it.second) {
                conjuncts.push_back(it2->root());
            }
        }
        _orc_reader->set_conjuncts_and_runtime_filters(conjuncts, _scanner_params.runtime_filter_collector);
    }
    _orc_reader->set_hive_column_names(_scanner_params.hive_column_names);
    if (config::enable_orc_late_materialization && _lazy_load_ctx.lazy_load_slots.size() != 0) {
        _orc_reader->set_lazy_load_context(&_lazy_load_ctx);
    }
    RETURN_IF_ERROR(_orc_reader->init(std::move(reader)));
    return Status::OK();
}

void HdfsOrcScanner::do_close(RuntimeState* runtime_state) noexcept {
    _orc_reader.reset(nullptr);
}

Status HdfsOrcScanner::do_get_next(RuntimeState* runtime_state, ChunkPtr* chunk) {
    CHECK(chunk != nullptr);
    if (_should_skip_file) {
        return Status::EndOfFile("");
    }

    ChunkPtr ck = std::make_shared<vectorized::Chunk>();
    // The column order of chunk is required to be invariable. When a table performs schema change,
    // says we want to add a new column to table A, the reader of old files of table A will append new
    // column to the tail of the chunk, while the reader of new files of table A will put the new column
    // in the chunk according to the order it's stored. This will lead two chunk from different file to
    // different column order, so we need to adjust the column order according to the input chunk to make
    // sure every chunk has same column order
    auto convert_to_output = [chunk, &ck]() {
        const auto& slot_id_to_index_map = (*chunk)->get_slot_id_to_index_map();
        for (auto iter = slot_id_to_index_map.begin(); iter != slot_id_to_index_map.end(); iter++) {
            (*chunk)->get_column_by_slot_id(iter->first)->swap_column(*ck->get_column_by_slot_id(iter->first));
        }
    };

    // this infinite for loop is for retry.
    for (;;) {
        orc::RowReader::ReadPosition position;
        size_t read_num_values = 0;
        bool has_used_dict_filter = false;
        {
            SCOPED_RAW_TIMER(&_stats.column_read_ns);
            RETURN_IF_ERROR(_orc_reader->read_next(&position));
            // read num values is how many rows actually read before doing dict filtering.
            read_num_values = position.num_values;
            RETURN_IF_ERROR(_orc_reader->apply_dict_filter_eval_cache(_orc_row_reader_filter->_dict_filter_eval_cache,
                                                                      &_dict_filter));
            if (_orc_reader->get_cvb_size() != read_num_values) {
                has_used_dict_filter = true;
            }
        }

        size_t chunk_size = 0;
        if (_orc_reader->get_cvb_size() != 0) {
            {
                StatusOr<ChunkPtr> ret;
                SCOPED_RAW_TIMER(&_stats.column_convert_ns);
                if (!_orc_reader->has_lazy_load_context()) {
                    ret = _orc_reader->get_chunk();
                } else {
                    ret = _orc_reader->get_active_chunk();
                }
                RETURN_IF_ERROR(ret);
                ck = std::move(ret.value());
            }

            // important to add columns before evaluation
            // because ctxs_by_slot maybe refers to some non-existed slot or partition slot.
            _scanner_ctx.append_not_existed_columns_to_chunk(&ck, ck->num_rows());
            _scanner_ctx.append_partition_column_to_chunk(&ck, ck->num_rows());
            chunk_size = ck->num_rows();
            // do stats before we filter rows which does not match.
            _stats.raw_rows_read += chunk_size;
            _chunk_filter.assign(chunk_size, 1);
            {
                SCOPED_RAW_TIMER(&_stats.expr_filter_ns);
                for (auto& it : _scanner_ctx.conjunct_ctxs_by_slot) {
                    // do evaluation.
                    if (_orc_row_reader_filter->is_slot_evaluated(it.first)) {
                        continue;
                    }
                    ASSIGN_OR_RETURN(chunk_size,
                                     ExecNode::eval_conjuncts_into_filter(it.second, ck.get(), &_chunk_filter));
                    if (chunk_size == 0) {
                        break;
                    }
                }
            }
            if (chunk_size != 0 && chunk_size != ck->num_rows()) {
                ck->filter(_chunk_filter);
            }
        }
        ck->set_num_rows(chunk_size);

        if (!_orc_reader->has_lazy_load_context()) {
            convert_to_output();
            return Status::OK();
        }

        // if has lazy load fields, skip it if chunk_size == 0
        if (chunk_size == 0) {
            continue;
        }
        {
            SCOPED_RAW_TIMER(&_stats.column_read_ns);
            _orc_reader->lazy_seek_to(position.row_in_stripe);
            _orc_reader->lazy_read_next(read_num_values);
        }
        {
            SCOPED_RAW_TIMER(&_stats.column_convert_ns);
            if (has_used_dict_filter) {
                _orc_reader->lazy_filter_on_cvb(&_dict_filter);
            }
            _orc_reader->lazy_filter_on_cvb(&_chunk_filter);
            StatusOr<ChunkPtr> ret = _orc_reader->get_lazy_chunk();
            RETURN_IF_ERROR(ret);
            Chunk& ret_ck = *(ret.value());
            ck->merge(std::move(ret_ck));
        }
        convert_to_output();
        return Status::OK();
    }
    __builtin_unreachable();
    return Status::OK();
}

Status HdfsOrcScanner::do_init(RuntimeState* runtime_state, const HdfsScannerParams& scanner_params) {
    _should_skip_file = false;
    _use_orc_sargs = true;
    // todo: build predicate hook and ranges hook.
    return Status::OK();
}
} // namespace starrocks::vectorized

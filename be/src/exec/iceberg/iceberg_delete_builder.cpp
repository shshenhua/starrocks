// Copyright 2021-present StarRocks, Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "exec/iceberg/iceberg_delete_builder.h"

#include "column/vectorized_fwd.h"
#include "exec/hdfs_scanner.h"
#include "exec/iceberg/iceberg_delete_file_iterator.h"
#include "formats/orc/orc_chunk_reader.h"
#include "formats/orc/orc_input_stream.h"
#include "formats/parquet/file_reader.h"
#include "gen_cpp/Types_types.h"
#include "runtime/descriptors.h"
#include "storage/chunk_helper.h"

namespace starrocks {

struct IcebergColumnMeta {
    int64_t id;
    std::string col_name;
    TPrimitiveType::type type;
};

static const IcebergColumnMeta k_delete_file_path{
        .id = INT32_MAX - 101, .col_name = "file_path", .type = TPrimitiveType::VARCHAR};

static const IcebergColumnMeta k_delete_file_pos{
        .id = INT32_MAX - 102, .col_name = "pos", .type = TPrimitiveType::BIGINT};

Status ParquetPositionDeleteBuilder::build(const std::string& timezone, const std::string& delete_file_path,
                                           int64_t file_length, std::set<int64_t>* need_skip_rowids) {
    std::vector<SlotDescriptor*> slot_descriptors{&(IcebergDeleteFileMeta::get_delete_file_path_slot()),
                                                  &(IcebergDeleteFileMeta::get_delete_file_pos_slot())};
    auto iter = std::make_unique<IcebergDeleteFileIterator>();
    RETURN_IF_ERROR(iter->init(_fs, timezone, delete_file_path, file_length, slot_descriptors, true));
    std::shared_ptr<::arrow::RecordBatch> batch;

    Status status;
    while (true) {
        status = iter->has_next();
        if (!status.ok()) {
            break;
        }

        batch = iter->next();
        ::arrow::StringArray* file_path_array = static_cast<arrow::StringArray*>(batch->column(0).get());
        ::arrow::Int64Array* pos_array = static_cast<arrow::Int64Array*>(batch->column(1).get());
        for (size_t row = 0; row < batch->num_rows(); row++) {
            if (file_path_array->Value(row) == _datafile_path) {
                need_skip_rowids->emplace(pos_array->Value(row));
            }
        }
    }

    // eof is expected, otherwise propagate error
    if (!status.is_end_of_file()) {
        LOG(WARNING) << status;
        return status;
    }
    return Status::OK();
}

Status ORCPositionDeleteBuilder::build(const std::string& timezone, const std::string& delete_file_path,
                                       int64_t file_length, std::set<int64_t>* need_skip_rowids) {
    std::vector<SlotDescriptor*> slot_descriptors{&(IcebergDeleteFileMeta::get_delete_file_path_slot()),
                                                  &(IcebergDeleteFileMeta::get_delete_file_pos_slot())};

    std::unique_ptr<RandomAccessFile> file;
    ASSIGN_OR_RETURN(file, _fs->new_random_access_file(delete_file_path));

    auto input_stream = std::make_unique<ORCHdfsFileStream>(file.get(), file_length, nullptr);
    std::unique_ptr<orc::Reader> reader;
    try {
        orc::ReaderOptions options;
        reader = orc::createReader(std::move(input_stream), options);
    } catch (std::exception& e) {
        auto s =
                strings::Substitute("ORCPositionDeleteBuilder::build create orc::Reader failed. reason = $0", e.what());
        LOG(WARNING) << s;
        return Status::InternalError(s);
    }

    auto orc_reader = std::make_unique<OrcChunkReader>(4096, slot_descriptors);
    orc_reader->disable_broker_load_mode();
    orc_reader->set_current_file_name(delete_file_path);
    RETURN_IF_ERROR(orc_reader->set_timezone(timezone));
    RETURN_IF_ERROR(orc_reader->init(std::move(reader)));

    orc::RowReader::ReadPosition position;
    Status s;

    while (true) {
        s = orc_reader->read_next(&position);
        if (s.is_end_of_file()) {
            return Status::OK();
        }

        RETURN_IF_ERROR(s);

        auto ret = orc_reader->get_chunk();
        if (!ret.ok()) {
            return ret.status();
        }

        ChunkPtr chunk = ret.value();
        size_t chunk_size = chunk->num_rows();
        const auto& slot_id_to_idx = chunk->get_slot_id_to_index_map();
        if (!slot_id_to_idx.contains(k_delete_file_path.id) || !slot_id_to_idx.contains(k_delete_file_pos.id)) {
            auto str = strings::Substitute("delete file schema doesn't meet requirement, need: [file_path, pos]");
            LOG(WARNING) << str;
            return Status::InternalError(str);
        }

        auto* file_path_col = static_cast<BinaryColumn*>(chunk->get_column_by_slot_id(k_delete_file_path.id).get());
        auto* position_col = static_cast<Int64Column*>(chunk->get_column_by_slot_id(k_delete_file_pos.id).get());
        for (auto row = 0; row < chunk_size; row++) {
            if (file_path_col->get_slice(row) != _datafile_path) {
                continue;
            }
            need_skip_rowids->emplace(position_col->get_data()[row]);
        }
    }
}

Status ORCEqualityDeleteBuilder::build(const std::string& timezone, const std::string& delete_file_path,
                                       int64_t file_length, const std::shared_ptr<DefaultMORProcessor> mor_processor,
                                       std::vector<SlotDescriptor*> slot_descs,
                                       TupleDescriptor* delete_column_tuple_desc,
                                       const TIcebergSchema* iceberg_equal_delete_schema, RuntimeState* state) {
    std::unique_ptr<RandomAccessFile> file;
    ASSIGN_OR_RETURN(file, _fs->new_random_access_file(delete_file_path));

    auto input_stream = std::make_unique<ORCHdfsFileStream>(file.get(), file_length, nullptr);
    std::unique_ptr<orc::Reader> reader;
    try {
        const orc::ReaderOptions options;
        reader = createReader(std::move(input_stream), options);
    } catch (std::exception& e) {
        const auto s =
                strings::Substitute("ORCEqualityDeleteBuilder::build create orc::Reader failed. reason = $0", e.what());
        LOG(WARNING) << s;
        return Status::InternalError(s);
    }

    const auto orc_reader = std::make_unique<OrcChunkReader>(4096, slot_descs);
    orc_reader->disable_broker_load_mode();
    orc_reader->set_current_file_name(delete_file_path);
    RETURN_IF_ERROR(orc_reader->set_timezone(timezone));
    RETURN_IF_ERROR(orc_reader->init(std::move(reader)));

    orc::RowReader::ReadPosition position;

    while (true) {
        Status status = orc_reader->read_next(&position);
        if (status.is_end_of_file()) {
            break;
        }

        RETURN_IF_ERROR(status);

        auto ret = orc_reader->get_chunk();
        if (!ret.ok()) {
            return ret.status();
        }

        ChunkPtr chunk = ret.value();
        RETURN_IF_ERROR(mor_processor->append_chunk_to_hashtable(state, chunk));
    }
    return Status::OK();
}

Status ParquetEqualityDeleteBuilder::build(const std::string& timezone, const std::string& delete_file_path,
                                           int64_t file_length,
                                           const std::shared_ptr<DefaultMORProcessor> mor_processor,
                                           std::vector<SlotDescriptor*> slot_descs,
                                           TupleDescriptor* delete_column_tuple_desc,
                                           const TIcebergSchema* iceberg_equal_delete_schema, RuntimeState* state) {
    std::unique_ptr<RandomAccessFile> file;
    ASSIGN_OR_RETURN(file, _fs->new_random_access_file(delete_file_path));

    std::unique_ptr<parquet::FileReader> reader;
    try {
        reader = std::make_unique<parquet::FileReader>(state->chunk_size(), file.get(), file->get_size().value(),
                                                       nullptr, nullptr);
    } catch (std::exception& e) {
        const auto s = strings::Substitute(
                "ParquetEqualityDeleteBuilder::build create parquet::FileReader failed. reason = $0", e.what());
        LOG(WARNING) << s;
        return Status::InternalError(s);
    }

    auto scanner_ctx = std::make_unique<HdfsScannerContext>();
    auto scan_stats = std::make_unique<HdfsScanStats>();
    std::vector<HdfsScannerContext::ColumnInfo> columns;
    THdfsScanRange scan_range;
    scan_range.offset = 0;
    scan_range.length = file_length;
    std::vector<const THdfsScanRange*> scan_ranges = {&scan_range};
    for (size_t i = 0; i < slot_descs.size(); i++) {
        auto* slot = slot_descs[i];
        HdfsScannerContext::ColumnInfo column;
        column.slot_desc = slot;
        column.idx_in_chunk = i;
        column.decode_needed = true;
        columns.emplace_back(column);
    }
    scanner_ctx->timezone = timezone;
    scanner_ctx->stats = scan_stats.get();
    scanner_ctx->tuple_desc = delete_column_tuple_desc;
    scanner_ctx->iceberg_schema = iceberg_equal_delete_schema;
    scanner_ctx->materialized_columns = std::move(columns);
    scanner_ctx->scan_ranges = scan_ranges;
    scanner_ctx->lazy_column_coalesce_counter = &_lazy_column_coalesce_counter;
    RETURN_IF_ERROR(reader->init(scanner_ctx.get()));

    while (true) {
        ChunkPtr chunk = ChunkHelper::new_chunk(*delete_column_tuple_desc, state->chunk_size());
        Status status = reader->get_next(&chunk);
        if (status.is_end_of_file()) {
            break;
        }

        RETURN_IF_ERROR(status);
        ChunkPtr& result = chunk;
        RETURN_IF_ERROR(mor_processor->append_chunk_to_hashtable(state, result));
    }
    return Status::OK();
}

SlotDescriptor IcebergDeleteFileMeta::gen_slot_helper(const IcebergColumnMeta& meta) {
    TSlotDescriptor desc;
    desc.__set_id(meta.id);
    desc.__set_parent(-1);
    TTypeNode type_node;
    type_node.__set_type(TTypeNodeType::SCALAR);
    type_node.__set_scalar_type({});
    type_node.scalar_type.__set_type(meta.type);
    type_node.scalar_type.__set_len(-1);
    desc.__set_slotType({});
    desc.slotType.__set_types({type_node});
    desc.__set_colName(meta.col_name);
    desc.__set_slotIdx(meta.id);
    desc.__set_isMaterialized(true);
    desc.__set_nullIndicatorByte(0);
    desc.__set_nullIndicatorBit(-1);

    return {desc};
}

SlotDescriptor& IcebergDeleteFileMeta::get_delete_file_path_slot() {
    static SlotDescriptor k_delete_file_path_slot = IcebergDeleteFileMeta::gen_slot_helper(k_delete_file_path);

    return k_delete_file_path_slot;
}

SlotDescriptor& IcebergDeleteFileMeta::get_delete_file_pos_slot() {
    static SlotDescriptor k_delete_file_pos_slot = IcebergDeleteFileMeta::gen_slot_helper(k_delete_file_pos);

    return k_delete_file_pos_slot;
}

} // namespace starrocks

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

#pragma once

#include <string>
#include <unordered_map>

#include "storage/olap_common.h"
#include "storage/primary_index.h"
#include "storage/rowset/column_iterator.h"
#include "storage/tablet_updates.h"

namespace starrocks {

class Tablet;
class DeltaColumnGroup;
class FileSystem;
class Segment;
class RandomAccessFile;
class ColumnIterator;

struct RowsetSegmentStat {
    int64_t num_rows_written = 0;
    int64_t total_row_size = 0;
    int64_t total_data_size = 0;
    int64_t total_index_size = 0;
    int64_t num_segment = 0;
};

// This struct can serve as the unique identifier for a specific segment.
// The unique identifier contructed by rowset_id + segment_id.
// There are two kinds of rowset_id in our implementations:
// 1. unique rowset_id, which generated by `rowset_id_generator`.
// 2. sequence rowset_id, from 0 and increment in one tablet
// We record both in this struct for diferent situations.
struct RowsetSegmentId {
    RowsetId unique_rowset_id;
    uint32_t sequence_rowset_id;
    uint32_t segment_id;
};

struct ColumnPartialUpdateState {
    bool inited = false;
    // Maintains the mapping of each row of data in the update segment to source row
    std::vector<uint64_t> src_rss_rowids;
    // this state been generated by this version
    EditVersion read_version;
    // Maintains the mapping of source row to update segment's row
    std::map<uint64_t, uint32_t> rss_rowid_to_update_rowid;
    // Maintains the rowids in update segment, which are need to be inserted
    std::vector<uint32> insert_rowids;

    // build `rss_rowid_to_update_rowid` from `src_rss_rowids`
    void build_rss_rowid_to_update_rowid() {
        rss_rowid_to_update_rowid.clear();
        insert_rowids.clear();
        for (uint32_t upt_row_id = 0; upt_row_id < src_rss_rowids.size(); upt_row_id++) {
            uint64_t each_rss_rowid = src_rss_rowids[upt_row_id];
            // build rssid & rowid -> update file's rowid
            // each_rss_rowid == UINT64_MAX means that key not exist in pk index
            if (each_rss_rowid < UINT64_MAX) {
                rss_rowid_to_update_rowid[each_rss_rowid] = upt_row_id;
            } else {
                insert_rowids.push_back(upt_row_id);
            }
        }
    }
};

using ColumnUniquePtr = std::unique_ptr<Column>;

// It contains multi segment's pk list, segment's id is between [start_idx, end_idx)
struct BatchPKs {
    ColumnUniquePtr upserts;
    uint32_t start_idx;
    uint32_t end_idx;
    std::vector<uint64_t> src_rss_rowids;
    // use offsets to record each segment's position in upserts and src_rss_rowids
    // Last item of offsets is equal to upserts size
    std::vector<size_t> offsets;

    // if this idx is last of this BatchPKs
    bool is_last(uint32_t idx) const { return idx == end_idx - 1; }

    // get src_ress_rowids by idx, and set it to `target_src_rss_rowids`
    void split_src_rss_rowids(uint32_t idx, std::vector<uint64_t>& target_src_rss_rowids) {
        DCHECK(idx - start_idx + 1 < offsets.size());
        target_src_rss_rowids.insert(target_src_rss_rowids.begin(), src_rss_rowids.begin() + offsets[idx - start_idx],
                                     src_rss_rowids.begin() + offsets[idx - start_idx + 1]);
    }

    size_t upserts_size(uint32_t idx) const { return offsets[idx + 1] - offsets[idx]; }
    size_t upserts_size() const { return upserts->size(); }
};

using BatchPKsPtr = std::shared_ptr<BatchPKs>;

// `RowsetColumnUpdateState` is used for maintain the middle state when handling partial update in column mode.
// It will be maintain in update_manager by `DynamicCache<string, RowsetColumnUpdateState>`, mapped from each rowset to it.
// It is created when new rowset is generated, and released when rowset apply finished.
// Because each tablet do apply in one thread, so it wouldn't be updated by multi threads.
class RowsetColumnUpdateState {
public:
    using DeltaColumnGroupPtr = std::shared_ptr<DeltaColumnGroup>;
    // rowid -> <update file id, update_rowids>
    using RowidsToUpdateRowids = std::map<uint32_t, std::pair<uint32_t, uint32_t>>;

    RowsetColumnUpdateState();
    ~RowsetColumnUpdateState();

    RowsetColumnUpdateState(const RowsetColumnUpdateState&) = delete;
    const RowsetColumnUpdateState& operator=(const RowsetColumnUpdateState&) = delete;

    // load primary key column and update data of this rowset.
    // |tablet| : current tablet
    // |rowset| : the rowset that we want to handle it to generate delta column group
    Status load(Tablet* tablet, Rowset* rowset, MemTracker* update_mem_tracker);

    // Generate delta columns by partial update states and rowset,
    // And distribute partial update column data to different `.col` files
    // |tablet| : current tablet
    // |rowset| : the rowset that we want to handle it to generate delta column group
    // |rowset_id| : the new rowset's id
    // |index_meta| : persistent index's meta
    // |delvecs| : new generate delvecs
    // |index| : tablet's primary key index
    Status finalize(Tablet* tablet, Rowset* rowset, uint32_t rowset_id, PersistentIndexMetaPB& index_meta,
                    vector<std::pair<uint32_t, DelVectorPtr>>& delvecs, PrimaryIndex& index);

    std::size_t memory_usage() const { return _memory_usage; }

    std::string to_string() const;

    const std::vector<ColumnPartialUpdateState>& parital_update_states() const { return _partial_update_states; }

    const std::map<uint32_t, DeltaColumnGroupPtr>& delta_column_groups() const { return _rssid_to_delta_column_group; }

    // For UT test now
    const std::vector<BatchPKsPtr>& upserts() const { return _upserts; }

private:
    Status _load_upserts(Rowset* rowset, uint32_t start_idx, uint32_t* end_idx);

    void _release_upserts(uint32_t start_idx, uint32_t end_idx);

    Status _do_load(Tablet* tablet, Rowset* rowset);

    // finalize decide the `ColumnPartialUpdateState` after conflict resolve
    // |tablet| : current tablet
    // |rowset| : the rowset that we want to handle it to generate delta column group
    // |latest_applied_version| : latest apply version of this tablet
    // |index| : tablet's primary key index
    Status _finalize_partial_update_state(Tablet* tablet, Rowset* rowset, EditVersion latest_applied_version,
                                          const PrimaryIndex& index);

    Status _check_and_resolve_conflict(Tablet* tablet, uint32_t rowset_id, uint32_t start_idx, uint32_t end_idx,
                                       EditVersion latest_applied_version, const PrimaryIndex& index);

    StatusOr<std::unique_ptr<SegmentWriter>> _prepare_delta_column_group_writer(Rowset* rowset,
                                                                                std::shared_ptr<TabletSchema> tschema,
                                                                                uint32_t rssid, int64_t ver);

    // to build `_partial_update_states`
    Status _prepare_partial_update_states(Tablet* tablet, Rowset* rowset, uint32_t start_idx, uint32_t end_idx,
                                          bool need_lock);

    // rebuild src_rss_rowids and rss_rowid_to_update_rowid
    Status _resolve_conflict(Tablet* tablet, uint32_t rowset_id, uint32_t start_idx, uint32_t end_idx,
                             EditVersion latest_applied_version, const PrimaryIndex& index);

    /// To reduce memory usage in primary index, we use 32-bit rssid as value.
    /// But we use <RowsetId, segment id> to spell out segment file names,
    /// so we need `_find_rowset_seg_id` and `_init_rowset_seg_id` to build this connection between them.
    // find <RowsetId, segment id> by rssid
    StatusOr<RowsetSegmentId> _find_rowset_seg_id(uint32_t rssid);
    // build the map from rssid to <RowsetId, segment id>
    Status _init_rowset_seg_id(Tablet* tablet);

    Status _read_chunk_from_update(const RowidsToUpdateRowids& rowid_to_update_rowid,
                                   std::vector<ChunkIteratorPtr>& update_iterators, std::vector<uint32_t>& rowids,
                                   Chunk* result_chunk);

    StatusOr<std::unique_ptr<SegmentWriter>> _prepare_segment_writer(Rowset* rowset, const TabletSchema& tablet_schema,
                                                                     int segment_id);

    Status _fill_default_columns(const TabletSchema& tablet_schema, const std::vector<uint32_t>& column_ids,
                                 const int64_t row_cnt, vector<std::shared_ptr<Column>>* columns);
    Status _update_primary_index(const TabletSchema& tablet_schema, Tablet* tablet, const EditVersion& edit_version,
                                 uint32_t rowset_id, std::map<int, ChunkUniquePtr>& segid_to_chunk,
                                 int64_t insert_row_cnt, PersistentIndexMetaPB& index_meta,
                                 vector<std::pair<uint32_t, DelVectorPtr>>& delvecs, PrimaryIndex& index);
    Status _update_rowset_meta(const RowsetSegmentStat& stat, Rowset* rowset);

    Status _insert_new_rows(const TabletSchema& tablet_schema, Tablet* tablet, const EditVersion& edit_version,
                            Rowset* rowset, uint32_t rowset_id, PersistentIndexMetaPB& index_meta,
                            vector<std::pair<uint32_t, DelVectorPtr>>& delvecs, PrimaryIndex& index);

private:
    int64_t _tablet_id = 0;
    std::once_flag _load_once_flag;
    Status _status;
    // it contains primary key seriable column for each update segment file
    std::vector<BatchPKsPtr> _upserts;
    std::vector<ChunkPtr> _update_chunk_cache;
    // total memory usage in current state.
    // it will not record the temp memory usage.
    size_t _memory_usage = 0;

    // maintain the reference from rowids in segment files been updated to rowids in update files.
    std::vector<ColumnPartialUpdateState> _partial_update_states;

    // maintain from rssid to rowsetid & segid
    std::map<uint32_t, RowsetSegmentId> _rssid_to_rowsetid_segid;

    // when generate delta column group finish, these fields will be filled
    bool _finalize_finished = false;
    std::map<uint32_t, DeltaColumnGroupPtr> _rssid_to_delta_column_group;
};

inline std::ostream& operator<<(std::ostream& os, const RowsetColumnUpdateState& o) {
    os << o.to_string();
    return os;
}

} // namespace starrocks

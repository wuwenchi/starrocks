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

#include "utils.h"

#include "column/column.h"
#include "column/datum.h"
#include "exprs/expr.h"
#include "formats/parquet/parquet_file_writer.h"
#include "util/url_coding.h"

namespace starrocks::connector {

StatusOr<std::string> HiveUtils::make_partition_name(
        const std::vector<std::string>& column_names,
        const std::vector<std::unique_ptr<ColumnEvaluator>>& column_evaluators, Chunk* chunk) {
    DCHECK_EQ(column_names.size(), column_evaluators.size());
    std::stringstream ss;
    for (size_t i = 0; i < column_evaluators.size(); i++) {
        ASSIGN_OR_RETURN(auto column, column_evaluators[i]->evaluate(chunk));
        if (column->has_null()) {
            return Status::NotSupported("Partition value can't be null.");
        }
        auto type = column_evaluators[i]->type();
        ASSIGN_OR_RETURN(auto value, column_value(type, column));
        ss << column_names[i] << "=" << value << "/";
    }
    return ss.str();
}

StatusOr<std::string> HiveUtils::make_partition_name_nullable(
        const std::vector<std::string>& column_names,
        const std::vector<std::unique_ptr<ColumnEvaluator>>& column_evaluators, Chunk* chunk) {
    DCHECK_EQ(column_names.size(), column_evaluators.size());
    std::stringstream ss;
    for (size_t i = 0; i < column_evaluators.size(); i++) {
        ASSIGN_OR_RETURN(auto column, column_evaluators[i]->evaluate(chunk));
        auto type = column_evaluators[i]->type();
        ASSIGN_OR_RETURN(auto value, column_value(type, column));
        ss << column_names[i] << "=" << value << "/";
    }
    return ss.str();
}

std::vector<formats::FileColumnId> IcebergUtils::generate_parquet_field_ids(
        const std::vector<TIcebergSchemaField>& fields) {
    std::vector<formats::FileColumnId> file_column_ids(fields.size());
    for (int i = 0; i < fields.size(); ++i) {
        file_column_ids[i].field_id = fields[i].field_id;
        file_column_ids[i].children = generate_parquet_field_ids(fields[i].children);
    }
    return file_column_ids;
}

// TODO(letian-jiang): translate org.apache.hadoop.hive.common.FileUtils#makePartName
StatusOr<std::string> HiveUtils::column_value(const TypeDescriptor& type_desc, const ColumnPtr& column) {
    DCHECK_GT(column->size(), 0);
    auto datum = column->get(0);
    if (datum.is_null()) {
        return "null";
    }

    switch (type_desc.type) {
    case TYPE_BOOLEAN: {
        return datum.get_uint8() ? "true" : "false";
    }
    case TYPE_TINYINT: {
        return std::to_string(datum.get_int8());
    }
    case TYPE_SMALLINT: {
        return std::to_string(datum.get_int16());
    }
    case TYPE_INT: {
        return std::to_string(datum.get_int32());
    }
    case TYPE_BIGINT: {
        return std::to_string(datum.get_int64());
    }
    case TYPE_DATE: {
        return datum.get_date().to_string();
    }
    case TYPE_DATETIME: {
        return url_encode(datum.get_timestamp().to_string());
    }
    case TYPE_CHAR: {
        std::string origin_str = datum.get_slice().to_string();
        if (origin_str.length() < type_desc.len) {
            origin_str.append(type_desc.len - origin_str.length(), ' ');
        }
        return url_encode(origin_str);
    }
    case TYPE_VARCHAR: {
        return url_encode(datum.get_slice().to_string());
    }
    default: {
        return Status::InvalidArgument("unsupported partition column type" + type_desc.debug_string());
    }
    }
}

StatusOr<ConnectorChunkSink::Futures> HiveUtils::hive_style_partitioning_write_chunk(
        const ChunkPtr& chunk, bool partitioned, const std::string& partition, int64_t max_file_size,
        const formats::FileWriterFactory* file_writer_factory, LocationProvider* location_provider,
        std::map<std::string, std::shared_ptr<formats::FileWriter>>& partition_writers) {
    ConnectorChunkSink::Futures futures;
    auto it = partition_writers.find(partition);
    if (it != partition_writers.end()) {
        auto* writer = it->second.get();
        if (writer->get_written_bytes() >= max_file_size) {
            futures.commit_file_futures.push_back(writer->commit());
            partition_writers.erase(it);
            auto path = partitioned ? location_provider->get(partition) : location_provider->get();
            ASSIGN_OR_RETURN(auto new_writer, file_writer_factory->create(path));
            RETURN_IF_ERROR(new_writer->init());
            futures.add_chunk_futures.push_back(new_writer->write(chunk));
            partition_writers.emplace(partition, std::move(new_writer));
        } else {
            futures.add_chunk_futures.push_back(writer->write(chunk));
        }
    } else {
        auto path = partitioned ? location_provider->get(partition) : location_provider->get();
        ASSIGN_OR_RETURN(auto new_writer, file_writer_factory->create(path));
        RETURN_IF_ERROR(new_writer->init());
        futures.add_chunk_futures.push_back(new_writer->write(chunk));
        partition_writers.emplace(partition, std::move(new_writer));
    }
    return futures;
}

} // namespace starrocks::connector

// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "arrow/engine/substrait/relation_internal.h"

#include "arrow/compute/api_scalar.h"
#include "arrow/compute/exec/options.h"
#include "arrow/dataset/file_ipc.h"
#include "arrow/dataset/file_parquet.h"
#include "arrow/dataset/plan.h"
#include "arrow/dataset/scanner.h"
#include "arrow/engine/substrait/expression_internal.h"
#include "arrow/engine/substrait/type_internal.h"
#include "arrow/filesystem/localfs.h"

namespace arrow {
namespace engine {

template <typename RelMessage>
Status CheckRelCommon(const RelMessage& rel) {
  if (rel.has_common()) {
    if (rel.common().has_emit()) {
      return Status::NotImplemented("substrait::RelCommon::Emit");
    }
    if (rel.common().has_hint()) {
      return Status::NotImplemented("substrait::RelCommon::Hint");
    }
    if (rel.common().has_advanced_extension()) {
      return Status::NotImplemented("substrait::RelCommon::advanced_extension");
    }
  }
  if (rel.has_advanced_extension()) {
    return Status::NotImplemented("substrait AdvancedExtensions");
  }
  return Status::OK();
}

Result<compute::Declaration> FromProto(const substrait::Rel& rel,
                                       const ExtensionSet& ext_set) {
  static bool dataset_init = false;
  if (!dataset_init) {
    dataset_init = true;
    dataset::internal::Initialize();
  }

  switch (rel.rel_type_case()) {
    case substrait::Rel::RelTypeCase::kRead: {
      const auto& read = rel.read();
      RETURN_NOT_OK(CheckRelCommon(read));

      ARROW_ASSIGN_OR_RAISE(auto base_schema, FromProto(read.base_schema(), ext_set));

      auto scan_options = std::make_shared<dataset::ScanOptions>();

      // FieldPath is not supported in scan filter. See ARROW-14658
      // Ignore the filter in ReadRel and use the push-down filter from Filter operator.
      // if (read.has_filter()) {
      //   ARROW_ASSIGN_OR_RAISE(scan_options->filter, FromProto(read.filter(), ext_set));
      // }

      if (read.has_projection()) {
        // NOTE: scan_options->projection is not used by the scanner and thus can't be
        // used for this
        return Status::NotImplemented("substrait::ReadRel::projection");
      }

      if (!read.has_local_files()) {
        return Status::NotImplemented(
            "substrait::ReadRel with read_type other than LocalFiles");
      }

      if (read.local_files().has_advanced_extension()) {
        return Status::NotImplemented(
            "substrait::ReadRel::LocalFiles::advanced_extension");
      }

      // Check whether the input is iterator.
      auto head = read.local_files().items().at(0);
      if (head.path_type_case() == substrait::ReadRel_LocalFiles_FileOrFiles::kUriFile &&
          util::string_view{head.uri_file()}.starts_with("iterator:")) {
        const auto& index = head.uri_file().substr(9);
        // Construct decl with the index of input iterator.
        return compute::Declaration{"source_index",
                                    compute::SourceIndexOptions{std::stoi(index)}};
      }

      std::shared_ptr<dataset::FileFormat> format;
      auto filesystem = std::make_shared<fs::LocalFileSystem>();
      std::vector<std::shared_ptr<dataset::FileFragment>> fragments;

      for (const auto& item : read.local_files().items()) {
        if (item.path_type_case() !=
            substrait::ReadRel_LocalFiles_FileOrFiles::kUriFile) {
          return Status::NotImplemented(
              "substrait::ReadRel::LocalFiles::FileOrFiles with "
              "path_type other than uri_file");
        }

        switch (item.file_format_case()) {
          case substrait::ReadRel_LocalFiles_FileOrFiles::kParquet:
            format = std::make_shared<dataset::ParquetFileFormat>();
            break;
          case substrait::ReadRel_LocalFiles_FileOrFiles::kArrow:
            format = std::make_shared<dataset::IpcFileFormat>();
            break;
          default:
            return Status::NotImplemented(
                "unknown substrait::ReadRel::LocalFiles::FileOrFiles::file_format");
        }

        if (!util::string_view{item.uri_file()}.starts_with("file:///")) {
          return Status::NotImplemented(
              "substrait::ReadRel::LocalFiles::FileOrFiles::uri_file "
              "with other than local filesystem (file:///)");
        }
        auto path = item.uri_file().substr(7);

        // Ignore partition index and use start and length to locate file fragment.
        // if (item.partition_index() != 0) {
        //   return Status::NotImplemented(
        //       "non-default
        //       substrait::ReadRel::LocalFiles::FileOrFiles::partition_index");
        // }

        // Read all row groups if both start and length are not specified.
        int64_t start_offset = item.length() == 0 && item.start() == 0
                                   ? -1
                                   : static_cast<int64_t>(item.start());
        int64_t length = static_cast<int64_t>(item.length());

        ARROW_ASSIGN_OR_RAISE(auto fragment,
                              format->MakeFragment(dataset::FileSource{
                                  std::move(path), filesystem, start_offset, length}));
        fragments.push_back(std::move(fragment));
      }

      ARROW_ASSIGN_OR_RAISE(
          auto ds, dataset::FileSystemDataset::Make(
                       std::move(base_schema), /*root_partition=*/compute::literal(true),
                       std::move(format), std::move(filesystem), std::move(fragments)));

      return compute::Declaration{
          "scan", dataset::ScanNodeOptions{std::move(ds), std::move(scan_options)}};
    }

    case substrait::Rel::RelTypeCase::kFilter: {
      const auto& filter = rel.filter();
      RETURN_NOT_OK(CheckRelCommon(filter));

      if (!filter.has_input()) {
        return Status::Invalid("substrait::FilterRel with no input relation");
      }
      ARROW_ASSIGN_OR_RAISE(auto input, FromProto(filter.input(), ext_set));

      if (!filter.has_condition()) {
        return Status::Invalid("substrait::FilterRel with no condition expression");
      }
      ARROW_ASSIGN_OR_RAISE(auto condition, FromProto(filter.condition(), ext_set));

      return compute::Declaration::Sequence({
          std::move(input),
          {"filter", compute::FilterNodeOptions{std::move(condition)}},
      });
    }

    case substrait::Rel::RelTypeCase::kProject: {
      const auto& project = rel.project();
      RETURN_NOT_OK(CheckRelCommon(project));

      if (!project.has_input()) {
        return Status::Invalid("substrait::ProjectRel with no input relation");
      }
      ARROW_ASSIGN_OR_RAISE(auto input, FromProto(project.input(), ext_set));

      std::vector<compute::Expression> expressions;
      for (const auto& expr : project.expressions()) {
        expressions.emplace_back();
        ARROW_ASSIGN_OR_RAISE(expressions.back(), FromProto(expr, ext_set));
      }

      return compute::Declaration::Sequence({
          std::move(input),
          {"project", compute::ProjectNodeOptions{std::move(expressions)}},
      });
    }

    default:
      break;
  }

  return Status::NotImplemented(
      "conversion to arrow::compute::Declaration from Substrait relation ",
      rel.DebugString());
}

}  // namespace engine
}  // namespace arrow

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

#include "arrow/dataset/scanner.h"

#include <algorithm>
#include <memory>
#include <mutex>

#include "arrow/dataset/dataset.h"
#include "arrow/dataset/dataset_internal.h"
#include "arrow/dataset/scanner_internal.h"
#include "arrow/io/memory.h"
#include "arrow/ipc/reader.h"
#include "arrow/ipc/writer.h"
#include "arrow/result.h"
#include "arrow/table.h"
#include "arrow/util/iterator.h"
#include "arrow/util/logging.h"
#include "arrow/util/task_group.h"
#include "arrow/util/thread_pool.h"

namespace arrow {
namespace dataset {

ScanOptions::ScanOptions(std::shared_ptr<Schema> schema)
    : projector(RecordBatchProjector(std::move(schema))) {}

std::shared_ptr<ScanOptions> ScanOptions::ReplaceSchema(
    std::shared_ptr<Schema> schema) const {
  auto copy = ScanOptions::Make(std::move(schema));
  copy->filter = filter;
  copy->batch_size = batch_size;
  return copy;
}

std::vector<std::string> ScanOptions::MaterializedFields() const {
  std::vector<std::string> fields;

  for (const auto& f : schema()->fields()) {
    fields.push_back(f->name());
  }

  for (const FieldRef& ref : FieldsInExpression(filter)) {
    DCHECK(ref.name());
    fields.push_back(*ref.name());
  }

  return fields;
}

Result<RecordBatchIterator> InMemoryScanTask::Execute() {
  return MakeVectorIterator(record_batches_);
}

Result<FragmentIterator> Scanner::GetFragments() {
  if (fragment_ != nullptr) {
    return MakeVectorIterator(FragmentVector{fragment_});
  }

  // Transform Datasets in a flat Iterator<Fragment>. This
  // iterator is lazily constructed, i.e. Dataset::GetFragments is
  // not invoked until a Fragment is requested.
  return GetFragmentsFromDatasets({dataset_}, scan_options_->filter);
}

Result<ScanTaskIterator> Scanner::Scan() {
  // Transforms Iterator<Fragment> into a unified
  // Iterator<ScanTask>. The first Iterator::Next invocation is going to do
  // all the work of unwinding the chained iterators.
  ARROW_ASSIGN_OR_RAISE(auto fragment_it, GetFragments());
  return GetScanTaskIterator(std::move(fragment_it), scan_options_, scan_context_);
}

Result<ScanTaskIterator> ScanTaskIteratorFromRecordBatch(
    std::vector<std::shared_ptr<RecordBatch>> batches,
    std::shared_ptr<ScanOptions> options, std::shared_ptr<ScanContext> context) {
  ScanTaskVector tasks{std::make_shared<InMemoryScanTask>(batches, std::move(options),
                                                          std::move(context))};
  return MakeVectorIterator(std::move(tasks));
}

ScannerBuilder::ScannerBuilder(std::shared_ptr<Dataset> dataset,
                               std::shared_ptr<ScanContext> scan_context)
    : dataset_(std::move(dataset)),
      fragment_(nullptr),
      scan_options_(ScanOptions::Make(dataset_->schema())),
      scan_context_(std::move(scan_context)) {
  DCHECK_OK(Filter(literal(true)));
}

ScannerBuilder::ScannerBuilder(std::shared_ptr<Schema> schema,
                               std::shared_ptr<Fragment> fragment,
                               std::shared_ptr<ScanContext> scan_context)
    : dataset_(nullptr),
      fragment_(std::move(fragment)),
      fragment_schema_(schema),
      scan_options_(ScanOptions::Make(std::move(schema))),
      scan_context_(std::move(scan_context)) {
  DCHECK_OK(Filter(literal(true)));
}

const std::shared_ptr<Schema>& ScannerBuilder::schema() const {
  return fragment_ ? fragment_schema_ : dataset_->schema();
}

Status ScannerBuilder::Project(std::vector<std::string> columns) {
  RETURN_NOT_OK(schema()->CanReferenceFieldsByNames(columns));
  has_projection_ = true;
  project_columns_ = std::move(columns);
  return Status::OK();
}

Status ScannerBuilder::Filter(const Expression& filter) {
  for (const auto& ref : FieldsInExpression(filter)) {
    RETURN_NOT_OK(ref.FindOne(*schema()));
  }
  ARROW_ASSIGN_OR_RAISE(scan_options_->filter, filter.Bind(*schema()));
  return Status::OK();
}

Status ScannerBuilder::UseThreads(bool use_threads) {
  scan_context_->use_threads = use_threads;
  return Status::OK();
}

Status ScannerBuilder::BatchSize(int64_t batch_size) {
  if (batch_size <= 0) {
    return Status::Invalid("BatchSize must be greater than 0, got ", batch_size);
  }
  scan_options_->batch_size = batch_size;
  return Status::OK();
}

Result<std::shared_ptr<Scanner>> ScannerBuilder::Finish() const {
  std::shared_ptr<ScanOptions> scan_options;
  if (has_projection_ && !project_columns_.empty()) {
    scan_options =
        scan_options_->ReplaceSchema(SchemaFromColumnNames(schema(), project_columns_));
  } else {
    scan_options = std::make_shared<ScanOptions>(*scan_options_);
  }

  if (dataset_ == nullptr) {
    return std::make_shared<Scanner>(fragment_, std::move(scan_options), scan_context_);
  }

  return std::make_shared<Scanner>(dataset_, std::move(scan_options), scan_context_);
}

using arrow::internal::TaskGroup;

std::shared_ptr<TaskGroup> ScanContext::TaskGroup() const {
  if (use_threads) {
    auto* thread_pool = arrow::internal::GetCpuThreadPool();
    return TaskGroup::MakeThreaded(thread_pool);
  }
  return TaskGroup::MakeSerial();
}

static inline RecordBatchVector FlattenRecordBatchVector(
    std::vector<RecordBatchVector> nested_batches) {
  RecordBatchVector flattened;

  for (auto& task_batches : nested_batches) {
    for (auto& batch : task_batches) {
      flattened.emplace_back(std::move(batch));
    }
  }

  return flattened;
}

struct TableAssemblyState {
  /// Protecting mutating accesses to batches
  std::mutex mutex{};
  std::vector<RecordBatchVector> batches{};

  void Emplace(RecordBatchVector b, size_t position) {
    std::lock_guard<std::mutex> lock(mutex);
    if (batches.size() <= position) {
      batches.resize(position + 1);
    }
    batches[position] = std::move(b);
  }
};

Result<std::shared_ptr<Table>> Scanner::ToTable() {
  ARROW_ASSIGN_OR_RAISE(auto scan_task_it, Scan());
  auto task_group = scan_context_->TaskGroup();

  /// Wraps the state in a shared_ptr to ensure that failing ScanTasks don't
  /// invalidate concurrently running tasks when Finish() early returns
  /// and the mutex/batches fail out of scope.
  auto state = std::make_shared<TableAssemblyState>();

  size_t scan_task_id = 0;
  for (auto maybe_scan_task : scan_task_it) {
    ARROW_ASSIGN_OR_RAISE(auto scan_task, maybe_scan_task);

    auto id = scan_task_id++;
    task_group->Append([state, id, scan_task] {
      ARROW_ASSIGN_OR_RAISE(auto batch_it, scan_task->Execute());
      ARROW_ASSIGN_OR_RAISE(auto local, batch_it.ToVector());
      state->Emplace(std::move(local), id);
      return Status::OK();
    });
  }

  // Wait for all tasks to complete, or the first error.
  RETURN_NOT_OK(task_group->Finish());

  return Table::FromRecordBatches(scan_options_->schema(),
                                  FlattenRecordBatchVector(std::move(state->batches)));
}

}  // namespace dataset
}  // namespace arrow

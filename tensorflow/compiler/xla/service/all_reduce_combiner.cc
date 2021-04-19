/* Copyright 2020 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/compiler/xla/service/all_reduce_combiner.h"

#include <algorithm>
#include <list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/strings/str_join.h"
#include "tensorflow/compiler/xla/literal.h"
#include "tensorflow/compiler/xla/service/collective_combiner_utils.h"
#include "tensorflow/compiler/xla/service/hlo_casting_utils.h"
#include "tensorflow/compiler/xla/service/hlo_domain_map.h"
#include "tensorflow/compiler/xla/service/hlo_instruction.h"
#include "tensorflow/compiler/xla/service/hlo_instructions.h"
#include "tensorflow/compiler/xla/service/hlo_opcode.h"
#include "tensorflow/compiler/xla/service/hlo_query.h"
#include "tensorflow/compiler/xla/service/hlo_reachability.h"
#include "tensorflow/compiler/xla/service/shape_inference.h"
#include "tensorflow/compiler/xla/shape_util.h"
#include "tensorflow/compiler/xla/status_macros.h"
#include "tensorflow/compiler/xla/xla_data.pb.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/platform/types.h"

namespace xla {
namespace {

// Combines the elements of to_combine into a single AllReduce op. All
// entries in to_combine must be AllReduce ops with exactly one operand
// and the same reduction operation.
Status CombineAllReduces(absl::Span<HloInstruction* const> to_combine) {
  if (to_combine.size() < 2) {
    return Status::OK();
  }
  VLOG(1) << "Combined " << to_combine.size() << " CRS ops";

  HloComputation& computation = *to_combine.back()->parent();
  HloComputation* reduction = to_combine[0]->to_apply();
  const HloOpcode type = reduction->root_instruction()->opcode();

  // Create a single bigger AllReduce of the operands of the smaller
  // AllReduces.
  std::vector<HloInstruction*> operands;
  std::vector<Shape> operand_shapes;
  VLOG(1) << "Combining set";
  for (HloInstruction* hlo : to_combine) {
    VLOG(1) << "Set element: " << hlo->ToString();
    TF_RET_CHECK(hlo->opcode() == HloOpcode::kAllReduce);
    TF_RET_CHECK(hlo->operands().size() == 1);
    TF_RET_CHECK(hlo->to_apply() == reduction ||
                 (hlo->to_apply()->instruction_count() == 3 &&
                  hlo->to_apply()->num_parameters() == 2 &&
                  hlo->to_apply()->root_instruction()->opcode() == type));
    TF_RET_CHECK(hlo->shape().IsArray());
    for (HloInstruction* operand : hlo->operands()) {
      operands.push_back(operand);
      operand_shapes.push_back(operand->shape());
    }
  }

  HloInstruction* combined;
  // AllReduce ops with more than one operand produce a tuple.
  TF_RET_CHECK(operands.size() >= 2);
  combined = computation.AddInstruction(HloInstruction::CreateAllReduce(
      ShapeUtil::MakeTupleShape(operand_shapes), operands, reduction,
      to_combine.front()->replica_groups(),
      /*constrain_layout=*/false, to_combine.front()->channel_id(),
      Cast<HloAllReduceInstruction>(to_combine.front())
          ->use_global_device_ids()));

  // We have to propagate the sharding manually because Domain instructions are
  // not guaranteed to preserve it for side effecting instructions.
  if (to_combine.front()->has_sharding()) {
    combined->set_sharding(to_combine.front()->sharding());
  }
  VLOG(1) << "Replacing with : " << combined->ToString();

  // Replace all the smaller AllReduces with elements of the tuple output
  // of the single bigger AllReduce.
  for (int64 i = 0; i < to_combine.size(); ++i) {
    auto replace_with = HloInstruction::CreateGetTupleElement(
        to_combine[i]->shape(), combined, i);
    TF_RETURN_IF_ERROR(computation.ReplaceWithNewInstruction(
        to_combine[i], std::move(replace_with)));
  }
  return Status::OK();
}

// The group key encapsulates all of the properties which must match for it
// to be possible to combine the instructions.
using GroupKey = std::tuple<HloOpcode, PrimitiveType, int64_t, bool, bool,
                            std::vector<std::vector<int64_t>>>;

// Returns a key that will be equal for instructions that might be combined, or
// different if not.
absl::optional<GroupKey> CombineKey(const HloInstruction* instruction,
                                    const HloDomainMap& domain_map) {
  if (instruction->opcode() != HloOpcode::kAllReduce) {
    return absl::nullopt;
  }

  if (instruction->to_apply()->instruction_count() != 3 ||
      instruction->to_apply()->num_parameters() != 2) {
    VLOG(1) << "Skipping due to non-trivial reduction function.";
    return absl::nullopt;
  }

  const auto* ar = Cast<HloAllReduceInstruction>(instruction);

  std::vector<std::vector<int64_t>> replica_groups;
  replica_groups.reserve(ar->replica_groups().size());
  for (const ReplicaGroup& replica_group : ar->replica_groups()) {
    replica_groups.push_back(
        std::vector<int64_t>(replica_group.replica_ids().begin(),
                             replica_group.replica_ids().end()));
  }

  const HloInstruction* to_apply_root = ar->to_apply()->root_instruction();
  return GroupKey{to_apply_root->opcode(),
                  to_apply_root->shape().element_type(),
                  domain_map.GetDomainMetadataId(ar),
                  ar->channel_id().has_value(),
                  ar->use_global_device_ids(),
                  replica_groups};
}

}  // namespace

AllReduceCombiner::AllReduceCombiner(int64 combine_threshold_in_bytes,
                                     int64 combine_threshold_count)
    : combine_threshold_in_bytes_(combine_threshold_in_bytes),
      combine_threshold_count_(combine_threshold_count) {}

StatusOr<bool> AllReduceCombiner::Run(HloModule* module) {
  VLOG(1) << "Running AllReduceCombiner with threshold of "
          << combine_threshold_in_bytes_ << " bytes";

  if (combine_threshold_in_bytes_ <= 0 || combine_threshold_count_ <= 0) {
    VLOG(1) << "Skip AllReduceCombiner because the threshold is zero";
    return false;
  }

  if (hlo_query::ContainsLayoutConstrainedAllReduce(*module)) {
    VLOG(1) << "Skip AllReduceCombiner because the module contains all-reduce "
               "with constrained layouts";
    return false;
  }

  bool changed = false;
  for (HloComputation* computation : module->MakeNonfusionComputations()) {
    TF_ASSIGN_OR_RETURN(auto domain_map, HloDomainMap::Create(computation, ""));

    auto key_fn = [&domain_map](const HloInstruction* instruction) {
      return CombineKey(instruction, *domain_map);
    };

    TF_ASSIGN_OR_RETURN(
        bool computation_changed,
        CombineInstructionsByKey<GroupKey>(
            computation, key_fn, &CombineAllReduces,
            combine_threshold_in_bytes_, combine_threshold_count_));
    changed |= computation_changed;
  }

  return changed;
}

}  // namespace xla

// Copyright 2024 The XLS Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "xls/dslx/type_system/zip_types.h"

#include <cstdint>
#include <utility>

#include "absl/status/status.h"
#include "xls/common/status/status_macros.h"
#include "xls/dslx/type_system/type.h"

namespace xls::dslx {
namespace {

// This is an implementation detail in traversing types and then recursively
// calling ZipTypes -- we inherit TypeVisitor because we need to learn the
// actual type of the generic `Type` on the left hand side and then compare that
// to what we see on the right hand side at each step.
class ZipTypeVisitor : public TypeVisitor {
 public:
  explicit ZipTypeVisitor(const Type& rhs, ZipTypesCallbacks& callbacks)
      : rhs_(rhs), callbacks_(callbacks) {}

  ~ZipTypeVisitor() override = default;

  // -- various non-aggregate types

  absl::Status HandleEnum(const EnumType& lhs) override {
    return HandleNonAggregate(lhs);
  }
  absl::Status HandleBits(const BitsType& lhs) override {
    return HandleNonAggregate(lhs);
  }
  absl::Status HandleBitsConstructor(const BitsConstructorType& lhs) override {
    return HandleNonAggregate(lhs);
  }
  absl::Status HandleToken(const TokenType& lhs) override {
    return HandleNonAggregate(lhs);
  }

  // -- types that contain other types

  absl::Status HandleTuple(const TupleType& lhs) override {
    if (auto* rhs = dynamic_cast<const TupleType*>(&rhs_)) {
      return HandleTupleLike(lhs, *rhs);
    }
    return callbacks_.NoteTypeMismatch(lhs, rhs_);
  }
  absl::Status HandleStruct(const StructType& lhs) override {
    if (auto* rhs = dynamic_cast<const StructType*>(&rhs_)) {
      return HandleTupleLike(lhs, *rhs);
    }
    return callbacks_.NoteTypeMismatch(lhs, rhs_);
  }
  absl::Status HandleArray(const ArrayType& lhs) override {
    if (auto* rhs = dynamic_cast<const ArrayType*>(&rhs_)) {
      AggregatePair aggregates = std::make_pair(&lhs, rhs);
      XLS_RETURN_IF_ERROR(callbacks_.NoteAggregateStart(aggregates));
      const Type& lhs_elem = lhs.element_type();
      const Type& rhs_elem = rhs->element_type();
      XLS_RETURN_IF_ERROR(ZipTypes(lhs_elem, rhs_elem, callbacks_));
      return callbacks_.NoteAggregateEnd(aggregates);
    }
    return callbacks_.NoteTypeMismatch(lhs, rhs_);
  }
  absl::Status HandleChannel(const ChannelType& lhs) override {
    if (auto* rhs = dynamic_cast<const ChannelType*>(&rhs_)) {
      AggregatePair aggregates = std::make_pair(&lhs, rhs);
      XLS_RETURN_IF_ERROR(callbacks_.NoteAggregateStart(aggregates));
      XLS_RETURN_IF_ERROR(
          ZipTypes(lhs.payload_type(), rhs->payload_type(), callbacks_));
      return callbacks_.NoteAggregateEnd(aggregates);
    }
    return callbacks_.NoteTypeMismatch(lhs, rhs_);
  }
  absl::Status HandleFunction(const FunctionType& lhs) override {
    if (auto* rhs = dynamic_cast<const FunctionType*>(&rhs_)) {
      AggregatePair aggregates = std::make_pair(&lhs, rhs);
      XLS_RETURN_IF_ERROR(callbacks_.NoteAggregateStart(aggregates));
      for (int64_t i = 0; i < lhs.GetParamCount(); ++i) {
        XLS_RETURN_IF_ERROR(
            ZipTypes(*lhs.GetParams()[i], *rhs->GetParams()[i], callbacks_));
      }
      XLS_RETURN_IF_ERROR(
          ZipTypes(lhs.return_type(), rhs->return_type(), callbacks_));
      return callbacks_.NoteAggregateEnd(aggregates);
    }
    return callbacks_.NoteTypeMismatch(lhs, rhs_);
  }
  absl::Status HandleMeta(const MetaType& lhs) override {
    if (auto* rhs = dynamic_cast<const MetaType*>(&rhs_)) {
      AggregatePair aggregates = std::make_pair(&lhs, rhs);
      XLS_RETURN_IF_ERROR(callbacks_.NoteAggregateStart(aggregates));
      XLS_RETURN_IF_ERROR(
          ZipTypes(*lhs.wrapped(), *rhs->wrapped(), callbacks_));
      return callbacks_.NoteAggregateEnd(aggregates);
    }
    return callbacks_.NoteTypeMismatch(lhs, rhs_);
  }

 private:
  // Handles tuples and structs which are quite similar.
  template <typename T>
  absl::Status HandleTupleLike(const T& lhs, const T& rhs) {
    bool structurally_compatible = lhs.size() == rhs.size();
    if (!structurally_compatible) {
      return callbacks_.NoteTypeMismatch(lhs, rhs);
    }
    AggregatePair aggregates = std::make_pair(&lhs, &rhs);
    XLS_RETURN_IF_ERROR(callbacks_.NoteAggregateStart(aggregates));
    for (int64_t i = 0; i < lhs.size(); ++i) {
      const Type& lhs_elem = lhs.GetMemberType(i);
      const Type& rhs_elem = rhs.GetMemberType(i);
      XLS_RETURN_IF_ERROR(ZipTypes(lhs_elem, rhs_elem, callbacks_));
    }
    XLS_RETURN_IF_ERROR(callbacks_.NoteAggregateEnd(aggregates));
    return absl::OkStatus();
  }

  absl::Status HandleNonAggregate(const Type& lhs) {
    if (lhs.CompatibleWith(rhs_)) {
      return callbacks_.NoteMatchedLeafType(lhs, rhs_);
    }
    return callbacks_.NoteTypeMismatch(lhs, rhs_);
  }

  const Type& rhs_;
  ZipTypesCallbacks& callbacks_;
};

}  // namespace

absl::Status ZipTypes(const Type& lhs, const Type& rhs,
                      ZipTypesCallbacks& callbacks) {
  ZipTypeVisitor visitor(rhs, callbacks);
  return lhs.Accept(visitor);
}

}  // namespace xls::dslx

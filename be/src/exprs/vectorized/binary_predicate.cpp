// This file is licensed under the Elastic License 2.0. Copyright 2021-present, StarRocks Limited.

#include "exprs/vectorized/binary_predicate.h"

#include "column/column_builder.h"
#include "column/column_viewer.h"
#include "exprs/vectorized/binary_function.h"
#include "runtime/primitive_type.h"
#include "runtime/primitive_type_infra.h"

namespace starrocks::vectorized {

#define DEFINE_BINARY_PRED_FN(FN, OP)                                    \
    struct BinaryPred##FN {                                              \
        template <typename LType, typename RType, typename ResultType>   \
        static inline ResultType apply(const LType& l, const RType& r) { \
            return l OP r;                                               \
        }                                                                \
    }

DEFINE_BINARY_PRED_FN(Eq, ==);
DEFINE_BINARY_PRED_FN(Ne, !=);
DEFINE_BINARY_PRED_FN(Lt, <);
DEFINE_BINARY_PRED_FN(Le, <=);
DEFINE_BINARY_PRED_FN(Gt, >);
DEFINE_BINARY_PRED_FN(Ge, >=);

#undef DEFINE_BINARY_PRED_FN

template <PrimitiveType Type, typename OP>
class VectorizedBinaryPredicate final : public Predicate {
public:
    explicit VectorizedBinaryPredicate(const TExprNode& node) : Predicate(node) {}
    ~VectorizedBinaryPredicate() override = default;

    Expr* clone(ObjectPool* pool) const override { return pool->add(new VectorizedBinaryPredicate(*this)); }

    ColumnPtr evaluate(ExprContext* context, vectorized::Chunk* ptr) override {
        auto l = _children[0]->evaluate(context, ptr);
        auto r = _children[1]->evaluate(context, ptr);
        return VectorizedStrictBinaryFunction<OP>::template evaluate<Type, TYPE_BOOLEAN>(l, r);
    }
};

template <PrimitiveType Type, typename OP>
class VectorizedNullSafeEqPredicate final : public Predicate {
public:
    explicit VectorizedNullSafeEqPredicate(const TExprNode& node) : Predicate(node) {}
    ~VectorizedNullSafeEqPredicate() override = default;

    Expr* clone(ObjectPool* pool) const override { return pool->add(new VectorizedNullSafeEqPredicate(*this)); }

    // if v1 null and v2 null = true
    // if v1 null and v2 not null = false
    // if v1 not null and v2 null = false
    // if v1 not null and v2 not null = v1 OP v2
    ColumnPtr evaluate(ExprContext* context, vectorized::Chunk* ptr) override {
        auto l = _children[0]->evaluate(context, ptr);
        auto r = _children[1]->evaluate(context, ptr);

        ColumnViewer<Type> v1(l);
        ColumnViewer<Type> v2(r);
        Columns list = {l, r};

        size_t size = list[0]->size();
        ColumnBuilder<TYPE_BOOLEAN> builder(size);
        for (int row = 0; row < size; ++row) {
            auto null1 = v1.is_null(row);
            auto null2 = v2.is_null(row);

            if (null1 & null2) {
                // all null = true
                builder.append(true);
            } else if (null1 ^ null2) {
                // one null = false
                builder.append(false);
            } else {
                // all not null = value eq
                builder.append(v1.value(row) == v2.value(row));
            }
        }

        return builder.build(ColumnHelper::is_all_const(list));
    }
};

struct BinaryPredicateBuilder {
    template <PrimitiveType data_type>
    Expr* operator()(const TExprNode& node) {
        switch (node.opcode) {
        case TExprOpcode::EQ:
            return new VectorizedBinaryPredicate<data_type, BinaryPredEq>(node);
        case TExprOpcode::NE:
            return new VectorizedBinaryPredicate<data_type, BinaryPredNe>(node);
        case TExprOpcode::LT:
            return new VectorizedBinaryPredicate<data_type, BinaryPredLt>(node);
        case TExprOpcode::LE:
            return new VectorizedBinaryPredicate<data_type, BinaryPredLe>(node);
        case TExprOpcode::GT:
            return new VectorizedBinaryPredicate<data_type, BinaryPredGt>(node);
        case TExprOpcode::GE:
            return new VectorizedBinaryPredicate<data_type, BinaryPredGe>(node);
        case TExprOpcode::EQ_FOR_NULL:
            return new VectorizedNullSafeEqPredicate<data_type, BinaryPredEq>(node);
        default:
            break;
        }
        return nullptr;
    }
};

Expr* VectorizedBinaryPredicateFactory::from_thrift(const TExprNode& node) {
    PrimitiveType type = thrift_to_type(node.child_type);

    return type_dispatch_predicate(type, BinaryPredicateBuilder(), node);
}

} // namespace starrocks::vectorized

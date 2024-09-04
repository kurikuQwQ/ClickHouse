#pragma once

#include <Core/Joins.h>
#include <vector>
#include <string>
#include <optional>
#include <Analyzer/IQueryTreeNode.h>

#include <QueryPipeline/SizeLimits.h>

namespace DB
{

enum class PredicateOperator : UInt8
{
    Equal,
    NullSafeEqual,
    Less,
    LessOrEquals,
    Greater,
    GreaterOrEquals,
};

inline std::optional<PredicateOperator> getJoinPredicateOperator(const String & func_name)
{
    if (func_name == "equals")
        return PredicateOperator::Equal;
    if (func_name == "isNotDistinctFrom")
        return PredicateOperator::NullSafeEqual;
    if (func_name == "less")
        return PredicateOperator::Less;
    if (func_name == "greater")
        return PredicateOperator::Greater;
    if (func_name == "lessOrEquals")
        return PredicateOperator::LessOrEquals;
    if (func_name == "greaterOrEquals")
        return PredicateOperator::GreaterOrEquals;
    return {};
}

inline PredicateOperator reversePredicateOperator(PredicateOperator op)
{
    switch (op)
    {
        case PredicateOperator::Equal: return PredicateOperator::Equal;
        case PredicateOperator::NullSafeEqual: return PredicateOperator::NullSafeEqual;
        case PredicateOperator::Less: return PredicateOperator::Greater;
        case PredicateOperator::Greater: return PredicateOperator::Less;
        case PredicateOperator::LessOrEquals: return PredicateOperator::GreaterOrEquals;
        case PredicateOperator::GreaterOrEquals: return PredicateOperator::LessOrEquals;
    }
}


struct JoinExpressionAction
{
    ActionsDAG actions_dag;

};

/// JoinPredicate represents a single join qualifier
/// that that apply to the combination of two tables.
struct JoinPredicate
{
    QueryTreeNodePtr left_node;
    QueryTreeNodePtr right_node;
    PredicateOperator op;
};

/// JoinCondition determines if rows from two tables can be joined
struct JoinCondition
{
    /// Join predicates that must be satisfied to join rows
    std::vector<JoinPredicate> predicates;

    /// Pre-Join filters applied to the left and right tables independently
    std::vector<QueryTreeNodePtr> left_filter_conditions;
    std::vector<QueryTreeNodePtr> right_filter_conditions;

    /// Residual conditions depend on data from both tables and must be evaluated after the join has been performed.
    /// Unlike the join predicates, these conditions can be arbitrary expressions.
    std::vector<QueryTreeNodePtr> residual_conditions;
};

struct JoinExpression
{
    /// Disjunctive join conditions represented by alternative conditions connected by the OR operator.
    /// If any of the conditions is true, corresponding rows from the left and right tables can be joined.
    std::vector<JoinCondition> disjunctive_conditions;

    /// Indicates if the join expression is defined with the USING clause
    bool is_using = false;
};


struct JoinInfo
{
    /// An expression in ON/USING clause of a JOIN statement
    JoinExpression expression;

    /// The type of join (e.g., INNER, LEFT, RIGHT, FULL)
    JoinKind kind;

    /// The strictness of the join (e.g., ALL, ANY, SEMI, ANTI)
    JoinStrictness strictness;

    /// The locality of the join (e.g., LOCAL, GLOBAL)
    JoinLocality locality;
};


// struct JoinSettings
// {
//     JoinAlgorithm algorithm;
//     size_t max_block_size;

//     bool join_use_nulls;
//     bool any_join_distinct_right_table_keys = false;

//     size_t max_rows_in_join;
//     size_t max_bytes_in_join;

//     OverflowMode join_overflow_mode;
//     bool join_any_take_last_row;

//     /// CROSS JOIN settings
//     UInt64 cross_join_min_rows_to_compress;
//     UInt64 cross_join_min_bytes_to_compress;

//     /// Partial merge join settings
//     UInt64 partial_merge_join_left_table_buffer_bytes;
//     UInt64 partial_merge_join_rows_in_right_blocks;
//     UInt64 join_on_disk_max_files_to_merge;

//     /// Grace hash join settings
//     UInt64 grace_hash_join_initial_buckets;
//     UInt64 grace_hash_join_max_buckets;

//     /// Full sortings merge join settings
//     UInt64 max_rows_in_set_to_optimize_join;

//     /// Hash/Parallel hash join settings
//     bool collect_hash_table_stats_during_joins;
//     UInt64 max_size_to_preallocate_for_joins;

//     bool query_plan_convert_outer_join_to_inner_join;
//     bool multiple_joins_try_to_keep_original_names;

//     bool parallel_replicas_prefer_local_join;
//     bool allow_experimental_join_condition;

//     UInt64 cross_to_inner_join_rewrite;
// };



}

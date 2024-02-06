#include <Analyzer/Passes/FunctionToSubcolumnsPass.h>

#include <DataTypes/DataTypesNumber.h>
#include <DataTypes/DataTypeTuple.h>
#include <DataTypes/DataTypeArray.h>
#include <DataTypes/DataTypeMap.h>

#include <Storages/IStorage.h>

#include <Functions/FunctionFactory.h>

#include <Interpreters/Context.h>

#include <Analyzer/InDepthQueryTreeVisitor.h>
#include <Analyzer/ConstantNode.h>
#include <Analyzer/ColumnNode.h>
#include <Analyzer/FunctionNode.h>
#include <Analyzer/TableNode.h>
#include <Analyzer/TableFunctionNode.h>
#include <Analyzer/Utils.h>

namespace DB
{

namespace
{

std::tuple<FunctionNode *, ColumnNode *, TableNode *> getTypedNodesForOptimization(const QueryTreeNodePtr & node)
{
    auto * function_node = node->as<FunctionNode>();
    if (!function_node)
        return {};

    auto & function_arguments_nodes = function_node->getArguments().getNodes();
    if (function_arguments_nodes.empty() || function_arguments_nodes.size() > 2)
        return {};

    auto * first_argument_column_node = function_arguments_nodes.front()->as<ColumnNode>();
    if (!first_argument_column_node || first_argument_column_node->getColumnName() == "__grouping_set")
        return {};

    auto column_source = first_argument_column_node->getColumnSource();
    auto * table_node = column_source->as<TableNode>();
    if (!table_node)
        return {};

    const auto & storage = table_node->getStorage();
    const auto & storage_snapshot = table_node->getStorageSnapshot();
    auto column = first_argument_column_node->getColumn();

    if (!storage->supportsOptimizationToSubcolumns() || storage->isVirtualColumn(column.name, storage_snapshot->metadata))
        return {};

    auto column_in_table = storage_snapshot->tryGetColumn(GetColumnsOptions::All, column.name);
    if (!column_in_table || !column_in_table->type->equals(*column.type))
        return {};

    return std::make_tuple(function_node, first_argument_column_node, table_node);
}

/// First pass collects info about identifiers to determine which identifiers are allowed to optimize.
class FunctionToSubcolumnsVisitorFirstPass : public InDepthQueryTreeVisitorWithContext<FunctionToSubcolumnsVisitorFirstPass>
{
public:
    using Base = InDepthQueryTreeVisitorWithContext<FunctionToSubcolumnsVisitorFirstPass>;
    using Base::Base;

    struct Data
    {
        bool has_final = false;
        std::unordered_set<Identifier> all_key_columns;
        std::unordered_map<Identifier, UInt64> indentifiers_count;
        std::unordered_map<Identifier, UInt64> optimized_identifiers_count;
    };

    Data getData() const { return data; }

    void enterImpl(const QueryTreeNodePtr & node)
    {
        if (!getSettings().optimize_functions_to_subcolumns)
            return;

        if (data.has_final)
            return;

        if (auto * table_node = node->as<TableNode>())
        {
            enterImpl(*table_node);
            return;
        }

        if (auto * column_node = node->as<ColumnNode>())
        {
            enterImpl(*column_node);
            return;
        }

        auto [function_node, first_argument_node, table_node] = getTypedNodesForOptimization(node);
        if (function_node && first_argument_node && table_node)
        {
            enterImpl(*function_node, *first_argument_node, *table_node);
            return;
        }
    }

private:
    Data data;
    NameSet processed_tables;

    void enterImpl(const TableNode & table_node)
    {
        if (table_node.hasTableExpressionModifiers() && table_node.getTableExpressionModifiers()->hasFinal())
        {
            data.has_final = true;
            return;
        }

        auto table_name = table_node.getStorage()->getStorageID().getFullTableName();
        if (processed_tables.emplace(table_name).second)
            return;

        auto add_key_columns = [&](const auto & key_columns)
        {
            for (const auto & column_name : key_columns)
            {
                Identifier identifier({table_name, column_name});
                data.all_key_columns.insert(identifier);
            }
        };

        const auto & metadata_snapshot = table_node.getStorageSnapshot()->metadata;

        const auto & primary_key_columns = metadata_snapshot->getColumnsRequiredForPrimaryKey();
        add_key_columns(primary_key_columns);

        const auto & partition_key_columns = metadata_snapshot->getColumnsRequiredForPartitionKey();
        add_key_columns(partition_key_columns);

        for (const auto & index : metadata_snapshot->getSecondaryIndices())
        {
            const auto & index_columns = index.expression->getRequiredColumns();
            add_key_columns(index_columns);
        }
    }

    void enterImpl(const ColumnNode & column_node)
    {
        if (column_node.getColumnName() == "__grouping_set")
            return;

        auto column_source = column_node.getColumnSource();
        auto * table_node = column_source->as<TableNode>();
        if (!table_node)
            return;

        auto table_name = table_node->getStorage()->getStorageID().getFullTableName();
        Identifier qualified_name({table_name, column_node.getColumnName()});

        ++data.indentifiers_count[qualified_name];
    }

    void enterImpl(const FunctionNode & function_node, const ColumnNode & first_argument_column_node, const TableNode & table_node)
    {
        const auto & function_arguments_nodes = function_node.getArguments().getNodes();
        const auto & function_name = function_node.getFunctionName();

        auto column = first_argument_column_node.getColumn();
        WhichDataType column_type(column.type);

        auto table_name = table_node.getStorage()->getStorageID().getFullTableName();
        Identifier qualified_name({table_name, column.name});

        if (function_arguments_nodes.size() == 1)
        {
            if (column_type.isArray())
            {
                if (function_name == "length" || function_name == "empty" || function_name == "notEmpty")
                    ++data.optimized_identifiers_count[qualified_name];
            }
            else if (column_type.isNullable())
            {
                if (function_name == "count" || function_name == "isNull" || function_name == "isNotNull")
                    ++data.optimized_identifiers_count[qualified_name];
            }
            else if (column_type.isMap())
            {
                if (function_name == "length" || function_name == "mapKeys" || function_name == "mapValues")
                    ++data.optimized_identifiers_count[qualified_name];
            }
        }
        else if (function_arguments_nodes.size() == 2)
        {
            const auto * second_argument_constant_node = function_arguments_nodes[1]->as<ConstantNode>();
            if (function_name == "tupleElement" && column_type.isTuple() && second_argument_constant_node)
            {
                const auto & constant_value = second_argument_constant_node->getValue();
                const auto & constant_value_type = constant_value.getType();

                if (constant_value_type == Field::Types::String || constant_value_type == Field::Types::UInt64)
                    ++data.optimized_identifiers_count[qualified_name];
            }
            else if (function_name == "variantElement" && column_type.isVariant() && second_argument_constant_node)
            {
                if (second_argument_constant_node->getValue().getType() == Field::Types::String)
                    ++data.optimized_identifiers_count[qualified_name];
            }
            else if (function_name == "mapContains" && column_type.isMap())
            {
                ++data.optimized_identifiers_count[qualified_name];
            }
        }
    }
};

/// Second pass optimizes functions to subcolumns for allowed identifiers.
class FunctionToSubcolumnsVisitorSecondPass : public InDepthQueryTreeVisitorWithContext<FunctionToSubcolumnsVisitorSecondPass>
{
private:
    std::unordered_set<Identifier> identifiers_to_optimize;

public:
    using Base = InDepthQueryTreeVisitorWithContext<FunctionToSubcolumnsVisitorSecondPass>;
    using Base::Base;

    FunctionToSubcolumnsVisitorSecondPass(ContextPtr context_, std::unordered_set<Identifier> identifiers_to_optimize_)
        : Base(std::move(context_)), identifiers_to_optimize(std::move(identifiers_to_optimize_))
    {
    }

    void enterImpl(QueryTreeNodePtr & node) const
    {
        if (!getSettings().optimize_functions_to_subcolumns)
            return;

        auto [function_node, first_argument_column_node, table_node] = getTypedNodesForOptimization(node);
        if (!function_node || !first_argument_column_node || !table_node)
            return;

        auto & function_arguments_nodes = function_node->getArguments().getNodes();
        const auto & function_name = function_node->getFunctionName();

        auto column = first_argument_column_node->getColumn();
        auto table_name = table_node->getStorage()->getStorageID().getFullTableName();

        Identifier qualified_name({table_name, column.name});
        if (!identifiers_to_optimize.contains(qualified_name))
            return;

        auto column_source = first_argument_column_node->getColumnSource();
        WhichDataType column_type(column.type);

        if (function_arguments_nodes.size() == 1)
        {
            if (column_type.isArray())
            {
                if (function_name == "length")
                {
                    /// Replace `length(array_argument)` with `array_argument.size0`
                    column.name += ".size0";
                    column.type = std::make_shared<DataTypeUInt64>();

                    node = std::make_shared<ColumnNode>(column, column_source);
                }
                else if (function_name == "empty")
                {
                    /// Replace `empty(array_argument)` with `equals(array_argument.size0, 0)`
                    column.name += ".size0";
                    column.type = std::make_shared<DataTypeUInt64>();

                    function_arguments_nodes.clear();
                    function_arguments_nodes.push_back(std::make_shared<ColumnNode>(column, column_source));
                    function_arguments_nodes.push_back(std::make_shared<ConstantNode>(static_cast<UInt64>(0)));

                    resolveOrdinaryFunctionNodeByName(*function_node, "equals", getContext());
                }
                else if (function_name == "notEmpty")
                {
                    /// Replace `notEmpty(array_argument)` with `notEquals(array_argument.size0, 0)`
                    column.name += ".size0";
                    column.type = std::make_shared<DataTypeUInt64>();

                    function_arguments_nodes.clear();
                    function_arguments_nodes.push_back(std::make_shared<ColumnNode>(column, column_source));
                    function_arguments_nodes.push_back(std::make_shared<ConstantNode>(static_cast<UInt64>(0)));

                    resolveOrdinaryFunctionNodeByName(*function_node, "notEquals", getContext());
                }
            }
            else if (column_type.isNullable())
            {
                if (function_name == "count")
                {
                    /// Replace `count(nullable_argument)` with `sum(not(nullable_argument.null))`
                    column.name += ".null";
                    column.type = std::make_shared<DataTypeUInt8>();

                    auto column_node = std::make_shared<ColumnNode>(column, column_source);
                    auto function_node_not = std::make_shared<FunctionNode>("not");

                    function_node_not->getArguments().getNodes().push_back(std::move(column_node));
                    resolveOrdinaryFunctionNodeByName(*function_node_not, "not", getContext());

                    function_arguments_nodes = {std::move(function_node_not)};
                    resolveAggregateFunctionNodeByName(*function_node, "sum");
                }
                else if (function_name == "isNull")
                {
                    /// Replace `isNull(nullable_argument)` with `nullable_argument.null`
                    column.name += ".null";
                    column.type = std::make_shared<DataTypeUInt8>();

                    node = std::make_shared<ColumnNode>(column, column_source);
                }
                else if (function_name == "isNotNull")
                {
                    /// Replace `isNotNull(nullable_argument)` with `not(nullable_argument.null)`
                    column.name += ".null";
                    column.type = std::make_shared<DataTypeUInt8>();

                    function_arguments_nodes = {std::make_shared<ColumnNode>(column, column_source)};

                    resolveOrdinaryFunctionNodeByName(*function_node, "not", getContext());
                }
            }
            else if (column_type.isMap())
            {
                if (function_name == "length")
                {
                    /// Replace `length(map_argument)` with `map_argument.size0`
                    column.name += ".size0";
                    column.type = std::make_shared<DataTypeUInt64>();

                    node = std::make_shared<ColumnNode>(column, column_source);
                }
                else if (function_name == "mapKeys")
                {
                    /// Replace `mapKeys(map_argument)` with `map_argument.keys`
                    column.name += ".keys";
                    column.type = function_node->getResultType();

                    node = std::make_shared<ColumnNode>(column, column_source);
                }
                else if (function_name == "mapValues")
                {
                    /// Replace `mapValues(map_argument)` with `map_argument.values`
                    column.name += ".values";
                    column.type = function_node->getResultType();

                    node = std::make_shared<ColumnNode>(column, column_source);
                }
            }
        }
        else if (function_arguments_nodes.size() == 2)
        {
            const auto * second_argument_constant_node = function_arguments_nodes[1]->as<ConstantNode>();
            if (function_name == "tupleElement" && column_type.isTuple() && second_argument_constant_node)
            {
                /** Replace `tupleElement(tuple_argument, string_literal)`, `tupleElement(tuple_argument, integer_literal)`
                  * with `tuple_argument.column_name`.
                  */
                const auto & tuple_element_constant_value = second_argument_constant_node->getValue();
                const auto & tuple_element_constant_value_type = tuple_element_constant_value.getType();

                const auto & data_type_tuple = assert_cast<const DataTypeTuple &>(*column.type);

                String subcolumn_name;

                if (tuple_element_constant_value_type == Field::Types::String)
                {
                    subcolumn_name = tuple_element_constant_value.get<const String &>();
                }
                else if (tuple_element_constant_value_type == Field::Types::UInt64)
                {
                    auto tuple_column_index = tuple_element_constant_value.get<UInt64>();
                    subcolumn_name = data_type_tuple.getNameByPosition(tuple_column_index);
                }
                else
                {
                    return;
                }

                column.name += '.';
                column.name += subcolumn_name;
                column.type = function_node->getResultType();

                node = std::make_shared<ColumnNode>(column, column_source);
            }
            else if (function_name == "variantElement" && isVariant(column_type) && second_argument_constant_node)
            {
                /// Replace `variantElement(variant_argument, type_name)` with `variant_argument.type_name`.
                const auto & variant_element_constant_value = second_argument_constant_node->getValue();
                String subcolumn_name;

                if (variant_element_constant_value.getType() != Field::Types::String)
                    return;

                subcolumn_name = variant_element_constant_value.get<const String &>();

                column.name += '.';
                column.name += subcolumn_name;
                column.type = function_node->getResultType();

                node = std::make_shared<ColumnNode>(column, column_source);
            }
            else if (function_name == "mapContains" && column_type.isMap())
            {
                const auto & data_type_map = assert_cast<const DataTypeMap &>(*column.type);

                /// Replace `mapContains(map_argument, argument)` with `has(map_argument.keys, argument)`
                column.name += ".keys";
                column.type = std::make_shared<DataTypeArray>(data_type_map.getKeyType());

                auto has_function_argument = std::make_shared<ColumnNode>(column, column_source);
                function_arguments_nodes[0] = std::move(has_function_argument);

                resolveOrdinaryFunctionNodeByName(*function_node, "has", getContext());
            }
        }
    }
};

}

void FunctionToSubcolumnsPass::run(QueryTreeNodePtr query_tree_node, ContextPtr context)
{
    FunctionToSubcolumnsVisitorFirstPass first_visitor(context);
    first_visitor.visit(query_tree_node);
    auto data = first_visitor.getData();

    /// For queries with FINAL converting function to subcolumn may alter
    /// special merging algorithms and produce wrong result of query.
    if (data.has_final)
        return;

    /// Do not optimize if full column is requested in other context.
    /// It doesn't make sense because it doesn't reduce amount of read data
    /// and optimized functions are not computation heavy. But introducing
    /// new identifier complicates query analysis and may break it.
    ///
    /// E.g. query:
    ///     SELECT n FROM table GROUP BY n HAVING isNotNull(n)
    /// may be optimized to incorrect query:
    ///     SELECT n FROM table GROUP BY n HAVING not(n.null)
    /// Will produce: `n.null` is not under aggregate function and not in GROUP BY keys)
    ///
    /// Do not optimize index columns (primary, min-max, secondary),
    /// because otherwise analysis of indexes may be broken.
    /// TODO: handle subcolumns in index analysis.

    std::unordered_set<Identifier> identifiers_to_optimize;
    for (const auto & [identifier, count] : data.optimized_identifiers_count)
        if (!data.all_key_columns.contains(identifier) && data.indentifiers_count[identifier] == count)
            identifiers_to_optimize.insert(identifier);

    if (identifiers_to_optimize.empty())
        return;

    FunctionToSubcolumnsVisitorSecondPass second_visitor(std::move(context), std::move(identifiers_to_optimize));
    second_visitor.visit(query_tree_node);
}

}

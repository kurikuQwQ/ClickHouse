#include <Processors/QueryPlan/JoinStep2.h>

#include <QueryPipeline/QueryPipelineBuilder.h>
#include <Processors/Transforms/JoiningTransform.h>
#include <Interpreters/IJoin.h>
#include <Interpreters/TableJoin.h>
#include <IO/Operators.h>
#include <Common/JSONBuilder.h>
#include <Common/typeid_cast.h>
#include <Interpreters/TableJoin.h>
#include <ranges>

namespace DB
{


namespace ErrorCodes
{
    extern const int NOT_IMPLEMENTED;
}

std::string_view toString(PredicateOperator op)
{
    switch (op)
    {
        case PredicateOperator::Equal: return "=";
        case PredicateOperator::NullSafeEqual: return "<=>";
        case PredicateOperator::Less: return "<";
        case PredicateOperator::LessOrEquals: return "<=";
        case PredicateOperator::Greater: return ">";
        case PredicateOperator::GreaterOrEquals: return ">=";
    }
}

std::string formatJoinCondition(const JoinCondition & join_condition)
{
    auto quote_string = std::views::transform([](const auto & s) { return fmt::format("({})", s->formatASTForErrorMessage()); });
    auto format_predicate = std::views::transform([](const auto & p) { return fmt::format("{} {} `{}`", p.left_node->formatASTForErrorMessage(), toString(p.op), p.right_node->formatASTForErrorMessage()); });
    Strings desc;
    desc.push_back(fmt::format("Keys: ({})", fmt::join(join_condition.predicates | format_predicate, " AND ")));
    if (!join_condition.left_filter_conditions.empty())
        desc.push_back(fmt::format("Left: ({})", fmt::join(join_condition.left_filter_conditions | quote_string, " AND ")));
    if (!join_condition.right_filter_conditions.empty())
        desc.push_back(fmt::format("Right: ({})", fmt::join(join_condition.right_filter_conditions | quote_string, " AND ")));
    if (!join_condition.residual_conditions.empty())
        desc.push_back(fmt::format("Residual: ({})", fmt::join(join_condition.residual_conditions | quote_string, " AND ")));
    return fmt::format("[{}]", fmt::join(desc, ", "));
}

std::vector<std::pair<String, String>> describeJoinActions(const JoinInfo & join_info)
{
    std::vector<std::pair<String, String>> description;

    description.emplace_back("Type", toString(join_info.kind));
    description.emplace_back("Strictness", toString(join_info.strictness));
    description.emplace_back("Locality", toString(join_info.locality));
    description.emplace_back("Expression", fmt::format("{} {}",
            join_info.expression.is_using ? "USING" : "ON",
            fmt::join(join_info.expression.disjunctive_conditions | std::views::transform(formatJoinCondition), " | ")));

    return description;
}


JoinStep2::JoinStep2(
    const DataStream & left_stream_,
    const DataStream & right_stream_,
    JoinInfo join_info_,
    Names required_output_columns_,
    ContextPtr context_)
    : join_info(std::move(join_info_))
    , required_output_columns(std::move(required_output_columns_))
    , context(std::move(context_))
{
    updateInputStreams(DataStreams{left_stream_, right_stream_});
}

QueryPipelineBuilderPtr JoinStep2::updatePipeline(QueryPipelineBuilders pipelines, const BuildQueryPipelineSettings & settings)
{
    UNUSED(pipelines);
    UNUSED(settings);
    throw Exception(ErrorCodes::NOT_IMPLEMENTED, "");
}


void JoinStep2::describePipeline(FormatSettings & settings) const
{
    IQueryPlanStep::describePipeline(processors, settings);
}

void JoinStep2::describeActions(FormatSettings & settings) const
{
    String prefix(settings.offset, ' ');

    for (const auto & [name, value] : describeJoinActions(join_info))
        settings.out << prefix << name << ": " << value << '\n';
}

void JoinStep2::describeActions(JSONBuilder::JSONMap & map) const
{
    for (const auto & [name, value] : describeJoinActions(join_info))
        map.add(name, value);
}

static Block stackHeadersFromStreams(const DataStreams & input_streams, const Names & required_output_columns)
{
    NameSet required_output_columns_set(required_output_columns.begin(), required_output_columns.end());

    Block result_header;
    for (const auto & stream : input_streams)
    {
        for (const auto & column : stream.header)
        {
            if (required_output_columns_set.contains(column.name))
            {
                result_header.insert(column);
            }
            else if (required_output_columns_set.empty())
            {
                /// If no required columns specified, use one first column.
                result_header.insert(column);
                return result_header;
            }
        }
    }
    return result_header;
}

void JoinStep2::updateOutputStream()
{
    output_stream = DataStream
    {
        .header = stackHeadersFromStreams(input_streams, required_output_columns),
    };
}

}

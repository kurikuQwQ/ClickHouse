#pragma once

#include <Processors/QueryPlan/IQueryPlanStep.h>
#include <Processors/QueryPlan/ITransformingStep.h>
#include <Interpreters/JoinInfo.h>

namespace DB
{

/// Perform JOIN operation.
class JoinStep2 final : public IQueryPlanStep
{
public:
    JoinStep2(
        const DataStream & left_stream_,
        const DataStream & right_stream_,
        JoinInfo join_info_,
        Names required_output_columns_,
        ContextPtr context_);

    String getName() const override { return "Join"; }

    QueryPipelineBuilderPtr updatePipeline(QueryPipelineBuilders pipelines, const BuildQueryPipelineSettings &) override;

    void describePipeline(FormatSettings & settings) const override;

    void describeActions(JSONBuilder::JSONMap & map) const override;
    void describeActions(FormatSettings & settings) const override;

    bool canUpdateInputStream() const override { return true; }

    bool allowPushDownToRight() const;

protected:
    void updateOutputStream() override;

    JoinInfo join_info;
    Names required_output_columns;
    ContextPtr context;
};

std::vector<std::pair<String, String>> describeJoinActions(const JoinInfo & join_info);

}

#include <Core/Block.h>
#include <DataStreams/formatBlock.h>
#include <Processors/Formats/IOutputFormat.h>
#include <Processors/Sources/SourceFromSingleChunk.h>
#include <Processors/QueryPipeline.h>
#include <Processors/Executors/CompletedPipelineExecutor.h>

namespace DB
{

void formatBlock(OutputFormatPtr out, const Block & block)
{
    auto source = std::make_shared<SourceFromSingleChunk>(block);
    QueryPipeline pipeline(source);
    pipeline.complete(out);
    CompletedPipelineExecutor executor(pipeline);
    executor.execute();
    out->flush();
}

}

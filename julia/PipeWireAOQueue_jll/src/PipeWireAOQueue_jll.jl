# Use baremodule to shave off a few KB from the serialized `.ji` file
baremodule PipeWireAOQueue_jll
using Base
using Base: UUID
import JLLWrappers

JLLWrappers.@generate_main_file_header("PipeWireAOQueue")
JLLWrappers.@generate_main_file("PipeWireAOQueue", Base.UUID("30c7d52a-5243-5e1f-b778-22f49ec5fee3"))
end  # module PipeWireAOQueue_jll

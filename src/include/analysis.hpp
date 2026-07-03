#pragma once
#include "duckdb.hpp"

namespace duckdb {

// Scalar UDFs: each takes a file path (VARCHAR) and analyzes the audio.
void RbBpmFun(DataChunk &args, ExpressionState &state, Vector &result);       // -> DOUBLE
void RbKeyFun(DataChunk &args, ExpressionState &state, Vector &result);       // -> VARCHAR (Camelot)  [DONE]
void RbBeatgridFun(DataChunk &args, ExpressionState &state, Vector &result);  // -> DOUBLE[]
void RbLoudnessFun(DataChunk &args, ExpressionState &state, Vector &result);  // -> STRUCT
// One-decode, all-features. -> STRUCT(bpm,key,beatgrid,lufs,true_peak,lra). Use this.
void RbAnalyzeFun(DataChunk &args, ExpressionState &state, Vector &result);

} // namespace duckdb

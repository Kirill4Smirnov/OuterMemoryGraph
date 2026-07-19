# LDBC Graphalytics PageRank fixture

`test-pr-directed` is the official small directed PageRank validation graph
from the [LDBC Graphalytics dataset suite](https://ldbcouncil.org/benchmarks/graphalytics/datasets/).

The fixture specifies 50 vertices, 246 directed edges, damping factor `0.85`,
and 14 iterations. `test-pr-directed-PR` is the published reference output.
Graphalytics considers PageRank values valid when their relative error is at
most `0.0001`; the project test applies the same rule.


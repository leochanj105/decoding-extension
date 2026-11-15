# Copy the five tasks' rows, every field verbatim, from MultiPL-E mbpp-rs
# (revision 28441b6024e71d4a1c1c0f6bf171c935cd5a43f2) to a JSON-lines file.
import json
import sys

import pyarrow.parquet as pq

NAMES = [
    "mbpp_68_is_Monotonic",
    "mbpp_270_sum_even_and_even_index",
    "mbpp_296_get_Inv_Count",
    "mbpp_751_check_min_heap",
    "mbpp_767_get_pairs_count",
]

parquet_path, out_path = sys.argv[1], sys.argv[2]
rows = {r["name"]: r for r in pq.read_table(parquet_path).to_pylist()}
with open(out_path, "w") as out:
    for name in NAMES:
        out.write(json.dumps(rows[name]) + "\n")  # KeyError if a task is missing

WITH '{ "v":1.1}' AS raw SELECT JSONExtract(raw, 'float') AS float32_1, JSONExtract(concat(tuple('1970-01-05', 10, materialize(10), 10, 10, 10, toUInt256(10), 10, toNullable(10), 10, 10), materialize(toUInt256(0)), ', ', 2, 2, toLowCardinality(toLowCardinality(2))), raw, toLowCardinality('v'), 'Float32') AS float32_2, JSONExtractFloat(raw) AS float64_1, JSONExtract(raw, 'v', 'double') AS float64_2;


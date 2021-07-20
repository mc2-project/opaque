<img src="https://mc2-project.github.io/opaque-sql/opaque.svg" width="315" alt="Opaque">

## Secure Apache Spark SQL

![Tests Status](https://github.com/mc2-project/opaque/actions/workflows/main.yml/badge.svg) 
[![License](https://img.shields.io/badge/License-Apache%202.0-blue.svg)](https://opensource.org/licenses/Apache-2.0) 
[<img src="https://img.shields.io/badge/slack-contact%20us-blueviolet?logo=slack">](https://join.slack.com/t/mc2-project/shared_invite/zt-rt3kxyy8-GS4KA0A351Ysv~GKwy8NEQ)
[![Contributor Covenant](https://img.shields.io/badge/Contributor%20Covenant-2.0-4baaaa.svg)](CODE_OF_CONDUCT.md)

Welcome to the landing page of Opaque SQL! Opaque SQL is a package for Apache Spark SQL that enables processing over encrypted DataFrames using the OpenEnclave framework. 

### Quick start
To quickly get started running Opaque SQL, please refer to this [usage section](https://mc2-project.github.io/client-docs/opaquesql_usage.html). Note that Opaque SQL requires the [MC<sup>2</sup> client](https://github.com/mc2-project/mc2) in order to securely run an encrypted query.

### Usage
Similar to Apache Spark SQL, Opaque SQL offer an *encrypted DataFrame abstraction*. Users familiar with the Spark API can easily run queries on encrypted DataFrames using the same API. The main difference is that we support saving and loading of DataFrames, but not actions like `.collect` or `.show`. An example script is the following:

```scala
// Import hooks to Opaque SQL
import edu.berkeley.cs.rise.opaque.implicits._
import org.apache.spark.sql.types._

// Load an encrypted DataFrame (saved using the MC2 client)
val df_en = spark.read.format("edu.berkeley.cs.rise.opaque.EncryptedSource").load("/tmp/opaquesql.csv.enc")
// Run a filter query on the encrypted DataFrame
val result = df_enc.filter($"Age" < lit(30))
// This will save the encrypted result to the result directory on the cloud
result.write.format("edu.berkeley.cs.rise.opaque.EncryptedSource").save("/tmp/opaque_sql_result")
```

For more details on how to use Opaque SQL, please refer to [this section](https://mc2-project.github.io/opaque-sql-docs/src/usage/usage.html).

### Documentation
For more details on building, using, and contributing, please see our [documentation](https://mc2-project.github.io/opaque-sql-docs/src/index.html).

### Paper
The open source is based on our NSDI 2017 [paper](https://www.usenix.org/system/files/conference/nsdi17/nsdi17-zheng.pdf).

### Contact
Join the discussion on [Slack](https://join.slack.com/t/mc2-project/shared_invite/zt-rt3kxyy8-GS4KA0A351Ysv~GKwy8NEQ) or email us at mc2-dev@googlegroups.com.

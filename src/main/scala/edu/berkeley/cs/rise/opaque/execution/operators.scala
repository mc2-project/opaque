/*
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package edu.berkeley.cs.rise.opaque.execution

import scala.collection.mutable.ArrayBuffer

import edu.berkeley.cs.rise.opaque.RA
import edu.berkeley.cs.rise.opaque.Utils
import org.apache.spark.rdd.RDD
import org.apache.spark.sql.catalyst.InternalRow
import org.apache.spark.sql.catalyst.expressions.AttributeSet
import org.apache.spark.sql.catalyst.expressions._
import org.apache.spark.sql.catalyst.expressions.aggregate._
import org.apache.spark.sql.catalyst.plans.JoinType
import org.apache.spark.sql.catalyst.plans.physical.Partitioning
import org.apache.spark.sql.execution.SparkPlan
import org.apache.spark.sql.types._
import org.apache.spark.unsafe.types.UTF8String

trait LeafExecNode extends SparkPlan {
  override final def children: Seq[SparkPlan] = Nil
  override def producedAttributes: AttributeSet = outputSet
}

trait UnaryExecNode extends SparkPlan {
  def child: SparkPlan

  override final def children: Seq[SparkPlan] = child :: Nil

  override def outputPartitioning: Partitioning = child.outputPartitioning
}

trait BinaryExecNode extends SparkPlan {
  def left: SparkPlan
  def right: SparkPlan

  override final def children: Seq[SparkPlan] = Seq(left, right)
}

case class EncryptedLocalTableScanExec(
    output: Seq[Attribute],
    plaintextData: Seq[InternalRow],
    override val isOblivious: Boolean)
  extends LeafExecNode with OpaqueOperatorExec {

  private val unsafeRows: Array[InternalRow] = {
    val proj = UnsafeProjection.create(output, output)
    println(s"unsafeRows input: ${plaintextData.toList}")
    println(s"unsafeRows proj: ${output}")
    val result: Array[InternalRow] = plaintextData.map(r => proj(r).copy()).toArray
    println(s"unsafeRows output: ${result.toList}")
    result
  }

  override def executeBlocked(): RDD[Block] = {
    // Locally partition plaintextData using the same logic as ParallelCollectionRDD.slice
    def positions(length: Long, numSlices: Int): Iterator[(Int, Int)] = {
      (0 until numSlices).iterator.map { i =>
        val start = ((i * length) / numSlices).toInt
        val end = (((i + 1) * length) / numSlices).toInt
        (start, end)
      }
    }
    val slicedPlaintextData: Seq[Seq[InternalRow]] =
      positions(unsafeRows.length, sqlContext.sparkContext.defaultParallelism).map {
        case (start, end) => unsafeRows.slice(start, end).toSeq
      }.toSeq

    // Encrypt each local partition
    val encryptedPartitions: Seq[Block] =
      slicedPlaintextData.map(slice =>
        Utils.encryptInternalRowsFlatbuffers(slice, output.map(_.dataType)))

    // Make an RDD from the encrypted partitions
    sqlContext.sparkContext.parallelize(encryptedPartitions)
  }
}

case class EncryptExec(
    override val isOblivious: Boolean,
    child: SparkPlan)
  extends UnaryExecNode with OpaqueOperatorExec {

  override def output: Seq[Attribute] = child.output

  override def executeBlocked(): RDD[Block] = {
    child.execute().mapPartitions { rowIter =>
      Iterator(Utils.encryptInternalRowsFlatbuffers(rowIter.toSeq, output.map(_.dataType)))
    }
  }
}

case class EncryptedBlockRDDScanExec(
    output: Seq[Attribute],
    rdd: RDD[Block],
    override val isOblivious: Boolean)
  extends LeafExecNode with OpaqueOperatorExec {

  override def executeBlocked(): RDD[Block] = rdd
}

case class Block(bytes: Array[Byte], numRows: Int) extends Serializable

trait OpaqueOperatorExec extends SparkPlan {
  def executeBlocked(): RDD[Block]

  def isOblivious: Boolean = children.exists(_.find {
    case p: OpaqueOperatorExec => p.isOblivious
    case _ => false
  }.nonEmpty)

  override def doExecute() = {
    sqlContext.sparkContext.emptyRDD
    // throw new UnsupportedOperationException("use executeBlocked")
  }

  override def executeCollect(): Array[InternalRow] = {
    executeBlocked().collect().flatMap { block =>
      Utils.decryptBlockFlatbuffers(block)
    }
  }

  override def executeTake(n: Int): Array[InternalRow] = {
    if (n == 0) {
      return new Array[InternalRow](0)
    }

    val childRDD = executeBlocked()

    val buf = new ArrayBuffer[InternalRow]
    val totalParts = childRDD.partitions.length
    var partsScanned = 0
    while (buf.size < n && partsScanned < totalParts) {
      // The number of partitions to try in this iteration. It is ok for this number to be
      // greater than totalParts because we actually cap it at totalParts in runJob.
      var numPartsToTry = 1L
      if (partsScanned > 0) {
        // If we didn't find any rows after the first iteration, just try all partitions next.
        // Otherwise, interpolate the number of partitions we need to try, but overestimate it
        // by 50%.
        if (buf.size == 0) {
          numPartsToTry = totalParts - 1
        } else {
          numPartsToTry = (1.5 * n * partsScanned / buf.size).toInt
        }
      }
      numPartsToTry = math.max(0, numPartsToTry)  // guard against negative num of partitions

      val left = n - buf.size
      val p = partsScanned.until(math.min(partsScanned + numPartsToTry, totalParts).toInt)
      val sc = sqlContext.sparkContext
      val res = sc.runJob(childRDD,
        (it: Iterator[Block]) => if (it.hasNext) Some(it.next()) else None, p)

      res.foreach {
        case Some(block) =>
          buf ++= Utils.decryptBlockFlatbuffers(block)
        case None => {}
      }

      partsScanned += p.size
    }

    if (buf.size > n) {
      buf.take(n).toArray
    } else {
      buf.toArray
    }
  }
}

/**
 * An extractor that matches expressions that represent a column of the input attributes (i.e., if
 * the expression is a direct reference to the column, or it is derived solely from the column). To
 * use this extractor, create an object deriving from this trait and provide a value for `input`.
 */
trait ColumnNumberMatcher extends Serializable {
  def input: Seq[Attribute]
  def unapply(expr: Expression): Option[(Int, DataType)] =
    if (expr.references.size == 1) {
      val attr = expr.references.head
      val colNum = input.indexWhere(attr.semanticEquals(_))
      if (colNum != -1) {
        Some(Tuple2(colNum + 1, attr.dataType))
      } else {
        None
      }
    } else {
      None
    }
}

case class ObliviousProjectExec(projectList: Seq[NamedExpression], child: SparkPlan)
  extends UnaryExecNode with OpaqueOperatorExec {

  override def output: Seq[Attribute] = projectList.map(_.toAttribute)

  override def executeBlocked() = {
    val execRDD = child.asInstanceOf[OpaqueOperatorExec].executeBlocked()
    Utils.ensureCached(execRDD)
    // RA.initRA(execRDD)
    val projectListSer = Utils.serializeProjectList(projectList, child.output)

    execRDD.map { block =>
      val (enclave, eid) = Utils.initEnclave()
      val serResult = enclave.Project(eid, projectListSer, block.bytes)
      Block(serResult, block.numRows)
    }
  }
}

case class ObliviousFilterExec(condition: Expression, child: SparkPlan)
  extends UnaryExecNode with OpaqueOperatorExec {

  override def output: Seq[Attribute] = child.output

  override def executeBlocked(): RDD[Block] = {
    val execRDD = child.asInstanceOf[OpaqueOperatorExec].executeBlocked()
    Utils.ensureCached(execRDD)
    // RA.initRA(execRDD)
    val conditionSer = Utils.serializeFilterExpression(condition, child.output)
    println(s"conditionSer = ${conditionSer.size} bytes")
    return execRDD.map { block =>
      val (enclave, eid) = Utils.initEnclave()
      val numOutputRows = new MutableInteger
      val filtered = enclave.Filter(
        eid, conditionSer, block.bytes, numOutputRows)
      Block(filtered, numOutputRows.value)
    }
  }
}

case class ObliviousPermuteExec(child: SparkPlan) extends UnaryExecNode with OpaqueOperatorExec {
  override def output: Seq[Attribute] = child.output

  override def executeBlocked() = {
    import Opcode._
    val execRDD = child.asInstanceOf[OpaqueOperatorExec].executeBlocked()
    Utils.ensureCached(execRDD)
    RA.initRA(execRDD)

    val rowsWithRandomIds = execRDD.map { block =>
      val (enclave, eid) = Utils.initEnclave()
      val serResult = enclave.Project(
        eid, ??? /*OP_PROJECT_ADD_RANDOM_ID.value*/, block.bytes)
      Block(serResult, block.numRows)
    }
    
    ObliviousSortExec.sortBlocks(rowsWithRandomIds, OP_SORT_COL1).map { block =>
      val (enclave, eid) = Utils.initEnclave()
      val serResult = enclave.Project(
        eid, ??? /*OP_PROJECT_DROP_COL1.value*/, block.bytes)
      Block(serResult, block.numRows)
    }
  }
}

case class ObliviousAggregateExec(
    groupingExpressions: Seq[Expression],
    aggExpressions: Seq[NamedExpression],
    child: SparkPlan)
  extends UnaryExecNode with OpaqueOperatorExec {

  import Utils.time

  private object Col extends ColumnNumberMatcher {
    override def input: Seq[Attribute] = child.output
  }

  override def producedAttributes: AttributeSet =
    AttributeSet(aggExpressions) -- AttributeSet(groupingExpressions)

  override def output: Seq[Attribute] = aggExpressions.map(_.toAttribute)

  override def executeBlocked(): RDD[Block] = {
    import Opcode._
    val (aggStep1Opcode, aggStep2Opcode, aggDummySortOpcode, aggDummyFilterOpcode) =
      (groupingExpressions, aggExpressions) match {
        case (Seq(Col(1, _)), Seq(Col(1, _),
          Alias(AggregateExpression(Sum(Col(2, IntegerType)), _, false, _), _))) =>
          (OP_GROUPBY_COL1_SUM_COL2_INT_STEP1,
            OP_GROUPBY_COL1_SUM_COL2_INT_STEP2,
            OP_SORT_COL2_IS_DUMMY_COL1,
            OP_FILTER_NOT_DUMMY)

        case (Seq(Col(1, _)), Seq(Col(1, _),
          Alias(AggregateExpression(Sum(Col(2, FloatType)), _, false, _), _))) =>
          (OP_GROUPBY_COL1_SUM_COL2_FLOAT_STEP1,
            OP_GROUPBY_COL1_SUM_COL2_FLOAT_STEP2,
            OP_SORT_COL2_IS_DUMMY_COL1,
            OP_FILTER_NOT_DUMMY)

        case (Seq(Col(1, _)), Seq(Col(1, _),
          Alias(AggregateExpression(Min(Col(2, IntegerType)), _, false, _), _))) =>
          (OP_GROUPBY_COL1_MIN_COL2_INT_STEP1,
            OP_GROUPBY_COL1_MIN_COL2_INT_STEP2,
            OP_SORT_COL2_IS_DUMMY_COL1,
            OP_FILTER_NOT_DUMMY)

        case (Seq(Col(2, _)), Seq(Col(2, _),
          Alias(AggregateExpression(Sum(Col(3, IntegerType)), _, false, _), _))) =>
          (OP_GROUPBY_COL2_SUM_COL3_INT_STEP1,
            OP_GROUPBY_COL2_SUM_COL3_INT_STEP2,
            OP_SORT_COL2_IS_DUMMY_COL1,
            OP_FILTER_NOT_DUMMY)

        case (Seq(Col(1, _)), Seq(Col(1, _),
          Alias(AggregateExpression(Sum(Col(3, FloatType)), _, false, _), _),
          Alias(AggregateExpression(Average(Col(2, IntegerType)), _, false, _), _))) =>
          (OP_GROUPBY_COL1_SUM_COL3_FLOAT_AVG_COL2_INT_STEP1,
            OP_GROUPBY_COL1_SUM_COL3_FLOAT_AVG_COL2_INT_STEP2,
            OP_SORT_COL2_IS_DUMMY_COL1,
            OP_FILTER_NOT_DUMMY)

        case (Seq(Col(1, _), Col(2, _)), Seq(Col(1, _), Col(2, _),
          Alias(AggregateExpression(Sum(Col(3, FloatType)), _, false, _), _))) =>
          (OP_GROUPBY_COL1_COL2_SUM_COL3_FLOAT_STEP1,
            OP_GROUPBY_COL1_COL2_SUM_COL3_FLOAT_STEP2,
            OP_SORT_COL2_IS_DUMMY_COL1,
            OP_FILTER_NOT_DUMMY)

      case _ =>
        throw new Exception(
          s"ObliviousAggregateExec: unknown grouping expressions $groupingExpressions, " +
            s"aggregation expressions $aggExpressions.\n" +
            s"Input: ${child.output}.\n" +
            s"Types: ${child.output.map(_.dataType)}")
      }

    val childRDD = child.asInstanceOf[OpaqueOperatorExec].executeBlocked()
    Utils.ensureCached(childRDD)
    time("aggregate - force child") { childRDD.count }
    // Process boundaries
    RA.initRA(childRDD)
    val boundaries = childRDD.map { block =>
      val (enclave, eid) = Utils.initEnclave()
      val boundary = enclave.AggregateStep1(
        eid, 0, 0, aggStep1Opcode.value, block.bytes, block.numRows)
      // enclave.StopEnclave(eid)
      boundary
    }

    val boundariesCollected = time("aggregate - step 1") { boundaries.collect }
    if (boundariesCollected.forall(_.isEmpty)) {
      return sqlContext.sparkContext.emptyRDD[Block]
    }
    val (enclave, eid) = Utils.initEnclave()
    val processedBoundariesConcat = time("aggregate - ProcessBoundary") {
      enclave.ProcessBoundary(
        eid, aggStep1Opcode.value,
        Utils.concatByteArrays(boundariesCollected), boundariesCollected.length)
    }

    // Send processed boundaries to partitions and generate a mix of partial and final aggregates
    val processedBoundaries = Utils.splitBytes(processedBoundariesConcat, boundariesCollected.length)
    val processedBoundariesRDD = sparkContext.parallelize(processedBoundaries, childRDD.partitions.length)
    val partialAggregates = childRDD.zipPartitions(processedBoundariesRDD) {
      (blockIter, boundaryIter) =>
        val blockArray = blockIter.toArray
        assert(blockArray.length == 1)
        val block = blockArray.head
        val boundaryArray = boundaryIter.toArray
        assert(boundaryArray.length == 1)
        val boundaryRecord = boundaryArray.head
        val (enclave, eid) = Utils.initEnclave()
        assert(block.numRows > 0)
        val partialAgg = enclave.AggregateStep2(
          eid, 0, 0, aggStep2Opcode.value, block.bytes, block.numRows, boundaryRecord)
        assert(partialAgg.nonEmpty,
          s"enclave.AggregateStep2($eid, 0, 0, $aggStep2Opcode, ${block.bytes.length}, ${block.numRows}, ${boundaryRecord.length}) returned empty result")
        Iterator(Block(partialAgg, block.numRows))
    }

    Utils.ensureCached(partialAggregates)
    time("aggregate - step 2") { partialAggregates.count }

    // Sort the partial and final aggregates using a comparator that causes final aggregates to come first
    val sortedAggregates = time("aggregate - sort dummies") {
      val result = ObliviousSortExec.sortBlocks(partialAggregates, aggDummySortOpcode)
      Utils.ensureCached(result)
      result.count
      result
    }

    // Filter out the non-final aggregates
    val finalAggregates = time("aggregate - filter out dummies") {
      val result = sortedAggregates.map { block =>
        val (enclave, eid) = Utils.initEnclave()
        val numOutputRows = new MutableInteger
        val filtered = enclave.Filter(
          eid, ???, block.bytes, numOutputRows)
        Block(filtered, numOutputRows.value)
      }
      Utils.ensureCached(result)
      result.count
      result
    }
    finalAggregates
  }
}

case class EncryptedAggregateExec(
    groupingExpressions: Seq[Expression],
    aggExpressions: Seq[NamedExpression],
    child: SparkPlan)
  extends UnaryExecNode with OpaqueOperatorExec {

  import Utils.time

  private object Col extends ColumnNumberMatcher {
    override def input: Seq[Attribute] = child.output
  }

  override def producedAttributes: AttributeSet =
    AttributeSet(aggExpressions) -- AttributeSet(groupingExpressions)

  override def output: Seq[Attribute] = aggExpressions.map(_.toAttribute)

  override def executeBlocked(): RDD[Block] = {
    import Opcode._
    val aggOpcode =
      (groupingExpressions, aggExpressions) match {
        case (Seq(Col(1, _)), Seq(Col(1, _),
          Alias(AggregateExpression(Sum(Col(2, IntegerType)), _, false, _), _))) =>
          OP_GROUPBY_COL1_SUM_COL2_INT

        case (Seq(Col(1, _)), Seq(Col(1, _),
          Alias(AggregateExpression(Sum(Col(2, FloatType)), _, false, _), _))) =>
          OP_GROUPBY_COL1_SUM_COL2_FLOAT

        case (Seq(Col(1, _)), Seq(Col(1, _),
          Alias(AggregateExpression(Min(Col(2, IntegerType)), _, false, _), _))) =>
          OP_GROUPBY_COL1_MIN_COL2_INT

        case (Seq(Col(1, _)), Seq(Col(1, _),
          Alias(AggregateExpression(Sum(Col(3, FloatType)), _, false, _), _),
          Alias(AggregateExpression(Average(Col(2, IntegerType)), _, false, _), _))) =>
          OP_GROUPBY_COL1_SUM_COL3_FLOAT_AVG_COL2_INT

      case _ =>
        throw new Exception(
          s"EncryptedAggregateExec: unknown grouping expressions $groupingExpressions, " +
            s"aggregation expressions $aggExpressions.\n" +
            s"Input: ${child.output}.\n" +
            s"Types: ${child.output.map(_.dataType)}")
      }

    val childRDD = child.asInstanceOf[OpaqueOperatorExec].executeBlocked()
    Utils.ensureCached(childRDD)
    time("aggregate - force child") { childRDD.count }
    RA.initRA(childRDD)
    // Process boundaries
    val aggregates = childRDD.map { block =>
      val (enclave, eid) = Utils.initEnclave()
      val numOutputRows = new MutableInteger
      val resultBytes = enclave.NonObliviousAggregate(
        eid, 0, 0, aggOpcode.value, block.bytes, block.numRows, numOutputRows)
      Block(resultBytes, numOutputRows.value)
    }
    Utils.ensureCached(aggregates)
    aggregates.count
    aggregates
  }
}

case class ObliviousSortMergeJoinExec(
    left: SparkPlan,
    right: SparkPlan,
    leftKeys: Seq[Expression],
    rightKeys: Seq[Expression],
    condition: Option[Expression])
  extends BinaryExecNode with OpaqueOperatorExec {

  import Utils.time

  override def output: Seq[Attribute] =
    left.output ++ right.output

  override def executeBlocked() = {
    import Opcode._
    val (joinOpcode, dummySortOpcode, dummyFilterOpcode) =
      OpaqueJoinUtils.getOpcodes(left.output, right.output, leftKeys, rightKeys, condition)

    val leftRDD = left.asInstanceOf[OpaqueOperatorExec].executeBlocked()
    val rightRDD = right.asInstanceOf[OpaqueOperatorExec].executeBlocked()
    Utils.ensureCached(leftRDD)
    time("Force left child of ObliviousSortMergeJoinExec") { leftRDD.count }
    Utils.ensureCached(rightRDD)
    time("Force right child of ObliviousSortMergeJoinExec") { rightRDD.count }

    RA.initRA(leftRDD)

    val processed = leftRDD.zipPartitions(rightRDD) { (leftBlockIter, rightBlockIter) =>
      val (enclave, eid) = Utils.initEnclave()

      val leftBlockArray = leftBlockIter.toArray
      assert(leftBlockArray.length == 1)
      val leftBlock = leftBlockArray.head

      val rightBlockArray = rightBlockIter.toArray
      assert(rightBlockArray.length == 1)
      val rightBlock = rightBlockArray.head

      val processed = enclave.JoinSortPreprocess(
        eid, 0, 0, joinOpcode.value, leftBlock.bytes, leftBlock.numRows,
        rightBlock.bytes, rightBlock.numRows)

      Iterator(Block(processed, leftBlock.numRows + rightBlock.numRows))
    }
    Utils.ensureCached(processed)
    time("join - preprocess") { processed.count }

    val sorted = time("join - sort") {
      val result = ObliviousSortExec.sortBlocks(processed, joinOpcode)
      Utils.ensureCached(result)
      result.count
      result
    }

    val lastPrimaryRows = sorted.map { block =>
      val (enclave, eid) = Utils.initEnclave()
      enclave.ScanCollectLastPrimary(eid, joinOpcode.value, block.bytes, block.numRows)
    }

    val lastPrimaryRowsCollected = time("join - collect last primary") { lastPrimaryRows.collect }
    val (enclave, eid) = Utils.initEnclave()
    val processedJoinRows = time("join - process boundary") {
      enclave.ProcessJoinBoundary(
        eid, joinOpcode.value, Utils.concatByteArrays(lastPrimaryRowsCollected),
        lastPrimaryRowsCollected.length)
    }

    val processedJoinRowsSplit = Utils.readVerifiedRows(processedJoinRows).toArray
    assert(processedJoinRowsSplit.length == sorted.partitions.length)
    val processedJoinRowsRDD =
      sparkContext.parallelize(processedJoinRowsSplit, sorted.partitions.length)

    val joined = sorted.zipPartitions(processedJoinRowsRDD) { (blockIter, joinRowIter) =>
      val block = blockIter.next()
      assert(!blockIter.hasNext)
      val joinRow = joinRowIter.next()
      assert(!joinRowIter.hasNext)
      val (enclave, eid) = Utils.initEnclave()
      val joined = enclave.SortMergeJoin(
        eid, 0, 0, joinOpcode.value, block.bytes, block.numRows, joinRow)
      Iterator(Block(joined, block.numRows))
    }
    Utils.ensureCached(joined)
    time("join - sort merge join") { joined.count }

    val joinedWithRandomIds = joined.map { block =>
      val (enclave, eid) = Utils.initEnclave()
      val serResult = enclave.Project(
        eid, ??? /*OP_PROJECT_ADD_RANDOM_ID.value*/, block.bytes)
      Block(serResult, block.numRows)
    }
    val permuted = ObliviousSortExec.sortBlocks(joinedWithRandomIds, OP_SORT_COL1).map { block =>
      val (enclave, eid) = Utils.initEnclave()
      val serResult = enclave.Project(
        eid, ??? /*OP_PROJECT_DROP_COL1.value*/, block.bytes)
      Block(serResult, block.numRows)
    }

    val nonDummy = permuted.map { block =>
      val (enclave, eid) = Utils.initEnclave()
      val numOutputRows = new MutableInteger
      val filtered = enclave.Filter(
        eid, ???, block.bytes, numOutputRows)
      Block(filtered, numOutputRows.value)
    }
    Utils.ensureCached(nonDummy)
    time("join - filter dummies") { nonDummy.count }
    nonDummy
  }
}

private object OpaqueJoinUtils {
  /** Given the join information, return (joinOpcode, dummySortOpcode, dummyFilterOpcode). */
  def getOpcodes(
      leftOutput: Seq[Attribute], rightOutput: Seq[Attribute], leftKeys: Seq[Expression],
      rightKeys: Seq[Expression], condition: Option[Expression])
      : (Opcode, Opcode, Opcode) = {

    import Opcode._

    object LeftCol extends ColumnNumberMatcher {
      override def input: Seq[Attribute] = leftOutput
    }
    object RightCol extends ColumnNumberMatcher {
      override def input: Seq[Attribute] = rightOutput
    }

    val info = (leftOutput.map(_.dataType), rightOutput.map(_.dataType),
      leftKeys, rightKeys, condition)
    val (joinOpcode, dummySortOpcode, dummyFilterOpcode) = info match {
      case (Seq(StringType, IntegerType), Seq(StringType, StringType, FloatType),
        Seq(LeftCol(1, _)), Seq(RightCol(1, _)), None) =>
        (OP_JOIN_COL1, OP_SORT_COL3_IS_DUMMY_COL1, OP_FILTER_NOT_DUMMY)

      case (Seq(IntegerType, StringType, IntegerType), Seq(IntegerType, StringType, IntegerType),
        Seq(LeftCol(2, _)), Seq(RightCol(2, _)), None) =>
        (OP_JOIN_COL2, OP_SORT_COL4_IS_DUMMY_COL2, OP_FILTER_NOT_DUMMY)

      case (Seq(IntegerType, FloatType), Seq(IntegerType, IntegerType, FloatType),
        Seq(LeftCol(1, _)), Seq(RightCol(1, _)), None) =>
        (OP_JOIN_PAGERANK, OP_SORT_COL3_IS_DUMMY_COL1, OP_FILTER_NOT_DUMMY)

      case (Seq(IntegerType, StringType),
        Seq(IntegerType, IntegerType, IntegerType, IntegerType, IntegerType, FloatType,
          IntegerType, FloatType, FloatType),
        Seq(LeftCol(1, _)), Seq(RightCol(2, _)), None) =>
        (OP_JOIN_TPCH9GENERIC_NATION, OP_SORT_COL3_IS_DUMMY_COL1, OP_FILTER_NOT_DUMMY)

      case (Seq(IntegerType, IntegerType),
        Seq(IntegerType, IntegerType, IntegerType, IntegerType, FloatType, IntegerType,
          FloatType, FloatType),
        Seq(LeftCol(1, _)), Seq(RightCol(4, _)), None) =>
        (OP_JOIN_TPCH9GENERIC_SUPPLIER, OP_SORT_COL3_IS_DUMMY_COL1, OP_FILTER_NOT_DUMMY)

      case (Seq(IntegerType, IntegerType),
        Seq(IntegerType, IntegerType, FloatType, IntegerType, IntegerType, FloatType, FloatType),
        Seq(LeftCol(1, _)), Seq(RightCol(4, _)), None) =>
        (OP_JOIN_TPCH9GENERIC_ORDERS, OP_SORT_COL3_IS_DUMMY_COL1, OP_FILTER_NOT_DUMMY)

      case (Seq(IntegerType, IntegerType, FloatType),
        Seq(IntegerType, IntegerType, IntegerType, IntegerType, FloatType, FloatType),
        Seq(LeftCol(2, _), LeftCol(1, _)), Seq(RightCol(3, _), RightCol(1, _)), None) =>
        (OP_JOIN_TPCH9GENERIC_PARTSUPP, OP_SORT_COL3_IS_DUMMY_COL1, OP_FILTER_NOT_DUMMY)

      case (Seq(IntegerType),
        Seq(IntegerType, IntegerType, IntegerType, IntegerType, FloatType, FloatType),
        Seq(LeftCol(1, _)), Seq(RightCol(2, _)), None) =>
        (OP_JOIN_TPCH9GENERIC_PART_LINEITEM, OP_SORT_COL3_IS_DUMMY_COL1, OP_FILTER_NOT_DUMMY)

      case (Seq(IntegerType, IntegerType),
        Seq(IntegerType, StringType, IntegerType, IntegerType, FloatType, IntegerType,
          IntegerType, FloatType, FloatType),
        Seq(LeftCol(1, _)), Seq(RightCol(6, _)), None) =>
        (OP_JOIN_TPCH9OPAQUE_ORDERS, OP_SORT_COL3_IS_DUMMY_COL1, OP_FILTER_NOT_DUMMY)

      case (Seq(IntegerType, StringType, IntegerType, IntegerType, FloatType),
        Seq(IntegerType, IntegerType, IntegerType, IntegerType, FloatType, FloatType),
        Seq(LeftCol(3, _), LeftCol(4, _)), Seq(RightCol(3, _), RightCol(2, _)), None) =>
        (OP_JOIN_TPCH9OPAQUE_LINEITEM, OP_SORT_COL3_IS_DUMMY_COL1, OP_FILTER_NOT_DUMMY)

      case (Seq(IntegerType, StringType),
        Seq(IntegerType, IntegerType, IntegerType, FloatType),
        Seq(LeftCol(1, _)), Seq(RightCol(2, _)), None) =>
        (OP_JOIN_TPCH9OPAQUE_NATION, OP_SORT_COL3_IS_DUMMY_COL1, OP_FILTER_NOT_DUMMY)

      case (Seq(IntegerType, IntegerType),
        Seq(IntegerType, IntegerType, FloatType),
        Seq(LeftCol(1, _)), Seq(RightCol(2, _)), None) =>
        (OP_JOIN_TPCH9OPAQUE_SUPPLIER, OP_SORT_COL3_IS_DUMMY_COL1, OP_FILTER_NOT_DUMMY)

      case (Seq(IntegerType),
        Seq(IntegerType, IntegerType, FloatType),
        Seq(LeftCol(1, _)), Seq(RightCol(1, _)), None) =>
        (OP_JOIN_TPCH9OPAQUE_PART_PARTSUPP, OP_SORT_COL3_IS_DUMMY_COL1, OP_FILTER_NOT_DUMMY)

      case (Seq(StringType, IntegerType),
        Seq(StringType, IntegerType, StringType, IntegerType, StringType, StringType),
        Seq(LeftCol(1, _)), Seq(RightCol(1, _)), None) =>
        (OP_JOIN_DISEASEDEFAULT_TREATMENT, OP_SORT_COL2_IS_DUMMY_COL1, OP_FILTER_NOT_DUMMY)

      case (Seq(StringType, IntegerType, StringType),
        Seq(IntegerType, StringType, StringType),
        Seq(LeftCol(1, _)), Seq(RightCol(2, _)), None) =>
        (OP_JOIN_DISEASEDEFAULT_PATIENT, OP_SORT_COL2_IS_DUMMY_COL1, OP_FILTER_NOT_DUMMY)

      case (Seq(StringType, IntegerType, StringType, StringType, IntegerType),
        Seq(IntegerType, StringType, StringType),
        Seq(LeftCol(1, _)), Seq(RightCol(2, _)), None) =>
        (OP_JOIN_DISEASEOPAQUE_PATIENT, OP_SORT_COL2_IS_DUMMY_COL1, OP_FILTER_NOT_DUMMY)

      case (Seq(StringType, IntegerType, StringType),
        Seq(StringType, IntegerType),
        Seq(LeftCol(1, _)), Seq(RightCol(1, _)), None) =>
        (OP_JOIN_DISEASEOPAQUE_TREATMENT, OP_SORT_COL2_IS_DUMMY_COL1, OP_FILTER_NOT_DUMMY)

      case (Seq(IntegerType, StringType),
        Seq(StringType, IntegerType, StringType, IntegerType, StringType, StringType),
        Seq(LeftCol(1, _)), Seq(RightCol(2, _)), None) =>
        (OP_JOIN_GENEDEFAULT_GENE, OP_SORT_COL2_IS_DUMMY_COL1, OP_FILTER_NOT_DUMMY)

      case (Seq(IntegerType, StringType),
        Seq(StringType, IntegerType, StringType),
        Seq(LeftCol(1, _)), Seq(RightCol(2, _)), None) =>
        (OP_JOIN_GENEOPAQUE_GENE, OP_SORT_COL2_IS_DUMMY_COL1, OP_FILTER_NOT_DUMMY)

      case (Seq(IntegerType, StringType, StringType, IntegerType, StringType),
        Seq(IntegerType, StringType, StringType),
        Seq(LeftCol(3, _)), Seq(RightCol(2, _)), None) =>
        (OP_JOIN_GENEOPAQUE_PATIENT, OP_SORT_COL2_IS_DUMMY_COL1, OP_FILTER_NOT_DUMMY)

      case _ =>
        throw new Exception(
          s"OpaqueJoinUtils: unknown left join keys $leftKeys, " +
            s"right join keys $rightKeys, condition $condition.\n" +
            s"Input: left $leftOutput, right $rightOutput.\n" +
            s"Types: left ${leftOutput.map(_.dataType)}, right ${rightOutput.map(_.dataType)}")
    }
    (joinOpcode, dummySortOpcode, dummyFilterOpcode)
  }
}

case class EncryptedSortMergeJoinExec(
    joinType: JoinType,
    leftKeys: Seq[Expression],
    rightKeys: Seq[Expression],
    condition: Option[Expression],
    child: SparkPlan)
  extends BinaryExecNode with OpaqueOperatorExec {

  import Utils.time

  override def output: Seq[Attribute] =
    left.output ++ right.output

  override def executeBlocked() = {
    val leftRDD = left.asInstanceOf[OpaqueOperatorExec].executeBlocked()
    val rightRDD = right.asInstanceOf[OpaqueOperatorExec].executeBlocked()
    Utils.ensureCached(leftRDD)
    time("Force left child of EncryptedSortMergeJoinExec") { leftRDD.count }
    Utils.ensureCached(rightRDD)
    time("Force right child of EncryptedSortMergeJoinExec") { rightRDD.count }

    // RA.initRA(leftRDD)

    val joined = child.map { block =>
      val (enclave, eid) = Utils.initEnclave()
      val numOutputRows = new MutableInteger
      val joined = enclave.EncryptedSortMergeJoin(
        eid, joinOpcode.value, block.bytes, block.numRows, numOutputRows)
      Block(joined, numOutputRows.value)
    }
    Utils.ensureCached(joined)
    time("join - sort merge join") { joined.count }

    joined
  }
}

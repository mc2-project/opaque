name := "opaque"

version := "0.1"

organization := "edu.berkeley.cs.amplab"

scalaVersion := "2.12.10"

libraryDependencies += "org.scalanlp" %% "breeze" % "1.1"

libraryDependencies += "org.scalatest" %% "scalatest" % "3.0.5" % "test"

libraryDependencies ++= Seq(
  "io.grpc" % "grpc-netty" % scalapb.compiler.Version.grpcJavaVersion,
  "com.thesamet.scalapb" %% "scalapb-runtime-grpc" % scalapb.compiler.Version.scalapbVersion,
  "com.google.guava" % "guava" % "21.0"
)

enablePlugins(SparkPlugin)
sparkVersion := "3.1.1"
sparkComponents ++= Seq("core", "sql", "catalyst", "repl")

concurrentRestrictions in Global := Seq(Tags.limit(Tags.Test, 1))

fork in Test := true

/* Create fat jar with src and test classes using build/sbt test:assembly */
Project.inConfig(Test)(baseAssemblySettings)

test in assembly := {}
test in (Test, assembly) := {}

// From https://stackoverflow.com/questions/14791955/assembly-merge-strategy-issues-using-sbt-assembly
// Note that this creates a larger fat jar than the old strategy of deleting everything in META-INF,
// but the GRPC client code requires a file defined in that folder
lazy val commonMergeStrategy: String => sbtassembly.MergeStrategy =
  x =>
    x match {
      case x if Assembly.isConfigFile(x) =>
        MergeStrategy.concat
      case PathList(ps @ _*) if Assembly.isReadme(ps.last) || Assembly.isLicenseFile(ps.last) =>
        MergeStrategy.rename
      case PathList("META-INF", xs @ _*) =>
        (xs map { _.toLowerCase }) match {
          case ("manifest.mf" :: Nil) | ("index.list" :: Nil) | ("dependencies" :: Nil) =>
            MergeStrategy.discard
          case ps @ (x :: xs) if ps.last.endsWith(".sf") || ps.last.endsWith(".dsa") =>
            MergeStrategy.discard
          case "plexus" :: xs =>
            MergeStrategy.discard
          case "services" :: xs =>
            MergeStrategy.filterDistinctLines
          case ("spring.schemas" :: Nil) | ("spring.handlers" :: Nil) =>
            MergeStrategy.filterDistinctLines
          case _ => MergeStrategy.first
        }
      case _ => MergeStrategy.first
    }
assembly / assemblyMergeStrategy := commonMergeStrategy
assemblyMergeStrategy in (Test, assembly) := commonMergeStrategy

/* Include the newer version of com.google.guava found in libraryDependencies
 * in the fat jar rather than 14.0 supplied by Spark (gRPC does not work otherwise)
 */
lazy val commonShadeRules = Seq(ShadeRule.rename("com.google.**" -> "updatedGuava.@1").inAll)
assemblyShadeRules in assembly := commonShadeRules
assemblyShadeRules in (Test, assembly) := commonShadeRules

/*
 * local-cluster[*,*,*] in our tests requires a packaged .jar file to run correctly.
 * See https://stackoverflow.com/questions/28186607/java-lang-classcastexception-using-lambda-expressions-in-spark-job-on-remote-ser
 * The following code creates dependencies for test and testOnly to ensure that the required .jar is always created by
 * build/sbt package before running any tests. Note that .class files are still only compiled ONCE for both package and tests,
 * so the performance impact is very minimal.
 */
(test in Test) := {
  ((test in Test))
    .dependsOn(Keys.`package` in Compile)
    .dependsOn(Keys.`package` in Test)
    .dependsOn(synthTestDataTask)
    .value
}
(Keys.`testOnly` in Test) := {
  ((Keys.`testOnly` in Test))
    .dependsOn(Keys.`package` in Compile)
    .dependsOn(Keys.`package` in Test)
    .dependsOn(synthTestDataTask)
    .evaluated
}

testOptions in Test += Tests.Argument("-oF")

lazy val commonJavaOptions =
  Seq("-Xmx4096m", "-XX:ReservedCodeCacheSize=384m")
Test / javaOptions := commonJavaOptions
run / javaOptions := commonJavaOptions ++ Seq("-Dspark.master=local[1]")

scalacOptions ++= Seq(
  "-deprecation",
  "-encoding",
  "UTF-8",
  "-feature",
  "-unchecked",
  "-Xfuture",
  "-Xlint:_",
  "-Ywarn-adapted-args",
  "-Ywarn-dead-code",
  "-Ywarn-inaccessible",
  "-Ywarn-infer-any",
  "-Ywarn-nullary-override",
  "-Ywarn-nullary-unit",
  "-Ywarn-unused-import"
)

scalacOptions in (Compile, console) := Seq.empty

val flatbuffersGenCppDir =
  SettingKey[File]("flatbuffersGenCppDir", "Location of Flatbuffers generated C++ files.")

flatbuffersGenCppDir := sourceManaged.value / "flatbuffers" / "gen-cpp"

val buildType = SettingKey[BuildType]("buildType", "Release, Debug, or Profile.")

buildType := Release

scalacOptions ++= { if (buildType.value == Debug) Seq("-g:vars") else Nil }
javaOptions ++= {
  if (buildType.value == Debug)
    Seq("-Xdebug", "-agentlib:jdwp=transport=dt_socket,server=y,suspend=n,address=8000")
  else Nil
}

val fetchFlatbuffersLibTask = TaskKey[File](
  "fetchFlatbuffersLib",
  "Fetches and builds the Flatbuffers library, returning its location."
)

unmanagedSources in Compile ++= ((fetchFlatbuffersLibTask.value / "java") ** "*.java").get

val buildFlatbuffersTask = TaskKey[Seq[File]](
  "buildFlatbuffers",
  "Generates Java and C++ sources from Flatbuffers interface files, returning the Java sources."
)

sourceGenerators in Compile += buildFlatbuffersTask.taskValue

val enclaveBuildTask = TaskKey[File](
  "enclaveBuild",
  "Builds the C++ enclave code, returning the directory containing the resulting shared libraries."
)

compile in Compile := { (compile in Compile).dependsOn(enclaveBuildTask).value }

val copyEnclaveLibrariesToResourcesTask = TaskKey[Seq[File]](
  "copyEnclaveLibrariesToResources",
  "Copies the enclave libraries to the managed resources directory, returning the copied files."
)

resourceGenerators in Compile += copyEnclaveLibrariesToResourcesTask.taskValue

// Add the managed resource directory to the resource classpath so we can find libraries at runtime
managedResourceDirectories in Compile += resourceManaged.value

unmanagedResources in Compile ++= ((sourceDirectory.value / "python") ** "*.py").get

Compile / PB.protoSources := Seq(sourceDirectory.value / "protobuf")
Compile / PB.targets := Seq(scalapb.gen() -> (Compile / sourceManaged).value / "scalapb")

// Watch the enclave C++ files
watchSources ++=
  ((sourceDirectory.value / "cpp") ** (("*.cpp" || "*.c" || "*.h" || "*.tcc" || "*.edl" || "CMakeLists.txt") -- ".*")).get

// Watch the Flatbuffer schemas
watchSources ++=
  ((sourceDirectory.value / "flatbuffers") ** "*.fbs").get

val synthTestDataTask = TaskKey[Unit]("synthTestData", "Synthesizes test data.")

val oeGdbTask =
  TaskKey[Unit](
    "oe-gdb-task",
    "Runs the test suite specified in ./project/resources under the oe-gdb debugger."
  )
def oeGdbCommand = Command.command("oe-gdb") { state =>
  Project.extract(state).runTask(oeGdbTask, state)
  state
}

val synthBenchmarkDataTask = TaskKey[Unit]("synthBenchmarkData", "Synthesizes benchmark data.")
def data = Command.command("data") { state =>
  Project.runTask(synthBenchmarkDataTask, state)
  state
}

commands += oeGdbCommand
commands += data

initialCommands in console :=
  """
    |import org.apache.spark.SparkContext
    |import org.apache.spark.sql.SQLContext
    |import org.apache.spark.sql.catalyst.analysis._
    |import org.apache.spark.sql.catalyst.dsl._
    |import org.apache.spark.sql.catalyst.errors._
    |import org.apache.spark.sql.catalyst.expressions._
    |import org.apache.spark.sql.catalyst.plans.logical._
    |import org.apache.spark.sql.catalyst.rules._
    |import org.apache.spark.sql.catalyst.util._
    |import org.apache.spark.sql.execution
    |import org.apache.spark.sql.functions._
    |import org.apache.spark.sql.types._
    |import org.apache.log4j.Level
    |import org.apache.log4j.LogManager
    |
    |LogManager.getLogger("org.apache.spark").setLevel(Level.WARN)
    |LogManager.getLogger("org.apache.spark.executor.Executor").setLevel(Level.WARN)
    |
    |val spark = (org.apache.spark.sql.SparkSession.builder()
    |  .master("local")
    |  .appName("Opaque shell")
    |  .getOrCreate())
    |val sc = spark.sparkContext
    |val sqlContext = spark.sqlContext
    |
    |import spark.implicits._
    |
    |import edu.berkeley.cs.rise.opaque.implicits._
    |edu.berkeley.cs.rise.opaque.Utils.initOpaqueSQL(spark, testing = true)
  """.stripMargin

cleanupCommands in console := "spark.stop()"

import scala.sys.process.Process

nativePlatform := {
  try {
    val lines = Process("uname -sm").lines
    if (lines.length == 0) {
      sys.error("Error occured trying to run `uname`")
    }
    // uname -sm returns "<kernel> <hardware name>"
    val parts = lines.head.split(" ")
    if (parts.length != 2) {
      sys.error("'uname -sm' returned unexpected string: " + lines.head)
    } else {
      val arch = parts(1).toLowerCase.replaceAll("\\s", "")
      val kernel = parts(0).toLowerCase.replaceAll("\\s", "")
      arch + "-" + kernel
    }
  } catch {
    case ex: Exception =>
      sLog.value.error("Error trying to determine platform.")
      sLog.value.warn("Cannot determine platform! It will be set to 'unknown'.")
      "unknown-unknown"
  }
}

oeGdbTask := {
  (compile in Test).value
  Process(
    Seq(
      "oegdb",
      "java",
      "-x",
      ((baseDirectory in ThisBuild).value / "project" / "resources" / "run-tests.gdb").getPath
    ),
    None,
    "CLASSPATH" -> (fullClasspath in Test).value.map(_.data.getPath).mkString(":")
  ).!<
}

fetchFlatbuffersLibTask := {
  val flatbuffersVersion = "1.7.0"
  val flatbuffersSource = target.value / "flatbuffers" / s"flatbuffers-$flatbuffersVersion"
  if (!flatbuffersSource.exists) {
    // Fetch flatbuffers from Github
    streams.value.log.info(s"Fetching Flatbuffers")
    val flatbuffersUrl =
      new java.net.URL(s"https://github.com/google/flatbuffers/archive/v$flatbuffersVersion.zip")
    IO.unzipURL(flatbuffersUrl, flatbuffersSource.getParentFile)
  }
  val flatc = flatbuffersSource / "flatc"
  if (!flatc.exists) {
    // Build flatbuffers with cmake
    streams.value.log.info(s"Building Flatbuffers")
    val nproc = java.lang.Runtime.getRuntime.availableProcessors
    if (
      Process(
        Seq(
          "cmake",
          "-G",
          "Unix Makefiles",
          "-DFLATBUFFERS_BUILD_TESTS=OFF",
          "-DFLATBUFFERS_BUILD_FLATLIB=OFF",
          "-DFLATBUFFERS_BUILD_FLATHASH=OFF",
          "-DFLATBUFFERS_BUILD_FLATC=ON"
        ),
        flatbuffersSource
      ).! != 0
      || Process(Seq("make", "-j" + nproc), flatbuffersSource).! != 0
    ) {
      sys.error("Flatbuffers library build failed.")
    }
  }
  flatbuffersSource
}

// Flatbuffers header file generation

buildFlatbuffersTask := {
  import sys.process._

  val flatc = fetchFlatbuffersLibTask.value / "flatc"

  val javaOutDir = sourceManaged.value / "flatbuffers" / "gen-java"

  val flatbuffers = ((sourceDirectory.value / "flatbuffers") ** "*.fbs").get
  // Only regenerate Flatbuffers headers if any .fbs file changed, indicated by their latest
  // modification time being newer than all generated headers. We do this because regenerating
  // Flatbuffers headers causes a full enclave rebuild, which is slow.
  val fbsLastMod = flatbuffers.map(_.lastModified).max
  val gen = (flatbuffersGenCppDir.value ** "*.h" +++ javaOutDir ** "*.java").get

  if (gen.isEmpty || fbsLastMod > gen.map(_.lastModified).max) {
    for (fbs <- flatbuffers) {
      streams.value.log.info(s"Generating flatbuffers for ${fbs}")
      if (
        Seq(flatc.getPath, "--cpp", "-o", flatbuffersGenCppDir.value.getPath, fbs.getPath).! != 0
        || Seq(flatc.getPath, "--java", "-o", javaOutDir.getPath, fbs.getPath).! != 0
      ) {
        sys.error("Flatbuffers build failed.")
      }
    }
  }

  (javaOutDir ** "*.java").get
}

enclaveBuildTask := {
  buildFlatbuffersTask.value // Enclave build depends on the generated C++ headers
  val enclaveSourceDir = baseDirectory.value / "src" / "cpp"
  val enclaveBuildDir = target.value / "cpp"
  enclaveBuildDir.mkdirs()
  val cmakeResult =
    Process(
      Seq(
        "cmake",
        s"-DCMAKE_INSTALL_PREFIX:PATH=${enclaveBuildDir.getPath}",
        s"-DCMAKE_BUILD_TYPE=${buildType.value}",
        s"-DFLATBUFFERS_LIB_DIR=${(fetchFlatbuffersLibTask.value / "include").getPath}",
        s"-DFLATBUFFERS_GEN_CPP_DIR=${flatbuffersGenCppDir.value.getPath}",
        enclaveSourceDir.getPath
      ),
      enclaveBuildDir
    ).!
  if (cmakeResult != 0) sys.error("C++ build failed.")
  val nproc = java.lang.Runtime.getRuntime.availableProcessors
  val buildResult = Process(Seq("make", "-j" + nproc), enclaveBuildDir).!
  if (buildResult != 0) sys.error("C++ build failed.")
  val installResult = Process(Seq("make", "install"), enclaveBuildDir).!
  if (installResult != 0) sys.error("C++ build failed.")
  enclaveBuildDir / "lib"
}

copyEnclaveLibrariesToResourcesTask := {
  val libraries = (enclaveBuildTask.value ** "*.so").get
  val mappings: Seq[(File, String)] =
    libraries pair Path.rebase(enclaveBuildTask.value, s"/native/${nativePlatform.value}")
  val resources: Seq[File] = for ((file, path) <- mappings) yield {
    val resource = resourceManaged.value / path
    IO.copyFile(file, resource)
    resource
  }
  resources
}

synthTestDataTask := {
  val diseaseDataFiles =
    for {
      diseaseDir <- (baseDirectory.value / "data" / "disease").get
      name <- Seq("disease.csv", "gene.csv", "treatment.csv", "patient-125.csv")
    } yield new File(diseaseDir, name)

  val tpchDir = baseDirectory.value / "data" / "tpch" / "sf_001"
  tpchDir.mkdirs()
  val tpchDataFiles =
    for {
      name <- Seq(
        "customer.tbl",
        "lineitem.tbl",
        "nation.tbl",
        "orders.tbl",
        "partsupp.tbl",
        "part.tbl",
        "region.tbl",
        "supplier.tbl"
      )
    } yield new File(tpchDir, name)

  if (!diseaseDataFiles.forall(_.exists)) {
    import sys.process._
    val ret = Seq("data/disease/synth-disease-data").!
    if (ret != 0) sys.error("Failed to synthesize disease test data.")
  }

  if (!tpchDataFiles.forall(_.exists)) {
    import sys.process._
    val ret = Seq("data/tpch/synth-tpch-test-data").!
    if (ret != 0) sys.error("Failed to synthesize TPC-H test data.")
  }
}

synthBenchmarkDataTask := {
  val tpchDir = baseDirectory.value / "data" / "tpch" / "sf_1"
  tpchDir.mkdirs()
  val tpchDataFiles =
    for {
      name <- Seq(
        "customer.tbl",
        "lineitem.tbl",
        "nation.tbl",
        "orders.tbl",
        "partsupp.tbl",
        "part.tbl",
        "region.tbl",
        "supplier.tbl"
      )
    } yield new File(tpchDir, name)

  if (!tpchDataFiles.forall(_.exists)) {
    import sys.process._
    val ret = Seq("data/tpch/synth-tpch-benchmark-data").!
    if (ret != 0) sys.error("Failed to synthesize TPC-H benchmark data.")
  }
}
